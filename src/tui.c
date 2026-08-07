#define _GNU_SOURCE
#include "qtc/tui.h"
#include "qtc/invite.h"
#include "qtc/ipc.h"
#include "qtc/message.h"
#include "qtc/notify.h"
#include "qtc/roster.h"
#include "qtc/util.h"

#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

typedef enum { VIEW_MESSAGES, VIEW_CHANNELS, VIEW_NODES, VIEW_SETTINGS } tui_view;
typedef enum {
    FEEDBACK_NONE = 0, FEEDBACK_INFO, FEEDBACK_SUCCESS, FEEDBACK_ERROR
} feedback_level;
typedef enum {
    MODE_NORMAL, MODE_SEARCH, MODE_COMPOSE, MODE_CREATE_CHANNEL, MODE_JOIN_CHANNEL,
    MODE_INVITE_PICKER, MODE_INVITE_REVIEW, MODE_ROTATE_CONFIRM, MODE_LEAVE_CONFIRM,
    MODE_INCOMING_INVITE, MODE_ALIAS, MODE_FAVORITE_GROUP, MODE_DEVICE_NAME,
    MODE_TX_POWER, MODE_THEME_PICKER, MODE_PRESET_PICKER
} tui_mode;

typedef struct {
    int kind;
    int source_index;
    char key[QTC_MAX_ID];
} menu_item;

typedef struct {
    int fd;
    qtc_ipc_reader reader;
    qtc_state state;
    bool loading;
    bool running;
    bool dirty;
    struct termios original;
    bool terminal_saved;
    int stdout_flags;
    bool stdout_flags_saved;
    char *output;
    size_t output_len;
    size_t output_off;
    int width;
    int height;
    tui_view view;
    tui_mode mode;
    char status[160];
    char action_feedback[160];
    feedback_level action_feedback_level;
    int64_t action_feedback_until;
    bool advert_feedback_pending;
    char search[QTC_MAX_NAME];
    char input[QTC_MAX_TEXT];
    size_t input_len;
    int selected_kind;
    char selected_key[QTC_MAX_ID];
    int selected_channel;
    char selected_node[QTC_MAX_ID];
    size_t contact_scroll;
    size_t history_scroll;
    qtc_conversation_kind open_kind;
    char open_key[QTC_MAX_ID];
    char invite_cursor[QTC_MAX_ID];
    char invite_search[QTC_MAX_NAME];
    char invite_ids[64][QTC_MAX_ID];
    size_t invite_count;
    bool confirm_action;
    int64_t modal_enter_block_until;
    int64_t last_ctrl_q;
    int64_t incoming_invite_id;
    int theme_cursor;
    int preset_cursor;
    char banner_title[QTC_MAX_NAME];
    char banner_body[256];
    int64_t banner_until;
    char escape_buf[32];
    size_t escape_len;
} tui_ctx;

typedef struct {
    const char *name;
    double freq_mhz;
    double bw_khz;
    int sf;
    int cr;
} radio_preset;

static const radio_preset RADIO_PRESETS[] = {
    {"Europe / UK", 867.500, 250.0, 10, 5},
    {"Europe / UK narrow", 869.618, 62.5, 8, 5},
    {"USA / Canada", 910.525, 62.5, 7, 5},
    {"Australia / New Zealand", 915.800, 250.0, 10, 5}
};

static volatile sig_atomic_t g_resize;
static void sigwinch_handler(int sig) { (void)sig; g_resize = 1; }
static const char *theme_name(int index);

static void set_action_feedback(tui_ctx *t, const char *message,
                                feedback_level level, int64_t duration_ms) {
    qtc_strlcpy(t->action_feedback, message, sizeof(t->action_feedback));
    t->action_feedback_level = level;
    t->action_feedback_until = qtc_now_millis() + duration_ms;
    t->dirty = true;
}

static bool is_advert_feedback(const char *message) {
    return message != NULL && strstr(message, "advertisement") != NULL;
}

static feedback_level advert_feedback_level(const char *message) {
    if (message == NULL) return FEEDBACK_INFO;
    if (strstr(message, "failed") != NULL ||
        strstr(message, "timed out") != NULL ||
        strstr(message, "not connected") != NULL ||
        strstr(message, "Could not") != NULL)
        return FEEDBACK_ERROR;
    if (strstr(message, "sent") != NULL)
        return FEEDBACK_SUCCESS;
    return FEEDBACK_INFO;
}

static void tty_write_all(const char *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(STDOUT_FILENO, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        off += (size_t)n;
    }
}

/* Terminal output must never block IPC processing. A full frame uses absolute
 * cursor addressing, so replacing a partially written older frame with the
 * newest frame is safe: the newest frame rewrites every visible row. */
static void discard_output(tui_ctx *t) {
    free(t->output);
    t->output = NULL;
    t->output_len = 0;
    t->output_off = 0;
}

static void queue_output(tui_ctx *t, char *data, size_t len) {
    discard_output(t);
    t->output = data;
    t->output_len = len;
}

static void flush_output(tui_ctx *t) {
    while (t->output != NULL && t->output_off < t->output_len) {
        ssize_t n = write(STDOUT_FILENO, t->output + t->output_off,
                          t->output_len - t->output_off);
        if (n > 0) {
            t->output_off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        discard_output(t);
        return;
    }
    if (t->output != NULL && t->output_off == t->output_len)
        discard_output(t);
}

static int copy_to_clipboard(const char *text) {
    static const char *commands[] = {
        "wl-copy 2>/dev/null",
        "xclip -selection clipboard 2>/dev/null",
        "xsel --clipboard --input 2>/dev/null",
        "pbcopy 2>/dev/null"
    };
    if (text == NULL || *text == 0) return -1;
    for (size_t i = 0; i < QTC_ARRAY_LEN(commands); i++) {
        FILE *pipe = popen(commands[i], "w");
        if (pipe == NULL) continue;
        size_t len = strlen(text);
        bool ok = fwrite(text, 1, len, pipe) == len;
        int rc = pclose(pipe);
        if (ok && rc == 0) return 0;
    }
    return -1;
}

static const char *contact_name(const qtc_contact *c) { return c->alias[0] ? c->alias : c->name; }

static qtc_contact *find_contact(tui_ctx *t, const char *id) {
    for (size_t i = 0; i < t->state.contact_count; i++)
        if (strcmp(t->state.contacts[i].id, id) == 0 || strcmp(t->state.contacts[i].prefix, id) == 0) return &t->state.contacts[i];
    return NULL;
}
static qtc_channel *find_channel(tui_ctx *t, int index) {
    for (size_t i = 0; i < t->state.channel_count; i++) if (t->state.channels[i].index == index) return &t->state.channels[i];
    return NULL;
}
static qtc_invitation *find_invitation(tui_ctx *t, int64_t id) {
    for (size_t i = 0; i < t->state.invitation_count; i++) if (t->state.invitations[i].id == id) return &t->state.invitations[i];
    return NULL;
}

static int set_raw_terminal(tui_ctx *t) {
    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &t->original) != 0) return -1;
    t->terminal_saved = true; struct termios raw = t->original;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO | IEXTEN | ISIG); raw.c_iflag &= (tcflag_t)~(IXON | ICRNL);
    raw.c_oflag &= (tcflag_t)~OPOST; raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return -1;
    const char *enter = "\x1b[?1049h\x1b[?25l";
    tty_write_all(enter, strlen(enter));
    t->stdout_flags = fcntl(STDOUT_FILENO, F_GETFL, 0);
    if (t->stdout_flags >= 0) {
        t->stdout_flags_saved = true;
        (void)fcntl(STDOUT_FILENO, F_SETFL, t->stdout_flags | O_NONBLOCK);
    }
    return 0;
}

static void restore_terminal(tui_ctx *t) {
    discard_output(t);
    if (t->stdout_flags_saved)
        (void)fcntl(STDOUT_FILENO, F_SETFL, t->stdout_flags);
    if (t->terminal_saved) (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &t->original);
    const char *leave = "\x1b[0m\x1b[?25h\x1b[?1049l";
    tty_write_all(leave, strlen(leave));
}

static void update_size(tui_ctx *t) {
    struct winsize ws = {0};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        t->width = ws.ws_col > 40 ? ws.ws_col : 80; t->height = ws.ws_row > 12 ? ws.ws_row : 24;
    } else { t->width = 100; t->height = 30; }
    if (t->width > 300) t->width = 300;
    if (t->height > 120) t->height = 120;
}

static void tui_upsert_contact(tui_ctx *t, const qtc_contact *contact) {
    for (size_t i = 0; i < t->state.contact_count; i++) {
        if (strcmp(t->state.contacts[i].id, contact->id) == 0) {
            t->state.contacts[i] = *contact;
            t->state.revisions.contacts++;
            t->dirty = true;
            return;
        }
    }
    if (t->state.contact_count < QTC_MAX_CONTACTS) {
        t->state.contacts[t->state.contact_count++] = *contact;
        t->state.revisions.contacts++;
        t->dirty = true;
    }
}

static void tui_upsert_channel(tui_ctx *t, const qtc_channel *channel) {
    for (size_t i = 0; i < t->state.channel_count; i++) {
        if (t->state.channels[i].index == channel->index) {
            t->state.channels[i] = *channel;
            t->state.revisions.channels++;
            t->dirty = true;
            return;
        }
    }
    if (t->state.channel_count < QTC_MAX_CHANNELS) {
        t->state.channels[t->state.channel_count++] = *channel;
        t->state.revisions.channels++;
        t->dirty = true;
    }
}

static void tui_upsert_message(tui_ctx *t, const qtc_message *message) {
    for (size_t i = 0; i < t->state.message_count; i++) {
        if (strcmp(t->state.messages[i].message_key, message->message_key) == 0) {
            t->state.messages[i] = *message;
            t->state.revisions.messages++;
            t->dirty = true;
            return;
        }
    }
    if (t->state.message_count == QTC_MAX_MESSAGES) {
        memmove(&t->state.messages[0], &t->state.messages[1],
                (QTC_MAX_MESSAGES - 1U) * sizeof(t->state.messages[0]));
        t->state.message_count--;
    }
    t->state.messages[t->state.message_count++] = *message;
    t->state.revisions.messages++;
    t->dirty = true;
}

static void tui_upsert_invitation(tui_ctx *t, const qtc_invitation *invitation) {
    for (size_t i = 0; i < t->state.invitation_count; i++) {
        if (t->state.invitations[i].id == invitation->id) {
            t->state.invitations[i] = *invitation;
            t->dirty = true;
            return;
        }
    }
    if (t->state.invitation_count < QTC_MAX_INVITATIONS) {
        t->state.invitations[t->state.invitation_count++] = *invitation;
        t->dirty = true;
    }
}

