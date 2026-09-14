#define _GNU_SOURCE
#include "memory_patch.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define BRANCH_RANGE ((uintptr_t)1 << 27)
#define MAX_PATCHES 2

static void fail(char *error, size_t size, const char *format, ...) {
  if (error == NULL || size == 0) return;
  va_list args;
  va_start(args, format);
  vsnprintf(error, size, format, args);
  va_end(args);
}

bool cf_branch_encode(uintptr_t from, uintptr_t to, bool link, uint32_t *word) {
  if (word == NULL || ((from | to) & 3u) != 0) return false;
  int64_t delta;
  if (to >= from) {
    if (to - from >= BRANCH_RANGE) return false;
    delta = (int64_t)(to - from);
  } else {
    if (from - to > BRANCH_RANGE) return false;
    delta = -(int64_t)(from - to);
  }
  *word = (link ? 0x94000000u : 0x14000000u) |
          ((uint32_t)(delta / 4) & 0x03ffffffu);
  return true;
}

bool cf_branch_decode(uintptr_t from, uint32_t word, bool link, uintptr_t *to) {
  if (to == NULL || (from & 3u) != 0 ||
      (word & 0xfc000000u) != (link ? 0x94000000u : 0x14000000u)) return false;
  int64_t offset = word & 0x03ffffffu;
  if (offset & 0x02000000) offset -= 0x04000000;
  offset *= 4;
  if ((offset < 0 && from < (uintptr_t)-offset) ||
      (offset >= 0 && from > UINTPTR_MAX - (uintptr_t)offset)) return false;
  *to = offset < 0 ? from - (uintptr_t)-offset : from + (uintptr_t)offset;
  return true;
}

static bool page_protection(uintptr_t address, int *protection) {
  FILE *maps = fopen("/proc/self/maps", "r");
  if (maps == NULL) return false;
  char line[1024], access[5];
  uintptr_t begin, end;
  bool found = false;
  while (fgets(line, sizeof(line), maps) != NULL) {
    if (sscanf(line, "%" SCNxPTR "-%" SCNxPTR " %4s", &begin, &end, access) != 3)
      continue;
    if (address >= begin && address < end && end - address >= 4) {
      *protection = (access[0] == 'r' ? PROT_READ : 0) |
                    (access[1] == 'w' ? PROT_WRITE : 0) |
                    (access[2] == 'x' ? PROT_EXEC : 0);
      found = true;
      break;
    }
  }
  fclose(maps);
  return found;
}

static void *try_near(uintptr_t hint, uintptr_t target, size_t page_size) {
  // A hint only: never replace an existing mapping with MAP_FIXED.
  void *memory = mmap((void *)hint, page_size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (memory == MAP_FAILED) return NULL;
  uint32_t unused;
  if (cf_branch_encode(target, (uintptr_t)memory, false, &unused)) return memory;
  munmap(memory, page_size);
  return NULL;
}

static void *allocate_near(uintptr_t target, size_t page_size) {
  void *memory = try_near(target & ~(page_size - 1u), target, page_size);
  if (memory != NULL) return memory;
  uintptr_t low = target > BRANCH_RANGE ? target - BRANCH_RANGE : page_size;
  uintptr_t high = target <= UINTPTR_MAX - BRANCH_RANGE
                       ? target + BRANCH_RANGE : UINTPTR_MAX;
  uintptr_t cursor = (low + page_size - 1u) & ~(page_size - 1u);
  FILE *maps = fopen("/proc/self/maps", "r");
  if (maps == NULL) return NULL;
  char line[1024];
  uintptr_t begin, end;
  while (fgets(line, sizeof(line), maps) != NULL) {
    if (sscanf(line, "%" SCNxPTR "-%" SCNxPTR, &begin, &end) != 2 || end <= cursor)
      continue;
    uintptr_t gap_end = begin < high ? begin : high;
    if (gap_end > cursor && gap_end - cursor >= page_size) {
      memory = try_near(cursor, target, page_size);
      if (memory != NULL) break;
    }
    if (end >= high) { cursor = high; break; }
    cursor = end;
  }
  fclose(maps);
  if (memory == NULL && high > cursor && high - cursor >= page_size)
    memory = try_near(cursor, target, page_size);
  return memory;
}

bool cf_patch_prepare(MemoryPatch *patch, uintptr_t address, uint32_t expected,
                      uintptr_t destination, bool link, char *error, size_t size) {
  if (patch == NULL || address == 0 || destination == 0 ||
      ((address | destination) & 3u) != 0 ||
      (expected & 0xfc000000u) != (link ? 0x94000000u : 0x14000000u)) {
    fail(error, size, "patch requires an aligned B/BL of the expected kind");
    return false;
  }
  memset(patch, 0, sizeof(*patch));
  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size < 4096 || ((size_t)page_size & ((size_t)page_size - 1u)) != 0 ||
      !page_protection(address, &patch->protection) ||
      (patch->protection & (PROT_READ | PROT_EXEC)) != (PROT_READ | PROT_EXEC)) {
    fail(error, size, "patch target is not readable executable memory");
    return false;
  }
  if (__atomic_load_n((uint32_t *)address, __ATOMIC_ACQUIRE) != expected) {
    fail(error, size, "patch target changed before preparation");
    return false;
  }
  patch->address = address;
  patch->original = expected;
  patch->page_size = (size_t)page_size;
  patch->page = address & ~(patch->page_size - 1u);
  patch->veneer = allocate_near(address, patch->page_size);
  if (patch->veneer == NULL) {
    fail(error, size, "no free mapping within ARM64 branch range");
    return false;
  }
  // LDR X17, literal at +8; BR X17; absolute destination. X17 is ABI scratch.
  const uint32_t code[2] = {0x58000051u, 0xd61f0220u};
  memcpy(patch->veneer, code, sizeof(code));
  memcpy((char *)patch->veneer + 8, &destination, sizeof(destination));
  __builtin___clear_cache((char *)patch->veneer, (char *)patch->veneer + 16);
  if (mprotect(patch->veneer, patch->page_size, PROT_READ | PROT_EXEC) != 0 ||
      !cf_branch_encode(address, (uintptr_t)patch->veneer, link, &patch->replacement)) {
    fail(error, size, "could not finalize branch veneer: %s", strerror(errno));
    cf_patch_discard_unpublished(patch, 1);
    return false;
  }
  return true;
}

