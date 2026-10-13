#include "gun_hooks.h"
#include "hook_util.h"
#include "ai_core.h"
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Gun improvements, all hooked through the GOT / vtables of the original
 * libminigore2.so (no code bytes are patched):
 *
 *  - Mouse aim support: the human controller's update is wrapped to
 *    publish the player's on-screen position (projected with the game's
 *    own view-projection matrix) and a "level in progress" heartbeat. The
 *    input thread uses them to drive the virtual aim stick at the cursor.
 *  - Recoil kick: each shot the player fires nudges the camera back along
 *    the aim direction (scaled by the weapon's damage per shot), decaying
 *    over ~0.1 s, layered on top of the game's own camera shakes.
 *  - Rebalance: player shots deal more damage and ammo pickups hold more
 *    rounds, to keep pace with the smarter horde.
 *  - Smart aim assist: when the game's auto-aim option is on, it picks the
 *    most dangerous target near the direction you are aiming (biters about
 *    to reach you, throwers in range, bosses) instead of simply the nearest
 *    enemy, and reaches far enough to answer throwers holding at range. */

/* m_env offsets (see ZombieStateFollow::update, setCamera,
 * CharacterControlHuman::update, CharacterControl::autoAim) */
#define ENV_PLAYER         0x1f4fa0  /* Character* the horde chases (human player) */
#define ENV_VIEWPROJ       0x1f5f6c  /* Matrix44 projection * view, column-major */
#define ENV_AIM_STICK      0x1f6048  /* Vector2 world-space aim stick */
#define ENV_ENTITY_SYSTEM  0x12784   /* EntitySystem */
#define ENTITY_POS         0x04      /* Vector2 position */
#define CHAR_AIM_DIR       0x50      /* Vector2 world-space aim direction */
#define GUN_WEAPON_TYPE    0x124     /* Weapons::Enum */

/* WeaponInfo (getWeaponInfo) */
#define WI_SIZE            0xc0
#define WI_IS_GUN          0x04      /* int, 1 for guns */
#define WI_DAMAGE          0x10      /* float, per projectile */
#define WI_PELLETS         0x50      /* int, projectiles per shot */
#define WI_AMMO_MIN        0x90      /* int, rounds in a pickup */
#define WI_AMMO_MAX        0x94

/* Query used by CharacterControl::autoAim */
#define AIM_QUERY_MASK     0x00000f8ffddfed83ull
#define AIM_QUERY_MAX      100
#define GOT_BASE           0x394520  /* PIC base the constants are relative to */
#define AIM_RADIUS_GUN     (GOT_BASE - 0x877cc)   /* 200.0 */
#define AIM_RADIUS_MELEE   (GOT_BASE - 0x8765c)   /* 45.0 */
#define SMART_AIM_REACH    1.4f      /* x gun radius, so throwers at range count */
#define AIM_CONE_COS       0.766f    /* cos(40 deg): aim assist stays near your aim */

#define VTBL_HEALTH        0x28      /* Entity: int health-ish, > 0 when alive */
#define VTBL_DAMAGEABLE    0x20      /* Entity: bool isDamageable */

typedef int   (__attribute__((cdecl)) *ControlUpdateFn)(void *self, float dt, void *character);
typedef int   (__attribute__((cdecl)) *FireShotFn)(void *self, float dt, unsigned index);
typedef void  (__attribute__((cdecl)) *SetCameraFn)(const float *eye, const float *look, const float *up,
                                                    float fov, float aspect, float znear, float zfar);
typedef void  (__attribute__((cdecl)) *GetWeaponInfoFn)(uint8_t *info, unsigned type);
typedef int   (__attribute__((cdecl)) *ApplyProjectileDamageFn)(float damage, const void *from, const void *to,
                                                                unsigned a, const unsigned *b, unsigned c,
                                                                unsigned long long mask);
typedef int   (__attribute__((cdecl)) *AutoAimFn)(const float *pos, float *out_x, float *out_y, int melee);
typedef unsigned (__attribute__((cdecl)) *QueryFn)(void *system, const float *pos, float radius,
                                                   unsigned long long mask, unsigned flags,
                                                   void *results, unsigned max_results);
typedef void *(__attribute__((cdecl)) *GetGunFn)(const void *character, int index);
typedef int   (__attribute__((cdecl)) *ScreenSizeFn)(void);
typedef int   (__attribute__((cdecl)) *EntityIntFn)(void *entity);
typedef void  (__attribute__((cdecl)) *StartShakeFn)(void *shaker);
typedef int   (__attribute__((cdecl)) *DieFn)(void *self, const void *damage, float t);

static GunConfig g_cfg;
static uint8_t **g_env;         /* &m_env */
static const float *g_aim_radius_gun;
static const float *g_aim_radius_melee;

static ControlUpdateFn o_control_update;
static FireShotFn o_fire_shot;
static SetCameraFn o_set_camera;
static GetWeaponInfoFn o_get_weapon_info;
static ApplyProjectileDamageFn o_apply_damage3;
static ApplyProjectileDamageFn o_apply_damage2;
static AutoAimFn o_auto_aim;
static QueryFn g_query;
static GetGunFn g_get_gun;
static ScreenSizeFn g_screen_w;
static ScreenSizeFn g_screen_h;
static StartShakeFn o_start_shake;
static DieFn o_die;

/* Pending rumble for the input thread: strongest request wins */
static CRITICAL_SECTION g_rumble_lock;
static volatile LONG g_rumble_ready = 0;
static float g_rumble_low, g_rumble_high;
static unsigned g_rumble_ms;

static LARGE_INTEGER g_qpc_freq;
static volatile LONG g_player_shooting = 0;

/* Published for the input thread */
static volatile float g_player_sx, g_player_sy;
static volatile LONG g_controls_tick = 0;  /* GetTickCount of last human control update */

/* Recoil state (game thread) */
static float g_kick = 0.0f;
static double g_kick_t = 0.0;

void gun_config_default(GunConfig *cfg) {
    cfg->mouse_aim = 1;
    cfg->kick = 1.0f;
    cfg->damage = 1.2f;
    cfg->ammo = 1.3f;
    cfg->smart_autoaim = 1;
}

static double now_seconds(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)g_qpc_freq.QuadPart;
}

