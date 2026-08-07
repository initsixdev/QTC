#define _GNU_SOURCE
#include "qtc/core.h"
#include "qtc/db.h"
#include "qtc/invite.h"
#include "qtc/ipc.h"
#include "qtc/message.h"
#include "qtc/notify.h"
#include "qtc/platform.h"
#include "qtc/protocol.h"
#include "qtc/serial.h"
#include "qtc/util.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define RADIO_QUEUE 128
#define RADIO_DEFAULT_TIMEOUT_MS 1500
#define RADIO_INBOX_TIMEOUT_MS 250
#define RADIO_CONTACTS_TIMEOUT_MS 5000
#define INBOX_FALLBACK_POLL_MS 500
#define BACKGROUND_SYNC_DELAY_MS 1500

typedef enum {
    RADIO_PURPOSE_GENERIC = 0,
    RADIO_PURPOSE_STARTUP_APP,
    RADIO_PURPOSE_STARTUP_DEVICE,
    RADIO_PURPOSE_DIRECT_SEND,
    RADIO_PURPOSE_CHANNEL_SEND,
    RADIO_PURPOSE_EXPORT_SELF,
    RADIO_PURPOSE_ADVERT_ZERO_HOP,
    RADIO_PURPOSE_ADVERT_FLOOD
} radio_purpose;

typedef enum {
    RADIO_PRIORITY_STARTUP = 0,
    RADIO_PRIORITY_URGENT = 1,
    RADIO_PRIORITY_INBOX = 2,
    RADIO_PRIORITY_NORMAL = 3,
    RADIO_PRIORITY_BACKGROUND = 4
} radio_priority;

typedef enum {
    RADIO_SESSION_DOWN = 0,
    RADIO_SESSION_WAIT_APP_START,
    RADIO_SESSION_WAIT_DEVICE_INFO,
    RADIO_SESSION_READY
} radio_session_phase;

typedef struct {
    uint8_t data[QTC_MAX_FRAME];
    size_t len;
    qtc_radio_event_type expected_a;
    qtc_radio_event_type expected_b;
    qtc_radio_event_type expected_c;
    char message_key[160];
    int transport_attempts;
    int message_attempt;
    radio_purpose purpose;
    radio_priority priority;
    uint64_t inbox_generation;
} radio_command;

typedef struct {
    int fd;
    qtc_ipc_reader reader;
    bool active;
    bool conversation_active;
    bool hello_ok;
    qtc_conversation_kind conversation_kind;
    char conversation_key[QTC_MAX_ID];
} core_client;

typedef struct {
    qtc_paths paths;
    qtc_db db;
    qtc_state state;
    bool demo;
    bool running;
    int server_fd;
    int lock_fd;
    core_client clients[QTC_MAX_CLIENTS];
    int current_client;
    qtc_serial serial;
    char requested_device[QTC_MAX_PATH];
    int64_t reconnect_at;
    int64_t next_stored_poll;
    int64_t background_sync_after;
    bool contacts_sync_needed;
    uint32_t contact_since;
    int next_channel_sync;
    uint64_t inbox_generation;
    uint64_t inbox_empty_generation;
    uint64_t incoming_serial;
    int clipboard_client;
    radio_command queue[RADIO_QUEUE];
    size_t queue_head;
    size_t queue_count;
    bool radio_pending;
    radio_session_phase session_phase;
    radio_command pending;
    int64_t pending_since;
    char last_status[160];
} core_ctx;

static qtc_contact *find_contact(core_ctx *c, const char *id_or_prefix);
static qtc_channel *find_channel(core_ctx *c, int index);
static qtc_message *find_message(core_ctx *c, const char *message_key);
static void service_radio_queue(core_ctx *c);
static void resume_outgoing_messages(core_ctx *c);
static void disconnect_radio(core_ctx *c, const char *reason);
static void schedule_background_sync(core_ctx *c, int64_t delay_ms);

static volatile sig_atomic_t g_stop;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void set_status(core_ctx *c, const char *message) {
    if (strcmp(c->last_status, message) != 0) {
        qtc_strlcpy(c->last_status, message, sizeof(c->last_status));
        c->state.revisions.status++;
    }
}

static int reload_state(core_ctx *c, uint64_t contact_rev, uint64_t channel_rev,
                        uint64_t message_rev, uint64_t settings_rev) {
    qtc_state *fresh = calloc(1, sizeof(*fresh));
    if (fresh == NULL) return -1;
    if (qtc_db_load_state(&c->db, fresh) != 0) { free(fresh); return -1; }
    fresh->radio_connected = c->state.radio_connected;
    qtc_strlcpy(fresh->radio_name, c->state.radio_name, sizeof(fresh->radio_name));
    qtc_strlcpy(fresh->radio_model, c->state.radio_model, sizeof(fresh->radio_model));
    qtc_strlcpy(fresh->radio_version, c->state.radio_version, sizeof(fresh->radio_version));
    fresh->radio_max_channels = c->state.radio_max_channels;
    fresh->radio_max_contacts = c->state.radio_max_contacts;
    fresh->radio_tx_power = c->state.radio_tx_power;
    fresh->radio_max_tx_power = c->state.radio_max_tx_power;
    fresh->radio_freq = c->state.radio_freq; fresh->radio_bw = c->state.radio_bw;
    fresh->radio_sf = c->state.radio_sf; fresh->radio_cr = c->state.radio_cr;
    fresh->revisions = c->state.revisions;
    fresh->revisions.contacts += contact_rev;
    fresh->revisions.channels += channel_rev;
    fresh->revisions.messages += message_rev;
    fresh->revisions.settings += settings_rev;
    c->state = *fresh;
    free(fresh);
    return 0;
}

static int send_status_frame(core_ctx *c, int fd) {
    qtc_ipc_status_payload s = {0};
    s.radio_connected = c->state.radio_connected; s.demo_mode = c->demo;
    s.max_channels = c->state.radio_max_channels; s.max_contacts = c->state.radio_max_contacts;
    qtc_strlcpy(s.radio_name, c->state.radio_name, sizeof(s.radio_name));
    qtc_strlcpy(s.radio_model, c->state.radio_model, sizeof(s.radio_model));
    qtc_strlcpy(s.radio_version, c->state.radio_version, sizeof(s.radio_version));
    s.tx_power = c->state.radio_tx_power;
    s.max_tx_power = c->state.radio_max_tx_power;
    s.freq = c->state.radio_freq;
    s.bw = c->state.radio_bw;
    s.sf = c->state.radio_sf;
    s.cr = c->state.radio_cr;
    s.revisions = c->state.revisions; qtc_strlcpy(s.message, c->last_status, sizeof(s.message));
    return qtc_ipc_send(fd, QTC_IPC_STATUS, &s, sizeof(s));
}

static int send_core_info(int fd) {
    qtc_ipc_core_info info = {.protocol_version = QTC_IPC_PROTOCOL_VERSION};
    qtc_strlcpy(info.app_version, QTC_VERSION, sizeof(info.app_version));
    return qtc_ipc_send(fd, QTC_IPC_CORE_INFO, &info, sizeof(info));
}

static int send_snapshot(core_ctx *c, int fd) {
    if (qtc_ipc_send(fd, QTC_IPC_STATE_BEGIN, &c->state.revisions, sizeof(c->state.revisions)) != 0) return -1;
    for (size_t i = 0; i < c->state.contact_count; i++)
        if (qtc_ipc_send(fd, QTC_IPC_CONTACT, &c->state.contacts[i], sizeof(qtc_contact)) != 0) return -1;
    for (size_t i = 0; i < c->state.channel_count; i++)
        if (qtc_ipc_send(fd, QTC_IPC_CHANNEL, &c->state.channels[i], sizeof(qtc_channel)) != 0) return -1;
    /* A bounded history keeps attach instantaneous while the full database remains preserved. */
    size_t first = c->state.message_count > 768 ? c->state.message_count - 768 : 0;
    for (size_t i = first; i < c->state.message_count; i++)
        if (qtc_ipc_send(fd, QTC_IPC_MESSAGE, &c->state.messages[i], sizeof(qtc_message)) != 0) return -1;
    for (size_t i = 0; i < c->state.invitation_count; i++)
        if (qtc_ipc_send(fd, QTC_IPC_INVITATION, &c->state.invitations[i], sizeof(qtc_invitation)) != 0) return -1;
    if (qtc_ipc_send(fd, QTC_IPC_SETTINGS, &c->state.settings, sizeof(qtc_settings)) != 0) return -1;
    if (send_status_frame(c, fd) != 0) return -1;
    return qtc_ipc_send(fd, QTC_IPC_STATE_END, NULL, 0);
}

static void close_client(core_ctx *c, int index) {
    if (index >= 0 && index < QTC_MAX_CLIENTS && c->clients[index].active) {
        close(c->clients[index].fd);
        c->clients[index].fd = -1;
        c->clients[index].active = false;
        c->clients[index].hello_ok = false;
        c->clients[index].conversation_active = false;
        c->clients[index].conversation_key[0] = 0;
    }
}

static void broadcast_snapshot(core_ctx *c) {
    for (int i = 0; i < QTC_MAX_CLIENTS; i++) {
        if (c->clients[i].active && send_snapshot(c, c->clients[i].fd) != 0) close_client(c, i);
    }
}

static void broadcast_delta(core_ctx *c, uint8_t type, const void *payload, uint32_t length) {
    for (int i = 0; i < QTC_MAX_CLIENTS; i++) {
        if (c->clients[i].active &&
            qtc_ipc_send_nonblocking(c->clients[i].fd, type, payload, length) != 0)
            close_client(c, i);
    }
}

static void broadcast_status(core_ctx *c) {
    qtc_ipc_status_payload s = {0};
    s.radio_connected = c->state.radio_connected;
    s.demo_mode = c->demo;
    s.max_channels = c->state.radio_max_channels;
    s.max_contacts = c->state.radio_max_contacts;
    qtc_strlcpy(s.radio_name, c->state.radio_name, sizeof(s.radio_name));
    qtc_strlcpy(s.radio_model, c->state.radio_model, sizeof(s.radio_model));
    qtc_strlcpy(s.radio_version, c->state.radio_version, sizeof(s.radio_version));
    s.tx_power = c->state.radio_tx_power;
    s.max_tx_power = c->state.radio_max_tx_power;
    s.freq = c->state.radio_freq;
    s.bw = c->state.radio_bw;
    s.sf = c->state.radio_sf;
    s.cr = c->state.radio_cr;
    s.revisions = c->state.revisions;
    qtc_strlcpy(s.message, c->last_status, sizeof(s.message));
    broadcast_delta(c, QTC_IPC_STATUS, &s, sizeof(s));
}

static bool advert_purpose(radio_purpose purpose) {
    return purpose == RADIO_PURPOSE_ADVERT_ZERO_HOP ||
           purpose == RADIO_PURPOSE_ADVERT_FLOOD;
}

static void advert_status(core_ctx *c, radio_purpose purpose,
                          const char *result) {
    char message[96];
    snprintf(message, sizeof(message), "%s advertisement %s",
             purpose == RADIO_PURPOSE_ADVERT_FLOOD ? "Flood" : "0-hop",
             result);
    set_status(c, message);
    broadcast_status(c);
}

static void state_upsert_contact(core_ctx *c, const qtc_contact *incoming, bool preserve_local) {
    qtc_contact merged = *incoming;
    qtc_contact *old = find_contact(c, incoming->id);
    if (old != NULL && preserve_local) {
        qtc_strlcpy(merged.alias, old->alias, sizeof(merged.alias));
        merged.favorite = old->favorite;
        qtc_strlcpy(merged.favorite_group, old->favorite_group,
                    sizeof(merged.favorite_group));
        merged.unread = old->unread;
        if (old->last_heard > merged.last_heard) merged.last_heard = old->last_heard;
    }
    if (old != NULL) {
        *old = merged;
    } else if (c->state.contact_count < QTC_MAX_CONTACTS) {
        c->state.contacts[c->state.contact_count++] = merged;
        old = &c->state.contacts[c->state.contact_count - 1U];
    }
    if (old != NULL) {
        c->state.revisions.contacts++;
        broadcast_delta(c, QTC_IPC_CONTACT, old, sizeof(*old));
    }
}

