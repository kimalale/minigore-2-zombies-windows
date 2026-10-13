#include "egl_wrap.h"
#include "gl_wrap.h"
#include "watchdog.h"
#include "net_host.h"
#include "determinism.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

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

typedef struct {
    GLint program, array_buffer, element_buffer, framebuffer;
} OverlayState;

static PFN_UseProgram ov_use_program;
static PFN_BindBuffer ov_bind_buffer;
static PFN_BindFramebuffer ov_bind_framebuffer;

/* Plain fixed-function drawing in window pixels, every piece of GL state
 * it touches saved and restored */
static int overlay_begin(OverlayState *st, int *w, int *h) {
    if (!ov_use_program) {
        ov_use_program = (PFN_UseProgram)SDL_GL_GetProcAddress("glUseProgram");
        ov_bind_buffer = (PFN_BindBuffer)SDL_GL_GetProcAddress("glBindBuffer");
        ov_bind_framebuffer = (PFN_BindFramebuffer)SDL_GL_GetProcAddress("glBindFramebuffer");
        if (!ov_use_program || !ov_bind_buffer) return 0;
    }
    *w = 1280;
    *h = 720;
    SDL_GL_GetDrawableSize(g_egl_window, w, h);

    memset(st, 0, sizeof(*st));
    glGetIntegerv(GL_CURRENT_PROGRAM, &st->program);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &st->array_buffer);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &st->element_buffer);
    if (ov_bind_framebuffer) glGetIntegerv(GL_FRAMEBUFFER_BINDING, &st->framebuffer);

    ov_use_program(0);
    ov_bind_buffer(GL_ARRAY_BUFFER, 0);
    ov_bind_buffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    if (ov_bind_framebuffer) ov_bind_framebuffer(GL_FRAMEBUFFER, 0);
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
    glViewport(0, 0, *w, *h);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, *w, *h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    return 1;
}

static void overlay_end(const OverlayState *st) {
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glPopClientAttrib();
    glPopAttrib();
    if (ov_bind_framebuffer) ov_bind_framebuffer(GL_FRAMEBUFFER, (GLuint)st->framebuffer);
    ov_bind_buffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)st->element_buffer);
    ov_bind_buffer(GL_ARRAY_BUFFER, (GLuint)st->array_buffer);
    ov_use_program((GLuint)st->program);
}

static void draw_cursor_overlay(int w, int h) {
    float x = g_cursor_x, y = g_cursor_y;
    float s = (float)h / 40.0f; /* cursor size scales with the window */

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
}

/* ------------------------------------------------------------- */
/* Own-world co-op overlay: both players' health, partner marker */
/* ------------------------------------------------------------- */

static EglCoopOverlay g_coop;  /* game thread only */
static int g_coop_frames = 0;  /* shown while refreshed each frame (levels only) */

void egl_overlay_set_coop(const EglCoopOverlay *info) {
    if (info) g_coop = *info;
    else g_coop.visible = 0;
    g_coop_frames = g_coop.visible ? 2 : 0;
}

/* 3x5 pixel font, rows top to bottom, 3 bits each (left = 4) */
static const struct { char c; unsigned char rows[5]; } k_glyphs[] = {
    { '0', { 2, 5, 5, 5, 2 } }, { '1', { 2, 6, 2, 2, 7 } }, { '2', { 6, 1, 2, 4, 7 } },
    { 'A', { 2, 5, 7, 5, 5 } }, { 'C', { 3, 4, 4, 4, 3 } }, { 'D', { 6, 5, 5, 5, 6 } },
    { 'F', { 7, 4, 6, 4, 4 } }, { 'N', { 5, 7, 7, 5, 5 } }, { 'O', { 2, 5, 5, 5, 2 } },
    { 'P', { 6, 5, 6, 4, 4 } }, { 'S', { 3, 4, 2, 1, 6 } }, { 'T', { 7, 2, 2, 2, 2 } },
    { 'U', { 5, 5, 5, 5, 7 } }, { 'W', { 5, 5, 7, 7, 5 } }, { 'Y', { 5, 5, 2, 2, 2 } },
};

static void draw_rect(float x, float y, float w, float h) {
    glBegin(GL_QUADS);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();
}

static float text_width(const char *t, float px) {
    return (float)strlen(t) * 4.0f * px - px;
}

static void draw_text(float x, float y, float px, const char *t) {
    for (; *t; t++, x += 4.0f * px) {
        for (size_t g = 0; g < sizeof(k_glyphs) / sizeof(k_glyphs[0]); g++) {
            if (k_glyphs[g].c != *t) continue;
            for (int r = 0; r < 5; r++) {
                for (int c = 0; c < 3; c++) {
                    if (k_glyphs[g].rows[r] & (4 >> c)) draw_rect(x + c * px, y + r * px, px, px);
                }
            }
        }
    }
}

static void player_color(int i, float a) {
    if (i == 0) glColor4f(0.35f, 0.65f, 1.0f, a);  /* player 1: blue */
    else glColor4f(1.0f, 0.6f, 0.15f, a);          /* player 2: orange */
}

