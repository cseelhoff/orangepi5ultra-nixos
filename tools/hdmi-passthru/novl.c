/* novl: see novl.h. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "novl.h"

#include <errno.h>
#include <grp.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAGIC 0x4C564F4Eu /* "NOVL" */
enum { T_OVERLAY = 1, T_SNAPSHOT, T_SNAPSHOT_REPLY, T_INFO, T_INFO_REPLY };
enum { ST_OK = 0, ST_NO_SIGNAL = 1, ST_FAILED = 2 };

#define SIGNAL_MS 500		/* no frame for this long: no signal */
#define HOLD_WAIT_MS 250	/* the daemon gives up on a reply after 400 */
#define FLIP_WAIT_MS 200
#define IO_TIMEOUT_S 2
#define MAX_PIXELS (8192 * 4320)
#define MAX_SIDE 16384

struct rect { int x, y, w, h; };

struct novl {
	struct novl_source src;
	struct novl_overlay ov;
	char path[108];
	int lfd, cfd;
	pthread_t thread;
	atomic_int quit;
	_Atomic uint64_t last_frame_ns;

	/* The hand-over with the display loop is lock-free: the loop's hooks are a few atomic
	 * operations and can never wait on this thread. The server thread polls for what it needs. */
	_Atomic unsigned ovs;	/* overlay state, see ov_make(); zero is the right start */
	atomic_int snap_want;	/* a snapshot waits for the loop to hand over a capture buffer */
	atomic_int snap_held;	/* the capture buffer handed over, plus one; 0 for none */

	struct rect dirty[2];	/* what was last drawn into each overlay buffer; server thread only */
};

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static int32_t get32(const uint8_t *p)
{
	return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
}

