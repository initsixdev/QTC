#include "qtc/notify.h"
#include "qtc/util.h"

#include <stdlib.h>

int qtc_notify_desktop(const char *title, const char *body) {
    if (!qtc_command_exists("notify-send")) return 1;
    char *argv[] = {"/usr/bin/env", "notify-send", "--app-name=QTC", "--", (char *)title,
                    (char *)body, NULL};
    return qtc_spawn_detached(argv);
}

int qtc_notify_sound(void) {
    if (qtc_command_exists("canberra-gtk-play")) {
        char *argv[] = {"/usr/bin/env", "canberra-gtk-play", "-i", "message-new-instant", NULL};
        return qtc_spawn_detached(argv);
    }
    if (qtc_command_exists("pw-play")) {
        const char *sound = getenv("QTC_SOUND_FILE");
        if (sound != NULL && *sound != 0) {
            char *argv[] = {"/usr/bin/env", "pw-play", (char *)sound, NULL};
            return qtc_spawn_detached(argv);
        }
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