static void ipc_frame(const qtc_ipc_frame *f, void *userdata) {
    tui_ctx *t = userdata;
    switch (f->type) {
        case QTC_IPC_STATE_BEGIN:
            t->loading = true; t->state.contact_count = t->state.channel_count = t->state.message_count = t->state.invitation_count = 0;
            if (f->length == sizeof(qtc_revisions)) memcpy(&t->state.revisions, f->payload, sizeof(qtc_revisions));
            break;
        case QTC_IPC_CONTACT:
            if (f->length == sizeof(qtc_contact)) {
                if (t->loading && t->state.contact_count < QTC_MAX_CONTACTS)
                    memcpy(&t->state.contacts[t->state.contact_count++], f->payload, sizeof(qtc_contact));
                else if (!t->loading)
                    tui_upsert_contact(t, (const qtc_contact *)f->payload);
            }
            break;
        case QTC_IPC_CHANNEL:
            if (f->length == sizeof(qtc_channel)) {
                if (t->loading && t->state.channel_count < QTC_MAX_CHANNELS)
                    memcpy(&t->state.channels[t->state.channel_count++], f->payload, sizeof(qtc_channel));
                else if (!t->loading)
                    tui_upsert_channel(t, (const qtc_channel *)f->payload);
            }
            break;
        case QTC_IPC_MESSAGE:
            if (f->length == sizeof(qtc_message)) {
                if (t->loading && t->state.message_count < QTC_MAX_MESSAGES)
                    memcpy(&t->state.messages[t->state.message_count++], f->payload, sizeof(qtc_message));
                else if (!t->loading)
                    tui_upsert_message(t, (const qtc_message *)f->payload);
            }
            break;
        case QTC_IPC_INVITATION:
            if (f->length == sizeof(qtc_invitation)) {
                if (t->loading && t->state.invitation_count < QTC_MAX_INVITATIONS)
                    memcpy(&t->state.invitations[t->state.invitation_count++], f->payload, sizeof(qtc_invitation));
                else if (!t->loading)
                    tui_upsert_invitation(t, (const qtc_invitation *)f->payload);
            }
            break;
        case QTC_IPC_SETTINGS:
            if (f->length == sizeof(qtc_settings)) {
                memcpy(&t->state.settings, f->payload, sizeof(qtc_settings));
                if (!t->loading) t->dirty = true;
            }
            break;
        case QTC_IPC_STATUS:
            if (f->length == sizeof(qtc_ipc_status_payload)) {
                const qtc_ipc_status_payload *s = (const void *)f->payload;
                t->state.radio_connected = s->radio_connected;
                t->state.radio_max_channels = s->max_channels;
                t->state.radio_max_contacts = s->max_contacts;
                t->state.radio_tx_power = s->tx_power;
                t->state.radio_max_tx_power = s->max_tx_power;
                t->state.radio_freq = s->freq;
                t->state.radio_bw = s->bw;
                t->state.radio_sf = s->sf;
                t->state.radio_cr = s->cr;
                qtc_strlcpy(t->state.radio_name, s->radio_name, sizeof(t->state.radio_name));
                qtc_strlcpy(t->state.radio_model, s->radio_model, sizeof(t->state.radio_model));
                qtc_strlcpy(t->state.radio_version, s->radio_version, sizeof(t->state.radio_version));
                qtc_strlcpy(t->status, s->message, sizeof(t->status));
                if (is_advert_feedback(s->message)) {
                    feedback_level level = advert_feedback_level(s->message);
                    set_action_feedback(t, s->message, level,
                                        level == FEEDBACK_INFO ? 8000 : 6000);
                    if (level != FEEDBACK_INFO) t->advert_feedback_pending = false;
                }
                if (!t->loading) t->dirty = true;
            }
            break;
        case QTC_IPC_STATE_END: t->loading = false; t->dirty = true; break;
        case QTC_IPC_BANNER:
            if (f->length == sizeof(qtc_ipc_banner_payload)) {
                const qtc_ipc_banner_payload *b = (const void *)f->payload;
                qtc_strlcpy(t->banner_title, b->title, sizeof(t->banner_title));
                qtc_strlcpy(t->banner_body, b->body, sizeof(t->banner_body));
                t->banner_until = qtc_now_millis() + 6000;
                t->dirty = true;
            }
            break;
        case QTC_IPC_CLIPBOARD_TEXT:
            if (f->length == sizeof(qtc_ipc_clipboard_payload)) {
                const qtc_ipc_clipboard_payload *p = (const void *)f->payload;
                if (copy_to_clipboard(p->text) == 0)
                    qtc_strlcpy(t->status, "MeshCore contact card copied to clipboard", sizeof(t->status));
                else
                    qtc_strlcpy(t->status, "Install wl-clipboard, xclip, or xsel to copy the contact card (pbcopy on macOS)", sizeof(t->status));
                t->dirty = true;
            }
            break;
        case QTC_IPC_ERROR: {
            const char *message = f->length ? (const char *)f->payload : "QTC core error";
            qtc_strlcpy(t->status, message, sizeof(t->status));
            if (t->advert_feedback_pending) {
                set_action_feedback(t, message, FEEDBACK_ERROR, 7000);
                t->advert_feedback_pending = false;
            }
            t->dirty = true;
            break;
        }
        default: break;
    }
}

static int verify_core_version(int fd) {
    qtc_ipc_hello h = {.protocol_version = QTC_IPC_PROTOCOL_VERSION};
    qtc_strlcpy(h.client_name, "qtc-tui", sizeof(h.client_name));
    qtc_strlcpy(h.app_version, QTC_VERSION, sizeof(h.app_version));
    if (qtc_ipc_send(fd, QTC_IPC_HELLO, &h, sizeof(h)) != 0) return -1;
    qtc_ipc_frame frame;
    if (qtc_ipc_recv_blocking(fd, &frame, 1500) != 0 ||
        frame.type != QTC_IPC_CORE_INFO ||
        frame.length != sizeof(qtc_ipc_core_info)) {
        errno = EPROTO;
        return -1;
    }
    const qtc_ipc_core_info *info = (const void *)frame.payload;
    if (info->protocol_version != QTC_IPC_PROTOCOL_VERSION ||
        strcmp(info->app_version, QTC_VERSION) != 0) {
        errno = EPROTO;
        return -1;
    }
    return 0;
}

static size_t build_menu(tui_ctx *t, menu_item *items, size_t max, qtc_roster *roster) {
    qtc_roster_build(&t->state, t->search, roster); size_t n = 0;
    for (size_t i = 0; i < roster->pinned_count && n < max; i++) {
        qtc_roster_row *r = &roster->pinned[i]; if (r->kind != 1 && r->kind != 2) continue;
        items[n].kind = r->kind; items[n].source_index = r->source_index;
        if (r->kind == 1) snprintf(items[n].key, sizeof(items[n].key), "%d", t->state.channels[r->source_index].index);
        else qtc_strlcpy(items[n].key, t->state.contacts[r->source_index].id, sizeof(items[n].key));
        n++;
    }
    for (size_t i = 0; i < roster->scrollable_count && n < max; i++) {
        qtc_roster_row *r = &roster->scrollable[i]; if (r->kind != 3) continue;
        items[n].kind = 3; items[n].source_index = r->source_index;
        qtc_strlcpy(items[n].key, t->state.contacts[r->source_index].id, sizeof(items[n].key)); n++;
    }
    return n;
}

static int selected_menu_pos(tui_ctx *t, menu_item *items, size_t count) {
    for (size_t i = 0; i < count; i++)
        if (items[i].kind == t->selected_kind && strcmp(items[i].key, t->selected_key) == 0) return (int)i;
    /* A contact changes roster kind when it is favorited or unfavorited. Keep the
     * identity selected instead of jumping to the first channel after the snapshot. */
    if (t->selected_kind == 2 || t->selected_kind == 3) {
        for (size_t i = 0; i < count; i++) {
            if ((items[i].kind == 2 || items[i].kind == 3) &&
                strcmp(items[i].key, t->selected_key) == 0) {
                t->selected_kind = items[i].kind;
                return (int)i;
            }
        }
    }
    if (count > 0) { t->selected_kind = items[0].kind; qtc_strlcpy(t->selected_key, items[0].key, sizeof(t->selected_key)); return 0; }
    t->selected_kind = 0; t->selected_key[0] = 0; return -1;
}

static void move_menu(tui_ctx *t, int delta) {
    menu_item items[QTC_MAX_CONTACTS + QTC_MAX_CHANNELS]; qtc_roster roster;
    size_t count = build_menu(t, items, QTC_ARRAY_LEN(items), &roster); int pos = selected_menu_pos(t, items, count);
    if (pos < 0) return;
    pos += delta;
    if (pos < 0) pos = 0;
    if ((size_t)pos >= count) pos = (int)count - 1;
    t->selected_kind = items[pos].kind; qtc_strlcpy(t->selected_key, items[pos].key, sizeof(t->selected_key)); t->dirty = true;
}

static void report_active_conversation(tui_ctx *t, bool active) {
    qtc_ipc_active_payload p = {0};
    p.active = active;
    p.kind = t->open_kind;
    qtc_strlcpy(p.key, active ? t->open_key : "", sizeof(p.key));
    (void)qtc_ipc_send(t->fd, QTC_IPC_ACTIVE_CONVERSATION, &p, sizeof(p));
}

static void leave_conversation(tui_ctx *t) {
    if (t->open_key[0]) report_active_conversation(t, false);
}

static void set_view(tui_ctx *t, tui_view view) {
    if (t->view == VIEW_MESSAGES && view != VIEW_MESSAGES) leave_conversation(t);
    t->view = view;
    t->mode = MODE_NORMAL;
    if (view == VIEW_MESSAGES && t->open_key[0]) report_active_conversation(t, true);
    t->dirty = true;
}

static void open_selected(tui_ctx *t) {
    if (t->selected_kind == 1) { t->open_kind = QTC_CONV_CHANNEL; qtc_strlcpy(t->open_key, t->selected_key, sizeof(t->open_key)); }
    else if (t->selected_kind == 2 || t->selected_kind == 3) { t->open_kind = QTC_CONV_CONTACT; qtc_strlcpy(t->open_key, t->selected_key, sizeof(t->open_key)); }
    else return;
    qtc_ipc_mark_read_payload p = {.kind = t->open_kind}; qtc_strlcpy(p.key, t->open_key, sizeof(p.key));
    (void)qtc_ipc_send(t->fd, QTC_IPC_MARK_READ, &p, sizeof(p));
    t->history_scroll = 0;
    report_active_conversation(t, true);
    t->dirty = true;
}

static void start_input(tui_ctx *t, tui_mode mode) { t->mode = mode; t->input[0] = 0; t->input_len = 0; t->dirty = true; }
static void append_input(tui_ctx *t, char c) {
    if (t->input_len + 1 < sizeof(t->input)) { t->input[t->input_len++] = c; t->input[t->input_len] = 0; t->dirty = true; }
}
static size_t utf8_previous_boundary(const char *text, size_t length) {
    if (length == 0) return 0;
    size_t pos = length - 1;
    while (pos > 0 && (((unsigned char)text[pos] & 0xc0U) == 0x80U)) pos--;
    return pos;
}
static void backspace_input(tui_ctx *t) {
    if (t->input_len) {
        t->input_len = utf8_previous_boundary(t->input, t->input_len);
        t->input[t->input_len] = 0;
        t->dirty = true;
    }
}

static void send_composed(tui_ctx *t) {
    qtc_trim(t->input); if (t->input[0] == 0) { t->mode = MODE_NORMAL; return; }
    if (t->open_kind == QTC_CONV_CONTACT) {
        qtc_ipc_send_direct_payload p = {0}; qtc_strlcpy(p.contact_id, t->open_key, sizeof(p.contact_id)); qtc_strlcpy(p.text, t->input, sizeof(p.text));
        (void)qtc_ipc_send(t->fd, QTC_IPC_SEND_DIRECT, &p, sizeof(p));
    } else if (t->open_kind == QTC_CONV_CHANNEL) {
        qtc_ipc_send_channel_payload p = {.channel_index = atoi(t->open_key)}; qtc_strlcpy(p.text, t->input, sizeof(p.text));
        (void)qtc_ipc_send(t->fd, QTC_IPC_SEND_CHANNEL, &p, sizeof(p));
    }
    t->mode = MODE_NORMAL; t->input[0] = 0; t->input_len = 0; t->dirty = true;
}

static size_t person_list(tui_ctx *t, int *indices, size_t max) {
    size_t n = 0;
    for (size_t i = 0; i < t->state.contact_count && n < max; i++) {
        qtc_contact *c = &t->state.contacts[i];
        if (c->node_type == QTC_NODE_PERSON && qtc_search_match(t->invite_search, contact_name(c))) indices[n++] = (int)i;
    }
    return n;
}
static bool invite_selected(tui_ctx *t, const char *id) {
    for (size_t i = 0; i < t->invite_count; i++) {
        if (strcmp(t->invite_ids[i], id) == 0) return true;
    }
    return false;
}
static void toggle_invite(tui_ctx *t, const char *id) {
    for (size_t i = 0; i < t->invite_count; i++) if (strcmp(t->invite_ids[i], id) == 0) {
        for (size_t j = i + 1; j < t->invite_count; j++) qtc_strlcpy(t->invite_ids[j - 1], t->invite_ids[j], QTC_MAX_ID);
        t->invite_count--; t->dirty = true; return;
    }
    if (t->invite_count < QTC_ARRAY_LEN(t->invite_ids)) qtc_strlcpy(t->invite_ids[t->invite_count++], id, QTC_MAX_ID);
    t->dirty = true;
}
static int invite_cursor_pos(tui_ctx *t, int *indices, size_t count) {
    for (size_t i = 0; i < count; i++) if (strcmp(t->state.contacts[indices[i]].id, t->invite_cursor) == 0) return (int)i;
    if (count) qtc_strlcpy(t->invite_cursor, t->state.contacts[indices[0]].id, sizeof(t->invite_cursor));
    return count ? 0 : -1;
}
static void move_invite_cursor(tui_ctx *t, int delta) {
    int idx[QTC_MAX_CONTACTS]; size_t n = person_list(t, idx, QTC_ARRAY_LEN(idx)); int pos = invite_cursor_pos(t, idx, n);
    if (pos < 0) return;
    pos += delta;
    if (pos < 0) pos = 0;
    if ((size_t)pos >= n) pos = (int)n - 1;
    qtc_strlcpy(t->invite_cursor, t->state.contacts[idx[pos]].id, sizeof(t->invite_cursor)); t->dirty = true;
}

static void send_invites(tui_ctx *t) {
    for (size_t i = 0; i < t->invite_count; i++) {
        qtc_ipc_channel_invite_payload p = {.channel_index = t->selected_channel};
        qtc_strlcpy(p.contact_id, t->invite_ids[i], sizeof(p.contact_id));
        (void)qtc_ipc_send(t->fd, QTC_IPC_CHANNEL_INVITE, &p, sizeof(p));
    }
    snprintf(t->status, sizeof(t->status), "Invitation queued for %zu contact%s", t->invite_count, t->invite_count == 1 ? "" : "s");
    t->mode = MODE_NORMAL; t->invite_count = 0; t->invite_search[0] = 0; t->confirm_action = false; t->dirty = true;
}

