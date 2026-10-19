// rockchip_gbm.so: a Mesa GBM backend that forwards to Arm's libmali GBM.
//
// Mesa's libgbm loads "<drm driver>_gbm.so" (here: rockchip) from
// /run/opengl-driver/lib/gbm before its own "dri" backend, the same hook
// NVIDIA's nvidia-drm_gbm.so uses. With this backend, programs linked
// against the system libgbm (compositors, kmscube, glmark2-es2-drm, ...)
// allocate their scanout buffers with libmali, which is what Mali's EGL can
// render into. Objects are thin wrappers around the libmali ones (no copies).
// libEGL_mali (egl_mali_glvnd.c) unwraps them with mali_gbm_unwrap() when a
// program hands them to EGL.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mesa-gbm/gbm_backend_abi.h"

#ifndef LIBMALI_PATH
#define LIBMALI_PATH "libmali.so.1"
#endif
#ifndef LIBMALI_HOOK_PATH
#define LIBMALI_HOOK_PATH "libmali-hook.so.1"
#endif

#define EXPORT __attribute__((visibility("default")))

/* ---- libmali's GBM entry points (standard gbm.h API) ---- */
static struct {
	struct gbm_device *(*create_device)(int);
	void (*device_destroy)(struct gbm_device *);
	int (*device_is_format_supported)(struct gbm_device *, uint32_t, uint32_t);
	int (*device_get_format_modifier_plane_count)(struct gbm_device *, uint32_t, uint64_t);
	struct gbm_bo *(*bo_create)(struct gbm_device *, uint32_t, uint32_t, uint32_t, uint32_t);
	struct gbm_bo *(*bo_create_with_modifiers2)(struct gbm_device *, uint32_t, uint32_t, uint32_t,
						    const uint64_t *, unsigned int, uint32_t);
	struct gbm_bo *(*bo_import)(struct gbm_device *, uint32_t, void *, uint32_t);
	void *(*bo_map)(struct gbm_bo *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *, void **);
	void (*bo_unmap)(struct gbm_bo *, void *);
	int (*bo_write)(struct gbm_bo *, const void *, size_t);
	int (*bo_get_fd)(struct gbm_bo *);
	int (*bo_get_plane_count)(struct gbm_bo *);
	union gbm_bo_handle (*bo_get_handle)(struct gbm_bo *);
	union gbm_bo_handle (*bo_get_handle_for_plane)(struct gbm_bo *, int);
	int (*bo_get_fd_for_plane)(struct gbm_bo *, int);
	uint32_t (*bo_get_width)(struct gbm_bo *);
	uint32_t (*bo_get_height)(struct gbm_bo *);
	uint32_t (*bo_get_stride)(struct gbm_bo *);
	uint32_t (*bo_get_stride_for_plane)(struct gbm_bo *, int);
	uint32_t (*bo_get_format)(struct gbm_bo *);
	uint32_t (*bo_get_offset)(struct gbm_bo *, int);
	uint64_t (*bo_get_modifier)(struct gbm_bo *);
	void (*bo_set_user_data)(struct gbm_bo *, void *, void (*)(struct gbm_bo *, void *));
	void (*bo_destroy)(struct gbm_bo *);
	struct gbm_surface *(*surface_create)(struct gbm_device *, uint32_t, uint32_t, uint32_t, uint32_t);
	struct gbm_surface *(*surface_create_with_modifiers2)(struct gbm_device *, uint32_t, uint32_t, uint32_t,
							      const uint64_t *, unsigned int, uint32_t);
	struct gbm_bo *(*surface_lock_front_buffer)(struct gbm_surface *);
	void (*surface_release_buffer)(struct gbm_surface *, struct gbm_bo *);
	int (*surface_has_free_buffers)(struct gbm_surface *);
	void (*surface_destroy)(struct gbm_surface *);
} M;

static void *hook, *mali;

static void *sym(const char *name)
{
	void *p = hook ? dlsym(hook, name) : NULL;
	return p ? p : dlsym(mali, name);
}

