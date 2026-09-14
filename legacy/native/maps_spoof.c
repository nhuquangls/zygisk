/*
 * maps_spoof.c — /proc/self/maps filtering via openat() hook
 *
 * When any library (libanort, libanogs, etc.) reads /proc/self/maps we
 * return a filtered copy that:
 *   1. Removes lines containing our module names
 *   2. Downgrades rwxp → r-xp for non-main-executable mappings
 *
 * Uses Dobby for inline hooking and memfd_create (API 29+) with a
 * fallback to a temporary file for the filtered output.
 */

#define _GNU_SOURCE
#include "maps_spoof.h"

#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* memfd_create requires API 29+; declare manually for older NDK targets. */
#if __ANDROID_API__ < 29
#include <sys/syscall.h>
static inline int memfd_create(const char *name, unsigned int flags) {
  return (int)syscall(__NR_memfd_create, name, flags);
}
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#endif

#include "dobby.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "MapsSpoof", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "MapsSpoof", __VA_ARGS__)

/* ------------------------------------------------------------------ */
/*  Original function pointer                                          */
/* ------------------------------------------------------------------ */

static int (*orig_openat)(int dirfd, const char *pathname, int flags, ...) = NULL;

/* ------------------------------------------------------------------ */
/*  Filter logic                                                       */
/* ------------------------------------------------------------------ */

/* Module name substrings to suppress.  Add more as needed. */
static const char *k_filtered_names[] = {
    "libcfrt.so",
    "libhook.so",
    "aimhook",
    "/data/adb/modules/",
    NULL,
};

/* Maximum output buffer for the filtered maps. */
#define MAPS_BUF_SIZE (256 * 1024)

/* Returns true if the line contains any of the filtered substrings. */
static bool should_filter_line(const char *line, size_t len) {
  for (const char **name = k_filtered_names; *name; ++name) {
    if (memmem(line, len, *name, strlen(*name)) != NULL) {
      return true;
    }
  }
  return false;
}

/* Downgrade rwxp → r-xp in a single maps line (in-place). */
static void downgrade_permissions(char *line, size_t len) {
  /* Maps format: addr-addr perms offset dev inode pathname
   * Permissions are at a fixed position after the first space. */
  char *perm = strchr(line, ' ');
  if (perm && (size_t)(perm - line) < len - 4) {
    if (perm[1] == 'r' && perm[2] == 'w' && perm[3] == 'x' && perm[4] == 'p') {
      perm[2] = '-';  /* w → - */
    }
  }
}

/* Read the real /proc/self/maps, filter, return bytes written. */
static ssize_t read_and_filter(int real_fd, char *out, size_t out_cap) {
  /* Read entire file.  /proc/self/maps is typically 20-60 KB. */
  char *buf = (char *)malloc(MAPS_BUF_SIZE);
  if (!buf) return -1;

  ssize_t total = 0;
  ssize_t n;
  while ((n = read(real_fd, buf + total, MAPS_BUF_SIZE - total)) > 0) {
    total += n;
    if (total >= MAPS_BUF_SIZE) break;
  }
  if (total <= 0) { free(buf); return 0; }

  /* Process line by line. */
  size_t in_pos = 0;
  size_t out_pos = 0;

  while (in_pos < (size_t)total && out_pos < out_cap) {
    size_t line_start = in_pos;
    while (in_pos < (size_t)total && buf[in_pos] != '\n') in_pos++;
    size_t line_len = in_pos - line_start;
    if (in_pos < (size_t)total) in_pos++;  /* skip \n */

    if (!should_filter_line(buf + line_start, line_len)) {
      /* Copy line to output. */
      size_t copy_len = line_len;
      if (out_pos + copy_len + 1 > out_cap) break;
      memcpy(out + out_pos, buf + line_start, copy_len);
      out_pos += copy_len;

      /* Add newline. */
      if (out_pos < out_cap) out[out_pos++] = '\n';
    }
  }

  free(buf);
  return (ssize_t)out_pos;
}

/* ------------------------------------------------------------------ */
/*  Hooked openat                                                      */
/* ------------------------------------------------------------------ */

static int hooked_openat(int dirfd, const char *pathname, int flags, ...) {
  va_list args;
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_start(args, flags);
    mode = va_arg(args, mode_t);
    va_end(args);
  }

  /* Only intercept /proc/self/maps reads. */
  if (pathname != NULL && strcmp(pathname, "/proc/self/maps") == 0) {
    int real_fd = orig_openat(dirfd, pathname, flags, mode);
    if (real_fd < 0) return real_fd;

    char *filtered = (char *)malloc(MAPS_BUF_SIZE);
    if (!filtered) {
      close(real_fd);
      /* Fall through to return unfiltered. */
      return orig_openat(dirfd, pathname, flags, mode);
    }

    ssize_t filtered_len = read_and_filter(real_fd, filtered, MAPS_BUF_SIZE);
    close(real_fd);

    if (filtered_len <= 0) {
      free(filtered);
      return orig_openat(dirfd, pathname, flags, mode);
    }

    /* Create an in-memory file with the filtered content. */
    int memfd = (int)memfd_create("maps", MFD_CLOEXEC);
    if (memfd < 0) {
      /* Fallback: temp file that auto-deletes on close. */
      char tmpl[] = "/data/local/tmp/.maps_XXXXXX";
      memfd = mkstemp(tmpl);
      if (memfd >= 0) unlink(tmpl);
    }

    if (memfd >= 0) {
      write(memfd, filtered, (size_t)filtered_len);
      lseek(memfd, 0, SEEK_SET);
      LOGI("filtered /proc/self/maps: %zd → %zd bytes (fd=%d)",
           (ssize_t)0, filtered_len, memfd);
    }

    free(filtered);
    return memfd;
  }

  /* Not maps — forward to original. */
  va_start(args, flags);
  if (flags & O_CREAT) {
    mode = va_arg(args, mode_t);
  }
  va_end(args);
  return orig_openat(dirfd, pathname, flags, mode);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

bool maps_spoof_init(void) {
  /* Resolve the real openat from libc directly.  dlsym(RTLD_NEXT) fails here
   * because the payload is dlopen()ed by the Zygisk loader into a dedicated
   * namespace, so RTLD_NEXT does not walk to libc.  Opening libc.so by name
   * and resolving the symbol there yields the true libc address. */
  void *libc = dlopen("libc.so", RTLD_NOW);
  if (libc == NULL) libc = dlopen("libc.so", RTLD_LAZY);
  if (libc == NULL) {
    LOGE("dlopen(libc.so) failed: %s", dlerror());
    return false;
  }
  orig_openat = (int (*)(int, const char *, int, ...))dlsym(libc, "openat");
  dlerror();
  if (orig_openat == NULL) {
    LOGE("dlsym(libc.so, openat) failed: %s", dlerror());
    return false;
  }

  /* Install Dobby inline hook. */
  int ret = DobbyHook((void *)orig_openat, (void *)hooked_openat,
                       (void **)&orig_openat);
  if (ret != 0) {
    LOGE("DobbyHook(openat) failed: %d", ret);
    return false;
  }

  LOGI("openat hooked at %p — /proc/self/maps spoofing active", orig_openat);
  return true;
}
