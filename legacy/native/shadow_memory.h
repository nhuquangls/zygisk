#ifndef CF_SHADOW_MEMORY_H
#define CF_SHADOW_MEMORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Shadow Memory Engine — CRC-scan invisible patching for libil2cpp.so
 *
 * After patching code in a PT_LOAD page (e.g. writing a branch veneer),
 * call shadow_page_save() BEFORE the write and shadow_page_protect()
 * AFTER the write.  The SIGSEGV handler then serves clean (original)
 * bytes to any reader (libanort CRC scanner) while the CPU still
 * executes the patched branch from the same page.
 *
 * Flow:
 *   shadow_init()                         ← once at startup
 *   shadow_page_save(page, size)          ← before patching
 *   <write branch veneer / DobbyHook>
 *   shadow_page_protect(page, size)       ← after patching
 *   → readers get SIGSEGV → handler emulates LDR from clean backup
 *   → CRC matches original bytes
 */

#define SHADOW_MAX_PAGES 32

typedef struct {
  uintptr_t page_start;
  size_t    page_size;
  uint8_t  *clean_backup;   /* original page contents */
  int       original_prot;  /* PROT_READ|PROT_EXEC before we stripped PROT_READ */
  bool      active;
} ShadowPage;

/* One-time: setup sigaltstack + install SA_SIGINFO handler. */
bool shadow_init(void);

/* Save the current page contents BEFORE applying a patch.
 * Must be called while the page still has PROT_READ. */
bool shadow_page_save(uintptr_t page_start, size_t page_size);

/* Strip PROT_READ from the page (leave PROT_EXEC only).
 * Call AFTER the patch bytes have been written. */
bool shadow_page_protect(uintptr_t page_start, size_t page_size);

/* Restore all pages and uninstall handler (for testing). */
void shadow_cleanup(void);

#endif /* CF_SHADOW_MEMORY_H */
