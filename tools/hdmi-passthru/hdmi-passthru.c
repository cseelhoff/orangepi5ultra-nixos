// hdmi-passthru: lowest-latency HDMI-in -> HDMI-out with a text overlay.
//
// Capture buffers from the rk_hdmirx V4L2 device are shown directly on a VOP2
// plane (zero copy, scaled by the display controller). The text is a separate
// ARGB plane on top, blended by the VOP2, so no pixel is touched by CPU or GPU.
//
// Each frame logs capture timestamp -> vblank at which it was latched.
// With rockchip_hdmirx.low_latency=1 the driver hands out a buffer when line 10
// of the frame has arrived, so the display can scan it out while it is still
// being written ("beam racing").
//
// Usage: hdmi-passthru [-t text] [-s seconds] [-n buffers] [-p phase_ms] [-L] [-f] [-q] [-l log.csv] [-R]
//                      [-S socket [-G group]]
//   -p  hold the output vblank this long after the capture timestamp (default 1.2;
//       negative disables the phase lock). Below ~0.7 ms frames start to miss.
//   -L  keep normal capture (frame handed out when complete, ~16 ms more latency).
//       By default low-latency hand-out is switched on while running, restored on exit.
//   -f  wait for the capture fence (frame complete) before scanout
//   -q  quiet: no per-second stats on stderr
//   -R  only repair a display left in a bad state (refresh range, plane order)
//   -S  serve the Natively edge daemon on this Unix socket (see novl.h): it draws the overlay,
//       which then covers the whole output instead of the stats box, and asks for screenshots.
//       The socket is mode 0660, group -G. At least 6 capture buffers are used.
#define _GNU_SOURCE
#include <cairo.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include "novl.h"

#define MAXBUF 8
#define die(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)

static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;
	do r = ioctl(fd, req, arg); while (r < 0 && errno == EINTR);
	return r;
}

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* ---------- DRM property helpers ---------- */
static uint32_t prop_id(int fd, uint32_t obj, uint32_t type, const char *name)
{
	drmModeObjectProperties *props = drmModeObjectGetProperties(fd, obj, type);
	uint32_t id = 0;
	for (uint32_t i = 0; props && i < props->count_props && !id; i++) {
		drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
		if (p && !strcmp(p->name, name))
			id = p->prop_id;
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
	return id;
}

static int prop_get(int fd, uint32_t obj, uint32_t type, const char *name, uint64_t *val, char *str, size_t strsz)
{
	drmModeObjectProperties *props = drmModeObjectGetProperties(fd, obj, type);
	int found = 0;
	for (uint32_t i = 0; props && i < props->count_props && !found; i++) {
		drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
		if (p && !strcmp(p->name, name)) {
			found = 1;
			*val = props->prop_values[i];
			if (str && (p->flags & DRM_MODE_PROP_BITMASK || p->flags & DRM_MODE_PROP_ENUM)) {
				str[0] = 0;
				for (int e = 0; e < p->count_enums; e++) {
					int match = (p->flags & DRM_MODE_PROP_BITMASK)
						? !!(*val & (1ull << p->enums[e].value))
						: *val == p->enums[e].value;
					if (match)
						snprintf(str, strsz, "%s", p->enums[e].name);
				}
			}
		}
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
	return found;
}

static void add(drmModeAtomicReq *req, int fd, uint32_t obj, uint32_t type, const char *name, uint64_t val)
{
	uint32_t id = prop_id(fd, obj, type, name);
	if (!id)
		die("object %u has no property %s", obj, name);
	drmModeAtomicAddProperty(req, obj, id, val);
}

static int plane_has_format(drmModePlane *p, uint32_t fmt)
{
	for (uint32_t i = 0; i < p->count_formats; i++)
		if (p->formats[i] == fmt)
			return 1;
	return 0;
}

/* ---------- overlay ---------- */
struct dumb { uint32_t handle, fb, w, h, pitch; uint8_t *map; size_t size; };

static void dumb_create(int fd, struct dumb *d, uint32_t w, uint32_t h)
{
	struct drm_mode_create_dumb c = { .width = w, .height = h, .bpp = 32 };
	if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c))
		die("create dumb: %s", strerror(errno));
	d->handle = c.handle; d->pitch = c.pitch; d->size = c.size; d->w = w; d->h = h;
	struct drm_mode_map_dumb m = { .handle = c.handle };
	if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m))
		die("map dumb: %s", strerror(errno));
	d->map = mmap(0, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, m.offset);
	if (d->map == MAP_FAILED)
		die("mmap dumb: %s", strerror(errno));
	uint32_t handles[4] = { c.handle }, pitches[4] = { c.pitch }, offsets[4] = { 0 };
	if (drmModeAddFB2(fd, w, h, DRM_FORMAT_ARGB8888, handles, pitches, offsets, &d->fb, 0))
		die("addfb overlay: %s", strerror(errno));
}

