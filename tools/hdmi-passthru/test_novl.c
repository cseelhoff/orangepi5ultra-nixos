/* Tests for novl.c. No board needed: the capture buffers are memfds, the overlay buffers are
 * malloc'd, and a thread plays the display loop by the same rules hdmi-passthru.c follows.
 *
 *   cc -O1 -g -Wall -fsanitize=address,undefined -pthread novl.c test_novl.c -o test_novl && ./test_novl
 */
#define _GNU_SOURCE
#include "novl.h"

#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define SW 64			/* the HDMI-in picture */
#define SH 48
#define SSTRIDE (SW + 16)
#define NBUF 4
#define OW 128			/* the output */
#define OH 96
#define OPITCH (OW * 4 + 64)

/* ---------- the fake display loop ---------- */

static struct {
	struct novl *n;
	pthread_t th;
	atomic_int run, paused, flip_ms, frame_ms;
	atomic_int shown, last_shown, bad;
	atomic_int multi;	/* a test sends several rectangles: the overlay is not one filled box, and the test compares the whole buffer itself */
	atomic_int holds, last_hold, frames;
	atomic_int state[NBUF];	/* 0 queued, 1 on screen, 2 held by a snapshot */
	atomic_long max_hook_us;
	int on_screen;
	uint8_t *ov[2];
} L;

/* the fake hardware encoder's knobs and what it saw */
static struct { atomic_int calls, fail, delay_ms, encoding, last_index, state_at_encode; } E;

static uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void requeue(void *ctx, int i)
{
	(void)ctx;
	if (atomic_exchange(&L.state[i], 0) != 2 || E.encoding)	/* it must be held, and not still being encoded */
		L.bad++;
}

/* The box of non-zero pixels, and whether they are all one colour filling it. */
static int overlay_box(const uint8_t *buf, int *bx, int *by, int *bw, int *bh, uint32_t *color)
{
	int x0 = OW, y0 = OH, x1 = -1, y1 = -1, count = 0;
	for (int y = 0; y < OH; y++)
		for (int x = 0; x < OW; x++) {
			uint32_t p;
			memcpy(&p, buf + (size_t)y * OPITCH + x * 4, 4);
			if (!p)
				continue;
			if (x < x0) x0 = x;
			if (x > x1) x1 = x;
			if (y < y0) y0 = y;
			if (y > y1) y1 = y;
			count++;
		}
	*bx = x0; *by = y0; *bw = x1 - x0 + 1; *bh = y1 - y0 + 1;
	if (count == 0) { *bw = *bh = 0; return 1; }
	memcpy(color, buf + (size_t)y0 * OPITCH + x0 * 4, 4);
	if (count != *bw * *bh)
		return 0;
	for (int y = y0; y <= y1; y++)
		for (int x = x0; x <= x1; x++)
			if (memcmp(buf + (size_t)y * OPITCH + x * 4, color, 4))
				return 0;
	return 1;
}

static void *display_loop(void *arg)
{
	(void)arg;
	while (L.run) {
		usleep(L.frame_ms * 1000);
		if (L.paused)
			continue;
		novl_frame(L.n);
		L.frames++;
		int i = -1;
		for (int b = 0; b < NBUF && i < 0; b++)
			if (L.state[b] == 0)
				i = b;
		if (i < 0)
			continue;
		L.state[i] = 1;
		uint64_t t0 = now_us();
		int o = novl_overlay_next(L.n);
		uint64_t dt = now_us() - t0;
		if ((long)dt > L.max_hook_us)
			L.max_hook_us = (long)dt;
		static uint8_t scanned_before[OPITCH * OH];
		int scanned = -1;
		if (o >= 0) {
			int x, y, w, h;
			uint32_t c;
			if (!L.multi && !overlay_box(L.ov[o], &x, &y, &w, &h, &c))
				L.bad++;	/* a buffer taken must be one whole overlay, never a half-drawn one */
			scanned = 1 - o;	/* until this flip latches, the display is still reading the other buffer */
			memcpy(scanned_before, L.ov[scanned], sizeof scanned_before);
			L.last_shown = o;
			L.shown++;
		}
		if (L.flip_ms)
			usleep(L.flip_ms * 1000);
		if (scanned >= 0 && memcmp(scanned_before, L.ov[scanned], sizeof scanned_before))
			L.bad++;	/* the server wrote the buffer that was being scanned out */
		novl_flip_done(L.n);
		int old = L.on_screen;
		L.on_screen = i;
		if (old >= 0) {
			L.state[old] = 2;	/* before the call: the server may requeue it the moment it returns */
			t0 = now_us();
			int held = novl_hold(L.n, old);
			dt = now_us() - t0;
			if ((long)dt > L.max_hook_us)
				L.max_hook_us = (long)dt;
			if (held) {
				L.last_hold = old;
				L.holds++;
			} else {
				L.state[old] = 0;
			}
		}
	}
	return NULL;
}

