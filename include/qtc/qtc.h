#ifndef QTC_QTC_H
#define QTC_QTC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sqlite3.h>

#define QTC_VERSION "1.0.0"
#define QTC_APP_NAME "qtc"
#define QTC_DB_SCHEMA_VERSION 10
#define QTC_MAX_CONTACTS 1024
#define QTC_MAX_CHANNELS 40
#define QTC_MAX_MESSAGES 4096
#define QTC_MAX_INVITATIONS 256
#define QTC_MAX_CLIENTS 16
#define QTC_MAX_FRAME 8192
#define QTC_MAX_TEXT 768
#define QTC_MAX_NAME 96
#define QTC_MAX_ID 65
#define QTC_MAX_GROUP 64
#define QTC_MAX_URI 1024
#define QTC_MAX_PATH 4096

#define QTC_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#define QTC_LOG_ERROR 0
#define QTC_LOG_WARN 1
#define QTC_LOG_INFO 2
#define QTC_LOG_DEBUG 3

typedef enum {
    QTC_NODE_UNKNOWN = 0,
    QTC_NODE_PERSON = 1,
    QTC_NODE_REPEATER = 2,
    QTC_NODE_ROOM = 3,
    QTC_NODE_SENSOR = 4
} qtc_node_type;

typedef enum {
    QTC_CONV_CONTACT = 1,
    QTC_CONV_CHANNEL = 2
} qtc_conversation_kind;

typedef enum {
    QTC_MSG_INCOMING = 0,
    QTC_MSG_OUTGOING = 1
} qtc_message_direction;

typedef enum {
    QTC_MSG_QUEUED = 0,
    QTC_MSG_SENDING = 1,
    QTC_MSG_SENT = 2,
    QTC_MSG_DELIVERED = 3,
    QTC_MSG_UNCONFIRMED = 4,
    QTC_MSG_FAILED = 5
} qtc_message_status;

typedef enum {
    QTC_INVITE_PENDING = 0,
    QTC_INVITE_ACCEPTED = 1,
    QTC_INVITE_IGNORED = 2,
    QTC_INVITE_INVALID = 3,
    QTC_INVITE_EXPIRED = 4
} qtc_invite_status;

typedef struct {
    char id[QTC_MAX_ID];
    char prefix[13];
    char name[QTC_MAX_NAME];
    char alias[QTC_MAX_NAME];
    char favorite_group[QTC_MAX_GROUP];
    qtc_node_type node_type;
    int route_hops;
    bool route_known;
    bool favorite;
    int unread;
    int64_t last_heard;
    double latitude;
    double longitude;
    uint8_t flags;
} qtc_contact;

typedef struct {
    int index;
    char name[33];
    uint8_t secret[16];
    bool configured;
    bool is_private;
    int unread;
} qtc_channel;

typedef struct {
    int64_t id;
    qtc_conversation_kind conversation_kind;
    char conversation_key[QTC_MAX_ID];
    qtc_message_direction direction;
    int64_t sender_timestamp;
    int attempt;
    qtc_message_status status;
    char message_key[160];
    char logical_key[160];
    char text[QTC_MAX_TEXT];
    int part_index;
    int part_total;
    uint32_t ack_code;
    int64_t ack_deadline;
    int snr_quarter_db;
    int path_len;
    int64_t created_at;
} qtc_message;

typedef struct {
    int64_t id;
    char sender_contact_id[QTC_MAX_ID];
    char channel_name[33];
    char uri[QTC_MAX_URI];
    int64_t source_message_id;
    qtc_invite_status status;
    int64_t received_at;
    int64_t action_at;
} qtc_invitation;

typedef struct {
    bool desktop_notifications;
    bool notify_direct;
    bool notify_channel;
    bool suppress_open_conversation;
    bool sound_enabled;
    bool banner_enabled;
    bool show_signal;
    int stored_poll_seconds;
    bool retry_unconfirmed;
    int max_direct_attempts;
    bool reset_stale_route;
    int theme;
    int tx_power;
    char serial_device[QTC_MAX_PATH];
} qtc_settings;

typedef struct {
    char profile[64];
    char data_dir[QTC_MAX_PATH];
    char config_dir[QTC_MAX_PATH];
    char runtime_dir[QTC_MAX_PATH];
    char db_path[QTC_MAX_PATH];
    char socket_path[QTC_MAX_PATH];
    char lock_path[QTC_MAX_PATH];
    char pid_path[QTC_MAX_PATH];
} qtc_paths;

typedef struct {
    sqlite3 *db;
    char path[QTC_MAX_PATH];
} qtc_db;

typedef struct {
    uint64_t contacts;
    uint64_t channels;
    uint64_t messages;
    uint64_t settings;
    uint64_t connection;
    uint64_t status;
} qtc_revisions;

typedef struct {
    qtc_contact contacts[QTC_MAX_CONTACTS];
    size_t contact_count;
    qtc_channel channels[QTC_MAX_CHANNELS];
    size_t channel_count;
    qtc_message messages[QTC_MAX_MESSAGES];
    size_t message_count;
    qtc_invitation invitations[QTC_MAX_INVITATIONS];
    size_t invitation_count;
    qtc_settings settings;
    qtc_revisions revisions;
    bool radio_connected;
    char radio_name[QTC_MAX_NAME];
    char radio_model[QTC_MAX_NAME];
    char radio_version[QTC_MAX_NAME];
    int radio_max_channels;
    int radio_max_contacts;
    int radio_tx_power;
    int radio_max_tx_power;
    double radio_freq;
    double radio_bw;
    int radio_sf;
    int radio_cr;
} qtc_state;

#endif
