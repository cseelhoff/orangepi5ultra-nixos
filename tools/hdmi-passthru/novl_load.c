/* novl_load: a stand-in for the edge daemon, to measure what -S costs the video path.
 *
 *   cc -O2 -o novl_load novl_load.c
 *
 *   hdmi-passthru -q -s 60                       # baseline: note the "capture timestamp -> vblank latch" line
 *   hdmi-passthru -q -s 60 -S /tmp/novl.sock &   # the same, with the server
 *   ./novl_load /tmp/novl.sock 60                # ... under load
 *
 * Load: an overlay of about 60% x 40% of the output sent 30 times a second with new pixels every
 * time (like an answer being streamed), and a full-picture snapshot every 2 s. The printed
 * mean / p50 / p95 / p99 / max and the dropped-frame count of the two hdmi-passthru runs are what to
 * compare; the video path has gained nothing if they match.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void put32(uint8_t *p, int32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static int32_t get32(const uint8_t *p) { return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24); }

static int send_all(int fd, const void *p, size_t n)
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

static int recv_all(int fd, void *p, size_t n)
{
	uint8_t *b = p;
	while (n) {
		ssize_t r = recv(fd, b, n, 0);
		if (r <= 0)
			return -1;
		b += r; n -= (size_t)r;
	}
	return 0;
}

static void header(uint8_t *h, uint32_t type, uint32_t len)
{
	put32(h, 0x4C564F4E); put32(h + 4, (int32_t)type); put32(h + 8, (int32_t)len);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s SOCKET [seconds]\n", argv[0]);
		return 2;
	}
	double seconds = argc > 2 ? atof(argv[2]) : 30;
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof a.sun_path, "%s", argv[1]);
	if (connect(fd, (struct sockaddr *)&a, sizeof a)) {
		perror(argv[1]);
		return 1;
	}

	uint8_t req[12], rep[12], body[20];
	header(req, 4, 0);
	if (send_all(fd, req, 12) || recv_all(fd, rep, 12) || get32(rep + 8) != 20 || recv_all(fd, body, 20)) {
		fprintf(stderr, "no INFO reply\n");
		return 1;
	}
	int src_w = get32(body), src_h = get32(body + 4), out_w = get32(body + 8), out_h = get32(body + 12);
	printf("picture %dx%d, output %dx%d, signal %d\n", src_w, src_h, out_w, out_h, get32(body + 16));

	int rx = out_w / 16, ry = out_h / 2, rw = out_w * 6 / 10, rh = out_h * 4 / 10;
	size_t pixels = (size_t)rw * rh * 4;
	uint8_t *msg = malloc(12 + 16 + pixels);
	header(msg, 1, (uint32_t)(16 + pixels));
	put32(msg + 12, rx); put32(msg + 16, ry); put32(msg + 20, rw); put32(msg + 24, rh);

	double t0 = now_ms(), next_overlay = t0, next_snap = t0 + 2000;
	long overlays = 0, snaps = 0, snap_failed = 0;
	double snap_min = 1e9, snap_max = 0, snap_sum = 0;
	while (now_ms() - t0 < seconds * 1000) {
		double t = now_ms();
		if (t >= next_snap) {
			next_snap += 2000;
			uint8_t sreq[28];
			header(sreq, 2, 16);
			put32(sreq + 12, 0); put32(sreq + 16, 0); put32(sreq + 20, 0); put32(sreq + 24, 0);
			uint8_t sh[12], meta[28];
			if (send_all(fd, sreq, sizeof sreq) || recv_all(fd, sh, 12) || recv_all(fd, meta, 28)) {
				fprintf(stderr, "snapshot: connection lost\n");
				return 1;
			}
			size_t n = (size_t)get32(sh + 8) - 28;
			uint8_t *junk = malloc(n ? n : 1);
			if (recv_all(fd, junk, n))
				return 1;
			free(junk);
			double ms = now_ms() - t;
			if (get32(meta + 24) == 0) {
				snaps++; snap_sum += ms;
				if (ms < snap_min) snap_min = ms;
				if (ms > snap_max) snap_max = ms;
			} else {
				snap_failed++;
			}
			continue;
		}
		if (t >= next_overlay) {
			next_overlay += 1000.0 / 30;
			uint8_t *px = msg + 28;
			for (size_t i = 0; i < pixels; i += 4) {	/* premultiplied: no channel above alpha (0x90) */
				px[i] = (uint8_t)((i / 4 + (size_t)overlays * 7) & 0x7F);
				px[i + 1] = (uint8_t)(((i / 4) >> 3) & 0x7F);
				px[i + 2] = (uint8_t)((overlays * 3) & 0x7F);
				px[i + 3] = 0x90;
			}
			if (send_all(fd, msg, 12 + 16 + pixels))
				return 1;
			overlays++;
			continue;
		}
		usleep(1000);
	}
	printf("overlays sent: %ld (%.1f / s)\n", overlays, overlays / seconds);
	printf("snapshots: %ld ok, %ld failed", snaps, snap_failed);
	if (snaps)
		printf("; round trip min %.1f avg %.1f max %.1f ms", snap_min, snap_sum / snaps, snap_max);
	printf("\n");
	return 0;
}