/* ---------- a fake hardware encoder ---------- */

/* The "JPEG" of a buffer: markers, the buffer index and a pattern that depends on it, so a test can tell which
 * buffer was encoded. It also checks what the real encoder relies on: the buffer is held while it works. */
#define FAKE_JPEG_LEN 3000

static int fake_encode(void *ctx, int index, void **jpeg, size_t *len)
{
	(void)ctx;
	E.calls++;
	E.last_index = index;
	E.state_at_encode = L.state[index];
	E.encoding = 1;
	if (E.delay_ms)
		usleep((unsigned)E.delay_ms * 1000);
	E.encoding = 0;
	if (E.fail)
		return -1;
	uint8_t *p = malloc(FAKE_JPEG_LEN);
	for (int i = 0; i < FAKE_JPEG_LEN; i++)
		p[i] = (uint8_t)(i * 7 + index);
	p[0] = 0xFF; p[1] = 0xD8; p[2] = (uint8_t)index; p[FAKE_JPEG_LEN - 2] = 0xFF; p[FAKE_JPEG_LEN - 1] = 0xD9;
	*jpeg = p;
	*len = FAKE_JPEG_LEN;
	return 0;
}

static int jpeg_is_buffer(const uint8_t *p, size_t n, int index)
{
	if (n != FAKE_JPEG_LEN || p[0] != 0xFF || p[1] != 0xD8 || p[2] != (uint8_t)index || p[n - 2] != 0xFF || p[n - 1] != 0xD9)
		return 0;
	for (size_t i = 3; i < n - 2; i++)
		if (p[i] != (uint8_t)(i * 7 + index))
			return 0;
	return 1;
}

/* ---------- harness ---------- */

static char sock_path[108];
static const char *sock_arg;	/* --serve: the path to listen on instead of a private one */
static struct novl_source src;

static void setup(void)
{
	L.multi = 0;
	memset(&src, 0, sizeof src);
	src.width = SW; src.height = SH; src.encode = fake_encode; src.requeue = requeue;
	E.calls = E.fail = E.delay_ms = E.encoding = E.last_index = E.state_at_encode = 0;
	struct novl_overlay ov = { .width = OW, .height = OH, .pitch = OPITCH };
	for (int i = 0; i < 2; i++)
		L.ov[i] = ov.map[i] = calloc(OPITCH, OH);
	if (sock_arg)
		snprintf(sock_path, sizeof sock_path, "%s", sock_arg);
	else
		snprintf(sock_path, sizeof sock_path, "/tmp/novl-test-%d.sock", (int)getpid());
	L.n = novl_start(sock_path, NULL, &src, &ov);
	if (!L.n)
		abort();
	for (int b = 0; b < NBUF; b++)
		L.state[b] = 0;
	L.on_screen = -1;
	L.shown = L.holds = L.bad = 0;
	L.max_hook_us = 0;
	L.run = 1; L.paused = 0; L.frame_ms = 4; L.flip_ms = 0;
	pthread_create(&L.th, NULL, display_loop, NULL);
}

static void teardown(void)
{
	L.run = 0;
	pthread_join(L.th, NULL);
	novl_stop(L.n);
	for (int i = 0; i < 2; i++)
		free(L.ov[i]);
}

static void put32(uint8_t *p, int32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static int32_t get32(const uint8_t *p) { return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24); }

