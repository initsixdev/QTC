#define _GNU_SOURCE
#include "qtc/core.h"
#include "qtc/ipc.h"
#include "qtc/notify.h"
#include "qtc/platform.h"
#include "qtc/serial.h"
#include "qtc/tui.h"
#include "qtc/util.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    qtc_state state;
    bool loading;
    bool done;
} cli_snapshot;

static void usage(FILE *out) {
    fprintf(out,
        "QTC Terminal %s - terminal messenger for MeshCore\n\n"
        "Usage:\n"
        "  qtc [--profile NAME] [--device PATH] [--demo]\n"
        "  qtc core [--profile NAME] [--device PATH] [--demo] [--foreground]\n"
        "  qtc status [--profile NAME]\n"
        "  qtc shutdown [--profile NAME]\n"
        "  qtc channel list|create|join|invite|rotate|leave ...\n"
        "  qtc --list-devices\n"
        "  qtc --print-udev-rule\n"
        "  qtc --version\n\n"
        "TUI: F6 channels, F7 network nodes, F8/Ctrl+C detach, Ctrl+Q twice shutdown.\n",
        QTC_VERSION);
}

static const char *arg_value(int argc, char **argv, const char *name) {
    for (int i = 1; i + 1 < argc; i++) if (strcmp(argv[i], name) == 0) return argv[i + 1];
    return NULL;
}
static bool has_arg(int argc, char **argv, const char *name) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], name) == 0) return true;
    }
    return false;
}
static int command_index(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--profile") == 0 || strcmp(argv[i], "--device") == 0) {
            if (i + 1 < argc) i++;
            continue;
        }
        if (argv[i][0] == '-') continue;
        return i;
    }
    return -1;
}

static void snapshot_frame(const qtc_ipc_frame *f, void *userdata) {
    cli_snapshot *s = userdata;
    switch (f->type) {
        case QTC_IPC_STATE_BEGIN: s->loading = true; s->state.contact_count = s->state.channel_count = s->state.message_count = s->state.invitation_count = 0; break;
        case QTC_IPC_CONTACT: if (s->loading && f->length == sizeof(qtc_contact) && s->state.contact_count < QTC_MAX_CONTACTS) memcpy(&s->state.contacts[s->state.contact_count++], f->payload, sizeof(qtc_contact)); break;
        case QTC_IPC_CHANNEL: if (s->loading && f->length == sizeof(qtc_channel) && s->state.channel_count < QTC_MAX_CHANNELS) memcpy(&s->state.channels[s->state.channel_count++], f->payload, sizeof(qtc_channel)); break;
        case QTC_IPC_MESSAGE: if (s->loading && f->length == sizeof(qtc_message) && s->state.message_count < QTC_MAX_MESSAGES) memcpy(&s->state.messages[s->state.message_count++], f->payload, sizeof(qtc_message)); break;
        case QTC_IPC_INVITATION: if (s->loading && f->length == sizeof(qtc_invitation) && s->state.invitation_count < QTC_MAX_INVITATIONS) memcpy(&s->state.invitations[s->state.invitation_count++], f->payload, sizeof(qtc_invitation)); break;
        case QTC_IPC_SETTINGS: if (f->length == sizeof(qtc_settings)) memcpy(&s->state.settings, f->payload, sizeof(qtc_settings)); break;
        case QTC_IPC_STATUS: if (f->length == sizeof(qtc_ipc_status_payload)) { const qtc_ipc_status_payload *p = (const void *)f->payload; s->state.radio_connected = p->radio_connected; s->state.radio_max_channels = p->max_channels; s->state.radio_max_contacts = p->max_contacts; } break;
        case QTC_IPC_STATE_END: s->loading = false; s->done = true; break;
        default: break;
    }
}

