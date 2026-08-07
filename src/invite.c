#include "qtc/invite.h"
#include "qtc/util.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static bool uri_safe(unsigned char c) {
    return isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
}

static int percent_encode(const char *in, char *out, size_t out_len) {
    static const char hex[] = "0123456789ABCDEF";
    size_t p = 0;
    for (const unsigned char *s = (const unsigned char *)in; *s != 0; s++) {
        if (uri_safe(*s)) {
            if (p + 1 >= out_len) return -1;
            out[p++] = (char)*s;
        } else {
            if (p + 3 >= out_len) return -1;
            out[p++] = '%'; out[p++] = hex[*s >> 4]; out[p++] = hex[*s & 15];
        }
    }
    if (p >= out_len) return -1;
    out[p] = 0;
    return 0;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int percent_decode(const char *in, char *out, size_t out_len) {
    size_t p = 0;
    while (*in != 0) {
        unsigned char c = (unsigned char)*in++;
        if (c == '%') {
            int a = hex_digit(*in++); int b = hex_digit(*in++);
            if (a < 0 || b < 0) return -1;
            c = (unsigned char)((a << 4) | b);
        } else if (c == '+') {
            c = ' ';
        }
        if (c == 0 || p + 1 >= out_len) return -1;
        out[p++] = (char)c;
    }
    out[p] = 0;
    return 0;
}

int qtc_channel_uri_build(const char *name, const uint8_t secret[16], char *out, size_t out_len) {
    if (name == NULL || *name == 0 || strlen(name) > 32 || secret == NULL || out == NULL) {
        errno = EINVAL; return -1;
    }
    char encoded[256]; char hex[33];
    if (percent_encode(name, encoded, sizeof(encoded)) != 0) { errno = ENOSPC; return -1; }
    qtc_hex_encode(secret, 16, hex, sizeof(hex));
    int n = snprintf(out, out_len, "meshcore://channel/add?name=%s&secret=%s", encoded, hex);
    if (n < 0 || (size_t)n >= out_len) { errno = ENOSPC; return -1; }
    return 0;
}

int qtc_channel_uri_parse(const char *uri, char *name, size_t name_len, uint8_t secret[16]) {
    static const char prefix[] = "meshcore://channel/add?";
    if (uri == NULL || strncmp(uri, prefix, sizeof(prefix) - 1) != 0) { errno = EINVAL; return -1; }
    char copy[QTC_MAX_URI]; qtc_strlcpy(copy, uri + sizeof(prefix) - 1, sizeof(copy));
    char encoded_name[256] = ""; char secret_hex[64] = "";
    char *save = NULL;
    for (char *part = strtok_r(copy, "&", &save); part != NULL; part = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(part, '='); if (eq == NULL) continue; *eq++ = 0;
        if (strcmp(part, "name") == 0) qtc_strlcpy(encoded_name, eq, sizeof(encoded_name));
        else if (strcmp(part, "secret") == 0) qtc_strlcpy(secret_hex, eq, sizeof(secret_hex));
    }
    if (encoded_name[0] == 0 || strlen(secret_hex) != 32 ||
        percent_decode(encoded_name, name, name_len) != 0 || name[0] == 0 || strlen(name) > 32 ||
        qtc_hex_decode(secret_hex, secret, 16) != 0) {
        errno = EINVAL; return -1;
    }
    return 0;
}

int qtc_channel_join_parse(const char *input, char *name, size_t name_len, uint8_t secret[16]) {
    if (input == NULL || name == NULL || name_len == 0 || secret == NULL) {
        errno = EINVAL;
        return -1;
    }
    char copy[QTC_MAX_URI];
    qtc_strlcpy(copy, input, sizeof(copy));
    qtc_trim(copy);
    if (strncmp(copy, "meshcore://channel/add?", 23) == 0)
        return qtc_channel_uri_parse(copy, name, name_len, secret);

    char *key = copy;
    char *separator = strchr(copy, ':');
    if (separator != NULL && strlen(separator + 1) == 32) {
        *separator = 0;
        qtc_trim(copy);
        key = separator + 1;
        qtc_trim(key);
        if (copy[0] == 0 || strlen(copy) > 32) {
            errno = EINVAL;
            return -1;
        }
        qtc_strlcpy(name, copy, name_len);
    } else {
        if (strlen(key) != 32) {
            errno = EINVAL;
            return -1;
        }
        char suffix[9];
        memcpy(suffix, key, 8);
        suffix[8] = 0;
        for (size_t i = 0; suffix[i] != 0; i++) suffix[i] = (char)toupper((unsigned char)suffix[i]);
        int n = snprintf(name, name_len, "Private-%s", suffix);
        if (n < 0 || (size_t)n >= name_len) {
            errno = ENOSPC;
            return -1;
        }
    }
    if (qtc_hex_decode(key, secret, 16) != 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int qtc_invite_message_build(const char *uri, char *out, size_t out_len) {
    if (uri == NULL || out == NULL) return -1;
    int n = snprintf(out, out_len, "QTC-CHANNEL-INVITE v1\n%s", uri);
    return n >= 0 && (size_t)n < out_len ? 0 : -1;
}

int qtc_invite_message_parse(const char *message, char *uri, size_t uri_len,
                             char *name, size_t name_len, uint8_t secret[16]) {
    static const char marker[] = "QTC-CHANNEL-INVITE v1\n";
    if (message == NULL || strncmp(message, marker, sizeof(marker) - 1) != 0) return -1;
    const char *start = message + sizeof(marker) - 1;
    const char *end = strpbrk(start, "\r\n");
    size_t n = end != NULL ? (size_t)(end - start) : strlen(start);
    if (n == 0 || n >= uri_len) return -1;
    memcpy(uri, start, n); uri[n] = 0;
    return qtc_channel_uri_parse(uri, name, name_len, secret);
}

int qtc_find_free_channel_slot(const qtc_state *state, int max_channels) {
    if (max_channels <= 0 || max_channels > QTC_MAX_CHANNELS) max_channels = QTC_MAX_CHANNELS;
    /* Slot zero is the public channel; private channels begin at slot one. */
    for (int slot = 1; slot < max_channels; slot++) {
        bool used = false;
        for (size_t i = 0; i < state->channel_count; i++) {
            if (state->channels[i].configured && state->channels[i].index == slot) { used = true; break; }
        }
        if (!used) return slot;
    }
    return -1;
}
