#ifndef CF_READONLY_EXPORTS_H
#define CF_READONLY_EXPORTS_H
#include "readonly_module.h"
#include <elf.h>
#include <stddef.h>

typedef struct CfExports {
    uintptr_t bias, symbols, strings, sysv_hash, gnu_hash;
    size_t string_size, load_count;
    Elf64_Phdr loads[32];
} CfExports;

// Inspect the existing Android ARM64 ELF image. No dlopen, relocation, pattern
// scan, or library initialization. Only defined executable named functions.
bool cf_exports_open(const CfModule *module, CfExports *exports);
uintptr_t cf_exports_function(const CfExports *exports, const char *name);
#endif