static void state_upsert_channel(core_ctx *c, const qtc_channel *incoming, bool preserve_unread) {
    qtc_channel merged = *incoming;
    qtc_channel *old = find_channel(c, incoming->index);
    if (old != NULL && preserve_unread) merged.unread = old->unread;
    if (old != NULL) {
        *old = merged;
    } else if (c->state.channel_count < QTC_MAX_CHANNELS) {
        c->state.channels[c->state.channel_count++] = merged;
        old = &c->state.channels[c->state.channel_count - 1U];
    }
    if (old != NULL) {
        c->state.revisions.channels++;
        broadcast_delta(c, QTC_IPC_CHANNEL, old, sizeof(*old));
    }
}

static qtc_message *state_upsert_message(core_ctx *c, const qtc_message *incoming,
                                         bool notify_clients) {
    qtc_message *message = find_message(c, incoming->message_key);
    if (message != NULL) {
        *message = *incoming;
    } else {
        if (c->state.message_count == QTC_MAX_MESSAGES) {
            memmove(&c->state.messages[0], &c->state.messages[1],
                    (QTC_MAX_MESSAGES - 1U) * sizeof(c->state.messages[0]));
            c->state.message_count--;
        }
        c->state.messages[c->state.message_count++] = *incoming;
        message = &c->state.messages[c->state.message_count - 1U];
    }
    c->state.revisions.messages++;
    if (notify_clients)
        broadcast_delta(c, QTC_IPC_MESSAGE, message, sizeof(*message));
    return message;
}

static void state_upsert_invitation(core_ctx *c, const qtc_invitation *incoming) {
    qtc_invitation *invitation = NULL;
    for (size_t i = 0; i < c->state.invitation_count; i++) {
        if (c->state.invitations[i].id == incoming->id) {
            invitation = &c->state.invitations[i];
            break;
        }
    }
    if (invitation != NULL) {
        *invitation = *incoming;
    } else if (c->state.invitation_count < QTC_MAX_INVITATIONS) {
        c->state.invitations[c->state.invitation_count++] = *incoming;
        invitation = &c->state.invitations[c->state.invitation_count - 1U];
    }
    if (invitation != NULL)
        broadcast_delta(c, QTC_IPC_INVITATION, invitation, sizeof(*invitation));
}

static qtc_contact *find_contact(core_ctx *c, const char *id_or_prefix) {
    for (size_t i = 0; i < c->state.contact_count; i++) {
        qtc_contact *ct = &c->state.contacts[i];
        if (strcmp(ct->id, id_or_prefix) == 0 || strcmp(ct->prefix, id_or_prefix) == 0 ||
            strncmp(ct->id, id_or_prefix, 12) == 0) return ct;
    }
    return NULL;
}

static qtc_channel *find_channel(core_ctx *c, int index) {
    for (size_t i = 0; i < c->state.channel_count; i++) if (c->state.channels[i].index == index) return &c->state.channels[i];
    return NULL;
}

static qtc_message *find_message(core_ctx *c, const char *message_key) {
    for (size_t i = 0; i < c->state.message_count; i++)
        if (strcmp(c->state.messages[i].message_key, message_key) == 0) return &c->state.messages[i];
    return NULL;
}

static uint32_t ack_code_from_bytes(const uint8_t bytes[4]) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static bool queue_has_code(const core_ctx *c, uint8_t code) {
    if (c->radio_pending && c->pending.len > 0 && c->pending.data[0] == code) return true;
    for (size_t i = 0; i < c->queue_count; i++) {
        size_t pos = (c->queue_head + i) % RADIO_QUEUE;
        if (c->queue[pos].len > 0 && c->queue[pos].data[0] == code) return true;
    }
    return false;
}

static int queue_radio_ex(core_ctx *c, const uint8_t *data, size_t len,
                          qtc_radio_event_type a, qtc_radio_event_type b, qtc_radio_event_type d,
                          const char *message_key, radio_purpose purpose, int message_attempt,
                          radio_priority priority) {
    if (len == 0 || len > QTC_MAX_FRAME || c->queue_count >= RADIO_QUEUE) return -1;
    size_t pos = (c->queue_head + c->queue_count) % RADIO_QUEUE;
    radio_command *cmd = &c->queue[pos]; memset(cmd, 0, sizeof(*cmd));
    memcpy(cmd->data, data, len); cmd->len = len; cmd->expected_a = a; cmd->expected_b = b; cmd->expected_c = d;
    qtc_strlcpy(cmd->message_key, message_key != NULL ? message_key : "", sizeof(cmd->message_key));
    cmd->purpose = purpose;
    cmd->message_attempt = message_attempt;
    cmd->priority = priority;
    c->queue_count++;
    return 0;
}

static int queue_radio(core_ctx *c, const uint8_t *data, size_t len,
                       qtc_radio_event_type a, qtc_radio_event_type b, qtc_radio_event_type d,
                       const char *message_key) {
    return queue_radio_ex(c, data, len, a, b, d, message_key,
                          RADIO_PURPOSE_GENERIC, 0, RADIO_PRIORITY_NORMAL);
}

static int queue_radio_urgent(core_ctx *c, const uint8_t *data, size_t len,
                              qtc_radio_event_type a, qtc_radio_event_type b,
                              qtc_radio_event_type d, const char *message_key) {
    return queue_radio_ex(c, data, len, a, b, d, message_key,
                          RADIO_PURPOSE_GENERIC, 0, RADIO_PRIORITY_URGENT);
}

static int queue_radio_inbox(core_ctx *c, const uint8_t *data, size_t len,
                             qtc_radio_event_type a, qtc_radio_event_type b,
                             qtc_radio_event_type d, const char *message_key) {
    int rc = queue_radio_ex(c, data, len, a, b, d, message_key,
                            RADIO_PURPOSE_GENERIC, 0, RADIO_PRIORITY_INBOX);
    if (rc == 0) {
        size_t pos = (c->queue_head + c->queue_count - 1U) % RADIO_QUEUE;
        c->queue[pos].inbox_generation = c->inbox_generation;
    }
    return rc;
}

static int queue_radio_background(core_ctx *c, const uint8_t *data, size_t len,
                                  qtc_radio_event_type a, qtc_radio_event_type b,
                                  qtc_radio_event_type d, const char *message_key) {
    return queue_radio_ex(c, data, len, a, b, d, message_key,
                          RADIO_PURPOSE_GENERIC, 0, RADIO_PRIORITY_BACKGROUND);
}

static int queue_radio_startup(core_ctx *c, const uint8_t *data, size_t len,
                               qtc_radio_event_type a, qtc_radio_event_type b,
                               radio_purpose purpose) {
    return queue_radio_ex(c, data, len, a, b, QTC_RADIO_NONE, "",
                          purpose, 0, RADIO_PRIORITY_STARTUP);
}

static bool queue_has_message(const core_ctx *c, const char *message_key) {
    if (message_key == NULL || *message_key == 0) return false;
    if (c->radio_pending && strcmp(c->pending.message_key, message_key) == 0) return true;
    for (size_t i = 0; i < c->queue_count; i++) {
        size_t pos = (c->queue_head + i) % RADIO_QUEUE;
        if (strcmp(c->queue[pos].message_key, message_key) == 0) return true;
    }
    return false;
}

static bool inbox_needs_drain(const core_ctx *c) {
    return c->inbox_empty_generation < c->inbox_generation;
}

static void request_inbox_drain(core_ctx *c, bool force_new_generation) {
    if (force_new_generation || !inbox_needs_drain(c)) c->inbox_generation++;
}

static void queue_next_inbox_message(core_ctx *c, radio_priority priority) {
    if (c->session_phase != RADIO_SESSION_READY ||
        !inbox_needs_drain(c) || queue_has_code(c, 10)) return;
    uint8_t cmd[2];
    size_t len = qtc_cmd_sync_next_message(cmd, sizeof(cmd));
    int rc;
    if (priority == RADIO_PRIORITY_URGENT) {
        rc = queue_radio_ex(c, cmd, len, QTC_RADIO_CONTACT_MESSAGE,
                            QTC_RADIO_CHANNEL_MESSAGE, QTC_RADIO_NO_MORE_MESSAGES,
                            "", RADIO_PURPOSE_GENERIC, 0, RADIO_PRIORITY_URGENT);
        if (rc == 0) {
            size_t pos = (c->queue_head + c->queue_count - 1U) % RADIO_QUEUE;
            c->queue[pos].inbox_generation = c->inbox_generation;
        }
    } else {
        rc = queue_radio_inbox(c, cmd, len, QTC_RADIO_CONTACT_MESSAGE,
                               QTC_RADIO_CHANNEL_MESSAGE, QTC_RADIO_NO_MORE_MESSAGES, "");
    }
    (void)rc;
}

static void assign_unique_incoming_key(core_ctx *c, qtc_message *message) {
    uint64_t random_nonce = 0;
    if (qtc_secure_random(&random_nonce, sizeof(random_nonce)) != 0)
        random_nonce = ((uint64_t)qtc_now_millis() << 16) ^ ++c->incoming_serial;
    else
        c->incoming_serial++;
    snprintf(message->message_key, sizeof(message->message_key),
             "rx:%d:%.24s:%lld:%016llx:%llu",
             message->conversation_kind, message->conversation_key,
             (long long)message->sender_timestamp,
             (unsigned long long)random_nonce,
             (unsigned long long)c->incoming_serial);
    if (message->part_total <= 1)
        qtc_strlcpy(message->logical_key, message->message_key,
                    sizeof(message->logical_key));
}

static void queue_startup(core_ctx *c) {
    uint8_t cmd[256]; size_t n;
    char app_name[64];
    snprintf(app_name, sizeof(app_name), "QTC Terminal %s", QTC_VERSION);
    n = qtc_cmd_app_start(cmd, sizeof(cmd), app_name);
    c->session_phase = RADIO_SESSION_WAIT_APP_START;
    (void)queue_radio_startup(c, cmd, n, QTC_RADIO_SELF_INFO,
                              QTC_RADIO_ERROR, RADIO_PURPOSE_STARTUP_APP);
}

static void queue_device_query(core_ctx *c) {
    uint8_t cmd[8];
    size_t n = qtc_cmd_device_query(cmd, sizeof(cmd));
    c->session_phase = RADIO_SESSION_WAIT_DEVICE_INFO;
    (void)queue_radio_startup(c, cmd, n, QTC_RADIO_DEVICE_INFO,
                              QTC_RADIO_ERROR, RADIO_PURPOSE_STARTUP_DEVICE);
}

static void schedule_background_sync(core_ctx *c, int64_t delay_ms) {
    c->contacts_sync_needed = true;
    c->next_channel_sync = 0;
    c->background_sync_after = qtc_now_millis() + delay_ms;
}

static void radio_session_ready(core_ctx *c) {
    c->session_phase = RADIO_SESSION_READY;
    c->next_stored_poll = qtc_now_millis() + INBOX_FALLBACK_POLL_MS;
    set_status(c, "MeshCore session ready");
    broadcast_status(c);

    /* Interactive traffic and queued messages are resumed before any large
     * roster synchronization. The radio protocol is strictly serialized, so
     * a GET_CONTACTS stream must never be allowed to jump ahead of inbox work
     * or messages the user already asked us to send. */
    request_inbox_drain(c, true);
    resume_outgoing_messages(c);
    schedule_background_sync(c, c->state.contact_count == 0 ? 100 : BACKGROUND_SYNC_DELAY_MS);
}

static void service_background_sync(core_ctx *c) {
    if (c->session_phase != RADIO_SESSION_READY || c->radio_pending ||
        c->queue_count != 0 || inbox_needs_drain(c) ||
        qtc_now_millis() < c->background_sync_after) return;

    uint8_t cmd[16];
    size_t n;
    if (c->contacts_sync_needed) {
        n = qtc_cmd_get_contacts(cmd, sizeof(cmd), c->contact_since);
        if (queue_radio_background(c, cmd, n, QTC_RADIO_CONTACT_END,
                                   QTC_RADIO_ERROR, QTC_RADIO_NONE, "") == 0) {
            c->contacts_sync_needed = false;
            qtc_log(QTC_LOG_DEBUG, "radio background contact sync queued");
        }
        return;
    }

    if (c->next_channel_sync < c->state.radio_max_channels) {
        n = qtc_cmd_get_channel(cmd, sizeof(cmd), c->next_channel_sync++);
        (void)queue_radio_background(c, cmd, n, QTC_RADIO_CHANNEL_INFO,
                                     QTC_RADIO_ERROR, QTC_RADIO_NONE, "");
    }
}

