/*
 * libanort_patch.c — disable libanort.so CRC dispatchers at runtime.
 *
 * Technique (matches the reference libLoader.so):
 *   - Resolve libanort.so load base via dl_iterate_phdr (name match).
 *   - For each dispatcher RVA, mprotect the page RW, write `RET`, then
 *     restore page protection.
 *   - Save clean bytes in Shadow Memory BEFORE writing, so a later RAM
 *     CRC read by libanort returns the original instruction stream.
 *
 * We only patch the three dispatcher entry points.  The kill/sendto/
 * GOT-poison/audit functions are deliberately left untouched: disabling
 * those earlier (file-patch experiments) still got the process killed by
 * libanogs.so, whereas dispatcher RET stops libanort from ever seeing the
 * libil2cpp patch at the source.
 */

#define _GNU_SOURCE
#include "libanort_patch.h"

#include <android/log.h>
#include <elf.h>
#include <link.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "shadow_memory.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "libanort", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "libanort", __VA_ARGS__)

/* AArch64 RET instruction. */
#define A64_RET 0xD65F03C0u

/* Dispatcher RVAs (module-relative to the load base). */
typedef struct {
  const char *name;
  uintptr_t   rva;
} Dispatcher;

static const Dispatcher k_dispatchers[] = {
  { "FUN_001B1D40", 0x000B1D40u },
  { "FUN_001B27B0", 0x000B27B0u },
  { "FUN_001B3220", 0x000B3220u },
};

struct find_ctx {
  uintptr_t base;
  char name[256];
};

static int on_phdr(struct dl_phdr_info *info, size_t size, void *data) {
  (void)size;
  struct find_ctx *ctx = (struct find_ctx *)data;
  if (info->dlpi_name != NULL &&
      strstr(info->dlpi_name, ctx->name) != NULL) {
    ctx->base = (uintptr_t)info->dlpi_addr;
  }
  return 0;
}

static uintptr_t find_module_base(const char *name) {
  struct find_ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.base = 0;
  strncpy(ctx.name, name, sizeof(ctx.name) - 1);
  dl_iterate_phdr(on_phdr, &ctx);
  return ctx.base;
}

static bool patch_one(uintptr_t base, const Dispatcher *d) {
  uintptr_t target = base + d->rva;
  long ps = sysconf(_SC_PAGESIZE);
  uintptr_t page = target & ~(uintptr_t)(ps - 1);
  if (page > 0 && page + (size_t)ps < base) return false; /* sanity */

  /* Save clean bytes in shadow memory BEFORE writing. */
  shadow_page_save(page, (size_t)ps);

  /* Open RW. */
  if (mprotect((void *)page, (size_t)ps, PROT_READ | PROT_WRITE) != 0) {
    LOGE("mprotect RW failed for %s at %p", d->name, (void *)target);
    return false;
  }

  uint32_t orig = *(volatile uint32_t *)target;
  *(volatile uint32_t *)target = A64_RET;
  __builtin___clear_cache((char *)target, (char *)target + 4);

  /* Restore RX (orig text pages are r-xp / rw-p per real layout; we keep
   * it executable + readable so subsequent inline reads still work). */
  if (mprotect((void *)page, (size_t)ps, PROT_READ | PROT_EXEC) != 0) {
    /* Non-fatal: page still writable, but we already wrote the patch. */
    LOGE("mprotect RX failed for %s at %p", d->name, (void *)target);
  }

  shadow_page_protect(page, (size_t)ps);

  LOGI("dispatcher %s @ %p: %08x -> RET", d->name, (void *)target, orig);
  return true;
}

bool cf_libanort_patch_dispatchers(void) {
  uintptr_t base = find_module_base("libanort.so");
  if (base == 0) {
    LOGE("libanort.so not loaded yet");
    return false;
  }
  LOGI("libanort.so base = %p", (void *)base);

  int ok = 0;
  for (size_t i = 0; i < sizeof(k_dispatchers) / sizeof(k_dispatchers[0]); ++i) {
    if (patch_one(base, &k_dispatchers[i])) ++ok;
  }

  if (ok == 0) {
    LOGE("all dispatcher patches failed");
    return false;
  }
  LOGI("libanort.dispatcher: %d/%d patched (RET)", ok,
       (int)(sizeof(k_dispatchers) / sizeof(k_dispatchers[0])));
  return true;
}
