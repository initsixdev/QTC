#define _GNU_SOURCE
#include "qtc/util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int g_log_level = QTC_LOG_INFO;

void qtc_set_log_level(int level) {
    g_log_level = level;
}

void qtc_log(int level, const char *fmt, ...) {
    if (level > g_log_level) {
        return;
    }
    static const char *names[] = {"ERROR", "WARN", "INFO", "DEBUG"};
    struct timespec ts;
    (void)clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv;
    localtime_r(&ts.tv_sec, &tmv);
    fprintf(stderr, "%02d:%02d:%02d.%03ld %-5s ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
            ts.tv_nsec / 1000000L, names[level]);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

int64_t qtc_now_seconds(void) {
    return (int64_t)time(NULL);
}

int64_t qtc_now_millis(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void qtc_strlcpy(char *dst, const char *src, size_t dst_len) {
    if (dst_len == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= dst_len) {
        n = dst_len - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void qtc_trim(char *s) {
    if (s == NULL) {
        return;
    }
    char *start = s;
    while (*start != '\0' && isspace((unsigned char)*start)) {
        start++;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        s[--n] = '\0';
    }
}

int qtc_mkdir_p(const char *path, mode_t mode) {
    if (path == NULL || *path == '\0') {
        errno = EINVAL;
        return -1;
    }
    char tmp[PATH_MAX];
    qtc_strlcpy(tmp, path, sizeof(tmp));
    size_t len = strlen(tmp);
    if (len == 0) {
        return 0;
    }
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }
    for (char *p = tmp + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
        return -1;
    }
    return chmod(tmp, mode);
}

static const char *env_or_home(const char *env_name, const char *suffix, char *out, size_t out_len) {
    const char *value = getenv(env_name);
    if (value != NULL && *value != '\0') {
        qtc_strlcpy(out, value, out_len);
        return out;
    }
    const char *home = getenv("HOME");
    if (home == NULL || *home == '\0') {
        return NULL;
    }
    (void)snprintf(out, out_len, "%s/%s", home, suffix);
    return out;
}

static int path_join2(char *out, size_t out_len, const char *a, const char *b) {
    size_t alen = strlen(a), blen = strlen(b);
    bool slash = alen > 0 && a[alen - 1] != '/';
    if (alen + (slash ? 1U : 0U) + blen + 1U > out_len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(out, a, alen);
    size_t pos = alen;
    if (slash) out[pos++] = '/';
    memcpy(out + pos, b, blen);
    out[pos + blen] = '\0';
    return 0;
}

static int path_join3(char *out, size_t out_len, const char *a, const char *b, const char *c) {
    char tmp[QTC_MAX_PATH];
    if (path_join2(tmp, sizeof(tmp), a, b) != 0) return -1;
    return path_join2(out, out_len, tmp, c);
}

int qtc_init_paths(qtc_paths *paths, const char *profile) {
    if (paths == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(paths, 0, sizeof(*paths));
    qtc_strlcpy(paths->profile, (profile != NULL && *profile != '\0') ? profile : "default",
                sizeof(paths->profile));
    for (const char *p = paths->profile; *p != '\0'; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '-' || *p == '_')) {
            errno = EINVAL;
            return -1;
        }
    }

    char base[QTC_MAX_PATH];
    if (env_or_home("XDG_DATA_HOME", ".local/share", base, sizeof(base)) == NULL) {
        return -1;
    }
    if (path_join3(paths->data_dir, sizeof(paths->data_dir), base, "qtc-terminal", paths->profile) != 0) return -1;
    if (env_or_home("XDG_CONFIG_HOME", ".config", base, sizeof(base)) == NULL) {
        return -1;
    }
    if (path_join3(paths->config_dir, sizeof(paths->config_dir), base, "qtc-terminal", paths->profile) != 0) return -1;

    const char *xdg_runtime = getenv("XDG_RUNTIME_DIR");
    if (xdg_runtime != NULL && *xdg_runtime != '\0') {
        if (path_join3(paths->runtime_dir, sizeof(paths->runtime_dir), xdg_runtime, "qtc", paths->profile) != 0) return -1;
    } else {
        char uid_dir[64];
        int n = snprintf(uid_dir, sizeof(uid_dir), "/tmp/qtc-%lu", (unsigned long)getuid());
        if (n < 0 || (size_t)n >= sizeof(uid_dir) || path_join2(paths->runtime_dir, sizeof(paths->runtime_dir), uid_dir, paths->profile) != 0) return -1;
    }
    if (path_join2(paths->db_path, sizeof(paths->db_path), paths->data_dir, "qtc.db") != 0 ||
        path_join2(paths->socket_path, sizeof(paths->socket_path), paths->runtime_dir, "qtc.sock") != 0 ||
        path_join2(paths->lock_path, sizeof(paths->lock_path), paths->runtime_dir, "qtc.lock") != 0 ||
        path_join2(paths->pid_path, sizeof(paths->pid_path), paths->runtime_dir, "qtc.pid") != 0) return -1;

    if (qtc_mkdir_p(paths->data_dir, 0700) != 0 || qtc_mkdir_p(paths->config_dir, 0700) != 0 ||
        qtc_mkdir_p(paths->runtime_dir, 0700) != 0) {
        return -1;
    }
    return 0;
}

int qtc_write_file_atomic(const char *path, const void *data, size_t len, mode_t mode) {
    char tmp[QTC_MAX_PATH];
    (void)snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
    int fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, mode);
    if (fd < 0) {
        return -1;
    }
    const uint8_t *p = data;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            int saved = errno;
            close(fd);
            unlink(tmp);
            errno = saved;
            return -1;
        }
        off += (size_t)n;
    }
    if (fsync(fd) != 0 || fchmod(fd, mode) != 0 || close(fd) != 0 || rename(tmp, path) != 0) {
        int saved = errno;
        unlink(tmp);
        errno = saved;
        return -1;
    }
    return 0;
}