static void put32(uint8_t *p, int32_t v)
{
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* The overlay buffers' state in one word, moved only by compare-and-swap:
 *   bit 0     front: the buffer the loop last took
 *   bits 1-2  ready: a finished buffer not yet taken, plus one (0: none)
 *   bit 3     busy: the commit that took `front` has not latched, so the other buffer may still
 *             be scanned out */
static unsigned ov_make(unsigned front, int ready, unsigned busy) { return front | (unsigned)(ready + 1) << 1 | busy << 3; }
static unsigned ov_front(unsigned s) { return s & 1; }
static int ov_ready(unsigned s) { return (int)((s >> 1) & 3) - 1; }
static unsigned ov_busy(unsigned s) { return (s >> 3) & 1; }

static void micros(unsigned us)
{
	struct timespec ts = { .tv_nsec = (long)us * 1000 };
	nanosleep(&ts, NULL);
}

static int has_signal(struct novl *n)
{
	uint64_t t = atomic_load_explicit(&n->last_frame_ns, memory_order_relaxed);
	return t && now_ns() - t < SIGNAL_MS * 1000000ull;
}

/* ---------- hooks for the display loop: wait-free, no locks, no system calls but the clock ---------- */

void novl_frame(struct novl *n)
{
	atomic_store_explicit(&n->last_frame_ns, now_ns(), memory_order_relaxed);
}

int novl_overlay_next(struct novl *n)
{
	unsigned s = atomic_load(&n->ovs);
	while (ov_ready(s) >= 0) {
		int r = ov_ready(s);
		if (atomic_compare_exchange_weak(&n->ovs, &s, ov_make((unsigned)r, -1, 1)))
			return r;
	}
	return -1;
}

void novl_flip_done(struct novl *n)
{
	unsigned s = atomic_load(&n->ovs);
	while (ov_busy(s) && !atomic_compare_exchange_weak(&n->ovs, &s, ov_make(ov_front(s), ov_ready(s), 0)))
		;
}

int novl_hold(struct novl *n, int index)
{
	if (!atomic_load(&n->snap_want) || !atomic_exchange(&n->snap_want, 0))
		return 0;
	atomic_store(&n->snap_held, index + 1);
	return 1;
}

/* ---------- socket I/O (server thread) ---------- */

static int recv_all(int fd, void *buf, size_t len)
{
	uint8_t *p = buf;
	while (len) {
		ssize_t r = recv(fd, p, len, 0);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return -1;	/* closed, or no data within the timeout */
		p += r;
		len -= (size_t)r;
	}
	return 0;
}

static int send_all(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	while (len) {
		ssize_t w = send(fd, p, len, MSG_NOSIGNAL);
		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			return -1;
		p += w;
		len -= (size_t)w;
	}
	return 0;
}

static int discard(int fd, uint64_t len)
{
	uint8_t junk[4096];
	while (len) {
		size_t k = len < sizeof junk ? (size_t)len : sizeof junk;
		if (recv_all(fd, junk, k))
			return -1;
		len -= k;
	}
	return 0;
}

/* ---------- OVERLAY ---------- */

static void clear_rect(struct novl *n, int b, struct rect r)
{
	for (int y = 0; y < r.h; y++)
		memset(n->ov.map[b] + (size_t)(r.y + y) * n->ov.pitch + (size_t)r.x * 4, 0, (size_t)r.w * 4);
}

/* -1: drop the client */
static int do_overlay(struct novl *n, uint32_t len)
{
	uint8_t hdr[16];
	if (len < 16 || recv_all(n->cfd, hdr, sizeof hdr))
		return -1;
	struct rect r = { get32(hdr), get32(hdr + 4), get32(hdr + 8), get32(hdr + 12) };
	int empty = r.w == 0 && r.h == 0;
	if (empty ? len != 16
		  : r.w <= 0 || r.h <= 0 || r.w > MAX_SIDE || r.h > MAX_SIDE || (int64_t)r.w * r.h > MAX_PIXELS ||
		    (uint64_t)len != 16 + (uint64_t)r.w * r.h * 4)
		return -1;

	/* The buffer to draw into is the one the loop is not showing. A finished but untaken
	 * overlay is withdrawn first (this one replaces it), so the loop never takes a half-drawn
	 * buffer. While the commit that took the front buffer has not latched, the other buffer
	 * may still be scanned out, so wait for that (a few hundred microseconds of polling at most
	 * per frame: the overlay is allowed to be a frame late, the video is not). */
	int back = -1;
	uint64_t deadline = now_ns() + FLIP_WAIT_MS * 1000000ull;
	for (;;) {
		unsigned s = atomic_load(&n->ovs);
		if (ov_ready(s) >= 0) {
			atomic_compare_exchange_strong(&n->ovs, &s, ov_make(ov_front(s), -1, ov_busy(s)));
			continue;	/* look again: the loop may have taken it first */
		}
		if (!ov_busy(s)) {
			back = 1 - (int)ov_front(s);
			break;
		}
		if (n->quit || now_ns() >= deadline)
			break;
		micros(200);
	}
	if (back < 0)
		return discard(n->cfd, len - 16);	/* the display is not flipping; the next update replaces this one */

	clear_rect(n, back, n->dirty[back]);
	n->dirty[back] = (struct rect){ 0 };
	if (!empty) {
		/* what lands inside the output; the rest is read and dropped. 64-bit: x and y are the client's. */
		int64_t cx0 = r.x < 0 ? 0 : r.x, cy0 = r.y < 0 ? 0 : r.y;
		int64_t cx1 = (int64_t)r.x + r.w, cy1 = (int64_t)r.y + r.h;
		if (cx1 > (int64_t)n->ov.width) cx1 = n->ov.width;
		if (cy1 > (int64_t)n->ov.height) cy1 = n->ov.height;
		int visible = cx1 > cx0 && cy1 > cy0;
		uint8_t *row = malloc((size_t)r.w * 4);
		if (!row)
			return -1;
		if (visible)
			n->dirty[back] = (struct rect){ (int)cx0, (int)cy0, (int)(cx1 - cx0), (int)(cy1 - cy0) };	/* set before the first write: a half-drawn buffer is cleaned next time */
		for (int y = 0; y < r.h; y++) {
			if (recv_all(n->cfd, row, (size_t)r.w * 4)) {
				free(row);
				return -1;
			}
			int64_t dy = (int64_t)r.y + y;
			if (!visible || dy < cy0 || dy >= cy1)
				continue;
			memcpy(n->ov.map[back] + (size_t)dy * n->ov.pitch + (size_t)cx0 * 4,
			       row + (size_t)(cx0 - r.x) * 4, (size_t)(cx1 - cx0) * 4);
		}
		free(row);
	}
	unsigned s = atomic_load(&n->ovs);	/* nothing else moves it now: the state is (front, none, idle) */
	while (!atomic_compare_exchange_weak(&n->ovs, &s, ov_make(ov_front(s), back, ov_busy(s))))
		;
	return 0;
}

/* ---------- SNAPSHOT ---------- */

struct yuv { int y_off, y_mul, rv, gu, gv, bu; };	/* 16.16 fixed point */

static struct yuv yuv_coeffs(int bt601, int full)
{
	double kr = bt601 ? 0.299 : 0.2126, kb = bt601 ? 0.114 : 0.0722, kg = 1 - kr - kb;
	double ys = full ? 1.0 : 255.0 / 219.0, cs = full ? 1.0 : 255.0 / 224.0;
	return (struct yuv){
		.y_off = full ? 0 : 16,
		.y_mul = (int)(ys * 65536 + 0.5),
		.rv = (int)(cs * 2 * (1 - kr) * 65536 + 0.5),
		.gu = (int)(cs * 2 * kb * (1 - kb) / kg * 65536 + 0.5),
		.gv = (int)(cs * 2 * kr * (1 - kr) / kg * 65536 + 0.5),
		.bu = (int)(cs * 2 * (1 - kb) * 65536 + 0.5),
	};
}

static uint8_t clamp8(int v)
{
	return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

/* ponytail: scalar C, about 4 ns a pixel; a 1080p full frame is ~10 ms on a worker thread.
 * NEON or the RGA would be the upgrade if snapshots ever became frequent. */
static void convert_crop(const struct novl_source *s, const uint8_t *buf, struct rect r, uint8_t *out)
{
	struct yuv k = yuv_coeffs(s->bt601, s->full_range);
	const uint8_t *chroma = buf + (size_t)s->stride * s->height;
	for (int y = 0; y < r.h; y++) {
		int sy = r.y + y;
		const uint8_t *yr = buf + (size_t)sy * s->stride;
		const uint8_t *cr = s->fmt == NOVL_NV12 ? chroma + (size_t)(sy >> 1) * s->stride
			: s->fmt == NOVL_NV16 ? chroma + (size_t)sy * s->stride
			: chroma + (size_t)sy * s->stride * 2;
		for (int x = 0; x < r.w; x++) {
			int sx = r.x + x;
			const uint8_t *uv = s->fmt == NOVL_NV24 ? cr + (size_t)sx * 2 : cr + (size_t)(sx >> 1) * 2;
			int yy = ((int)yr[sx] - k.y_off) * k.y_mul, cb = (int)uv[0] - 128, rr = (int)uv[1] - 128;
			*out++ = clamp8((yy + k.rv * rr + 32768) >> 16);
			*out++ = clamp8((yy - k.gu * cb - k.gv * rr + 32768) >> 16);
			*out++ = clamp8((yy + k.bu * cb + 32768) >> 16);
		}
	}
}

static int reply_snapshot(struct novl *n, int status, struct rect r, const uint8_t *rgb)
{
	size_t pixels = status == ST_OK ? (size_t)r.w * r.h * 3 : 0;
	uint8_t head[40];
	put32(head, (int32_t)MAGIC); put32(head + 4, T_SNAPSHOT_REPLY); put32(head + 8, (int32_t)(28 + pixels));
	put32(head + 12, r.x); put32(head + 16, r.y); put32(head + 20, r.w); put32(head + 24, r.h);
	put32(head + 28, (int32_t)n->src.width); put32(head + 32, (int32_t)n->src.height); put32(head + 36, status);
	return send_all(n->cfd, head, sizeof head) || (pixels && send_all(n->cfd, rgb, pixels)) ? -1 : 0;
}

/* Waits for the loop to hand over a complete capture buffer; -1 if it does not. */
static int take_stable_buffer(struct novl *n)
{
	atomic_store(&n->snap_want, 1);
	uint64_t deadline = now_ns() + HOLD_WAIT_MS * 1000000ull;
	int held;
	while (!(held = atomic_load(&n->snap_held)) && !n->quit && now_ns() < deadline)
		micros(500);
	if (!held && atomic_exchange(&n->snap_want, 0))
		return -1;	/* the offer stood and nobody took it */
	/* The loop took it just now and is about to say which buffer. */
	for (int i = 0; i < 1000 && !held; i++) {
		micros(100);
		held = atomic_load(&n->snap_held);
	}
	return held - 1;
}

static int do_snapshot(struct novl *n, uint32_t len)
{
	uint8_t body[16];
	if (len != 16 || recv_all(n->cfd, body, sizeof body))
		return -1;
	const struct novl_source *s = &n->src;
	struct rect want = { get32(body), get32(body + 4), get32(body + 8), get32(body + 12) };
	struct rect r = { 0, 0, (int)s->width, (int)s->height };	/* w = 0 (or h = 0): the whole picture */
	if (want.w > 0 && want.h > 0) {
		int64_t x1 = (int64_t)want.x + want.w, y1 = (int64_t)want.y + want.h;
		r.x = want.x < 0 ? 0 : want.x;
		r.y = want.y < 0 ? 0 : want.y;
		r.w = (int)(x1 > (int64_t)s->width ? (int64_t)s->width : x1) - r.x;
		r.h = (int)(y1 > (int64_t)s->height ? (int64_t)s->height : y1) - r.y;
	}
	struct rect none = { 0 };
	if (r.w <= 0 || r.h <= 0 || r.x >= (int)s->width || r.y >= (int)s->height)
		return reply_snapshot(n, ST_FAILED, none, NULL);
	if (!has_signal(n))
		return reply_snapshot(n, ST_NO_SIGNAL, none, NULL);

	int b = take_stable_buffer(n);
	if (b < 0)
		return reply_snapshot(n, has_signal(n) ? ST_FAILED : ST_NO_SIGNAL, none, NULL);
	int status = ST_FAILED;
	uint8_t *rgb = malloc((size_t)r.w * r.h * 3);
	uint8_t *map = rgb ? mmap(NULL, s->size, PROT_READ, MAP_SHARED, s->dmafd[b], 0) : MAP_FAILED;
	if (map != MAP_FAILED) {
		struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
		ioctl(s->dmafd[b], DMA_BUF_IOCTL_SYNC, &sync);	/* not every exporter needs it; ignore ENOTTY */
		convert_crop(s, map, r, rgb);
		sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
		ioctl(s->dmafd[b], DMA_BUF_IOCTL_SYNC, &sync);
		munmap(map, s->size);
		status = ST_OK;
	}
	s->requeue(s->ctx, b);
	atomic_store(&n->snap_held, 0);

	int rc = reply_snapshot(n, status, status == ST_OK ? r : none, rgb);
	free(rgb);
	return rc;
}

/* ---------- INFO and the serve loop ---------- */

static int do_info(struct novl *n, uint32_t len)
{
	if (len != 0)
		return -1;
	uint8_t reply[32];
	put32(reply, (int32_t)MAGIC); put32(reply + 4, T_INFO_REPLY); put32(reply + 8, 20);
	put32(reply + 12, (int32_t)n->src.width); put32(reply + 16, (int32_t)n->src.height);
	put32(reply + 20, (int32_t)n->ov.width); put32(reply + 24, (int32_t)n->ov.height);
	put32(reply + 28, has_signal(n));
	return send_all(n->cfd, reply, sizeof reply);
}

static int serve_one(struct novl *n)
{
	uint8_t h[12];
	if (recv_all(n->cfd, h, sizeof h) || (uint32_t)get32(h) != MAGIC)
		return -1;
	uint32_t len = (uint32_t)get32(h + 8);
	switch (get32(h + 4)) {
	case T_OVERLAY: return do_overlay(n, len);
	case T_SNAPSHOT: return do_snapshot(n, len);
	case T_INFO: return do_info(n, len);
	default: return -1;
	}
}

static void *serve(void *arg)
{
	struct novl *n = arg;
	sigset_t all;
	sigfillset(&all);
	pthread_sigmask(SIG_BLOCK, &all, NULL);	/* Ctrl-C must interrupt the display loop's poll, not this thread's */
	/* The display loop is the latency path. A batch-policy thread does not preempt the thread it
	 * wakes up next to (CFS), and nice 10 gives it a small share when they do compete. Best effort. */
	struct sched_param sp = { 0 };
	sched_setscheduler(0, SCHED_BATCH, &sp);	/* pid 0: this thread, on Linux */
	setpriority(PRIO_PROCESS, 0, 10);
	struct timeval tv = { .tv_sec = IO_TIMEOUT_S };
	while (!n->quit) {
		struct pollfd pf[2] = { { n->lfd, POLLIN, 0 }, { n->cfd, POLLIN, 0 } };	/* a negative fd is ignored */
		if (poll(pf, 2, 200) < 0 && errno != EINTR)
			break;
		if (n->cfd >= 0 && (pf[1].revents & (POLLIN | POLLHUP | POLLERR)) && serve_one(n)) {
			close(n->cfd);
			n->cfd = -1;
		}
		if (pf[0].revents & POLLIN) {	/* a new client replaces the old one */
			int fd = accept4(n->lfd, NULL, NULL, SOCK_CLOEXEC);
			if (fd >= 0) {
				if (n->cfd >= 0)
					close(n->cfd);
				n->cfd = fd;
				setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
				setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
			}
		}
	}
	return NULL;
}

/* ---------- start and stop ---------- */

static struct novl *fail(struct novl *n, const char *what)
{
	int e = errno;
	fprintf(stderr, "novl: %s: %s\n", what, strerror(e));
	if (n->lfd >= 0)
		close(n->lfd);
	free(n);
	errno = e;
	return NULL;
}

struct novl *novl_start(const char *path, const char *group, const struct novl_source *src,
			const struct novl_overlay *ov)
{
	struct novl *n = calloc(1, sizeof *n);
	if (!n)
		return NULL;
	n->src = *src;
	n->ov = *ov;
	n->lfd = n->cfd = -1;
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	if (strlen(path) >= sizeof a.sun_path) {
		errno = ENAMETOOLONG;
		return fail(n, "socket path");
	}
	strcpy(n->path, path);
	strcpy(a.sun_path, path);

	struct group *g = NULL;
	if (group && !(g = getgrnam(group))) {
		errno = ENOENT;
		return fail(n, group);
	}
	n->lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (n->lfd < 0)
		return fail(n, "socket");
	unlink(path);	/* a stale socket from an earlier run */
	mode_t old = umask(0117);	/* created 0660: the picture of the meeting machine is not for everyone */
	int rc = bind(n->lfd, (struct sockaddr *)&a, sizeof a);
	umask(old);
	if (rc || listen(n->lfd, 1))
		return fail(n, path);
	if (g && chown(path, (uid_t)-1, g->gr_gid))
		return fail(n, path);

	if ((errno = pthread_create(&n->thread, NULL, serve, n)))
		return fail(n, "thread");
	return n;
}

void novl_stop(struct novl *n)
{
	if (!n)
		return;
	n->quit = 1;
	pthread_join(n->thread, NULL);
	if (n->cfd >= 0)
		close(n->cfd);
	close(n->lfd);
	unlink(n->path);
	free(n);
}
