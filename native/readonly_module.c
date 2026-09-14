#define _GNU_SOURCE
#include "readonly_module.h"
#include "readonly_memory.h"
#include <elf.h>
#include <link.h>
#include <stddef.h>
#include <string.h>

typedef struct Search {
    const char *name;
    CfModule *module;
    bool found;
} Search;

static void read_build_id(uintptr_t address, size_t length, char result[129]) {
    if (address > UINTPTR_MAX - length || length > 65536) return;
    size_t cursor = 0;
    while (cursor <= length && length - cursor >= sizeof(Elf64_Nhdr)) {
        Elf64_Nhdr note;
        if (!cf_read_self(address + cursor, &note, sizeof(note))) return;
        cursor += sizeof(note);
        size_t name_size = ((size_t)note.n_namesz + 3u) & ~(size_t)3u;
        size_t desc_size = ((size_t)note.n_descsz + 3u) & ~(size_t)3u;
        if (name_size > length - cursor) return;
        size_t desc = cursor + name_size;
        if (desc_size > length - desc) return;
        if (note.n_type == NT_GNU_BUILD_ID && note.n_namesz == 4 &&
            note.n_descsz > 0 && note.n_descsz <= 64) {
            char owner[4];
            unsigned char bytes[64];
            if (cf_read_self(address + cursor, owner, sizeof(owner)) &&
                memcmp(owner, "GNU", 4) == 0 &&
                cf_read_self(address + desc, bytes, note.n_descsz)) {
                static const char hex[] = "0123456789abcdef";
                for (size_t i = 0; i < note.n_descsz; ++i) {
                    result[2 * i] = hex[bytes[i] >> 4];
                    result[2 * i + 1] = hex[bytes[i] & 15];
                }
                result[2 * note.n_descsz] = '\0';
                return;
            }
        }
        cursor = desc + desc_size;
    }
}

static int visit_module(struct dl_phdr_info *info, size_t size, void *opaque) {
    (void)size;
    Search *search = opaque;
    if (info->dlpi_name == NULL || info->dlpi_phdr == NULL) return 0;
    const char *name = strrchr(info->dlpi_name, '/');
    name = name == NULL ? info->dlpi_name : name + 1;
    if (strcmp(name, search->name) != 0) return 0;
    CfModule candidate = { .load_bias = info->dlpi_addr,
                           .image_begin = UINTPTR_MAX };
    for (size_t i = 0; i < info->dlpi_phnum; ++i) {
        const Elf64_Phdr *header = &info->dlpi_phdr[i];
        if (header->p_type != PT_LOAD || header->p_memsz == 0) continue;
        if (header->p_vaddr > UINTPTR_MAX - candidate.load_bias) return 0;
        uintptr_t begin = candidate.load_bias + header->p_vaddr;
        if (header->p_memsz > UINTPTR_MAX - begin) return 0;
        uintptr_t end = begin + header->p_memsz;
        if (begin < candidate.image_begin) candidate.image_begin = begin;
        if (end > candidate.image_end) candidate.image_end = end;
    }
    if (candidate.image_begin >= candidate.image_end) return 0;
    for (size_t i = 0; i < info->dlpi_phnum; ++i) {
        const Elf64_Phdr *header = &info->dlpi_phdr[i];
        if (header->p_type != PT_NOTE ||
            header->p_vaddr > UINTPTR_MAX - candidate.load_bias) continue;
        read_build_id(candidate.load_bias + header->p_vaddr,
                      header->p_memsz, candidate.build_id);
        if (candidate.build_id[0] != '\0') break;
    }
    *search->module = candidate;
    search->found = true;
    return 1;
}

bool cf_find_module(const char *basename, CfModule *result) {
    if (result == NULL) return false;
    memset(result, 0, sizeof(*result));
    if (basename == NULL || basename[0] == '\0' || strchr(basename, '/') != NULL)
        return false;
    Search search = { .name = basename, .module = result };
    dl_iterate_phdr(visit_module, &search);
    return search.found;
}
