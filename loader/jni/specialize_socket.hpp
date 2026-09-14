#pragma once

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// A descriptor number may be closed and reused during specialization. Never
// hand off (or close) a different object that happens to reuse that number.
class SpecializeSocket final {
public:
    SpecializeSocket() = default;
    SpecializeSocket(const SpecializeSocket &) = delete;
    SpecializeSocket &operator=(const SpecializeSocket &) = delete;
    ~SpecializeSocket() { reset(); }

    bool capture(int fd) {
        reset();
        if (fd < 0) return false;
        uint64_t cookie = 0;
        if (!readCookie(fd, &cookie) || !readSocketLink(fd, link_, sizeof(link_))) {
            close(fd);
            return false;
        }
        fd_ = fd;
        cookie_ = cookie;
        return true;
    }

    int get() const { return fd_; }
    int error() const { return error_; }

    int take() {
        const int fd = fd_;
        fd_ = -1;
        error_ = 0;
        uint64_t current = 0;
        if (fd < 0) { error_ = EBADF; return -1; }
        if (!readCookie(fd, &current)) {
            error_ = errno;
            if (error_ != EACCES && error_ != EPERM) return -1;
            // App policy may deny socket getattr/getopt even for an inherited
            // descriptor. The kernel's /proc/self/fd link still identifies the
            // socket inode; never accept a different socket or regular file.
            char link[64];
            if (!readSocketLink(fd, link, sizeof(link))) { error_ = errno; return -1; }
            if (strcmp(link, link_) != 0) { error_ = ESTALE; return -1; }
            error_ = 0;
            return fd;
        }
        if (current != cookie_) { error_ = ESTALE; return -1; }
        return fd;
    }

    void reset() {
        const int fd = take();
        if (fd >= 0) close(fd);
    }

private:
    static bool readSocketLink(int fd, char *link, size_t capacity) {
        char path[64];
        snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
        const ssize_t size = readlink(path, link, capacity - 1);
        if (size < 0) return false;
        if (size < 10 || static_cast<size_t>(size) >= capacity - 1 ||
            strncmp(link, "socket:[", 8) != 0 || link[size-1] != ']') {
            errno = ENOTSOCK;
            return false;
        }
        for (ssize_t i = 8; i < size-1; ++i) if (link[i] < '0' || link[i] > '9') {
            errno = ENOTSOCK;
            return false;
        }
        link[size] = '\0';
        return true;
    }
    // Socket identity through the socket API, without filesystem getattr on a
    // descriptor created under the companion's different SELinux domain.
    static bool readCookie(int fd, uint64_t *cookie) {
        socklen_t size = sizeof(*cookie);
        if (getsockopt(fd, SOL_SOCKET, SO_COOKIE, cookie, &size) != 0) return false;
        if (size != sizeof(*cookie)) { errno = EPROTO; return false; }
        return true;
    }
    int fd_ = -1;
    int error_ = 0;
    uint64_t cookie_ = 0;
    char link_[64] = {};
};