static uint8_t *env(void) {
    return g_env ? *g_env : NULL;
}

static uint8_t *player_character(void) {
    uint8_t *e = env();
    return e ? *(uint8_t **)(e + ENV_PLAYER) : NULL;
}

/* ---------------------------------------------------------------- */
/* Mouse aim support                                                */
/* ---------------------------------------------------------------- */

static void publish_player_screen_pos(void) {
    uint8_t *e = env();
    uint8_t *player = player_character();
    if (!e || !player || !g_screen_w || !g_screen_h) return;

    const float *m = (const float *)(e + ENV_VIEWPROJ);
    const float *p = (const float *)(player + ENTITY_POS);
    /* Ground-plane position (x, 0, z) through projection * view */
    float x = p[0], y = 0.0f, z = p[1];
    float cx = m[0] * x + m[4] * y + m[8] * z + m[12];
    float cy = m[1] * x + m[5] * y + m[9] * z + m[13];
    float cw = m[3] * x + m[7] * y + m[11] * z + m[15];
    if (cw <= 1e-6f) return;
    float w = (float)g_screen_w(), h = (float)g_screen_h();
    g_player_sx = (cx / cw * 0.5f + 0.5f) * w;
    g_player_sy = (1.0f - (cy / cw * 0.5f + 0.5f)) * h; /* window y grows down */
}

static int __attribute__((cdecl)) h_control_update(void *self, float dt, void *character) {
    publish_player_screen_pos();
    InterlockedExchange(&g_controls_tick, (LONG)GetTickCount());
    return o_control_update(self, dt, character);
}

int gun_in_level(void) {
    LONG tick = g_controls_tick;
    return o_control_update && tick != 0 && (LONG)GetTickCount() - tick <= 150; /* not menus / paused */
}

int gun_player_screen_pos(float *x, float *y) {
    if (!g_cfg.mouse_aim || !gun_in_level()) return 0;
    *x = g_player_sx;
    *y = g_player_sy;
    return 1;
}

/* ---------------------------------------------------------------- */
/* Controller rumble                                                */
/* ---------------------------------------------------------------- */

static void request_rumble(float low, float high, unsigned ms) {
    if (!g_rumble_ready) return;
    EnterCriticalSection(&g_rumble_lock);
    if (low + high > g_rumble_low + g_rumble_high) {
        g_rumble_low = low;
        g_rumble_high = high;
        g_rumble_ms = ms;
    }
    LeaveCriticalSection(&g_rumble_lock);
}

