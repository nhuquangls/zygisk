#ifndef CF_MEMORY_PATCH_H
#define CF_MEMORY_PATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A prepared, single-instruction ARM64 patch. There is no runtime toggle API.
// Original words are retained only for validation and failed-install rollback.
typedef struct {
  uintptr_t address;
  uint32_t original;
  uint32_t replacement;
  void *veneer;
  uintptr_t page;
  size_t page_size;
  int protection;
  bool published;
} MemoryPatch;

bool cf_branch_encode(uintptr_t from, uintptr_t to, bool link, uint32_t *word);
bool cf_branch_decode(uintptr_t from, uint32_t word, bool link, uintptr_t *to);
bool cf_patch_prepare(MemoryPatch *patch, uintptr_t address, uint32_t expected,
                      uintptr_t destination, bool link, char *error, size_t size);
bool cf_patch_commit(MemoryPatch *patches, size_t count, char *error, size_t size);
void cf_patch_discard_unpublished(MemoryPatch *patches, size_t count);

#endif
