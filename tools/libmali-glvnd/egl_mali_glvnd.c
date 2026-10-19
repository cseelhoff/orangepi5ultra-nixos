// libEGL_mali: a libglvnd EGL vendor library that forwards to Arm's libmali.
//
// libmali is a monolithic EGL/GLES driver without glvnd's vendor entry point
// (__egl_Main), so on a glvnd system (NixOS) programs could only reach it by
// putting its own libEGL/libGLESv2 first in the library path. This adapter
// registers libmali with glvnd like any other driver (an egl_vendor.d JSON
// file), so unmodified programs use it. GL calls go through glvnd's normal
// dispatch, the same path every Mesa program takes.
//
// libmali-hook (Rockchip's compatibility fixes for eglGetDisplay, GBM, ...)
// is searched before libmali itself, as the libmali wrapper libraries do.
//
// GBM: programs linked against the system (Mesa) libgbm get objects from our
// GBM backend (mali_gbm_backend.c, installed as rockchip_gbm.so); window
// surfaces, GBM displays and native-pixmap images are unwrapped to the
// libmali objects before they reach Mali's EGL. libmali is loaded with
// RTLD_DEEPBIND so its own gbm_* calls never bind to Mesa's libgbm.
//
// Limitation: EGL extension functions are handed to glvnd directly instead of
// through per-display dispatch stubs. That is correct while libmali owns the
// displays (the only GPU on the RK3588; 10_mali.json sorts before Mesa's
// 50_mesa.json), but a program that mixes Mesa and Mali EGL displays in one
// process would call Mali's extension functions with a Mesa display.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <glvnd/libeglabi.h>

#ifndef LIBMALI_HOOK_PATH
#define LIBMALI_HOOK_PATH "libmali-hook.so.1"
#endif
#ifndef LIBMALI_PATH
#define LIBMALI_PATH "libmali.so.1"
#endif
#ifndef MALI_GBM_BACKEND_PATH
#define MALI_GBM_BACKEND_PATH "rockchip_gbm.so"
#endif

static void *hook, *mali;

/* Programs linked against the system libgbm get GBM objects from our Mesa
 * GBM backend (mali_gbm_backend.c); Mali's EGL needs the libmali objects
 * behind them. Identity when the backend is not loaded (e.g. libmali-run). */
static void *gbm_unwrap(void *p)
{
	static void *(*unwrap)(void *);
	static int looked;
	if (!looked) {
		void *b = dlopen(MALI_GBM_BACKEND_PATH, RTLD_NOW | RTLD_NOLOAD);
		if (b)
			unwrap = (void *(*)(void *))dlsym(b, "mali_gbm_unwrap");
		/* look again later if the backend wasn't loaded yet */
		looked = unwrap != NULL;
	}
	return unwrap && p ? unwrap(p) : p;
}
static PFNEGLGETPROCADDRESSPROC mali_get_proc_address;
static PFNEGLGETDISPLAYPROC mali_get_display;
static PFNEGLGETPLATFORMDISPLAYPROC mali_get_platform_display;
static PFNEGLQUERYSTRINGPROC mali_query_string;
static void *mali_gbm_create_device;

static void *sym(const char *name)
{
	void *p = hook ? dlsym(hook, name) : NULL;
	if (!p && mali)
		p = dlsym(mali, name);
	return p;
}

static void *mali_proc(const char *name)
{
	void *p = sym(name);
	if (!p && mali_get_proc_address)
		p = (void *)mali_get_proc_address(name);
	return p;
}

/* EGL entry points that take a native window/pixmap: unwrap GBM objects. */
static PFNEGLCREATEWINDOWSURFACEPROC real_create_window_surface;
static PFNEGLCREATEPLATFORMWINDOWSURFACEPROC real_create_platform_window_surface;
static PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC real_create_platform_window_surface_ext;
static PFNEGLCREATEIMAGEKHRPROC real_create_image_khr;
static PFNEGLCREATEIMAGEPROC real_create_image;

static EGLSurface create_window_surface(EGLDisplay d, EGLConfig c, EGLNativeWindowType w, const EGLint *a)
{
	return real_create_window_surface(d, c, (EGLNativeWindowType)gbm_unwrap((void *)w), a);
}

static EGLSurface create_platform_window_surface(EGLDisplay d, EGLConfig c, void *w, const EGLAttrib *a)
{
	return real_create_platform_window_surface(d, c, gbm_unwrap(w), a);
}

static EGLSurface create_platform_window_surface_ext(EGLDisplay d, EGLConfig c, void *w, const EGLint *a)
{
	return real_create_platform_window_surface_ext(d, c, gbm_unwrap(w), a);
}

static EGLImageKHR create_image_khr(EGLDisplay d, EGLContext c, EGLenum t, EGLClientBuffer b, const EGLint *a)
{
	if (t == EGL_NATIVE_PIXMAP_KHR)
		b = (EGLClientBuffer)gbm_unwrap((void *)b);
	return real_create_image_khr(d, c, t, b, a);
}

static EGLImage create_image(EGLDisplay d, EGLContext c, EGLenum t, EGLClientBuffer b, const EGLAttrib *a)
{
	if (t == EGL_NATIVE_PIXMAP_KHR)
		b = (EGLClientBuffer)gbm_unwrap((void *)b);
	return real_create_image(d, c, t, b, a);
}

