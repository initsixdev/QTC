#ifndef QTC_SERIAL_H
#define QTC_SERIAL_H

#include "qtc/protocol.h"
#include <termios.h>
#include <sys/types.h>

typedef struct {
    int fd;
    char device[QTC_MAX_PATH];
    struct termios original;
    bool original_valid;
    qtc_serial_parser parser;
} qtc_serial;

int qtc_serial_open(qtc_serial *serial, const char *device, int baud);
void qtc_serial_close(qtc_serial *serial);
ssize_t qtc_serial_read(qtc_serial *serial, uint8_t *buf, size_t len);
int qtc_serial_send(qtc_serial *serial, const uint8_t *payload, size_t len);
int qtc_serial_list_devices(char devices[][QTC_MAX_PATH], size_t max_devices, size_t *count);
int qtc_serial_autodetect(char *out, size_t out_len);

#endif
