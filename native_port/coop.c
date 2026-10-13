#include "coop.h"
#include "hook_util.h"
#include "net_host.h"
#include "gun_hooks.h"
#include "gl_wrap.h"
#include "egl_wrap.h"
#include "determinism.h"
#include "lockstep.h"
#include <windows.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Player 2 is a companion character - the game's own allied character,
 * created with PlayerCharacter::createCompanionCharacter, which already
 * has health, weapons, collisions and is targeted by the horde.
 *
 * Its controller (CharacterControlCompanion, vtable slot 2) normally runs
 * the companion AI. While the guest is connected it instead runs the
 * human controller's logic (CharacterControlHuman::update) with the global
 * stick state temporarily swapped for the guest's sticks, then restored,
 * so player 1 is unaffected. For companions the controller's return value
 * decides whether they fire, so the guest's fire button maps directly.
 *
 * Player 2 is (re)spawned from StateGame::update, outside the entity
 * update loop, and is only ever dereferenced after checking it is still
 * listed in the game's character array.
 *
 * Split view (default): each frame StateGame::draw runs a second time,
 * first, into an off-screen framebuffer with the camera moved onto player
 * 2 (the game's binds of framebuffer 0 are redirected there). Player 1's
 * HUD, the menu GUI and the per-frame GUI renderer cleanup are skipped in
 * that pass. The guest is streamed this view, so each player has their
 * own screen and can roam the map; zombies chase whichever player is
 * closer (see ai_hooks.c).
 *
 * Own-world co-op (lockstep.c): both PCs run this same logic on the same
 * inputs, so player 2 spawns, moves and fights identically in both
 * worlds. Everything here that affects the game then runs on the virtual
 * clock. The host plays with its normal single view. The guest's screen
 * follows player 2: StateGame::draw runs first with the camera on player
 * 2, without player 1's HUD and with the random generators restored after
 * it; then again exactly as on the host (same camera, same dt), but into
 * an off-screen framebuffer, with only its HUD / menus routed to the
 * window. The game's own state therefore advances as on the host. */

#define ENV_PLAYER           0x1f4fa0  /* Character* the camera / horde follow */
#define ENV_CHARACTERS       0x1f4f1c  /* Character* [32], player-side characters */
#define ENV_CHARACTER_COUNT  0x1f4f9c
#define ENV_STICKS           0x1f6040  /* move x, y, aim x, y (pad convention) */
#define ENV_SHOOT_STATE      0x1f4f10  /* 12 bytes of player 1 auto-aim shooting state */
#define ENV_AUTO_AIM         0x1f6b3e  /* "Auto Aim" option */
#define MAX_CHARACTERS       32
#define ENTITY_POS           0x04
#define CHAR_TYPE            0x70      /* Characters::Enum */
#define VTBL_HEALTH          0x28

#define SPAWN_OFFSET         60.0f     /* units beside player 1 */
#define RESPAWN_MS           4000
#define INPUT_TIMEOUT_MS     500
#define CAMERA_MAX_SHIFT     260.0f
#define CAMERA_ZOOM_START    250.0f
#define CAMERA_ZOOM_RANGE    900.0f
#define CAMERA_MAX_ZOOM      0.6f

typedef int   (__attribute__((cdecl)) *StateUpdateFn)(void *self, float dt);
typedef int   (__attribute__((cdecl)) *ControlUpdateFn)(void *self, float dt, void *character);
typedef void *(__attribute__((cdecl)) *CreateCompanionFn)(void *player, int type, const float *pos);
typedef int   (__attribute__((cdecl)) *EntityIntFn)(void *entity);
typedef void  (__attribute__((cdecl)) *DrawFn)(void *self, float dt);
typedef void  (__attribute__((cdecl)) *Void0Fn)(void);
typedef void  (__attribute__((cdecl)) *Void1Fn)(void *a);
typedef void  (__attribute__((cdecl)) *Void2Fn)(void *a, void *b);
typedef int   (__attribute__((cdecl)) *IntGetterFn)(const void *self);

typedef void (APIENTRY *PFN_GenFb)(GLsizei, GLuint *);
typedef void (APIENTRY *PFN_BindFb)(GLenum, GLuint);
typedef void (APIENTRY *PFN_GenRb)(GLsizei, GLuint *);
typedef void (APIENTRY *PFN_BindRb)(GLenum, GLuint);
typedef void (APIENTRY *PFN_RbStorage)(GLenum, GLenum, GLsizei, GLsizei);
typedef void (APIENTRY *PFN_FbRb)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum (APIENTRY *PFN_CheckFb)(GLenum);
#define CO_GL_FRAMEBUFFER              0x8D40
#define CO_GL_RENDERBUFFER             0x8D41
#define CO_GL_COLOR_ATTACHMENT0        0x8CE0
#define CO_GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define CO_GL_DEPTH24_STENCIL8         0x88F0
#define CO_GL_FRAMEBUFFER_COMPLETE     0x8CD5

static uint8_t **g_env;
static StateUpdateFn o_state_update;
static ControlUpdateFn o_companion_update;
static ControlUpdateFn g_human_update;
static CreateCompanionFn g_create_companion;

static uint8_t *g_p2 = NULL;
static uint8_t *g_p2_live = NULL;    /* validated each tick for other modules */
static int g_split_view = 1;
static int g_in_p2_pass = 0;
static DrawFn o_draw;
static Void1Fn o_draw_hud, o_draw_tutorials, o_draw_vignette, o_delete_renderers;
static Void0Fn o_draw_arena_score, o_draw_score;
static Void2Fn o_draw_gui, o_draw_score_messages;
static IntGetterFn g_get_health, g_get_max_health;

/* Player 2's off-screen view */
static PFN_GenFb gl_gen_fb;
static PFN_BindFb gl_bind_fb;
static PFN_GenRb gl_gen_rb;
static PFN_BindRb gl_bind_rb;
static PFN_RbStorage gl_rb_storage;
static PFN_FbRb gl_fb_rb;
static PFN_CheckFb gl_check_fb;
static GLuint g_view_fb, g_view_color, g_view_depth;
static int g_view_w, g_view_h, g_view_failed;
static DWORD g_p2_dead_since = 0;
static DWORD g_next_spawn = 0;
static int g_hud_to_window = 0;     /* guest: HUD of the host-identical pass goes to the window */

/* Game-thread time for spawns: virtual during own-world co-op */
static DWORD coop_now_ms(void) {
    return det_active() ? (DWORD)(det_now_ns() / 1000000ULL) : GetTickCount();
}

static int guest_connected(void) {
    return lockstep_active() ? 1 : net_host_guest_connected();
}

static uint8_t *env(void) {
    return g_env ? *g_env : NULL;
}

static int character_listed(uint8_t *e, const uint8_t *c) {
    if (!e || !c) return 0;
    unsigned count = *(const unsigned *)(e + ENV_CHARACTER_COUNT);
    if (count > MAX_CHARACTERS) count = MAX_CHARACTERS;
    uint8_t **list = (uint8_t **)(e + ENV_CHARACTERS);
    for (unsigned i = 0; i < count; i++) {
        if (list[i] == c) return 1;
    }
    return 0;
}

static int character_alive(uint8_t *c) {
    uintptr_t vt = *(uintptr_t *)c;
    return ((EntityIntFn)(*(uintptr_t *)(vt + VTBL_HEALTH)))(c) > 0;
}

/* p2 if it is still a live, listed character; NULL otherwise */
static uint8_t *live_p2(uint8_t *e) {
    if (!g_p2 || !character_listed(e, g_p2)) return NULL;
    return character_alive(g_p2) ? g_p2 : NULL;
}

void coop_set_split_view(int split) {
    g_split_view = split;
}

void *coop_p2_live(void) {
    return g_p2_live;
}

static float p2_health_fraction(uint8_t *p2) {
    if (!g_get_health || !g_get_max_health) return 1.0f;
    int max = g_get_max_health(p2);
    int hp = g_get_health(p2);
    if (max <= 0) return 1.0f;
    float f = (float)hp / (float)max;
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

static void coop_tick(void) {
    uint8_t *e = env();
    uint8_t *player = e ? *(uint8_t **)(e + ENV_PLAYER) : NULL;
    int in_level = gun_in_level();
    DWORD now = coop_now_ms();

    if (g_p2 && !character_listed(e, g_p2)) g_p2 = NULL; /* removed by the game */
    g_p2_live = NULL;

    if (!player || !in_level || !guest_connected()) {
        g_p2_live = NULL;
        if (lockstep_active()) {
            egl_overlay_set_coop(NULL);
            return;
        }
        net_host_set_p2(0.5f, 0.5f, 0, in_level);
        net_host_set_p2_status(0.0f, 0);
        return;
    }

    if (g_p2) {
        if (!character_alive(g_p2)) {
            if (!g_p2_dead_since) {
                g_p2_dead_since = now;
                printf("[Co-op] Player 2 is down; respawning in %d s\n", RESPAWN_MS / 1000);
                fflush(stdout);
            } else if (now - g_p2_dead_since > RESPAWN_MS) {
                g_p2 = NULL; /* leave the body to the game */
                g_next_spawn = now;
            }
        } else {
            g_p2_dead_since = 0;
        }
    }

    if (!g_p2 && (LONG)(now - g_next_spawn) >= 0 && g_create_companion) {
        const float *p = (const float *)(player + ENTITY_POS);
        float pos[2] = { p[0] + SPAWN_OFFSET, p[1] };
        int type = *(const int *)(player + CHAR_TYPE);
        g_p2 = (uint8_t *)g_create_companion(player, type, pos);
        g_p2_dead_since = 0;
        if (g_p2) {
            printf("[Co-op] Player 2 spawned\n");
        } else {
            printf("[Co-op] Could not spawn player 2 (character limit?); retrying\n");
            g_next_spawn = now + 2000;
        }
        fflush(stdout);
    }

    uint8_t *p2 = live_p2(e);
    g_p2_live = p2;
    if (lockstep_active()) return; /* nothing is streamed */
    net_host_set_p2_status(p2 ? p2_health_fraction(p2) : 0.0f, g_split_view && p2 != NULL);
    /* Where player 2 is on screen, so the guest can mouse-aim from it. In
     * split view this is measured during player 2's own render pass. */
    if (!g_split_view || !p2) {
        float sx = 0.5f, sy = 0.5f;
        if (p2) {
            const float *p = (const float *)(p2 + ENTITY_POS);
            gun_project_ground_normalized(p[0], p[1], &sx, &sy);
        }
        net_host_set_p2(sx, sy, p2 != NULL, 1);
    }
}

static int __attribute__((cdecl)) h_state_update(void *self, float dt) {
    coop_tick();
    return o_state_update(self, dt);
}

/* Player 2's input this frame: the guest's sticks (own-world co-op, the
 * same on both PCs) or the latest streamed input */
static int p2_input(MsgInput *in) {
    if (!lockstep_active()) return net_host_get_input(in, INPUT_TIMEOUT_MS);
    float s[4];
    if (!lockstep_p2_sticks(s)) return 0;
    in->move_x = s[0];
    in->move_y = s[1];
    in->aim_x = s[2];
    in->aim_y = s[3];
    in->fire = 1; /* twin-stick: aiming fires */
    in->buttons = 0;
    return 1;
}

static int __attribute__((cdecl)) h_companion_update(void *self, float dt, void *character) {
    MsgInput in;
    uint8_t *e = env();
    if (e && g_p2 && character == (void *)g_p2 && g_human_update && p2_input(&in)) {
        uint8_t saved_sticks[16], saved_shoot[12];
        uint8_t saved_auto_aim = e[ENV_AUTO_AIM];
        memcpy(saved_sticks, e + ENV_STICKS, sizeof(saved_sticks));
        memcpy(saved_shoot, e + ENV_SHOOT_STATE, sizeof(saved_shoot));

        float *sticks = (float *)(e + ENV_STICKS);
        int firing = in.fire && (fabsf(in.aim_x) > 0.05f || fabsf(in.aim_y) > 0.05f);
        sticks[0] = in.move_x;
        sticks[1] = in.move_y;
        sticks[2] = firing ? in.aim_x : 0.0f;
        sticks[3] = firing ? in.aim_y : 0.0f;
        e[ENV_AUTO_AIM] = 0; /* the guest aims for themselves */

        g_human_update(self, dt, character);

        memcpy(e + ENV_STICKS, saved_sticks, sizeof(saved_sticks));
        memcpy(e + ENV_SHOOT_STATE, saved_shoot, sizeof(saved_shoot));
        e[ENV_AUTO_AIM] = saved_auto_aim;
        return firing;
    }
    /* Guest away (or an ordinary companion): the game's companion AI */
    return o_companion_update(self, dt, character);
}

/* Keep both players in view: centre between them and pull back as they
 * spread out */
static int coop_camera(float eye[3], float look[3]) {
    if (!g_p2 || !gun_in_level()) return 0;
    if (lockstep_active() && !g_in_p2_pass) return 0; /* player 1's camera, as on the host */
    uint8_t *e = env();
    uint8_t *player = e ? *(uint8_t **)(e + ENV_PLAYER) : NULL;
    uint8_t *p2 = live_p2(e);
    if (!player || !p2) return 0;

    const float *a = (const float *)(player + ENTITY_POS);
    const float *b = (const float *)(p2 + ENTITY_POS);
    float dx = b[0] - a[0], dz = b[1] - a[1];

    if (g_split_view) {
        if (!g_in_p2_pass) return 0; /* player 1's own camera, unchanged */
        /* The game's camera follows player 1: slide it onto player 2 */
        look[0] += dx; look[2] += dz;
        eye[0] += dx;  eye[2] += dz;
        return 1; /* no player 1 recoil in player 2's view */
    }

    float dist = sqrtf(dx * dx + dz * dz);
    float ox = dx * 0.5f, oz = dz * 0.5f;
    float shift = dist * 0.5f;
    if (shift > CAMERA_MAX_SHIFT) {
        ox *= CAMERA_MAX_SHIFT / shift;
        oz *= CAMERA_MAX_SHIFT / shift;
    }
    look[0] += ox; look[2] += oz;
    eye[0] += ox;  eye[2] += oz;

    float zoom = (dist - CAMERA_ZOOM_START) / CAMERA_ZOOM_RANGE;
    zoom = zoom < 0.0f ? 0.0f : (zoom > CAMERA_MAX_ZOOM ? CAMERA_MAX_ZOOM : zoom);
    for (int i = 0; i < 3; i++) eye[i] = look[i] + (eye[i] - look[i]) * (1.0f + zoom);
    return 0;
}

/* ---------------------------------------------------------------- */
/* Split view: player 2's render pass                               */
/* ---------------------------------------------------------------- */

static int view_target_ready(int w, int h) {
    if (g_view_failed) return 0;
    if (!gl_gen_fb) {
        gl_gen_fb = (PFN_GenFb)SDL_GL_GetProcAddress("glGenFramebuffers");
        gl_bind_fb = (PFN_BindFb)SDL_GL_GetProcAddress("glBindFramebuffer");
        gl_gen_rb = (PFN_GenRb)SDL_GL_GetProcAddress("glGenRenderbuffers");
        gl_bind_rb = (PFN_BindRb)SDL_GL_GetProcAddress("glBindRenderbuffer");
        gl_rb_storage = (PFN_RbStorage)SDL_GL_GetProcAddress("glRenderbufferStorage");
        gl_fb_rb = (PFN_FbRb)SDL_GL_GetProcAddress("glFramebufferRenderbuffer");
        gl_check_fb = (PFN_CheckFb)SDL_GL_GetProcAddress("glCheckFramebufferStatus");
        if (!gl_gen_fb || !gl_bind_fb || !gl_gen_rb || !gl_bind_rb || !gl_rb_storage || !gl_fb_rb || !gl_check_fb) {
            printf("[Co-op] Framebuffer objects unavailable; using a shared camera\n");
            g_view_failed = 1;
            return 0;
        }
        gl_gen_fb(1, &g_view_fb);
        gl_gen_rb(1, &g_view_color);
        gl_gen_rb(1, &g_view_depth);
    }
    if (w == g_view_w && h == g_view_h) return 1;

    GLint prev_rb = 0;
    glGetIntegerv(0x8CA7 /* GL_RENDERBUFFER_BINDING */, &prev_rb);
    gl_bind_rb(CO_GL_RENDERBUFFER, g_view_color);
    gl_rb_storage(CO_GL_RENDERBUFFER, GL_RGBA8, w, h);
    gl_bind_rb(CO_GL_RENDERBUFFER, g_view_depth);
    gl_rb_storage(CO_GL_RENDERBUFFER, CO_GL_DEPTH24_STENCIL8, w, h);
    gl_bind_rb(CO_GL_RENDERBUFFER, (GLuint)prev_rb);
    gl_bind_fb(CO_GL_FRAMEBUFFER, g_view_fb);
    gl_fb_rb(CO_GL_FRAMEBUFFER, CO_GL_COLOR_ATTACHMENT0, CO_GL_RENDERBUFFER, g_view_color);
    gl_fb_rb(CO_GL_FRAMEBUFFER, CO_GL_DEPTH_STENCIL_ATTACHMENT, CO_GL_RENDERBUFFER, g_view_depth);
    GLenum status = gl_check_fb(CO_GL_FRAMEBUFFER);
    gl_bind_fb(CO_GL_FRAMEBUFFER, 0);
    if (status != CO_GL_FRAMEBUFFER_COMPLETE) {
        printf("[Co-op] Player 2 view framebuffer incomplete (0x%x); using a shared camera\n", status);
        g_view_failed = 1;
        return 0;
    }
    g_view_w = w;
    g_view_h = h;
    return 1;
}

/* Own-world co-op overlay: both players' health and where they are on
 * this PC's screen (call right after the pass drawn to the window) */
static void publish_overlay(void) {
    uint8_t *e = env();
    uint8_t *player = e ? *(uint8_t **)(e + ENV_PLAYER) : NULL;
    EglCoopOverlay o;
    memset(&o, 0, sizeof(o));
    if (!player || !gun_in_level()) {
        egl_overlay_set_coop(NULL);
        return;
    }
    o.visible = 1;
    o.local = lockstep_is_guest() ? 1 : 0;
    o.out_of_sync = lockstep_out_of_sync();
    uint8_t *chars[2] = { player, (g_p2 && character_listed(e, g_p2)) ? g_p2 : NULL };
    for (int i = 0; i < 2; i++) {
        uint8_t *c = chars[i];
        if (!c) {
            o.p[i].present = i == 1; /* player 2 is coming back */
            continue;
        }
        o.p[i].present = 1;
        o.p[i].alive = character_alive(c);
        o.p[i].health = p2_health_fraction(c);
        const float *p = (const float *)(c + ENTITY_POS);
        o.p[i].on_screen_valid = gun_project_ground(p[0], p[1], &o.p[i].sx, &o.p[i].sy);
    }
    egl_overlay_set_coop(&o);
}

static void route_hud_to_window(int on) {
    GLuint fb = on ? 0 : g_view_fb;
    gl_wrap_set_default_framebuffer(fb);
    gl_bind_fb(CO_GL_FRAMEBUFFER, fb);
}

/* Guest: player 2's view to the window, then the host-identical pass
 * off-screen with its HUD / menus routed to the window */
static void lockstep_guest_draw(void *self, float dt, uint8_t *p2) {
    DetRng rng;
    det_rng_save(&rng);
    g_in_p2_pass = 1;
    o_draw(self, 0.0f);
    const float *p = (const float *)(p2 + ENTITY_POS);
    float sx, sy;
    if (gun_project_ground(p[0], p[1], &sx, &sy)) gun_set_aim_origin(sx, sy); /* mouse aim from player 2 */
    publish_overlay();
    g_in_p2_pass = 0;
    det_rng_restore(&rng);

    g_hud_to_window = 1;
    gl_wrap_set_default_framebuffer(g_view_fb);
    gl_bind_fb(CO_GL_FRAMEBUFFER, g_view_fb);
    o_draw(self, dt);
    gl_wrap_set_default_framebuffer(0);
    gl_bind_fb(CO_GL_FRAMEBUFFER, 0);
    g_hud_to_window = 0;
}

static void __attribute__((cdecl)) h_draw(void *self, float dt) {
    uint8_t *p2 = g_p2_live;
    if (lockstep_active()) {
        int w = 0, h = 0;
        SDL_Window *win = SDL_GL_GetCurrentWindow();
        if (win) SDL_GL_GetDrawableSize(win, &w, &h);
        if (lockstep_is_guest() && p2 && w > 0 && view_target_ready(w, h)) {
            lockstep_guest_draw(self, dt, p2);
        } else {
            o_draw(self, dt);
            publish_overlay();
        }
        return;
    }
    int w = 0, h = 0;
    SDL_Window *win = SDL_GL_GetCurrentWindow();
    if (win) SDL_GL_GetDrawableSize(win, &w, &h);

    if (g_split_view && p2 && net_host_guest_connected() && w > 0 && view_target_ready(w, h)) {
        /* Player 2's view first, off-screen. dt 0: animations advance once,
         * in player 1's pass. */
        g_in_p2_pass = 1;
        gl_wrap_set_default_framebuffer(g_view_fb);
        gl_bind_fb(CO_GL_FRAMEBUFFER, g_view_fb);
        o_draw(self, 0.0f);
        /* The view-projection is now player 2's: where they are on their screen */
        const float *p = (const float *)(p2 + ENTITY_POS);
        float sx = 0.5f, sy = 0.5f;
        gun_project_ground_normalized(p[0], p[1], &sx, &sy);
        net_host_set_p2(sx, sy, 1, 1);
        gl_wrap_set_default_framebuffer(0);
        gl_bind_fb(CO_GL_FRAMEBUFFER, 0);
        g_in_p2_pass = 0;
        net_host_set_capture_source(g_view_fb);
    } else {
        net_host_set_capture_source(0); /* stream the window (shared view) */
    }
    o_draw(self, dt);
}

/* Player 1's HUD and the GUI are left out of player 2's pass; the per-frame
 * GUI renderer cleanup must only run after player 1's pass. In the own-world
 * guest's off-screen pass they are drawn to the window instead. */
#define HUD_HOOK(call)                              \
    do {                                            \
        if (g_in_p2_pass) return;                   \
        if (g_hud_to_window) route_hud_to_window(1);\
        call;                                       \
        if (g_hud_to_window) route_hud_to_window(0);\
    } while (0)
static void __attribute__((cdecl)) h_draw_hud(void *a) { HUD_HOOK(o_draw_hud(a)); }
static void __attribute__((cdecl)) h_draw_tutorials(void *a) { HUD_HOOK(o_draw_tutorials(a)); }
static void __attribute__((cdecl)) h_draw_vignette(void *a) { HUD_HOOK(o_draw_vignette(a)); }
static void __attribute__((cdecl)) h_delete_renderers(void *a) { if (!g_in_p2_pass) o_delete_renderers(a); }
static void __attribute__((cdecl)) h_draw_arena_score(void) { HUD_HOOK(o_draw_arena_score()); }
static void __attribute__((cdecl)) h_draw_score(void) { HUD_HOOK(o_draw_score()); }
static void __attribute__((cdecl)) h_draw_gui(void *a, void *b) { HUD_HOOK(o_draw_gui(a, b)); }
static void __attribute__((cdecl)) h_draw_score_messages(void *a, void *b) { HUD_HOOK(o_draw_score_messages(a, b)); }

void coop_install(elf_module_t *mod) {
    g_env = (uint8_t **)elf_sym(mod, "m_env");
    g_human_update = (ControlUpdateFn)elf_sym(mod, "_ZN21CharacterControlHuman6updateEfP9Character");
    g_create_companion = (CreateCompanionFn)elf_sym(mod, "_ZN15PlayerCharacter24createCompanionCharacterEN10Characters4EnumERKN2xt7Vector2IfEE");
    if (!g_env || !g_human_update || !g_create_companion) {
        printf("[Co-op] Required game functions not found; co-op disabled\n");
        return;
    }

    o_state_update = (StateUpdateFn)hook_vtable(mod, "_ZTV9StateGame", 4, (void *)h_state_update);
    if (o_state_update) hook_got(mod, "_ZN9StateGame6updateEf", (void *)h_state_update);
    o_companion_update = (ControlUpdateFn)hook_vtable(mod, "_ZTV25CharacterControlCompanion", 2, (void *)h_companion_update);
    if (o_companion_update) hook_got(mod, "_ZN25CharacterControlCompanion6updateEfP9Character", (void *)h_companion_update);
    gun_set_camera_adjust(coop_camera);
    if (lockstep_active()) {
        g_split_view = 1; /* the guest's own view; the host draws once */
        if (lockstep_is_guest()) gun_set_local_character(coop_p2_live);
    }

    g_get_health = (IntGetterFn)elf_sym(mod, "_ZNK9Character9getHealthEv");
    g_get_max_health = (IntGetterFn)elf_sym(mod, "_ZNK15PlayerCharacter12getMaxHealthEv");

    if (g_split_view) {
        /* All of these must hook, or player 2's pass would draw player 1's
         * HUD or free this frame's GUI early - fall back to a shared view */
        o_draw_hud = (Void1Fn)hook_got(mod, "_ZN9StateGame12drawHudLayerEv", (void *)h_draw_hud);
        o_draw_tutorials = (Void1Fn)hook_got(mod, "_ZN9StateGame13drawTutorialsEv", (void *)h_draw_tutorials);
        o_draw_vignette = (Void1Fn)hook_got(mod, "_ZN9StateGame16drawVignetteDarkEv", (void *)h_draw_vignette);
        o_delete_renderers = (Void1Fn)hook_got(mod, "_ZN17UiRendererFactory15deleteRenderersEv", (void *)h_delete_renderers);
        o_draw_arena_score = (Void0Fn)hook_got(mod, "_Z14drawArenaScorev", (void *)h_draw_arena_score);
        o_draw_score = (Void0Fn)hook_got(mod, "_Z9drawScorev", (void *)h_draw_score);
        o_draw_gui = (Void2Fn)hook_got(mod, "_Z15drawGuiCommandsjj", (void *)h_draw_gui);
        o_draw_score_messages = (Void2Fn)hook_got(mod, "_ZN18ScoreMessageSystem4drawEP5xFont", (void *)h_draw_score_messages);
        int all = o_draw_hud && o_draw_tutorials && o_draw_vignette && o_delete_renderers &&
                  o_draw_arena_score && o_draw_score && o_draw_gui && o_draw_score_messages;
        if (all) {
            o_draw = (DrawFn)hook_vtable(mod, "_ZTV9StateGame", 5, (void *)h_draw);
            if (o_draw) hook_got(mod, "_ZN9StateGame4drawEf", (void *)h_draw);
        }
        if (!o_draw) {
            printf("[Co-op] Split view unavailable; using a shared camera\n");
            g_split_view = 0;
        }
    }

    if (lockstep_active()) {
        printf("[Co-op] Own-world co-op: player 2 joins both worlds as a companion when a level starts\n");
    } else {
        printf("[Co-op] Player 2 joins as a companion character when a guest connects and a level is running (%s)\n",
               g_split_view ? "own screen each" : "shared camera");
    }
    fflush(stdout);
}
