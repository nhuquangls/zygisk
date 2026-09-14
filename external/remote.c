#define _GNU_SOURCE
#include "remote.h"
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

static CfRemote *active;
static bool identity(pid_t pid, uint64_t *ticks) {
    char path[64], buffer[4096];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t n = read(fd, buffer, sizeof(buffer)-1);
    close(fd);
    if (n <= 0) return false;
    buffer[n] = 0;
    if (strcmp(buffer, CF_GAME_PROCESS)) { errno = EINVAL; return false; }
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    n = read(fd, buffer, sizeof(buffer)-1);
    close(fd);
    if (n <= 0) return false;
    buffer[n] = 0;
    char *cursor = strrchr(buffer, ')');
    if (!cursor || cursor[1] != ' ') return false;
    cursor += 2;
    for (unsigned field = 3; field < 22; ++field) {
        cursor = strchr(cursor, ' ');
        if (!cursor) return false;
        while (*cursor == ' ') ++cursor;
    }
    char *end;
    *ticks = strtoull(cursor, &end, 10);
    return end != cursor && *ticks != 0;
}
pid_t cf_remote_find(void) {
    DIR *dir = opendir("/proc");
    if (!dir) return -1;
    pid_t found = -1;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        char *end;
        long value = strtol(entry->d_name, &end, 10);
        if (*end || value <= 0 || value > INT_MAX) continue;
        uint64_t ticks;
        if (identity((pid_t)value, &ticks)) { found = (pid_t)value; break; }
    }
    closedir(dir);
    return found;
}
bool cf_remote_open(CfRemote *r, pid_t pid) {
    if (!r || pid <= 0 || pid == getpid()) { errno = EINVAL; return false; }
    *r = (CfRemote){.pid = pid, .pidfd = -1};
    if (!identity(pid, &r->start_ticks)) return false;
    r->pidfd = (int)syscall(__NR_pidfd_open, pid, 0);
    if (r->pidfd < 0) return false; // Require a kernel process-lifetime handle.
    uint64_t after;
    if (!identity(pid, &after) || after != r->start_ticks || !cf_remote_alive(r)) {
        cf_remote_close(r); errno = ESRCH; return false;
    }
    return true;
}
bool cf_remote_alive(const CfRemote *r) {
    if (!r || r->pidfd < 0) return false;
    struct pollfd p = {.fd = r->pidfd, .events = POLLIN};
    int result;
    do { result = poll(&p, 1, 0); } while (result < 0 && errno == EINTR);
    return result == 0;
}
void cf_remote_close(CfRemote *r) {
    if (!r) return;
    if (active == r) active = NULL;
    if (r->pidfd >= 0) close(r->pidfd);
    r->pidfd = -1; r->pid = -1;
}
bool cf_remote_read(CfRemote *r, uintptr_t address, void *out, size_t size) {
    if (!out || !size || size > 4096) { errno = EINVAL; return false; }
    if (!r || !address || address > UINTPTR_MAX - (size-1) || !cf_remote_alive(r)) {
        memset(out, 0, size); errno = ESRCH; return false;
    }
    struct iovec local = {.iov_base=out, .iov_len=size};
    struct iovec remote = {.iov_base=(void *)address, .iov_len=size};
    ssize_t n;
    do { n = process_vm_readv(r->pid, &local, 1, &remote, 1, 0); }
    while (n < 0 && errno == EINTR);
    int error = n < 0 ? errno : EIO;
    ++r->reads;
    if (n != (ssize_t)size || !cf_remote_alive(r)) {
        memset(out, 0, size); ++r->failures; errno = error; return false;
    }
    r->bytes += size;
    return true;
}
void cf_remote_use(CfRemote *r) { active = r; }
bool cf_read_target(uintptr_t address, void *out, size_t size) {
    return cf_remote_read(active, address, out, size);
}
bool cf_remote_string(CfRemote *r, uintptr_t p, char *out, size_t capacity) {
    if (!out || capacity < 2 || capacity > 4096) return false;
    out[0] = 0;
    for (size_t used = 0; used < capacity-1;) {
        if (p > UINTPTR_MAX - used) break;
        size_t chunk = 32, boundary = 4096 - ((p+used) & 4095);
        if (chunk > boundary) chunk = boundary;
        if (chunk > capacity-1-used) chunk = capacity-1-used;
        if (!cf_remote_read(r, p+used, out+used, chunk)) break;
        if (memchr(out+used, 0, chunk)) return true;
        used += chunk;
    }
    out[0] = 0;
    return false;
}
bool cf_remote_module(CfRemote *r, const char *name, CfModule *out) {
    if (!r || !name || !out || strchr(name, '/')) return false;
    *out = (CfModule){0};
    char path[64], line[2048];
    snprintf(path, sizeof(path), "/proc/%d/maps", r->pid);
    FILE *maps = fopen(path, "re");
    if (!maps) return false;
    uintptr_t header_address = 0;
    while (fgets(line, sizeof(line), maps)) {
        unsigned long begin, end, offset;
        char permissions[5]; int used = 0;
        if (sscanf(line, "%lx-%lx %4s %lx %*s %*s %n", &begin, &end,
                   permissions, &offset, &used) != 4 || !used || offset != 0 || permissions[0] != 'r') continue;
        char *base = strrchr(line+used, '/');
        if (!base) continue;
        base[strcspn(base, "\r\n")] = 0;
        if (!strcmp(base+1, name)) { header_address = begin; break; }
    }
    fclose(maps);
    if (!header_address) { errno = ENOENT; return false; }
    Elf64_Ehdr eh;
    if (!cf_remote_read(r, header_address, &eh, sizeof(eh)) ||
        memcmp(eh.e_ident, ELFMAG, SELFMAG) || eh.e_ident[EI_CLASS] != ELFCLASS64 ||
        eh.e_ident[EI_DATA] != ELFDATA2LSB || eh.e_machine != EM_AARCH64 ||
        eh.e_phentsize != sizeof(Elf64_Phdr) || !eh.e_phnum || eh.e_phnum > 64 ||
        eh.e_phoff > 65536 || header_address > UINTPTR_MAX-eh.e_phoff) return false;
    Elf64_Phdr ph[64];
    if (!cf_remote_read(r, header_address+eh.e_phoff, ph, eh.e_phnum*sizeof(*ph))) return false;
    bool have_bias = false;
    for (unsigned i=0; i<eh.e_phnum; ++i) if (ph[i].p_type == PT_LOAD && ph[i].p_offset == 0) {
        if (header_address < ph[i].p_vaddr) return false;
        out->load_bias = header_address-ph[i].p_vaddr; have_bias = true; break;
    }
    if (!have_bias) return false;
    out->image_begin = UINTPTR_MAX;
    for (unsigned i=0; i<eh.e_phnum; ++i) {
        if (ph[i].p_vaddr > UINTPTR_MAX-out->load_bias) return false;
        uintptr_t begin = out->load_bias+ph[i].p_vaddr;
        if (ph[i].p_memsz > UINTPTR_MAX-begin) return false;
        if (ph[i].p_type == PT_LOAD && ph[i].p_memsz) {
            if (begin < out->image_begin) out->image_begin = begin;
            if (begin+ph[i].p_memsz > out->image_end) out->image_end = begin+ph[i].p_memsz;
        }
        if (ph[i].p_type != PT_NOTE || ph[i].p_memsz > 65536) continue;
        size_t cursor = 0;
        while (cursor <= ph[i].p_memsz && ph[i].p_memsz-cursor >= sizeof(Elf64_Nhdr)) {
            Elf64_Nhdr note;
            if (!cf_remote_read(r, begin+cursor, &note, sizeof(note))) break;
            cursor += sizeof(note);
            size_t names = ((size_t)note.n_namesz+3)&~(size_t)3;
            size_t descs = ((size_t)note.n_descsz+3)&~(size_t)3;
            if (names > ph[i].p_memsz-cursor) break;
            size_t desc = cursor+names;
            if (descs > ph[i].p_memsz-desc) break;
            if (note.n_type == NT_GNU_BUILD_ID && note.n_namesz == 4 && note.n_descsz && note.n_descsz <= 64) {
                char owner[4]; unsigned char bytes[64];
                if (cf_remote_read(r, begin+cursor, owner, 4) && !memcmp(owner,"GNU",4) &&
                    cf_remote_read(r, begin+desc, bytes, note.n_descsz)) {
                    const char hex[] = "0123456789abcdef";
                    for (unsigned j=0; j<note.n_descsz; ++j) {
                        out->build_id[j*2] = hex[bytes[j]>>4]; out->build_id[j*2+1] = hex[bytes[j]&15];
                    }
                    out->build_id[note.n_descsz*2] = 0;
                }
            }
            cursor = desc+descs;
        }
    }
    return out->image_begin < out->image_end && out->build_id[0] && cf_remote_alive(r);
}
