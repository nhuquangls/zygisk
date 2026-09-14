#define _GNU_SOURCE
#include "readonly_memory.h"
#include <errno.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

bool cf_read_self(uintptr_t source, void *destination, size_t size) {
    if (destination == NULL || size == 0 || size > CF_READ_MAX) {
        errno = EINVAL;
        return false;
    }
    if (source == 0 || source > UINTPTR_MAX - (size - 1)) {
        memset(destination, 0, size);
        errno = EINVAL;
        return false;
    }
    struct iovec local = { .iov_base = destination, .iov_len = size };
    struct iovec remote = { .iov_base = (void *)source, .iov_len = size };
    ssize_t transferred;
    do {
        transferred = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    } while (transferred < 0 && errno == EINTR);
    if (transferred != (ssize_t)size) {
        int failure = transferred < 0 ? errno : EIO;
        memset(destination, 0, size);
        errno = failure;
        return false;
    }
    return true;
}

bool cf_follow_chain(uintptr_t base, const size_t *offsets, size_t count,
                     uintptr_t *result) {
    if (result == NULL) return false;
    *result = 0;
    if (base == 0 || offsets == NULL || count == 0 || count > CF_CHAIN_MAX)
        return false;
    uintptr_t current = base;
    for (size_t i = 0; i < count; ++i) {
        if (current > UINTPTR_MAX - offsets[i]) return false;
        uintptr_t address = current + offsets[i];
        if ((address & (sizeof(uintptr_t) - 1)) != 0 ||
            !cf_read_self(address, &current, sizeof(current)) || current == 0)
            return false;
    }
    *result = current;
    return true;
}