static int load_snapshot(int fd, cli_snapshot *snapshot) {
    memset(snapshot, 0, sizeof(*snapshot));
    qtc_ipc_hello h = {.protocol_version = QTC_IPC_PROTOCOL_VERSION};
    qtc_strlcpy(h.client_name, "qtc-cli", sizeof(h.client_name));
    qtc_strlcpy(h.app_version, QTC_VERSION, sizeof(h.app_version));
    if (qtc_ipc_send(fd, QTC_IPC_HELLO, &h, sizeof(h)) != 0) return -1;
    qtc_ipc_frame info_frame;
    if (qtc_ipc_recv_blocking(fd, &info_frame, 1000) != 0 ||
        info_frame.type != QTC_IPC_CORE_INFO ||
        info_frame.length != sizeof(qtc_ipc_core_info)) {
        errno = EPROTO;
        return -1;
    }
    const qtc_ipc_core_info *info = (const void *)info_frame.payload;
    if (info->protocol_version != QTC_IPC_PROTOCOL_VERSION ||
        strcmp(info->app_version, QTC_VERSION) != 0) {
        errno = EPROTO;
        return -1;
    }
    qtc_ipc_reader reader; qtc_ipc_reader_init(&reader);
    while (!snapshot->done) {
        uint8_t buf[8192]; ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) return -1;
        if (qtc_ipc_reader_feed(&reader, buf, (size_t)n, snapshot_frame, snapshot) != 0) return -1;
    }
    return 0;
}

static qtc_channel *find_channel_name(cli_snapshot *s, const char *name) {
    qtc_channel *match = NULL;
    char *end = NULL;
    errno = 0;
    long numeric = strtol(name, &end, 10);
    bool is_numeric = errno == 0 && end != name && *end == '\0' && numeric >= 0 && numeric <= INT_MAX;
    for (size_t i = 0; i < s->state.channel_count; i++) {
        qtc_channel *ch = &s->state.channels[i];
        if (strcmp(ch->name, name) == 0 || (is_numeric && numeric == ch->index)) {
            if (match != NULL && match != ch) return NULL;
            match = ch;
        }
    }
    return match;
}
static qtc_contact *find_contact_name(cli_snapshot *s, const char *name) {
    qtc_contact *match = NULL;
    for (size_t i = 0; i < s->state.contact_count; i++) {
        qtc_contact *c = &s->state.contacts[i]; if (c->node_type != QTC_NODE_PERSON) continue;
        const char *display = c->alias[0] ? c->alias : c->name;
        if (strcmp(c->id, name) == 0 || strcmp(c->prefix, name) == 0 || qtc_casecmp(display, name) == 0) {
            if (match != NULL && match != c) return NULL;
            match = c;
        }
    }
    return match;
}

static int channel_cli(const qtc_paths *paths, int argc, char **argv, int command_pos) {
    if (command_pos < 0 || command_pos + 1 >= argc) { usage(stderr); return 2; }
    int fd = qtc_ipc_client_connect(paths->socket_path, 1000);
    if (fd < 0) { fprintf(stderr, "QTC core is not running\n"); return 1; }
    cli_snapshot snap; if (load_snapshot(fd, &snap) != 0) { close(fd); return 1; }
    const char *op = argv[command_pos + 1]; int rc = 0;
    if (strcmp(op, "list") == 0) {
        for (size_t i = 0; i < snap.state.channel_count; i++) {
            qtc_channel *ch = &snap.state.channels[i]; if (ch->configured) printf("%d\t%s\t%s\n", ch->index, ch->name, ch->is_private ? "private" : "public");
        }
    } else if (strcmp(op, "create") == 0 && command_pos + 2 < argc) {
        qtc_ipc_channel_action_payload p = {0}; qtc_strlcpy(p.name, argv[command_pos + 2], sizeof(p.name)); rc = qtc_ipc_send(fd, QTC_IPC_CHANNEL_CREATE, &p, sizeof(p));
    } else if (strcmp(op, "join") == 0 && command_pos + 2 < argc) {
        qtc_ipc_channel_action_payload p = {0}; qtc_strlcpy(p.uri, argv[command_pos + 2], sizeof(p.uri)); rc = qtc_ipc_send(fd, QTC_IPC_CHANNEL_JOIN, &p, sizeof(p));
    } else if ((strcmp(op, "rotate") == 0 || strcmp(op, "leave") == 0) && command_pos + 2 < argc) {
        const char *channel_name = argv[command_pos + 2];
        qtc_channel *ch = find_channel_name(&snap, channel_name);
        if (ch == NULL) { fprintf(stderr, "Channel not found or ambiguous: %s\n", channel_name); rc = -1; }
        else { qtc_ipc_channel_action_payload p = {.channel_index = ch->index}; rc = qtc_ipc_send(fd, strcmp(op, "rotate") == 0 ? QTC_IPC_CHANNEL_ROTATE : QTC_IPC_CHANNEL_LEAVE, &p, sizeof(p)); }
    } else if (strcmp(op, "invite") == 0 && command_pos + 3 < argc) {
        qtc_channel *ch = find_channel_name(&snap, argv[command_pos + 2]); qtc_contact *ct = find_contact_name(&snap, argv[command_pos + 3]);
        if (ch == NULL || ct == NULL) { fprintf(stderr, "Channel/contact not found or ambiguous\n"); rc = -1; }
        else { qtc_ipc_channel_invite_payload p = {.channel_index = ch->index}; qtc_strlcpy(p.contact_id, ct->id, sizeof(p.contact_id)); rc = qtc_ipc_send(fd, QTC_IPC_CHANNEL_INVITE, &p, sizeof(p)); }
    } else { usage(stderr); rc = -1; }
    close(fd); return rc == 0 ? 0 : 1;
}

