#include "qtc/protocol.h"
#include "qtc/util.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define CMD_APP_START 1
#define CMD_SEND_TXT 2
#define CMD_SEND_CHANNEL 3
#define CMD_GET_CONTACTS 4
#define CMD_SEND_ADVERT 7
#define CMD_SET_NAME 8
#define CMD_SYNC_MESSAGE 10
#define CMD_SET_RADIO 11
#define CMD_SET_POWER 12
#define CMD_RESET_PATH 13
#define CMD_EXPORT_CONTACT 17
#define CMD_GET_BATTERY 20
#define CMD_DEVICE_QUERY 22
#define CMD_GET_CHANNEL 31
#define CMD_SET_CHANNEL 32

static uint16_t get_u16(const uint8_t *p) { return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8); }
static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int32_t get_i32(const uint8_t *p) { return (int32_t)get_u32(p); }
static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint64_t fnv64(const void *data, size_t len, uint64_t h) {
    const uint8_t *p = data;
    if (h == 0) h = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= UINT64_C(1099511628211); }
    return h;
}

static void copy_fixed_string(char *dst, size_t dst_len, const uint8_t *src, size_t src_len) {
    size_t n = 0;
    while (n < src_len && src[n] != 0) n++;
    if (n >= dst_len) n = dst_len - 1;
    memcpy(dst, src, n); dst[n] = 0; qtc_trim(dst);
}

void qtc_serial_parser_init(qtc_serial_parser *p) { memset(p, 0, sizeof(*p)); }

int qtc_serial_parser_feed(qtc_serial_parser *p, const uint8_t *data, size_t len,
                           qtc_radio_frame_cb callback, void *userdata) {
    while (len > 0) {
        if (p->header_len == 0) {
            const uint8_t *marker = memchr(data, '>', len);
            if (marker == NULL) return 0;
            size_t skip = (size_t)(marker - data); data += skip; len -= skip;
            p->header[p->header_len++] = *data++; len--;
        }
        while (p->header_len < 3 && len > 0) {
            p->header[p->header_len++] = *data++; len--;
        }
        if (p->header_len < 3) continue;
        if (p->expected == 0) {
            p->expected = get_u16(p->header + 1);
            if (p->expected == 0 || p->expected > QTC_MAX_FRAME) {
                memset(p, 0, sizeof(*p)); errno = EMSGSIZE; return -1;
            }
        }
        size_t need = p->expected - p->payload_len, take = len < need ? len : need;
        memcpy(p->payload + p->payload_len, data, take); p->payload_len += take; data += take; len -= take;
        if (p->payload_len == p->expected) {
            callback(p->payload, p->payload_len, userdata);
            memset(p, 0, sizeof(*p));
        }
    }
    return 0;
}

int qtc_protocol_wrap_command(const uint8_t *payload, size_t len, uint8_t *out, size_t out_len, size_t *written) {
    if (payload == NULL || out == NULL || written == NULL || len == 0 || len > UINT16_MAX || out_len < len + 3) {
        errno = EINVAL; return -1;
    }
    out[0] = '<'; out[1] = (uint8_t)len; out[2] = (uint8_t)(len >> 8); memcpy(out + 3, payload, len);
    *written = len + 3; return 0;
}

static qtc_node_type node_type(uint8_t type) {
    switch (type) { case 1: return QTC_NODE_PERSON; case 2: return QTC_NODE_REPEATER;
                    case 3: return QTC_NODE_ROOM; case 4: return QTC_NODE_SENSOR;
                    default: return QTC_NODE_UNKNOWN; }
}

