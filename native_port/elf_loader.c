#include "elf_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* Standard 32-bit ELF definitions */
#define EI_NIDENT 16
#define ELFMAG "\177ELF"
#define SELFMAG 4

#define ELFCLASS32  1
#define ELFDATA2LSB 1
#define EM_386      3

#define PT_LOAD    1
#define PT_DYNAMIC 2

#define DT_NULL       0
#define DT_PLTGOT     3
#define DT_HASH       4
#define DT_STRTAB     5
#define DT_SYMTAB     6
#define DT_RELA       7
#define DT_RELASZ     8
#define DT_RELAENT    9
#define DT_STRSZ     10
#define DT_SYMENT    11
#define DT_INIT      12
#define DT_FINI      13
#define DT_SONAME    14
#define DT_RPATH     15
#define DT_SYMBOLIC  16
#define DT_REL       17
#define DT_RELSZ     18
#define DT_RELENT    19
#define DT_PLTRELSZ   2
#define DT_PLTREL    20
#define DT_DEBUG     21
#define DT_TEXTREL   22
#define DT_JMPREL    23
#define DT_INIT_ARRAY   25
#define DT_FINI_ARRAY   26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28

#define SHN_UNDEF 0

#define ELF32_R_SYM(val)  ((val) >> 8)
#define ELF32_R_TYPE(val) ((val) & 0xff)

#define R_386_NONE      0
#define R_386_32        1
#define R_386_PC32      2
#define R_386_GOT32     3
#define R_386_PLT32     4
#define R_386_COPY      5
#define R_386_GLOB_DAT  6
#define R_386_JMP_SLOT  7
#define R_386_RELATIVE  8

#pragma pack(push, 1)

typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} Elf32_Phdr;

typedef struct {
    int32_t  d_tag;
    uint32_t d_val;
} Elf32_Dyn;

typedef struct {
    uint32_t st_name;
    uint32_t st_value;
    uint32_t st_size;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
} Elf32_Sym;

typedef struct {
    uint32_t r_offset;
    uint32_t r_info;
} Elf32_Rel;

#pragma pack(pop)

static uint32_t elf_hash(const uint8_t *name) {
    uint32_t h = 0, g;
    while (*name) {
        h = (h << 4) + *name++;
        if ((g = (h & 0xf0000000)) != 0)
            h ^= g >> 24;
        h &= 0x0fffffff;
    }
    return h;
}