static void save_settings(tui_ctx *t) { (void)qtc_ipc_send(t->fd, QTC_IPC_SETTINGS, &t->state.settings, sizeof(t->state.settings)); }

static void cycle_theme(tui_ctx *t, int delta) {
    int theme = t->state.settings.theme;
    if (theme < 0 || theme > 3) theme = 0;
    theme = (theme + delta) % 4;
    if (theme < 0) theme += 4;
    t->state.settings.theme = theme;
    save_settings(t);
    t->dirty = true;
}

static void apply_preset(tui_ctx *t) {
    if (t->preset_cursor < 0 || (size_t)t->preset_cursor >= QTC_ARRAY_LEN(RADIO_PRESETS)) return;
    const radio_preset *preset = &RADIO_PRESETS[t->preset_cursor];
    qtc_ipc_radio_preset_payload p = {0};
    qtc_strlcpy(p.name, preset->name, sizeof(p.name));
    p.freq_mhz = preset->freq_mhz;
    p.bw_khz = preset->bw_khz;
    p.sf = preset->sf;
    p.cr = preset->cr;
    p.repeat_mode = false;
    (void)qtc_ipc_send(t->fd, QTC_IPC_DEVICE_SET_PRESET, &p, sizeof(p));
    snprintf(t->status, sizeof(t->status), "%s preset queued", preset->name);
}

static void handle_enter(tui_ctx *t) {
    if (t->mode == MODE_SEARCH) { t->mode = MODE_NORMAL; t->dirty = true; return; }
    if (t->mode == MODE_COMPOSE) { send_composed(t); return; }
    if (t->mode == MODE_CREATE_CHANNEL) {
        qtc_trim(t->input); if (t->input[0]) { qtc_ipc_channel_action_payload p = {0}; qtc_strlcpy(p.name, t->input, sizeof(p.name));
            (void)qtc_ipc_send(t->fd, QTC_IPC_CHANNEL_CREATE, &p, sizeof(p)); }
        t->mode = MODE_NORMAL; t->dirty = true; return;
    }
    if (t->mode == MODE_JOIN_CHANNEL) {
        qtc_trim(t->input); if (t->input[0]) { qtc_ipc_channel_action_payload p = {0}; qtc_strlcpy(p.uri, t->input, sizeof(p.uri));
            (void)qtc_ipc_send(t->fd, QTC_IPC_CHANNEL_JOIN, &p, sizeof(p)); }
        t->mode = MODE_NORMAL; t->dirty = true; return;
    }
    if (t->mode == MODE_ALIAS) {
        qtc_contact *ct = find_contact(t, t->selected_key);
        if (ct != NULL) {
            qtc_trim(t->input);
            qtc_ipc_contact_text_payload p = {0};
            qtc_strlcpy(p.contact_id, ct->id, sizeof(p.contact_id));
            qtc_strlcpy(p.value, t->input, sizeof(p.value));
            (void)qtc_ipc_send(t->fd, QTC_IPC_SET_ALIAS, &p, sizeof(p));
        }
        t->mode = MODE_NORMAL; t->dirty = true; return;
    }
    if (t->mode == MODE_FAVORITE_GROUP) {
        qtc_contact *ct = find_contact(t, t->selected_key);
        if (ct != NULL) {
            qtc_trim(t->input);
            qtc_ipc_favorite_payload p = {.favorite = true};
            qtc_strlcpy(p.contact_id, ct->id, sizeof(p.contact_id));
            qtc_strlcpy(p.group, t->input[0] ? t->input : "Favorites", sizeof(p.group));
            (void)qtc_ipc_send(t->fd, QTC_IPC_SET_FAVORITE, &p, sizeof(p));
        }
        t->mode = MODE_NORMAL; t->dirty = true; return;
    }
    if (t->mode == MODE_DEVICE_NAME) {
        qtc_trim(t->input);
        if (t->input[0]) {
            qtc_ipc_device_action_payload p = {0};
            qtc_strlcpy(p.text, t->input, sizeof(p.text));
            (void)qtc_ipc_send(t->fd, QTC_IPC_DEVICE_SET_NAME, &p, sizeof(p));
        }
        t->mode = MODE_NORMAL; t->dirty = true; return;
    }
    if (t->mode == MODE_TX_POWER) {
        qtc_trim(t->input);
        char *end = NULL;
        long power = strtol(t->input, &end, 10);
        if (t->input[0] && end != NULL && *end == 0 && power >= -20 && power <= 30) {
            qtc_ipc_device_action_payload p = {.value = (int)power};
            (void)qtc_ipc_send(t->fd, QTC_IPC_DEVICE_SET_TX_POWER, &p, sizeof(p));
        } else {
            qtc_strlcpy(t->status, "TX power must be a whole number in dBm", sizeof(t->status));
        }
        t->mode = MODE_NORMAL; t->dirty = true; return;
    }
    if (t->mode == MODE_THEME_PICKER) {
        t->state.settings.theme = t->theme_cursor;
        save_settings(t);
        t->mode = MODE_NORMAL;
        snprintf(t->status, sizeof(t->status), "Theme changed to %s", theme_name(t->state.settings.theme));
        t->dirty = true;
        return;
    }
    if (t->mode == MODE_PRESET_PICKER) {
        apply_preset(t);
        t->mode = MODE_NORMAL;
        t->dirty = true;
        return;
    }
    if (t->mode == MODE_INVITE_PICKER) {
        if (t->invite_count > 0) { t->mode = MODE_INVITE_REVIEW; t->confirm_action = false; t->modal_enter_block_until = qtc_now_millis() + 250; t->dirty = true; }
        return;
    }
    if (t->mode == MODE_INVITE_REVIEW) {
        if (qtc_now_millis() < t->modal_enter_block_until) return;
        if (t->confirm_action) send_invites(t); else { t->mode = MODE_INVITE_PICKER; t->dirty = true; }
        return;
    }
    if (t->mode == MODE_ROTATE_CONFIRM || t->mode == MODE_LEAVE_CONFIRM) {
        if (qtc_now_millis() < t->modal_enter_block_until) return;
        if (t->confirm_action) { qtc_ipc_channel_action_payload p = {.channel_index = t->selected_channel};
            (void)qtc_ipc_send(t->fd, t->mode == MODE_ROTATE_CONFIRM ? QTC_IPC_CHANNEL_ROTATE : QTC_IPC_CHANNEL_LEAVE, &p, sizeof(p)); }
        t->mode = MODE_NORMAL; t->confirm_action = false; t->dirty = true; return;
    }
    if (t->mode == MODE_INCOMING_INVITE) {
        if (qtc_now_millis() < t->modal_enter_block_until) return;
        int64_t id = t->incoming_invite_id;
        (void)qtc_ipc_send(t->fd, t->confirm_action ? QTC_IPC_INVITE_ACCEPT : QTC_IPC_INVITE_IGNORE, &id, sizeof(id));
        t->mode = MODE_NORMAL; t->confirm_action = false; t->dirty = true; return;
    }
    if (t->view == VIEW_MESSAGES) {
        open_selected(t);
        if (t->open_key[0]) start_input(t, MODE_COMPOSE);
    }
    else if (t->view == VIEW_CHANNELS && t->selected_channel >= 0) {
        t->open_kind = QTC_CONV_CHANNEL; snprintf(t->open_key, sizeof(t->open_key), "%d", t->selected_channel);
        t->selected_kind = 1; qtc_strlcpy(t->selected_key, t->open_key, sizeof(t->selected_key));
        set_view(t, VIEW_MESSAGES); open_selected(t); start_input(t, MODE_COMPOSE);
    }
}

static void escape_mode(tui_ctx *t) {
    if (t->mode == MODE_SEARCH) t->search[0] = 0;
    if (t->mode != MODE_NORMAL) { t->mode = MODE_NORMAL; t->confirm_action = false; t->dirty = true; return; }
    if (t->view != VIEW_MESSAGES) set_view(t, VIEW_MESSAGES);
}

static void move_channels(tui_ctx *t, int delta) {
    int slots[QTC_MAX_CHANNELS]; size_t n = 0;
    for (size_t i = 0; i < t->state.channel_count; i++) if (t->state.channels[i].configured) slots[n++] = t->state.channels[i].index;
    if (!n) { t->selected_channel = -1; return; }
    size_t pos = 0; while (pos < n && slots[pos] != t->selected_channel) pos++;
    if (pos == n) pos = 0;
    int next = (int)pos + delta;
    if (next < 0) next = 0;
    if ((size_t)next >= n) next = (int)n - 1;
    t->selected_channel = slots[next]; t->dirty = true;
}

static void move_nodes(tui_ctx *t, int delta) {
    int idx[QTC_MAX_CONTACTS]; size_t n = 0;
    for (size_t i = 0; i < t->state.contact_count; i++) if (t->state.contacts[i].node_type != QTC_NODE_PERSON) idx[n++] = (int)i;
    if (!n) { t->selected_node[0] = 0; return; }
    size_t pos = 0; while (pos < n && strcmp(t->state.contacts[idx[pos]].id, t->selected_node) != 0) pos++;
    if (pos == n) pos = 0;
    int next = (int)pos + delta;
    if (next < 0) next = 0;
    if ((size_t)next >= n) next = (int)n - 1;
    qtc_strlcpy(t->selected_node, t->state.contacts[idx[next]].id, sizeof(t->selected_node)); t->dirty = true;
}

static void first_pending_invite(tui_ctx *t) {
    for (size_t i = 0; i < t->state.invitation_count; i++) if (t->state.invitations[i].status == QTC_INVITE_PENDING) {
        t->incoming_invite_id = t->state.invitations[i].id; t->mode = MODE_INCOMING_INVITE; t->confirm_action = false;
        t->modal_enter_block_until = qtc_now_millis() + 250; t->dirty = true; return;
    }
    qtc_strlcpy(t->status, "No pending private-channel invitations", sizeof(t->status)); t->dirty = true;
}

