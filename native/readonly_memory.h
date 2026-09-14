#ifndef CF_READONLY_MEMORY_H
#define CF_READONLY_MEMORY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define CF_READ_MAX 4096u
#define CF_CHAIN_MAX 8u
// Destination is caller-owned writable storage. Partial reads are cleared.
// No direct game-memory dereference, permission change or signal handler.
bool cf_read_self(uintptr_t source, void *destination, size_t size);
// For each hop: pointer = read(pointer + offsets[i]). Layouts must be verified
// separately. This function does not assume any Unity/game object layout.
bool cf_follow_chain(uintptr_t base, const size_t *offsets, size_t count,
                     uintptr_t *result);
#endif
