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
	atomic_int holds, last_hold;
	atomic_int state[NBUF];	/* 0 queued, 1 on screen, 2 held by a snapshot */
	atomic_long max_hook_us;
	int on_screen;
	uint8_t *ov[2];
} L;

static uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void requeue(void *ctx, int i)
{
	(void)ctx;
	if (atomic_exchange(&L.state[i], 0) != 2)
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
			if (!overlay_box(L.ov[o], &x, &y, &w, &h, &c))
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

/* ---------- a source with known pixels ---------- */

static enum novl_fmt cur_fmt;

static int chroma_pitch(enum novl_fmt f) { return f == NOVL_NV24 ? SSTRIDE * 2 : SSTRIDE; }
static int chroma_rows(enum novl_fmt f) { return f == NOVL_NV12 ? SH / 2 : SH; }
static int Y_at(int b, int x, int y) { return 16 + (x * 3 + y * 5 + b * 40) % 200; }
static int U_at(int b, int cx, int cy) { return 64 + (cx * 7 + cy * 3 + b * 11) % 128; }
static int V_at(int b, int cx, int cy) { return 64 + (cx * 5 + cy * 9 + b * 17) % 128; }

static size_t source_size(enum novl_fmt f) { return (size_t)SSTRIDE * SH + (size_t)chroma_pitch(f) * chroma_rows(f); }

static int make_buffer(enum novl_fmt f, int b)
{
	size_t size = source_size(f);
	int fd = memfd_create("novl-test", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t)size))
		abort();
	uint8_t *m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	for (int y = 0; y < SH; y++)
		for (int x = 0; x < SW; x++)
			m[y * SSTRIDE + x] = (uint8_t)Y_at(b, x, y);
	uint8_t *c = m + (size_t)SSTRIDE * SH;
	int cw = f == NOVL_NV24 ? SW : SW / 2;
	for (int cy = 0; cy < chroma_rows(f); cy++)
		for (int cx = 0; cx < cw; cx++) {
			c[(size_t)cy * chroma_pitch(f) + cx * 2] = (uint8_t)U_at(b, cx, cy);
			c[(size_t)cy * chroma_pitch(f) + cx * 2 + 1] = (uint8_t)V_at(b, cx, cy);
		}
	munmap(m, size);
	return fd;
}

/* The picture as an independent float derivation (not the code's fixed-point coefficients). */
static void ref_rgb(int Y, int U, int V, int bt601, int full, int out[3])
{
	double kr = bt601 ? 0.299 : 0.2126, kb = bt601 ? 0.114 : 0.0722, kg = 1 - kr - kb;
	double y = full ? Y / 255.0 : (Y - 16) / 219.0;
	double cb = (U - 128) / (full ? 255.0 : 224.0), cr = (V - 128) / (full ? 255.0 : 224.0);
	double r = y + 2 * (1 - kr) * cr, b = y + 2 * (1 - kb) * cb, g = (y - kr * r - kb * b) / kg;
	double v[3] = { r, g, b };
	for (int i = 0; i < 3; i++) {
		double d = v[i] * 255 + 0.5;
		out[i] = d < 0 ? 0 : d > 255 ? 255 : (int)d;
	}
}

static void expected(int b, int x, int y, int bt601, int full, int out[3])
{
	int cx = x >> 1, cy = cur_fmt == NOVL_NV12 ? y >> 1 : y;
	if (cur_fmt == NOVL_NV24)
		cx = x;
	ref_rgb(Y_at(b, x, y), U_at(b, cx, cy), V_at(b, cx, cy), bt601, full, out);
}

/* ---------- harness ---------- */

static char sock_path[108];
static const char *sock_arg;	/* --serve: the path to listen on instead of a private one */
static struct novl_source src;