static void normal_key(tui_ctx *t, unsigned char c) {
    if (c == 3) { leave_conversation(t); t->running = false; return; }
    if (c == 17) {
        int64_t now = qtc_now_millis();
        if (now - t->last_ctrl_q < 2000) { (void)qtc_ipc_send(t->fd, QTC_IPC_SHUTDOWN, NULL, 0); t->running = false; }
        else { t->last_ctrl_q = now; qtc_strlcpy(t->status, "Press Ctrl+Q again to stop the background core", sizeof(t->status)); t->dirty = true; }
        return;
    }
    if (t->mode == MODE_SEARCH || t->mode == MODE_COMPOSE ||
        t->mode == MODE_CREATE_CHANNEL || t->mode == MODE_JOIN_CHANNEL ||
        t->mode == MODE_ALIAS || t->mode == MODE_FAVORITE_GROUP ||
        t->mode == MODE_DEVICE_NAME || t->mode == MODE_TX_POWER) {
        if (c == 127 || c == 8) backspace_input(t); else if (c == '\r' || c == '\n') handle_enter(t); else if (c == 27) escape_mode(t); else if (c >= 32) append_input(t, (char)c);
        if (t->mode == MODE_SEARCH) qtc_strlcpy(t->search, t->input, sizeof(t->search));
        return;
    }
    if (t->mode == MODE_INVITE_PICKER) {
        if (c == '\r' || c == '\n') handle_enter(t);
        else if (c == 27) escape_mode(t);
        else if (c == ' ') { if (t->invite_cursor[0]) toggle_invite(t, t->invite_cursor); }
        else if (c == 127 || c == 8) { size_t n = strlen(t->invite_search); if (n) t->invite_search[utf8_previous_boundary(t->invite_search, n)] = 0; t->dirty = true; }
        else if (c >= 32) { size_t n = strlen(t->invite_search); if (n + 1 < sizeof(t->invite_search)) { t->invite_search[n] = (char)c; t->invite_search[n + 1] = 0; t->dirty = true; } }
        return;
    }
    if (t->mode == MODE_INVITE_REVIEW || t->mode == MODE_ROTATE_CONFIRM || t->mode == MODE_LEAVE_CONFIRM || t->mode == MODE_INCOMING_INVITE) {
        if (c == '\t' || c == 'h' || c == 'l') { t->confirm_action = !t->confirm_action; t->dirty = true; }
        else if (c == '\r' || c == '\n') handle_enter(t);
        else if (c == 27) escape_mode(t);
        return;
    }
    if (t->mode == MODE_THEME_PICKER || t->mode == MODE_PRESET_PICKER) {
        int max = t->mode == MODE_THEME_PICKER ? 4 : (int)QTC_ARRAY_LEN(RADIO_PRESETS);
        int *cursor = t->mode == MODE_THEME_PICKER ? &t->theme_cursor : &t->preset_cursor;
        if (c == 'j') { if (*cursor + 1 < max) (*cursor)++; t->dirty = true; }
        else if (c == 'k') { if (*cursor > 0) (*cursor)--; t->dirty = true; }
        else if (c >= '1' && c < '1' + max) { *cursor = c - '1'; handle_enter(t); }
        else if (c == '\r' || c == '\n') handle_enter(t);
        else if (c == 27) escape_mode(t);
        return;
    }
    if (c == 27) { escape_mode(t); return; }
    if (c == '\r' || c == '\n') { handle_enter(t); return; }
    if (t->view == VIEW_MESSAGES) {
        if (c == 'j') move_menu(t, 1); else if (c == 'k') move_menu(t, -1);
        else if (c == '/') { t->mode = MODE_SEARCH; qtc_strlcpy(t->input, t->search, sizeof(t->input)); t->input_len = strlen(t->input); t->dirty = true; }
        else if (c == 'm' && t->open_key[0]) start_input(t, MODE_COMPOSE);
        else if (c == 'f' && (t->selected_kind == 2 || t->selected_kind == 3)) {
            qtc_contact *ct = find_contact(t, t->selected_key); if (ct) { qtc_ipc_favorite_payload p = {.favorite = !ct->favorite};
                qtc_strlcpy(p.contact_id, ct->id, sizeof(p.contact_id)); qtc_strlcpy(p.group, ct->favorite_group[0] ? ct->favorite_group : "Favorites", sizeof(p.group));
                (void)qtc_ipc_send(t->fd, QTC_IPC_SET_FAVORITE, &p, sizeof(p));
                snprintf(t->status, sizeof(t->status), "%s %s Favorites", contact_name(ct), p.favorite ? "added to" : "removed from");
                t->dirty = true; }
        }
        else if (c == 'e' && (t->selected_kind == 2 || t->selected_kind == 3)) {
            qtc_contact *ct = find_contact(t, t->selected_key);
            if (ct != NULL) {
                start_input(t, MODE_ALIAS);
                qtc_strlcpy(t->input, ct->alias, sizeof(t->input));
                t->input_len = strlen(t->input);
            }
        }
        else if (c == 'g' && (t->selected_kind == 2 || t->selected_kind == 3)) {
            qtc_contact *ct = find_contact(t, t->selected_key);
            if (ct != NULL) {
                start_input(t, MODE_FAVORITE_GROUP);
                qtc_strlcpy(t->input, ct->favorite_group[0] ? ct->favorite_group : "Favorites", sizeof(t->input));
                t->input_len = strlen(t->input);
            }
        }
        else if (c == 's') set_view(t, VIEW_SETTINGS);
    } else if (t->view == VIEW_CHANNELS) {
        if (c == 'n') move_channels(t, 1); else if (c == 'p') move_channels(t, -1);
        else if (c == 'c') start_input(t, MODE_CREATE_CHANNEL);
        else if (c == 'j') start_input(t, MODE_JOIN_CHANNEL);
        else if (c == 'i' && t->selected_channel >= 0) { t->mode = MODE_INVITE_PICKER; t->invite_count = 0; t->invite_search[0] = 0; t->invite_cursor[0] = 0; t->dirty = true; }
        else if (c == 'r' && t->selected_channel >= 0) { t->mode = MODE_ROTATE_CONFIRM; t->confirm_action = false; t->modal_enter_block_until = qtc_now_millis() + 250; t->dirty = true; }
        else if (c == 'd' && t->selected_channel >= 0) { t->mode = MODE_LEAVE_CONFIRM; t->confirm_action = false; t->modal_enter_block_until = qtc_now_millis() + 250; t->dirty = true; }
        else if (c == 'v') first_pending_invite(t);
    } else if (t->view == VIEW_NODES) {
        if (c == 'j') move_nodes(t, 1); else if (c == 'k') move_nodes(t, -1);
    } else if (t->view == VIEW_SETTINGS) {
        if (c == '1') { t->state.settings.desktop_notifications = !t->state.settings.desktop_notifications; save_settings(t); t->dirty = true; }
        else if (c == '2') { t->state.settings.sound_enabled = !t->state.settings.sound_enabled; save_settings(t); t->dirty = true; }
        else if (c == '3') { t->state.settings.notify_direct = !t->state.settings.notify_direct; save_settings(t); t->dirty = true; }
        else if (c == '4') { t->state.settings.notify_channel = !t->state.settings.notify_channel; save_settings(t); t->dirty = true; }
        else if (c == '5') cycle_theme(t, 1);
        else if (c == 't') { t->theme_cursor = t->state.settings.theme; t->mode = MODE_THEME_PICKER; t->dirty = true; }
        else if (c == '6') { t->state.settings.banner_enabled = !t->state.settings.banner_enabled; save_settings(t); t->dirty = true; }
        else if (c == '7') { t->state.settings.suppress_open_conversation = !t->state.settings.suppress_open_conversation; save_settings(t); t->dirty = true; }
        else if (c == '8') { t->state.settings.retry_unconfirmed = !t->state.settings.retry_unconfirmed; save_settings(t); t->dirty = true; }
        else if (c == '9') { t->state.settings.reset_stale_route = !t->state.settings.reset_stale_route; save_settings(t); t->dirty = true; }
        else if (c == '0') { t->state.settings.show_signal = !t->state.settings.show_signal; save_settings(t); t->dirty = true; }
        else if (c == ',' && t->state.settings.stored_poll_seconds > 1) { t->state.settings.stored_poll_seconds--; save_settings(t); t->dirty = true; }
        else if (c == '.' && t->state.settings.stored_poll_seconds < 3600) { t->state.settings.stored_poll_seconds++; save_settings(t); t->dirty = true; }
        else if (c == '[' && t->state.settings.max_direct_attempts > 1) { t->state.settings.max_direct_attempts--; save_settings(t); t->dirty = true; }
        else if (c == ']' && t->state.settings.max_direct_attempts < 4) { t->state.settings.max_direct_attempts++; save_settings(t); t->dirty = true; }
        else if (c == 'd') {
            start_input(t, MODE_DEVICE_NAME);
            qtc_strlcpy(t->input, t->state.radio_name, sizeof(t->input));
            t->input_len = strlen(t->input);
        }
        else if (c == 'p') {
            start_input(t, MODE_TX_POWER);
            snprintf(t->input, sizeof(t->input), "%d", t->state.radio_tx_power);
            t->input_len = strlen(t->input);
        }
        else if (c == 'z' || c == 'x') {
            qtc_ipc_device_action_payload p = {.flag = c == 'x'};
            const char *sending = p.flag ?
                                  "Sending flood advertisement — waiting for radio confirmation..." :
                                  "Sending 0-hop advertisement — waiting for radio confirmation...";
            if (qtc_ipc_send(t->fd, QTC_IPC_DEVICE_ADVERTISE,
                             &p, sizeof(p)) == 0) {
                qtc_strlcpy(t->status, sending, sizeof(t->status));
                t->advert_feedback_pending = true;
                set_action_feedback(t, sending, FEEDBACK_INFO, 8000);
            } else {
                const char *failed = "Could not send advertisement request to the QTC core";
                qtc_strlcpy(t->status, failed, sizeof(t->status));
                t->advert_feedback_pending = false;
                set_action_feedback(t, failed, FEEDBACK_ERROR, 7000);
            }
        }
        else if (c == 'c') (void)qtc_ipc_send(t->fd, QTC_IPC_DEVICE_COPY_CARD, NULL, 0);
        else if (c == 'o') { t->preset_cursor = 0; t->mode = MODE_PRESET_PICKER; t->dirty = true; }
        else if (c == 'y') (void)qtc_ipc_send(t->fd, QTC_IPC_DEVICE_SYNC_MESSAGES, NULL, 0);
        else if (c == 'r') (void)qtc_ipc_send(t->fd, QTC_IPC_DEVICE_RECONNECT, NULL, 0);
        else if (c == 'n') {
            int rc = qtc_notify_desktop("QTC notification test", "Desktop notifications are working");
            qtc_strlcpy(t->status, rc == 0 ? "Desktop notification test sent" : "No desktop notification helper is available", sizeof(t->status));
            t->dirty = true;
        } else if (c == 'a') {
            int rc = qtc_notify_sound();
            qtc_strlcpy(t->status, rc == 0 ? "Notification sound test started" : "No supported sound player or sound file is available", sizeof(t->status));
            t->dirty = true;
        }
    }
}

static void special_key(tui_ctx *t, const char *seq) {
    if (strcmp(seq, "\x1b[A") == 0) {
        if (t->mode == MODE_INVITE_PICKER) move_invite_cursor(t, -1);
        else if (t->mode == MODE_THEME_PICKER) { if (t->theme_cursor > 0) t->theme_cursor--; t->dirty = true; }
        else if (t->mode == MODE_PRESET_PICKER) { if (t->preset_cursor > 0) t->preset_cursor--; t->dirty = true; }
        else if (t->view == VIEW_MESSAGES) move_menu(t, -1);
        else if (t->view == VIEW_CHANNELS) move_channels(t, -1);
        else if (t->view == VIEW_NODES) move_nodes(t, -1);
    } else if (strcmp(seq, "\x1b[B") == 0) {
        if (t->mode == MODE_INVITE_PICKER) move_invite_cursor(t, 1);
        else if (t->mode == MODE_THEME_PICKER) { if (t->theme_cursor < 3) t->theme_cursor++; t->dirty = true; }
        else if (t->mode == MODE_PRESET_PICKER) { if ((size_t)(t->preset_cursor + 1) < QTC_ARRAY_LEN(RADIO_PRESETS)) t->preset_cursor++; t->dirty = true; }
        else if (t->view == VIEW_MESSAGES) move_menu(t, 1);
        else if (t->view == VIEW_CHANNELS) move_channels(t, 1);
        else if (t->view == VIEW_NODES) move_nodes(t, 1);
    } else if (strcmp(seq, "\x1b[C") == 0 || strcmp(seq, "\x1b[D") == 0) {
        if (t->mode == MODE_INVITE_REVIEW || t->mode == MODE_ROTATE_CONFIRM ||
            t->mode == MODE_LEAVE_CONFIRM || t->mode == MODE_INCOMING_INVITE) {
            t->confirm_action = !t->confirm_action;
            t->dirty = true;
        }
    } else if (strcmp(seq, "\x1b[5~") == 0) {
        if (t->view == VIEW_MESSAGES && t->open_key[0]) {
            t->history_scroll += (size_t)(t->height > 12 ? (t->height - 8) / 2 : 4);
            t->dirty = true;
        }
    } else if (strcmp(seq, "\x1b[6~") == 0) {
        size_t step = (size_t)(t->height > 12 ? (t->height - 8) / 2 : 4);
        t->history_scroll = t->history_scroll > step ? t->history_scroll - step : 0;
        t->dirty = true;
    } else if (strcmp(seq, "\x1b[12~") == 0 || strcmp(seq, "\x1bOQ") == 0) {
        if (t->view == VIEW_MESSAGES && (t->selected_kind == 2 || t->selected_kind == 3)) {
            qtc_contact *ct = find_contact(t, t->selected_key);
            if (ct != NULL) {
                start_input(t, MODE_ALIAS);
                qtc_strlcpy(t->input, ct->alias, sizeof(t->input));
                t->input_len = strlen(t->input);
            }
        }
    } else if (strcmp(seq, "\x1b[14~") == 0 || strcmp(seq, "\x1bOS") == 0) {
        set_view(t, t->view == VIEW_SETTINGS ? VIEW_MESSAGES : VIEW_SETTINGS);
    } else if (strcmp(seq, "\x1b[15~") == 0) {
        (void)qtc_ipc_send(t->fd, QTC_IPC_DEVICE_RECONNECT, NULL, 0);
    } else if (strcmp(seq, "\x1b[17~") == 0) {
        set_view(t, t->view == VIEW_CHANNELS ? VIEW_MESSAGES : VIEW_CHANNELS);
    } else if (strcmp(seq, "\x1b[18~") == 0) {
        set_view(t, t->view == VIEW_NODES ? VIEW_MESSAGES : VIEW_NODES);
    } else if (strcmp(seq, "\x1b[19~") == 0) {
        leave_conversation(t);
        t->running = false;
    }
}

