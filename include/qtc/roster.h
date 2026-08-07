#ifndef QTC_ROSTER_H
#define QTC_ROSTER_H

#include "qtc/qtc.h"

typedef struct {
    int kind; /* 1 channel, 2 favorite, 3 contact, 4 heading */
    int source_index;
    int route_hops;
    bool route_known;
    char label[QTC_MAX_NAME + QTC_MAX_GROUP + 32];
} qtc_roster_row;

typedef struct {
    qtc_roster_row pinned[QTC_MAX_CONTACTS + QTC_MAX_CHANNELS + 64];
    size_t pinned_count;
    qtc_roster_row scrollable[QTC_MAX_CONTACTS * 2 + 64];
    size_t scrollable_count;
} qtc_roster;

void qtc_roster_build(const qtc_state *state, const char *search, qtc_roster *roster);
void qtc_roster_clamp(size_t selected, size_t row_count, size_t viewport_height,
                      size_t *scroll_offset);
const char *qtc_node_type_label(qtc_node_type type);

#endif
