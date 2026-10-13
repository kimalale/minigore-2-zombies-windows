#ifndef SHIMS_H
#define SHIMS_H

#include "elf_loader.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void shims_init(void);
const host_symbol_t *shims_get_table(size_t *count);

/* Live host mutexes / condition variables (for hang reports) */
void shims_get_sync_stats(long *mutexes, long *conds);

/* The lrand48 generator state (determinism fingerprints) */
const volatile long long *shims_rand48_state(void);

#ifdef __cplusplus
}
#endif

#endif /* SHIMS_H */
