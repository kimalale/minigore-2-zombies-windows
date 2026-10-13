#ifndef COOP_H
#define COOP_H

#include "elf_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Host side of online co-op: spawns player 2 as a companion character,
 * drives it with the guest's input and frames the camera on both
 * players. Requires net_host_start() and gun_hooks_install(). */
void coop_install(elf_module_t *mod);

/* 1 (default): player 2 gets their own camera, rendered off-screen and
 * streamed; 0: one shared camera framing both players. */
void coop_set_split_view(int split);

/* Player 2's character while alive and in the game (game thread), else NULL. */
void *coop_p2_live(void);

#ifdef __cplusplus
}
#endif

#endif /* COOP_H */
