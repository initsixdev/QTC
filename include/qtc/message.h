#ifndef QTC_MESSAGE_H
#define QTC_MESSAGE_H

#include "qtc/qtc.h"

#define QTC_DIRECT_RADIO_TEXT_MAX 160
#define QTC_CHANNEL_RADIO_TEXT_MAX 120
#define QTC_LONG_CHUNK_MAX 112

uint32_t qtc_message_token(qtc_conversation_kind kind, const char *conversation_key,
                           int64_t sender_timestamp, const char *text);
int qtc_long_wire_build(uint32_t token, int part_index, int part_total,
                        const char *chunk, char *out, size_t out_len);
int qtc_long_wire_parse(const char *wire, uint32_t *token, int *part_index,
                        int *part_total, char *chunk, size_t chunk_len);
size_t qtc_utf8_chunk_length(const char *text, size_t max_bytes);
int qtc_message_assemble(const qtc_message *messages, size_t count,
                         const char *logical_key, char *out, size_t out_len,
                         int *part_total, int *part_count,
                         qtc_message_status *aggregate_status);

#endif
