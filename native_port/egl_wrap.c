#include "egl_wrap.h"
#include "gl_wrap.h"
#include "watchdog.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
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

#ifndef GL_MULTISAMPLE
#define GL_MULTISAMPLE 0x809D
#endif

static SDL_Window *g_egl_window = NULL;
static int g_msaa_enabled = 0;
static SDL_GLContext g_gl_context = NULL;
static EGLContext g_current_ctx = NULL;
static EGLSurface g_current_draw = NULL;
static EGLSurface g_current_read = NULL;

void egl_wrap_init(SDL_Window *window) {
    g_egl_window = window;
    g_gl_context = NULL;
    g_current_ctx = NULL;
    g_current_draw = NULL;
    g_current_read = NULL;
}

void egl_wrap_set_msaa(int enabled) {
    g_msaa_enabled = enabled;
}

void egl_wrap_shutdown(void) {
    if (g_gl_context) {
        SDL_GL_DeleteContext(g_gl_context);
        g_gl_context = NULL;
    }
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
    printf("[EGL] eglCreateContext on game thread %lu\n", GetCurrentThreadId());
    fflush(stdout);
    
    if (!g_gl_context && g_egl_window) {
        /* Configure OpenGL 2.1 Compatibility Context on this thread */
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
        
        g_gl_context = SDL_GL_CreateContext(g_egl_window);
        if (g_gl_context) {
            SDL_GL_MakeCurrent(g_egl_window, g_gl_context);
            SDL_GL_SetSwapInterval(1); /* VSync */
            if (g_msaa_enabled) glEnable(GL_MULTISAMPLE);
            gl_wrap_init();
            printf("[EGL] Context created: %s\n", (const char *)glGetString(GL_VERSION));
            printf("[EGL] Renderer: %s\n", (const char *)glGetString(GL_RENDERER));
            fflush(stdout);
        } else {
            fprintf(stderr, "[EGL] Failed to create GL context: %s\n", SDL_GetError());
            fflush(stderr);
        }
    }
    return (EGLContext)g_gl_context;
}

EGLBoolean CDECL wrap_eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
    g_current_ctx = ctx;
    g_current_draw = draw;
    g_current_read = read;
    if (g_egl_window) {
        if (ctx) {
            if (!g_gl_context) {
                wrap_eglCreateContext(dpy, NULL, NULL, NULL);
            }
            if (g_gl_context) {
                int ret = SDL_GL_MakeCurrent(g_egl_window, g_gl_context);
                if (ret < 0) {
                    printf("[EGL] SDL_GL_MakeCurrent returned %d (%s)\n", ret, SDL_GetError());
                    fflush(stdout);
                }
            }
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
        SDL_GL_GetDrawableSize(g_egl_window, &w, &h);
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

/* ------------------------------------------------------------- */
/* Controller menu cursor overlay                                */
/* ------------------------------------------------------------- */
/* The game's menus are touch-only, so with a controller a cursor is moved
 * with the left stick and A taps at it. It is drawn here, after the game
 * has finished its frame, with every piece of GL state it touches saved
 * and restored. */

#ifndef GL_CURRENT_PROGRAM
#define GL_CURRENT_PROGRAM 0x8B8D
#endif
#ifndef GL_ARRAY_BUFFER_BINDING
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_ARRAY_BUFFER_BINDING 0x8894
#define GL_ELEMENT_ARRAY_BUFFER_BINDING 0x8895
#endif
#ifndef GL_FRAMEBUFFER_BINDING
#define GL_FRAMEBUFFER 0x8D40
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#endif

typedef void (APIENTRY *PFN_UseProgram)(GLuint);
typedef void (APIENTRY *PFN_BindBuffer)(GLenum, GLuint);
typedef void (APIENTRY *PFN_BindFramebuffer)(GLenum, GLuint);

static volatile float g_cursor_x = 0.0f, g_cursor_y = 0.0f;
static volatile int g_cursor_visible = 0;
static volatile int g_cursor_pressed = 0;

void egl_overlay_set_cursor(float x, float y, int visible, int pressed) {
    g_cursor_x = x;
    g_cursor_y = y;
    g_cursor_pressed = pressed;
    g_cursor_visible = visible;
}

static void draw_cursor_overlay(void) {
    static PFN_UseProgram use_program;
    static PFN_BindBuffer bind_buffer;
    static PFN_BindFramebuffer bind_framebuffer;
    if (!use_program) {
        use_program = (PFN_UseProgram)SDL_GL_GetProcAddress("glUseProgram");
        bind_buffer = (PFN_BindBuffer)SDL_GL_GetProcAddress("glBindBuffer");
        bind_framebuffer = (PFN_BindFramebuffer)SDL_GL_GetProcAddress("glBindFramebuffer");
        if (!use_program || !bind_buffer) return;
    }

    int w = 1280, h = 720;
    SDL_GL_GetDrawableSize(g_egl_window, &w, &h);
    float x = g_cursor_x, y = g_cursor_y;
    float s = (float)h / 40.0f; /* cursor size scales with the window */

    GLint program = 0, array_buffer = 0, element_buffer = 0, framebuffer = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &element_buffer);
    if (bind_framebuffer) glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);

    use_program(0);
    bind_buffer(GL_ARRAY_BUFFER, 0);
    bind_buffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    if (bind_framebuffer) bind_framebuffer(GL_FRAMEBUFFER, 0);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, w, h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    /* Arrow: dark outline, then a white (or yellow while pressed) fill */
    for (int pass = 0; pass < 2; pass++) {
        float g = pass == 0 ? s * 0.18f : 0.0f;
        if (pass == 0) glColor4f(0.0f, 0.0f, 0.0f, 0.85f);
        else if (g_cursor_pressed) glColor4f(1.0f, 0.85f, 0.2f, 1.0f);
        else glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glBegin(GL_TRIANGLES);
        glVertex2f(x - g * 0.7f, y - g * 1.6f);
        glVertex2f(x - g * 0.7f, y + s * 1.4f + g * 1.4f);
        glVertex2f(x + s * 1.0f + g * 1.6f, y + s * 1.0f + g * 0.7f);
        glEnd();
    }

    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glPopClientAttrib();
    glPopAttrib();
    if (bind_framebuffer) bind_framebuffer(GL_FRAMEBUFFER, (GLuint)framebuffer);
    bind_buffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)element_buffer);
    bind_buffer(GL_ARRAY_BUFFER, (GLuint)array_buffer);
    use_program((GLuint)program);
}

static int s_swap_count = 0;
EGLBoolean CDECL wrap_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    if (g_egl_window && g_cursor_visible) {
        draw_cursor_overlay();
    }
    if (g_egl_window) {
        SDL_GL_SwapWindow(g_egl_window);
    }
    s_swap_count++;
    watchdog_frame();
    if ((s_swap_count % 300) == 1) {
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
    return (void *)SDL_GL_GetProcAddress(procname);
}
