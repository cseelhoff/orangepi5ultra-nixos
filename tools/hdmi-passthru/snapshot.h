/* Hardware JPEG snapshots (RGA scale + MPP JPEG) on a worker thread. */
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Called on the worker thread when buffer `index` may be reused. */
typedef void (*snap_done_fn)(int index, void *arg);

/* rgb: the input is packed B,G,R (in_stride in bytes), converted to NV12 by the RGA; else NV12.
 * out_w: scale to this width first (0 = native). quality: JPEG 1..99.
 * dir != NULL: writes <dir>/latest.jpg atomically, plus snap-<time>.jpg if keep_all (snap_submit). */
int snap_init(int in_w, int in_h, int in_stride, int rgb, int out_w, int quality,
	      const char *dir, int keep_all, snap_done_fn done, void *arg);
/* 1 = taken (done() follows), 0 = encoder busy, keep the buffer. */
int snap_submit(int index, int dmafd, size_t size);
/* Encodes now on the calling thread; *jpeg is a malloc'd block to free(). 0 = ok. */
int snap_grab(int dmafd, size_t size, void **jpeg, size_t *len);
void snap_stats(unsigned *count, double *last_ms);
void snap_close(void);

#ifdef __cplusplus
}
#endif