static int parse_contact(const uint8_t *f, size_t len, qtc_radio_event *e) {
    /* code + key32 + type + flags + pathlen + path64 + name32 + lastadv4 + lat4 + lon4 + lastmod4 */
    if (len < 148) return -1;
    const uint8_t *p = f + 1;
    qtc_hex_encode(p, 32, e->contact.id, sizeof(e->contact.id));
    qtc_hex_encode(p, 6, e->contact.prefix, sizeof(e->contact.prefix)); p += 32;
    e->contact.node_type = node_type(*p++); e->contact.flags = *p++;
    uint8_t path_len = *p++;
    e->contact.route_known = path_len != 0xff;
    e->contact.route_hops = e->contact.route_known ? (path_len & 0x3f) : 0;
    p += 64; copy_fixed_string(e->contact.name, sizeof(e->contact.name), p, 32); p += 32;
    e->contact.last_heard = (int64_t)get_u32(p); p += 4;
    e->contact.latitude = get_i32(p) / 1000000.0; p += 4;
    e->contact.longitude = get_i32(p) / 1000000.0; p += 4;
    e->value = get_u32(p); e->type = QTC_RADIO_CONTACT; return 0;
}

static int parse_message(const uint8_t *f, size_t len, qtc_radio_event *e, bool direct, bool v3) {
    size_t off = v3 ? 4 : 1;
    memset(&e->message, 0, sizeof(e->message));
    e->message.direction = QTC_MSG_INCOMING; e->message.status = QTC_MSG_DELIVERED;
    e->message.part_index = 1; e->message.part_total = 1;
    e->message.path_len = -1;
    if (v3) e->message.snr_quarter_db = (int8_t)f[1];
    e->message.created_at = qtc_now_seconds();
    uint8_t txt_type;
    if (direct) {
        if (len < off + 12) return -1;
        char prefix[13]; qtc_hex_encode(f + off, 6, prefix, sizeof(prefix));
        qtc_strlcpy(e->message.conversation_key, prefix, sizeof(e->message.conversation_key));
        e->message.conversation_kind = QTC_CONV_CONTACT; off += 6;
        e->message.path_len = f[off++]; txt_type = f[off++];
    } else {
        if (len < off + 7) return -1;
        unsigned channel = f[off++];
        snprintf(e->message.conversation_key, sizeof(e->message.conversation_key), "%u", channel);
        e->message.conversation_kind = QTC_CONV_CHANNEL; e->message.path_len = f[off++]; txt_type = f[off++];
    }
    e->message.sender_timestamp = get_u32(f + off); off += 4;
    if (txt_type == 2) { if (len < off + 4) return -1; off += 4; }
    size_t text_len = len - off; if (text_len >= sizeof(e->message.text)) text_len = sizeof(e->message.text) - 1;
    memcpy(e->message.text, f + off, text_len); e->message.text[text_len] = 0;
    uint64_t h = fnv64(e->message.conversation_key, strlen(e->message.conversation_key), 0);
    h = fnv64(&e->message.sender_timestamp, sizeof(e->message.sender_timestamp), h);
    h = fnv64(e->message.text, text_len, h);
    snprintf(e->message.message_key, sizeof(e->message.message_key), "in:%d:%s:%lld:%016llx",
             e->message.conversation_kind, e->message.conversation_key,
             (long long)e->message.sender_timestamp, (unsigned long long)h);
    qtc_strlcpy(e->message.logical_key, e->message.message_key, sizeof(e->message.logical_key));
    e->type = direct ? QTC_RADIO_CONTACT_MESSAGE : QTC_RADIO_CHANNEL_MESSAGE;
    return 0;
}