static int update_message_state(core_ctx *c, const char *message_key,
                                qtc_message_status status, int attempt,
                                uint32_t ack_code, int64_t ack_deadline,
                                bool notify_clients) {
    if (qtc_db_update_message_send_state(&c->db, message_key, status, attempt,
                                         ack_code, ack_deadline) != 0) return -1;
    qtc_message *message = find_message(c, message_key);
    if (message != NULL) {
        message->status = status;
        message->attempt = attempt;
        message->ack_code = ack_code;
        message->ack_deadline = ack_deadline;
        c->state.revisions.messages++;
        if (notify_clients)
            broadcast_delta(c, QTC_IPC_MESSAGE, message, sizeof(*message));
    }
    return 0;
}

static int message_wire_text(const qtc_message *m, char *wire, size_t wire_len) {
    if (m->part_total <= 1) {
        qtc_strlcpy(wire, m->text, wire_len);
        return 0;
    }
    const char *last_colon = strrchr(m->logical_key, ':');
    if (last_colon == NULL || strlen(last_colon + 1) != 8) return -1;
    char *end = NULL;
    unsigned long value = strtoul(last_colon + 1, &end, 16);
    if (end == NULL || *end != 0 || value > UINT32_MAX) return -1;
    return qtc_long_wire_build((uint32_t)value, m->part_index, m->part_total,
                               m->text, wire, wire_len);
}

static int queue_message_send(core_ctx *c, const qtc_message *m) {
    if (m == NULL || queue_has_message(c, m->message_key)) return m == NULL ? -1 : 0;
    char wire[QTC_DIRECT_RADIO_TEXT_MAX + 1];
    if (message_wire_text(m, wire, sizeof(wire)) != 0) return -1;
    uint8_t cmd[256];
    size_t n = 0;
    radio_purpose purpose;
    if (m->conversation_kind == QTC_CONV_CONTACT) {
        qtc_contact *ct = find_contact(c, m->conversation_key);
        uint8_t prefix[6];
        if (ct == NULL || qtc_hex_decode(ct->prefix, prefix, sizeof(prefix)) != 0) return -1;
        if (c->state.settings.reset_stale_route && m->attempt > 0) {
            uint8_t public_key[32], reset[40];
            if (qtc_hex_decode(ct->id, public_key, sizeof(public_key)) == 0) {
                size_t reset_len = qtc_cmd_reset_path(reset, sizeof(reset), public_key);
                (void)queue_radio(c, reset, reset_len, QTC_RADIO_OK, QTC_RADIO_ERROR,
                                  QTC_RADIO_NONE, "");
            }
        }
        n = qtc_cmd_send_direct(cmd, sizeof(cmd), prefix,
                                (uint32_t)m->sender_timestamp,
                                (uint8_t)m->attempt, wire);
        purpose = RADIO_PURPOSE_DIRECT_SEND;
    } else {
        int channel_index = atoi(m->conversation_key);
        n = qtc_cmd_send_channel(cmd, sizeof(cmd), channel_index,
                                 (uint32_t)m->sender_timestamp, wire);
        purpose = RADIO_PURPOSE_CHANNEL_SEND;
    }
    return queue_radio_ex(c, cmd, n, QTC_RADIO_MSG_SENT, QTC_RADIO_OK,
                          QTC_RADIO_ERROR, m->message_key, purpose, m->attempt,
                          RADIO_PRIORITY_URGENT);
}

static int retry_or_finish_message(core_ctx *c, const char *message_key,
                                   bool acknowledgement_timeout) {
    qtc_message *m = find_message(c, message_key);
    if (m == NULL) return -1;
    if (m->status == QTC_MSG_DELIVERED) return 0;
    int max_attempts = c->state.settings.max_direct_attempts;
    if (max_attempts < 1) max_attempts = 1;
    bool may_retry = m->conversation_kind == QTC_CONV_CONTACT &&
                     m->attempt + 1 < max_attempts &&
                     (!acknowledgement_timeout || c->state.settings.retry_unconfirmed);
    if (!may_retry) {
        qtc_message_status final_status = acknowledgement_timeout ? QTC_MSG_UNCONFIRMED : QTC_MSG_FAILED;
        return update_message_state(c, message_key, final_status, m->attempt, 0, 0, true);
    }
    int next_attempt = m->attempt + 1;
    if (update_message_state(c, message_key, QTC_MSG_QUEUED, next_attempt, 0, 0, false) != 0) return -1;
    m = find_message(c, message_key);
    if (m == NULL || queue_message_send(c, m) != 0) {
        (void)update_message_state(c, message_key, QTC_MSG_FAILED, next_attempt, 0, 0, true);
        return -1;
    }
    broadcast_delta(c, QTC_IPC_MESSAGE, m, sizeof(*m));
    service_radio_queue(c);
    return 0;
}

static void resume_outgoing_messages(core_ctx *c) {
    char keys[QTC_MAX_MESSAGES][160];
    int attempts[QTC_MAX_MESSAGES];
    size_t count = 0;
    for (size_t i = 0; i < c->state.message_count && count < QTC_MAX_MESSAGES; i++) {
        const qtc_message *m = &c->state.messages[i];
        if (m->direction != QTC_MSG_OUTGOING) continue;
        if (m->status == QTC_MSG_QUEUED || m->status == QTC_MSG_SENDING) {
            qtc_strlcpy(keys[count], m->message_key, sizeof(keys[count]));
            attempts[count] = m->attempt;
            count++;
        }
    }
    for (size_t i = 0; i < count; i++) {
        (void)update_message_state(c, keys[i], QTC_MSG_QUEUED,
                                   attempts[i], 0, 0, false);
        qtc_message *fresh = find_message(c, keys[i]);
        if (fresh != NULL) (void)queue_message_send(c, fresh);
    }
}

static void service_ack_timeouts(core_ctx *c) {
    int64_t now = qtc_now_millis();
    char expired[QTC_MAX_MESSAGES][160];
    size_t count = 0;
    for (size_t i = 0; i < c->state.message_count && count < QTC_MAX_MESSAGES; i++) {
        qtc_message *m = &c->state.messages[i];
        if (m->direction == QTC_MSG_OUTGOING &&
            m->conversation_kind == QTC_CONV_CONTACT &&
            m->status == QTC_MSG_SENT && m->ack_code != 0 &&
            m->ack_deadline > 0 && m->ack_deadline <= now) {
            qtc_strlcpy(expired[count++], m->message_key, sizeof(expired[0]));
        }
    }
    for (size_t i = 0; i < count; i++) (void)retry_or_finish_message(c, expired[i], true);
}

static bool queue_pop_next(core_ctx *c, radio_command *out) {
    if (c->queue_count == 0 || out == NULL) return false;
    size_t best = 0;
    radio_priority best_priority = c->queue[c->queue_head].priority;
    for (size_t i = 1; i < c->queue_count; i++) {
        size_t pos = (c->queue_head + i) % RADIO_QUEUE;
        if (c->queue[pos].priority < best_priority) {
            best = i;
            best_priority = c->queue[pos].priority;
        }
    }
    size_t best_pos = (c->queue_head + best) % RADIO_QUEUE;
    *out = c->queue[best_pos];
    for (size_t i = best; i + 1 < c->queue_count; i++) {
        size_t dst = (c->queue_head + i) % RADIO_QUEUE;
        size_t src = (c->queue_head + i + 1) % RADIO_QUEUE;
        c->queue[dst] = c->queue[src];
    }
    c->queue_count--;
    return true;
}

static int radio_command_timeout_ms(const radio_command *command) {
    if (command == NULL || command->len == 0) return RADIO_DEFAULT_TIMEOUT_MS;
    switch (command->data[0]) {
        case 4:  /* CMD_GET_CONTACTS streams a complete contact list. */
            return RADIO_CONTACTS_TIMEOUT_MS;
        case 10: /* CMD_SYNC_NEXT_MESSAGE is a local queue read. */
            return RADIO_INBOX_TIMEOUT_MS;
        default:
            return RADIO_DEFAULT_TIMEOUT_MS;
    }
}

static int radio_command_transport_retries(const radio_command *command) {
    if (command == NULL || command->len == 0) return 0;
    if (command->data[0] == 10) return 0;
    if (command->purpose == RADIO_PURPOSE_STARTUP_APP ||
        command->purpose == RADIO_PURPOSE_STARTUP_DEVICE) return 1;
    /* A single transport retry is enough on a local USB serial link.  The old
     * three-times-six-second policy could hold every message behind one lost
     * response for eighteen seconds.  Message-level retries remain separate. */
    return command->purpose == RADIO_PURPOSE_GENERIC ? 1 : 0;
}

static void service_radio_queue(core_ctx *c) {
    if (!c->state.radio_connected || c->serial.fd < 0) return;
    if (c->radio_pending) {
        if (qtc_now_millis() - c->pending_since >
            radio_command_timeout_ms(&c->pending)) {
            qtc_log(QTC_LOG_WARN, "radio command 0x%02x timed out", c->pending.data[0]);
            if (c->pending.transport_attempts <
                radio_command_transport_retries(&c->pending)) {
                c->pending.transport_attempts++;
                if (qtc_serial_send(&c->serial, c->pending.data, c->pending.len) == 0) {
                    c->pending_since = qtc_now_millis();
                    return;
                }
            }
            char message_key[160];
            radio_purpose purpose = c->pending.purpose;
            qtc_strlcpy(message_key, c->pending.message_key, sizeof(message_key));
            c->radio_pending = false;
            if (purpose == RADIO_PURPOSE_STARTUP_APP ||
                purpose == RADIO_PURPOSE_STARTUP_DEVICE) {
                disconnect_radio(c, "MeshCore startup handshake timed out; reconnecting");
                return;
            }
            if (message_key[0]) {
                if (purpose == RADIO_PURPOSE_DIRECT_SEND)
                    (void)retry_or_finish_message(c, message_key, false);
                else
                    (void)update_message_state(c, message_key, QTC_MSG_UNCONFIRMED,
                                               c->pending.message_attempt, 0, 0, true);
            } else if (advert_purpose(purpose)) {
                advert_status(c, purpose, "timed out");
            }
        } else {
            return;
        }
    }
    if (c->queue_count == 0) return;
    do {
        if (!queue_pop_next(c, &c->pending)) return;
        if (c->pending.message_key[0]) {
            qtc_message *current = find_message(c, c->pending.message_key);
            if (current != NULL && current->status == QTC_MSG_DELIVERED) {
                if (c->queue_count == 0) return;
                continue;
            }
        }
        break;
    } while (c->queue_count > 0);
    if (qtc_serial_send(&c->serial, c->pending.data, c->pending.len) != 0) {
        qtc_log(QTC_LOG_WARN, "serial write failed: %s", strerror(errno));
        char message_key[160];
        radio_purpose purpose = c->pending.purpose;
        qtc_strlcpy(message_key, c->pending.message_key, sizeof(message_key));
        c->radio_pending = false;
        if (purpose == RADIO_PURPOSE_STARTUP_APP ||
            purpose == RADIO_PURPOSE_STARTUP_DEVICE) {
            disconnect_radio(c, "MeshCore startup write failed; reconnecting");
            return;
        }
        if (message_key[0]) {
            if (purpose == RADIO_PURPOSE_DIRECT_SEND)
                (void)retry_or_finish_message(c, message_key, false);
            else
                (void)update_message_state(c, message_key, QTC_MSG_UNCONFIRMED,
                                           c->pending.message_attempt, 0, 0, true);
        } else if (advert_purpose(purpose)) {
            advert_status(c, purpose, "failed");
        }
        return;
    }
    c->radio_pending = true;
    c->pending_since = qtc_now_millis();
    qtc_log(QTC_LOG_DEBUG, "radio TX cmd=0x%02x purpose=%d queue=%zu",
            c->pending.data[0], (int)c->pending.purpose, c->queue_count);
    if (c->pending.message_key[0]) {
        (void)update_message_state(c, c->pending.message_key, QTC_MSG_SENDING,
                                   c->pending.message_attempt, 0, 0, true);
    }
}

