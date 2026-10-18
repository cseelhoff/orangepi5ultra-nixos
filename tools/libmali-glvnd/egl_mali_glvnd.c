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

static void *hook, *mali;
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

static void *get_proc_address(const char *name)
{
	void *p = sym(name);
	if (!p && mali_get_proc_address)
		p = (void *)mali_get_proc_address(name);
	return p;
}

static EGLDisplay get_platform_display(EGLenum platform, void *native, const EGLAttrib *attribs)
{
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
	if (native && mali_gbm_create_device && *(void **)native == mali_gbm_create_device)
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

	mali = dlopen(LIBMALI_PATH, RTLD_NOW | RTLD_LOCAL);
	if (!mali) {
		fprintf(stderr, "libEGL_mali: %s\n", dlerror());
		return EGL_FALSE;
	}
	hook = dlopen(LIBMALI_HOOK_PATH, RTLD_NOW | RTLD_LOCAL);

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
