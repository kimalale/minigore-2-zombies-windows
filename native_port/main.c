#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <windows.h>
#include <math.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>

#ifndef BUILD_ID
#define BUILD_ID "dev"
#endif

#include "elf_loader.h"
#include "shims.h"
#include "gl_wrap.h"
#include "egl_wrap.h"
#include "android_ndk.h"
#include "opensles.h"
#include "watchdog.h"
#include "log.h"
#include "java_stubs.h"
#include "ai_core.h"
#include "ai_hooks.h"
#include "gun_hooks.h"
#include "net_host.h"
#include "coop.h"
#include "guest.h"

typedef void (*android_main_fn)(struct android_app *state);

static SDL_Window *g_window = NULL;
static HANDLE g_game_thread = NULL;
static bool g_running = true;
static int g_win_w = 1280;
static int g_win_h = 720;

/* Virtual touch joystick state */
static bool g_wasd_active = false;
static float g_wasd_x = 0.0f;
static float g_wasd_y = 0.0f;

static bool g_aim_active = false;
static float g_aim_x = 0.0f;
static float g_aim_y = 0.0f;

static bool g_mouse_active = false;
static float g_mouse_x = 0.0f;
static float g_mouse_y = 0.0f;

static SDL_GameController *g_gamepad = NULL;

/* Mouse aim: while a level is in play, holding the left button aims the
 * virtual aim stick from the player towards the cursor (and so fires). */
static bool g_mouse_aim_firing = false;
static float g_mouse_aim_dx = 1.0f;
static float g_mouse_aim_dy = 0.0f;
static GunConfig g_guns;

/* Online co-op */
static int g_coop_host = 0;
static int g_coop_join = 0;
static NetHostConfig g_host_cfg;
static GuestConfig g_guest_cfg;

/* Xbox / XInput controller */
#define PAD_DEADZONE        0.22f   /* radial, as a share of full deflection */
#define PAD_TRIGGER_FIRE    0.35f   /* right trigger fires along the last aim */
#define PAD_CURSOR_SPEED    1.1f    /* window heights per second at full tilt */
#define PAD_CURSOR_TIMEOUT  6000    /* ms of inactivity before the cursor hides */
static float g_pad_cursor_x = -1.0f, g_pad_cursor_y = -1.0f;
static Uint32 g_pad_cursor_active_at = 0;
static bool g_pad_touching = false;     /* A held on the menu cursor */
static float g_pad_aim_dx = 0.0f, g_pad_aim_dy = -1.0f; /* last aim, for trigger fire */

/* Radial deadzone with rescaling, so small tilts move slowly and there is
 * no square "snap" at the axes. Returns the magnitude (0 = inside deadzone). */
static float pad_stick(SDL_GameControllerAxis ax_x, SDL_GameControllerAxis ax_y, float *ox, float *oy) {
    *ox = *oy = 0.0f;
    if (!g_gamepad) return 0.0f;
    float x = SDL_GameControllerGetAxis(g_gamepad, ax_x) / 32767.0f;
    float y = SDL_GameControllerGetAxis(g_gamepad, ax_y) / 32767.0f;
    float m = sqrtf(x * x + y * y);
    if (m < PAD_DEADZONE) return 0.0f;
    float scaled = (m - PAD_DEADZONE) / (1.0f - PAD_DEADZONE);
    if (scaled > 1.0f) scaled = 1.0f;
    *ox = x / m * scaled;
    *oy = y / m * scaled;
    return scaled;
}

static float pad_trigger(SDL_GameControllerAxis axis) {
    return g_gamepad ? SDL_GameControllerGetAxis(g_gamepad, axis) / 32767.0f : 0.0f;
}

/* Video settings (overridable from the command line) */
typedef struct {
    int width;
    int height;
    int fullscreen;
    int msaa;        /* MSAA samples, 0 = off */
    int anisotropy;  /* anisotropic filtering level, <= 1 = off */
} video_config_t;

