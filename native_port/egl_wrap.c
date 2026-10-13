#include "egl_wrap.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CDECL
#undef CDECL
#endif
#define CDECL __attribute__((cdecl))

typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLSurface;
typedef void *EGLContext;
typedef int32_t EGLint;
typedef uint32_t EGLBoolean;
typedef uint32_t EGLenum;

#define EGL_FALSE           0
#define EGL_TRUE            1
#define EGL_SUCCESS         0x3000
#define EGL_WIDTH           0x3057
#define EGL_HEIGHT          0x3056
#define EGL_NONE            0x3038
#define EGL_DRAW            0x3059
#define EGL_READ            0x305a

static SDL_Window *g_egl_window = NULL;
static SDL_GLContext g_gl_context = NULL;
static EGLContext g_current_ctx = NULL;
static EGLSurface g_current_draw = NULL;
static EGLSurface g_current_read = NULL;

void egl_wrap_init(SDL_Window *window, SDL_GLContext gl_ctx) {
    g_egl_window = window;
    g_gl_context = gl_ctx;
    g_current_ctx = NULL;
    g_current_draw = NULL;
    g_current_read = NULL;
}

EGLDisplay CDECL wrap_eglGetDisplay(void *native_display) {
    return (EGLDisplay)1;
}

EGLBoolean CDECL wrap_eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor) {
    if (major) *major = 1;
    if (minor) *minor = 4;
    return EGL_TRUE;
}

EGLBoolean CDECL wrap_eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list, EGLConfig *configs, EGLint config_size, EGLint *num_config) {
    if (configs && config_size > 0) {
        configs[0] = (EGLConfig)1;
    }
    if (num_config) {
        *num_config = 1;
    }
    return EGL_TRUE;
}

EGLBoolean CDECL wrap_eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint *value) {
    if (value) *value = 0;
    return EGL_TRUE;
}

EGLSurface CDECL wrap_eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, void *native_window, const EGLint *attrib_list) {
    printf("[EGL] eglCreateWindowSurface(window=%p)\n", native_window);
    fflush(stdout);
    return (EGLSurface)0x87654321;
}

EGLContext CDECL wrap_eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share_context, const EGLint *attrib_list) {
    printf("[EGL] eglCreateContext\n");
    fflush(stdout);
    return (EGLContext)0x12345678;
}

EGLBoolean CDECL wrap_eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
    printf("[EGL] eglMakeCurrent(ctx=%p)\n", ctx);
    fflush(stdout);
    g_current_ctx = ctx;
    g_current_draw = draw;
    g_current_read = read;
    if (g_egl_window) {
        if (ctx && g_gl_context) {
            int ret = SDL_GL_MakeCurrent(g_egl_window, g_gl_context);
            printf("[EGL] SDL_GL_MakeCurrent returned %d (%s)\n", ret, ret < 0 ? SDL_GetError() : "OK");
            fflush(stdout);
        } else {
            SDL_GL_MakeCurrent(g_egl_window, NULL);
        }
    }
    return EGL_TRUE;
}

EGLContext CDECL wrap_eglGetCurrentContext(void) {
    return g_current_ctx;
}

EGLSurface CDECL wrap_eglGetCurrentSurface(EGLint readdraw) {
    return (readdraw == EGL_DRAW) ? g_current_draw : g_current_read;
}

EGLBoolean CDECL wrap_eglQuerySurface(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint *value) {
    int w = 1280, h = 720;
    if (g_egl_window) {
        SDL_GetWindowSize(g_egl_window, &w, &h);
    }
    if (value) {
        if (attribute == EGL_WIDTH) {
            *value = w;
        } else if (attribute == EGL_HEIGHT) {
            *value = h;
        } else {
            *value = 0;
        }
    }
    return EGL_TRUE;
}

static int s_swap_count = 0;
EGLBoolean CDECL wrap_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    if (g_egl_window) {
        SDL_GL_SwapWindow(g_egl_window);
    }
    s_swap_count++;
    if ((s_swap_count % 60) == 1) {
        printf("[EGL] Frame %d swapped (60 FPS)\n", s_swap_count);
        fflush(stdout);
    }
    return EGL_TRUE;
}

EGLint CDECL wrap_eglGetError(void) {
    return EGL_SUCCESS;
}

EGLBoolean CDECL wrap_eglBindAPI(EGLenum api) {
    return EGL_TRUE;
}

EGLBoolean CDECL wrap_eglDestroyContext(EGLDisplay dpy, EGLContext ctx) {
    if (g_current_ctx == ctx) {
        g_current_ctx = NULL;
    }
    return EGL_TRUE;
}

EGLBoolean CDECL wrap_eglDestroySurface(EGLDisplay dpy, EGLSurface surface) {
    if (g_current_draw == surface) g_current_draw = NULL;
    if (g_current_read == surface) g_current_read = NULL;
    return EGL_TRUE;
}

EGLBoolean CDECL wrap_eglTerminate(EGLDisplay dpy) {
    return EGL_TRUE;
}

void * CDECL wrap_eglGetProcAddress(const char *procname) {
    if (!procname) return NULL;
    return SDL_GL_GetProcAddress(procname);
}