static int cli_connect(void)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	strcpy(a.sun_path, sock_path);
	if (connect(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); return -1; }
	return fd;
}

static int send_raw(int fd, const void *p, size_t n)
{
	const uint8_t *b = p;
	while (n) {
		ssize_t w = send(fd, b, n, MSG_NOSIGNAL);
		if (w <= 0)
			return -1;
		b += w; n -= (size_t)w;
	}
	return 0;
}

static int recv_n(int fd, void *p, size_t n)
{
	uint8_t *b = p;
	while (n) {
		struct pollfd pf = { fd, POLLIN, 0 };
		if (poll(&pf, 1, 3000) <= 0)
			return -1;
		ssize_t r = recv(fd, b, n, 0);
		if (r <= 0)
			return -1;
		b += r; n -= (size_t)r;
	}
	return 0;
}

static int send_msg(int fd, uint32_t type, const uint8_t *payload, uint32_t len)
{
	uint8_t h[12];
	put32(h, 0x4C564F4E); put32(h + 4, (int32_t)type); put32(h + 8, (int32_t)len);
	return send_raw(fd, h, 12) || (len && send_raw(fd, payload, len)) ? -1 : 0;
}

static int info(int fd, int out[5])
{
	uint8_t h[12], b[20];
	if (send_msg(fd, 4, NULL, 0) || recv_n(fd, h, 12) || get32(h + 4) != 5 || get32(h + 8) != 20 || recv_n(fd, b, 20))
		return -1;
	for (int i = 0; i < 5; i++)
		out[i] = get32(b + i * 4);
	return 0;
}

static int send_overlay(int fd, int x, int y, int w, int h, uint32_t color)
{
	size_t n = (size_t)(w > 0 ? w : 0) * (h > 0 ? h : 0) * 4;
	uint8_t *p = malloc(16 + n);
	put32(p, x); put32(p + 4, y); put32(p + 8, w); put32(p + 12, h);
	for (size_t i = 0; i < n; i += 4)
		memcpy(p + 16 + i, &color, 4);
	int rc = send_msg(fd, 1, p, (uint32_t)(16 + n));
	free(p);
	return rc;
}

static int wait_shown(int count)
{
	for (int i = 0; i < 500 && L.shown < count; i++)
		usleep(2000);
	return L.shown >= count;
}

/* Sends an overlay and waits for the loop to take it. */
static int show(int fd, int x, int y, int w, int h, uint32_t color)
{
	int before = L.shown;
	return send_overlay(fd, x, y, w, h, color) == 0 && wait_shown(before + 1);
}

static int shown_is(int x, int y, int w, int h, uint32_t color)
{
	int bx, by, bw, bh;
	uint32_t c = 0;
	if (!overlay_box(L.ov[L.last_shown], &bx, &by, &bw, &bh, &c))
		return 0;
	if (w == 0)
		return bw == 0;
	return bx == x && by == y && bw == w && bh == h && c == color;
}

/* returns the reply status, or -1 if there was no proper reply; *jpeg is malloc'd when the reply has a payload */
static int snapshot(int fd, int x, int y, int w, int h, int rect[4], int src_wh[2], uint8_t **jpeg, size_t *jlen)
{
	uint8_t req[16], hd[12], meta[28];
	put32(req, x); put32(req + 4, y); put32(req + 8, w); put32(req + 12, h);
	if (send_msg(fd, 2, req, 16) || recv_n(fd, hd, 12) || get32(hd + 4) != 3 || get32(hd + 8) < 28 || recv_n(fd, meta, 28))
		return -1;
	for (int i = 0; i < 4; i++)
		rect[i] = get32(meta + i * 4);
	src_wh[0] = get32(meta + 16); src_wh[1] = get32(meta + 20);
	*jlen = (size_t)get32(hd + 8) - 28;
	*jpeg = NULL;
	if (*jlen) {
		*jpeg = malloc(*jlen);
		if (recv_n(fd, *jpeg, *jlen))
			return -1;
	}
	return get32(meta + 24);
}

/* ---------- tests ---------- */

