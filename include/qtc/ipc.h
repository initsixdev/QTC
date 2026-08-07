#ifndef QTC_IPC_H
#define QTC_IPC_H

#include "qtc/qtc.h"
#include <sys/types.h>

typedef enum {
    QTC_IPC_HELLO = 1,
    QTC_IPC_STATE_BEGIN = 2,
    QTC_IPC_CONTACT = 3,
    QTC_IPC_CHANNEL = 4,
    QTC_IPC_MESSAGE = 5,
    QTC_IPC_SETTINGS = 6,
    QTC_IPC_STATE_END = 7,
    QTC_IPC_STATUS = 8,
    QTC_IPC_INVITATION = 9,
    QTC_IPC_CORE_INFO = 10,
    QTC_IPC_SEND_DIRECT = 20,
    QTC_IPC_SEND_CHANNEL = 21,
    QTC_IPC_MARK_READ = 22,
    QTC_IPC_SET_FAVORITE = 23,
    QTC_IPC_CHANNEL_CREATE = 24,
    QTC_IPC_CHANNEL_JOIN = 25,
    QTC_IPC_CHANNEL_ROTATE = 26,
    QTC_IPC_CHANNEL_LEAVE = 27,
    QTC_IPC_CHANNEL_INVITE = 28,
    QTC_IPC_INVITE_ACCEPT = 29,
    QTC_IPC_INVITE_IGNORE = 30,
    QTC_IPC_SYNC = 31,
    QTC_IPC_SHUTDOWN = 32,
    QTC_IPC_PING = 33,
    QTC_IPC_PONG = 34,
    QTC_IPC_SET_ALIAS = 35,
    QTC_IPC_ACTIVE_CONVERSATION = 36,
    QTC_IPC_DEVICE_SET_NAME = 37,
    QTC_IPC_DEVICE_SET_TX_POWER = 38,
    QTC_IPC_DEVICE_ADVERTISE = 39,
    QTC_IPC_DEVICE_RECONNECT = 40,
    QTC_IPC_DEVICE_SYNC_MESSAGES = 41,
    QTC_IPC_BANNER = 42,
    QTC_IPC_DEVICE_COPY_CARD = 43,
    QTC_IPC_DEVICE_SET_PRESET = 44,
    QTC_IPC_CLIPBOARD_TEXT = 45,
    QTC_IPC_ERROR = 255
} qtc_ipc_type;

#define QTC_IPC_PROTOCOL_VERSION 2U

typedef struct {
    uint8_t type;
    uint32_t length;
    uint8_t payload[QTC_MAX_FRAME];
} qtc_ipc_frame;


typedef struct {
    uint32_t protocol_version;
    char client_name[32];
    char app_version[16];
} qtc_ipc_hello;

typedef struct {
    uint32_t protocol_version;
    char app_version[16];
} qtc_ipc_core_info;

typedef struct {
    bool radio_connected;
    bool demo_mode;
    int max_channels;
    int max_contacts;
    char radio_name[QTC_MAX_NAME];
    char radio_model[QTC_MAX_NAME];
    char radio_version[QTC_MAX_NAME];
    int tx_power;
    int max_tx_power;
    double freq;
    double bw;
    int sf;
    int cr;
    qtc_revisions revisions;
    char message[160];
} qtc_ipc_status_payload;

typedef struct {
    char contact_id[QTC_MAX_ID];
    char text[QTC_MAX_TEXT];
} qtc_ipc_send_direct_payload;

typedef struct {
    int channel_index;
    char text[QTC_MAX_TEXT];
} qtc_ipc_send_channel_payload;

typedef struct {
    qtc_conversation_kind kind;
    char key[QTC_MAX_ID];
} qtc_ipc_mark_read_payload;

typedef struct {
    char contact_id[QTC_MAX_ID];
    bool favorite;
    char group[QTC_MAX_GROUP];
} qtc_ipc_favorite_payload;

typedef struct {
    char name[33];
    char uri[QTC_MAX_URI];
    int channel_index;
} qtc_ipc_channel_action_payload;

typedef struct {
    int channel_index;
    char contact_id[QTC_MAX_ID];
} qtc_ipc_channel_invite_payload;

typedef struct {
    char contact_id[QTC_MAX_ID];
    char value[QTC_MAX_NAME];
} qtc_ipc_contact_text_payload;

typedef struct {
    bool active;
    qtc_conversation_kind kind;
    char key[QTC_MAX_ID];
} qtc_ipc_active_payload;

typedef struct {
    char text[QTC_MAX_NAME];
    int value;
    bool flag;
} qtc_ipc_device_action_payload;

typedef struct {
    char name[32];
    double freq_mhz;
    double bw_khz;
    int sf;
    int cr;
    bool repeat_mode;
} qtc_ipc_radio_preset_payload;

typedef struct {
    char text[QTC_MAX_URI];
} qtc_ipc_clipboard_payload;

typedef struct {
    qtc_conversation_kind kind;
    char key[QTC_MAX_ID];
    char title[QTC_MAX_NAME];
    char body[256];
    int64_t created_at;
} qtc_ipc_banner_payload;

typedef struct {
    uint8_t header[5];
    size_t header_len;
    uint32_t expected;
    uint8_t type;
    uint8_t payload[QTC_MAX_FRAME];
    size_t payload_len;
} qtc_ipc_reader;

typedef void (*qtc_ipc_frame_cb)(const qtc_ipc_frame *frame, void *userdata);

void qtc_ipc_reader_init(qtc_ipc_reader *reader);
int qtc_ipc_reader_feed(qtc_ipc_reader *reader, const uint8_t *data, size_t len,
                        qtc_ipc_frame_cb callback, void *userdata);
int qtc_ipc_send(int fd, uint8_t type, const void *payload, uint32_t length);
int qtc_ipc_send_nonblocking(int fd, uint8_t type, const void *payload, uint32_t length);
int qtc_ipc_recv_blocking(int fd, qtc_ipc_frame *frame, int timeout_ms);
int qtc_ipc_server_open(const char *socket_path);
int qtc_ipc_client_connect(const char *socket_path, int timeout_ms);
int qtc_ipc_verify_peer_uid(int fd, uid_t expected_uid);

#endif
