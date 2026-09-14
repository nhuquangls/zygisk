#ifndef CF_READONLY_MODULE_H
#define CF_READONLY_MODULE_H
#include <stdbool.h>
#include <stdint.h>
typedef struct CfModule {
    uintptr_t load_bias;
    uintptr_t image_begin;
    uintptr_t image_end;
    char build_id[129];
} CfModule;
// Inspect the linker's module list; never dlopen or invoke the target library.
bool cf_find_module(const char *basename, CfModule *result);
#endif
