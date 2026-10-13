#ifndef JAVA_STUBS_H
#define JAVA_STUBS_H

#include "elf_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Replaces the game's Java (JNI) helpers that call into Java without
 * checking that a Java environment exists - there is none on Windows -
 * with safe stubs. Call after relocation, before the game starts. */
void java_stubs_install(elf_module_t *mod);

#ifdef __cplusplus
}
#endif

#endif /* JAVA_STUBS_H */
