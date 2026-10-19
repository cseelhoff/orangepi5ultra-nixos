// Hardware JPEG snapshots of capture buffers for hdmi-passthru.
//
// Runs on its own thread so the display path never waits: the passthrough
// hands over a buffer that is fully captured, this thread (optionally) scales
// it with RGA, encodes it with the VPU's JPEG encoder (Rockchip MPP) and gives
// the buffer back through a callback. No CPU pixel work.
#include <cstddef>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <rga/im2d.hpp>
#include <rga/RgaUtils.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/mpp_meta.h>
#include <rockchip/rk_venc_cfg.h>
#include "snapshot.h"

#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))

static struct {
	/* config */
	int in_w, in_h, in_stride;
	int out_w, out_h;
	char dir[512];
	int keep_all;
	/* MPP */
	MppCtx ctx;
	MppApi *mpi;
	MppBufferGroup grp;
	MppBuffer pkt_buf;
	/* RGA scale target */
	int scaled_fd;
	size_t scaled_size;
	/* hand-over slot */
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	int pending_fd;
	size_t pending_size;
	int pending_index;
	snap_done_fn done;
	void *done_arg;
	int busy;
	int quit;
	/* stats */
	unsigned count;
	double last_ms;
} S;

static double now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static int heap_alloc(size_t size)
{
	int h = open("/dev/dma_heap/system-uncached", O_RDWR | O_CLOEXEC);
	if (h < 0)
		h = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
	if (h < 0)
		return -1;
	struct dma_heap_allocation_data d = {};
	d.len = size;
	d.fd_flags = O_RDWR | O_CLOEXEC;
	int r = ioctl(h, DMA_HEAP_IOCTL_ALLOC, &d);
	close(h);
	return r ? -1 : (int)d.fd;
}

static int write_file(const char *path, const void *data, size_t len)
{
	char tmp[600];
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	FILE *f = fopen(tmp, "wb");
	if (!f)
		return -1;
	size_t n = fwrite(data, 1, len, f);
	fclose(f);
	if (n != len)
		return -1;
	return rename(tmp, path); /* readers never see a half-written file */
}

static int encode(int fd, size_t size)
{
	int w = S.in_w, h = S.in_h, hstride = S.in_stride, vstride = S.in_h;

	/* optional RGA downscale into our own buffer */
	if (S.out_w && S.out_w != S.in_w) {
		rga_buffer_handle_t sh = importbuffer_fd(fd, size);
		rga_buffer_handle_t dh = importbuffer_fd(S.scaled_fd, S.scaled_size);
		rga_buffer_t src = wrapbuffer_handle(sh, S.in_w, S.in_h, RK_FORMAT_YCbCr_420_SP, S.in_stride, S.in_h);
		rga_buffer_t dst = wrapbuffer_handle(dh, S.out_w, S.out_h, RK_FORMAT_YCbCr_420_SP,
						     ALIGN(S.out_w, 16), ALIGN(S.out_h, 16));
		IM_STATUS st = imresize(src, dst);
		releasebuffer_handle(sh);
		releasebuffer_handle(dh);
		if (st != IM_STATUS_SUCCESS) {
			fprintf(stderr, "snapshot: RGA scale failed: %s\n", imStrError(st));
			return -1;
		}
		fd = S.scaled_fd;
		size = S.scaled_size;
		w = S.out_w;
		h = S.out_h;
		hstride = ALIGN(S.out_w, 16);
		vstride = ALIGN(S.out_h, 16);
	}

	MppBuffer in = NULL;
	MppBufferInfo info;
	memset(&info, 0, sizeof info);
	info.type = MPP_BUFFER_TYPE_EXT_DMA;
	info.fd = fd;
	info.size = size;
	if (mpp_buffer_import(&in, &info)) {
		fprintf(stderr, "snapshot: mpp_buffer_import failed\n");
		return -1;
	}

	MppFrame frame = NULL;
	mpp_frame_init(&frame);
	mpp_frame_set_width(frame, w);
	mpp_frame_set_height(frame, h);
	mpp_frame_set_hor_stride(frame, hstride);
	mpp_frame_set_ver_stride(frame, vstride);
	mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
	mpp_frame_set_buffer(frame, in);
	mpp_frame_set_eos(frame, 0);

	MppPacket pkt = NULL;
	mpp_packet_init_with_buffer(&pkt, S.pkt_buf);
	mpp_packet_set_length(pkt, 0);
	mpp_meta_set_packet(mpp_frame_get_meta(frame), KEY_OUTPUT_PACKET, pkt);

	int ret = -1;
	if (S.mpi->encode_put_frame(S.ctx, frame) == MPP_OK) {
		MppPacket out = NULL;
		if (S.mpi->encode_get_packet(S.ctx, &out) == MPP_OK && out) {
			const void *data = mpp_packet_get_pos(out);
			size_t len = mpp_packet_get_length(out);
			char path[600];
			snprintf(path, sizeof path, "%s/latest.jpg", S.dir);
			ret = write_file(path, data, len);
			if (!ret && S.keep_all) {
				struct timeval tv;
				gettimeofday(&tv, NULL);
				struct tm tm;
				localtime_r(&tv.tv_sec, &tm);
				char ts[64];
				strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &tm);
				snprintf(path, sizeof path, "%s/snap-%s.%03ld.jpg", S.dir, ts, (long)tv.tv_usec / 1000);
				ret = write_file(path, data, len);
			}
			if (out != pkt)
				mpp_packet_deinit(&out);
		}
	}
	mpp_packet_deinit(&pkt);
	mpp_frame_deinit(&frame);
	mpp_buffer_put(in);
	return ret;
}