int gun_take_rumble(float *low, float *high, unsigned *ms) {
    int have = 0;
    if (!g_rumble_ready) return 0;
    EnterCriticalSection(&g_rumble_lock);
    if (g_rumble_ms) {
        *low = g_rumble_low;
        *high = g_rumble_high;
        *ms = g_rumble_ms;
        g_rumble_low = g_rumble_high = 0.0f;
        g_rumble_ms = 0;
        have = 1;
    }
    LeaveCriticalSection(&g_rumble_lock);
    return have;
}

/* Screen shakes accompany taking damage, explosions and level-ups */
static void __attribute__((cdecl)) h_start_shake(void *shaker) {
    request_rumble(0.55f, 0.35f, 220);
    o_start_shake(shaker);
}

static int __attribute__((cdecl)) h_die(void *self, const void *damage, float t) {
    if (self == (void *)player_character()) request_rumble(1.0f, 0.8f, 600);
    return o_die(self, damage, t);
}

/* ---------------------------------------------------------------- */
/* Shots: recoil kick and damage                                    */
/* ---------------------------------------------------------------- */

static float shot_strength(const uint8_t *character) {
    if (!g_get_gun || !o_get_weapon_info) return 1.0f;
    const uint8_t *gun = (const uint8_t *)g_get_gun(character, 0);
    if (!gun) return 1.0f;
    uint8_t info[WI_SIZE];
    o_get_weapon_info(info, *(const unsigned *)(gun + GUN_WEAPON_TYPE));
    if (*(const int *)(info + WI_IS_GUN) != 1) return 1.0f;
    float per_shot = *(const float *)(info + WI_DAMAGE) * (float)(*(const int *)(info + WI_PELLETS));
    /* machine gun (8) ~1.1, shotgun (5 x 60) ~6.9, grenade (250) ~6.3 */
    return 0.4f * sqrtf(per_shot > 1.0f ? per_shot : 1.0f);
}

static int __attribute__((cdecl)) h_fire_shot(void *self, float dt, unsigned index) {
    int is_player = (self != NULL && self == (void *)player_character());
    if (is_player) InterlockedExchange(&g_player_shooting, 1);
    int fired = o_fire_shot(self, dt, index) & 0xff;
    if (is_player) InterlockedExchange(&g_player_shooting, 0);

    if (is_player && fired) {
        float strength = shot_strength((const uint8_t *)self);
        if (g_cfg.kick > 0.0f) {
            g_kick += strength * g_cfg.kick;
            if (g_kick > 10.0f * g_cfg.kick) g_kick = 10.0f * g_cfg.kick;
        }
        /* machine gun ~1 -> light buzz, shotgun ~7 -> solid thump */
        float r = strength / 8.0f;
        if (r > 0.8f) r = 0.8f;
        request_rumble(r * 0.6f, 0.15f + r * 0.5f, strength > 3.0f ? 90 : 45);
    }
    return fired;
}

static int __attribute__((cdecl)) h_apply_damage3(float damage, const void *from, const void *to, unsigned a,
                                                  const unsigned *b, unsigned c, unsigned long long mask) {
    if (g_player_shooting) damage *= g_cfg.damage;
    return o_apply_damage3(damage, from, to, a, b, c, mask);
}

static int __attribute__((cdecl)) h_apply_damage2(float damage, const void *from, const void *to, unsigned a,
                                                  const unsigned *b, unsigned c, unsigned long long mask) {
    if (g_player_shooting) damage *= g_cfg.damage;
    return o_apply_damage2(damage, from, to, a, b, c, mask);
}

static void __attribute__((cdecl)) h_get_weapon_info(uint8_t *info, unsigned type) {
    o_get_weapon_info(info, type);
    if (*(int *)(info + WI_IS_GUN) == 1 && g_cfg.ammo != 1.0f) {
        int *lo = (int *)(info + WI_AMMO_MIN);
        int *hi = (int *)(info + WI_AMMO_MAX);
        *lo = (int)lroundf((float)*lo * g_cfg.ammo);
        *hi = (int)lroundf((float)*hi * g_cfg.ammo);
    }
}