static void test_info_and_signal(void)
{
	setup();
	L.paused = 1;	/* no frame yet */
	int fd = cli_connect(), v[5];
	CHECK(fd >= 0 && info(fd, v) == 0);
	CHECK(v[0] == SW && v[1] == SH && v[2] == OW && v[3] == OH);
	CHECK(v[4] == 0);
	int rect[4], wh[2];
	uint8_t *jpeg;
	size_t jlen;
	CHECK(snapshot(fd, 0, 0, 0, 0, rect, wh, &jpeg, &jlen) == 1);	/* no signal */
	CHECK(jlen == 0 && E.calls == 0);	/* and the encoder was not troubled */
	free(jpeg);

	L.paused = 0;
	usleep(60000);
	CHECK(info(fd, v) == 0 && v[4] == 1);
	L.paused = 1;
	usleep(700000);
	CHECK(info(fd, v) == 0 && v[4] == 0);	/* frames stopped: no signal again */
	CHECK(snapshot(fd, 0, 0, 0, 0, rect, wh, &jpeg, &jlen) == 1);
	free(jpeg);
	close(fd);
	teardown();
}

static void test_overlay(void)
{
	setup();
	int fd = cli_connect();
	CHECK(fd >= 0);
	CHECK(show(fd, 10, 20, 30, 15, 0xFF112233u));
	CHECK(shown_is(10, 20, 30, 15, 0xFF112233u));
	CHECK(show(fd, 60, 50, 40, 30, 0xFF445566u));
	CHECK(shown_is(60, 50, 40, 30, 0xFF445566u));
	/* the third update lands in the first buffer again: nothing of the first may remain */
	CHECK(show(fd, 1, 2, 3, 4, 0xFF778899u));
	CHECK(shown_is(1, 2, 3, 4, 0xFF778899u));
	CHECK(show(fd, 0, 0, 0, 0, 0));	/* an empty overlay clears the screen */
	CHECK(shown_is(0, 0, 0, 0, 0));
	CHECK(show(fd, 0, 0, OW, OH, 0xFFAABBCCu));	/* the whole output */
	CHECK(shown_is(0, 0, OW, OH, 0xFFAABBCCu));
	CHECK(show(fd, 0, 0, 0, 0, 0));
	CHECK(shown_is(0, 0, 0, 0, 0));

	/* partly outside the output: what is inside is drawn, the rest dropped, the stream stays in step */
	CHECK(show(fd, 120, 90, 20, 20, 0xFF010203u));
	CHECK(shown_is(120, 90, 8, 6, 0xFF010203u));
	CHECK(show(fd, -5, -3, 12, 10, 0xFF040506u));
	CHECK(shown_is(0, 0, 7, 7, 0xFF040506u));
	CHECK(show(fd, 500, 500, 4, 4, 0xFF070809u));	/* entirely outside */
	CHECK(shown_is(0, 0, 0, 0, 0));
	CHECK(show(fd, 5, 6, 7, 8, 0xFF0A0B0Cu));	/* and the connection still works */
	CHECK(shown_is(5, 6, 7, 8, 0xFF0A0B0Cu));
	CHECK(L.bad == 0);
	close(fd);
	teardown();
}

static void test_no_write_while_scanned_out(void)
{
	setup();
	int fd = cli_connect();
	CHECK(show(fd, 0, 0, 10, 10, 0xFF0000FFu));
	L.flip_ms = 100;	/* the next commit is slow to latch (a real one takes 16 ms; the server waits up to 200) */
	CHECK(show(fd, 0, 0, 20, 20, 0xFF00FF00u));	/* the loop is now showing B, and A is still being scanned out */
	uint64_t taken = now_us();
	/* the third update has nowhere to go until that flip latches: it must wait, and the loop itself
	 * checks on every commit that the buffer being scanned out was not touched meanwhile */
	CHECK(send_overlay(fd, 5, 5, 6, 6, 0xFFFF0000u) == 0);
	L.flip_ms = 0;
	CHECK(wait_shown(3));
	CHECK(now_us() - taken >= 80000);
	CHECK(shown_is(5, 5, 6, 6, 0xFFFF0000u));
	CHECK(L.bad == 0);
	close(fd);
	teardown();
}

