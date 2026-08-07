#define _GNU_SOURCE
#include "qtc/ipc.h"
#include "qtc/util.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void qtc_ipc_reader_init(qtc_ipc_reader *r) { memset(r, 0, sizeof(*r)); }

int qtc_ipc_reader_feed(qtc_ipc_reader *r, const uint8_t *data, size_t len,
                        qtc_ipc_frame_cb callback, void *userdata) {
    while (len > 0) {
        if (r->header_len < 5) {
            size_t need = 5 - r->header_len, take = len < need ? len : need;
            memcpy(r->header + r->header_len, data, take);
            r->header_len += take; data += take; len -= take;
            if (r->header_len < 5) continue;
            r->expected = le32(r->header);
            r->type = r->header[4];
            if (r->expected > QTC_MAX_FRAME) { errno = EMSGSIZE; return -1; }
            r->payload_len = 0;
            if (r->expected == 0) {
                qtc_ipc_frame f = {.type = r->type, .length = 0};
                callback(&f, userdata); r->header_len = 0;
            }
        }
        if (r->header_len == 5 && r->expected > 0) {
            size_t need = r->expected - r->payload_len, take = len < need ? len : need;
            memcpy(r->payload + r->payload_len, data, take);
            r->payload_len += take; data += take; len -= take;
            if (r->payload_len == r->expected) {
                qtc_ipc_frame f = {.type = r->type, .length = r->expected};
                memcpy(f.payload, r->payload, r->expected);
                callback(&f, userdata);
                r->header_len = 0; r->payload_len = 0; r->expected = 0;
            }
        }
    }
    return 0;
}

static int write_all_flags(int fd, const void *data, size_t len, int flags) {
    const uint8_t *p = data; size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, p + off, len - off, MSG_NOSIGNAL | flags);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) { errno = EPIPE; return -1; }
        off += (size_t)n;
    }
    return 0;
}

int qtc_ipc_send(int fd, uint8_t type, const void *payload, uint32_t length) {
    if (length > QTC_MAX_FRAME) { errno = EMSGSIZE; return -1; }
    uint8_t header[5] = {(uint8_t)length, (uint8_t)(length >> 8), (uint8_t)(length >> 16),
                         (uint8_t)(length >> 24), type};
    if (write_all_flags(fd, header, sizeof(header), 0) != 0) return -1;
    return length == 0 ? 0 : write_all_flags(fd, payload, length, 0);
}

int qtc_ipc_send_nonblocking(int fd, uint8_t type, const void *payload, uint32_t length) {
    if (length > QTC_MAX_FRAME) { errno = EMSGSIZE; return -1; }
    uint8_t header[5] = {(uint8_t)length, (uint8_t)(length >> 8), (uint8_t)(length >> 16),
                         (uint8_t)(length >> 24), type};
    /* Incremental updates must never stall the serial event loop behind a slow
     * or suspended terminal client. A partially written frame is handled by
     * dropping that client; it will receive a clean snapshot when reattached. */
    if (write_all_flags(fd, header, sizeof(header), MSG_DONTWAIT) != 0) return -1;
    return length == 0 ? 0 : write_all_flags(fd, payload, length, MSG_DONTWAIT);
}

static int read_exact_timeout(int fd, void *buf, size_t len, int timeout_ms) {
    uint8_t *p = buf; size_t off = 0;
    int64_t deadline = qtc_now_millis() + timeout_ms;
    while (off < len) {
        int remaining = (int)(deadline - qtc_now_millis());
        if (remaining < 0) { errno = ETIMEDOUT; return -1; }
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int rc = poll(&pfd, 1, remaining);
        if (rc < 0) { if (errno == EINTR) continue; return -1; }
        if (rc == 0) { errno = ETIMEDOUT; return -1; }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) { errno = ECONNRESET; return -1; }
        ssize_t n = read(fd, p + off, len - off);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) { errno = ECONNRESET; return -1; }
        off += (size_t)n;
    }
    return 0;
}

int qtc_ipc_recv_blocking(int fd, qtc_ipc_frame *frame, int timeout_ms) {
    uint8_t h[5];
    if (read_exact_timeout(fd, h, sizeof(h), timeout_ms) != 0) return -1;
    frame->length = le32(h); frame->type = h[4];
    if (frame->length > QTC_MAX_FRAME) { errno = EMSGSIZE; return -1; }
    return frame->length == 0 ? 0 : read_exact_timeout(fd, frame->payload, frame->length, timeout_ms);
}

int qtc_ipc_server_open(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr = {0}; addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) { close(fd); errno = ENAMETOOLONG; return -1; }
    qtc_strlcpy(addr.sun_path, path, sizeof(addr.sun_path));
    unlink(path);
    mode_t old = umask(0077);
    int rc = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    umask(old);
    if (rc != 0 || chmod(path, 0600) != 0 || listen(fd, QTC_MAX_CLIENTS) != 0) {
        int saved = errno; close(fd); unlink(path); errno = saved; return -1;
    }
    return fd;
}

int qtc_ipc_client_connect(const char *path, int timeout_ms) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr = {0}; addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) { close(fd); errno = ENAMETOOLONG; return -1; }
    qtc_strlcpy(addr.sun_path, path, sizeof(addr.sun_path));
    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc != 0 && errno != EINPROGRESS) { close(fd); return -1; }
    if (rc != 0) {
        struct pollfd pfd = {.fd = fd, .events = POLLOUT};
        do { rc = poll(&pfd, 1, timeout_ms); } while (rc < 0 && errno == EINTR);
        if (rc <= 0) { int saved = rc == 0 ? ETIMEDOUT : errno; close(fd); errno = saved; return -1; }
        int error = 0; socklen_t n = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &n) != 0 || error != 0) {
            if (error != 0) errno = error;
            close(fd);
            return -1;
        }
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) (void)fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    return fd;
}

int qtc_ipc_verify_peer_uid(int fd, uid_t expected_uid) {
#ifdef SO_PEERCRED
    struct ucred cred; socklen_t len = sizeof(cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) return -1;
    if (cred.uid != expected_uid) { errno = EACCES; return -1; }
    return 0;
#else
    (void)fd; (void)expected_uid; return 0;
#endif
}
