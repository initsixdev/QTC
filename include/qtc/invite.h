#ifndef QTC_INVITE_H
#define QTC_INVITE_H

#include "qtc/qtc.h"

int qtc_channel_uri_build(const char *name, const uint8_t secret[16], char *out, size_t out_len);
int qtc_channel_uri_parse(const char *uri, char *name, size_t name_len, uint8_t secret[16]);
int qtc_channel_join_parse(const char *input, char *name, size_t name_len, uint8_t secret[16]);
int qtc_invite_message_build(const char *uri, char *out, size_t out_len);
int qtc_invite_message_parse(const char *message, char *uri, size_t uri_len,
                             char *name, size_t name_len, uint8_t secret[16]);
int qtc_find_free_channel_slot(const qtc_state *state, int max_channels);

#endif
