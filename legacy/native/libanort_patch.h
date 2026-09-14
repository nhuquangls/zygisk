#ifndef CF_LIBANORT_PATCH_H
#define CF_LIBANORT_PATCH_H

#include <stdbool.h>

/*
 * libanort.so dispatcher neutralization ("pre-emptive patching").
 *
 * libanort.so runs its CRC check on libil2cpp.so by iterating a callback
 * table from three dispatcher functions.  If those dispatchers are turned
 * into `RET`, the library can never invoke the CRC-comparison callback, so
 * it never sees that libil2cpp.so has been patched.  This is the layer the
 * reference mod (libLoader.so) uses in addition to Shadow Memory; shadow
 * memory alone is only a secondary safety net.
 *
 * Dispatcher RVAs (module-relative, image base 0x00100000):
 *   FUN_001B1D40 -> 0x000B1D40
 *   FUN_001B27B0 -> 0x000B27B0
 *   FUN_001B3220 -> 0x000B3220
 *
 * Patches are applied ONLY in RAM (mprotect + write + clear_cache); the
 * on-disk file is never modified.  Patched sites are covered by Shadow
 * Memory so a later RAM CRC read by libanort returns the original bytes.
 */

/* Resolve libanort.so load base and disable the three dispatchers.
 * Returns true if all three were patched. */
bool cf_libanort_patch_dispatchers(void);

#endif /* CF_LIBANORT_PATCH_H */