static void process_input(tui_ctx *t, const uint8_t *data, size_t len) {
    size_t i = 0;
    while (i < len) {
        if (data[i] == 27) {
            size_t remain = len - i, n = remain < sizeof(t->escape_buf) - 1 ? remain : sizeof(t->escape_buf) - 1;
            memcpy(t->escape_buf, data + i, n); t->escape_buf[n] = 0;
            const char *known[] = {"\x1b[A", "\x1b[B", "\x1b[C", "\x1b[D", "\x1b[5~", "\x1b[6~", "\x1b[12~", "\x1bOQ", "\x1b[14~", "\x1bOS", "\x1b[15~", "\x1b[17~", "\x1b[18~", "\x1b[19~"};
            bool matched = false;
            for (size_t k = 0; k < QTC_ARRAY_LEN(known); k++) if (strncmp(t->escape_buf, known[k], strlen(known[k])) == 0) {
                special_key(t, known[k]); i += strlen(known[k]); matched = true; break;
            }
            if (!matched) { normal_key(t, 27); i++; }
        } else { normal_key(t, data[i++]); }
    }
}

/* Styled, UTF-8-aware terminal framebuffer. Frames use absolute cursor addressing,
 * so rendering remains correct even when the terminal is in raw mode with OPOST off. */
typedef enum {
    UI_NORMAL = 0,
    UI_HEADER,
    UI_BORDER,
    UI_SECTION,
    UI_MUTED,
    UI_SELECTED,
    UI_UNREAD,
    UI_OUTGOING,
    UI_INCOMING,
    UI_STATUS,
    UI_INPUT,
    UI_ACCENT,
    UI_DANGER,
    UI_SUCCESS,
    UI_STYLE_COUNT
} ui_style;

typedef struct {
    const char *name;
    const char *sgr[UI_STYLE_COUNT];
} ui_theme;

static const ui_theme THEMES[4] = {
    {
        .name = "Green Phosphor",
        .sgr = {
            "\x1b[0;32;40m", "\x1b[1;30;42m", "\x1b[0;32;40m",
            "\x1b[1;92;40m", "\x1b[2;32;40m", "\x1b[1;30;102m",
            "\x1b[1;97;40m", "\x1b[1;92;40m", "\x1b[0;32;40m",
            "\x1b[0;30;42m", "\x1b[1;97;40m", "\x1b[1;92;40m",
            "\x1b[1;91;40m", "\x1b[1;92;40m"
        }
    },
    {
        .name = "Amber CRT",
        .sgr = {
            "\x1b[0;33;40m", "\x1b[1;30;43m", "\x1b[0;33;40m",
            "\x1b[1;93;40m", "\x1b[2;33;40m", "\x1b[1;30;103m",
            "\x1b[1;97;40m", "\x1b[1;93;40m", "\x1b[0;33;40m",
            "\x1b[0;30;43m", "\x1b[1;97;40m", "\x1b[1;93;40m",
            "\x1b[1;91;40m", "\x1b[1;93;40m"
        }
    },
    {
        .name = "Midnight BBS",
        .sgr = {
            "\x1b[0;97;44m", "\x1b[1;97;45m", "\x1b[0;96;44m",
            "\x1b[1;96;44m", "\x1b[2;37;44m", "\x1b[1;30;106m",
            "\x1b[1;93;44m", "\x1b[1;96;44m", "\x1b[0;97;44m",
            "\x1b[1;97;45m", "\x1b[1;93;44m", "\x1b[1;96;44m",
            "\x1b[1;91;44m", "\x1b[1;92;44m"
        }
    },
    {
        .name = "Mono TTY",
        .sgr = {
            "\x1b[0m", "\x1b[7m", "\x1b[2m", "\x1b[1m",
            "\x1b[2m", "\x1b[7m", "\x1b[1m", "\x1b[1m",
            "\x1b[0m", "\x1b[7m", "\x1b[1m", "\x1b[1m",
            "\x1b[1;4m", "\x1b[1m"
        }
    }
};

typedef struct {
    char bytes[12];
    uint8_t len;
    uint8_t width;
    uint8_t continuation;
    uint8_t style;
} screen_cell;

typedef struct {
    int w;
    int h;
    screen_cell *cells;
    int cursor_r;
    int cursor_c;
    bool cursor;
} screen;

static const ui_theme *active_theme(const tui_ctx *t) {
    int index = t->state.settings.theme;
    if (index < 0 || index >= (int)QTC_ARRAY_LEN(THEMES)) index = 0;
    return &THEMES[index];
}

static const char *theme_name(int index) {
    if (index < 0 || index >= (int)QTC_ARRAY_LEN(THEMES)) index = 0;
    return THEMES[index].name;
}

static void cell_space(screen_cell *cell, ui_style style) {
    memset(cell, 0, sizeof(*cell));
    cell->bytes[0] = ' ';
    cell->len = 1;
    cell->width = 1;
    cell->style = (uint8_t)style;
}

static int screen_init(screen *s, int w, int h) {
    memset(s, 0, sizeof(*s));
    s->w = w;
    s->h = h;
    s->cells = calloc((size_t)w * (size_t)h, sizeof(*s->cells));
    if (s->cells == NULL) return -1;
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) cell_space(&s->cells[i], UI_NORMAL);
    s->cursor_r = h;
    s->cursor_c = 1;
    return 0;
}

static void screen_free(screen *s) {
    free(s->cells);
    s->cells = NULL;
}

static screen_cell *screen_at(screen *s, int r, int c) {
    if (r < 0 || r >= s->h || c < 0 || c >= s->w) return NULL;
    return &s->cells[(size_t)r * (size_t)s->w + (size_t)c];
}

static void screen_clear_footprint(screen *s, int r, int c, ui_style style) {
    screen_cell *cell = screen_at(s, r, c);
    if (cell == NULL) return;
    if (cell->continuation && c > 0) {
        screen_cell *lead = screen_at(s, r, c - 1);
        if (lead != NULL && lead->width == 2) cell_space(lead, style);
    } else if (cell->width == 2 && c + 1 < s->w) {
        screen_cell *tail = screen_at(s, r, c + 1);
        if (tail != NULL) cell_space(tail, style);
    }
    cell_space(cell, style);
}

static void screen_fill(screen *s, int r, int left, int width, ui_style style) {
    if (r < 0 || r >= s->h || width <= 0) return;
    if (left < 0) { width += left; left = 0; }
    if (left + width > s->w) width = s->w - left;
    for (int c = left; c < left + width; c++) cell_space(screen_at(s, r, c), style);
}

static size_t decode_char(const char *text, size_t remaining, wchar_t *wc) {
    mbstate_t state;
    memset(&state, 0, sizeof(state));
    size_t n = mbrtowc(wc, text, remaining, &state);
    if (n == (size_t)-1 || n == (size_t)-2 || n == 0) {
        *wc = L'?';
        return 1;
    }
    return n;
}

static int text_width(const char *text) {
    int width = 0;
    const char *p = text;
    size_t remaining = strlen(text);
    while (remaining > 0) {
        wchar_t wc;
        size_t n = decode_char(p, remaining, &wc);
        int cw = wcwidth(wc);
        if (cw < 0) cw = 1;
        width += cw;
        p += n;
        remaining -= n;
    }
    return width;
}

static int screen_put_text(screen *s, int r, int c, int max_cells, const char *text, ui_style style) {
    if (r < 0 || r >= s->h || c >= s->w || max_cells <= 0 || text == NULL) return c;
    int start = c;
    int limit = c + max_cells;
    if (limit > s->w - 1) limit = s->w - 1;
    const char *p = text;
    size_t remaining = strlen(text);
    while (remaining > 0 && c < limit) {
        wchar_t wc;
        size_t n = decode_char(p, remaining, &wc);
        int cw = wcwidth(wc);
        if (cw < 0) { wc = L'?'; cw = 1; n = 1; }
        if (cw == 0) {
            int lead_col = c - 1;
            while (lead_col >= start) {
                screen_cell *lead = screen_at(s, r, lead_col);
                if (lead != NULL && !lead->continuation) {
                    if ((size_t)lead->len + n < sizeof(lead->bytes)) {
                        memcpy(lead->bytes + lead->len, p, n);
                        lead->len = (uint8_t)(lead->len + n);
                    }
                    break;
                }
                lead_col--;
            }
            p += n;
            remaining -= n;
            continue;
        }
        if (cw > 2) cw = 1;
        if (c + cw > limit) break;
        screen_clear_footprint(s, r, c, style);
        screen_cell *cell = screen_at(s, r, c);
        if (cell == NULL) break;
        memset(cell, 0, sizeof(*cell));
        if (n >= sizeof(cell->bytes)) { cell->bytes[0] = '?'; cell->len = 1; cell->width = 1; }
        else { memcpy(cell->bytes, p, n); cell->len = (uint8_t)n; cell->width = (uint8_t)cw; }
        cell->style = (uint8_t)style;
        if (cw == 2) {
            screen_cell *tail = screen_at(s, r, c + 1);
            if (tail != NULL) {
                memset(tail, 0, sizeof(*tail));
                tail->continuation = 1;
                tail->style = (uint8_t)style;
            }
        }
        c += cw;
        p += n;
        remaining -= n;
    }
    return c;
}

static int screen_put_fmt(screen *s, int r, int c, int max_cells, ui_style style, const char *fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return screen_put_text(s, r, c, max_cells, buf, style);
}

static void screen_hline(screen *s, int r, int left, int width, char ch, ui_style style) {
    if (width <= 0) return;
    char one[2] = {ch, 0};
    for (int c = left; c < left + width && c < s->w - 1; c++) (void)screen_put_text(s, r, c, 1, one, style);
}

static void screen_vline(screen *s, int c, int top, int height, char ch, ui_style style) {
    char one[2] = {ch, 0};
    for (int r = top; r < top + height && r < s->h; r++) (void)screen_put_text(s, r, c, 1, one, style);
}

static void screen_box(screen *s, int top, int left, int height, int width, const char *title) {
    if (width > s->w - 2) width = s->w - 2;
    if (height > s->h - 2) height = s->h - 2;
    if (width < 8 || height < 4) return;
    for (int r = top; r < top + height; r++) screen_fill(s, r, left, width, UI_NORMAL);
    screen_hline(s, top, left, width, '-', UI_BORDER);
    screen_hline(s, top + height - 1, left, width, '-', UI_BORDER);
    screen_vline(s, left, top, height, '|', UI_BORDER);
    screen_vline(s, left + width - 1, top, height, '|', UI_BORDER);
    (void)screen_put_text(s, top, left, 1, "+", UI_BORDER);
    (void)screen_put_text(s, top, left + width - 1, 1, "+", UI_BORDER);
    (void)screen_put_text(s, top + height - 1, left, 1, "+", UI_BORDER);
    (void)screen_put_text(s, top + height - 1, left + width - 1, 1, "+", UI_BORDER);
    if (title != NULL) screen_put_fmt(s, top, left + 2, width - 4, UI_SECTION, " %s ", title);
}

static ui_style roster_style(const qtc_roster_row *row, bool selected) {
    if (selected) return UI_SELECTED;
    if (row->kind == 4) return row->label[0] == ' ' ? UI_MUTED : UI_SECTION;
    if (row->label[0] == '!') return UI_UNREAD;
    return UI_NORMAL;
}

static void render_roster_row(screen *s, int row, int split, const qtc_roster_row *rr, bool selected) {
    ui_style style = roster_style(rr, selected);
    screen_fill(s, row, 0, split, style);
    if (rr->kind == 4) {
        screen_put_text(s, row, 2, split - 4, rr->label, style);
    } else {
        screen_put_text(s, row, 1, 1, selected ? ">" : " ", style);
        screen_put_text(s, row, 3, split - 5, rr->label, style);
    }
}

static const char *message_status_label(qtc_message_status status) {
    switch (status) {
        case QTC_MSG_QUEUED: return "queued";
        case QTC_MSG_SENDING: return "sending";
        case QTC_MSG_SENT: return "sent";
        case QTC_MSG_DELIVERED: return "delivered";
        case QTC_MSG_UNCONFIRMED: return "unconfirmed";
        case QTC_MSG_FAILED: return "failed";
        default: return "";
    }
}

typedef struct {
    const qtc_message *first;
    char text[QTC_MAX_TEXT];
    qtc_message_status status;
    int part_total;
    int part_count;
} logical_message_view;

static const char *message_logical_key(const qtc_message *m) {
    return m->logical_key[0] ? m->logical_key : m->message_key;
}

static size_t collect_logical_messages(tui_ctx *t, size_t *indices, size_t max) {
    size_t count = 0;
    for (size_t i = 0; i < t->state.message_count && count < max; i++) {
        const qtc_message *m = &t->state.messages[i];
        if (m->conversation_kind != t->open_kind ||
            strcmp(m->conversation_key, t->open_key) != 0) continue;
        const char *key = message_logical_key(m);
        bool seen = false;
        for (size_t j = 0; j < count; j++) {
            if (strcmp(message_logical_key(&t->state.messages[indices[j]]), key) == 0) {
                seen = true;
                break;
            }
        }
        if (!seen) indices[count++] = i;
    }
    return count;
}