static void test_stalled_flip_drops_one_update(void)
{
	setup();
	int fd = cli_connect();
	CHECK(show(fd, 0, 0, 10, 10, 0xFF0000FFu));
	L.flip_ms = 450;	/* a display that stops latching: the server waits 200 ms, then gives up on that update */
	CHECK(show(fd, 0, 0, 20, 20, 0xFF00FF00u));
	CHECK(send_overlay(fd, 5, 5, 6, 6, 0xFFFF0000u) == 0);	/* dropped */
	usleep(600000);
	L.flip_ms = 0;
	CHECK(show(fd, 7, 8, 9, 10, 0xFF0F0F0Fu));	/* the connection is still in step and works */
	CHECK(shown_is(7, 8, 9, 10, 0xFF0F0F0Fu));
	CHECK(L.bad == 0);
	close(fd);
	teardown();
}

static void test_flood(void)
{
	setup();
	int fd = cli_connect();
	srand(7);
	for (int i = 0; i < 400; i++) {
		int w = 1 + rand() % OW, h = 1 + rand() % OH;
		CHECK(send_overlay(fd, rand() % (OW - w + 1), rand() % (OH - h + 1), w, h, 0xFF000000u | (uint32_t)(i * 7919 + 1)) == 0);
		if (i % 7 == 0)
			usleep(3000);
	}
	CHECK(send_overlay(fd, 3, 4, 5, 6, 0xFF123456u) == 0);
	/* The last update comes behind a queue of earlier ones. Look at the shown buffer only once the loop has
	 * taken nothing for 50 ms: while updates still arrive the server may rewrite it under the reader. */
	for (int i = 0, last = -1, quiet = 0; i < 500 && quiet < 25; i++) {
		int s = L.shown;
		quiet = s == last ? quiet + 1 : 0;
		last = s;
		usleep(2000);
	}
	CHECK(shown_is(3, 4, 5, 6, 0xFF123456u));
	CHECK(L.shown > 5);
	CHECK(L.bad == 0);	/* every buffer the loop ever took was one whole overlay */
	close(fd);
	teardown();
}

static void test_big_overlay_does_not_stall_the_loop(void)
{
	setup();
	int fd = cli_connect();
	for (int i = 0; i < 40; i++)
		CHECK(send_overlay(fd, 0, 0, OW, OH, 0xFF000000u | (uint32_t)(i + 1)) == 0);
	usleep(200000);
	CHECK(show(fd, 1, 1, 2, 2, 0xFFFEDCBAu));
	CHECK(L.max_hook_us < 20000);	/* the loop's hooks never waited on the socket or the blit */
	printf("novl: worst display-loop hook time while 40 full overlays were arriving: %ld us\n", (long)L.max_hook_us);
	close(fd);
	teardown();
}