static void print_usage(const char *exe) {
    printf("Usage: %s [options]\n", exe);
    printf("  --width <px>      Window width  (default 1280)\n");
    printf("  --height <px>     Window height (default 720)\n");
    printf("  --fullscreen      Start in borderless fullscreen at desktop resolution\n");
    printf("  --msaa <n>        Multisample anti-aliasing samples: 0, 2, 4, 8 (default 4)\n");
    printf("  --aniso <n>       Anisotropic filtering level: 0-16 (default 16)\n");
    printf("  --ai <mode>       Zombie AI: smart (default) or classic\n");
    printf("  --ai-encircle <f> Share of zombies that surround the player, 0-1 (default 0.75)\n");
    printf("  --ai-lead <f>     How far ahead of a moving player zombies aim, 0-1 (default 0.8)\n");
    printf("  --ai-spread <f>   How strongly zombies spread out from each other, 0-2 (default 0.4)\n");
    printf("  --ai-no-throwers  Throwers walk straight in as before\n");
    printf("  --ai-no-bosses    Bosses and minions don't coordinate\n");
    printf("  --no-mouse-aim    Left mouse button acts as a touch, as before\n");
    printf("  --gun-kick <f>    Camera recoil strength, 0-2 (default 1)\n");
    printf("  --gun-damage <f>  Player gun damage multiplier, 0.5-3 (default 1.2)\n");
    printf("  --gun-ammo <f>    Ammo per pickup multiplier, 0.5-3 (default 1.3)\n");
    printf("  --no-smart-aim    Original aim assist (nearest enemy)\n");
    printf("Online co-op (player 2 joins over the network):\n");
    printf("  --host [port]     Host a game; a friend can join as player 2 (default port %d)\n", NET_DEFAULT_PORT);
    printf("  --join <ip[:port]> Join a friend's game as player 2\n");
    printf("  --code <text>     Join code both players must use (optional)\n");
    printf("  --coop-width <px> Video width streamed to player 2 (default 960)\n");
    printf("  --coop-fps <n>    Video frame rate streamed to player 2 (default 30)\n");
    printf("  --coop-quality <n> JPEG quality 10-95 (default 60; lower for slow connections)\n");
    printf("In game: F11 or Alt+Enter toggles fullscreen.\n");
}

static int parse_video_config(int argc, char *argv[], video_config_t *cfg) {
    AiConfig ai;
    ai_config_default(&ai);
    gun_config_default(&g_guns);
    net_host_config_default(&g_host_cfg);
    memset(&g_guest_cfg, 0, sizeof(g_guest_cfg));
    g_guest_cfg.port = NET_DEFAULT_PORT;
    cfg->width = 1280;
    cfg->height = 720;
    cfg->fullscreen = 0;
    cfg->msaa = 4;
    cfg->anisotropy = 16;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        int has_val = (i + 1 < argc);
        if (strcmp(arg, "--width") == 0 && has_val) {
            cfg->width = atoi(argv[++i]);
        } else if (strcmp(arg, "--height") == 0 && has_val) {
            cfg->height = atoi(argv[++i]);
        } else if (strcmp(arg, "--fullscreen") == 0) {
            cfg->fullscreen = 1;
        } else if (strcmp(arg, "--msaa") == 0 && has_val) {
            cfg->msaa = atoi(argv[++i]);
        } else if (strcmp(arg, "--aniso") == 0 && has_val) {
            cfg->anisotropy = atoi(argv[++i]);
        } else if (strcmp(arg, "--ai") == 0 && has_val) {
            const char *mode = argv[++i];
            if (strcmp(mode, "classic") == 0) {
                ai.enabled = 0;
            } else if (strcmp(mode, "smart") == 0) {
                ai.enabled = 1;
            } else {
                fprintf(stderr, "Unknown AI mode: %s (use smart or classic)\n", mode);
                return 0;
            }
        } else if (strcmp(arg, "--ai-encircle") == 0 && has_val) {
            ai.flank_fraction = (float)atof(argv[++i]);
        } else if (strcmp(arg, "--ai-lead") == 0 && has_val) {
            ai.lead = (float)atof(argv[++i]);
        } else if (strcmp(arg, "--ai-spread") == 0 && has_val) {
            ai.separation = (float)atof(argv[++i]);
        } else if (strcmp(arg, "--ai-no-throwers") == 0) {
            ai.ranged_tactics = 0;
        } else if (strcmp(arg, "--ai-no-bosses") == 0) {
            ai.boss_tactics = 0;
        } else if (strcmp(arg, "--no-mouse-aim") == 0) {
            g_guns.mouse_aim = 0;
        } else if (strcmp(arg, "--gun-kick") == 0 && has_val) {
            g_guns.kick = (float)atof(argv[++i]);
        } else if (strcmp(arg, "--gun-damage") == 0 && has_val) {
            g_guns.damage = (float)atof(argv[++i]);
        } else if (strcmp(arg, "--gun-ammo") == 0 && has_val) {
            g_guns.ammo = (float)atof(argv[++i]);
        } else if (strcmp(arg, "--no-smart-aim") == 0) {
            g_guns.smart_autoaim = 0;
        } else if (strcmp(arg, "--host") == 0) {
            g_coop_host = 1;
            if (has_val && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') g_host_cfg.port = atoi(argv[++i]);
        } else if (strcmp(arg, "--join") == 0 && has_val) {
            g_coop_join = 1;
            const char *target = argv[++i];
            const char *colon = strrchr(target, ':');
            size_t n = colon ? (size_t)(colon - target) : strlen(target);
            if (n >= sizeof(g_guest_cfg.host)) n = sizeof(g_guest_cfg.host) - 1;
            memcpy(g_guest_cfg.host, target, n);
            g_guest_cfg.host[n] = '\0';
            if (colon) g_guest_cfg.port = atoi(colon + 1);
        } else if (strcmp(arg, "--code") == 0 && has_val) {
            strncpy(g_host_cfg.code, argv[i + 1], NET_CODE_LEN - 1);
            strncpy(g_guest_cfg.code, argv[++i], NET_CODE_LEN - 1);
        } else if (strcmp(arg, "--coop-width") == 0 && has_val) {
            g_host_cfg.width = atoi(argv[++i]);
        } else if (strcmp(arg, "--coop-fps") == 0 && has_val) {
            g_host_cfg.fps = atoi(argv[++i]);
        } else if (strcmp(arg, "--coop-quality") == 0 && has_val) {
            g_host_cfg.quality = atoi(argv[++i]);
        } else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown or incomplete option: %s\n", arg);
            print_usage(argv[0]);
            return 0;
        }
    }

    if (cfg->width < 640) cfg->width = 640;
    if (cfg->height < 360) cfg->height = 360;
    if (cfg->msaa < 0) cfg->msaa = 0;
    if (cfg->msaa > 16) cfg->msaa = 16;
    if (cfg->anisotropy > 16) cfg->anisotropy = 16;
    
    if (ai.flank_fraction < 0.0f) ai.flank_fraction = 0.0f;
    if (ai.flank_fraction > 1.0f) ai.flank_fraction = 1.0f;
    if (ai.lead < 0.0f) ai.lead = 0.0f;
    if (ai.lead > 1.0f) ai.lead = 1.0f;
    if (ai.separation < 0.0f) ai.separation = 0.0f;
    if (ai.separation > 2.0f) ai.separation = 2.0f;
    ai_set_config(&ai);
    
    if (g_guns.kick < 0.0f) g_guns.kick = 0.0f;
    if (g_guns.kick > 2.0f) g_guns.kick = 2.0f;
    if (g_guns.damage < 0.5f) g_guns.damage = 0.5f;
    if (g_guns.damage > 3.0f) g_guns.damage = 3.0f;
    if (g_guns.ammo < 0.5f) g_guns.ammo = 0.5f;
    if (g_guns.ammo > 3.0f) g_guns.ammo = 3.0f;
    
    if (g_coop_host && g_coop_join) {
        fprintf(stderr, "Use either --host or --join, not both\n");
        return 0;
    }
    if (g_host_cfg.port <= 0 || g_host_cfg.port > 65535) g_host_cfg.port = NET_DEFAULT_PORT;
    if (g_guest_cfg.port <= 0 || g_guest_cfg.port > 65535) g_guest_cfg.port = NET_DEFAULT_PORT;
    if (g_host_cfg.width < 320) g_host_cfg.width = 320;
    if (g_host_cfg.width > 1920) g_host_cfg.width = 1920;
    if (g_host_cfg.fps < 5) g_host_cfg.fps = 5;
    if (g_host_cfg.fps > 60) g_host_cfg.fps = 60;
    if (g_host_cfg.quality < 10) g_host_cfg.quality = 10;
    if (g_host_cfg.quality > 95) g_host_cfg.quality = 95;
    return 1;
}