static void load_logical_message(tui_ctx *t, size_t index, logical_message_view *view) {
    memset(view, 0, sizeof(*view));
    view->first = &t->state.messages[index];
    view->status = view->first->status;
    view->part_total = view->first->part_total > 0 ? view->first->part_total : 1;
    view->part_count = 1;
    if (qtc_message_assemble(t->state.messages, t->state.message_count,
                             message_logical_key(view->first), view->text,
                             sizeof(view->text), &view->part_total,
                             &view->part_count, &view->status) < 0) {
        qtc_strlcpy(view->text, view->first->text, sizeof(view->text));
    }
}

static size_t wrapped_segment(const char *text, size_t offset, int max_cells,
                              char *out, size_t out_len, size_t *next_offset) {
    size_t length = strlen(text);
    while (offset < length && text[offset] == ' ') offset++;
    if (offset >= length) {
        if (out_len) out[0] = 0;
        *next_offset = length;
        return 0;
    }
    size_t pos = offset;
    size_t last_space = SIZE_MAX;
    int cells = 0;
    while (pos < length) {
        if (text[pos] == '\n') break;
        wchar_t wc;
        size_t n = decode_char(text + pos, length - pos, &wc);
        int cw = wcwidth(wc);
        if (cw < 0) cw = 1;
        if (cells + cw > max_cells) break;
        if (wc == L' ' || wc == L'\t') last_space = pos;
        cells += cw;
        pos += n;
    }
    size_t end = pos;
    size_t next = pos;
    if (pos < length && text[pos] == '\n') {
        next = pos + 1;
    } else if (pos < length && last_space != SIZE_MAX && last_space > offset) {
        end = last_space;
        next = last_space + 1;
    } else if (end == offset) {
        wchar_t wc;
        size_t n = decode_char(text + offset, length - offset, &wc);
        (void)wc;
        end = offset + n;
        next = end;
    }
    while (end > offset && (text[end - 1] == ' ' || text[end - 1] == '\t')) end--;
    size_t bytes = end - offset;
    if (out_len > 0) {
        if (bytes >= out_len) bytes = out_len - 1;
        memcpy(out, text + offset, bytes);
        out[bytes] = 0;
    }
    *next_offset = next;
    return bytes;
}

static int wrapped_line_count(const char *text, int width) {
    if (text == NULL || *text == 0) return 1;
    size_t offset = 0;
    int lines = 0;
    char discard[QTC_MAX_TEXT];
    while (offset < strlen(text)) {
        size_t next = offset;
        (void)wrapped_segment(text, offset, width, discard, sizeof(discard), &next);
        lines++;
        if (next <= offset) break;
        offset = next;
    }
    return lines > 0 ? lines : 1;
}

static void render_messages(tui_ctx *t, screen *s) {
    int split = t->width / 3;
    if (split < 30) split = 30;
    if (split > 44) split = 44;
    int footer_top = t->height - 2;
    screen_vline(s, split, 2, footer_top - 2, '|', UI_BORDER);

    qtc_roster roster;
    menu_item items[QTC_MAX_CONTACTS + QTC_MAX_CHANNELS];
    size_t item_count = build_menu(t, items, QTC_ARRAY_LEN(items), &roster);
    (void)selected_menu_pos(t, items, item_count);

    int row = 2;
    for (size_t i = 0; i < roster.pinned_count && row < footer_top; i++, row++) {
        qtc_roster_row *rr = &roster.pinned[i];
        bool selected = false;
        if (rr->kind == 1) {
            char key[16];
            snprintf(key, sizeof(key), "%d", t->state.channels[rr->source_index].index);
            selected = t->selected_kind == 1 && strcmp(t->selected_key, key) == 0;
        } else if (rr->kind == 2) {
            selected = t->selected_kind == 2 && strcmp(t->selected_key, t->state.contacts[rr->source_index].id) == 0;
        }
        render_roster_row(s, row, split, rr, selected);
    }

    int viewport = footer_top - row;
    if (viewport < 0) viewport = 0;
    size_t selected_scroll_row = 0;
    for (size_t i = 0; i < roster.scrollable_count; i++) {
        if (roster.scrollable[i].kind == 3 && t->selected_kind == 3 &&
            strcmp(t->selected_key, t->state.contacts[roster.scrollable[i].source_index].id) == 0) {
            selected_scroll_row = i;
            break;
        }
    }
    qtc_roster_clamp(selected_scroll_row, roster.scrollable_count, (size_t)viewport, &t->contact_scroll);
    for (int v = 0; v < viewport; v++) {
        size_t i = t->contact_scroll + (size_t)v;
        if (i >= roster.scrollable_count) break;
        qtc_roster_row *rr = &roster.scrollable[i];
        bool selected = rr->kind == 3 && t->selected_kind == 3 &&
                        strcmp(t->selected_key, t->state.contacts[rr->source_index].id) == 0;
        render_roster_row(s, row + v, split, rr, selected);
    }

    int right = split + 2;
    int rw = t->width - right - 1;
    char title[256] = "Select a conversation";
    char subtitle[256] = "Use Up/Down and Enter to open a chat";
    if (t->open_kind == QTC_CONV_CONTACT) {
        qtc_contact *ct = find_contact(t, t->open_key);
        if (ct != NULL) {
            snprintf(title, sizeof(title), "%s", contact_name(ct));
            if (ct->route_known) snprintf(subtitle, sizeof(subtitle), "%s - %d hop%s - key %.12s", qtc_node_type_label(ct->node_type), ct->route_hops, ct->route_hops == 1 ? "" : "s", ct->prefix);
            else snprintf(subtitle, sizeof(subtitle), "%s - flood - key %.12s", qtc_node_type_label(ct->node_type), ct->prefix);
        }
    } else if (t->open_kind == QTC_CONV_CHANNEL) {
        qtc_channel *ch = find_channel(t, atoi(t->open_key));
        if (ch != NULL) {
            snprintf(title, sizeof(title), "# %s", ch->name);
            snprintf(subtitle, sizeof(subtitle), "%s channel - radio slot %d", ch->is_private ? "private" : "public", ch->index);
        }
    }
    screen_put_text(s, 2, right, rw, title, UI_SECTION);
    screen_put_text(s, 3, right, rw, subtitle, UI_MUTED);
    screen_hline(s, 4, right, rw, '-', UI_BORDER);

    int first_message_row = 5;
    int message_rows = footer_top - first_message_row;
    size_t logical_indices[768];
    size_t logical_count = collect_logical_messages(t, logical_indices,
                                                     QTC_ARRAY_LEN(logical_indices));
    int text_area = rw - 2;
    if (text_area < 12) text_area = 12;
    size_t total_lines = 0;
    for (size_t i = 0; i < logical_count; i++) {
        logical_message_view view;
        load_logical_message(t, logical_indices[i], &view);
        total_lines += 1U + (size_t)wrapped_line_count(view.text, text_area);
    }
    size_t max_scroll = total_lines > (size_t)message_rows ?
                        total_lines - (size_t)message_rows : 0;
    if (t->history_scroll > max_scroll) t->history_scroll = max_scroll;
    size_t window_start = total_lines > (size_t)message_rows + t->history_scroll ?
                          total_lines - (size_t)message_rows - t->history_scroll : 0;
    size_t window_end = window_start + (size_t)message_rows;
    size_t visual_line = 0;
    int draw_row = first_message_row;
    for (size_t i = 0; i < logical_count && draw_row < footer_top; i++) {
        logical_message_view view;
        load_logical_message(t, logical_indices[i], &view);
        const qtc_message *m = view.first;
        struct tm tmv;
        time_t ts = (time_t)m->created_at;
        localtime_r(&ts, &tmv);
        char meta[256];
        const char *who = m->direction == QTC_MSG_OUTGOING ? "YOU" : "THEM";
        const char *status = m->direction == QTC_MSG_OUTGOING ?
                             message_status_label(view.status) : "";
        if (view.part_total > 1 && view.part_count < view.part_total) {
            snprintf(meta, sizeof(meta), "%02d:%02d  %s  [%d/%d parts]%s%s",
                     tmv.tm_hour, tmv.tm_min, who, view.part_count, view.part_total,
                     status[0] ? "  " : "", status);
        } else if (t->state.settings.show_signal && m->direction == QTC_MSG_INCOMING) {
            snprintf(meta, sizeof(meta), "%02d:%02d  %s  [SNR %.1f dB / path %d]",
                     tmv.tm_hour, tmv.tm_min, who,
                     (double)m->snr_quarter_db / 4.0, m->path_len);
        } else {
            if (status[0])
                snprintf(meta, sizeof(meta), "%02d:%02d  %s  [%s]",
                         tmv.tm_hour, tmv.tm_min, who, status);
            else
                snprintf(meta, sizeof(meta), "%02d:%02d  %s",
                         tmv.tm_hour, tmv.tm_min, who);
        }
        if (visual_line >= window_start && visual_line < window_end && draw_row < footer_top) {
            ui_style meta_style = view.status == QTC_MSG_FAILED ? UI_DANGER : UI_MUTED;
            screen_put_text(s, draw_row++, right, rw, meta, meta_style);
        }
        visual_line++;

        size_t offset = 0;
        int line_count = wrapped_line_count(view.text, text_area);
        for (int line = 0; line < line_count; line++) {
            char segment[QTC_MAX_TEXT];
            size_t next = offset;
            (void)wrapped_segment(view.text, offset, text_area, segment,
                                  sizeof(segment), &next);
            if (visual_line >= window_start && visual_line < window_end && draw_row < footer_top) {
                ui_style style = m->direction == QTC_MSG_OUTGOING ? UI_OUTGOING : UI_INCOMING;
                screen_put_text(s, draw_row++, right + 2, text_area, segment, style);
            }
            visual_line++;
            if (next <= offset) break;
            offset = next;
        }
    }
    if (logical_count == 0 && t->open_key[0])
        screen_put_text(s, first_message_row + 1, right, rw,
                        "No messages in this conversation yet.", UI_MUTED);
    if (t->history_scroll > 0)
        screen_put_fmt(s, 4, right + rw - 28, 28, UI_ACCENT,
                       "%zu newer line%s below", t->history_scroll,
                       t->history_scroll == 1 ? "" : "s");

    screen_fill(s, footer_top, 0, t->width - 1, UI_STATUS);
    if (t->banner_until > qtc_now_millis() && t->banner_title[0])
        screen_put_fmt(s, footer_top, 1, t->width - 3, UI_UNREAD,
                       "NEW  %s: %s", t->banner_title, t->banner_body);
    else
        screen_put_text(s, footer_top, 1, t->width - 3, "Enter Write  f Favorite  F2 Alias  g Group  F4 Settings  F5 Reconnect  F6 Channels  F7 Network  F8 Detach", UI_STATUS);
    screen_fill(s, t->height - 1, 0, t->width - 1, t->mode == MODE_SEARCH || t->mode == MODE_COMPOSE ? UI_INPUT : UI_NORMAL);
    if (t->mode == MODE_SEARCH) {
        screen_put_text(s, t->height - 1, 1, 8, "Search: ", UI_INPUT);
        screen_put_text(s, t->height - 1, 9, t->width - 11, t->input, UI_INPUT);
        s->cursor = true;
        s->cursor_r = t->height;
        s->cursor_c = 10 + text_width(t->input);
    } else if (t->mode == MODE_COMPOSE) {
        screen_put_text(s, t->height - 1, 1, 9, "Message: ", UI_INPUT);
        screen_put_text(s, t->height - 1, 10, t->width - 12, t->input, UI_INPUT);
        s->cursor = true;
        s->cursor_r = t->height;
        s->cursor_c = 11 + text_width(t->input);
    } else {
        if (t->banner_until > qtc_now_millis() && t->banner_title[0])
            screen_put_fmt(s, t->height - 1, 1, t->width - 3, UI_UNREAD,
                           "%s: %s", t->banner_title, t->banner_body);
        else
            screen_put_text(s, t->height - 1, 1, t->width - 3, t->status, UI_MUTED);
    }
}

static size_t pending_invites(tui_ctx *t) {
    size_t n = 0;
    for (size_t i = 0; i < t->state.invitation_count; i++) if (t->state.invitations[i].status == QTC_INVITE_PENDING) n++;
    return n;
}

static void render_page_header(screen *s, const char *title, const char *description) {
    screen_put_text(s, 2, 3, s->w - 6, title, UI_SECTION);
    screen_put_text(s, 3, 3, s->w - 6, description, UI_MUTED);
    screen_hline(s, 4, 2, s->w - 5, '-', UI_BORDER);
}

