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

#ifdef __cplusplus
}
#endif

#endif /* GUN_HOOKS_H */