static void toggle_fullscreen(void) {
    Uint32 flags = SDL_GetWindowFlags(g_window);
    int is_full = (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
    SDL_SetWindowFullscreen(g_window, is_full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
}

/* Touch emulation with stable finger IDs. The game tracks each finger by
 * its ID from DOWN to UP and crashes on a MOVE for an ID it never saw go
 * down. IDs used to follow list position, so e.g. releasing the move stick
 * while aiming turned the aim finger into the just-released ID 0 - a
 * normal "stop walking while shooting" crashed the game. */
enum { FINGER_MOVE = 0, FINGER_AIM = 1, FINGER_TAP = 2, FINGER_COUNT = 3 };
static bool g_finger_down[FINGER_COUNT];
static float g_finger_x[FINGER_COUNT], g_finger_y[FINGER_COUNT];

/* Fingers that are down (plus `extra`, if >= 0), in ID order. Returns the
 * count; *index gets the position of `extra` in the list. */
static int finger_list(int extra, int *ids, float *xs, float *ys, int *index) {
    int n = 0;
    for (int f = 0; f < FINGER_COUNT; f++) {
        if (!g_finger_down[f] && f != extra) continue;
        if (f == extra && index) *index = n;
        ids[n] = f;
        xs[n] = g_finger_x[f];
        ys[n] = g_finger_y[f];
        n++;
    }
    return n;
}

static void finger_move_all(void) {
    int ids[FINGER_COUNT];
    float xs[FINGER_COUNT], ys[FINGER_COUNT];
    int n = finger_list(-1, ids, xs, ys, NULL);
    if (n > 0) android_app_push_touch(AMOTION_EVENT_ACTION_MOVE, n, ids, xs, ys);
}

static void finger_set(int finger, bool down, float x, float y) {
    int ids[FINGER_COUNT], index = 0;
    float xs[FINGER_COUNT], ys[FINGER_COUNT];
    g_finger_x[finger] = x;
    g_finger_y[finger] = y;
    if (down == g_finger_down[finger]) {
        if (down) finger_move_all();
        return;
    }
    int n = finger_list(finger, ids, xs, ys, &index);
    int action;
    if (down) {
        action = n == 1 ? AMOTION_EVENT_ACTION_DOWN
                        : AMOTION_EVENT_ACTION_POINTER_DOWN | (index << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    } else {
        action = n == 1 ? AMOTION_EVENT_ACTION_UP
                        : AMOTION_EVENT_ACTION_POINTER_UP | (index << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    }
    g_finger_down[finger] = down;
    android_app_push_touch(action, n, ids, xs, ys);
}

static void update_touch_events(void) {
    if (g_mouse_active) {
        g_finger_x[FINGER_TAP] = g_mouse_x;
        g_finger_y[FINGER_TAP] = g_mouse_y;
    }
    finger_move_all();
}

static void touch_press(float x, float y) {
    g_mouse_active = true;
    g_mouse_x = x;
    g_mouse_y = y;
    finger_set(FINGER_TAP, true, x, y);
}

static void touch_release(float x, float y) {
    g_mouse_active = false;
    g_mouse_x = x;
    g_mouse_y = y;
    finger_set(FINGER_TAP, false, x, y);
}

static void update_wasd_stick(void) {
    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    float dx = 0.0f;
    float dy = 0.0f;
    
    if (keys[SDL_SCANCODE_W]) dy -= 1.0f;
    if (keys[SDL_SCANCODE_S]) dy += 1.0f;
    if (keys[SDL_SCANCODE_A]) dx -= 1.0f;
    if (keys[SDL_SCANCODE_D]) dx += 1.0f;
    
    /* Gamepad left stick / D-pad move the player (in menus they drive the cursor) */
    if (g_gamepad && gun_in_level()) {
        float x, y;
        if (pad_stick(SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, &x, &y) > 0.0f) {
            dx = x;
            dy = y;
        }
        if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_DPAD_UP)) dy -= 1.0f;
        if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) dy += 1.0f;
        if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) dx -= 1.0f;
        if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) dx += 1.0f;
    }
    
    float len = sqrtf(dx * dx + dy * dy);
    if (len > 0.001f) {
        if (len > 1.0f) {
            dx /= len;
            dy /= len;
        }
        
        float base_x = (float)g_win_w * 0.18f;
        float base_y = (float)g_win_h * 0.72f;
        float radius = (float)g_win_w * 0.09f;
        
        g_wasd_x = base_x + dx * radius;
        g_wasd_y = base_y + dy * radius;
        
        g_wasd_active = true;
        finger_set(FINGER_MOVE, true, g_wasd_x, g_wasd_y);
    } else if (g_wasd_active) {
        g_wasd_active = false;
        finger_set(FINGER_MOVE, false, g_wasd_x, g_wasd_y);
    }
}