static void draw_coop_overlay(int w, int h) {
    float u = (float)h / 720.0f;
    float px = 3.0f * u;                      /* font pixel */
    float bar_w = (float)w * 0.16f, bar_h = 12.0f * u, gap = 36.0f * u;
    float top = 14.0f * u + 7.0f * px;

    for (int i = 0; i < 2; i++) {
        if (!g_coop.p[i].present) continue;
        float x = i == 0 ? w * 0.5f - gap * 0.5f - bar_w : w * 0.5f + gap * 0.5f;
        const char *name = i == 0 ? "P1" : "P2";
        player_color(i, 1.0f);
        draw_text(x, top - 7.0f * px, px, name);
        if (i == g_coop.local) {
            glColor4f(1.0f, 1.0f, 1.0f, 0.9f);
            draw_text(x + text_width(name, px) + 3.0f * px, top - 7.0f * px, px, "YOU");
        }
        glColor4f(0.0f, 0.0f, 0.0f, 0.6f);
        draw_rect(x - 2.0f * u, top - 2.0f * u, bar_w + 4.0f * u, bar_h + 4.0f * u);
        float f = g_coop.p[i].alive ? g_coop.p[i].health : 0.0f;
        if (f > 0.0f) {
            if (f < 0.3f) glColor4f(0.95f, 0.2f, 0.15f, 1.0f);
            else player_color(i, 1.0f);
            draw_rect(x, top, bar_w * f, bar_h);
        }
        if (!g_coop.p[i].alive) {
            glColor4f(1.0f, 1.0f, 1.0f, 0.9f);
            draw_text(x + (bar_w - text_width("DOWN", px)) * 0.5f, top + (bar_h - 5.0f * px) * 0.5f, px, "DOWN");
        }
    }

    /* Where the partner is: a marker over them, or an arrow at the edge */
    int o = 1 - g_coop.local;
    if (g_coop.p[o].present && g_coop.p[o].alive && g_coop.p[o].on_screen_valid) {
        float tx = g_coop.p[o].sx, ty = g_coop.p[o].sy - 75.0f * u;
        float m = 28.0f * u, r = 11.0f * u;
        int inside = tx > m && tx < w - m && ty > m && ty < h - m;
        float dx = 0.0f, dy = 1.0f; /* pointing down at them */
        if (!inside) {
            float cx = w * 0.5f, cy = h * 0.5f;
            dx = tx - cx;
            dy = ty - cy;
            float l = sqrtf(dx * dx + dy * dy);
            if (l < 1e-3f) l = 1.0f;
            dx /= l;
            dy /= l;
            float sx = dx != 0.0f ? ((dx > 0 ? w - m : m) - cx) / dx : 1e9f;
            float sy = dy != 0.0f ? ((dy > 0 ? h - m : m) - cy) / dy : 1e9f;
            float t = sx < sy ? sx : sy;
            tx = cx + dx * t;
            ty = cy + dy * t;
        }
        float nx = -dy, ny = dx;
        for (int pass = 0; pass < 2; pass++) {
            float g = pass == 0 ? 3.0f * u : 0.0f;
            if (pass == 0) glColor4f(0.0f, 0.0f, 0.0f, 0.7f);
            else player_color(o, 1.0f);
            glBegin(GL_TRIANGLES);
            glVertex2f(tx + dx * (r + g), ty + dy * (r + g));
            glVertex2f(tx - dx * r + nx * (r + g), ty - dy * r + ny * (r + g));
            glVertex2f(tx - dx * r - nx * (r + g), ty - dy * r - ny * (r + g));
            glEnd();
        }
        const char *name = o == 0 ? "P1" : "P2";
        player_color(o, 1.0f);
        draw_text(tx - dx * (r + 9.0f * px) - text_width(name, px) * 0.5f,
                  ty - dy * (r + 9.0f * px) - 2.5f * px, px, name);
    }

    if (g_coop.out_of_sync) {
        const char *t = "OUT OF SYNC";
        glColor4f(1.0f, 0.25f, 0.2f, 1.0f);
        draw_text(w * 0.5f - text_width(t, px) * 0.5f, top + bar_h + 8.0f * u, px, t);
    }
}

static int s_swap_count = 0;
EGLBoolean CDECL wrap_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    det_frame_end(); /* determinism test: fingerprint this frame's state */
    if (g_egl_window && net_host_guest_connected()) {
        /* Stream the finished frame (before the host's menu cursor) */
        int w, h;
        SDL_GL_GetDrawableSize(g_egl_window, &w, &h);
        net_host_capture_frame(w, h);
    }
    int coop = g_coop_frames > 0;
    if (g_coop_frames > 0) g_coop_frames--;
    if (g_egl_window && (g_cursor_visible || coop)) {
        OverlayState st;
        int w, h;
        if (overlay_begin(&st, &w, &h)) {
            if (coop) draw_coop_overlay(w, h);
            if (g_cursor_visible) draw_cursor_overlay(w, h);
            overlay_end(&st);
        }
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
