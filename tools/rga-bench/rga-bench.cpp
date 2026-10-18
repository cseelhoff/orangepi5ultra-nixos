// rga-bench: throughput of the RGA 2D engine through librga (im2d) on dma-buf
// buffers, for the conversions a capture/display pipeline needs.
// Usage: rga-bench [loops] [heap]   (heap: system-uncached (default), system, cma)
#include <cstddef> // the im2d headers use NULL without including it
#include <rga/im2d.hpp>
#include <rga/RgaUtils.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int heap_alloc(const char *heap, size_t size)
{
	char path[64];
	snprintf(path, sizeof path, "/dev/dma_heap/%s", heap);
	int h = open(path, O_RDWR | O_CLOEXEC);
	if (h < 0) { perror(path); exit(1); }
	struct dma_heap_allocation_data d = {};
	d.len = size;
	d.fd_flags = O_RDWR | O_CLOEXEC;
	if (ioctl(h, DMA_HEAP_IOCTL_ALLOC, &d)) { perror("DMA_HEAP_IOCTL_ALLOC"); exit(1); }
	close(h);
	return d.fd;
}

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

struct Case { const char *name; int sw, sh, sf, dw, dh, df; };

int main(int argc, char **argv)
{
	int loops = argc > 1 ? atoi(argv[1]) : 200;
	const char *heap = argc > 2 ? argv[2] : "system-uncached";
	printf("librga %s, heap %s\n", querystring(RGA_VERSION), heap);
	printf("%s\n", querystring(RGA_ALL));

	Case cases[] = {
		{ "4K NV12 -> 1080p NV12 (scale)", 3840, 2160, RK_FORMAT_YCbCr_420_SP, 1920, 1080, RK_FORMAT_YCbCr_420_SP },
		{ "4K NV12 -> 4K RGBA (convert)", 3840, 2160, RK_FORMAT_YCbCr_420_SP, 3840, 2160, RK_FORMAT_RGBA_8888 },
		{ "4K NV12 -> 1080p RGBA (scale+convert)", 3840, 2160, RK_FORMAT_YCbCr_420_SP, 1920, 1080, RK_FORMAT_RGBA_8888 },
		{ "1080p NV12 -> 4K NV12 (upscale)", 1920, 1080, RK_FORMAT_YCbCr_420_SP, 3840, 2160, RK_FORMAT_YCbCr_420_SP },
	};
	for (auto &c : cases) {
		size_t ss = c.sw * c.sh * get_bpp_from_format(c.sf), ds = c.dw * c.dh * get_bpp_from_format(c.df);
		int sfd = heap_alloc(heap, ss), dfd = heap_alloc(heap, ds);
		void *sp = mmap(NULL, ss, PROT_READ | PROT_WRITE, MAP_SHARED, sfd, 0);
		memset(sp, 0x80, ss);
		munmap(sp, ss);
		rga_buffer_handle_t sh = importbuffer_fd(sfd, ss), dh = importbuffer_fd(dfd, ds);
		rga_buffer_t src = wrapbuffer_handle(sh, c.sw, c.sh, c.sf);
		rga_buffer_t dst = wrapbuffer_handle(dh, c.dw, c.dh, c.df);
		IM_STATUS st = imcheck(src, dst, {}, {});
		if (st != IM_STATUS_NOERROR) {
			printf("%-40s imcheck: %s\n", c.name, imStrError(st));
			continue;
		}
		improcess(src, dst, {}, {}, {}, {}, IM_SYNC); /* warm up */
		double t0 = now();
		for (int i = 0; i < loops; i++) {
			st = improcess(src, dst, {}, {}, {}, {}, IM_SYNC);
			if (st != IM_STATUS_SUCCESS) { printf("%s: %s\n", c.name, imStrError(st)); break; }
		}
		double dt = now() - t0;
		printf("%-40s %7.1f fps  %5.2f ms/frame  %6.0f Mpix/s out\n", c.name, loops / dt,
		       dt / loops * 1e3, (double)c.dw * c.dh * loops / dt / 1e6);
		releasebuffer_handle(sh);
		releasebuffer_handle(dh);
		close(sfd);
		close(dfd);
	}
	return 0;
}
