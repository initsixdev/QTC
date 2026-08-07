#include "qtc/notify.h"
#include "qtc/util.h"

#include <stdlib.h>

int qtc_notify_desktop(const char *title, const char *body) {
    if (qtc_command_exists("notify-send")) {
        char *argv[] = {"/usr/bin/env", "notify-send", "--app-name=QTC", "--", (char *)title,
                        (char *)body, NULL};
        return qtc_spawn_detached(argv);
    }
    if (qtc_command_exists("terminal-notifier")) {
        char *argv[] = {"/usr/bin/env", "terminal-notifier", "-title", (char *)title,
                        "-message", (char *)body, NULL};
        return qtc_spawn_detached(argv);
    }
    if (qtc_command_exists("osascript")) {
        /* Message text is remote-controlled, so it is passed as argv and read back
         * through AppleScript's own argv rather than interpolated into the script
         * source. There is no quoting to get wrong and nothing to escape. */
        char *argv[] = {"/usr/bin/env", "osascript",
                        "-e", "on run argv",
                        "-e", "display notification (item 1 of argv) with title (item 2 of argv)",
                        "-e", "end run",
                        "--", (char *)body, (char *)title, NULL};
        return qtc_spawn_detached(argv);
    }
    return 1;
}

int qtc_notify_sound(void) {
    const char *sound = getenv("QTC_SOUND_FILE");
    if (qtc_command_exists("canberra-gtk-play")) {
        char *argv[] = {"/usr/bin/env", "canberra-gtk-play", "-i", "message-new-instant", NULL};
        return qtc_spawn_detached(argv);
    }
    if (qtc_command_exists("pw-play")) {
        if (sound != NULL && *sound != 0) {
            char *argv[] = {"/usr/bin/env", "pw-play", (char *)sound, NULL};
            return qtc_spawn_detached(argv);
        }
    }
    if (qtc_command_exists("afplay")) {
        const char *file = sound != NULL && *sound != 0 ? sound : "/System/Library/Sounds/Ping.aiff";
        char *argv[] = {"/usr/bin/env", "afplay", (char *)file, NULL};
        return qtc_spawn_detached(argv);
    }
    return 1;
}

int qtc_notify_message(const qtc_settings *s, bool channel, const char *title, const char *body) {
    int rc = 0;
    if (s->desktop_notifications && ((channel && s->notify_channel) || (!channel && s->notify_direct)))
        rc |= qtc_notify_desktop(title, body);
    if (s->sound_enabled) rc |= qtc_notify_sound();
    return rc;
}
