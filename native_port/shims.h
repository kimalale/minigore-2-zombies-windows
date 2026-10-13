#ifndef SHIMS_H
#define SHIMS_H

#include "elf_loader.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void shims_init(void);
const host_symbol_t *shims_get_table(size_t *count);

#ifdef __cplusplus
}
#endif

#endif /* SHIMS_H */