static void draw_text(struct dumb *d, const char *line1, const char *line2)
{
	cairo_surface_t *s = cairo_image_surface_create_for_data(d->map, CAIRO_FORMAT_ARGB32, d->w, d->h, d->pitch);
	cairo_t *cr = cairo_create(s);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_paint(cr);
	/* translucent rounded box */
	double r = 18, w = d->w, h = d->h;
	cairo_new_sub_path(cr);
	cairo_arc(cr, w - r, r, r, -1.5708, 0);
	cairo_arc(cr, w - r, h - r, r, 0, 1.5708);
	cairo_arc(cr, r, h - r, r, 1.5708, 3.14159);
	cairo_arc(cr, r, r, r, 3.14159, 4.71239);
	cairo_close_path(cr);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
	cairo_fill(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
	cairo_set_font_size(cr, 44);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.92);
	cairo_move_to(cr, 28, 58);
	cairo_show_text(cr, line1);
	cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, 26);
	cairo_set_source_rgba(cr, 1, 0.85, 0.3, 0.95);
	cairo_move_to(cr, 30, 102);
	cairo_show_text(cr, line2);
	cairo_destroy(cr);
	cairo_surface_flush(s);
	cairo_surface_destroy(s);
}

/* ---------- state ---------- */
struct cap { int dmafd; uint32_t fb; uint64_t ts; int fence; };
static struct cap bufs[MAXBUF];
static int vfd, dfd, nbuf = 4;
static int on_screen = -1, pending = -1, latest = -1;
static uint64_t pending_commit_ns;
static uint32_t crtc_id, vplane, oplane;
static struct dumb ov[2];
static int ov_cur, ov_dirty;
static struct novl *novl;	/* the edge daemon's server, with -S */
static int wait_fence;

/* phase lock: nudge the output frame length (VOP2 "variable refresh rate",
 * whole Hz -> vtotal) so each vblank lands target_ms after the capture timestamp */
static double target_ms = 1.2, period_ms = 16.667;
static int base_hz = 60, want_hz = 60, cur_hz = 60, nudges;
static FILE *logf;

/* stats for the current second */
static double lat_sum, lat_min = 1e9, lat_max, cmt_sum;
static int lat_n, dropped, total_shown, total_dropped;
static double all_lat[100000];
static int all_n;

static int qbuf_try(int i)
{
	struct v4l2_plane pl[VIDEO_MAX_PLANES] = { 0 };
	struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_MMAP,
				 .index = i, .m.planes = pl, .length = 1 };
	return xioctl(vfd, VIDIOC_QBUF, &b);
}

static void qbuf(int i)
{
	if (qbuf_try(i))
		die("QBUF: %s", strerror(errno));
}

/* a capture buffer a snapshot was reading; called from the novl thread, which must not exit() */
static void novl_requeue(void *ctx, int i)
{
	(void)ctx;
	qbuf_try(i);
}

static void commit(int i)
{
	drmModeAtomicReq *req = drmModeAtomicAlloc();
	add(req, dfd, vplane, DRM_MODE_OBJECT_PLANE, "FB_ID", bufs[i].fb);
	if (wait_fence && bufs[i].fence >= 0)
		add(req, dfd, vplane, DRM_MODE_OBJECT_PLANE, "IN_FENCE_FD", bufs[i].fence);
	if (novl) {
		int o = novl_overlay_next(novl);
		if (o >= 0)
			add(req, dfd, oplane, DRM_MODE_OBJECT_PLANE, "FB_ID", ov[o].fb);
	} else if (ov_dirty) {
		add(req, dfd, oplane, DRM_MODE_OBJECT_PLANE, "FB_ID", ov[ov_cur].fb);
		ov_dirty = 0;
	}
	if (want_hz != cur_hz) {
		add(req, dfd, crtc_id, DRM_MODE_OBJECT_CRTC, "variable refresh rate", want_hz);
		cur_hz = want_hz;
	}
	pending_commit_ns = now_ns();
	int r = drmModeAtomicCommit(dfd, req, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, (void *)(intptr_t)(i + 1));
	drmModeAtomicFree(req);
	if (r)
		die("flip commit: %s", strerror(errno));
	if (bufs[i].fence >= 0) {
		close(bufs[i].fence);
		bufs[i].fence = -1;
	}
	pending = i;
}