static void __attribute__((cdecl)) h_set_camera(const float *eye, const float *look, const float *up,
                                                float fov, float aspect, float znear, float zfar) {
    double t = now_seconds();
    double dt = t - g_kick_t;
    g_kick_t = t;
    if (dt > 0.0 && dt < 0.5) g_kick *= (float)exp(-dt * 18.0);
    else g_kick = 0.0f;

    /* Only touch the player object while a level is running */
    LONG tick = g_controls_tick;
    int in_level = tick != 0 && (LONG)GetTickCount() - tick < 150;
    uint8_t *player = in_level ? player_character() : NULL;
    if (g_kick < 0.01f || !player) {
        o_set_camera(eye, look, up, fov, aspect, znear, zfar);
        return;
    }

    /* Push the view back against the aim direction, with a little jitter */
    const float *aim = (const float *)(player + CHAR_AIM_DIR);
    float ax = aim[0], az = aim[1];
    float len = sqrtf(ax * ax + az * az);
    if (len > 1e-4f) { ax /= len; az /= len; } else { ax = 0.0f; az = 0.0f; }
    float jitter = ((float)(GetTickCount() % 7) - 3.0f) / 3.0f * 0.25f * g_kick;
    float ox = -ax * g_kick - az * jitter;
    float oz = -az * g_kick + ax * jitter;

    float e[3] = { eye[0] + ox, eye[1], eye[2] + oz };
    float l[3] = { look[0] + ox, look[1], look[2] + oz };
    o_set_camera(e, l, up, fov, aspect, znear, zfar);
}

/* ---------------------------------------------------------------- */
/* Smart aim assist                                                 */
/* ---------------------------------------------------------------- */

typedef struct {
    uint8_t *entity;
    float value;
} QueryResult;

static int __attribute__((cdecl)) h_auto_aim(const float *pos, float *out_x, float *out_y, int melee) {
    uint8_t *e = env();
    uint8_t *player = player_character();
    if (!g_cfg.smart_autoaim || !e || !g_query) return o_auto_aim(pos, out_x, out_y, melee);

    /* Intent: only the human player's own aim stick (companions use this too) */
    float ix = 0.0f, iy = 0.0f;
    int has_intent = 0;
    if (player && pos == (const float *)(player + ENTITY_POS)) {
        const float *stick = (const float *)(e + ENV_AIM_STICK);
        float l = sqrtf(stick[0] * stick[0] + stick[1] * stick[1]);
        if (l > 0.2f) {
            ix = stick[0] / l;
            iy = stick[1] / l;
            has_intent = 1;
        }
    }

    float base_radius = melee & 0xff ? *g_aim_radius_melee : *g_aim_radius_gun;
    float reach = melee & 0xff ? base_radius : base_radius * SMART_AIM_REACH;
    static QueryResult results[AIM_QUERY_MAX];
    unsigned n = g_query(e + ENV_ENTITY_SYSTEM, pos, reach, AIM_QUERY_MASK, 0, results, AIM_QUERY_MAX);

    float best_score = 0.0f;
    float bx = 0.0f, by = 0.0f;
    for (unsigned i = 0; i < n && i < AIM_QUERY_MAX; i++) {
        uint8_t *ent = results[i].entity;
        if (!ent) continue;
        uintptr_t vt = *(uintptr_t *)ent;
        if (((EntityIntFn)(*(uintptr_t *)(vt + VTBL_HEALTH)))(ent) == 0) continue;
        if ((((EntityIntFn)(*(uintptr_t *)(vt + VTBL_DAMAGEABLE)))(ent) & 0xff) == 0) continue;

        const float *ep = (const float *)(ent + ENTITY_POS);
        float dx = ep[0] - pos[0], dy = ep[1] - pos[1];
        float d = sqrtf(dx * dx + dy * dy);
        if (d < 1e-3f) continue;
        dx /= d;
        dy /= d;

        AiRole role = AI_ROLE_MELEE;
        int known = ai_lookup_role((uintptr_t)ent, &role);

        /* Only beyond the normal radius for throwers holding at range */
        if (d > base_radius && !(known && role == AI_ROLE_RANGED)) continue;

        float score = 100.0f / (d + 20.0f);
        if (d < base_radius * 0.4f) score *= 1.5f;                 /* about to reach you */
        if (known && role == AI_ROLE_RANGED) score *= 1.8f;         /* bombing you */
        if (known && role == AI_ROLE_BOSS) score *= 1.3f;
        if (has_intent) {
            float c = dx * ix + dy * iy;
            if (c < AIM_CONE_COS) continue;                         /* respect your aim */
            score *= 0.5f + 0.5f * c;
        }
        if (score > best_score) {
            best_score = score;
            bx = dx;
            by = dy;
        }
    }

    if (best_score > 0.0f) {
        *out_x = bx;
        *out_y = by;
        return 1;
    }
    if (has_intent) {
        /* Nothing to help with: shoot where the player is aiming */
        *out_x = ix;
        *out_y = iy;
        return 1;
    }
    *out_x = 0.0f;
    *out_y = 0.0f;
    return 0;
}

