#include "hook_util.h"
#include <stdint.h>
#include <string.h>

typedef struct {
    uint32_t r_offset;
    uint32_t r_info;
} HookRel;

typedef struct {
    uint32_t st_name;
    uint32_t st_value;
    uint32_t st_size;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
} HookSym;

#define HOOK_R_386_GLOB_DAT 6
#define HOOK_R_386_JMP_SLOT 7

static void *patch_relocs(elf_module_t *mod, const HookRel *rels, size_t count,
                          const char *symbol, void *replacement, void *original) {
    const HookSym *syms = (const HookSym *)mod->dynsym;
    for (size_t i = 0; i < count; i++) {
        uint32_t type = rels[i].r_info & 0xff;
        uint32_t sym = rels[i].r_info >> 8;
        if ((type != HOOK_R_386_JMP_SLOT && type != HOOK_R_386_GLOB_DAT) || sym == 0) continue;
        if (strcmp(mod->dynstr + syms[sym].st_name, symbol) != 0) continue;
        void **slot = (void **)((uint8_t *)mod->base + rels[i].r_offset);
        if (!original) original = *slot;
        *slot = replacement;
    }
    return original;
}

void *hook_got(elf_module_t *mod, const char *symbol, void *replacement) {
    void *original = NULL;
    original = patch_relocs(mod, (const HookRel *)mod->relplt, mod->num_relplt, symbol, replacement, original);
    original = patch_relocs(mod, (const HookRel *)mod->reldyn, mod->num_reldyn, symbol, replacement, original);
    return original;
}

void *hook_vtable(elf_module_t *mod, const char *vtable_symbol, int slot, void *replacement) {
    uintptr_t vt = elf_sym(mod, vtable_symbol);
    if (!vt) return NULL;
    void **entry = (void **)(vt + 8 + (uintptr_t)slot * 4);
    void *original = *entry;
    *entry = replacement; /* the module is mapped read-write-execute */
    return original;
}

void *module_addr(const elf_module_t *mod, uintptr_t offset) {
    return (uint8_t *)mod->base + offset;
}
