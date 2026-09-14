#ifndef CF_MAPS_SPOOF_H
#define CF_MAPS_SPOOF_H

#include <stdbool.h>

/*
 * /proc/self/maps spoofing engine.
 *
 * Hooks libc openat() so that any reader (libanort / libanogs) gets a
 * filtered copy where:
 *   - Lines containing our module names are removed
 *   - "rwxp" permissions are downgraded to "r-xp"
 *
 * Uses Dobby for inline hooking and memfd_create for the filtered copy.
 */

/* Install the openat hook. Call once at startup. */
bool maps_spoof_init(void);

#endif /* CF_MAPS_SPOOF_H */