static void update_aim_stick(void) {
    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    float dx = 0.0f;
    float dy = 0.0f;
    
    if (keys[SDL_SCANCODE_UP]    || keys[SDL_SCANCODE_I]) dy -= 1.0f;
    if (keys[SDL_SCANCODE_DOWN]  || keys[SDL_SCANCODE_K]) dy += 1.0f;
    if (keys[SDL_SCANCODE_LEFT]  || keys[SDL_SCANCODE_J]) dx -= 1.0f;
    if (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_L]) dx += 1.0f;
    
    /* Gamepad: right stick aims and fires; right trigger fires along the
     * last aim direction (or the way you're moving) without the stick */
    if (g_gamepad && gun_in_level()) {
        float x, y;
        if (pad_stick(SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY, &x, &y) > 0.0f) {
            float l = sqrtf(x * x + y * y);
            g_pad_aim_dx = x / l;
            g_pad_aim_dy = y / l;
            dx = g_pad_aim_dx; /* full deflection: direction is what matters */
            dy = g_pad_aim_dy;
        } else if (pad_trigger(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > PAD_TRIGGER_FIRE) {
            float mx, my;
            /* Fire the way you're running, unless LT is held to strafe */
            if (pad_stick(SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, &mx, &my) > 0.3f &&
                pad_trigger(SDL_CONTROLLER_AXIS_TRIGGERLEFT) < PAD_TRIGGER_FIRE) {
                float l = sqrtf(mx * mx + my * my);
                g_pad_aim_dx = mx / l;
                g_pad_aim_dy = my / l;
            }
            dx = g_pad_aim_dx;
            dy = g_pad_aim_dy;
        }
    }
    
    /* Mouse aim: point the stick from the player towards the cursor */
    if (g_mouse_aim_firing) {
        float px, py;
        if (gun_player_screen_pos(&px, &py)) {
            int mx, my;
            SDL_GetMouseState(&mx, &my);
            float vx = (float)mx - px, vy = (float)my - py;
            float vl = sqrtf(vx * vx + vy * vy);
            if (vl > 8.0f) { /* keep the last direction when the cursor is on the player */
                g_mouse_aim_dx = vx / vl;
                g_mouse_aim_dy = vy / vl;
            }
            dx = g_mouse_aim_dx;
            dy = g_mouse_aim_dy;
        } else {
            g_mouse_aim_firing = false; /* level ended or paused */
        }
    }
    
    float len = sqrtf(dx * dx + dy * dy);
    if (len > 0.001f) {
        if (len > 1.0f) {
            dx /= len;
            dy /= len;
        }
        
        float base_x = (float)g_win_w * 0.82f;
        float base_y = (float)g_win_h * 0.72f;
        float radius = (float)g_win_w * 0.09f;
        
        g_aim_x = base_x + dx * radius;
        g_aim_y = base_y + dy * radius;
        
        g_aim_active = true;
        finger_set(FINGER_AIM, true, g_aim_x, g_aim_y);
    } else if (g_aim_active) {
        g_aim_active = false;
        finger_set(FINGER_AIM, false, g_aim_x, g_aim_y);
    }
}

