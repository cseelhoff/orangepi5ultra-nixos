/* novl: the display process's end of the Natively edge protocol.
 *
 * Over one Unix socket the edge daemon (natively-edge) pushes the picture to draw over the
 * video IN, and asks for a screenshot and the picture size OUT. The wire format is
 * docs/edge-display.md in the natively-linux repo (messages OVERLAY, SNAPSHOT, INFO).
 *
 * A server thread owns the socket, the overlay blit and the screenshot encode. The display
 * loop only calls the hooks below: none of them waits on a client or copies pixels, so the
 * latency path stays as it was. The CPU never reads the capture buffers (they are uncached):
 * a screenshot is encoded by the hardware, through the `encode` callback.
 *
 * Overlay: two ARGB8888 buffers the size of the output. The thread draws the next overlay
 * into the one that is not on screen; the loop puts it on the plane at its next commit.
 * Snapshot: the loop hands over the capture buffer it is about to requeue, which has been
 * on screen for a whole frame and so is complete. It is returned once it has been encoded.
 */
#ifndef NOVL_H
#define NOVL_H

#include <stddef.h>
#include <stdint.h>

struct novl_source {
	uint32_t width, height;	/* the picture the capture device delivers */
	/* Encodes the whole picture in held capture buffer `index` as a JPEG: a malloc'd block in
	 * *jpeg, *len bytes. 0 = ok. Called on the server thread; it may take as long as it needs. */
	int (*encode)(void *ctx, int index, void **jpeg, size_t *len);
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