static bool event_matches(const radio_command *cmd, qtc_radio_event_type type) {
    return type == cmd->expected_a || type == cmd->expected_b || type == cmd->expected_c;
}

static void resolve_message_contact(core_ctx *c, qtc_message *m) {
    if (m->conversation_kind != QTC_CONV_CONTACT) return;
    qtc_contact *ct = find_contact(c, m->conversation_key);
    if (ct != NULL) qtc_strlcpy(m->conversation_key, ct->id, sizeof(m->conversation_key));
}

static void normalize_long_message(qtc_message *m) {
    uint32_t token = 0;
    int part_index = 0, part_total = 0;
    char chunk[QTC_MAX_TEXT];
    if (qtc_long_wire_parse(m->text, &token, &part_index, &part_total,
                            chunk, sizeof(chunk)) != 0) {
        qtc_strlcpy(m->logical_key, m->message_key, sizeof(m->logical_key));
        m->part_index = 1;
        m->part_total = 1;
        return;
    }
    snprintf(m->logical_key, sizeof(m->logical_key), "in-long:%d:%s:%08x",
             m->conversation_kind, m->conversation_key, token);
    m->part_index = part_index;
    m->part_total = part_total;
    qtc_strlcpy(m->text, chunk, sizeof(m->text));
}

static bool conversation_open(const core_ctx *c, qtc_conversation_kind kind,
                              const char *key) {
    for (int i = 0; i < QTC_MAX_CLIENTS; i++) {
        const core_client *client = &c->clients[i];
        if (client->active && client->conversation_active &&
            client->conversation_kind == kind &&
            strcmp(client->conversation_key, key) == 0) return true;
    }
    return false;
}

static void message_title(core_ctx *c, const qtc_message *m, char *title, size_t title_len) {
    if (m->conversation_kind == QTC_CONV_CONTACT) {
        qtc_contact *ct = find_contact(c, m->conversation_key);
        snprintf(title, title_len, "%s", ct != NULL ? (ct->alias[0] ? ct->alias : ct->name) : "QTC direct message");
    } else {
        qtc_channel *ch = find_channel(c, atoi(m->conversation_key));
        snprintf(title, title_len, "# %s", ch != NULL ? ch->name : "QTC channel");
    }
}

static void broadcast_banner(core_ctx *c, const qtc_message *m,
                             const char *title, const char *body) {
    if (!c->state.settings.banner_enabled) return;
    qtc_ipc_banner_payload banner = {0};
    banner.kind = m->conversation_kind;
    qtc_strlcpy(banner.key, m->conversation_key, sizeof(banner.key));
    qtc_strlcpy(banner.title, title, sizeof(banner.title));
    qtc_strlcpy(banner.body, body, sizeof(banner.body));
    banner.created_at = qtc_now_seconds();
    for (int i = 0; i < QTC_MAX_CLIENTS; i++) {
        if (c->clients[i].active &&
            qtc_ipc_send_nonblocking(c->clients[i].fd, QTC_IPC_BANNER,
                                     &banner, sizeof(banner)) != 0)
            close_client(c, i);
    }
}

static void notify_incoming_message(core_ctx *c, const qtc_message *m) {
    char body[QTC_MAX_TEXT];
    const char *visible = m->text;
    if (m->part_total > 1) {
        int total = 0, count = 0;
        qtc_message_status status;
        int assembled = qtc_message_assemble(c->state.messages, c->state.message_count,
                                             m->logical_key, body, sizeof(body),
                                             &total, &count, &status);
        (void)status;
        if (assembled != 0 || count != total) return;
        visible = body;
    }
    char title[QTC_MAX_NAME];
    message_title(c, m, title, sizeof(title));
    bool suppress = c->state.settings.suppress_open_conversation &&
                    conversation_open(c, m->conversation_kind, m->conversation_key);
    if (!suppress)
        (void)qtc_notify_message(&c->state.settings,
                                 m->conversation_kind == QTC_CONV_CHANNEL,
                                 title, visible);
    broadcast_banner(c, m, title, visible);
}

static void handle_incoming_invite(core_ctx *c, qtc_message *m) {
    char uri[QTC_MAX_URI], name[33]; uint8_t secret[16];
    if (qtc_invite_message_parse(m->text, uri, sizeof(uri), name, sizeof(name), secret) != 0) return;
    qtc_invitation inv = {0}; qtc_strlcpy(inv.sender_contact_id, m->conversation_key, sizeof(inv.sender_contact_id));
    qtc_strlcpy(inv.channel_name, name, sizeof(inv.channel_name)); qtc_strlcpy(inv.uri, uri, sizeof(inv.uri));
    inv.source_message_id = m->id; inv.status = QTC_INVITE_PENDING; inv.received_at = qtc_now_seconds();
    if (qtc_db_insert_invitation(&c->db, &inv) == 0 && inv.id != 0)
        state_upsert_invitation(c, &inv);
}

static void radio_event(core_ctx *c, const qtc_radio_event *e) {
    bool matched = c->radio_pending && event_matches(&c->pending, e->type);
    radio_command completed = {0};
    if (matched) {
        completed = c->pending;
        c->radio_pending = false;
    }
    qtc_log(QTC_LOG_DEBUG, "radio RX event=%d matched=%d pending=%d phase=%d",
            (int)e->type, matched ? 1 : 0, c->radio_pending ? 1 : 0,
            (int)c->session_phase);

    switch (e->type) {
        case QTC_RADIO_OK:
            if (matched && completed.purpose == RADIO_PURPOSE_CHANNEL_SEND &&
                completed.message_key[0]) {
                (void)update_message_state(c, completed.message_key, QTC_MSG_SENT,
                                           completed.message_attempt, 0, 0, true);
            } else if (matched && advert_purpose(completed.purpose)) {
                advert_status(c, completed.purpose, "sent");
            }
            break;
        case QTC_RADIO_SELF_INFO:
            c->state.radio_tx_power = e->tx_power;
            c->state.radio_max_tx_power = e->max_tx_power;
            c->state.radio_freq = e->freq;
            c->state.radio_bw = e->bw;
            c->state.radio_sf = e->sf;
            c->state.radio_cr = e->cr;
            qtc_strlcpy(c->state.radio_name, e->radio_name,
                        sizeof(c->state.radio_name));
            c->state.revisions.connection++;
            broadcast_status(c);
            if (matched && completed.purpose == RADIO_PURPOSE_STARTUP_APP) {
                qtc_log(QTC_LOG_DEBUG, "MeshCore APP_START completed; requesting device info");
                queue_device_query(c);
            }
            break;
        case QTC_RADIO_DEVICE_INFO:
            if (e->max_channels > 0 && e->max_channels <= QTC_MAX_CHANNELS)
                c->state.radio_max_channels = e->max_channels;
            if (e->max_contacts > 0) c->state.radio_max_contacts = e->max_contacts;
            qtc_strlcpy(c->state.radio_model, e->model,
                        sizeof(c->state.radio_model));
            qtc_strlcpy(c->state.radio_version, e->version,
                        sizeof(c->state.radio_version));
            c->state.revisions.connection++;
            broadcast_status(c);
            if (matched && completed.purpose == RADIO_PURPOSE_STARTUP_DEVICE) {
                qtc_log(QTC_LOG_DEBUG, "MeshCore DEVICE_QUERY completed; session ready");
                radio_session_ready(c);
            }
            break;
        case QTC_RADIO_CONTACT_END:
            if (c->contact_since > 0) {
                char value[32];
                snprintf(value, sizeof(value), "%u", c->contact_since);
                (void)qtc_db_save_setting(&c->db, "contact_sync_since", value);
            }
            break;
        case QTC_RADIO_CONTACT:
        case QTC_RADIO_NEW_ADVERT: {
            if (e->value > c->contact_since) c->contact_since = e->value;
            qtc_contact *old = find_contact(c, e->contact.id);
            bool changed = old == NULL || strcmp(old->name, e->contact.name) != 0 ||
                           old->route_hops != e->contact.route_hops ||
                           old->route_known != e->contact.route_known ||
                           old->last_heard != e->contact.last_heard ||
                           old->node_type != e->contact.node_type;
            if (qtc_db_upsert_contact(&c->db, &e->contact) == 0 && changed)
                state_upsert_contact(c, &e->contact, true);
            break;
        }
        case QTC_RADIO_CONTACTS_DIRTY:
            c->contacts_sync_needed = true;
            c->background_sync_after = qtc_now_millis() + BACKGROUND_SYNC_DELAY_MS;
            qtc_log(QTC_LOG_DEBUG, "radio contact/path update deferred to idle sync");
            break;
        case QTC_RADIO_CHANNEL_INFO: {
            qtc_channel *old = find_channel(c, e->channel.index);
            bool changed = old == NULL || old->configured != e->channel.configured ||
                           strcmp(old->name, e->channel.name) != 0 ||
                           memcmp(old->secret, e->channel.secret, 16) != 0;
            if (e->channel.configured)
                (void)qtc_db_upsert_channel(&c->db, &e->channel);
            else
                (void)qtc_db_remove_channel(&c->db, e->channel.index);
            if (changed && e->channel.configured)
                state_upsert_channel(c, &e->channel, true);
            break;
        }
        case QTC_RADIO_CONTACT_MESSAGE:
        case QTC_RADIO_CHANNEL_MESSAGE: {
            qtc_log(QTC_LOG_DEBUG, "radio message RX kind=%d sender_ts=%lld",
                    (int)e->message.conversation_kind,
                    (long long)e->message.sender_timestamp);
            if (!inbox_needs_drain(c)) request_inbox_drain(c, true);
            qtc_message message = e->message;
            resolve_message_contact(c, &message);
            normalize_long_message(&message);
            assign_unique_incoming_key(c, &message);
            bool first_logical_part = true;
            for (size_t i = 0; i < c->state.message_count; i++) {
                const qtc_message *old = &c->state.messages[i];
                if (old->direction == QTC_MSG_INCOMING &&
                    strcmp(old->logical_key, message.logical_key) == 0) {
                    first_logical_part = false;
                    break;
                }
            }
            bool inserted = false;
            if (qtc_db_insert_message(&c->db, &message, &inserted) == 0 && inserted) {
                qtc_message *stored = state_upsert_message(c, &message, true);
                if (first_logical_part) {
                    if (message.conversation_kind == QTC_CONV_CONTACT) {
                        qtc_contact *contact = find_contact(c, message.conversation_key);
                        if (contact != NULL) {
                            contact->unread++;
                            c->state.revisions.contacts++;
                            broadcast_delta(c, QTC_IPC_CONTACT, contact, sizeof(*contact));
                        }
                    } else {
                        qtc_channel *channel = find_channel(c, atoi(message.conversation_key));
                        if (channel != NULL) {
                            channel->unread++;
                            c->state.revisions.channels++;
                            broadcast_delta(c, QTC_IPC_CHANNEL, channel, sizeof(*channel));
                        }
                    }
                }
                handle_incoming_invite(c, stored);
                notify_incoming_message(c, stored);
            }
            queue_next_inbox_message(c, RADIO_PRIORITY_URGENT);
            break;
        }
        case QTC_RADIO_MESSAGES_WAITING:
            /* A waiting push can race with the response to an earlier empty
             * sync. Give every push its own generation so an older
             * NO_MORE_MESSAGES response can never cancel newer work. */
            request_inbox_drain(c, true);
            qtc_log(QTC_LOG_DEBUG, "radio PUSH_MSG_WAITING generation=%llu",
                    (unsigned long long)c->inbox_generation);
            queue_next_inbox_message(c, RADIO_PRIORITY_URGENT);
            break;
        case QTC_RADIO_NO_MORE_MESSAGES:
            if (matched && completed.len > 0 && completed.data[0] == 10 &&
                completed.inbox_generation > c->inbox_empty_generation)
                c->inbox_empty_generation = completed.inbox_generation;
            queue_next_inbox_message(c, RADIO_PRIORITY_URGENT);
            break;
        case QTC_RADIO_EXPORT_CONTACT: {
            int client = c->clipboard_client;
            c->clipboard_client = -1;
            if (client >= 0 && client < QTC_MAX_CLIENTS && c->clients[client].active) {
                qtc_ipc_clipboard_payload payload = {0};
                size_t max_card = (sizeof(payload.text) - strlen("meshcore://") - 1U) / 2U;
                if (e->card_data_len == 0 || e->card_data_len > max_card) {
                    const char *msg = "Radio returned an invalid contact card";
                    (void)qtc_ipc_send_nonblocking(c->clients[client].fd, QTC_IPC_ERROR,
                                                   msg, (uint32_t)(strlen(msg) + 1));
                } else {
                    qtc_strlcpy(payload.text, "meshcore://", sizeof(payload.text));
                    qtc_hex_encode(e->card_data, e->card_data_len,
                                   payload.text + strlen(payload.text),
                                   sizeof(payload.text) - strlen(payload.text));
                    (void)qtc_ipc_send_nonblocking(c->clients[client].fd,
                                                   QTC_IPC_CLIPBOARD_TEXT,
                                                   &payload, sizeof(payload));
                    set_status(c, "MeshCore contact card exported");
                    broadcast_status(c);
                }
            }
            break;
        }
        case QTC_RADIO_MSG_SENT:
            if (matched && completed.message_key[0]) {
                qtc_message *current = find_message(c, completed.message_key);
                if (current != NULL && current->status == QTC_MSG_DELIVERED) break;
                if (completed.purpose == RADIO_PURPOSE_DIRECT_SEND) {
                    uint32_t ack = e->expected_ack_len == 4 ?
                                   ack_code_from_bytes(e->expected_ack) : 0;
                    qtc_log(QTC_LOG_DEBUG,
                            "radio SEND accepted key=%.40s ack=%08x timeout=%ums",
                            completed.message_key, ack, e->suggested_timeout_ms);
                    int64_t timeout = e->suggested_timeout_ms > 0 ?
                                      e->suggested_timeout_ms : 6000;
                    qtc_message_status status = ack != 0 ?
                                                QTC_MSG_SENT : QTC_MSG_UNCONFIRMED;
                    if (ack != 0)
                        (void)qtc_db_record_message_ack(&c->db,
                                                        completed.message_key,
                                                        ack,
                                                        completed.message_attempt);
                    (void)update_message_state(c, completed.message_key, status,
                                               completed.message_attempt, ack,
                                               ack != 0 ? qtc_now_millis() + timeout : 0,
                                               true);
                } else {
                    (void)update_message_state(c, completed.message_key, QTC_MSG_SENT,
                                               completed.message_attempt, 0, 0, true);
                }
            }
            break;
        case QTC_RADIO_ACK: {
            uint32_t ack = e->expected_ack_len == 4 ?
                           ack_code_from_bytes(e->expected_ack) : 0;
            qtc_log(QTC_LOG_DEBUG, "radio ACK ack=%08x rtt=%ums", ack,
                    e->round_trip_ms);
            char keys[16][160];
            size_t key_count = 0;
            int changed = 0;
            if (ack != 0 &&
                qtc_db_get_ack_message_keys(&c->db, ack, keys,
                                            QTC_ARRAY_LEN(keys), &key_count) == 0 &&
                qtc_db_mark_ack_delivered(&c->db, ack, &changed) == 0 && changed > 0) {
                for (size_t i = 0; i < key_count; i++) {
                    qtc_message *message = find_message(c, keys[i]);
                    if (message == NULL || message->status == QTC_MSG_DELIVERED) continue;
                    message->status = QTC_MSG_DELIVERED;
                    message->ack_code = 0;
                    message->ack_deadline = 0;
                    c->state.revisions.messages++;
                    broadcast_delta(c, QTC_IPC_MESSAGE, message, sizeof(*message));
                }
            }
            break;
        }
        case QTC_RADIO_ERROR:
            if (matched && (completed.purpose == RADIO_PURPOSE_STARTUP_APP ||
                            completed.purpose == RADIO_PURPOSE_STARTUP_DEVICE)) {
                disconnect_radio(c, "MeshCore rejected startup handshake; reconnecting");
                return;
            } else if (matched && completed.message_key[0]) {
                if (completed.purpose == RADIO_PURPOSE_DIRECT_SEND)
                    (void)retry_or_finish_message(c, completed.message_key, false);
                else
                    (void)update_message_state(c, completed.message_key,
                                               QTC_MSG_UNCONFIRMED,
                                               completed.message_attempt, 0, 0, true);
            } else if (matched && advert_purpose(completed.purpose)) {
                advert_status(c, completed.purpose, "failed");
            } else {
                set_status(c, "Radio rejected the last command");
                broadcast_status(c);
            }
            break;
        default:
            break;
    }

    /* The companion protocol is request/response, but the next request can be
     * issued immediately after the matching frame. Do not wait for the outer
     * event-loop timer tick, especially while draining received messages. */
    service_radio_queue(c);
}