static void test_snapshot(void)
{
	setup();
	int fd = cli_connect(), rect[4], wh[2];
	uint8_t *jpeg;
	size_t jlen;
	usleep(50000);

	/* the reply is the JPEG of the buffer that was held, of the whole picture, whatever rectangle was asked for */
	CHECK(snapshot(fd, 0, 0, 0, 0, rect, wh, &jpeg, &jlen) == 0);
	CHECK(rect[0] == 0 && rect[1] == 0 && rect[2] == SW && rect[3] == SH && wh[0] == SW && wh[1] == SH);
	CHECK(jpeg_is_buffer(jpeg, jlen, E.last_index));
	CHECK(E.state_at_encode == 2);	/* it was held while it was encoded */
	free(jpeg);
	CHECK(snapshot(fd, 9, 7, 21, 13, rect, wh, &jpeg, &jlen) == 0);
	CHECK(rect[0] == 0 && rect[1] == 0 && rect[2] == SW && rect[3] == SH);	/* the receiver crops */
	CHECK(jpeg_is_buffer(jpeg, jlen, E.last_index));
	free(jpeg);

	/* many snapshots: every buffer comes back, none while still being encoded, the loop keeps running */
	for (int i = 0; i < 30; i++) {
		CHECK(snapshot(fd, 0, 0, 0, 0, rect, wh, &jpeg, &jlen) == 0);
		CHECK(jpeg_is_buffer(jpeg, jlen, E.last_index));
		free(jpeg);
	}
	CHECK(L.holds >= 32 && E.calls >= 32);
	CHECK(L.bad == 0);

	/* a slow encoder is no failure: the server has no patience limit of its own, and the display loop never notices */
	E.delay_ms = 900;
	int frames = L.frames;
	uint64_t t0 = now_us();
	CHECK(snapshot(fd, 0, 0, 0, 0, rect, wh, &jpeg, &jlen) == 0);
	CHECK(now_us() - t0 >= 850000);
	CHECK(L.frames - frames > 100);	/* 900 ms at 4 ms a frame is ~225: the loop kept going */
	free(jpeg);
	E.delay_ms = 0;

	/* an encoder failure is status 2 with no payload; the buffer still goes back and the next one works */
	E.fail = 1;
	CHECK(snapshot(fd, 0, 0, 0, 0, rect, wh, &jpeg, &jlen) == 2 && jlen == 0);
	free(jpeg);
	E.fail = 0;
	CHECK(snapshot(fd, 0, 0, 0, 0, rect, wh, &jpeg, &jlen) == 0);
	free(jpeg);

	/* the loop stops flipping while the signal is still alive: nothing can be handed over, and it says so (no hang) */
	L.paused = 1;
	usleep(20000);
	t0 = now_us();
	CHECK(snapshot(fd, 0, 0, 0, 0, rect, wh, &jpeg, &jlen) == 1);	/* by then there is no signal */
	CHECK(now_us() - t0 < 1800000);
	free(jpeg);
	L.paused = 0;
	usleep(80000);
	CHECK(snapshot(fd, 0, 0, 0, 0, rect, wh, &jpeg, &jlen) == 0);	/* the abandoned request left nothing behind */
	free(jpeg);
	L.run = 0;
	pthread_join(L.th, NULL);
	int held = 0;
	for (int b = 0; b < NBUF; b++)
		held += L.state[b] == 2;
	CHECK(held == 0);	/* no capture buffer was kept from the capture queue */
	CHECK(L.bad == 0);
	L.run = 1;
	pthread_create(&L.th, NULL, display_loop, NULL);
	close(fd);
	teardown();
}