/* ---------------------------------------------------------------- */

void gun_hooks_install(elf_module_t *mod, const GunConfig *cfg) {
    g_cfg = *cfg;
    QueryPerformanceFrequency(&g_qpc_freq);
    InitializeCriticalSection(&g_rumble_lock);
    InterlockedExchange(&g_rumble_ready, 1);
    g_env = (uint8_t **)elf_sym(mod, "m_env");
    if (!g_env) {
        printf("[Guns] m_env not found; gun changes disabled\n");
        return;
    }
    g_aim_radius_gun = (const float *)module_addr(mod, AIM_RADIUS_GUN);
    g_aim_radius_melee = (const float *)module_addr(mod, AIM_RADIUS_MELEE);
    g_query = (QueryFn)elf_sym(mod, "_ZN12EntitySystem19queryEntitiesAroundERKN2xt7Vector2IfEEfyjPNS_11QueryResultEj");
    g_get_gun = (GetGunFn)elf_sym(mod, "_ZNK9Character6getGunEi");
    g_screen_w = (ScreenSizeFn)elf_sym(mod, "_ZN2xt6Screen8getWidthEv");
    g_screen_h = (ScreenSizeFn)elf_sym(mod, "_ZN2xt6Screen9getHeightEv");

    /* Weapon info is used by every feature below, so hook it first */
    o_get_weapon_info = (GetWeaponInfoFn)hook_got(mod, "_Z13getWeaponInfoR10WeaponInfoj", (void *)h_get_weapon_info);

    /* Heartbeat + player screen position (also gates the recoil code).
     * Virtual calls go through the vtable, direct ones through the GOT. */
    o_control_update = (ControlUpdateFn)hook_vtable(mod, "_ZTV21CharacterControlHuman", 2, (void *)h_control_update);
    if (o_control_update) hook_got(mod, "_ZN21CharacterControlHuman6updateEfP9Character", (void *)h_control_update);
    o_fire_shot = (FireShotFn)hook_got(mod, "_ZN9Character8fireShotEfj", (void *)h_fire_shot);
    if (cfg->kick > 0.0f) {
        o_set_camera = (SetCameraFn)hook_got(mod, "_Z9setCameraRKN2xt7Vector3IfEES3_S3_ffff", (void *)h_set_camera);
    }
    if (cfg->damage != 1.0f) {
        o_apply_damage3 = (ApplyProjectileDamageFn)hook_got(mod, "_Z21applyProjectileDamagefRKN2xt7Vector3IfEES3_jPKjjy", (void *)h_apply_damage3);
        o_apply_damage2 = (ApplyProjectileDamageFn)hook_got(mod, "_Z21applyProjectileDamagefRKN2xt7Vector2IfEES3_jPKjjy", (void *)h_apply_damage2);
    }
    o_start_shake = (StartShakeFn)hook_got(mod, "_ZN12CameraShaker10startShakeEv", (void *)h_start_shake);
    o_die = (DieFn)hook_got(mod, "_ZN15PlayerCharacter3dieERK6Damagef", (void *)h_die);

    if (cfg->smart_autoaim && g_query) {
        o_auto_aim = (AutoAimFn)hook_got(mod, "_ZN16CharacterControl7autoAimERKN2xt7Vector2IfEEPfS5_b", (void *)h_auto_aim);
    }

    printf("[Guns] mouse aim %s, recoil kick %.1f, damage x%.2f, ammo x%.2f, smart aim assist %s\n",
           (cfg->mouse_aim && o_control_update) ? "on" : "off", o_set_camera ? cfg->kick : 0.0f,
           o_apply_damage3 ? cfg->damage : 1.0f, o_get_weapon_info ? cfg->ammo : 1.0f,
           o_auto_aim ? "on" : "off");
    fflush(stdout);
}