static void render_channels(tui_ctx *t, screen *s) {
    render_page_header(s, "CHANNELS", "Create, join, invite, rotate, or leave without exposing raw keys.");
    int row = 6;
    bool found = false;
    for (size_t i = 0; i < t->state.channel_count && row < t->height - 3; i++) {
        qtc_channel *ch = &t->state.channels[i];
        if (!ch->configured) continue;
        if (t->selected_channel < 0) t->selected_channel = ch->index;
        bool selected = ch->index == t->selected_channel;
        found = true;
        ui_style style = selected ? UI_SELECTED : (ch->unread ? UI_UNREAD : UI_NORMAL);
        screen_fill(s, row, 3, t->width - 7, style);
        screen_put_fmt(s, row, 4, t->width - 9, style, "%c  %-2d  %s", selected ? '>' : ' ', ch->index, ch->name);
        const char *type = ch->is_private ? "PRIVATE" : "PUBLIC";
        int type_width = text_width(type);
        screen_put_text(s, row, t->width - type_width - 5, type_width, type, style);
        if (ch->unread) screen_put_text(s, row, t->width - type_width - 12, 6, "NEW", style);
        row++;
    }
    if (!found) {
        t->selected_channel = -1;
        screen_put_text(s, row++, 5, t->width - 10, "No configured channels. Press c to create one or J to join an invitation.", UI_MUTED);
    }
    size_t pending = pending_invites(t);
    if (pending > 0) screen_put_fmt(s, row + 1, 5, t->width - 10, UI_UNREAD, "%zu pending private-channel invitation%s - press v to review", pending, pending == 1 ? "" : "s");
    screen_fill(s, t->height - 2, 0, t->width - 1, UI_STATUS);
    screen_put_text(s, t->height - 2, 1, t->width - 3, "c Create  j Join  i Invite  r Rotate  d Leave  n/p Select  Enter Write  v Pending  F6 Back", UI_STATUS);
    screen_put_text(s, t->height - 1, 1, t->width - 3, t->status, UI_MUTED);
}

static void render_nodes(tui_ctx *t, screen *s) {
    render_page_header(s, "NETWORK NODES", "Repeaters and infrastructure are separate from people you message.");
    int row = 6;
    bool any = false;
    for (size_t i = 0; i < t->state.contact_count && row < t->height - 3; i++) {
        qtc_contact *c = &t->state.contacts[i];
        if (c->node_type == QTC_NODE_PERSON) continue;
        any = true;
        if (!t->selected_node[0]) qtc_strlcpy(t->selected_node, c->id, sizeof(t->selected_node));
        bool selected = strcmp(t->selected_node, c->id) == 0;
        ui_style style = selected ? UI_SELECTED : UI_NORMAL;
        screen_fill(s, row, 3, t->width - 7, style);
        char route[32];
        if (!c->route_known) qtc_strlcpy(route, "flood", sizeof(route));
        else snprintf(route, sizeof(route), "%d hop%s", c->route_hops, c->route_hops == 1 ? "" : "s");
        screen_put_fmt(s, row, 4, t->width - 9, style, "%c  %-28s  %-10s  %-9s  key %.12s", selected ? '>' : ' ', contact_name(c), qtc_node_type_label(c->node_type), route, c->prefix);
        row++;
    }
    if (!any) screen_put_text(s, row, 5, t->width - 10, "No repeaters, rooms, sensors, or unknown nodes.", UI_MUTED);
    screen_fill(s, t->height - 2, 0, t->width - 1, UI_STATUS);
    screen_put_text(s, t->height - 2, 1, t->width - 3, "Up/Down Select  F7 Back  Esc Back", UI_STATUS);
    screen_put_text(s, t->height - 1, 1, t->width - 3, t->status, UI_MUTED);
}

static const char *onoff(bool value) { return value ? "ON" : "OFF"; }

static void setting_row(screen *s, int row, const char *key, const char *label, const char *value) {
    screen_put_text(s, row, 5, 4, key, UI_ACCENT);
    screen_put_text(s, row, 10, 42, label, UI_NORMAL);
    screen_put_text(s, row, 54, s->w - 59, value, strcmp(value, "OFF") == 0 ? UI_MUTED : UI_SUCCESS);
}

static void render_settings(tui_ctx *t, screen *s) {
    render_page_header(s, "SETTINGS", "Messaging, notification, display, and radio controls are saved per profile.");
    int row = 6;
    setting_row(s, row++, "1", "Desktop notifications", onoff(t->state.settings.desktop_notifications));
    setting_row(s, row++, "2", "Notification sound", onoff(t->state.settings.sound_enabled));
    setting_row(s, row++, "3", "Notify for direct messages", onoff(t->state.settings.notify_direct));
    setting_row(s, row++, "4", "Notify for channel messages", onoff(t->state.settings.notify_channel));
    setting_row(s, row++, "6", "In-terminal message banners", onoff(t->state.settings.banner_enabled));
    setting_row(s, row++, "7", "Suppress open-chat notifications", onoff(t->state.settings.suppress_open_conversation));
    setting_row(s, row++, "8", "Retry unconfirmed direct messages", onoff(t->state.settings.retry_unconfirmed));
    setting_row(s, row++, "9", "Reset stale route before retry", onoff(t->state.settings.reset_stale_route));
    setting_row(s, row++, "0", "Show SNR and path in history", onoff(t->state.settings.show_signal));
    if (row < t->height - 7) {
        screen_put_fmt(s, row++, 5, t->width - 10, UI_NORMAL,
                       "Theme [t]: %s", theme_name(t->state.settings.theme));
        screen_put_text(s, row++, 5, t->width - 10,
                        "Themes: Green Phosphor | Amber CRT | Midnight BBS | Mono TTY", UI_MUTED);
        screen_put_fmt(s, row++, 5, t->width - 10, UI_NORMAL,
                       "Stored-message poll [,/.]: %d seconds    Direct attempts [[/]]: %d",
                       t->state.settings.stored_poll_seconds,
                       t->state.settings.max_direct_attempts);
        screen_put_fmt(s, row++, 5, t->width - 10, UI_NORMAL,
                       "Radio [d name / p power]: %s    TX %d dBm (max %d)",
                       t->state.radio_name[0] ? t->state.radio_name : "unknown",
                       t->state.radio_tx_power, t->state.radio_max_tx_power);
        screen_put_fmt(s, row++, 5, t->width - 10, UI_MUTED,
                       "Device: %s", t->state.settings.serial_device[0] ?
                       t->state.settings.serial_device : "automatic");
        if (t->state.radio_freq > 0.0)
            screen_put_fmt(s, row++, 5, t->width - 10, UI_MUTED,
                           "Preset: %.4f MHz  BW %.1f kHz  SF %d  CR %d",
                           t->state.radio_freq, t->state.radio_bw,
                           t->state.radio_sf, t->state.radio_cr);
    }
    if (t->action_feedback_until > qtc_now_millis() &&
        t->action_feedback[0] != 0) {
        ui_style style = UI_ACCENT;
        if (t->action_feedback_level == FEEDBACK_SUCCESS) style = UI_SUCCESS;
        else if (t->action_feedback_level == FEEDBACK_ERROR) style = UI_DANGER;
        screen_fill(s, t->height - 3, 0, t->width - 1, style);
        screen_put_fmt(s, t->height - 3, 2, t->width - 4, style,
                       "ADVERT  %s", t->action_feedback);
    }
    screen_fill(s, t->height - 2, 0, t->width - 1, UI_STATUS);
    screen_put_text(s, t->height - 2, 1, t->width - 3,
                    "o First-connect preset  t Theme  d Name  p Power  z 0-hop advert  x Flood advert  c Copy card  y Sync  r/F5 Reconnect",
                    UI_STATUS);
    screen_put_text(s, t->height - 1, 1, t->width - 3, t->status, UI_MUTED);
}

static void render_modal(tui_ctx *t, screen *s) {
    int w = t->width > 90 ? 80 : t->width - 6;
    int h = 9;
    int top = (t->height - h) / 2;
    int left = (t->width - w) / 2;
    if (t->mode == MODE_ALIAS || t->mode == MODE_FAVORITE_GROUP ||
        t->mode == MODE_DEVICE_NAME || t->mode == MODE_TX_POWER) {
        const char *title = "EDIT VALUE";
        const char *prompt = "Value:";
        if (t->mode == MODE_ALIAS) { title = "CONTACT ALIAS"; prompt = "Alias (empty clears it):"; }
        else if (t->mode == MODE_FAVORITE_GROUP) { title = "FAVORITE GROUP"; prompt = "Favorite group:"; }
        else if (t->mode == MODE_DEVICE_NAME) { title = "RADIO NAME"; prompt = "New radio name (1-32 bytes):"; }
        else if (t->mode == MODE_TX_POWER) { title = "TX POWER"; prompt = "TX power in dBm:"; }
        screen_box(s, top, left, h, w, title);
        screen_put_text(s, top + 2, left + 3, w - 6, prompt, UI_NORMAL);
        screen_fill(s, top + 4, left + 3, w - 6, UI_INPUT);
        screen_put_text(s, top + 4, left + 4, 2, "> ", UI_INPUT);
        screen_put_text(s, top + 4, left + 6, w - 10, t->input, UI_INPUT);
        screen_put_text(s, top + 6, left + 3, w - 6,
                        "Enter saves. Esc cancels.", UI_MUTED);
        s->cursor = true;
        s->cursor_r = top + 5;
        s->cursor_c = left + 7 + text_width(t->input);
        return;
    }
    if (t->mode == MODE_CREATE_CHANNEL || t->mode == MODE_JOIN_CHANNEL) {
        const char *title = t->mode == MODE_CREATE_CHANNEL ? "CREATE PRIVATE CHANNEL" : "JOIN PRIVATE CHANNEL";
        screen_box(s, top, left, h, w, title);
        screen_put_text(s, top + 2, left + 3, w - 6, t->mode == MODE_CREATE_CHANNEL ? "Channel name (QTC generates a secure key):" : "Invitation URI, raw 32-character key, or Name:key:", UI_NORMAL);
        screen_fill(s, top + 4, left + 3, w - 6, UI_INPUT);
        screen_put_text(s, top + 4, left + 4, 2, "> ", UI_INPUT);
        screen_put_text(s, top + 4, left + 6, w - 10, t->input, UI_INPUT);
        screen_put_text(s, top + 6, left + 3, w - 6, "Enter confirms. Esc cancels.", UI_MUTED);
        s->cursor = true;
        s->cursor_r = top + 5;
        s->cursor_c = left + 7 + text_width(t->input);
        return;
    }
    if (t->mode == MODE_THEME_PICKER) {
        static const char *const themes[] = {
            "Green Phosphor", "Amber CRT", "Midnight BBS", "Mono TTY"
        };
        h = 13;
        top = (t->height - h) / 2;
        screen_box(s, top, left, h, w, "SELECT THEME");
        for (int i = 0; i < 4; i++) {
            ui_style style = i == t->theme_cursor ? UI_SELECTED : UI_NORMAL;
            screen_fill(s, top + 2 + i * 2, left + 3, w - 6, style);
            screen_put_fmt(s, top + 2 + i * 2, left + 4, w - 8, style,
                           "%d. %s%s", i + 1, themes[i],
                           i == t->state.settings.theme ? "  [current]" : "");
        }
        screen_put_text(s, top + h - 2, left + 3, w - 6,
                        "Up/Down or j/k selects. Enter applies. Esc cancels.", UI_MUTED);
        return;
    }
    if (t->mode == MODE_PRESET_PICKER) {
        h = 17;
        top = (t->height - h) / 2;
        screen_box(s, top, left, h, w, "FIRST-CONNECT RADIO PRESET");
        screen_put_text(s, top + 2, left + 3, w - 6,
                        "This changes the radio frequency and LoRa parameters.", UI_DANGER);
        for (size_t i = 0; i < QTC_ARRAY_LEN(RADIO_PRESETS); i++) {
            const radio_preset *preset = &RADIO_PRESETS[i];
            ui_style style = (int)i == t->preset_cursor ? UI_SELECTED : UI_NORMAL;
            screen_fill(s, top + 4 + (int)i * 2, left + 3, w - 6, style);
            screen_put_fmt(s, top + 4 + (int)i * 2, left + 4, w - 8, style,
                           "%zu. %-26s %.4f MHz  BW %.1f  SF%d  CR%d",
                           i + 1, preset->name, preset->freq_mhz, preset->bw_khz,
                           preset->sf, preset->cr);
        }
        screen_put_text(s, top + 13, left + 3, w - 6,
                        "Confirm the exact preset with your local MeshCore community.", UI_DANGER);
        screen_put_text(s, top + h - 2, left + 3, w - 6,
                        "Up/Down or j/k selects. Enter applies. Esc cancels.", UI_MUTED);
        return;
    }
    if (t->mode == MODE_INVITE_PICKER) {
        h = t->height - 6;
        if (h > 28) h = 28;
        top = (t->height - h) / 2;
        screen_box(s, top, left, h, w, "SELECT MESHCORE CONTACTS");
        qtc_channel *ch = find_channel(t, t->selected_channel);
        screen_put_fmt(s, top + 2, left + 3, w - 6, UI_NORMAL, "Invite to \"%s\"", ch ? ch->name : "channel");
        screen_put_fmt(s, top + 3, left + 3, w - 6, UI_INPUT, "Search: %s", t->invite_search);
        int indices[QTC_MAX_CONTACTS];
        size_t count = person_list(t, indices, QTC_ARRAY_LEN(indices));
        int pos = invite_cursor_pos(t, indices, count);
        int rows = h - 7;
        int start = pos >= rows ? pos - rows + 1 : 0;
        for (int r = 0; r < rows && (size_t)(start + r) < count; r++) {
            qtc_contact *c = &t->state.contacts[indices[start + r]];
            bool selected = start + r == pos;
            ui_style style = selected ? UI_SELECTED : UI_NORMAL;
            screen_fill(s, top + 5 + r, left + 2, w - 4, style);
            screen_put_fmt(s, top + 5 + r, left + 3, w - 6, style, "%c [%c] %s", selected ? '>' : ' ', invite_selected(t, c->id) ? 'x' : ' ', contact_name(c));
        }
        screen_put_fmt(s, top + h - 2, left + 3, w - 6, UI_MUTED, "Type search  Space select  Enter review  Esc cancel   Selected: %zu", t->invite_count);
        return;
    }
    if (t->mode == MODE_INVITE_REVIEW) {
        h = 11 + (int)t->invite_count;
        if (h > t->height - 4) h = t->height - 4;
        top = (t->height - h) / 2;
        screen_box(s, top, left, h, w, "REVIEW PRIVATE CHANNEL INVITATION");
        qtc_channel *ch = find_channel(t, t->selected_channel);
        if (t->invite_count == 1) {
            qtc_contact *ct = find_contact(t, t->invite_ids[0]);
            screen_put_fmt(s, top + 2, left + 3, w - 6, UI_NORMAL, "Invite %s to private channel \"%s\"?", ct ? contact_name(ct) : t->invite_ids[0], ch ? ch->name : "");
        } else {
            screen_put_fmt(s, top + 2, left + 3, w - 6, UI_NORMAL, "Send \"%s\" invitation to %zu contacts?", ch ? ch->name : "", t->invite_count);
        }
        screen_put_text(s, top + 3, left + 3, w - 6, "This sends each recipient the private channel key.", UI_DANGER);
        int recipient_row = top + 5;
        for (size_t i = 0; i < t->invite_count && recipient_row < top + h - 3; i++, recipient_row++) {
            qtc_contact *ct = find_contact(t, t->invite_ids[i]);
            screen_put_text(s, recipient_row, left + 5, w - 10, ct ? contact_name(ct) : t->invite_ids[i], UI_NORMAL);
        }
        screen_put_fmt(s, top + h - 2, left + 3, w - 6, UI_ACCENT, "%s Cancel       %s Send %zu invite%s", !t->confirm_action ? ">" : " ", t->confirm_action ? ">" : " ", t->invite_count, t->invite_count == 1 ? "" : "s");
        return;
    }
    if (t->mode == MODE_ROTATE_CONFIRM || t->mode == MODE_LEAVE_CONFIRM) {
        screen_box(s, top, left, h, w, t->mode == MODE_ROTATE_CONFIRM ? "ROTATE CHANNEL KEY" : "LEAVE CHANNEL");
        qtc_channel *ch = find_channel(t, t->selected_channel);
        screen_put_fmt(s, top + 2, left + 3, w - 6, UI_NORMAL, "%s \"%s\"?", t->mode == MODE_ROTATE_CONFIRM ? "Generate a new private key for" : "Leave", ch ? ch->name : "channel");
        screen_put_text(s, top + 3, left + 3, w - 6, t->mode == MODE_ROTATE_CONFIRM ? "Old invitations will stop working." : "Local message history will be preserved.", UI_DANGER);
        screen_put_fmt(s, top + 6, left + 3, w - 6, UI_ACCENT, "%s Cancel               %s %s", !t->confirm_action ? ">" : " ", t->confirm_action ? ">" : " ", t->mode == MODE_ROTATE_CONFIRM ? "Rotate" : "Leave");
        return;
    }
    if (t->mode == MODE_INCOMING_INVITE) {
        qtc_invitation *inv = find_invitation(t, t->incoming_invite_id);
        screen_box(s, top, left, h, w, "PRIVATE CHANNEL INVITATION");
        screen_put_text(s, top + 2, left + 3, w - 6, inv ? inv->channel_name : "Unknown channel", UI_SECTION);
        qtc_contact *ct = inv ? find_contact(t, inv->sender_contact_id) : NULL;
        screen_put_fmt(s, top + 3, left + 3, w - 6, UI_NORMAL, "From: %s", ct ? contact_name(ct) : (inv ? inv->sender_contact_id : "unknown"));
        screen_put_text(s, top + 4, left + 3, w - 6, "Joining writes the private key into a free radio channel slot.", UI_DANGER);
        screen_put_fmt(s, top + 6, left + 3, w - 6, UI_ACCENT, "%s Ignore               %s Join", !t->confirm_action ? ">" : " ", t->confirm_action ? ">" : " ");
    }
}