static void *worker(void *)
{
	pthread_mutex_lock(&S.lock);
	for (;;) {
		while (S.pending_fd < 0 && !S.quit)
			pthread_cond_wait(&S.cond, &S.lock);
		if (S.quit)
			break;
		int fd = S.pending_fd, idx = S.pending_index;
		size_t size = S.pending_size;
		S.pending_fd = -1;
		pthread_mutex_unlock(&S.lock);

		double t0 = now_ms();
		int r = encode(fd, size);
		double dt = now_ms() - t0;
		if (S.done)
			S.done(idx, S.done_arg); /* buffer goes back to the capture queue */

		pthread_mutex_lock(&S.lock);
		if (!r) {
			S.count++;
			S.last_ms = dt;
		}
		S.busy = 0;
	}
	pthread_mutex_unlock(&S.lock);
	return NULL;
}

extern "C" int snap_init(int in_w, int in_h, int in_stride, int out_w, int quality,
			 const char *dir, int keep_all, snap_done_fn done, void *arg)
{
	memset(&S, 0, sizeof S);
	S.in_w = in_w;
	S.in_h = in_h;
	S.in_stride = in_stride;
	S.out_w = out_w > 0 && out_w < in_w ? ALIGN(out_w, 2) : 0;
	S.out_h = S.out_w ? ALIGN(in_h * S.out_w / in_w, 2) : 0;
	snprintf(S.dir, sizeof S.dir, "%s", dir);
	S.keep_all = keep_all;
	S.done = done;
	S.done_arg = arg;
	S.pending_fd = -1;
	S.scaled_fd = -1;
	pthread_mutex_init(&S.lock, NULL);
	pthread_cond_init(&S.cond, NULL);

	if (S.out_w) {
		S.scaled_size = (size_t)ALIGN(S.out_w, 16) * ALIGN(S.out_h, 16) * 3 / 2;
		S.scaled_fd = heap_alloc(S.scaled_size);
		if (S.scaled_fd < 0) {
			fprintf(stderr, "snapshot: cannot allocate scale buffer\n");
			return -1;
		}
	}

	if (mpp_create(&S.ctx, &S.mpi) || mpp_init(S.ctx, MPP_CTX_ENC, MPP_VIDEO_CodingMJPEG)) {
		fprintf(stderr, "snapshot: MPP JPEG encoder unavailable\n");
		return -1;
	}
	MppEncCfg cfg;
	mpp_enc_cfg_init(&cfg);
	S.mpi->control(S.ctx, MPP_ENC_GET_CFG, cfg);
	int w = S.out_w ? S.out_w : in_w, h = S.out_w ? S.out_h : in_h;
	mpp_enc_cfg_set_s32(cfg, "prep:width", w);
	mpp_enc_cfg_set_s32(cfg, "prep:height", h);
	mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", S.out_w ? ALIGN(S.out_w, 16) : in_stride);
	mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", S.out_w ? ALIGN(S.out_h, 16) : in_h);
	mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV420SP);
	mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_FIXQP);
	mpp_enc_cfg_set_s32(cfg, "jpeg:q_factor", quality);
	mpp_enc_cfg_set_s32(cfg, "jpeg:qf_max", 99);
	mpp_enc_cfg_set_s32(cfg, "jpeg:qf_min", 1);
	if (S.mpi->control(S.ctx, MPP_ENC_SET_CFG, cfg)) {
		fprintf(stderr, "snapshot: MPP JPEG config rejected\n");
		return -1;
	}
	mpp_enc_cfg_deinit(cfg);

	mpp_buffer_group_get_internal(&S.grp, MPP_BUFFER_TYPE_DRM);
	mpp_buffer_get(S.grp, &S.pkt_buf, (size_t)w * h * 2); /* worst-case JPEG */

	if (pthread_create(&S.thread, NULL, worker, NULL))
		return -1;
	fprintf(stderr, "snapshots: %dx%d JPEG q%d -> %s/latest.jpg%s\n", w, h, quality, S.dir,
		keep_all ? " (+ timestamped copies)" : "");
	return 0;
}

/* Hand over a fully captured buffer. Returns 1 if taken (done() will be
 * called later), 0 if the encoder is still busy (caller keeps the buffer). */
extern "C" int snap_submit(int index, int dmafd, size_t size)
{
	int taken = 0;
	pthread_mutex_lock(&S.lock);
	if (!S.busy && S.ctx) {
		S.busy = 1;
		S.pending_fd = dmafd;
		S.pending_size = size;
		S.pending_index = index;
		pthread_cond_signal(&S.cond);
		taken = 1;
	}
	pthread_mutex_unlock(&S.lock);
	return taken;
}

extern "C" void snap_stats(unsigned *count, double *last_ms)
{
	pthread_mutex_lock(&S.lock);
	*count = S.count;
	*last_ms = S.last_ms;
	pthread_mutex_unlock(&S.lock);
}

extern "C" void snap_close(void)
{
	if (!S.ctx)
		return;
	pthread_mutex_lock(&S.lock);
	S.quit = 1;
	pthread_cond_signal(&S.cond);
	pthread_mutex_unlock(&S.lock);
	pthread_join(S.thread, NULL);
	if (S.pkt_buf)
		mpp_buffer_put(S.pkt_buf);
	if (S.grp)
		mpp_buffer_group_put(S.grp);
	mpp_destroy(S.ctx);
	S.ctx = NULL;
	if (S.scaled_fd >= 0)
		close(S.scaled_fd);
}
