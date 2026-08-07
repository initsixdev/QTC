#ifndef QTC_PLATFORM_H
#define QTC_PLATFORM_H

#include "qtc/qtc.h"

#include <sys/socket.h>
#include <sys/types.h>

/* Darwin has no MSG_NOSIGNAL; SO_NOSIGPIPE is set on the socket instead. */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* Absolute path of the running executable, used to re-exec the background core.
 * argv0 is the fallback when the kernel interface is unavailable. */
void qtc_platform_self_path(char *out, size_t out_len, const char *argv0);

/* Credentials of the peer on a connected unix socket. Returns 0 and stores the
 * peer uid, or -1 with errno set. */
int qtc_platform_peer_uid(int fd, uid_t *out_uid);

/* Real uid owning a running process. Returns 0 and stores the uid, or -1. */
int qtc_platform_process_uid(pid_t pid, uid_t *out_uid);

/* Fills buf from the kernel entropy pool. Returns 0, or -1 with errno set. */
int qtc_platform_secure_random(void *buf, size_t len);

/* Operator instructions for granting access to the USB radio, printed by
 * --print-udev-rule. Never NULL. */
const char *qtc_platform_device_access_help(void);

/* Marks fd close-on-exec and sets its blocking mode explicitly. Accepted sockets
 * inherit O_NONBLOCK from the listener on BSD but not on Linux, so callers must
 * not rely on the inherited mode. Returns 0, or -1 with errno set. */
int qtc_platform_prepare_fd(int fd, bool nonblocking);

#endif
