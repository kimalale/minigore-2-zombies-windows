#include "ui_nav.h"
#include "hook_util.h"
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define MAX_BUTTONS 256
#define MIN_SIZE    8      /* ignore degenerate hit areas */

typedef struct {
    int x, y, w, h;
} UiRect;

typedef int  (__attribute__((cdecl)) *ButtonLogicFn)(unsigned id, int x, int y, int w, int h);
typedef void (__attribute__((cdecl)) *BeginFrameFn)(float dt);

static ButtonLogicFn o_button_logic;
static BeginFrameFn o_begin_frame;

/* Game thread collects this frame's buttons; the previous complete frame
 * is published for the input thread under g_lock. */
static UiRect g_building[MAX_BUTTONS];
static int g_building_count = 0;
static UiRect g_published[MAX_BUTTONS];
static int g_published_count = 0;
static CRITICAL_SECTION g_lock;

static int __attribute__((cdecl)) h_button_logic(unsigned id, int x, int y, int w, int h) {
    if (w >= MIN_SIZE && h >= MIN_SIZE && g_building_count < MAX_BUTTONS) {
        UiRect *r = &g_building[g_building_count++];
        r->x = x;
        r->y = y;
        r->w = w;
        r->h = h;
    }
    return o_button_logic(id, x, y, w, h);
}

static void __attribute__((cdecl)) h_begin_frame(float dt) {
    EnterCriticalSection(&g_lock);
    memcpy(g_published, g_building, sizeof(UiRect) * (size_t)g_building_count);
    g_published_count = g_building_count;
    LeaveCriticalSection(&g_lock);
    g_building_count = 0;
    o_begin_frame(dt);
}

int ui_nav_find(float cx, float cy, int dx, int dy, float *tx, float *ty) {
    if (!o_button_logic) return 0;
    int found = 0;
    float best = 1e30f;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_published_count; i++) {
        const UiRect *r = &g_published[i];
        float bx = r->x + r->w * 0.5f, by = r->y + r->h * 0.5f;
        float vx = bx - cx, vy = by - cy;
        float score;
        if (dx == 0 && dy == 0) {
            score = vx * vx + vy * vy;
        } else {
            float along = vx * dx + vy * dy;          /* distance in the pressed direction */
            float perp = fabsf(vx * dy - vy * dx);   /* sideways offset */
            if (along <= 4.0f) continue;              /* behind or level with the cursor */
            if (perp > along * 2.5f) continue;        /* too far off to the side */
            score = along + perp * 2.0f;
        }
        if (score < best) {
            best = score;
            *tx = bx;
            *ty = by;
            found = 1;
        }
    }
    LeaveCriticalSection(&g_lock);
    return found;
}

void ui_nav_install(elf_module_t *mod) {
    InitializeCriticalSection(&g_lock);
    /* beginFrame first: buttonLogic is only hooked if both are available */
    o_begin_frame = (BeginFrameFn)hook_got(mod, "_ZN2xt5Imgui10beginFrameEf", (void *)h_begin_frame);
    if (o_begin_frame) {
        o_button_logic = (ButtonLogicFn)hook_got(mod, "_ZN2xt5Imgui11buttonLogicEjiiii", (void *)h_button_logic);
    }
    if (!o_button_logic) printf("[UI] Imgui hooks not found; D-pad menu navigation disabled\n");
}