int qtc_protocol_parse(const uint8_t *f, size_t len, qtc_radio_event *e) {
    if (f == NULL || len == 0 || e == NULL) return -1;
    memset(e, 0, sizeof(*e));
    switch (f[0]) {
        case 0: e->type = QTC_RADIO_OK; if (len >= 5) e->value = get_u32(f + 1); return 0;
        case 1: e->type = QTC_RADIO_ERROR; e->error_code = len > 1 ? f[1] : 0; return 0;
        case 2: e->type = QTC_RADIO_CONTACT_START; if (len >= 5) e->value = get_u32(f + 1); return 0;
        case 3: return parse_contact(f, len, e);
        case 4: e->type = QTC_RADIO_CONTACT_END; if (len >= 5) e->value = get_u32(f + 1); return 0;
        case 5:
            if (len < 46) return -1;
            e->type = QTC_RADIO_SELF_INFO; e->tx_power = (int8_t)f[2]; e->max_tx_power = (int8_t)f[3];
            e->freq = len >= 52 ? get_u32(f + 48) / 1000.0 : 0;
            e->bw = len >= 56 ? get_u32(f + 52) / 1000.0 : 0;
            if (len >= 58) { e->sf = f[56]; e->cr = f[57]; }
            if (len > 58) copy_fixed_string(e->radio_name, sizeof(e->radio_name), f + 58, len - 58);
            return 0;
        case 6:
            e->type = QTC_RADIO_MSG_SENT;
            if (len >= 10) { memcpy(e->expected_ack, f + 2, 4); e->expected_ack_len = 4; e->suggested_timeout_ms = get_u32(f + 6); }
            return 0;
        case 7: return parse_message(f, len, e, true, false);
        case 8: return parse_message(f, len, e, false, false);
        case 10: e->type = QTC_RADIO_NO_MORE_MESSAGES; return 0;
        case 11:
            e->type = QTC_RADIO_EXPORT_CONTACT;
            e->card_data_len = len - 1;
            if (e->card_data_len > sizeof(e->card_data)) return -1;
            if (e->card_data_len > 0) memcpy(e->card_data, f + 1, e->card_data_len);
            return 0;
        case 12: e->type = QTC_RADIO_BATTERY; if (len >= 3) e->value = get_u16(f + 1); return 0;
        case 13:
            e->type = QTC_RADIO_DEVICE_INFO;
            if (len >= 4) { e->max_contacts = f[2] * 2; e->max_channels = f[3]; }
            if (len >= 80) { copy_fixed_string(e->model, sizeof(e->model), f + 20, 40);
                              copy_fixed_string(e->version, sizeof(e->version), f + 60, 20); }
            return 0;
        case 16: return parse_message(f, len, e, true, true);
        case 17: return parse_message(f, len, e, false, true);
        case 18:
            if (len < 50) return -1;
            e->type = QTC_RADIO_CHANNEL_INFO; e->channel.index = f[1];
            copy_fixed_string(e->channel.name, sizeof(e->channel.name), f + 2, 32);
            memcpy(e->channel.secret, f + 34, 16); e->channel.configured = e->channel.name[0] != 0;
            e->channel.is_private = false;
            for (size_t i = 0; i < 16; i++) if (e->channel.secret[i] != 0) e->channel.is_private = true;
            return 0;
        case 0x82:
            if (len < 9) return -1;
            e->type = QTC_RADIO_ACK;
            e->expected_ack_len = 4;
            memcpy(e->expected_ack, f + 1, 4);
            e->round_trip_ms = get_u32(f + 5);
            return 0;
        case 0x83: e->type = QTC_RADIO_MESSAGES_WAITING; return 0;
        case 0x80:
        case 0x81:
            e->type = QTC_RADIO_CONTACTS_DIRTY;
            return 0;
        case 0x8a: return parse_contact(f, len, e);
        default: e->type = QTC_RADIO_UNKNOWN; e->value = f[0]; return 0;
    }
}

