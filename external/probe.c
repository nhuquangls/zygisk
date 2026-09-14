#include "remote.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    CfRemote r;
    pid_t pid = cf_remote_find();
    if (!cf_remote_open(&r, pid)) { perror("target open"); return 1; }
    CfModule module;
    if (!cf_remote_module(&r, "libil2cpp.so", &module)) { perror("module"); cf_remote_close(&r); return 1; }
    fprintf(stderr, "pid=%d bias=0x%lx build_id=%s\n", pid, (unsigned long)module.load_bias, module.build_id);
    if (argc == 4 && (!strcmp(argv[1], "--rva") || !strcmp(argv[1], "--address"))) {
        char *end;
        uintptr_t address = strtoull(argv[2], &end, 0);
        if (*end) return 2;
        if (!strcmp(argv[1], "--rva")) {
            if (address > UINTPTR_MAX-module.load_bias) return 2;
            address += module.load_bias;
        }
        size_t size = strtoull(argv[3], &end, 0);
        if (*end || !size || size > 4096) return 2;
        unsigned char bytes[4096];
        if (!cf_remote_read(&r,address,bytes,size)) { perror("read"); return 1; }
        for (size_t i=0;i<size;++i) printf("%02x",bytes[i]);
        puts("");
    }
    cf_remote_close(&r);
    return 0;
}
