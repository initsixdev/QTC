#include "qtc/message.h"
#include "qtc/util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t fnv32(const void *data, size_t len, uint32_t h) {
    const unsigned char *p = data;
    if (h == 0) h = UINT32_C(2166136261);
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= UINT32_C(16777619);
    }
    return h;
}

uint32_t qtc_message_token(qtc_conversation_kind kind, const char *conversation_key,
                           int64_t sender_timestamp, const char *text) {
    uint32_t h = fnv32(&kind, sizeof(kind), 0);
    if (conversation_key != NULL) h = fnv32(conversation_key, strlen(conversation_key), h);
    h = fnv32(&sender_timestamp, sizeof(sender_timestamp), h);
    if (text != NULL) h = fnv32(text, strlen(text), h);
    return h != 0 ? h : UINT32_C(1);
}

int qtc_long_wire_build(uint32_t token, int part_index, int part_total,
                        const char *chunk, char *out, size_t out_len) {
    if (part_index < 1 || part_total < 2 || part_index > part_total ||
        part_total > 99 || chunk == NULL || out == NULL || out_len == 0) {
        errno = EINVAL;
        return -1;
    }
    int n = snprintf(out, out_len, "QTC-LONG/1:%08x:%02d/%02d:%s",
                     token, part_index, part_total, chunk);
    if (n < 0 || (size_t)n >= out_len || n > QTC_DIRECT_RADIO_TEXT_MAX) {
        errno = EMSGSIZE;
        return -1;
    }
    return 0;
}

int qtc_long_wire_parse(const char *wire, uint32_t *token, int *part_index,
                        int *part_total, char *chunk, size_t chunk_len) {
    if (wire == NULL) return -1;
    unsigned tok = 0;
    int index = 0, total = 0, consumed = 0;
    if (sscanf(wire, "QTC-LONG/1:%8x:%2d/%2d:%n", &tok, &index, &total, &consumed) != 3 ||
        consumed <= 0 || index < 1 || total < 2 || index > total || total > 99) return -1;
    if (token != NULL) *token = (uint32_t)tok;
    if (part_index != NULL) *part_index = index;
    if (part_total != NULL) *part_total = total;
    if (chunk != NULL && chunk_len > 0) qtc_strlcpy(chunk, wire + consumed, chunk_len);
    return 0;
}

size_t qtc_utf8_chunk_length(const char *text, size_t max_bytes) {
    if (text == NULL || max_bytes == 0) return 0;
    size_t length = strlen(text);
    if (length <= max_bytes) return length;
    size_t cut = max_bytes;
    while (cut > 0 && (((unsigned char)text[cut] & 0xc0U) == 0x80U)) cut--;
    if (cut == 0) return max_bytes;
    return cut;
}

static int part_cmp(const void *a, const void *b) {
    const qtc_message *const *ma = a;
    const qtc_message *const *mb = b;
    if ((*ma)->part_index != (*mb)->part_index) return (*ma)->part_index - (*mb)->part_index;
    return (*ma)->id < (*mb)->id ? -1 : ((*ma)->id > (*mb)->id ? 1 : 0);
}

static qtc_message_status combine_status(qtc_message_status a, qtc_message_status b) {
    if (a == QTC_MSG_FAILED || b == QTC_MSG_FAILED) return QTC_MSG_FAILED;
    if (a == QTC_MSG_UNCONFIRMED || b == QTC_MSG_UNCONFIRMED) return QTC_MSG_UNCONFIRMED;
    if (a == QTC_MSG_QUEUED || b == QTC_MSG_QUEUED) return QTC_MSG_QUEUED;
    if (a == QTC_MSG_SENDING || b == QTC_MSG_SENDING) return QTC_MSG_SENDING;
    if (a == QTC_MSG_SENT || b == QTC_MSG_SENT) return QTC_MSG_SENT;
    return QTC_MSG_DELIVERED;
}

int qtc_message_assemble(const qtc_message *messages, size_t count,
                         const char *logical_key, char *out, size_t out_len,
                         int *part_total, int *part_count,
                         qtc_message_status *aggregate_status) {
    if (messages == NULL || logical_key == NULL || out == NULL || out_len == 0) return -1;
    const qtc_message *parts[99];
    size_t n = 0;
    int total = 1;
    qtc_message_status status = QTC_MSG_DELIVERED;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(messages[i].logical_key, logical_key) != 0) continue;
        if (n < QTC_ARRAY_LEN(parts)) parts[n++] = &messages[i];
        if (messages[i].part_total > total) total = messages[i].part_total;
        status = combine_status(status, messages[i].status);
    }
    if (n == 0) return -1;
    qsort(parts, n, sizeof(parts[0]), part_cmp);
    out[0] = 0;
    size_t used = 0;
    int unique = 0;
    int last_part = -1;
    for (size_t i = 0; i < n; i++) {
        if (parts[i]->part_index == last_part) continue;
        last_part = parts[i]->part_index;
        unique++;
        size_t length = strlen(parts[i]->text);
        if (length >= out_len - used) length = out_len - used - 1;
        memcpy(out + used, parts[i]->text, length);
        used += length;
        out[used] = 0;
        if (used + 1 >= out_len) break;
    }
    if (part_total != NULL) *part_total = total;
    if (part_count != NULL) *part_count = unique;
    if (aggregate_status != NULL) *aggregate_status = status;
    return unique == total ? 0 : 1;
}
