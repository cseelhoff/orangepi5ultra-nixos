/* novl: the display process's end of the Natively edge protocol.
 *
 * Over one Unix socket the edge daemon (natively-edge) pushes the picture to draw over the
 * video IN, and asks for screenshot crops and the picture size OUT. The wire format is
 * docs/edge-display.md in the natively-linux repo (messages OVERLAY, SNAPSHOT, INFO).
 *
 * A server thread owns the socket, the overlay blit and the YUV->RGB conversion. The display
 * loop only calls the hooks below: none of them waits on a client or copies pixels, so the
 * latency path stays as it was.
 *
 * Overlay: two ARGB8888 buffers the size of the output. The thread draws the next overlay
 * into the one that is not on screen; the loop puts it on the plane at its next commit.
 * Snapshot: the loop hands over the capture buffer it is about to requeue, which has been
 * on screen for a whole frame and so is complete. It is returned when the crop is copied.
 */
#ifndef NOVL_H
#define NOVL_H

#include <stddef.h>
#include <stdint.h>

#define NOVL_MAXBUF 8

enum novl_fmt { NOVL_NV12, NOVL_NV16, NOVL_NV24 };

struct novl_source {
	enum novl_fmt fmt;
	uint32_t width, height;
	uint32_t stride;	/* bytes per luma row; the chroma plane follows at stride * height */
	size_t size;		/* bytes in one capture buffer */
	int bt601, full_range;	/* colour matrix (else BT.709) and range (else limited) */
	int nbuf;
	int dmafd[NOVL_MAXBUF];	/* the capture buffers as dma-bufs (mmap-able) */
	/* gives a held capture buffer back to the V4L2 queue; called from the server thread */
	void (*requeue)(void *ctx, int index);
	void *ctx;
};

struct novl_overlay {
	uint32_t width, height, pitch;
	uint8_t *map[2];	/* the two ARGB8888 dumb buffers, mapped */
};

struct novl;

/* Listens on `path` (mode 0660, group `group` if not NULL) and starts the server thread.
 * NULL with errno set on failure (a message has been printed). */
struct novl *novl_start(const char *path, const char *group, const struct novl_source *src,
			const struct novl_overlay *ov);
void novl_stop(struct novl *n);

/* ---- hooks for the display loop; wait-free (a few atomic operations, no locks), so they can
 * never delay a frame ---- */

/* A capture frame arrived: the signal is alive. */
void novl_frame(struct novl *n);
/* Call while building a commit. Returns the index (0/1) of a finished overlay buffer to put on
 * the overlay plane, or -1 to leave the plane as it is. */
int novl_overlay_next(struct novl *n);
/* The commit has latched (page-flip event). */
void novl_flip_done(struct novl *n);
/* About to requeue capture buffer `index`. Returns 1 if a snapshot is reading it: the caller
 * must not requeue it (the server thread does, through source.requeue). */
int novl_hold(struct novl *n, int index);

#endif
