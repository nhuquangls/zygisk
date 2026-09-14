#ifndef CF_REMOTE_H
#define CF_REMOTE_H
#include "../native/readonly_module.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define CF_GAME_PROCESS "com.vnggames.cfl.crossfirelegends"
typedef struct CfRemote {
    pid_t pid;
    int pidfd;
    uint64_t start_ticks;
    uint64_t reads, bytes, failures;
} CfRemote;
pid_t cf_remote_find(void);
bool cf_remote_open(CfRemote *, pid_t);
bool cf_remote_alive(const CfRemote *);
void cf_remote_close(CfRemote *);
bool cf_remote_read(CfRemote *, uintptr_t, void *, size_t);
bool cf_remote_string(CfRemote *, uintptr_t, char *, size_t);
bool cf_remote_module(CfRemote *, const char *, CfModule *);
void cf_remote_use(CfRemote *);
bool cf_read_target(uintptr_t, void *, size_t);
#endif
