#ifndef QTC_PROTOCOL_H
#define QTC_PROTOCOL_H

#include "qtc/qtc.h"

typedef enum {
    QTC_RADIO_NONE = 0,
    QTC_RADIO_OK,
    QTC_RADIO_ERROR,
    QTC_RADIO_CONTACT_START,
    QTC_RADIO_CONTACT,
    QTC_RADIO_CONTACT_END,
    QTC_RADIO_SELF_INFO,
    QTC_RADIO_MSG_SENT,
    QTC_RADIO_CONTACT_MESSAGE,
    QTC_RADIO_CHANNEL_MESSAGE,
    QTC_RADIO_NO_MORE_MESSAGES,
    QTC_RADIO_BATTERY,
    QTC_RADIO_EXPORT_CONTACT,
    QTC_RADIO_DEVICE_INFO,
    QTC_RADIO_CHANNEL_INFO,
    QTC_RADIO_ACK,
    QTC_RADIO_MESSAGES_WAITING,
    QTC_RADIO_CONTACTS_DIRTY,
    QTC_RADIO_NEW_ADVERT,
    QTC_RADIO_UNKNOWN
} qtc_radio_event_type;

typedef struct {
    qtc_radio_event_type type;
    int error_code;
    uint32_t value;
    qtc_contact contact;
    qtc_channel channel;
    qtc_message message;
    uint8_t expected_ack[6];
    size_t expected_ack_len;
    uint32_t suggested_timeout_ms;
    uint32_t round_trip_ms;
    uint8_t card_data[QTC_MAX_FRAME - 1];
    size_t card_data_len;
    int max_contacts;
    int max_channels;
    char radio_name[QTC_MAX_NAME];
    char model[QTC_MAX_NAME];
    char version[QTC_MAX_NAME];
    int tx_power;
    int max_tx_power;
    double freq;
    double bw;
    int sf;
    int cr;
} qtc_radio_event;

typedef struct {
    uint8_t header[3];
    size_t header_len;
    uint16_t expected;
    uint8_t payload[QTC_MAX_FRAME];
    size_t payload_len;
} qtc_serial_parser;

typedef void (*qtc_radio_frame_cb)(const uint8_t *frame, size_t len, void *userdata);

void qtc_serial_parser_init(qtc_serial_parser *parser);
int qtc_serial_parser_feed(qtc_serial_parser *parser, const uint8_t *data, size_t len,
                           qtc_radio_frame_cb callback, void *userdata);
int qtc_protocol_wrap_command(const uint8_t *payload, size_t payload_len,
                              uint8_t *out, size_t out_len, size_t *written);
int qtc_protocol_parse(const uint8_t *frame, size_t len, qtc_radio_event *event);

size_t qtc_cmd_app_start(uint8_t *out, size_t out_len, const char *app_name);
size_t qtc_cmd_device_query(uint8_t *out, size_t out_len);
size_t qtc_cmd_get_contacts(uint8_t *out, size_t out_len, uint32_t since);
size_t qtc_cmd_get_channel(uint8_t *out, size_t out_len, int index);
size_t qtc_cmd_set_channel(uint8_t *out, size_t out_len, int index,
                           const char *name, const uint8_t secret[16]);
size_t qtc_cmd_sync_next_message(uint8_t *out, size_t out_len);
size_t qtc_cmd_send_direct(uint8_t *out, size_t out_len, const uint8_t dst_prefix[6],
                           uint32_t timestamp, uint8_t attempt, const char *text);
size_t qtc_cmd_send_channel(uint8_t *out, size_t out_len, int channel_index,
                            uint32_t timestamp, const char *text);
size_t qtc_cmd_get_battery(uint8_t *out, size_t out_len);
size_t qtc_cmd_set_name(uint8_t *out, size_t out_len, const char *name);
size_t qtc_cmd_set_tx_power(uint8_t *out, size_t out_len, int tx_power);
size_t qtc_cmd_advertise(uint8_t *out, size_t out_len, bool flood);
size_t qtc_cmd_export_self(uint8_t *out, size_t out_len);
size_t qtc_cmd_set_radio_params(uint8_t *out, size_t out_len,
                                double freq_mhz, double bw_khz,
                                int sf, int cr, bool repeat_mode);
size_t qtc_cmd_reset_path(uint8_t *out, size_t out_len, const uint8_t public_key[32]);

#endif