static void flip_done(int fd, unsigned int seq, unsigned int sec, unsigned int usec, void *data)
{
	(void)fd; (void)seq;
	int i = (int)(intptr_t)data - 1;
	uint64_t vbl = (uint64_t)sec * 1000000000ull + (uint64_t)usec * 1000;
	double lat = ((double)vbl - (double)bufs[i].ts) / 1e6;
	double cm = ((double)vbl - (double)pending_commit_ns) / 1e6;
	lat_sum += lat; cmt_sum += cm; lat_n++;
	if (lat < lat_min) lat_min = lat;
	if (lat > lat_max) lat_max = lat;
	if (all_n < (int)(sizeof all_lat / sizeof *all_lat))
		all_lat[all_n++] = lat;
	if (logf)
		fprintf(logf, "%llu,%llu,%.3f,%.3f\n", (unsigned long long)bufs[i].ts, (unsigned long long)vbl, lat,
			((double)pending_commit_ns - (double)bufs[i].ts) / 1e6);
	total_shown++;
	if (target_ms >= 0) {
		/* signed phase error, wrapped to (-P/2, P/2] */
		/* Control the phase (vblank - capture) mod period. If the target is
		 * shorter than the commit deadline, every frame just latches one
		 * vblank later; that is stable, only one frame slower. */
		double e = lat - target_ms;
		while (e > period_ms / 2) e -= period_ms;
		while (e <= -period_ms / 2) e += period_ms;
		int w = base_hz;
		if (e > 0.15) w = base_hz + 1;		/* vblank too late: shorter frame */
		else if (e < -0.15) w = base_hz - 1;	/* too early: longer frame */
		if (w != base_hz) nudges++;
		want_hz = w;
	}
	if (novl)
		novl_flip_done(novl);
	/* The buffer that was on screen has been scanned out for a whole frame, so it is complete.
	 * A snapshot may be waiting for exactly such a buffer; it requeues it when done. */
	if (on_screen >= 0 && on_screen != i && !(novl && novl_hold(novl, on_screen)))
		qbuf(on_screen);
	on_screen = i;
	pending = -1;
	/* A frame captured just before this vblank would now go out a whole
	 * frame late, and every following frame would queue behind it. With the
	 * phase lock on, drop it once so the next frame is committed as soon as
	 * it is captured (at most every 2 s, so an unreachable target only
	 * costs a frame now and then). */
	static uint64_t last_break;
	if (latest >= 0 && target_ms >= 0 && bufs[latest].ts < vbl &&
	    vbl - bufs[latest].ts < (uint64_t)(period_ms * 0.5e6) && vbl - last_break > 2000000000ull) {
		if (bufs[latest].fence >= 0) { close(bufs[latest].fence); bufs[latest].fence = -1; }
		qbuf(latest);
		latest = -1;
		dropped++; total_dropped++;
		last_break = vbl;
	}
	if (latest >= 0) {
		int n = latest;
		latest = -1;
		commit(n);
	}
}

static char *find_hdmirx(void)
{
	static char dev[64];
	glob_t g;
	if (glob("/sys/class/video4linux/*/name", 0, NULL, &g))
		die("no video devices");
	for (size_t i = 0; i < g.gl_pathc; i++) {
		char name[64] = { 0 };
		FILE *f = fopen(g.gl_pathv[i], "r");
		if (f && fgets(name, sizeof name, f) && strstr(name, "hdmirx")) {
			char *p = strrchr(g.gl_pathv[i], '/');
			*p = 0;
			snprintf(dev, sizeof dev, "/dev/%s", strrchr(g.gl_pathv[i], '/') + 1);
		}
		if (f) fclose(f);
	}
	globfree(&g);
	if (!dev[0])
		die("no hdmirx video device");
	return dev;
}

/* Undo what this tool changes on the CRTC: frame-length range, the forced
 * refresh rate and plane order. The console's fbdev restores its own plane
 * when we close the device. */
static int reset_only;
static uint64_t saved_vz, saved_oz;
static uint32_t saved_conn;
static drmModeModeInfo saved_blob_mode;