int elf_load(const char *path, elf_module_t *mod) {
    memset(mod, 0, sizeof(*mod));
    
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[ELF] Failed to open %s\n", path);
        return -1;
    }
    
    fseek(f, 0, SEEK_END);
    size_t file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    uint8_t *file_data = (uint8_t *)malloc(file_size);
    if (!file_data) {
        fclose(f);
        return -1;
    }
    if (fread(file_data, 1, file_size, f) != file_size) {
        free(file_data);
        fclose(f);
        return -1;
    }
    fclose(f);
    
    Elf32_Ehdr *ehdr = (Elf32_Ehdr *)file_data;
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0 ||
        ehdr->e_ident[4] != ELFCLASS32 ||
        ehdr->e_ident[5] != ELFDATA2LSB ||
        ehdr->e_machine != EM_386) {
        fprintf(stderr, "[ELF] Invalid or unsupported ELF format\n");
        free(file_data);
        return -1;
    }
    
    Elf32_Phdr *phdrs = (Elf32_Phdr *)(file_data + ehdr->e_phoff);
    uint32_t min_vaddr = 0xFFFFFFFF;
    uint32_t max_vaddr = 0;
    
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_LOAD) {
            if (phdrs[i].p_vaddr < min_vaddr)
                min_vaddr = phdrs[i].p_vaddr;
            uint32_t end = phdrs[i].p_vaddr + phdrs[i].p_memsz;
            if (end > max_vaddr)
                max_vaddr = end;
        }
    }
    
    size_t total_size = (max_vaddr - min_vaddr + 0xFFF) & ~0xFFF;
    void *base = VirtualAlloc(NULL, total_size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!base) {
        fprintf(stderr, "[ELF] VirtualAlloc failed (%zu bytes)\n", total_size);
        free(file_data);
        return -1;
    }
    
    mod->base = base;
    mod->size = total_size;
    
    /* Load segments */
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_LOAD) {
            uint8_t *dest = (uint8_t *)base + phdrs[i].p_vaddr;
            if (phdrs[i].p_filesz > 0) {
                memcpy(dest, file_data + phdrs[i].p_offset, phdrs[i].p_filesz);
            }
            if (phdrs[i].p_memsz > phdrs[i].p_filesz) {
                memset(dest + phdrs[i].p_filesz, 0, phdrs[i].p_memsz - phdrs[i].p_filesz);
            }
        }
    }
    
    /* Locate PT_DYNAMIC */
    Elf32_Dyn *dyn = NULL;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_DYNAMIC) {
            dyn = (Elf32_Dyn *)((uint8_t *)base + phdrs[i].p_vaddr);
            break;
        }
    }
    
    if (dyn) {
        uint32_t init_array_addr = 0;
        uint32_t init_array_sz = 0;
        
        for (Elf32_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
            switch (d->d_tag) {
                case DT_STRTAB:
                    mod->dynstr = (const char *)base + d->d_val;
                    break;
                case DT_SYMTAB:
                    mod->dynsym = (void *)((uint8_t *)base + d->d_val);
                    break;
                case DT_HASH:
                    mod->hash = (uint32_t *)((uint8_t *)base + d->d_val);
                    break;
                case DT_REL:
                    mod->reldyn = (void *)((uint8_t *)base + d->d_val);
                    break;
                case DT_RELSZ:
                    mod->num_reldyn = d->d_val / sizeof(Elf32_Rel);
                    break;
                case DT_JMPREL:
                    mod->relplt = (void *)((uint8_t *)base + d->d_val);
                    break;
                case DT_PLTRELSZ:
                    mod->num_relplt = d->d_val / sizeof(Elf32_Rel);
                    break;
                case DT_INIT:
                    mod->init_func = (void (*)(void))((uint8_t *)base + d->d_val);
                    break;
                case DT_INIT_ARRAY:
                    init_array_addr = d->d_val;
                    break;
                case DT_INIT_ARRAYSZ:
                    init_array_sz = d->d_val;
                    break;
            }
        }
        
        if (init_array_addr && init_array_sz) {
            mod->init_array = (void (**)(void))((uint8_t *)base + init_array_addr);
            mod->num_init_array = init_array_sz / sizeof(void*);
        }
        
        if (mod->hash) {
            /* Number of symbol entries is given by chain count in DT_HASH */
            mod->num_dynsym = mod->hash[1];
        }
    }
    
    free(file_data);
    printf("[ELF] Loaded successfully at base %p (size: %zu KB)\n", mod->base, mod->size / 1024);
    return 0;
}

uintptr_t elf_sym(elf_module_t *mod, const char *name) {
    if (!mod || !mod->base || !mod->dynsym || !mod->dynstr)
        return 0;
        
    Elf32_Sym *syms = (Elf32_Sym *)mod->dynsym;
    
    if (mod->hash) {
        uint32_t h = elf_hash((const uint8_t *)name);
        uint32_t nbucket = mod->hash[0];
        uint32_t nchain = mod->hash[1];
        uint32_t *bucket = &mod->hash[2];
        uint32_t *chain = &bucket[nbucket];
        
        for (uint32_t idx = bucket[h % nbucket]; idx != 0 && idx < nchain; idx = chain[idx]) {
            if (syms[idx].st_shndx != SHN_UNDEF &&
                strcmp(mod->dynstr + syms[idx].st_name, name) == 0) {
                return (uintptr_t)mod->base + syms[idx].st_value;
            }
        }
    }
    
    /* Fallback: linear search */
    for (size_t i = 0; i < mod->num_dynsym; i++) {
        if (syms[i].st_shndx != SHN_UNDEF &&
            strcmp(mod->dynstr + syms[i].st_name, name) == 0) {
            return (uintptr_t)mod->base + syms[i].st_value;
        }
    }
    
    return 0;
}