static void *get_proc_address(const char *name)
{
	static const struct { const char *name; void *wrapper; void **real; } wrapped[] = {
		{ "eglCreateWindowSurface", create_window_surface, (void **)&real_create_window_surface },
		{ "eglCreatePlatformWindowSurface", create_platform_window_surface, (void **)&real_create_platform_window_surface },
		{ "eglCreatePlatformWindowSurfaceEXT", create_platform_window_surface_ext, (void **)&real_create_platform_window_surface_ext },
		{ "eglCreateImageKHR", create_image_khr, (void **)&real_create_image_khr },
		{ "eglCreateImage", create_image, (void **)&real_create_image },
	};
	for (size_t i = 0; i < sizeof(wrapped) / sizeof(wrapped[0]); i++)
		if (strcmp(name, wrapped[i].name) == 0) {
			if (!*wrapped[i].real)
				*wrapped[i].real = mali_proc(name);
			return *wrapped[i].real ? wrapped[i].wrapper : NULL;
		}
	return mali_proc(name);
}

static EGLDisplay get_platform_display(EGLenum platform, void *native, const EGLAttrib *attribs)
{
	if (native && (platform == EGL_PLATFORM_GBM_KHR || platform == EGL_NONE))
		native = gbm_unwrap(native);
	if (platform == EGL_NONE)
		return mali_get_display ? mali_get_display((EGLNativeDisplayType)native) : EGL_NO_DISPLAY;
	if (!mali_get_platform_display)
		return EGL_NO_DISPLAY;
	return mali_get_platform_display(platform, native, attribs);
}

static EGLBoolean get_supports_api(EGLenum api)
{
	return api == EGL_OPENGL_ES_API;
}

/* glvnd asks for the platform extensions this vendor supports. */
static const char *get_vendor_string(int name)
{
	static char platforms[1024];
	static int done;

	if (name != __EGL_VENDOR_STRING_PLATFORM_EXTENSIONS)
		return NULL;
	if (!done && mali_query_string) {
		const char *all = mali_query_string(EGL_NO_DISPLAY, EGL_EXTENSIONS);
		char *copy = all ? strdup(all) : NULL, *save = NULL;
		for (char *t = copy ? strtok_r(copy, " ", &save) : NULL; t; t = strtok_r(NULL, " ", &save)) {
			if (!strstr(t, "platform"))
				continue;
			if (strlen(platforms) + strlen(t) + 2 >= sizeof platforms)
				break;
			if (platforms[0])
				strcat(platforms, " ");
			strcat(platforms, t);
		}
		free(copy);
		done = 1;
	}
	return platforms;
}

/* Single-vendor shortcut, see the limitation note at the top. */
static void *get_dispatch_address(const char *name)
{
	return strncmp(name, "egl", 3) == 0 ? get_proc_address(name) : NULL;
}

static void set_dispatch_index(const char *name, int index)
{
	(void)name;
	(void)index;
}

/* A gbm_device starts with a pointer to gbm_create_device of the libgbm that
 * made it; recognise libmali's own GBM devices. */
static EGLenum find_native_display_platform(void *native)
{
	if (!native)
		return EGL_NONE;
	/* our Mesa GBM backend's devices, or libmali's own (first field is
	 * the gbm_create_device of the libgbm that made it) */
	if (gbm_unwrap(native) != native ||
	    (mali_gbm_create_device && *(void **)native == mali_gbm_create_device))
		return EGL_PLATFORM_GBM_KHR;
	return EGL_NONE;
}

EGLBoolean __egl_Main(uint32_t version, const __EGLapiExports *exports,
		      __EGLvendorInfo *vendor, __EGLapiImports *imports)
{
	(void)exports;
	(void)vendor;
	if (EGL_VENDOR_ABI_GET_MAJOR_VERSION(version) != EGL_VENDOR_ABI_MAJOR_VERSION)
		return EGL_FALSE;

	mali = dlopen(LIBMALI_PATH, RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
	if (!mali) {
		fprintf(stderr, "libEGL_mali: %s\n", dlerror());
		return EGL_FALSE;
	}
	hook = dlopen(LIBMALI_HOOK_PATH, RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);

	mali_get_proc_address = (PFNEGLGETPROCADDRESSPROC)sym("eglGetProcAddress");
	mali_get_display = (PFNEGLGETDISPLAYPROC)sym("eglGetDisplay");
	mali_get_platform_display = (PFNEGLGETPLATFORMDISPLAYPROC)sym("eglGetPlatformDisplay");
	mali_query_string = (PFNEGLQUERYSTRINGPROC)sym("eglQueryString");
	mali_gbm_create_device = dlsym(mali, "gbm_create_device");
	if (!mali_get_proc_address || !mali_get_display || !mali_query_string)
		return EGL_FALSE;

	memset(imports, 0, sizeof(*imports));
	imports->getPlatformDisplay = get_platform_display;
	imports->getSupportsAPI = get_supports_api;
	imports->getVendorString = get_vendor_string;
	imports->getProcAddress = get_proc_address;
	imports->getDispatchAddress = get_dispatch_address;
	imports->setDispatchIndex = set_dispatch_index;
	imports->findNativeDisplayPlatform = find_native_display_platform;
	return EGL_TRUE;
}