static void restore_display(void)
{
	static int done;
	if (done || !crtc_id)
		return;
	done = 1;
	uint32_t blob;
	drmModeCreatePropertyBlob(dfd, &saved_blob_mode, sizeof saved_blob_mode, &blob);
	drmModeAtomicReq *req = drmModeAtomicAlloc();
	add(req, dfd, saved_conn, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", crtc_id);
	add(req, dfd, crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID", blob);
	add(req, dfd, crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", 1);
	if (prop_id(dfd, crtc_id, DRM_MODE_OBJECT_CRTC, "variable refresh rate")) {
		add(req, dfd, crtc_id, DRM_MODE_OBJECT_CRTC, "variable refresh rate", 0);
		add(req, dfd, crtc_id, DRM_MODE_OBJECT_CRTC, "min refresh rate", 0);
		add(req, dfd, crtc_id, DRM_MODE_OBJECT_CRTC, "max refresh rate", 0);
	}
	add(req, dfd, vplane, DRM_MODE_OBJECT_PLANE, "FB_ID", 0);
	add(req, dfd, vplane, DRM_MODE_OBJECT_PLANE, "CRTC_ID", 0);
	add(req, dfd, vplane, DRM_MODE_OBJECT_PLANE, "zpos", saved_vz);
	add(req, dfd, oplane, DRM_MODE_OBJECT_PLANE, "FB_ID", 0);
	add(req, dfd, oplane, DRM_MODE_OBJECT_PLANE, "CRTC_ID", 0);
	add(req, dfd, oplane, DRM_MODE_OBJECT_PLANE, "zpos", saved_oz);
	if (drmModeAtomicCommit(dfd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL))
		fprintf(stderr, "restore commit failed: %s\n", strerror(errno));
	drmModeAtomicFree(req);
}

/* HDMI-in low-latency hand-out is a module parameter read when streaming
 * starts. Turn it on for this run and put the old value back on exit. */
#define LOW_LATENCY_PARAM "/sys/module/rockchip_hdmirx/parameters/low_latency"
static int keep_normal;
static char saved_ll = 0;

static void restore_low_latency(void)
{
	if (!saved_ll)
		return;
	int fd = open(LOW_LATENCY_PARAM, O_WRONLY);
	if (fd >= 0) {
		if (write(fd, &saved_ll, 1) != 1)
			fprintf(stderr, "could not restore %s\n", LOW_LATENCY_PARAM);
		close(fd);
	}
}

static void set_low_latency(int on)
{
	char cur = 0;
	int fd = open(LOW_LATENCY_PARAM, O_RDWR);
	if (fd < 0 || read(fd, &cur, 1) != 1) {
		fprintf(stderr, "note: cannot change %s (%s); using the current capture mode\n",
			LOW_LATENCY_PARAM, strerror(errno));
		if (fd >= 0) close(fd);
		return;
	}
	char want = on ? 'Y' : 'N';
	if (cur != want) {
		lseek(fd, 0, SEEK_SET);
		if (write(fd, &want, 1) == 1) {
			saved_ll = cur;
			atexit(restore_low_latency);
		}
	}
	close(fd);
	fprintf(stderr, "HDMI-in low-latency hand-out: %s\n", on ? "on" : "off");
}

static int cmp_d(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
	const char *text = "HDMI passthrough - Orange Pi 5 Ultra";
	const char *logpath = NULL, *sockpath = NULL, *sockgroup = NULL;
	int seconds = 0, quiet = 0, opt;
	while ((opt = getopt(argc, argv, "t:s:n:fql:p:RLS:G:")) != -1) {
		switch (opt) {
		case 'R': reset_only = 1; break;
		case 'p': target_ms = atof(optarg); break;
		case 'L': keep_normal = 1; break;
		case 't': text = optarg; break;
		case 's': seconds = atoi(optarg); break;
		case 'n': nbuf = atoi(optarg); break;
		case 'f': wait_fence = 1; break;
		case 'q': quiet = 1; break;
		case 'l': logpath = optarg; break;
		case 'S': sockpath = optarg; break;
		case 'G': sockgroup = optarg; break;
		default: die("usage: %s [-t text] [-s seconds] [-n buffers 2-%d] [-f] [-q] [-l log.csv] [-p phase_ms] [-L] [-R] [-S socket [-G group]]", argv[0], MAXBUF);
		}
	}
	if (nbuf < 2 || nbuf > MAXBUF)
		die("buffers must be 2..%d", MAXBUF);
	/* a snapshot keeps one capture buffer out of the queue while it copies the crop */
	if (sockpath && nbuf < 6)
		nbuf = 6;
	if (logpath && !(logf = fopen(logpath, "w")))
		die("open %s: %s", logpath, strerror(errno));
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);

	/* ---- capture ---- */
	if (!keep_normal && !reset_only)
		set_low_latency(1);
	const char *vdev = find_hdmirx();
	vfd = open(vdev, O_RDWR | O_NONBLOCK);
	if (vfd < 0)
		die("open %s: %s", vdev, strerror(errno));
	struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
	if (xioctl(vfd, VIDIOC_G_FMT, &fmt))
		die("G_FMT: %s", strerror(errno));
	uint32_t W = fmt.fmt.pix_mp.width, H = fmt.fmt.pix_mp.height;
	uint32_t stride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline, pix = fmt.fmt.pix_mp.pixelformat;
	uint32_t drmfmt;
	switch (pix) {
	case V4L2_PIX_FMT_NV12: drmfmt = DRM_FORMAT_NV12; break;
	case V4L2_PIX_FMT_NV16: drmfmt = DRM_FORMAT_NV16; break;
	case V4L2_PIX_FMT_NV24: drmfmt = DRM_FORMAT_NV24; break;
	default: die("unsupported capture format %.4s", (char *)&pix);
	}
	struct v4l2_dv_timings tim = { 0 };
	double in_hz = 0;
	if (!xioctl(vfd, VIDIOC_QUERY_DV_TIMINGS, &tim) && tim.bt.pixelclock) {
		double tot = (double)V4L2_DV_BT_FRAME_WIDTH(&tim.bt) * V4L2_DV_BT_FRAME_HEIGHT(&tim.bt);
		in_hz = tim.bt.pixelclock / tot;
	}
	fprintf(stderr, "capture %s: %ux%u %.4s stride %u, %.3f Hz\n", vdev, W, H, (char *)&pix, stride, in_hz);

	struct v4l2_requestbuffers rb = { .count = nbuf, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_MMAP };
	if (xioctl(vfd, VIDIOC_REQBUFS, &rb))
		die("REQBUFS: %s", strerror(errno));
	nbuf = rb.count;

	/* ---- display ---- */
	dfd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (dfd < 0)
		die("open card0: %s", strerror(errno));
	if (drmSetClientCap(dfd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) || drmSetClientCap(dfd, DRM_CLIENT_CAP_ATOMIC, 1))
		die("atomic not supported");
	drmModeRes *res = drmModeGetResources(dfd);
	drmModeConnector *conn = NULL;
	for (int i = 0; i < res->count_connectors && !conn; i++) {
		drmModeConnector *c = drmModeGetConnector(dfd, res->connectors[i]);
		if (c->connection == DRM_MODE_CONNECTED && c->connector_type == DRM_MODE_CONNECTOR_HDMIA && c->count_modes)
			conn = c;
		else
			drmModeFreeConnector(c);
	}
	if (!conn)
		die("no connected HDMI output");
	drmModeModeInfo mode = conn->modes[0];
	for (int i = 0; i < conn->count_modes; i++)
		if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) { mode = conn->modes[i]; break; }
	int crtc_idx = -1;
	for (int e = 0; e < conn->count_encoders && crtc_idx < 0; e++) {
		drmModeEncoder *enc = drmModeGetEncoder(dfd, conn->encoders[e]);
		for (int c = 0; c < res->count_crtcs; c++)
			if (enc->possible_crtcs & (1u << c)) { crtc_idx = c; break; }
		drmModeFreeEncoder(enc);
	}
	if (crtc_idx < 0)
		die("no crtc for connector");
	crtc_id = res->crtcs[crtc_idx];
	double out_hz = mode.clock * 1000.0 / (mode.htotal * mode.vtotal);
	fprintf(stderr, "display: connector %u crtc %u mode %s %.3f Hz\n", conn->connector_id, crtc_id, mode.name, out_hz);

	/* pick planes: video = a plane that takes the capture format (prefer non-primary),
	 * overlay = a different plane that takes ARGB8888 */
	drmModePlaneRes *pr = drmModeGetPlaneResources(dfd);
	uint32_t primary = 0;
	for (uint32_t i = 0; i < pr->count_planes; i++) {
		drmModePlane *p = drmModeGetPlane(dfd, pr->planes[i]);
		uint64_t type = 0, nameval = 0;
		char name[64] = "?";
		prop_get(dfd, p->plane_id, DRM_MODE_OBJECT_PLANE, "type", &type, NULL, 0);
		prop_get(dfd, p->plane_id, DRM_MODE_OBJECT_PLANE, "NAME", &nameval, name, sizeof name);
		int usable = p->possible_crtcs & (1u << crtc_idx);
		if (usable && type == DRM_PLANE_TYPE_PRIMARY && !primary)
			primary = p->plane_id;
		if (usable && !vplane && plane_has_format(p, drmfmt) && type != DRM_PLANE_TYPE_PRIMARY)
			vplane = p->plane_id;
		if (getenv("PASSTHRU_VERBOSE"))
			fprintf(stderr, "plane %u %s type %llu crtcs %#x nv12 %d argb %d\n", p->plane_id, name,
				(unsigned long long)type, p->possible_crtcs, plane_has_format(p, drmfmt),
				plane_has_format(p, DRM_FORMAT_ARGB8888));
		drmModeFreePlane(p);
	}
	if (!vplane)
		die("no plane for the capture format");
	/* the overlay goes on the primary plane if it can, else any other ARGB plane */
	for (uint32_t i = 0; i < pr->count_planes && !oplane; i++) {
		drmModePlane *p = drmModeGetPlane(dfd, pr->planes[i]);
		if (p->plane_id == primary && plane_has_format(p, DRM_FORMAT_ARGB8888))
			oplane = p->plane_id;
		drmModeFreePlane(p);
	}
	for (uint32_t i = 0; i < pr->count_planes && !oplane; i++) {
		drmModePlane *p = drmModeGetPlane(dfd, pr->planes[i]);
		if (p->plane_id != vplane && (p->possible_crtcs & (1u << crtc_idx)) && plane_has_format(p, DRM_FORMAT_ARGB8888))
			oplane = p->plane_id;
		drmModeFreePlane(p);
	}
	if (!oplane)
		die("no overlay plane");
	fprintf(stderr, "video plane %u, overlay plane %u\n", vplane, oplane);
	uint64_t vz = 0, oz = 0;
	prop_get(dfd, vplane, DRM_MODE_OBJECT_PLANE, "zpos", &vz, NULL, 0);
	prop_get(dfd, oplane, DRM_MODE_OBJECT_PLANE, "zpos", &oz, NULL, 0);
	if (reset_only) {
		/* repair a display left behind by an older build: primary back to the bottom */
		vz = 1;
		oz = 0;
	}
	saved_vz = vz; saved_oz = oz; saved_conn = conn->connector_id;
	saved_blob_mode = mode;
	if (reset_only) {
		restore_display();
		return 0;
	}
	atexit(restore_display);

	/* capture buffers -> dmabuf -> DRM framebuffers */
	for (int i = 0; i < nbuf; i++) {
		struct v4l2_exportbuffer eb = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .index = i, .plane = 0, .flags = O_RDWR | O_CLOEXEC };
		if (xioctl(vfd, VIDIOC_EXPBUF, &eb))
			die("EXPBUF: %s", strerror(errno));
		bufs[i].dmafd = eb.fd;
		bufs[i].fence = -1;
		uint32_t h;
		if (drmPrimeFDToHandle(dfd, eb.fd, &h))
			die("prime import: %s", strerror(errno));
		uint32_t cpp_uv = drmfmt == DRM_FORMAT_NV24 ? 2 : 1;
		uint32_t handles[4] = { h, h }, pitches[4] = { stride, stride * cpp_uv }, offsets[4] = { 0, stride * H };
		if (drmModeAddFB2(dfd, W, H, drmfmt, handles, pitches, offsets, &bufs[i].fb, 0))
			die("addfb capture: %s", strerror(errno));
	}

	/* overlay: two buffers so updates never tear. With -S the daemon draws it, so it covers the
	 * whole output; otherwise it is the stats box. */
	uint32_t ow = 1100, oh = 124;
	if (sockpath) {
		ow = mode.hdisplay;
		oh = mode.vdisplay;
	} else if (ow > mode.hdisplay - 64) {
		ow = mode.hdisplay - 64;
	}
	dumb_create(dfd, &ov[0], ow, oh);
	dumb_create(dfd, &ov[1], ow, oh);
	if (sockpath) {
		memset(ov[0].map, 0, ov[0].size);
		memset(ov[1].map, 0, ov[1].size);
	} else {
		draw_text(&ov[0], text, "starting...");
	}

	/* video scaled to fit the screen, keeping aspect */
	uint32_t dw = mode.hdisplay, dh = (uint64_t)mode.hdisplay * H / W;
	if (dh > mode.vdisplay) { dh = mode.vdisplay; dw = (uint64_t)mode.vdisplay * W / H; }
	uint32_t dx = (mode.hdisplay - dw) / 2, dy = (mode.vdisplay - dh) / 2;

	for (int i = 0; i < nbuf; i++)
		qbuf(i);
	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (xioctl(vfd, VIDIOC_STREAMON, &type))
		die("STREAMON: %s", strerror(errno));

	/* wait for the first frame, then modeset with both planes */
	struct v4l2_plane pl[VIDEO_MAX_PLANES];
	struct v4l2_buffer b;
	for (;;) {
		struct pollfd pf = { vfd, POLLIN, 0 };
		if (poll(&pf, 1, 3000) <= 0)
			die("no frames from %s", vdev);
		memset(&b, 0, sizeof b); memset(pl, 0, sizeof pl);
		b.type = type; b.memory = V4L2_MEMORY_MMAP; b.m.planes = pl; b.length = VIDEO_MAX_PLANES;
		if (!xioctl(vfd, VIDIOC_DQBUF, &b))
			break;
	}
	uint32_t blob;
	drmModeCreatePropertyBlob(dfd, &mode, sizeof mode, &blob);
	drmModeAtomicReq *req = drmModeAtomicAlloc();
	add(req, dfd, conn->connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", crtc_id);
	add(req, dfd, crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID", blob);
	add(req, dfd, crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", 1);
	base_hz = want_hz = cur_hz = (int)(out_hz + 0.5);
	period_ms = 1000.0 / out_hz;
	if (target_ms >= 0) {
		add(req, dfd, crtc_id, DRM_MODE_OBJECT_CRTC, "min refresh rate", base_hz - 1);
		add(req, dfd, crtc_id, DRM_MODE_OBJECT_CRTC, "max refresh rate", base_hz + 1);
		add(req, dfd, crtc_id, DRM_MODE_OBJECT_CRTC, "variable refresh rate", base_hz);
	} else if (prop_id(dfd, crtc_id, DRM_MODE_OBJECT_CRTC, "min refresh rate")) {
		add(req, dfd, crtc_id, DRM_MODE_OBJECT_CRTC, "min refresh rate", 0);
		add(req, dfd, crtc_id, DRM_MODE_OBJECT_CRTC, "max refresh rate", 0);
	}
	/* turn off every other plane on this crtc (e.g. the console) */
	for (uint32_t i = 0; i < pr->count_planes; i++) {
		drmModePlane *p = drmModeGetPlane(dfd, pr->planes[i]);
		if (p->crtc_id == crtc_id && p->plane_id != vplane && p->plane_id != oplane) {
			add(req, dfd, p->plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", 0);
			add(req, dfd, p->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", 0);
		}
		drmModeFreePlane(p);
	}
	struct { uint32_t id, fb, sx, sy, sw, sh, cx, cy, cw, ch; uint64_t z; } pls[2] = {
		{ vplane, bufs[b.index].fb, 0, 0, W, H, dx, dy, dw, dh, 0 },
		{ oplane, ov[0].fb, 0, 0, ow, oh, sockpath ? 0 : 32, sockpath ? 0 : mode.vdisplay - oh - 32, ow, oh, 1 },
	};
	for (int i = 0; i < 2; i++) {
		add(req, dfd, pls[i].id, DRM_MODE_OBJECT_PLANE, "FB_ID", pls[i].fb);
		add(req, dfd, pls[i].id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", crtc_id);
		add(req, dfd, pls[i].id, DRM_MODE_OBJECT_PLANE, "SRC_X", 0);
		add(req, dfd, pls[i].id, DRM_MODE_OBJECT_PLANE, "SRC_Y", 0);
		add(req, dfd, pls[i].id, DRM_MODE_OBJECT_PLANE, "SRC_W", (uint64_t)pls[i].sw << 16);
		add(req, dfd, pls[i].id, DRM_MODE_OBJECT_PLANE, "SRC_H", (uint64_t)pls[i].sh << 16);
		add(req, dfd, pls[i].id, DRM_MODE_OBJECT_PLANE, "CRTC_X", pls[i].cx);
		add(req, dfd, pls[i].id, DRM_MODE_OBJECT_PLANE, "CRTC_Y", pls[i].cy);
		add(req, dfd, pls[i].id, DRM_MODE_OBJECT_PLANE, "CRTC_W", pls[i].cw);
		add(req, dfd, pls[i].id, DRM_MODE_OBJECT_PLANE, "CRTC_H", pls[i].ch);
		if (prop_id(dfd, pls[i].id, DRM_MODE_OBJECT_PLANE, "zpos"))
			add(req, dfd, pls[i].id, DRM_MODE_OBJECT_PLANE, "zpos", pls[i].z);
	}
	if (drmModeAtomicCommit(dfd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL))
		die("modeset commit: %s", strerror(errno));
	drmModeAtomicFree(req);
	on_screen = b.index;

	if (sockpath) {
		/* Colour matrix and range as the receiver reports them; HD with nothing said is BT.709 limited. */
		uint32_t cs = fmt.fmt.pix_mp.colorspace, enc = fmt.fmt.pix_mp.ycbcr_enc, q = fmt.fmt.pix_mp.quantization;
		struct novl_source ns = {
			.fmt = drmfmt == DRM_FORMAT_NV12 ? NOVL_NV12 : drmfmt == DRM_FORMAT_NV16 ? NOVL_NV16 : NOVL_NV24,
			.width = W, .height = H, .stride = stride,
			.size = fmt.fmt.pix_mp.plane_fmt[0].sizeimage,
			.bt601 = enc == V4L2_YCBCR_ENC_601 || enc == V4L2_YCBCR_ENC_XV601 ||
				 (enc == V4L2_YCBCR_ENC_DEFAULT && (cs == V4L2_COLORSPACE_SMPTE170M ||
				  cs == V4L2_COLORSPACE_470_SYSTEM_M || cs == V4L2_COLORSPACE_470_SYSTEM_BG ||
				  (cs == V4L2_COLORSPACE_DEFAULT && H < 720))),
			.full_range = q == V4L2_QUANTIZATION_FULL_RANGE ||
				      (q == V4L2_QUANTIZATION_DEFAULT && cs == V4L2_COLORSPACE_JPEG),
			.nbuf = nbuf, .requeue = novl_requeue,
		};
		for (int i = 0; i < nbuf; i++)
			ns.dmafd[i] = bufs[i].dmafd;
		struct novl_overlay no = { .width = ow, .height = oh, .pitch = ov[0].pitch, .map = { ov[0].map, ov[1].map } };
		novl = novl_start(sockpath, sockgroup, &ns, &no);
		if (!novl)
			die("cannot serve %s: %s", sockpath, strerror(errno));
		fprintf(stderr, "edge daemon socket %s (overlay %ux%u, %s %s)\n", sockpath, ow, oh,
			ns.bt601 ? "BT.601" : "BT.709", ns.full_range ? "full" : "limited");
	}
	drmEventContext ev = { .version = 2, .page_flip_handler = flip_done };
	uint64_t t0 = now_ns(), tsec = t0;
	char line2[160];
	while (!stop && (!seconds || now_ns() - t0 < (uint64_t)seconds * 1000000000ull)) {
		struct pollfd pf[2] = { { vfd, POLLIN, 0 }, { dfd, POLLIN, 0 } };
		if (poll(pf, 2, 1000) < 0 && errno != EINTR)
			die("poll: %s", strerror(errno));
		if (pf[1].revents & POLLIN)
			drmHandleEvent(dfd, &ev);
		if (pf[0].revents & POLLIN) {
			for (;;) {
				memset(&b, 0, sizeof b); memset(pl, 0, sizeof pl);
				b.type = type; b.memory = V4L2_MEMORY_MMAP; b.m.planes = pl; b.length = VIDEO_MAX_PLANES;
				if (xioctl(vfd, VIDIOC_DQBUF, &b))
					break;
				int i = b.index;
				bufs[i].ts = (uint64_t)b.timestamp.tv_sec * 1000000000ull + (uint64_t)b.timestamp.tv_usec * 1000;
				if (novl)
					novl_frame(novl);
				uint32_t fence;
				memcpy(&fence, b.timecode.userbits, 4);
				if (fence && fence != 0xffffffffu)
					bufs[i].fence = (int)fence;
				if (pending < 0) {
					commit(i);
				} else {
					/* a flip is already queued: keep only the newest frame */
					if (latest >= 0) {
						if (bufs[latest].fence >= 0) { close(bufs[latest].fence); bufs[latest].fence = -1; }
						qbuf(latest);
						dropped++; total_dropped++;
					}
					latest = i;
				}
			}
		}
		uint64_t t = now_ns();
		if (t - tsec >= 1000000000ull) {
			double avg = lat_n ? lat_sum / lat_n : 0;
			snprintf(line2, sizeof line2, "capture->scanout %.1f ms (min %.1f max %.1f)  %d fps  drop %d",
				 avg, lat_n ? lat_min : 0, lat_max, lat_n, dropped);
			if (!quiet)
				fprintf(stderr, "%s  [commit->vblank %.2f ms, nudged frames %d]\n", line2,
					lat_n ? cmt_sum / lat_n : 0, nudges);
			nudges = 0;
			if (!novl) {	/* with -S the overlay is the daemon's */
				int nxt = !ov_cur;
				draw_text(&ov[nxt], text, line2);
				ov_cur = nxt; ov_dirty = 1;
			}
			lat_sum = cmt_sum = 0; lat_n = dropped = 0; lat_min = 1e9; lat_max = 0;
			tsec = t;
		}
	}

	novl_stop(novl);	/* before STREAMOFF: it requeues buffers */
	xioctl(vfd, VIDIOC_STREAMOFF, &type);
	if (all_n) {
		qsort(all_lat, all_n, sizeof *all_lat, cmp_d);
		double s = 0;
		for (int i = 0; i < all_n; i++) s += all_lat[i];
		printf("frames shown %d, dropped %d\n", total_shown, total_dropped);
		printf("capture timestamp -> vblank latch (ms): mean %.2f  p50 %.2f  p95 %.2f  p99 %.2f  max %.2f\n",
		       s / all_n, all_lat[all_n / 2], all_lat[all_n * 95 / 100], all_lat[all_n * 99 / 100], all_lat[all_n - 1]);
	}
	if (logf)
		fclose(logf);
	return 0;
}