static void out_append(char *out, size_t cap, size_t *used, const char *data, size_t len) {
    if (*used >= cap || len == 0) return;
    if (len > cap - *used) len = cap - *used;
    memcpy(out + *used, data, len);
    *used += len;
}

static void out_fmt(char *out, size_t cap, size_t *used, const char *fmt, ...) {
    if (*used >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + *used, cap - *used, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    size_t written = (size_t)n;
    if (written > cap - *used) written = cap - *used;
    *used += written;
}

static void render(tui_ctx *t) {
    update_size(t);
    screen s;
    if (screen_init(&s, t->width, t->height) != 0) return;
    const ui_theme *theme = active_theme(t);

    screen_fill(&s, 0, 0, t->width - 1, UI_HEADER);
    screen_put_fmt(&s, 0, 1, t->width - 3, UI_HEADER, "QTC TERMINAL %s", QTC_VERSION);
    char connection[512];
    snprintf(connection, sizeof(connection), "%s  %s", t->state.radio_connected ? "CONNECTED" : "OFFLINE", t->state.radio_name[0] ? t->state.radio_name : "MeshCore companion");
    int connection_width = text_width(connection);
    if (connection_width < t->width - 24) screen_put_text(&s, 0, t->width - connection_width - 2, connection_width, connection, UI_HEADER);
    screen_hline(&s, 1, 0, t->width - 1, '=', UI_BORDER);

    if (t->width < 68 || t->height < 18) {
        screen_put_fmt(&s, 4, 3, t->width - 6, UI_DANGER, "Terminal is too small: %dx%d", t->width, t->height);
        screen_put_text(&s, 6, 3, t->width - 6, "QTC needs at least 68 columns by 18 rows.", UI_NORMAL);
        screen_put_text(&s, 8, 3, t->width - 6, "Resize the terminal; the interface will redraw automatically.", UI_MUTED);
    } else {
        if (t->view == VIEW_MESSAGES) render_messages(t, &s);
        else if (t->view == VIEW_CHANNELS) render_channels(t, &s);
        else if (t->view == VIEW_NODES) render_nodes(t, &s);
        else render_settings(t, &s);
        if (t->mode != MODE_NORMAL && t->mode != MODE_SEARCH && t->mode != MODE_COMPOSE) render_modal(t, &s);
    }

    size_t cap = (size_t)t->width * (size_t)t->height * 24U + (size_t)t->height * 40U + 512U;
    char *out = malloc(cap);
    if (out == NULL) { screen_free(&s); return; }
    size_t used = 0;
    out_append(out, cap, &used, "\x1b[?25l", strlen("\x1b[?25l"));
    for (int r = 0; r < t->height; r++) {
        out_fmt(out, cap, &used, "\x1b[%d;1H", r + 1);
        out_append(out, cap, &used, theme->sgr[UI_NORMAL], strlen(theme->sgr[UI_NORMAL]));
        out_append(out, cap, &used, "\x1b[2K", strlen("\x1b[2K"));
        int current_style = -1;
        for (int c = 0; c < t->width - 1; c++) {
            screen_cell *cell = screen_at(&s, r, c);
            if (cell == NULL || cell->continuation) continue;
            if ((int)cell->style != current_style) {
                const char *sgr = theme->sgr[cell->style < UI_STYLE_COUNT ? cell->style : UI_NORMAL];
                out_append(out, cap, &used, sgr, strlen(sgr));
                current_style = cell->style;
            }
            out_append(out, cap, &used, cell->bytes, cell->len);
        }
    }
    out_append(out, cap, &used, "\x1b[0m", strlen("\x1b[0m"));
    if (s.cursor) {
        if (s.cursor_c < 1) s.cursor_c = 1;
        if (s.cursor_c > t->width) s.cursor_c = t->width;
        out_fmt(out, cap, &used, "\x1b[%d;%dH\x1b[?25h", s.cursor_r, s.cursor_c);
    } else {
        out_append(out, cap, &used, "\x1b[?25l", strlen("\x1b[?25l"));
    }
    queue_output(t, out, used);
    screen_free(&s);
    t->dirty = false;
}

int qtc_tui_run(const qtc_paths *paths) {
    (void)setlocale(LC_CTYPE, "");
    tui_ctx t;
    memset(&t, 0, sizeof(t));
    t.running = true;
    t.dirty = true;
    t.selected_channel = -1;
    t.fd = qtc_ipc_client_connect(paths->socket_path, 2000);
    if (t.fd < 0) {
        fprintf(stderr, "Could not connect to QTC core: %s\n", strerror(errno));
        return 1;
    }
    qtc_ipc_reader_init(&t.reader);
    if (verify_core_version(t.fd) != 0) {
        close(t.fd);
        fprintf(stderr, "QTC background core is incompatible with QTC %s; restart QTC\n",
                QTC_VERSION);
        return 1;
    }
    if (set_raw_terminal(&t) != 0) {
        close(t.fd);
        fprintf(stderr, "QTC requires an interactive terminal\n");
        return 1;
    }
    signal(SIGWINCH, sigwinch_handler);
    signal(SIGPIPE, SIG_IGN);

    while (t.running) {
        int64_t now = qtc_now_millis();
        if (g_resize) {
            g_resize = 0;
            t.dirty = true;
        }
        if (t.banner_until > 0 && t.banner_until <= now) {
            t.banner_until = 0;
            t.banner_title[0] = 0;
            t.banner_body[0] = 0;
            t.dirty = true;
        }
        if (t.action_feedback_until > 0 &&
            t.action_feedback_until <= now) {
            t.action_feedback_until = 0;
            t.action_feedback[0] = 0;
            t.action_feedback_level = FEEDBACK_NONE;
            t.dirty = true;
        }

        /* Build only the newest frame. Terminal writes are nonblocking, so a
         * slow terminal cannot delay message/ACK processing or keyboard input. */
        if (t.dirty) render(&t);
        flush_output(&t);

        struct pollfd pfd[3] = {
            {.fd = t.fd, .events = POLLIN},
            {.fd = STDIN_FILENO, .events = POLLIN},
            {.fd = STDOUT_FILENO,
             .events = t.output != NULL ? POLLOUT : 0}
        };
        int timeout = 100;
        if (t.banner_until > now && t.banner_until - now < timeout)
            timeout = (int)(t.banner_until - now);
        if (t.action_feedback_until > now &&
            t.action_feedback_until - now < timeout)
            timeout = (int)(t.action_feedback_until - now);
        int rc = poll(pfd, 3, timeout);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* Core events are deliberately handled before keyboard input. This
         * keeps message delivery live even while a user is continuously
         * typing or pasting into the composer. */
        if (pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            qtc_strlcpy(t.status, "Background core disconnected", sizeof(t.status));
            t.dirty = true;
            t.running = false;
        } else if (pfd[0].revents & POLLIN) {
            for (;;) {
                uint8_t buf[65536];
                ssize_t n = recv(t.fd, buf, sizeof(buf), MSG_DONTWAIT);
                if (n > 0) {
                    if (qtc_ipc_reader_feed(&t.reader, buf, (size_t)n,
                                            ipc_frame, &t) != 0) {
                        t.running = false;
                        break;
                    }
                    continue;
                }
                if (n == 0) t.running = false;
                else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                    t.running = false;
                break;
            }
        }

        if (t.running && (pfd[1].revents & POLLIN)) {
            for (;;) {
                uint8_t buf[4096];
                ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
                if (n > 0) {
                    process_input(&t, buf, (size_t)n);
                    continue;
                }
                if (n < 0 && errno == EINTR) continue;
                if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                    t.running = false;
                break;
            }
        }

        if (pfd[2].revents & POLLOUT) flush_output(&t);

        /* Coalesce all events from this poll wake into one newest frame, then
         * attempt the write immediately. The compose buffer and cursor are
         * rendered from state on every frame, so incoming events cannot erase
         * a draft. */
        if (t.running && t.dirty) {
            render(&t);
            flush_output(&t);
        }
    }
    leave_conversation(&t);
    restore_terminal(&t);
    close(t.fd);
    return 0;
}