static void test_clients(void)
{
	setup();
	int a = cli_connect(), b, v[5];
	CHECK(a >= 0 && info(a, v) == 0);
	b = cli_connect();	/* a new connection replaces the old one */
	CHECK(b >= 0 && info(b, v) == 0);
	CHECK(info(a, v) != 0);
	close(a);

	uint8_t junk[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
	CHECK(send_raw(b, junk, sizeof junk) == 0);	/* bad magic: dropped */
	CHECK(info(b, v) != 0);
	close(b);

	int c = cli_connect();
	CHECK(c >= 0 && info(c, v) == 0);	/* and the server is none the worse */
	uint8_t big[16];
	put32(big, 0); put32(big + 4, 0); put32(big + 8, 100000); put32(big + 12, 100000);
	CHECK(send_msg(c, 1, big, 16) == 0);	/* an absurd rectangle: dropped */
	CHECK(info(c, v) != 0);
	close(c);

	int d = cli_connect();
	put32(big + 8, 10); put32(big + 12, 10);
	CHECK(d >= 0 && send_msg(d, 1, big, 16) == 0);	/* the length does not match the rectangle */
	CHECK(info(d, v) != 0);
	close(d);

	int e = cli_connect();
	CHECK(e >= 0 && info(e, v) == 0);
	CHECK(L.bad == 0);
	close(e);
	teardown();
}

/* ---------- OVERLAY_RECTS ---------- */

struct rc { int x, y, w, h; uint32_t color; };

/* A message of several rectangles (type 6). count and extra let a test lie about it. */
static int send_rects_ex(int fd, const struct rc *r, int n, int32_t count, int extra)
{
	size_t total = 4 + (size_t)extra;
	for (int i = 0; i < n; i++)
		total += 16 + (size_t)r[i].w * r[i].h * 4;
	uint8_t *p = calloc(1, total), *q = p;
	put32(q, count); q += 4;
	for (int i = 0; i < n; i++) {
		put32(q, r[i].x); put32(q + 4, r[i].y); put32(q + 8, r[i].w); put32(q + 12, r[i].h); q += 16;
		for (size_t k = 0; k < (size_t)r[i].w * r[i].h; k++, q += 4)
			memcpy(q, &r[i].color, 4);
	}
	int rc = send_msg(fd, 6, p, (uint32_t)total);
	free(p);
	return rc;
}

static int send_rects(int fd, const struct rc *r, int n) { return send_rects_ex(fd, r, n, n, 0); }

static int show_rects(int fd, const struct rc *r, int n)
{
	int before = L.shown;
	return send_rects(fd, r, n) == 0 && wait_shown(before + 1);
}

/* The shown overlay is exactly these rectangles (clipped to the output) and nothing else. */
static int shown_rects_are(const struct rc *r, int n)
{
	const uint8_t *buf = L.ov[L.last_shown];
	for (int y = 0; y < OH; y++)
		for (int x = 0; x < OW; x++) {
			uint32_t want = 0;
			for (int i = 0; i < n; i++)
				if (x >= r[i].x && x < r[i].x + r[i].w && y >= r[i].y && y < r[i].y + r[i].h)
					want = r[i].color;
			uint32_t got;
			memcpy(&got, buf + (size_t)y * OPITCH + x * 4, 4);
			if (got != want)
				return 0;
		}
	return 1;
}

static void test_overlay_rects(void)
{
	setup();
	L.multi = 1;
	int fd = cli_connect(), v[5];
	CHECK(fd >= 0);
	/* a badge in one corner and a panel far from it: two rectangles, and nothing in between */
	struct rc a[] = { { 100, 2, 20, 10, 0xFF112233u }, { 5, 70, 60, 20, 0xFF445566u } };
	CHECK(show_rects(fd, a, 2));
	CHECK(shown_rects_are(a, 2));
	/* the next message replaces all of it, in the other buffer, and the old rectangles are cleaned out */
	struct rc b[] = { { 10, 10, 30, 30, 0xFF778899u } };
	CHECK(show_rects(fd, b, 1));
	CHECK(shown_rects_are(b, 1));
	struct rc c[] = { { 100, 2, 20, 10, 0xFFAAAAAAu }, { 0, 0, 4, 4, 0xFFBBBBBBu }, { 50, 40, 8, 8, 0xFFCCCCCCu } };
	CHECK(show_rects(fd, c, 3));
	CHECK(shown_rects_are(c, 3));
	CHECK(show_rects(fd, b, 1));
	CHECK(shown_rects_are(b, 1));
	/* none at all clears everything */
	CHECK(show_rects(fd, NULL, 0));
	CHECK(shown_rects_are(NULL, 0));
	/* partly outside the output is clipped, wholly outside is nothing */
	struct rc d[] = { { -5, -5, 12, 12, 0xFF010101u }, { OW - 3, OH - 3, 10, 10, 0xFF020202u }, { OW + 20, 0, 4, 4, 0xFF030303u } };
	CHECK(show_rects(fd, d, 3));
	CHECK(shown_rects_are(d, 3));
	/* the old single-rectangle message still works and mixes freely with the new one */
	CHECK(show(fd, 7, 8, 9, 10, 0xFF0F0F0Fu));
	CHECK(shown_is(7, 8, 9, 10, 0xFF0F0F0Fu));
	CHECK(show_rects(fd, a, 2));
	CHECK(shown_rects_are(a, 2));
	/* a full rectangle for every one the limit allows */
	struct rc many[64];
	for (int i = 0; i < 64; i++)
		many[i] = (struct rc){ (i % 8) * 16, (i / 8) * 10, 8, 5, 0xFF000000u | (uint32_t)(i + 1) };
	CHECK(show_rects(fd, many, 64));
	CHECK(shown_rects_are(many, 64));
	CHECK(show_rects(fd, b, 1));
	CHECK(shown_rects_are(b, 1));	/* all 64 cleaned again */
	CHECK(info(fd, v) == 0);
	CHECK(L.bad == 0);
	close(fd);
	teardown();
}

static void test_overlay_rects_rejects(void)
{
	int v[5];
	struct rc one[] = { { 1, 1, 4, 4, 0xFF010203u } };
	struct rc two[] = { { 1, 1, 4, 4, 0xFF010203u }, { 9, 9, 3, 3, 0xFF040506u } };

	setup();
	L.multi = 1;
	int fd = cli_connect();
	CHECK(fd >= 0 && send_rects_ex(fd, one, 1, 65, 0) == 0);	/* more rectangles than the limit */
	CHECK(info(fd, v) != 0);
	close(fd);

	fd = cli_connect();
	CHECK(fd >= 0 && send_rects_ex(fd, one, 1, -1, 0) == 0);	/* a negative count */
	CHECK(info(fd, v) != 0);
	close(fd);

	fd = cli_connect();
	CHECK(fd >= 0 && send_rects_ex(fd, two, 2, 1, 0) == 0);	/* more bytes than the count accounts for */
	CHECK(info(fd, v) != 0);
	close(fd);

	fd = cli_connect();
	CHECK(fd >= 0 && send_rects_ex(fd, one, 1, 1, 12) == 0);	/* trailing junk */
	CHECK(info(fd, v) != 0);
	close(fd);

	fd = cli_connect();
	uint8_t shortmsg[8];
	put32(shortmsg, 2); put32(shortmsg + 4, 0);
	CHECK(fd >= 0 && send_msg(fd, 6, shortmsg, 8) == 0);	/* promises two rectangles, carries none */
	CHECK(info(fd, v) != 0);
	close(fd);

	fd = cli_connect();
	uint8_t absurd[20];
	put32(absurd, 1); put32(absurd + 4, 0); put32(absurd + 8, 0); put32(absurd + 12, 100000); put32(absurd + 16, 100000);
	CHECK(fd >= 0 && send_msg(fd, 6, absurd, 20) == 0);	/* an absurd rectangle */
	CHECK(info(fd, v) != 0);
	close(fd);

	fd = cli_connect();
	put32(absurd + 12, 0); put32(absurd + 16, 5);
	CHECK(fd >= 0 && send_msg(fd, 6, absurd, 20) == 0);	/* an empty rectangle inside a list */
	CHECK(info(fd, v) != 0);
	close(fd);

	fd = cli_connect();
	CHECK(fd >= 0 && info(fd, v) == 0);	/* and the server is none the worse */
	CHECK(L.bad == 0);
	close(fd);
	teardown();
}

static void test_overlay_rects_while_the_flip_stalls(void)
{
	setup();
	L.multi = 1;
	int fd = cli_connect();
	struct rc a[] = { { 0, 0, 10, 10, 0xFF0000FFu } };
	struct rc b[] = { { 0, 0, 20, 20, 0xFF00FF00u } };
	struct rc dropped[] = { { 5, 5, 6, 6, 0xFFFF0000u }, { 40, 40, 6, 6, 0xFFFF0000u } };
	CHECK(show_rects(fd, a, 1));
	L.flip_ms = 450;
	CHECK(show_rects(fd, b, 1));
	CHECK(send_rects(fd, dropped, 2) == 0);	/* nowhere to go for 200 ms: dropped, and its bytes are skipped in step */
	usleep(600000);
	L.flip_ms = 0;
	struct rc after[] = { { 7, 8, 9, 10, 0xFF0F0F0Fu } };
	CHECK(show_rects(fd, after, 1));
	CHECK(shown_rects_are(after, 1));
	CHECK(L.bad == 0);
	close(fd);
	teardown();
}

int main(int argc, char **argv)
{
	if (argc == 4 && !strcmp(argv[1], "--serve")) {	/* the server and a fake 60 Hz display loop, for novl_load */
		sock_arg = argv[2];
		setup();
		L.frame_ms = 16;
		sleep((unsigned)atoi(argv[3]));
		teardown();
		return 0;
	}
	test_info_and_signal();
	test_overlay();
	test_no_write_while_scanned_out();
	test_stalled_flip_drops_one_update();
	test_flood();
	test_big_overlay_does_not_stall_the_loop();
	test_snapshot();
	test_clients();
	test_overlay_rects();
	test_overlay_rects_rejects();
	test_overlay_rects_while_the_flip_stalls();
	printf("novl: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