static uintptr_t find_host_sym(const host_symbol_t *syms, size_t count, const char *name) {
    /* Exact match */
    for (size_t i = 0; i < count; i++) {
        if (strcmp(syms[i].name, name) == 0)
            return syms[i].func;
    }
    /* Versioned match (e.g. "fopen@LIBC" or "malloc@@LIBC") */
    const char *at = strchr(name, '@');
    if (at) {
        size_t base_len = (size_t)(at - name);
        for (size_t i = 0; i < count; i++) {
            if (strncmp(syms[i].name, name, base_len) == 0 && syms[i].name[base_len] == '\0') {
                return syms[i].func;
            }
        }
    }
    return 0;
}

int elf_relocate(elf_module_t *mod, const host_symbol_t *host_syms, size_t sym_count) {
    uint8_t *base = (uint8_t *)mod->base;
    Elf32_Sym *syms = (Elf32_Sym *)mod->dynsym;
    
    size_t total_relocs = mod->num_reldyn + mod->num_relplt;
    int unresolved = 0;
    
    for (size_t i = 0; i < total_relocs; i++) {
        Elf32_Rel *rel = (i < mod->num_reldyn) ?
            &((Elf32_Rel *)mod->reldyn)[i] :
            &((Elf32_Rel *)mod->relplt)[i - mod->num_reldyn];
            
        uint32_t r_type = ELF32_R_TYPE(rel->r_info);
        uint32_t r_sym  = ELF32_R_SYM(rel->r_info);
        uint32_t *target = (uint32_t *)(base + rel->r_offset);
        
        if (r_type == R_386_RELATIVE) {
            *target += (uint32_t)base;
            continue;
        }
        
        uintptr_t sym_val = 0;
        const char *sym_name = NULL;
        
        if (r_sym != 0) {
            sym_name = mod->dynstr + syms[r_sym].st_name;
            if (syms[r_sym].st_shndx != SHN_UNDEF) {
                /* Internal symbol */
                sym_val = (uintptr_t)base + syms[r_sym].st_value;
            } else {
                /* External symbol */
                sym_val = find_host_sym(host_syms, sym_count, sym_name);
                if (!sym_val) {
                    fprintf(stderr, "[ELF] Unresolved symbol: %s\n", sym_name);
                    unresolved++;
                }
            }
        }
        
        switch (r_type) {
            case R_386_32:
                *target += sym_val;
                break;
            case R_386_GLOB_DAT:
            case R_386_JMP_SLOT:
                *target = sym_val;
                break;
            case R_386_PC32:
                *target += sym_val - (uint32_t)target;
                break;
            default:
                fprintf(stderr, "[ELF] Unsupported relocation type %u at offset 0x%x\n",
                        r_type, rel->r_offset);
                break;
        }
    }
    
    if (unresolved > 0) {
        fprintf(stderr, "[ELF] Total unresolved symbols: %d\n", unresolved);
        return -1;
    }
    
    printf("[ELF] Relocations completed (%zu processed).\n", total_relocs);
    return 0;
}

void elf_init(elf_module_t *mod) {
    if (mod->init_func) {
        printf("[ELF] Calling init_func @ %p...\n", mod->init_func);
        mod->init_func();
    }
    
    if (mod->init_array && mod->num_init_array > 0) {
        printf("[ELF] Calling %zu init_array constructors...\n", mod->num_init_array);
        for (size_t i = 0; i < mod->num_init_array; i++) {
            if (mod->init_array[i] && (intptr_t)mod->init_array[i] != -1) {
                mod->init_array[i]();
            }
        }
    }
    printf("[ELF] Constructors finished.\n");
}
