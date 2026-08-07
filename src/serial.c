#define _DEFAULT_SOURCE
#include "qtc/serial.h"
#include "qtc/util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static speed_t baud_flag(int baud) {
    switch (baud) {
        case 9600: return B9600; case 19200: return B19200; case 38400: return B38400;
        case 57600: return B57600; case 115200: return B115200;
#ifdef B230400
        case 230400: return B230400;
#endif
        default: return B115200;
    }
}

int qtc_serial_open(qtc_serial *s, const char *device, int baud) {
    if (s == NULL || device == NULL) { errno = EINVAL; return -1; }
    memset(s, 0, sizeof(*s)); s->fd = -1;
    int fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    struct termios t;
    if (tcgetattr(fd, &s->original) == 0) s->original_valid = true;
    if (tcgetattr(fd, &t) != 0) { close(fd); return -1; }
    cfmakeraw(&t); speed_t speed = baud_flag(baud);
    cfsetispeed(&t, speed); cfsetospeed(&t, speed);
    t.c_cflag |= CLOCAL | CREAD; t.c_cflag &= ~(CSTOPB | CRTSCTS); t.c_cflag &= ~PARENB;
    t.c_cflag = (t.c_cflag & ~CSIZE) | CS8; t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &t) != 0) { close(fd); return -1; }
    tcflush(fd, TCIOFLUSH); s->fd = fd; qtc_strlcpy(s->device, device, sizeof(s->device));
    qtc_serial_parser_init(&s->parser); return 0;
}

void qtc_serial_close(qtc_serial *s) {
    if (s != NULL && s->fd >= 0) {
        (void)tcdrain(s->fd); if (s->original_valid) (void)tcsetattr(s->fd, TCSANOW, &s->original);
        close(s->fd); s->fd = -1;
    }
}

ssize_t qtc_serial_read(qtc_serial *s, uint8_t *buf, size_t len) {
    if (s == NULL || s->fd < 0) { errno = EBADF; return -1; }
    return read(s->fd, buf, len);
}

int qtc_serial_send(qtc_serial *s, const uint8_t *payload, size_t len) {
    uint8_t frame[QTC_MAX_FRAME + 3]; size_t n = 0;
    if (s == NULL || s->fd < 0 || qtc_protocol_wrap_command(payload, len, frame, sizeof(frame), &n) != 0) return -1;
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(s->fd, frame + off, n - off);
        if (w < 0) { if (errno == EINTR) continue; if (errno == EAGAIN) { usleep(1000); continue; } return -1; }
        off += (size_t)w;
    }
    return 0;
}

static bool device_exists(const char *path) { struct stat st; return stat(path, &st) == 0 && S_ISCHR(st.st_mode); }

int qtc_serial_list_devices(char devices[][QTC_MAX_PATH], size_t max, size_t *count) {
    if (count == NULL) return -1;
    *count = 0;
    const char *patterns[] = {"/dev/serial/by-id/*", "/dev/ttyACM*", "/dev/ttyUSB*"};
    for (size_t p = 0; p < QTC_ARRAY_LEN(patterns); p++) {
        glob_t g = {0}; int rc = glob(patterns[p], 0, NULL, &g);
        if (rc == GLOB_NOMATCH) { globfree(&g); continue; }
        if (rc != 0) { globfree(&g); continue; }
        for (size_t i = 0; i < g.gl_pathc && *count < max; i++) {
            if (!device_exists(g.gl_pathv[i])) continue;
            bool duplicate = false;
            for (size_t j = 0; j < *count; j++) if (strcmp(devices[j], g.gl_pathv[i]) == 0) duplicate = true;
            if (!duplicate) qtc_strlcpy(devices[(*count)++], g.gl_pathv[i], QTC_MAX_PATH);
        }
        globfree(&g);
    }
    return 0;
}

int qtc_serial_autodetect(char *out, size_t out_len) {
    char devices[32][QTC_MAX_PATH]; size_t count = 0;
    if (qtc_serial_list_devices(devices, QTC_ARRAY_LEN(devices), &count) != 0 || count == 0) {
        errno = ENODEV; return -1;
    }
    qtc_strlcpy(out, devices[0], out_len); return 0;
}