bool cf_patch_commit(MemoryPatch *patches, size_t count, char *error, size_t size) {
  if (patches == NULL || count == 0 || count > MAX_PATCHES) {
    fail(error, size, "invalid patch batch");
    return false;
  }
  size_t opened = 0, written = 0;
  bool ok = false;
  for (size_t i = 0; i < count; ++i) {
    if (patches[i].veneer == NULL || patches[i].published ||
        __atomic_load_n((uint32_t *)patches[i].address, __ATOMIC_ACQUIRE) !=
            patches[i].original) {
      fail(error, size, "patch batch is unprepared or target changed");
      return false;
    }
    for (size_t j = 0; j < i; ++j) {
      if (patches[i].address == patches[j].address) {
        fail(error, size, "duplicate patch address");
        return false;
      }
    }
  }
  // Open all pages before publishing either branch. Each code write is one
  // aligned atomic B->B or BL->BL replacement; never a multiword entry patch.
  for (; opened < count; ++opened) {
    MemoryPatch *patch = &patches[opened];
    if (mprotect((void *)patch->page, patch->page_size,
                 patch->protection | PROT_WRITE) != 0) {
      fail(error, size, "mprotect before patch failed: %s", strerror(errno));
      goto finish;
    }
  }
  __atomic_thread_fence(__ATOMIC_RELEASE);
  for (; written < count; ++written) {
    MemoryPatch *patch = &patches[written];
    uint32_t expected = patch->original;
    if (!__atomic_compare_exchange_n((uint32_t *)patch->address, &expected,
                                     patch->replacement, false,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
      fail(error, size, "concurrent modification at patch target");
      goto rollback;
    }
    patch->published = true;
    __builtin___clear_cache((char *)patch->address, (char *)patch->address + 4);
  }
  ok = true;
  goto finish;

rollback:
  while (written > 0) {
    MemoryPatch *patch = &patches[--written];
    uint32_t expected = patch->replacement;
    if (!__atomic_compare_exchange_n((uint32_t *)patch->address, &expected,
                                     patch->original, false,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
      fail(error, size, "rollback refused: another writer changed the patch");
    }
    __builtin___clear_cache((char *)patch->address, (char *)patch->address + 4);
    // Keep published veneers mapped: another thread may still be inside one.
  }
finish:
  while (opened > 0) {
    MemoryPatch *patch = &patches[--opened];
    if (mprotect((void *)patch->page, patch->page_size, patch->protection) != 0) {
      fail(error, size, "page protection restore failed; code may be active: %s",
           strerror(errno));
      ok = false;
    }
  }
  return ok;
}

void cf_patch_discard_unpublished(MemoryPatch *patches, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (patches[i].veneer != NULL && !patches[i].published) {
      munmap(patches[i].veneer, patches[i].page_size);
      patches[i].veneer = NULL;
    }
  }
}
