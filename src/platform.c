/* Every Linux/macOS difference that cannot be resolved at runtime lives here.
 * Behavioral differences that can be probed at runtime - serial device names,
 * notification helpers, clipboard helpers - deliberately do not belong in this
 * file; they are ordered candidate lists in serial.c, notify.c, and tui.c. */
#define _GNU_SOURCE
#include "qtc/platform.h"
#include "qtc/util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#endif

void qtc_platform_self_path(char *out, size_t out_len, const char *argv0) {
#ifdef __APPLE__
    /* Darwin records the full launch path even when the command was resolved
     * through PATH, so no PATH search is needed here. */
    char raw[QTC_MAX_PATH];
    uint32_t size = (uint32_t)sizeof(raw);
    if (_NSGetExecutablePath(raw, &size) == 0) {
        char resolved[QTC_MAX_PATH];
        if (realpath(raw, resolved) != NULL) { qtc_strlcpy(out, resolved, out_len); return; }
        qtc_strlcpy(out, raw, out_len);
        return;
    }
#else
    ssize_t n = readlink("/proc/self/exe", out, out_len - 1);
    if (n > 0) { out[n] = 0; return; }
#endif
    if (argv0 != NULL && realpath(argv0, out) != NULL) return;
    qtc_strlcpy(out, argv0 != NULL ? argv0 : "", out_len);
}

int qtc_platform_peer_uid(int fd, uid_t *out_uid) {
    if (out_uid == NULL) { errno = EINVAL; return -1; }
#ifdef __APPLE__
    uid_t uid = (uid_t)-1;
    gid_t gid = (gid_t)-1;
    if (getpeereid(fd, &uid, &gid) != 0) return -1;
    *out_uid = uid;
    return 0;
#else
    struct ucred cred;
    socklen_t len = sizeof(cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) return -1;
    *out_uid = cred.uid;
    return 0;
#endif
}

int qtc_platform_process_uid(pid_t pid, uid_t *out_uid) {
    if (out_uid == NULL) { errno = EINVAL; return -1; }
#ifdef __APPLE__
    struct kinfo_proc info;
    size_t len = sizeof(info);
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)pid};
    if (sysctl(mib, 4, &info, &len, NULL, 0) != 0) return -1;
    if (len == 0) { errno = ESRCH; return -1; }
    *out_uid = info.kp_eproc.e_ucred.cr_uid;
    return 0;
#else
    char path[64];
    (void)snprintf(path, sizeof(path), "/proc/%ld/status", (long)pid);
    FILE *f = fopen(path, "r");
    if (f == NULL) return -1;
    char line[256];
    bool found = false;
    while (fgets(line, sizeof(line), f) != NULL) {
        unsigned long uid = 0;
        if (sscanf(line, "Uid:\t%lu", &uid) == 1) { *out_uid = (uid_t)uid; found = true; break; }
    }
    fclose(f);
    if (!found) { errno = ESRCH; return -1; }
    return 0;
#endif
}

int qtc_platform_secure_random(void *buf, size_t len) {
    uint8_t *p = buf;
    size_t off = 0;
    while (off < len) {
#ifdef __APPLE__
        /* getentropy() rejects requests larger than 256 bytes. */
        size_t take = len - off > 256 ? 256 : len - off;
        if (getentropy(p + off, take) != 0) return -1;
        off += take;
#else
        ssize_t n = getrandom(p + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)n;
#endif
    }
    return 0;
}

const char *qtc_platform_device_access_help(void) {
#ifdef __APPLE__
    return "# QTC / MeshCore serial access on macOS\n"
           "# No device rule is required. A connected USB Companion radio appears as\n"
           "# /dev/cu.usbmodem*, /dev/cu.usbserial*, or /dev/cu.wchusbserial* and is\n"
           "# readable and writable by the logged-in user.\n"
           "# Some USB-serial bridges need a vendor driver before the device appears.";
#else
    return "# QTC / MeshCore serial access for the active desktop user\n"
           "SUBSYSTEM==\"tty\", KERNEL==\"ttyACM[0-9]*\", TAG+=\"uaccess\", MODE=\"0660\"\n"
           "SUBSYSTEM==\"tty\", KERNEL==\"ttyUSB[0-9]*\", TAG+=\"uaccess\", MODE=\"0660\"";
#endif
}

int qtc_platform_prepare_fd(int fd, bool nonblocking) {
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) return -1;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    flags = nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(fd, F_SETFL, flags) == 0 ? 0 : -1;
}