static int list_devices(void) {
    char devices[64][QTC_MAX_PATH]; size_t count = 0;
    if (qtc_serial_list_devices(devices, QTC_ARRAY_LEN(devices), &count) != 0) return 1;
    if (count == 0) { puts("No serial devices found"); return 1; }
    for (size_t i = 0; i < count; i++) puts(devices[i]);
    return 0;
}

int main(int argc, char **argv) {
    const char *profile = arg_value(argc, argv, "--profile"); if (profile == NULL) profile = "default";
    const char *device = arg_value(argc, argv, "--device"); bool demo = has_arg(argc, argv, "--demo");
    qtc_paths paths; if (qtc_init_paths(&paths, profile) != 0) { fprintf(stderr, "Cannot initialize QTC paths: %s\n", strerror(errno)); return 1; }
    if (has_arg(argc, argv, "--debug")) qtc_set_log_level(QTC_LOG_DEBUG);
    if (has_arg(argc, argv, "--version")) { printf("qtc %s\n", QTC_VERSION); return 0; }
    if (has_arg(argc, argv, "--help") || has_arg(argc, argv, "-h")) { usage(stdout); return 0; }
    if (has_arg(argc, argv, "--list-devices")) return list_devices();
    if (has_arg(argc, argv, "--print-udev-rule")) { puts(qtc_platform_device_access_help()); return 0; }

    int command_pos = command_index(argc, argv);
    const char *command = command_pos >= 0 ? argv[command_pos] : NULL;
    if (command && strcmp(command, "core") == 0) return qtc_core_run(&paths, device, demo, has_arg(argc, argv, "--foreground"));
    if (command && strcmp(command, "status") == 0) return qtc_core_status(&paths);
    if (command && (strcmp(command, "shutdown") == 0 || strcmp(command, "quit") == 0)) return qtc_core_shutdown(&paths);
    if (command && strcmp(command, "channel") == 0) return channel_cli(&paths, argc, argv, command_pos);
    if (command && strcmp(command, "test-notify") == 0) return qtc_notify_desktop("QTC notification test", "Desktop notifications are working") == 0 ? 0 : 1;
    if (command && strcmp(command, "test-sound") == 0) return qtc_notify_sound() == 0 ? 0 : 1;
    if (command != NULL) { usage(stderr); return 2; }

    char executable[PATH_MAX]; qtc_platform_self_path(executable, sizeof(executable), argv[0]);
    if (qtc_core_ensure_running(executable, &paths, device, demo) != 0) {
        fprintf(stderr, "Could not start QTC background core: %s\n", strerror(errno)); return 1;
    }
    return qtc_tui_run(&paths);
}