/* Vectored handler: runs first for every exception on every thread,
 * including the many harmless first-chance ones graphics drivers raise and
 * catch themselves (thread naming, C++ exceptions, OutputDebugString,
 * memory probes), sometimes while holding the loader lock. So it does no
 * I/O: it only commits stack pages on demand and otherwise passes. */
static LONG WINAPI stack_commit_handler(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR fault_addr = ep->ExceptionRecord->ExceptionInformation[1];
        MEMORY_BASIC_INFORMATION mbi, own_stack;
        /* Only inside the faulting thread's own stack reservation */
        if (VirtualQuery((void *)fault_addr, &mbi, sizeof(mbi)) &&
            VirtualQuery(&own_stack, &own_stack, sizeof(own_stack)) &&
            mbi.AllocationBase == own_stack.AllocationBase &&
            mbi.State == MEM_RESERVE) {
            void *page = (void *)(fault_addr & ~0xFFF);
            VirtualAlloc(page, 64 * 1024, MEM_COMMIT, PAGE_READWRITE);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Only exceptions nobody handled reach here: a real crash */
static LONG WINAPI crash_handler(EXCEPTION_POINTERS *ep) {
    char where[512];
    watchdog_describe_address((uint32_t)(uintptr_t)ep->ExceptionRecord->ExceptionAddress, where, sizeof(where));
    FILE *outs[2] = { stdout, fopen("crash_report.txt", "w") };
    for (int o = 0; o < 2; o++) {
        FILE *f = outs[o];
        if (!f) continue;
        fprintf(f, "\n================ CRASH ================\n");
        fprintf(f, "Exception 0x%08lX at %p  %s\n", ep->ExceptionRecord->ExceptionCode,
                ep->ExceptionRecord->ExceptionAddress, where);
        if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
            fprintf(f, "Access violation: %s at %p\n",
                    ep->ExceptionRecord->ExceptionInformation[0] == 0 ? "read" :
                    ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "write" : "execute",
                    (void *)ep->ExceptionRecord->ExceptionInformation[1]);
        }
        PCONTEXT ctx = ep->ContextRecord;
        fprintf(f, "EAX %08lX EBX %08lX ECX %08lX EDX %08lX ESI %08lX EDI %08lX EBP %08lX ESP %08lX\n",
                ctx->Eax, ctx->Ebx, ctx->Ecx, ctx->Edx, ctx->Esi, ctx->Edi, ctx->Ebp, ctx->Esp);
        fflush(f);
        if (o == 1) fclose(f);
    }
    printf("[Crash] Details written to crash_report.txt\n");
    log_flush();
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI game_thread_proc(LPVOID data) {
    android_main_fn p_main = (android_main_fn)data;
    printf("[Thread] Entering android_main...\n");
    fflush(stdout);

    /* Commit entire thread stack allocation upfront */
    MEMORY_BASIC_INFORMATION mbi;
    char *p = (char *)&mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi))) {
        if (mbi.AllocationBase) {
            SIZE_T size_to_commit = (SIZE_T)(p - (char *)mbi.AllocationBase);
            VirtualAlloc(mbi.AllocationBase, size_to_commit, MEM_COMMIT, PAGE_READWRITE);
        }
    }
    
    p_main(android_app_get());
    
    printf("[Thread] android_main returned.\n");
    fflush(stdout);
    g_running = false;
    return 0;
}

