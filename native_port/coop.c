#include "coop.h"
#include "hook_util.h"
#include "net_host.h"
#include "gun_hooks.h"
#include <windows.h>
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
 * listed in the game's character array. */

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

static uint8_t **g_env;
static StateUpdateFn o_state_update;
static ControlUpdateFn o_companion_update;
static ControlUpdateFn g_human_update;
static CreateCompanionFn g_create_companion;

static uint8_t *g_p2 = NULL;
static DWORD g_p2_dead_since = 0;
static DWORD g_next_spawn = 0;

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

static void coop_tick(void) {
    uint8_t *e = env();
    uint8_t *player = e ? *(uint8_t **)(e + ENV_PLAYER) : NULL;
    int in_level = gun_in_level();
    DWORD now = GetTickCount();

    if (g_p2 && !character_listed(e, g_p2)) g_p2 = NULL; /* removed by the game */

    if (!player || !in_level || !net_host_guest_connected()) {
        net_host_set_p2(0.5f, 0.5f, 0, in_level);
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

    /* Where player 2 is on screen, so the guest can mouse-aim from it */
    uint8_t *p2 = live_p2(e);
    float sx = 0.5f, sy = 0.5f;
    if (p2) {
        const float *p = (const float *)(p2 + ENTITY_POS);
        gun_project_ground_normalized(p[0], p[1], &sx, &sy);
    }
    net_host_set_p2(sx, sy, p2 != NULL, 1);
}

static int __attribute__((cdecl)) h_state_update(void *self, float dt) {
    coop_tick();
    return o_state_update(self, dt);
}

static int __attribute__((cdecl)) h_companion_update(void *self, float dt, void *character) {
    MsgInput in;
    uint8_t *e = env();
    if (e && g_p2 && character == (void *)g_p2 && g_human_update && net_host_get_input(&in, INPUT_TIMEOUT_MS)) {
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
static void coop_camera(float eye[3], float look[3]) {
    if (!g_p2 || !gun_in_level()) return;
    uint8_t *e = env();
    uint8_t *player = e ? *(uint8_t **)(e + ENV_PLAYER) : NULL;
    uint8_t *p2 = live_p2(e);
    if (!player || !p2) return;

    const float *a = (const float *)(player + ENTITY_POS);
    const float *b = (const float *)(p2 + ENTITY_POS);
    float dx = b[0] - a[0], dz = b[1] - a[1];
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
}

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

    printf("[Co-op] Player 2 joins as a companion character when a guest connects and a level is running\n");
    fflush(stdout);
}
