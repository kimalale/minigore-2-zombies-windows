#ifndef GUN_HOOKS_H
#define GUN_HOOKS_H

#include "elf_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int   mouse_aim;      /* aim at the cursor while the left button is held */
    float kick;           /* camera recoil strength, 0 = off */
    float damage;         /* player gun damage multiplier */
    float ammo;           /* ammo per pickup multiplier */
    int   smart_autoaim;  /* threat-aware aim assist (game's auto-aim option) */
} GunConfig;

void gun_config_default(GunConfig *cfg);

/* Installs the hooks. Call after elf_relocate, before the game starts. */
void gun_hooks_install(elf_module_t *mod, const GunConfig *cfg);

/* For the input thread: returns 1 while a level is being played (the human
 * controller ran in the last moment) and gives the player's position in
 * window pixels, so mouse aim can point the aim stick at the cursor. */
int gun_player_screen_pos(float *x, float *y);

/* 1 while a level is being played (independent of mouse aim). */
int gun_in_level(void);

/* Controller rumble requested by the game thread (shots, hits, death).
 * Returns 1 and the strongest pending request, clearing it. */
int gun_take_rumble(float *low, float *high, unsigned *ms);

/* Direct stick input from the input thread, in the game's pad convention
 * (x right, y up, length <= 1). Applied right after the game reads its
 * touch sticks, replacing touch emulation for keyboard, mouse aim and
 * controllers. A zero vector leaves that stick to the touch screen.
 * Returns 1 if the direct path is available. */
int gun_set_sticks(float move_x, float move_y, float aim_x, float aim_y);

/* Projects a ground-plane world position to window pixels using the
 * game's last view-projection matrix. Returns 0 if it is behind the camera. */
int gun_project_ground(float x, float z, float *sx, float *sy);
/* Same, but 0..1 across the screen (y down). */
int gun_project_ground_normalized(float x, float z, float *nx, float *ny);

/* Lets co-op reframe the camera: called with modifiable eye / look-at
 * vectors every time the game sets up its camera. */
void gun_set_camera_adjust(void (*adjust)(float eye[3], float look[3]));

#ifdef __cplusplus
}
#endif

#endif /* GUN_HOOKS_H */
