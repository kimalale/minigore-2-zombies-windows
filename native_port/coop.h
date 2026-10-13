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

#ifdef __cplusplus
}
#endif

#endif /* COOP_H */