static void radio_frame_cb(const uint8_t *frame, size_t len, void *userdata) {
    core_ctx *c = userdata; qtc_radio_event e;
    if (qtc_protocol_parse(frame, len, &e) == 0) radio_event(c, &e);
    else qtc_log(QTC_LOG_WARN, "ignored malformed radio frame (%zu bytes)", len);
}

static int connect_radio(core_ctx *c) {
    if (c->demo) return 0;
    char device[QTC_MAX_PATH];
    if (c->requested_device[0]) qtc_strlcpy(device, c->requested_device, sizeof(device));
    else if (c->state.settings.serial_device[0]) qtc_strlcpy(device, c->state.settings.serial_device, sizeof(device));
    else if (qtc_serial_autodetect(device, sizeof(device)) != 0) {
        set_status(c, "No MeshCore serial device found; background core is waiting"); return -1;
    }
    if (qtc_serial_open(&c->serial, device, 115200) != 0) {
        char msg[160]; snprintf(msg, sizeof(msg), "Cannot open %.96s: %.48s", device, strerror(errno)); set_status(c, msg); return -1;
    }
    c->state.radio_connected = true; c->state.revisions.connection++;
    qtc_strlcpy(c->state.settings.serial_device, device, sizeof(c->state.settings.serial_device));
    (void)qtc_db_save_setting(&c->db, "serial_device", device);
    c->queue_head = c->queue_count = 0;
    c->radio_pending = false;
    c->session_phase = RADIO_SESSION_DOWN;
    c->contacts_sync_needed = false;
    c->next_channel_sync = 0;
    queue_startup(c);
    char msg[160];
    snprintf(msg, sizeof(msg), "Serial connected to %.104s; starting MeshCore session", device);
    set_status(c, msg);
    broadcast_status(c);
    /* Do not burn the first event-loop timeout before APP_START. */
    service_radio_queue(c);
    return 0;
}

static void disconnect_radio(core_ctx *c, const char *reason) {
    if (c->serial.fd >= 0) qtc_serial_close(&c->serial);
    c->state.radio_connected = false; c->state.revisions.connection++; c->radio_pending = false;
    c->session_phase = RADIO_SESSION_DOWN;
    c->queue_head = c->queue_count = 0;
    c->inbox_empty_generation = c->inbox_generation;
    c->contacts_sync_needed = false;
    c->next_channel_sync = 0;
    c->clipboard_client = -1; set_status(c, reason); c->reconnect_at = qtc_now_millis() + 3000;
    broadcast_status(c);
}

static int next_outgoing_timestamp(core_ctx *c, qtc_conversation_kind kind, const char *key) {
    int64_t ts = qtc_now_seconds();
    for (size_t i = 0; i < c->state.message_count; i++) {
        const qtc_message *m = &c->state.messages[i];
        if (m->direction == QTC_MSG_OUTGOING && m->conversation_kind == kind &&
            strcmp(m->conversation_key, key) == 0 && m->sender_timestamp >= ts) ts = m->sender_timestamp + 1;
    }
    return (int)ts;
}

