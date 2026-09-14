// Android/Linux regression test for descriptor closure and reuse across the
// specialization boundary. No game process, module load or input is involved.
#include <errno.h>
#include <sys/socket.h>
static int denied_option = 0;
static int test_getsockopt(int fd, int level, int option, void *value, socklen_t *size) {
    if (denied_option) { errno = denied_option; return -1; }
    return getsockopt(fd, level, option, value, size);
}
#define getsockopt test_getsockopt
#include "../loader/jni/specialize_socket.hpp"
#undef getsockopt
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/socket.h>

int main() {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
    {
        SpecializeSocket pending;
        assert(pending.capture(pair[0]));
        assert(pending.take() == pair[0]);
        assert(pending.take() == -1);
    }
    assert(write(pair[0], "a", 1) == 1);
    char byte = 0;
    assert(read(pair[1], &byte, 1) == 1 && byte == 'a');
    close(pair[0]); close(pair[1]);

    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
    {
        SpecializeSocket pending;
        assert(pending.capture(pair[0]));
        close(pair[0]);
        assert(pending.take() == -1);
    }
    close(pair[1]);

    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
    int replacement[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, replacement) == 0);
    {
        SpecializeSocket pending;
        assert(pending.capture(pair[0]));
        assert(dup2(replacement[0], pair[0]) == pair[0]);
        assert(pending.take() == -1);
    }
    assert(write(pair[0], "b", 1) == 1);
    assert(read(replacement[1], &byte, 1) == 1 && byte == 'b');
    close(pair[0]); close(pair[1]);
    close(replacement[0]); close(replacement[1]);

    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
    const int other = open("/dev/null", O_RDWR | O_CLOEXEC);
    assert(other >= 0);
    {
        SpecializeSocket pending;
        assert(pending.capture(pair[0]));
        assert(dup2(other, pair[0]) == pair[0]);
        // Destructor must leave the replacement FD open.
    }
    assert(fcntl(pair[0], F_GETFD) >= 0);
    close(pair[0]); close(pair[1]); close(other);

    const int invalid = open("/dev/null", O_RDONLY | O_CLOEXEC);
    assert(invalid >= 0);
    {
        SpecializeSocket pending;
        assert(!pending.capture(invalid));
        assert(pending.take() == -1);
    }
    assert(fcntl(invalid, F_GETFD) == -1);
    for (int replaced = 0; replaced < 3; ++replaced) {
        assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
        assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, replacement) == 0);
        const int file = open("/dev/null", O_RDONLY | O_CLOEXEC);
        assert(file >= 0);
        {
            SpecializeSocket pending;
            assert(pending.capture(pair[0]));
            if (replaced) assert(dup2(replaced == 1 ? replacement[0] : file, pair[0]) == pair[0]);
            denied_option = EACCES;
            assert(pending.take() == (replaced ? -1 : pair[0]));
            denied_option = 0;
        }
        assert(fcntl(pair[0], F_GETFD) >= 0);
        close(pair[0]); close(pair[1]); close(replacement[0]); close(replacement[1]); close(file);
    }
    puts("8 specialize socket regression cases passed");
}
