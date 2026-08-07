#ifndef QTC_CORE_H
#define QTC_CORE_H

#include "qtc/qtc.h"

int qtc_core_run(const qtc_paths *paths, const char *device, bool demo, bool foreground);
int qtc_core_status(const qtc_paths *paths);
int qtc_core_shutdown(const qtc_paths *paths);
int qtc_core_ensure_running(const char *self_path, const qtc_paths *paths,
                            const char *device, bool demo);

#endif