size_t qtc_cmd_app_start(uint8_t *out, size_t max, const char *name) {
    size_t n = name != NULL ? strlen(name) : 0; if (n > 64) n = 64;
    if (max < 8 + n) return 0;
    memset(out, 0, 8 + n);
    out[0] = CMD_APP_START;
    out[1] = 3;
    if (n) memcpy(out + 8, name, n);
    return 8 + n;
}
size_t qtc_cmd_device_query(uint8_t *out, size_t max) { if (max < 2) return 0; out[0] = CMD_DEVICE_QUERY; out[1] = 3; return 2; }
size_t qtc_cmd_get_contacts(uint8_t *out, size_t max, uint32_t since) { if (max < 5) return 0; out[0] = CMD_GET_CONTACTS; put_u32(out + 1, since); return 5; }
size_t qtc_cmd_get_channel(uint8_t *out, size_t max, int index) { if (max < 2 || index < 0 || index > 255) return 0; out[0] = CMD_GET_CHANNEL; out[1] = (uint8_t)index; return 2; }
size_t qtc_cmd_set_channel(uint8_t *out, size_t max, int index, const char *name, const uint8_t secret[16]) {
    if (max < 50 || index < 0 || index > 255 || name == NULL || secret == NULL) return 0;
    memset(out, 0, 50); out[0] = CMD_SET_CHANNEL; out[1] = (uint8_t)index;
    size_t n = strlen(name); if (n > 32) n = 32; memcpy(out + 2, name, n); memcpy(out + 34, secret, 16); return 50;
}
size_t qtc_cmd_sync_next_message(uint8_t *out, size_t max) { if (max < 1) return 0; out[0] = CMD_SYNC_MESSAGE; return 1; }
size_t qtc_cmd_send_direct(uint8_t *out, size_t max, const uint8_t dst[6], uint32_t ts, uint8_t attempt, const char *text) {
    size_t n = text != NULL ? strlen(text) : 0; if (n > 160) n = 160;
    if (max < 13 + n) return 0;
    out[0] = CMD_SEND_TXT;
    out[1] = 0;
    out[2] = attempt;
    put_u32(out + 3, ts);
    memcpy(out + 7, dst, 6); if (n) memcpy(out + 13, text, n); return 13 + n;
}
size_t qtc_cmd_send_channel(uint8_t *out, size_t max, int index, uint32_t ts, const char *text) {
    size_t n = text != NULL ? strlen(text) : 0; if (n > 160) n = 160;
    if (max < 7 + n || index < 0 || index > 255) return 0;
    out[0] = CMD_SEND_CHANNEL;
    out[1] = 0;
    out[2] = (uint8_t)index;
    put_u32(out + 3, ts); if (n) memcpy(out + 7, text, n); return 7 + n;
}
size_t qtc_cmd_get_battery(uint8_t *out, size_t max) { if (max < 1) return 0; out[0] = CMD_GET_BATTERY; return 1; }
size_t qtc_cmd_set_name(uint8_t *out, size_t max, const char *name) { size_t n = name ? strlen(name) : 0; if (max < 1 + n) return 0; out[0] = CMD_SET_NAME; memcpy(out + 1, name, n); return 1 + n; }
size_t qtc_cmd_set_tx_power(uint8_t *out, size_t max, int power) { if (max < 2) return 0; out[0] = CMD_SET_POWER; out[1] = (uint8_t)power; return 2; }
size_t qtc_cmd_advertise(uint8_t *out, size_t max, bool flood) { if (max < 2) return 0; out[0] = CMD_SEND_ADVERT; out[1] = flood ? 1 : 0; return 2; }
size_t qtc_cmd_export_self(uint8_t *out, size_t max) {
    if (max < 1) return 0;
    out[0] = CMD_EXPORT_CONTACT;
    return 1;
}
size_t qtc_cmd_set_radio_params(uint8_t *out, size_t max,
                                double freq_mhz, double bw_khz,
                                int sf, int cr, bool repeat_mode) {
    if (max < 12 || freq_mhz <= 0.0 || bw_khz <= 0.0 ||
        sf < 5 || sf > 12 || cr < 5 || cr > 8) return 0;
    uint32_t freq = (uint32_t)(freq_mhz * 1000.0 + 0.5);
    uint32_t bw = (uint32_t)(bw_khz * 1000.0 + 0.5);
    out[0] = CMD_SET_RADIO;
    put_u32(out + 1, freq);
    put_u32(out + 5, bw);
    out[9] = (uint8_t)sf;
    out[10] = (uint8_t)cr;
    out[11] = repeat_mode ? 1U : 0U;
    return 12;
}
size_t qtc_cmd_reset_path(uint8_t *out, size_t max, const uint8_t public_key[32]) {
    if (max < 33 || public_key == NULL) return 0;
    out[0] = CMD_RESET_PATH;
    memcpy(out + 1, public_key, 32);
    return 33;
}