int qtc_secure_random(void *buf, size_t len) {
    uint8_t *p = buf;
    size_t off = 0;
    while (off < len) {
        ssize_t n = getrandom(p + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno != ENOSYS) {
                return -1;
            }
            break;
        }
        off += (size_t)n;
    }
    if (off == len) {
        return 0;
    }
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    while (off < len) {
        ssize_t n = read(fd, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return -1;
        }
        if (n == 0) {
            close(fd);
            errno = EIO;
            return -1;
        }
        off += (size_t)n;
    }
    close(fd);
    return 0;
}

void qtc_hex_encode(const uint8_t *in, size_t len, char *out, size_t out_len) {
    static const char hex[] = "0123456789abcdef";
    if (out_len == 0) {
        return;
    }
    size_t max = (out_len - 1) / 2;
    if (len > max) {
        len = max;
    }
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = hex[in[i] >> 4];
        out[i * 2 + 1] = hex[in[i] & 0x0f];
    }
    out[len * 2] = '\0';
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int qtc_hex_decode(const char *hex, uint8_t *out, size_t out_len) {
    size_t n = strlen(hex);
    if ((n & 1U) != 0 || n / 2 != out_len) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_value(hex[i * 2]);
        int lo = hex_value(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            errno = EINVAL;
            return -1;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

int qtc_casecmp(const char *a, const char *b) {
    return strcasecmp(a != NULL ? a : "", b != NULL ? b : "");
}

static bool wildcard_impl(const char *p, const char *s) {
    while (*p != '\0') {
        if (*p == '*') {
            while (*p == '*') p++;
            if (*p == '\0') return true;
            while (*s != '\0') {
                if (wildcard_impl(p, s)) return true;
                s++;
            }
            return false;
        }
        if (*s == '\0') return false;
        if (*p != '?' && tolower((unsigned char)*p) != tolower((unsigned char)*s)) return false;
        p++;
        s++;
    }
    return *s == '\0';
}

bool qtc_wildcard_match(const char *pattern, const char *text) {
    return wildcard_impl(pattern != NULL ? pattern : "", text != NULL ? text : "");
}

bool qtc_search_match(const char *query, const char *text) {
    if (query == NULL || *query == '\0') return true;
    char pattern[QTC_MAX_TEXT];
    if (strchr(query, '*') == NULL && strchr(query, '?') == NULL) {
        (void)snprintf(pattern, sizeof(pattern), "*%s*", query);
    } else {
        qtc_strlcpy(pattern, query, sizeof(pattern));
    }
    return qtc_wildcard_match(pattern, text != NULL ? text : "");
}

int qtc_command_exists(const char *command) {
    const char *path = getenv("PATH");
    if (path == NULL || command == NULL || strchr(command, '/') != NULL) return 0;
    char copy[8192];
    qtc_strlcpy(copy, path, sizeof(copy));
    char *save = NULL;
    for (char *dir = strtok_r(copy, ":", &save); dir != NULL; dir = strtok_r(NULL, ":", &save)) {
        char full[QTC_MAX_PATH];
        (void)snprintf(full, sizeof(full), "%s/%s", dir, command);
        if (access(full, X_OK) == 0) return 1;
    }
    return 0;
}

int qtc_spawn_detached(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        pid_t pid2 = fork();
        if (pid2 < 0) _exit(127);
        if (pid2 > 0) _exit(0);
        (void)setsid();
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) {
            (void)dup2(fd, STDIN_FILENO);
            (void)dup2(fd, STDOUT_FILENO);
            (void)dup2(fd, STDERR_FILENO);
            if (fd > STDERR_FILENO) close(fd);
        }
        execv(argv[0], argv);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

int qtc_process_is_alive(pid_t pid, uid_t expected_uid) {
    if (pid <= 1 || kill(pid, 0) != 0) return 0;
    char path[64];
    (void)snprintf(path, sizeof(path), "/proc/%ld/status", (long)pid);
    FILE *f = fopen(path, "r");
    if (f == NULL) return 0;
    char line[256];
    uid_t real_uid = (uid_t)-1;
    while (fgets(line, sizeof(line), f) != NULL) {
        unsigned long uid = 0;
        if (sscanf(line, "Uid:\t%lu", &uid) == 1) {
            real_uid = (uid_t)uid;
            break;
        }
    }
    fclose(f);
    return real_uid == expected_uid;
}
