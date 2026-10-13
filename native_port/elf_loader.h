#ifndef ELF_LOADER_H
#define ELF_LOADER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    uintptr_t func;
} host_symbol_t;

typedef struct {
    void *base;             /* Base address where ELF was mapped */
    size_t size;            /* Total virtual memory size */
    void *entry;            /* ELF entry point if any */
    
    /* Dynamic section pointers */
    const char *dynstr;
    void *dynsym;           /* Elf32_Sym* */
    size_t num_dynsym;
    uint32_t *hash;         /* DT_HASH table */
    
    /* Relocations */
    void *reldyn;           /* Elf32_Rel* */
    size_t num_reldyn;
    void *relplt;           /* Elf32_Rel* */
    size_t num_relplt;
    
    /* Init functions */
    void (**init_array)(void);
    size_t num_init_array;
    void (*init_func)(void);
} elf_module_t;

/* Load an x86 32-bit ELF shared object into executable memory */
int elf_load(const char *path, elf_module_t *mod);

/* Relocate and resolve symbols using the provided host symbol table */
int elf_relocate(elf_module_t *mod, const host_symbol_t *syms, size_t sym_count);

/* Run .init and .init_array constructors */
void elf_init(elf_module_t *mod);

/* Lookup an exported symbol in the loaded module */
uintptr_t elf_sym(elf_module_t *mod, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* ELF_LOADER_H */
