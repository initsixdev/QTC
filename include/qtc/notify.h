#ifndef QTC_NOTIFY_H
#define QTC_NOTIFY_H

#include "qtc/qtc.h"

int qtc_notify_desktop(const char *title, const char *body);
int qtc_notify_sound(void);
int qtc_notify_message(const qtc_settings *settings, bool is_channel,
                       const char *title, const char *body);

#endif