int main(int argc, char *argv[]) {
    /* Everything printed also goes to game_log.txt; the console can never
     * stall the game */
    log_init("game_log.txt");
    AddVectoredExceptionHandler(1, stack_commit_handler);
    SetUnhandledExceptionFilter(crash_handler);
    
    video_config_t video;
    if (!parse_video_config(argc, argv, &video)) {
        return 1;
    }
    g_win_w = video.width;
    g_win_h = video.height;
    
    /* Render at true pixel resolution instead of letting Windows
     * bitmap-stretch the window on high-DPI displays. */
    SetProcessDPIAware();
    
    /* Player 2 doesn't run the game: it shows the host's stream */
    if (g_coop_join) {
        return guest_main(&g_guest_cfg);
    }
    
    printf("====================================================\n");
    printf("  Minigore 2: Zombies - Native Windows Recompilation\n");
    printf("  Build %s, compiled %s %s\n", BUILD_ID, __DATE__, __TIME__);
    printf("  Direct Android x86 ELF Loader under WOW64\n");
    printf("====================================================\n\n");
    fflush(stdout);
    
    /* Ensure saves folder exists */
    CreateDirectoryA("saves", NULL);
    
    /* Initialize SDL */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) < 0) {
        fprintf(stderr, "Failed to initialize SDL: %s\n", SDL_GetError());
        return 1;
    }
    
    /* Configure OpenGL 2.1 Compatibility Context */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    /* The pixel format (and so MSAA) is fixed when the window is created;
     * the GL context itself is created later on the game thread. */
    if (video.msaa > 0) {
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, video.msaa);
    }
    
    Uint32 window_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
    if (video.fullscreen) {
        window_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }
    
    g_window = SDL_CreateWindow(
        "Minigore 2: Zombies",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        g_win_w, g_win_h,
        window_flags
    );
    if (!g_window && video.msaa > 0) {
        fprintf(stderr, "MSAA x%d unavailable (%s), retrying without it\n", video.msaa, SDL_GetError());
        video.msaa = 0;
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
        g_window = SDL_CreateWindow(
            "Minigore 2: Zombies",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            g_win_w, g_win_h,
            window_flags
        );
    }
    
    if (!g_window) {
        fprintf(stderr, "Failed to create SDL window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    
    /* Fullscreen windows take the desktop size */
    SDL_GetWindowSize(g_window, &g_win_w, &g_win_h);
    printf("[Host] Render resolution %dx%d%s, MSAA x%d, anisotropy %dx\n",
           g_win_w, g_win_h, video.fullscreen ? " (fullscreen)" : "", video.msaa,
           video.anisotropy > 1 ? video.anisotropy : 1);
    
    /* Initialize host wrapper subsystems */
    shims_init();
    gl_wrap_set_anisotropy(video.anisotropy);
    egl_wrap_init(g_window);
    egl_wrap_set_msaa(video.msaa > 0);
    opensles_init();
    android_ndk_init(g_win_w, g_win_h);
    
    /* Load libminigore2.so */
    elf_module_t mod;
    size_t sym_count = 0;
    const host_symbol_t *syms = shims_get_table(&sym_count);
    
    printf("[Host] Loading lib/x86/libminigore2.so...\n");
    fflush(stdout);
    
    if (elf_load("lib/x86/libminigore2.so", &mod) != 0) {
        fprintf(stderr, "[Host] Failed to load libminigore2.so\n");
        return 1;
    }
    
    printf("[Host] Relocating ELF symbols...\n");
    fflush(stdout);
    
    if (elf_relocate(&mod, syms, sym_count) != 0) {
        fprintf(stderr, "[Host] Failed to relocate ELF module\n");
        return 1;
    }
    
    printf("[Host] Executing ELF constructors...\n");
    fflush(stdout);
    elf_init(&mod);
    
    java_stubs_install(&mod);
    ai_hooks_install(&mod);
    gun_hooks_install(&mod, &g_guns);
    if (g_coop_host && net_host_start(&g_host_cfg) == 0) {
        coop_install(&mod);
    }
    watchdog_start(&mod);
    watchdog_register_thread(GetCurrentThread(), "main");
    
    /* Find android_main entry point */
    android_main_fn p_android_main = (android_main_fn)elf_sym(&mod, "android_main");
    if (!p_android_main) {
        fprintf(stderr, "[Host] Symbol 'android_main' not found in module!\n");
        return 1;
    }
    
    /* Push initial Android lifecycle commands so they are waiting when android_main begins polling */
    printf("[Host] Queueing Android lifecycle commands (START, INIT_WINDOW, RESUME, GAINED_FOCUS)...\n");
    fflush(stdout);
    android_app_push_cmd(APP_CMD_START);
    android_app_push_cmd(APP_CMD_INIT_WINDOW);
    android_app_push_cmd(APP_CMD_RESUME);
    android_app_push_cmd(APP_CMD_GAINED_FOCUS);
    
    printf("[Host] Starting game thread with 16MB committed stack...\n");
    fflush(stdout);
    g_game_thread = CreateThread(NULL, 16 * 1024 * 1024, game_thread_proc, (void *)p_android_main, 0, NULL);
    if (!g_game_thread) {
        fprintf(stderr, "[Host] Failed to create game thread: %lu\n", GetLastError());
        return 1;
    }
    watchdog_register_thread(g_game_thread, "game");
    
    /* Main Event Loop */
    SDL_Event ev;
    while (g_running) {
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_QUIT:
                    printf("[Host] Quit event received.\n");
                    g_running = false;
                    break;
                    
                case SDL_WINDOWEVENT:
                    /* SIZE_CHANGED covers user resizes and fullscreen toggles */
                    if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                        g_win_w = ev.window.data1;
                        g_win_h = ev.window.data2;
                        android_app_set_window_size(g_win_w, g_win_h);
                        android_app_push_cmd(APP_CMD_WINDOW_RESIZED);
                    } else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                        watchdog_set_focused(1);
                        android_app_push_cmd(APP_CMD_GAINED_FOCUS);
                    } else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                        watchdog_set_focused(0);
                        android_app_push_cmd(APP_CMD_LOST_FOCUS);
                    }
                    break;
                    
                case SDL_MOUSEBUTTONDOWN:
                    if (ev.button.button == SDL_BUTTON_LEFT && !g_mouse_active) {
                        float px, py;
                        if (gun_player_screen_pos(&px, &py)) {
                            g_mouse_aim_firing = true;
                            update_aim_stick();
                            break;
                        }
                    }
                    /* Left button in menus, right button anywhere: touch the screen */
                    if ((ev.button.button == SDL_BUTTON_LEFT || ev.button.button == SDL_BUTTON_RIGHT) && !g_mouse_active) {
                        touch_press((float)ev.button.x, (float)ev.button.y);
                    }
                    break;
                    
                case SDL_MOUSEBUTTONUP:
                    if (ev.button.button == SDL_BUTTON_LEFT && g_mouse_aim_firing) {
                        g_mouse_aim_firing = false;
                        update_aim_stick();
                        break;
                    }
                    if ((ev.button.button == SDL_BUTTON_LEFT || ev.button.button == SDL_BUTTON_RIGHT) &&
                        g_mouse_active && !g_pad_touching) {
                        touch_release((float)ev.button.x, (float)ev.button.y);
                    }
                    break;
                    
                case SDL_MOUSEMOTION:
                    if (g_mouse_active && !g_pad_touching) {
                        g_mouse_x = (float)ev.motion.x;
                        g_mouse_y = (float)ev.motion.y;
                        update_touch_events();
                    }
                    break;
                    
                case SDL_KEYDOWN:
                case SDL_KEYUP: {
                    int action = (ev.type == SDL_KEYDOWN) ? AKEY_EVENT_ACTION_DOWN : AKEY_EVENT_ACTION_UP;
                    
                    /* F11 / Alt+Enter: toggle fullscreen (not forwarded to the game) */
                    if (ev.key.keysym.sym == SDLK_F11 ||
                        (ev.key.keysym.sym == SDLK_RETURN && (ev.key.keysym.mod & KMOD_ALT))) {
                        if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
                            toggle_fullscreen();
                        }
                        break;
                    }
                    
                    /* Update WASD virtual movement */
                    if (ev.key.keysym.scancode == SDL_SCANCODE_W ||
                        ev.key.keysym.scancode == SDL_SCANCODE_A ||
                        ev.key.keysym.scancode == SDL_SCANCODE_S ||
                        ev.key.keysym.scancode == SDL_SCANCODE_D) {
                        update_wasd_stick();
                    }
                    
                    /* Update Aim virtual stick (Arrow keys / IJKL) */
                    if (ev.key.keysym.scancode == SDL_SCANCODE_UP ||
                        ev.key.keysym.scancode == SDL_SCANCODE_DOWN ||
                        ev.key.keysym.scancode == SDL_SCANCODE_LEFT ||
                        ev.key.keysym.scancode == SDL_SCANCODE_RIGHT ||
                        ev.key.keysym.scancode == SDL_SCANCODE_I ||
                        ev.key.keysym.scancode == SDL_SCANCODE_K ||
                        ev.key.keysym.scancode == SDL_SCANCODE_J ||
                        ev.key.keysym.scancode == SDL_SCANCODE_L) {
                        update_aim_stick();
                    }
                    
                    /* Back button / ESC */
                    if (ev.key.keysym.sym == SDLK_ESCAPE) {
                        android_app_push_key_event(action, AKEYCODE_BACK);
                    } else if (ev.key.keysym.sym == SDLK_SPACE || ev.key.keysym.sym == SDLK_RETURN) {
                        android_app_push_key_event(action, AKEYCODE_BUTTON_A);
                    }
                    break;
                }
                
                case SDL_CONTROLLERDEVICEADDED:
                    if (!g_gamepad) {
                        g_gamepad = SDL_GameControllerOpen(ev.cdevice.which);
                        if (g_gamepad) {
                            printf("[Host] Gamepad connected: %s\n", SDL_GameControllerName(g_gamepad));
                            fflush(stdout);
                        }
                    }
                    break;
                    
                case SDL_CONTROLLERDEVICEREMOVED:
                    if (g_gamepad) {
                        SDL_GameControllerClose(g_gamepad);
                        g_gamepad = NULL;
                        printf("[Host] Gamepad disconnected.\n");
                        fflush(stdout);
                    }
                    break;
                    
                case SDL_CONTROLLERAXISMOTION:
                    break; /* sticks are polled every loop */
                    
                case SDL_CONTROLLERBUTTONDOWN:
                case SDL_CONTROLLERBUTTONUP: {
                    bool down = (ev.type == SDL_CONTROLLERBUTTONDOWN);
                    int action = down ? AKEY_EVENT_ACTION_DOWN : AKEY_EVENT_ACTION_UP;
                    g_pad_cursor_active_at = SDL_GetTicks();
                    switch (ev.cbutton.button) {
                        case SDL_CONTROLLER_BUTTON_A:
                            /* Menus: tap at the cursor. In a level: the game's A button. */
                            if (down && !gun_in_level() && !g_mouse_active) {
                                g_pad_touching = true;
                                touch_press(g_pad_cursor_x, g_pad_cursor_y);
                            } else if (!down && g_pad_touching) {
                                g_pad_touching = false;
                                touch_release(g_pad_cursor_x, g_pad_cursor_y);
                            } else if (gun_in_level()) {
                                android_app_push_key_event(action, AKEYCODE_BUTTON_A);
                            }
                            break;
                        case SDL_CONTROLLER_BUTTON_B:
                        case SDL_CONTROLLER_BUTTON_BACK:
                        case SDL_CONTROLLER_BUTTON_START:
                            /* Back in menus, pause in a level */
                            android_app_push_key_event(action, AKEYCODE_BACK);
                            break;
                        default:
                            break;
                    }
                    break;
                }
                
                default:
                    break;
            }
        }
        
        /* Keep the mouse-aim direction tracking the player as they move */
        if (g_mouse_aim_firing) {
            update_aim_stick();
        }
        
        if (g_gamepad) {
            bool in_level = gun_in_level();
            /* Sticks and triggers are polled every loop so held input and the
             * switch between menus and levels are handled */
            update_wasd_stick();
            update_aim_stick();
            
            /* Menu cursor: left stick or D-pad */
            if (g_pad_cursor_x < 0.0f) {
                g_pad_cursor_x = g_win_w * 0.5f;
                g_pad_cursor_y = g_win_h * 0.5f;
            }
            if (!in_level) {
                float x, y;
                pad_stick(SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, &x, &y);
                if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_DPAD_UP)) y -= 0.5f;
                if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) y += 0.5f;
                if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) x -= 0.5f;
                if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) x += 0.5f;
                if (x != 0.0f || y != 0.0f) {
                    /* squared response: fine control near centre, fast at full tilt */
                    float step = PAD_CURSOR_SPEED * (float)g_win_h * 0.010f * 1.6f; /* loop ~10 ms */
                    g_pad_cursor_x += x * fabsf(x) * step;
                    g_pad_cursor_y += y * fabsf(y) * step;
                    if (g_pad_cursor_x < 0) g_pad_cursor_x = 0;
                    if (g_pad_cursor_y < 0) g_pad_cursor_y = 0;
                    if (g_pad_cursor_x > g_win_w - 1) g_pad_cursor_x = (float)(g_win_w - 1);
                    if (g_pad_cursor_y > g_win_h - 1) g_pad_cursor_y = (float)(g_win_h - 1);
                    g_pad_cursor_active_at = SDL_GetTicks();
                    if (g_pad_touching) { /* drag (sliders, scrolling) */
                        g_mouse_x = g_pad_cursor_x;
                        g_mouse_y = g_pad_cursor_y;
                        update_touch_events();
                    }
                }
            } else if (g_pad_touching) {
                g_pad_touching = false; /* level started mid-tap */
                touch_release(g_pad_cursor_x, g_pad_cursor_y);
            }
            bool show = !in_level && g_pad_cursor_active_at != 0 &&
                        SDL_GetTicks() - g_pad_cursor_active_at < PAD_CURSOR_TIMEOUT;
            egl_overlay_set_cursor(g_pad_cursor_x, g_pad_cursor_y, show, g_pad_touching);
            
            float lo, hi;
            unsigned ms;
            if (gun_take_rumble(&lo, &hi, &ms)) {
                SDL_GameControllerRumble(g_gamepad, (Uint16)(lo * 65535.0f), (Uint16)(hi * 65535.0f), ms);
            }
        } else {
            egl_overlay_set_cursor(0, 0, 0, 0);
            float lo, hi;
            unsigned ms;
            gun_take_rumble(&lo, &hi, &ms); /* discard */
        }
        
        if (android_app_is_destroy_requested()) {
            printf("[Host] Game requested destroy.\n");
            g_running = false;
        }
        
        SDL_Delay(10);
    }
    
    printf("[Host] Shutting down application...\n");
    android_app_push_cmd(APP_CMD_LOST_FOCUS);
    android_app_push_cmd(APP_CMD_PAUSE);
    android_app_push_cmd(APP_CMD_TERM_WINDOW);
    android_app_push_cmd(APP_CMD_DESTROY);
    
    if (g_game_thread) {
        WaitForSingleObject(g_game_thread, 2000);
        CloseHandle(g_game_thread);
    }
    
    opensles_shutdown();
    
    egl_wrap_shutdown();
    if (g_gamepad) {
        SDL_GameControllerClose(g_gamepad);
        g_gamepad = NULL;
    }
    
    if (g_window) {
        SDL_DestroyWindow(g_window);
    }
    
    SDL_Quit();
    
    printf("[Host] Exited cleanly.\n");
    log_flush();
    /* End the process even if the game thread didn't stop in time (it is
     * only given 2 s above); returning from main would wait for it. */
    ExitProcess(0);
}