static void setup(enum novl_fmt f, int bt601, int full)
{
	cur_fmt = f;
	memset(&src, 0, sizeof src);
	src.fmt = f; src.width = SW; src.height = SH; src.stride = SSTRIDE; src.size = source_size(f);
	src.bt601 = bt601; src.full_range = full; src.nbuf = NBUF;
	for (int b = 0; b < NBUF; b++)
		src.dmafd[b] = make_buffer(f, b);
	src.requeue = requeue;
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
	for (int b = 0; b < NBUF; b++)
		close(src.dmafd[b]);
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

/* returns the reply status, or -1 if there was no proper reply; *rgb is malloc'd on success */
static int snapshot(int fd, int x, int y, int w, int h, int rect[4], int src_wh[2], uint8_t **rgb)
{
	uint8_t req[16], hd[12], meta[28];
	put32(req, x); put32(req + 4, y); put32(req + 8, w); put32(req + 12, h);
	if (send_msg(fd, 2, req, 16) || recv_n(fd, hd, 12) || get32(hd + 4) != 3 || get32(hd + 8) < 28 || recv_n(fd, meta, 28))
		return -1;
	for (int i = 0; i < 4; i++)
		rect[i] = get32(meta + i * 4);
	src_wh[0] = get32(meta + 16); src_wh[1] = get32(meta + 20);
	size_t pixels = (size_t)get32(hd + 8) - 28;
	*rgb = NULL;
	if (pixels) {
		*rgb = malloc(pixels);
		if (recv_n(fd, *rgb, pixels))
			return -1;
	}
	return get32(meta + 24);
}

/* ---------- tests ---------- */

static void test_info_and_signal(void)
{
	setup(NOVL_NV12, 0, 0);
	L.paused = 1;	/* no frame yet */
	int fd = cli_connect(), v[5];
	CHECK(fd >= 0 && info(fd, v) == 0);
	CHECK(v[0] == SW && v[1] == SH && v[2] == OW && v[3] == OH);
	CHECK(v[4] == 0);
	int rect[4], wh[2];
	uint8_t *rgb;
	CHECK(snapshot(fd, 0, 0, 0, 0, rect, wh, &rgb) == 1);	/* no signal */
	free(rgb);

	L.paused = 0;
	usleep(60000);
	CHECK(info(fd, v) == 0 && v[4] == 1);
	L.paused = 1;
	usleep(700000);
	CHECK(info(fd, v) == 0 && v[4] == 0);	/* frames stopped: no signal again */
	CHECK(snapshot(fd, 0, 0, 0, 0, rect, wh, &rgb) == 1);
	free(rgb);
	close(fd);
	teardown();
}

static void test_overlay(void)
{
	setup(NOVL_NV12, 0, 0);
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
	setup(NOVL_NV12, 0, 0);
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
	setup(NOVL_NV12, 0, 0);
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
	setup(NOVL_NV12, 0, 0);
	int fd = cli_connect();
	srand(7);
	for (int i = 0; i < 400; i++) {
		int w = 1 + rand() % OW, h = 1 + rand() % OH;
		CHECK(send_overlay(fd, rand() % (OW - w + 1), rand() % (OH - h + 1), w, h, 0xFF000000u | (uint32_t)(i * 7919 + 1)) == 0);
		if (i % 7 == 0)
			usleep(3000);
	}
	CHECK(send_overlay(fd, 3, 4, 5, 6, 0xFF123456u) == 0);
	for (int i = 0; i < 500 && !shown_is(3, 4, 5, 6, 0xFF123456u); i++)	/* the last update, behind a queue of earlier ones */
		usleep(2000);
	CHECK(shown_is(3, 4, 5, 6, 0xFF123456u));
	CHECK(L.shown > 5);
	CHECK(L.bad == 0);	/* every buffer the loop ever took was one whole overlay */
	close(fd);
	teardown();
}

static void test_big_overlay_does_not_stall_the_loop(void)
{
	setup(NOVL_NV12, 0, 0);
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

static void check_snapshot_case(enum novl_fmt f, int bt601, int full)
{
	setup(f, bt601, full);
	int fd = cli_connect(), rect[4], wh[2];
	uint8_t *rgb;
	usleep(50000);
	int rc = snapshot(fd, 0, 0, 0, 0, rect, wh, &rgb);
	CHECK(rc == 0 && rect[0] == 0 && rect[1] == 0 && rect[2] == SW && rect[3] == SH && wh[0] == SW && wh[1] == SH);
	int held = L.last_hold, worst = 0;
	if (rc == 0)
		for (int y = 0; y < SH; y++)
			for (int x = 0; x < SW; x++) {
				int e[3];
				expected(held, x, y, bt601, full, e);
				for (int k = 0; k < 3; k++) {
					int d = abs(e[k] - rgb[((size_t)y * SW + x) * 3 + k]);
					if (d > worst)
						worst = d;
				}
			}
	CHECK(worst <= 2);	/* fixed point against a float derivation */
	free(rgb);

	/* a crop: the same pixels as the full picture's, at the right place */
	rc = snapshot(fd, 9, 7, 21, 13, rect, wh, &rgb);
	CHECK(rc == 0 && rect[0] == 9 && rect[1] == 7 && rect[2] == 21 && rect[3] == 13);
	if (rc == 0) {
		worst = 0;
		held = L.last_hold;
		for (int y = 0; y < 13; y++)
			for (int x = 0; x < 21; x++) {
				int e[3];
				expected(held, 9 + x, 7 + y, bt601, full, e);
				for (int k = 0; k < 3; k++) {
					int d = abs(e[k] - rgb[((size_t)y * 21 + x) * 3 + k]);
					if (d > worst)
						worst = d;
				}
			}
		CHECK(worst <= 2);
	}
	free(rgb);
	close(fd);
	teardown();
}

static void test_snapshot_formats(void)
{
	for (int f = NOVL_NV12; f <= NOVL_NV24; f++)
		for (int bt601 = 0; bt601 < 2; bt601++)
			for (int full = 0; full < 2; full++)
				check_snapshot_case((enum novl_fmt)f, bt601, full);
}

static void test_snapshot_clamping_and_holds(void)
{
	setup(NOVL_NV12, 0, 0);
	int fd = cli_connect(), rect[4], wh[2];
	uint8_t *rgb;
	usleep(50000);
	CHECK(snapshot(fd, SW - 5, SH - 4, 100, 100, rect, wh, &rgb) == 0);	/* clamped to the picture */
	CHECK(rect[0] == SW - 5 && rect[1] == SH - 4 && rect[2] == 5 && rect[3] == 4);
	free(rgb);
	CHECK(snapshot(fd, -10, -10, 20, 20, rect, wh, &rgb) == 0);
	CHECK(rect[0] == 0 && rect[1] == 0 && rect[2] == 10 && rect[3] == 10);
	free(rgb);
	CHECK(snapshot(fd, SW + 3, 0, 10, 10, rect, wh, &rgb) == 2 && rgb == NULL);	/* nothing of it is inside */
	CHECK(snapshot(fd, 0, 0, 10, 10, rect, wh, &rgb) == 0);	/* and the stream is still in step */
	free(rgb);

	/* many snapshots: every buffer comes back, the loop keeps running */
	int frames = L.shown;
	(void)frames;
	for (int i = 0; i < 30; i++) {
		CHECK(snapshot(fd, 0, 0, 8, 8, rect, wh, &rgb) == 0);
		free(rgb);
	}
	CHECK(L.holds >= 30);
	CHECK(L.bad == 0);

	/* the loop stops after a frame: the signal is still alive but nothing can be handed over */
	L.paused = 1;
	usleep(20000);
	uint64_t t0 = now_us();
	CHECK(snapshot(fd, 0, 0, 8, 8, rect, wh, &rgb) == 2);
	CHECK(now_us() - t0 < 380000);	/* inside the daemon's 400 ms patience */
	L.paused = 0;
	usleep(80000);
	CHECK(snapshot(fd, 0, 0, 8, 8, rect, wh, &rgb) == 0);	/* the abandoned request left nothing behind */
	free(rgb);
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
	setup(NOVL_NV12, 0, 0);
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

int main(int argc, char **argv)
{
	if (argc == 4 && !strcmp(argv[1], "--serve")) {	/* the server and a fake 60 Hz display loop, for novl_load */
		sock_arg = argv[2];
		setup(NOVL_NV12, 0, 0);
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
	test_snapshot_formats();
	test_snapshot_clamping_and_holds();
	test_clients();
	printf("novl: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