static int load_mali(void)
{
	if (mali)
		return 0;
	/* DEEPBIND: libmali's own gbm_* calls must not bind to Mesa's libgbm,
	 * which is already loaded in every program that reaches this backend. */
	mali = dlopen(LIBMALI_PATH, RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
	if (!mali) {
		fprintf(stderr, "rockchip_gbm: %s\n", dlerror());
		return -1;
	}
	hook = dlopen(LIBMALI_HOOK_PATH, RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
#define L(field, name) M.field = sym(name)
	L(create_device, "gbm_create_device");
	L(device_destroy, "gbm_device_destroy");
	L(device_is_format_supported, "gbm_device_is_format_supported");
	L(device_get_format_modifier_plane_count, "gbm_device_get_format_modifier_plane_count");
	L(bo_create, "gbm_bo_create");
	L(bo_create_with_modifiers2, "gbm_bo_create_with_modifiers2");
	L(bo_import, "gbm_bo_import");
	L(bo_map, "gbm_bo_map");
	L(bo_unmap, "gbm_bo_unmap");
	L(bo_write, "gbm_bo_write");
	L(bo_get_fd, "gbm_bo_get_fd");
	L(bo_get_plane_count, "gbm_bo_get_plane_count");
	L(bo_get_handle, "gbm_bo_get_handle");
	L(bo_get_handle_for_plane, "gbm_bo_get_handle_for_plane");
	L(bo_get_fd_for_plane, "gbm_bo_get_fd_for_plane");
	L(bo_get_width, "gbm_bo_get_width");
	L(bo_get_height, "gbm_bo_get_height");
	L(bo_get_stride, "gbm_bo_get_stride");
	L(bo_get_stride_for_plane, "gbm_bo_get_stride_for_plane");
	L(bo_get_format, "gbm_bo_get_format");
	L(bo_get_offset, "gbm_bo_get_offset");
	L(bo_get_modifier, "gbm_bo_get_modifier");
	L(bo_set_user_data, "gbm_bo_set_user_data");
	L(bo_destroy, "gbm_bo_destroy");
	L(surface_create, "gbm_surface_create");
	L(surface_create_with_modifiers2, "gbm_surface_create_with_modifiers2");
	L(surface_lock_front_buffer, "gbm_surface_lock_front_buffer");
	L(surface_release_buffer, "gbm_surface_release_buffer");
	L(surface_has_free_buffers, "gbm_surface_has_free_buffers");
	L(surface_destroy, "gbm_surface_destroy");
#undef L
	if (!M.create_device || !M.bo_create || !M.surface_create || !M.bo_set_user_data) {
		fprintf(stderr, "rockchip_gbm: libmali lacks the GBM API\n");
		return -1;
	}
	return 0;
}

/* ---- wrappers and the wrapper -> libmali registry ---- */
struct wdev { struct gbm_device base; struct gbm_device *mali; };
struct wbo { struct gbm_bo base; struct gbm_bo *mali; };
struct wsurf { struct gbm_surface base; struct gbm_surface *mali; };

static pthread_mutex_t reg_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { void *wrapper, *mali; } *reg;
static size_t reg_n, reg_cap;

static void reg_add(void *w, void *m)
{
	pthread_mutex_lock(&reg_lock);
	if (reg_n == reg_cap) {
		size_t cap = reg_cap ? reg_cap * 2 : 64;
		void *n = realloc(reg, cap * sizeof(*reg));
		if (!n) { pthread_mutex_unlock(&reg_lock); return; }
		reg = n;
		reg_cap = cap;
	}
	reg[reg_n].wrapper = w;
	reg[reg_n].mali = m;
	reg_n++;
	pthread_mutex_unlock(&reg_lock);
}

static void reg_del(void *w)
{
	pthread_mutex_lock(&reg_lock);
	for (size_t i = 0; i < reg_n; i++)
		if (reg[i].wrapper == w) {
			reg[i] = reg[--reg_n];
			break;
		}
	pthread_mutex_unlock(&reg_lock);
}

/* For libEGL_mali: the libmali object behind a wrapper, or p unchanged. */
EXPORT void *mali_gbm_unwrap(void *p)
{
	void *m = p;
	pthread_mutex_lock(&reg_lock);
	for (size_t i = 0; i < reg_n; i++)
		if (reg[i].wrapper == p) {
			m = reg[i].mali;
			break;
		}
	pthread_mutex_unlock(&reg_lock);
	return m;
}

#define WDEV(g) ((struct wdev *)(g))
#define WBO(b) ((struct wbo *)(b))
#define WSURF(s) ((struct wsurf *)(s))

/* libmali destroyed its buffer (bo_destroy or surface teardown). */
static void mali_bo_gone(struct gbm_bo *mb, void *data)
{
	struct wbo *w = data;
	(void)mb;
	if (w->base.v0.destroy_user_data)
		w->base.v0.destroy_user_data(&w->base, w->base.v0.user_data);
	reg_del(w);
	free(w);
}

/* One wrapper per libmali buffer, kept in libmali's user-data slot, so
 * surface buffers are wrapped once and reused every frame. */
static struct gbm_bo *wrap_bo(struct gbm_device *gbm, struct gbm_bo *mb)
{
	if (!mb)
		return NULL;
	void *(*get_user_data)(struct gbm_bo *) = sym("gbm_bo_get_user_data");
	struct wbo *w = get_user_data ? get_user_data(mb) : NULL;
	if (w)
		return &w->base;
	w = calloc(1, sizeof(*w));
	if (!w)
		return NULL;
	w->mali = mb;
	w->base.gbm = gbm;
	w->base.v0.width = M.bo_get_width(mb);
	w->base.v0.height = M.bo_get_height(mb);
	w->base.v0.stride = M.bo_get_stride(mb);
	w->base.v0.format = M.bo_get_format(mb);
	w->base.v0.handle = M.bo_get_handle(mb);
	M.bo_set_user_data(mb, w, mali_bo_gone);
	reg_add(w, mb);
	return &w->base;
}

/* ---- gbm_device_v0 ---- */
static void dev_destroy(struct gbm_device *gbm)
{
	M.device_destroy(WDEV(gbm)->mali);
	reg_del(gbm);
	free(gbm);
}

static int is_format_supported(struct gbm_device *gbm, uint32_t format, uint32_t usage)
{
	return M.device_is_format_supported ? M.device_is_format_supported(WDEV(gbm)->mali, format, usage) : 1;
}

static int get_format_modifier_plane_count(struct gbm_device *gbm, uint32_t format, uint64_t modifier)
{
	return M.device_get_format_modifier_plane_count ?
	       M.device_get_format_modifier_plane_count(WDEV(gbm)->mali, format, modifier) : -1;
}

static struct gbm_bo *bo_create(struct gbm_device *gbm, uint32_t w, uint32_t h, uint32_t format,
				uint32_t usage, const uint64_t *modifiers, const unsigned int count)
{
	struct gbm_bo *mb = count && M.bo_create_with_modifiers2 ?
		M.bo_create_with_modifiers2(WDEV(gbm)->mali, w, h, format, modifiers, count, usage) :
		M.bo_create(WDEV(gbm)->mali, w, h, format, usage);
	return wrap_bo(gbm, mb);
}

static struct gbm_bo *bo_import(struct gbm_device *gbm, uint32_t type, void *buffer, uint32_t usage)
{
	return M.bo_import ? wrap_bo(gbm, M.bo_import(WDEV(gbm)->mali, type, buffer, usage)) : NULL;
}

static void *bo_map(struct gbm_bo *bo, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
		    uint32_t flags, uint32_t *stride, void **map_data)
{
	return M.bo_map ? M.bo_map(WBO(bo)->mali, x, y, w, h, flags, stride, map_data) : NULL;
}

static void bo_unmap(struct gbm_bo *bo, void *map_data)
{
	if (M.bo_unmap)
		M.bo_unmap(WBO(bo)->mali, map_data);
}

static int bo_write(struct gbm_bo *bo, const void *buf, size_t n)
{
	if (!M.bo_write) { errno = ENOSYS; return -1; }
	return M.bo_write(WBO(bo)->mali, buf, n);
}

static int bo_get_fd(struct gbm_bo *bo) { return M.bo_get_fd(WBO(bo)->mali); }
static int bo_get_planes(struct gbm_bo *bo) { return M.bo_get_plane_count ? M.bo_get_plane_count(WBO(bo)->mali) : 1; }

static union gbm_bo_handle bo_get_handle(struct gbm_bo *bo, int plane)
{
	if (M.bo_get_handle_for_plane)
		return M.bo_get_handle_for_plane(WBO(bo)->mali, plane);
	return M.bo_get_handle(WBO(bo)->mali);
}

static int bo_get_plane_fd(struct gbm_bo *bo, int plane)
{
	return M.bo_get_fd_for_plane ? M.bo_get_fd_for_plane(WBO(bo)->mali, plane) : M.bo_get_fd(WBO(bo)->mali);
}

static uint32_t bo_get_stride(struct gbm_bo *bo, int plane)
{
	if (M.bo_get_stride_for_plane)
		return M.bo_get_stride_for_plane(WBO(bo)->mali, plane);
	return M.bo_get_stride(WBO(bo)->mali);
}

static uint32_t bo_get_offset(struct gbm_bo *bo, int plane)
{
	return M.bo_get_offset ? M.bo_get_offset(WBO(bo)->mali, plane) : 0;
}

static uint64_t bo_get_modifier(struct gbm_bo *bo)
{
	return M.bo_get_modifier ? M.bo_get_modifier(WBO(bo)->mali) : 0; /* DRM_FORMAT_MOD_LINEAR */
}

static void bo_destroy(struct gbm_bo *bo)
{
	/* Mesa's gbm_bo_destroy already ran the user's destructor. */
	bo->v0.destroy_user_data = NULL;
	M.bo_destroy(WBO(bo)->mali); /* frees the wrapper through mali_bo_gone */
}

static struct gbm_surface *surface_create(struct gbm_device *gbm, uint32_t w, uint32_t h, uint32_t format,
					  uint32_t flags, const uint64_t *modifiers, const unsigned count)
{
	struct gbm_surface *ms = count && M.surface_create_with_modifiers2 ?
		M.surface_create_with_modifiers2(WDEV(gbm)->mali, w, h, format, modifiers, count, flags) :
		M.surface_create(WDEV(gbm)->mali, w, h, format, flags);
	if (!ms)
		return NULL;
	struct wsurf *s = calloc(1, sizeof(*s));
	if (!s) {
		M.surface_destroy(ms);
		return NULL;
	}
	s->mali = ms;
	s->base.gbm = gbm;
	s->base.v0.width = w;
	s->base.v0.height = h;
	s->base.v0.format = format;
	s->base.v0.flags = flags;
	reg_add(s, ms);
	return &s->base;
}

static struct gbm_bo *surface_lock_front_buffer(struct gbm_surface *surf)
{
	return wrap_bo(surf->gbm, M.surface_lock_front_buffer(WSURF(surf)->mali));
}

static void surface_release_buffer(struct gbm_surface *surf, struct gbm_bo *bo)
{
	M.surface_release_buffer(WSURF(surf)->mali, WBO(bo)->mali);
}

static int surface_has_free_buffers(struct gbm_surface *surf)
{
	return M.surface_has_free_buffers(WSURF(surf)->mali);
}

static void surface_destroy(struct gbm_surface *surf)
{
	reg_del(surf);
	M.surface_destroy(WSURF(surf)->mali);
	free(surf);
}

/* ---- backend entry point ---- */
static struct gbm_device *create_device(int fd, uint32_t abi_version)
{
	if (load_mali())
		return NULL;
	struct gbm_device *md = M.create_device(fd);
	if (!md)
		return NULL;
	struct wdev *d = calloc(1, sizeof(*d));
	if (!d) {
		M.device_destroy(md);
		return NULL;
	}
	d->mali = md;
	d->base.v0.backend_version = abi_version;
	d->base.v0.fd = fd;
	d->base.v0.name = "mali";
	d->base.v0.destroy = dev_destroy;
	d->base.v0.is_format_supported = is_format_supported;
	d->base.v0.get_format_modifier_plane_count = get_format_modifier_plane_count;
	d->base.v0.bo_create = bo_create;
	d->base.v0.bo_import = bo_import;
	d->base.v0.bo_map = bo_map;
	d->base.v0.bo_unmap = bo_unmap;
	d->base.v0.bo_write = bo_write;
	d->base.v0.bo_get_fd = bo_get_fd;
	d->base.v0.bo_get_planes = bo_get_planes;
	d->base.v0.bo_get_handle = bo_get_handle;
	d->base.v0.bo_get_plane_fd = bo_get_plane_fd;
	d->base.v0.bo_get_stride = bo_get_stride;
	d->base.v0.bo_get_offset = bo_get_offset;
	d->base.v0.bo_get_modifier = bo_get_modifier;
	d->base.v0.bo_destroy = bo_destroy;
	d->base.v0.surface_create = surface_create;
	d->base.v0.surface_lock_front_buffer = surface_lock_front_buffer;
	d->base.v0.surface_release_buffer = surface_release_buffer;
	d->base.v0.surface_has_free_buffers = surface_has_free_buffers;
	d->base.v0.surface_destroy = surface_destroy;
	reg_add(d, md);
	return &d->base;
}

static const struct gbm_backend backend = {
	.v0 = {
		.backend_version = GBM_BACKEND_ABI_VERSION,
		.backend_name = "mali",
		.create_device = create_device,
	},
};

EXPORT const struct gbm_backend *gbmint_get_backend(const struct gbm_core *core)
{
	(void)core;
	return &backend;
}
