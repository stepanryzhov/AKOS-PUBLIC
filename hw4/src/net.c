#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "net.h"

static int socket_address(const char* text, int port, struct sockaddr_in* address) {
    *address = (struct sockaddr_in){.sin_family = AF_INET, .sin_port = htons((unsigned short)port)};
    if (inet_pton(AF_INET, text, &address->sin_addr) == 1) return 0;
    errno = EINVAL;
    return -1;
}

static int nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void no_delay(int fd) {
    int enabled = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
}

int net_listen(const char* text, int port) {
    struct sockaddr_in address;
    if (socket_address(text, port, &address) < 0) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0 ||
        nonblocking(fd) < 0 || bind(fd, (struct sockaddr*)&address, sizeof(address)) < 0 ||
        listen(fd, 128) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

int net_accept(int listener) {
    int fd = accept(listener, NULL, NULL);
    if (fd < 0) return -1;
    if (nonblocking(fd) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    no_delay(fd);
    return fd;
}

static int wait_socket(int fd, short events, long long deadline) {
    for (;;) {
        if (interrupted) { errno = EINTR; return -1; }
        long long remaining = deadline ? deadline - monotonic_ms() : 100;
        if (remaining <= 0) { errno = ETIMEDOUT; return -1; }
        struct pollfd descriptor = {.fd = fd, .events = events};
        int result = poll(&descriptor, 1, remaining > 100 ? 100 : (int)remaining);
        if (result < 0 && errno == EINTR) continue;
        if (result < 0) return -1;
        // HUP тоже будит чтение: recv вернёт оставшиеся данные или EOF.
        if (result) return 0;
    }
}

int net_connect(const char* text, int port) {
    struct sockaddr_in address;
    if (socket_address(text, port, &address) < 0) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (nonblocking(fd) < 0) goto error;
    if (connect(fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        if (errno != EINPROGRESS || wait_socket(fd, POLLOUT, monotonic_ms() + 5000) < 0)
            goto error;
        int failure = 0;
        socklen_t size = sizeof(failure);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &failure, &size) < 0) goto error;
        if (failure) { errno = failure; goto error; }
    }
    no_delay(fd);
    return fd;
error: {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
}

int net_send(int fd, const char* text) {
    size_t length = strlen(text);
    long long deadline = monotonic_ms() + 5000;
    while (length) {
        if (interrupted) { errno = EINTR; return -1; }
        ssize_t sent = send(fd, text, length, 0);
        if (sent < 0 && errno == EINTR) continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (wait_socket(fd, POLLOUT, deadline) < 0) return -1;
            continue;
        }
        if (sent <= 0) return -1;
        text += sent;
        length -= (size_t)sent;
    }
    return 0;
}

int net_read_line(int fd, LineReader* reader, char* line, int timeout_ms) {
    long long deadline = timeout_ms ? monotonic_ms() + timeout_ms : 0;
    for (;;) {
        char* end = memchr(reader->buffer, '\n', reader->used);
        if (end) {
            size_t length = (size_t)(end - reader->buffer);
            memcpy(line, reader->buffer, length);
            line[length] = '\0';
            reader->used -= length + 1;
            memmove(reader->buffer, end + 1, reader->used);
            return 1;
        }
        if (reader->used == sizeof(reader->buffer) - 1) { errno = EPROTO; return -1; }
        if (wait_socket(fd, POLLIN, deadline) < 0) return -1;
        ssize_t size = recv(fd, reader->buffer + reader->used,
                            sizeof(reader->buffer) - 1 - reader->used, 0);
        if (size < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (size <= 0) {
            if (size == 0 && reader->used) { errno = EPROTO; return -1; }
            return (int)size;
        }
        if (memchr(reader->buffer + reader->used, '\0', (size_t)size)) {
            errno = EPROTO;
            return -1;
        }
        reader->used += (size_t)size;
    }
}

const char* lot_state_name(LotState state) {
    switch (state) {
        case WAITING: return "ожидает";
        case OPEN: return "открыт";
        case SOLD: return "продан";
        case UNSOLD: return "не продан";
        case CANCELLED: return "отменён";
    }
    return "неизвестно";
}
