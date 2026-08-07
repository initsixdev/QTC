#ifndef QTC_UTIL_H
#define QTC_UTIL_H

#include "qtc/qtc.h"
#include <stdarg.h>
#include <sys/types.h>

void qtc_set_log_level(int level);
void qtc_log(int level, const char *fmt, ...);
int64_t qtc_now_seconds(void);
int64_t qtc_now_millis(void);
int qtc_mkdir_p(const char *path, mode_t mode);
int qtc_init_paths(qtc_paths *paths, const char *profile);
int qtc_write_file_atomic(const char *path, const void *data, size_t len, mode_t mode);
int qtc_secure_random(void *buf, size_t len);
void qtc_hex_encode(const uint8_t *in, size_t len, char *out, size_t out_len);
int qtc_hex_decode(const char *hex, uint8_t *out, size_t out_len);
int qtc_casecmp(const char *a, const char *b);
bool qtc_wildcard_match(const char *pattern, const char *text);
bool qtc_search_match(const char *query, const char *text);
void qtc_strlcpy(char *dst, const char *src, size_t dst_len);
void qtc_trim(char *s);
int qtc_command_exists(const char *command);
int qtc_spawn_detached(char *const argv[]);
int qtc_process_is_alive(pid_t pid, uid_t expected_uid);

#endif
