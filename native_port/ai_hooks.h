#ifndef AI_HOOKS_H
#define AI_HOOKS_H

#include "elf_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Patches libminigore2.so so following zombies steer with ai_core.
 * Verifies the original instruction bytes first and leaves the game
 * untouched (returning 0) if they don't match. Call after relocation. */
int ai_hooks_install(elf_module_t *mod);

#ifdef __cplusplus
}
#endif

#endif /* AI_HOOKS_H */