static int send_message_action(core_ctx *c, qtc_conversation_kind kind,
                               const char *conversation_key, const char *text) {
    if (conversation_key == NULL || *conversation_key == 0 || text == NULL || *text == 0)
        return -1;
    size_t text_len = strlen(text);
    if (text_len >= QTC_MAX_TEXT) return -1;

    if (kind == QTC_CONV_CONTACT) {
        qtc_contact *ct = find_contact(c, conversation_key);
        if (ct == NULL || ct->node_type != QTC_NODE_PERSON) return -1;
        conversation_key = ct->id;
    } else if (kind == QTC_CONV_CHANNEL) {
        qtc_channel *ch = find_channel(c, atoi(conversation_key));
        if (ch == NULL || !ch->configured) return -1;
    } else {
        return -1;
    }

    const size_t single_limit = kind == QTC_CONV_CONTACT ?
                                QTC_DIRECT_RADIO_TEXT_MAX : QTC_CHANNEL_RADIO_TEXT_MAX;
    const size_t long_chunk = kind == QTC_CONV_CONTACT ?
                              QTC_LONG_CHUNK_MAX : 90U;
    int total = text_len <= single_limit ? 1 :
                (int)((text_len + long_chunk - 1U) / long_chunk);
    if (total > 99) return -1;

    int64_t base_timestamp = next_outgoing_timestamp(c, kind, conversation_key);
    uint32_t token = qtc_message_token(kind, conversation_key, base_timestamp, text);
    char logical_key[160];
    if (total == 1) {
        snprintf(logical_key, sizeof(logical_key), "out:%s:%s:%lld",
                 kind == QTC_CONV_CONTACT ? "dm" : "ch", conversation_key,
                 (long long)base_timestamp);
    } else {
        snprintf(logical_key, sizeof(logical_key), "out-long:%d:%.96s:%08x",
                 kind, conversation_key, token);
    }

    qtc_message *parts = calloc((size_t)total, sizeof(*parts));
    if (parts == NULL) return -1;
    int inserted_parts = 0;
    const char *cursor = text;
    if (sqlite3_exec(c->db.db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        goto fail;
    for (int part = 1; part <= total; part++) {
        size_t chunk_len = total == 1 ? strlen(cursor) :
                           qtc_utf8_chunk_length(cursor, long_chunk);
        if (chunk_len == 0 || chunk_len >= QTC_MAX_TEXT) goto rollback;
        qtc_message message = {0};
        message.conversation_kind = kind;
        qtc_strlcpy(message.conversation_key, conversation_key,
                    sizeof(message.conversation_key));
        message.direction = QTC_MSG_OUTGOING;
        message.sender_timestamp = base_timestamp + part - 1;
        message.attempt = 0;
        message.status = c->demo ? QTC_MSG_DELIVERED : QTC_MSG_QUEUED;
        message.part_index = part;
        message.part_total = total;
        message.created_at = qtc_now_seconds();
        qtc_strlcpy(message.logical_key, logical_key, sizeof(message.logical_key));
        if (total == 1) {
            qtc_strlcpy(message.message_key, logical_key, sizeof(message.message_key));
        } else {
            snprintf(message.message_key, sizeof(message.message_key),
                     "%.140s:p%02d", logical_key, part);
        }
        memcpy(message.text, cursor, chunk_len);
        message.text[chunk_len] = 0;
        bool inserted = false;
        if (qtc_db_insert_message(&c->db, &message, &inserted) != 0 || !inserted)
            goto rollback;
        parts[inserted_parts++] = message;
        cursor += chunk_len;
    }
    if (sqlite3_exec(c->db.db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK)
        goto rollback_after_commit;

    for (int i = 0; i < inserted_parts; i++)
        (void)state_upsert_message(c, &parts[i], false);

    if (!c->demo) {
        for (int i = 0; i < inserted_parts; i++) {
            qtc_message *message = find_message(c, parts[i].message_key);
            if (message == NULL || queue_message_send(c, message) != 0)
                (void)update_message_state(c, parts[i].message_key,
                                           QTC_MSG_FAILED, 0, 0, 0, false);
        }
    }

    /* Write an idle radio immediately before publishing the new message.
     * This prevents the UI from flashing "queued" when the command has
     * already reached USB serial. If another radio request is genuinely in
     * flight, queued remains the honest state until that response arrives. */
    service_radio_queue(c);
    for (int i = 0; i < inserted_parts; i++) {
        qtc_message *message = find_message(c, parts[i].message_key);
        if (message != NULL)
            broadcast_delta(c, QTC_IPC_MESSAGE, message, sizeof(*message));
    }
    free(parts);
    return 0;

rollback:
    (void)sqlite3_exec(c->db.db, "ROLLBACK", NULL, NULL, NULL);
    free(parts);
    return -1;
rollback_after_commit:
    (void)sqlite3_exec(c->db.db, "ROLLBACK", NULL, NULL, NULL);
fail:
    free(parts);
    return -1;
}

static int send_direct_action(core_ctx *c, const char *contact_id, const char *text) {
    return send_message_action(c, QTC_CONV_CONTACT, contact_id, text);
}

static int send_channel_action(core_ctx *c, int index, const char *text) {
    char key[16];
    snprintf(key, sizeof(key), "%d", index);
    return send_message_action(c, QTC_CONV_CHANNEL, key, text);
}

static int apply_channel(core_ctx *c, int index, const char *name, const uint8_t secret[16]) {
    qtc_channel ch = {0}; ch.index = index; ch.configured = name != NULL && *name != 0; ch.is_private = ch.configured;
    qtc_strlcpy(ch.name, name, sizeof(ch.name)); memcpy(ch.secret, secret, 16);
    if (ch.configured) { if (qtc_db_upsert_channel(&c->db, &ch) != 0) return -1; }
    else if (qtc_db_remove_channel(&c->db, index) != 0) return -1;
    if (!c->demo) {
        uint8_t cmd[64]; size_t n = qtc_cmd_set_channel(cmd, sizeof(cmd), index, ch.name, ch.secret);
        if (queue_radio(c, cmd, n, QTC_RADIO_OK, QTC_RADIO_ERROR, QTC_RADIO_NONE, "") != 0) return -1;
        n = qtc_cmd_get_channel(cmd, sizeof(cmd), index);
        (void)queue_radio(c, cmd, n, QTC_RADIO_CHANNEL_INFO, QTC_RADIO_ERROR, QTC_RADIO_NONE, "");
    }
    (void)reload_state(c, 0, 1, 0, 0); broadcast_snapshot(c); return 0;
}

static void reply_error(core_ctx *c, const char *message) {
    int idx = c->current_client;
    if (idx >= 0 && idx < QTC_MAX_CLIENTS && c->clients[idx].active)
        if (qtc_ipc_send_nonblocking(c->clients[idx].fd, QTC_IPC_ERROR,
                                     message, (uint32_t)(strlen(message) + 1)) != 0)
            close_client(c, idx);
}

static void handle_client_frame(const qtc_ipc_frame *f, void *userdata) {
    core_ctx *c = userdata;
    int client_index = c->current_client;
    if (f->type != QTC_IPC_HELLO && f->type != QTC_IPC_SHUTDOWN &&
        f->type != QTC_IPC_PING &&
        (client_index < 0 || client_index >= QTC_MAX_CLIENTS ||
         !c->clients[client_index].hello_ok)) {
        reply_error(c, "QTC IPC handshake required");
        if (client_index >= 0 && client_index < QTC_MAX_CLIENTS)
            close_client(c, client_index);
        return;
    }
    switch (f->type) {
        case QTC_IPC_HELLO: {
            int idx = c->current_client;
            if (idx < 0 || idx >= QTC_MAX_CLIENTS || !c->clients[idx].active) break;
            if (f->length != sizeof(qtc_ipc_hello)) {
                reply_error(c, "Incompatible QTC background core/client; restart QTC");
                close_client(c, idx);
                break;
            }
            const qtc_ipc_hello *hello = (const void *)f->payload;
            if (hello->protocol_version != QTC_IPC_PROTOCOL_VERSION ||
                strcmp(hello->app_version, QTC_VERSION) != 0) {
                reply_error(c, "QTC core/client version mismatch; restart the background core");
                close_client(c, idx);
                break;
            }
            c->clients[idx].hello_ok = true;
            if (send_core_info(c->clients[idx].fd) != 0 ||
                send_snapshot(c, c->clients[idx].fd) != 0)
                close_client(c, idx);
            break;
        }
        case QTC_IPC_SYNC:
            if (c->clients[c->current_client].hello_ok &&
                send_snapshot(c, c->clients[c->current_client].fd) != 0)
                close_client(c, c->current_client);
            break;
        case QTC_IPC_PING:
            (void)send_core_info(c->clients[c->current_client].fd);
            (void)send_status_frame(c, c->clients[c->current_client].fd);
            break;
        case QTC_IPC_SETTINGS:
            if (f->length == sizeof(qtc_settings)) {
                qtc_settings updated = *(const qtc_settings *)f->payload;
                if (updated.stored_poll_seconds < 1) updated.stored_poll_seconds = 1;
                if (updated.stored_poll_seconds > 3600) updated.stored_poll_seconds = 3600;
                if (updated.max_direct_attempts < 1) updated.max_direct_attempts = 1;
                if (updated.max_direct_attempts > 4) updated.max_direct_attempts = 4;
                if (updated.theme < 0 || updated.theme > 3) updated.theme = 0;
                const qtc_settings *s = &updated;
                char value[32];
                (void)qtc_db_save_setting(&c->db, "desktop_notifications", s->desktop_notifications ? "1" : "0");
                (void)qtc_db_save_setting(&c->db, "notify_direct", s->notify_direct ? "1" : "0");
                (void)qtc_db_save_setting(&c->db, "notify_channel", s->notify_channel ? "1" : "0");
                (void)qtc_db_save_setting(&c->db, "suppress_open_conversation", s->suppress_open_conversation ? "1" : "0");
                (void)qtc_db_save_setting(&c->db, "sound_enabled", s->sound_enabled ? "1" : "0");
                (void)qtc_db_save_setting(&c->db, "banner_enabled", s->banner_enabled ? "1" : "0");
                (void)qtc_db_save_setting(&c->db, "show_signal", s->show_signal ? "1" : "0");
                snprintf(value, sizeof(value), "%d", s->stored_poll_seconds); (void)qtc_db_save_setting(&c->db, "stored_poll_seconds", value);
                (void)qtc_db_save_setting(&c->db, "retry_unconfirmed", s->retry_unconfirmed ? "1" : "0");
                snprintf(value, sizeof(value), "%d", s->max_direct_attempts); (void)qtc_db_save_setting(&c->db, "max_direct_attempts", value);
                (void)qtc_db_save_setting(&c->db, "reset_stale_route", s->reset_stale_route ? "1" : "0");
                snprintf(value, sizeof(value), "%d", s->theme); (void)qtc_db_save_setting(&c->db, "theme", value);
                snprintf(value, sizeof(value), "%d", s->tx_power); (void)qtc_db_save_setting(&c->db, "tx_power", value);
                if (s->serial_device[0]) (void)qtc_db_save_setting(&c->db, "serial_device", s->serial_device);
                c->state.settings = updated;
                c->state.revisions.settings++;
                broadcast_delta(c, QTC_IPC_SETTINGS, &c->state.settings,
                                sizeof(c->state.settings));
            }
            break;
        case QTC_IPC_SEND_DIRECT:
            if (f->length != sizeof(qtc_ipc_send_direct_payload) ||
                send_direct_action(c, ((const qtc_ipc_send_direct_payload *)f->payload)->contact_id,
                                   ((const qtc_ipc_send_direct_payload *)f->payload)->text) != 0)
                reply_error(c, "Could not send direct message");
            break;
        case QTC_IPC_SEND_CHANNEL:
            if (f->length != sizeof(qtc_ipc_send_channel_payload) ||
                send_channel_action(c, ((const qtc_ipc_send_channel_payload *)f->payload)->channel_index,
                                    ((const qtc_ipc_send_channel_payload *)f->payload)->text) != 0)
                reply_error(c, "Could not send channel message");
            break;
        case QTC_IPC_MARK_READ:
            if (f->length == sizeof(qtc_ipc_mark_read_payload)) {
                const qtc_ipc_mark_read_payload *p = (const void *)f->payload;
                if (p->kind == QTC_CONV_CONTACT) {
                    if (qtc_db_mark_contact_read(&c->db, p->key) == 0) {
                        qtc_contact *contact = find_contact(c, p->key);
                        if (contact != NULL && contact->unread != 0) {
                            contact->unread = 0;
                            c->state.revisions.contacts++;
                            broadcast_delta(c, QTC_IPC_CONTACT, contact, sizeof(*contact));
                        }
                    }
                } else {
                    int channel_index = atoi(p->key);
                    if (qtc_db_mark_channel_read(&c->db, channel_index) == 0) {
                        qtc_channel *channel = find_channel(c, channel_index);
                        if (channel != NULL && channel->unread != 0) {
                            channel->unread = 0;
                            c->state.revisions.channels++;
                            broadcast_delta(c, QTC_IPC_CHANNEL, channel, sizeof(*channel));
                        }
                    }
                }
            }
            break;
        case QTC_IPC_SET_FAVORITE:
            if (f->length == sizeof(qtc_ipc_favorite_payload)) {
                const qtc_ipc_favorite_payload *p = (const void *)f->payload;
                if (qtc_db_set_contact_favorite(&c->db, p->contact_id, p->favorite, p->group) == 0) {
                    qtc_contact *contact = find_contact(c, p->contact_id);
                    if (contact != NULL) {
                        contact->favorite = p->favorite;
                        qtc_strlcpy(contact->favorite_group,
                                    p->favorite ? p->group : "",
                                    sizeof(contact->favorite_group));
                        c->state.revisions.contacts++;
                        broadcast_delta(c, QTC_IPC_CONTACT, contact, sizeof(*contact));
                    }
                }
            }
            break;
        case QTC_IPC_SET_ALIAS:
            if (f->length == sizeof(qtc_ipc_contact_text_payload)) {
                const qtc_ipc_contact_text_payload *p = (const void *)f->payload;
                if (find_contact(c, p->contact_id) == NULL ||
                    qtc_db_set_contact_alias(&c->db, p->contact_id, p->value) != 0) {
                    reply_error(c, "Could not update contact alias");
                } else {
                    qtc_contact *contact = find_contact(c, p->contact_id);
                    if (contact != NULL) {
                        qtc_strlcpy(contact->alias, p->value, sizeof(contact->alias));
                        c->state.revisions.contacts++;
                        broadcast_delta(c, QTC_IPC_CONTACT, contact, sizeof(*contact));
                    }
                }
            }
            break;
        case QTC_IPC_ACTIVE_CONVERSATION:
            if (f->length == sizeof(qtc_ipc_active_payload) &&
                c->current_client >= 0 && c->current_client < QTC_MAX_CLIENTS) {
                const qtc_ipc_active_payload *p = (const void *)f->payload;
                core_client *client = &c->clients[c->current_client];
                client->conversation_active = p->active;
                client->conversation_kind = p->kind;
                qtc_strlcpy(client->conversation_key, p->active ? p->key : "",
                            sizeof(client->conversation_key));
            }
            break;
        case QTC_IPC_DEVICE_SET_NAME:
            if (f->length == sizeof(qtc_ipc_device_action_payload)) {
                const qtc_ipc_device_action_payload *p = (const void *)f->payload;
                size_t nlen = strlen(p->text);
                if (nlen == 0 || nlen > 32) {
                    reply_error(c, "Radio name must contain 1 to 32 bytes");
                } else if (c->demo) {
                    qtc_strlcpy(c->state.radio_name, p->text, sizeof(c->state.radio_name));
                    c->state.revisions.connection++;
                    set_status(c, "Demo radio name updated");
                    broadcast_status(c);
                } else if (!c->state.radio_connected) {
                    reply_error(c, "Radio is not connected");
                } else {
                    uint8_t cmd[64];
                    size_t n = qtc_cmd_set_name(cmd, sizeof(cmd), p->text);
                    if (n == 0 || queue_radio(c, cmd, n, QTC_RADIO_OK,
                                              QTC_RADIO_ERROR, QTC_RADIO_NONE, "") != 0) {
                        reply_error(c, "Could not queue radio-name update");
                    } else {
                        n = qtc_cmd_app_start(cmd, sizeof(cmd), "QTC Terminal");
                        (void)queue_radio(c, cmd, n, QTC_RADIO_SELF_INFO,
                                          QTC_RADIO_ERROR, QTC_RADIO_NONE, "");
                        set_status(c, "Radio-name update queued");
                    }
                }
            }
            break;
        case QTC_IPC_DEVICE_SET_TX_POWER:
            if (f->length == sizeof(qtc_ipc_device_action_payload)) {
                const qtc_ipc_device_action_payload *p = (const void *)f->payload;
                int power = p->value;
                if (power < -20 || power > 30 ||
                    (c->state.radio_max_tx_power > 0 && power > c->state.radio_max_tx_power)) {
                    reply_error(c, "Requested TX power is outside the radio range");
                } else if (c->demo) {
                    char value[32];
                    c->state.radio_tx_power = power;
                    c->state.settings.tx_power = power;
                    snprintf(value, sizeof(value), "%d", power);
                    (void)qtc_db_save_setting(&c->db, "tx_power", value);
                    c->state.revisions.connection++;
                    broadcast_status(c);
                } else if (!c->state.radio_connected) {
                    reply_error(c, "Radio is not connected");
                } else {
                    uint8_t cmd[64];
                    size_t n = qtc_cmd_set_tx_power(cmd, sizeof(cmd), power);
                    if (n == 0 || queue_radio(c, cmd, n, QTC_RADIO_OK,
                                              QTC_RADIO_ERROR, QTC_RADIO_NONE, "") != 0) {
                        reply_error(c, "Could not queue TX-power update");
                    } else {
                        char value[32];
                        snprintf(value, sizeof(value), "%d", power);
                        (void)qtc_db_save_setting(&c->db, "tx_power", value);
                        n = qtc_cmd_app_start(cmd, sizeof(cmd), "QTC Terminal");
                        (void)queue_radio(c, cmd, n, QTC_RADIO_SELF_INFO,
                                          QTC_RADIO_ERROR, QTC_RADIO_NONE, "");
                        set_status(c, "TX-power update queued");
                    }
                }
            }
            break;
        case QTC_IPC_DEVICE_ADVERTISE:
            if (f->length == sizeof(qtc_ipc_device_action_payload)) {
                const qtc_ipc_device_action_payload *p = (const void *)f->payload;
                if (c->demo) {
                    radio_purpose purpose = p->flag ? RADIO_PURPOSE_ADVERT_FLOOD :
                                                     RADIO_PURPOSE_ADVERT_ZERO_HOP;
                    advert_status(c, purpose, "sent");
                } else if (!c->state.radio_connected) {
                    reply_error(c, "Radio is not connected");
                } else {
                    uint8_t cmd[4];
                    size_t n = qtc_cmd_advertise(cmd, sizeof(cmd), p->flag);
                    radio_purpose purpose = p->flag ? RADIO_PURPOSE_ADVERT_FLOOD :
                                                     RADIO_PURPOSE_ADVERT_ZERO_HOP;
                    if (n == 0 || queue_radio_ex(c, cmd, n, QTC_RADIO_OK,
                                                 QTC_RADIO_ERROR, QTC_RADIO_NONE, "",
                                                 purpose, 0, RADIO_PRIORITY_URGENT) != 0) {
                        reply_error(c, "Could not send advertisement");
                    } else {
                        set_status(c, p->flag ? "Sending flood advertisement..." :
                                                 "Sending 0-hop advertisement...");
                        broadcast_status(c);
                        service_radio_queue(c);
                    }
                }
            }
            break;
        case QTC_IPC_DEVICE_COPY_CARD:
            if (c->demo) {
                qtc_ipc_clipboard_payload payload = {0};
                qtc_strlcpy(payload.text, "meshcore://01020304aabbccdd", sizeof(payload.text));
                if (qtc_ipc_send_nonblocking(c->clients[c->current_client].fd,
                                             QTC_IPC_CLIPBOARD_TEXT,
                                             &payload, sizeof(payload)) != 0)
                    close_client(c, c->current_client);
            } else if (!c->state.radio_connected) {
                reply_error(c, "Radio is not connected");
            } else {
                uint8_t cmd[2];
                size_t n = qtc_cmd_export_self(cmd, sizeof(cmd));
                c->clipboard_client = c->current_client;
                if (n == 0 || queue_radio_ex(c, cmd, n, QTC_RADIO_EXPORT_CONTACT,
                                             QTC_RADIO_ERROR, QTC_RADIO_NONE, "",
                                             RADIO_PURPOSE_EXPORT_SELF, 0,
                                             RADIO_PRIORITY_URGENT) != 0) {
                    c->clipboard_client = -1;
                    reply_error(c, "Could not export the MeshCore contact card");
                } else {
                    set_status(c, "Exporting MeshCore contact card");
                }
            }
            break;
        case QTC_IPC_DEVICE_SET_PRESET:
            if (f->length == sizeof(qtc_ipc_radio_preset_payload)) {
                const qtc_ipc_radio_preset_payload *p = (const void *)f->payload;
                if (p->freq_mhz <= 0.0 || p->bw_khz <= 0.0 ||
                    p->sf < 5 || p->sf > 12 || p->cr < 5 || p->cr > 8) {
                    reply_error(c, "Invalid radio preset");
                } else if (c->demo) {
                    c->state.radio_freq = p->freq_mhz;
                    c->state.radio_bw = p->bw_khz;
                    c->state.radio_sf = p->sf;
                    c->state.radio_cr = p->cr;
                    c->state.revisions.connection++;
                    set_status(c, "Demo radio preset applied");
                    broadcast_status(c);
                } else if (!c->state.radio_connected) {
                    reply_error(c, "Radio is not connected");
                } else {
                    uint8_t cmd[32];
                    size_t n = qtc_cmd_set_radio_params(cmd, sizeof(cmd),
                                                        p->freq_mhz, p->bw_khz,
                                                        p->sf, p->cr, p->repeat_mode);
                    if (n == 0 || queue_radio_urgent(c, cmd, n, QTC_RADIO_OK,
                                                     QTC_RADIO_ERROR, QTC_RADIO_NONE, "") != 0) {
                        reply_error(c, "Could not queue radio preset");
                    } else {
                        c->state.radio_freq = p->freq_mhz;
                        c->state.radio_bw = p->bw_khz;
                        c->state.radio_sf = p->sf;
                        c->state.radio_cr = p->cr;
                        c->state.revisions.connection++;
                        char status[160];
                        snprintf(status, sizeof(status), "%.31s preset queued; reconnect after it is saved",
                                 p->name[0] ? p->name : "Radio");
                        set_status(c, status);
                        broadcast_status(c);
                    }
                }
            }
            break;
        case QTC_IPC_DEVICE_RECONNECT:
            if (c->demo) {
                set_status(c, "Demo radio reconnected");
                broadcast_status(c);
            } else {
                disconnect_radio(c, "Manual USB reconnect requested");
                c->reconnect_at = qtc_now_millis();
            }
            break;
        case QTC_IPC_DEVICE_SYNC_MESSAGES:
            if (c->demo) {
                set_status(c, "Demo stored-message sync complete");
                broadcast_status(c);
            } else if (!c->state.radio_connected) {
                reply_error(c, "Radio is not connected");
            } else {
                request_inbox_drain(c, true);
                queue_next_inbox_message(c, RADIO_PRIORITY_URGENT);
                set_status(c, "Stored-message sync started");
                broadcast_status(c);
                service_radio_queue(c);
            }
            break;
        case QTC_IPC_CHANNEL_CREATE:
            if (f->length == sizeof(qtc_ipc_channel_action_payload)) {
                const qtc_ipc_channel_action_payload *p = (const void *)f->payload;
                int slot = qtc_find_free_channel_slot(&c->state, c->state.radio_max_channels);
                uint8_t secret[16];
                if (slot < 0) reply_error(c, "No free channel slot");
                else if (p->name[0] == 0 || strlen(p->name) > 32 || qtc_secure_random(secret, 16) != 0 ||
                         apply_channel(c, slot, p->name, secret) != 0) reply_error(c, "Could not create channel");
            }
            break;
        case QTC_IPC_CHANNEL_JOIN:
            if (f->length == sizeof(qtc_ipc_channel_action_payload)) {
                const qtc_ipc_channel_action_payload *p = (const void *)f->payload; char name[33]; uint8_t secret[16];
                int slot = qtc_find_free_channel_slot(&c->state, c->state.radio_max_channels);
                if (slot < 0) reply_error(c, "No free channel slot");
                else if (qtc_channel_join_parse(p->uri, name, sizeof(name), secret) != 0) reply_error(c, "Enter an invitation URI, raw 32-character key, or Name:key");
                else if (apply_channel(c, slot, name, secret) != 0) reply_error(c, "Could not join channel");
            }
            break;
        case QTC_IPC_CHANNEL_ROTATE:
            if (f->length == sizeof(qtc_ipc_channel_action_payload)) {
                const qtc_ipc_channel_action_payload *p = (const void *)f->payload; qtc_channel *ch = find_channel(c, p->channel_index);
                uint8_t secret[16]; if (ch == NULL || qtc_secure_random(secret, 16) != 0 || apply_channel(c, ch->index, ch->name, secret) != 0)
                    reply_error(c, "Could not rotate channel key");
            }
            break;
        case QTC_IPC_CHANNEL_LEAVE:
            if (f->length == sizeof(qtc_ipc_channel_action_payload)) {
                const qtc_ipc_channel_action_payload *p = (const void *)f->payload; uint8_t zero[16] = {0};
                if (apply_channel(c, p->channel_index, "", zero) != 0) reply_error(c, "Could not leave channel");
            }
            break;
        case QTC_IPC_CHANNEL_INVITE:
            if (f->length == sizeof(qtc_ipc_channel_invite_payload)) {
                const qtc_ipc_channel_invite_payload *p = (const void *)f->payload; qtc_channel *ch = find_channel(c, p->channel_index);
                char uri[QTC_MAX_URI], body[QTC_MAX_TEXT];
                if (ch == NULL || !ch->configured || qtc_channel_uri_build(ch->name, ch->secret, uri, sizeof(uri)) != 0 ||
                    qtc_invite_message_build(uri, body, sizeof(body)) != 0 || send_direct_action(c, p->contact_id, body) != 0)
                    reply_error(c, "Could not send channel invitation");
            }
            break;
        case QTC_IPC_INVITE_ACCEPT:
        case QTC_IPC_INVITE_IGNORE:
            if (f->length == sizeof(int64_t)) {
                int64_t id; memcpy(&id, f->payload, sizeof(id)); qtc_invitation *inv = NULL;
                for (size_t n = 0; n < c->state.invitation_count; n++) if (c->state.invitations[n].id == id) inv = &c->state.invitations[n];
                if (inv == NULL) reply_error(c, "Invitation not found");
                else if (f->type == QTC_IPC_INVITE_IGNORE) {
                    (void)qtc_db_update_invitation_status(&c->db, id, QTC_INVITE_IGNORED);
                    (void)reload_state(c, 0, 0, 1, 0); broadcast_snapshot(c);
                } else {
                    char name[33]; uint8_t secret[16]; int slot = qtc_find_free_channel_slot(&c->state, c->state.radio_max_channels);
                    if (slot < 0) reply_error(c, "No free channel slot");
                    else if (qtc_channel_uri_parse(inv->uri, name, sizeof(name), secret) != 0 || apply_channel(c, slot, name, secret) != 0)
                        reply_error(c, "Could not accept invitation");
                    else { (void)qtc_db_update_invitation_status(&c->db, id, QTC_INVITE_ACCEPTED); (void)reload_state(c, 0, 0, 1, 0); broadcast_snapshot(c); }
                }
            }
            break;
        case QTC_IPC_SHUTDOWN: c->running = false; break;
        default: break;
    }
}

static void accept_clients(core_ctx *c) {
    for (;;) {
        int fd = accept(c->server_fd, NULL, NULL);
        if (fd < 0) { if (errno == EAGAIN || errno == EWOULDBLOCK) return; if (errno == EINTR) continue; return; }
        /* BSD hands back a descriptor that inherited O_NONBLOCK from the listener
         * while Linux does not, so the mode is set explicitly rather than assumed.
         * send_snapshot() relies on blocking writes; an inherited O_NONBLOCK made
         * it fail with EAGAIN part-way through a snapshot and drop the client. */
        if (qtc_platform_prepare_fd(fd, false) != 0) { close(fd); continue; }
        /* Darwin defaults a unix socket to an 8 KB send buffer against roughly
         * 200 KB on Linux, which would otherwise stall the event loop on every
         * snapshot. Failure is harmless: the write simply blocks sooner. */
        int sndbuf = 256 * 1024;
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
        if (qtc_ipc_verify_peer_uid(fd, getuid()) != 0) { close(fd); continue; }
        int slot = -1; for (int i = 0; i < QTC_MAX_CLIENTS; i++) if (!c->clients[i].active) { slot = i; break; }
        if (slot < 0) { close(fd); continue; }
        c->clients[slot].fd = fd; c->clients[slot].active = true; qtc_ipc_reader_init(&c->clients[slot].reader);
    }
}

static int acquire_lock(core_ctx *c) {
    c->lock_fd = open(c->paths.lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (c->lock_fd < 0 || flock(c->lock_fd, LOCK_EX | LOCK_NB) != 0) return -1;
    char pid[32]; int n = snprintf(pid, sizeof(pid), "%ld\n", (long)getpid());
    if (qtc_write_file_atomic(c->paths.pid_path, pid, (size_t)n, 0600) != 0) return -1;
    return 0;
}

int qtc_core_run(const qtc_paths *paths, const char *device, bool demo, bool foreground) {
    (void)foreground;
    static core_ctx c;
    memset(&c, 0, sizeof(c)); c.paths = *paths; c.demo = demo; c.running = true;
    c.server_fd = c.lock_fd = -1; c.serial.fd = -1; c.current_client = -1;
    c.clipboard_client = -1;
    qtc_strlcpy(c.requested_device, device, sizeof(c.requested_device));
    if (acquire_lock(&c) != 0) { qtc_log(QTC_LOG_ERROR, "a QTC core is already running for profile %s", paths->profile); return 2; }
    signal(SIGTERM, on_signal); signal(SIGINT, on_signal); signal(SIGHUP, SIG_IGN); signal(SIGPIPE, SIG_IGN);
    if (qtc_db_open(&c.db, paths->db_path) != 0 || qtc_db_migrate(&c.db) != 0) goto fail;
    if (demo && qtc_db_seed_demo(&c.db) != 0) goto fail;
    if (qtc_db_load_state(&c.db, &c.state) != 0) goto fail;
    {
        char since[32];
        if (qtc_db_get_setting(&c.db, "contact_sync_since", since, sizeof(since)) == 0) {
            char *end = NULL;
            unsigned long parsed = strtoul(since, &end, 10);
            if (end != since && *end == 0 && parsed <= UINT32_MAX)
                c.contact_since = (uint32_t)parsed;
        }
    }
    c.state.radio_max_channels = c.state.radio_max_channels > 0 ? c.state.radio_max_channels : 8;
    c.state.radio_max_contacts = c.state.radio_max_contacts > 0 ? c.state.radio_max_contacts : QTC_MAX_CONTACTS;
    if (demo) { c.state.radio_connected = true; qtc_strlcpy(c.state.radio_name, "QTC Demo Radio", sizeof(c.state.radio_name));
                qtc_strlcpy(c.state.radio_model, "software simulation", sizeof(c.state.radio_model)); set_status(&c, "Demo mode: no USB radio is used"); }
    else { set_status(&c, "Background core started; connecting to radio"); (void)connect_radio(&c); }
    c.server_fd = qtc_ipc_server_open(paths->socket_path); if (c.server_fd < 0) goto fail;
    c.next_stored_poll = qtc_now_millis() + INBOX_FALLBACK_POLL_MS;

    while (c.running && !g_stop) {
        struct pollfd pfds[2 + QTC_MAX_CLIENTS]; int map[2 + QTC_MAX_CLIENTS]; nfds_t n = 0;
        pfds[n] = (struct pollfd){.fd = c.server_fd, .events = POLLIN}; map[n++] = -2;
        if (!c.demo && c.serial.fd >= 0) { pfds[n] = (struct pollfd){.fd = c.serial.fd, .events = POLLIN}; map[n++] = -3; }
        for (int i = 0; i < QTC_MAX_CLIENTS; i++) if (c.clients[i].active) {
            pfds[n] = (struct pollfd){.fd = c.clients[i].fd, .events = POLLIN}; map[n++] = i;
        }
        int rc = poll(pfds, n, 50);
        if (rc < 0 && errno != EINTR) break;
        for (nfds_t j = 0; j < n; j++) {
            if (pfds[j].revents == 0) continue;
            if (map[j] == -2 && (pfds[j].revents & POLLIN)) accept_clients(&c);
            else if (map[j] == -3) {
                if (pfds[j].revents & (POLLERR | POLLHUP | POLLNVAL)) disconnect_radio(&c, "Radio disconnected; reconnecting");
                else if (pfds[j].revents & POLLIN) {
                    for (;;) {
                        uint8_t buf[8192];
                        ssize_t got = qtc_serial_read(&c.serial, buf, sizeof(buf));
                        if (got > 0) {
                            if (qtc_serial_parser_feed(&c.serial.parser, buf, (size_t)got,
                                                       radio_frame_cb, &c) != 0) {
                                disconnect_radio(&c, "Radio protocol parse failed; reconnecting");
                                break;
                            }
                            continue;
                        }
                        /* VMIN=0 permits a zero-length read after the available
                         * bytes have been drained; POLLHUP/POLLERR handles a
                         * real disconnect. */
                        if (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                            errno != EINTR) {
                            disconnect_radio(&c, "Radio read failed; reconnecting");
                        }
                        break;
                    }
                }
            } else if (map[j] >= 0 && map[j] < QTC_MAX_CLIENTS) {
                int i = map[j];
                /* A client may send one final command and close immediately. POLLIN and
                 * POLLHUP can then arrive together, so drain readable data before closing. */
                if (pfds[j].revents & POLLIN) {
                    for (;;) {
                        uint8_t buf[16384];
                        ssize_t got = recv(c.clients[i].fd, buf, sizeof(buf), MSG_DONTWAIT);
                        if (got > 0) {
                            c.current_client = i;
                            if (qtc_ipc_reader_feed(&c.clients[i].reader, buf, (size_t)got,
                                                    handle_client_frame, &c) != 0) {
                                close_client(&c, i);
                            }
                            c.current_client = -1;
                            if (!c.clients[i].active) break;
                            continue;
                        }
                        if (got == 0) close_client(&c, i);
                        else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                            close_client(&c, i);
                        break;
                    }
                }
                if (c.clients[i].active && (pfds[j].revents & (POLLERR | POLLHUP | POLLNVAL))) close_client(&c, i);
            }
        }
        int64_t now = qtc_now_millis();
        if (!c.demo && c.serial.fd < 0 && now >= c.reconnect_at) { c.reconnect_at = now + 3000; (void)connect_radio(&c); }
        if (!c.demo && c.state.radio_connected &&
            c.session_phase == RADIO_SESSION_READY &&
            now >= c.next_stored_poll) {
            int64_t interval = c.state.settings.stored_poll_seconds <= 1
                                   ? INBOX_FALLBACK_POLL_MS
                                   : c.state.settings.stored_poll_seconds * 1000LL;
            c.next_stored_poll = now + interval;
            /* PUSH_MSG_WAITING is the primary receive trigger. The fallback is
             * deliberately opportunistic so it cannot occupy the serialized
             * command channel while interactive traffic is waiting. */
            if (!inbox_needs_drain(&c) && !c.radio_pending && c.queue_count == 0)
                request_inbox_drain(&c, true);
        }
        if (!c.demo && c.state.radio_connected &&
            c.session_phase == RADIO_SESSION_READY && inbox_needs_drain(&c))
            queue_next_inbox_message(&c, RADIO_PRIORITY_INBOX);
        service_ack_timeouts(&c);
        service_background_sync(&c);
        service_radio_queue(&c);
    }

    for (int i = 0; i < QTC_MAX_CLIENTS; i++) close_client(&c, i);
    if (c.serial.fd >= 0) qtc_serial_close(&c.serial);
    if (c.server_fd >= 0) close(c.server_fd);
    unlink(paths->socket_path); unlink(paths->pid_path); qtc_db_close(&c.db);
    if (c.lock_fd >= 0) { flock(c.lock_fd, LOCK_UN); close(c.lock_fd); }
    return 0;
fail:
    qtc_log(QTC_LOG_ERROR, "core startup failed: %s", strerror(errno));
    if (c.server_fd >= 0) close(c.server_fd);
    if (c.serial.fd >= 0) qtc_serial_close(&c.serial);
    unlink(paths->socket_path);
    unlink(paths->pid_path);
    qtc_db_close(&c.db);
    if (c.lock_fd >= 0) close(c.lock_fd);
    return 1;
}

int qtc_core_status(const qtc_paths *paths) {
    int fd = qtc_ipc_client_connect(paths->socket_path, 500);
    if (fd < 0) { puts("QTC core: stopped"); return 1; }
    (void)qtc_ipc_send(fd, QTC_IPC_PING, NULL, 0); qtc_ipc_frame f;
    int rc = 1;
    for (int i = 0; i < 16; i++) {
        if (qtc_ipc_recv_blocking(fd, &f, 1000) != 0) break;
        if (f.type == QTC_IPC_CORE_INFO && f.length == sizeof(qtc_ipc_core_info)) {
            const qtc_ipc_core_info *info = (const void *)f.payload;
            if (info->protocol_version != QTC_IPC_PROTOCOL_VERSION ||
                strcmp(info->app_version, QTC_VERSION) != 0) {
                printf("QTC core: incompatible (%s)\n", info->app_version);
                break;
            }
            continue;
        }
        if (f.type == QTC_IPC_STATUS && f.length == sizeof(qtc_ipc_status_payload)) {
            const qtc_ipc_status_payload *s = (const void *)f.payload;
            printf("QTC core: running\nMode: %s\nRadio: %s\nStatus: %s\n",
                   s->demo_mode ? "demo" : "radio", s->radio_connected ? "connected" : "disconnected", s->message); rc = 0; break;
        }
    }
    close(fd); return rc;
}

int qtc_core_shutdown(const qtc_paths *paths) {
    int fd = qtc_ipc_client_connect(paths->socket_path, 500);
    if (fd < 0) { fprintf(stderr, "QTC core is not running\n"); return 1; }
    int rc = qtc_ipc_send(fd, QTC_IPC_SHUTDOWN, NULL, 0); close(fd); return rc == 0 ? 0 : 1;
}

static int probe_core_compatibility(const qtc_paths *paths, int timeout_ms) {
    int fd = qtc_ipc_client_connect(paths->socket_path, timeout_ms);
    if (fd < 0) return -1;
    int rc = -1;
    if (qtc_ipc_send(fd, QTC_IPC_PING, NULL, 0) == 0) {
        qtc_ipc_frame frame;
        if (qtc_ipc_recv_blocking(fd, &frame, timeout_ms) == 0 &&
            frame.type == QTC_IPC_CORE_INFO &&
            frame.length == sizeof(qtc_ipc_core_info)) {
            const qtc_ipc_core_info *info = (const void *)frame.payload;
            rc = info->protocol_version == QTC_IPC_PROTOCOL_VERSION &&
                 strcmp(info->app_version, QTC_VERSION) == 0 ? 0 : 1;
        } else {
            /* A running pre-handshake QTC core responds to PING with STATUS,
             * not CORE_INFO. Treat it as incompatible rather than silently
             * attaching a new TUI to old behavior. */
            rc = 1;
        }
    }
    close(fd);
    return rc;
}

int qtc_core_ensure_running(const char *self, const qtc_paths *paths, const char *device, bool demo) {
    int probe = probe_core_compatibility(paths, 250);
    if (probe == 0) return 0;
    if (probe > 0) {
        qtc_log(QTC_LOG_INFO, "stopping incompatible QTC background core before restart");
        (void)qtc_core_shutdown(paths);
        for (int tries = 0; tries < 30; tries++) {
            if (access(paths->socket_path, F_OK) != 0) break;
            usleep(50000);
        }
    }
    char profile_arg[96]; snprintf(profile_arg, sizeof(profile_arg), "%s", paths->profile);
    char *argv[12]; int i = 0; argv[i++] = (char *)self; argv[i++] = "core"; argv[i++] = "--profile"; argv[i++] = profile_arg;
    if (device != NULL && *device != 0) { argv[i++] = "--device"; argv[i++] = (char *)device; }
    if (demo) argv[i++] = "--demo";
    argv[i] = NULL;
    if (qtc_spawn_detached(argv) != 0) return -1;
    for (int tries = 0; tries < 50; tries++) {
        usleep(100000);
        if (probe_core_compatibility(paths, 150) == 0) return 0;
    }
    errno = ETIMEDOUT; return -1;
}
