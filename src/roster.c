#include "qtc/roster.h"
#include "qtc/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *display_name(const qtc_contact *c) { return c->alias[0] != 0 ? c->alias : c->name; }

static bool contact_matches(const char *search, const qtc_contact *c) {
    if (search == NULL || *search == 0) return true;
    return qtc_search_match(search, display_name(c)) ||
           qtc_search_match(search, c->name) ||
           qtc_search_match(search, c->alias) ||
           qtc_search_match(search, c->favorite_group) ||
           qtc_search_match(search, c->prefix);
}

const char *qtc_node_type_label(qtc_node_type type) {
    switch (type) {
        case QTC_NODE_PERSON: return "person";
        case QTC_NODE_REPEATER: return "repeater";
        case QTC_NODE_ROOM: return "room";
        case QTC_NODE_SENSOR: return "sensor";
        default: return "unknown";
    }
}

static const qtc_state *g_sort_state;
static int contact_cmp(const void *pa, const void *pb) {
    size_t ia = *(const size_t *)pa, ib = *(const size_t *)pb;
    const qtc_contact *a = &g_sort_state->contacts[ia], *b = &g_sort_state->contacts[ib];
    if (a->route_known != b->route_known) return a->route_known ? -1 : 1;
    if (a->route_known && a->route_hops != b->route_hops) return a->route_hops - b->route_hops;
    if (a->unread != b->unread) return b->unread - a->unread;
    if (a->last_heard != b->last_heard) return a->last_heard > b->last_heard ? -1 : 1;
    int c = qtc_casecmp(display_name(a), display_name(b));
    return c != 0 ? c : qtc_casecmp(a->id, b->id);
}

static int favorite_cmp(const void *pa, const void *pb) {
    size_t ia = *(const size_t *)pa, ib = *(const size_t *)pb;
    const qtc_contact *a = &g_sort_state->contacts[ia], *b = &g_sort_state->contacts[ib];
    int c = qtc_casecmp(a->favorite_group, b->favorite_group);
    if (c != 0) return c;
    if (a->unread != b->unread) return b->unread - a->unread;
    c = qtc_casecmp(display_name(a), display_name(b));
    return c != 0 ? c : qtc_casecmp(a->id, b->id);
}

void qtc_roster_build(const qtc_state *state, const char *search, qtc_roster *r) {
    memset(r, 0, sizeof(*r));
    qtc_roster_row *row = &r->pinned[r->pinned_count++]; row->kind = 4; qtc_strlcpy(row->label, "CHANNELS", sizeof(row->label));
    for (size_t i = 0; i < state->channel_count && r->pinned_count < QTC_ARRAY_LEN(r->pinned); i++) {
        const qtc_channel *ch = &state->channels[i];
        if (!ch->configured || !qtc_search_match(search, ch->name)) continue;
        row = &r->pinned[r->pinned_count++]; row->kind = 1; row->source_index = (int)i;
        snprintf(row->label, sizeof(row->label), "%s%s%s", ch->unread ? "! " : "", ch->name,
                 ch->unread ? " [NEW]" : "");
    }
    row = &r->pinned[r->pinned_count++]; row->kind = 4; qtc_strlcpy(row->label, "FAVORITES", sizeof(row->label));
    size_t fav[QTC_MAX_CONTACTS], fn = 0, normal[QTC_MAX_CONTACTS], nn = 0;
    for (size_t i = 0; i < state->contact_count; i++) {
        const qtc_contact *c = &state->contacts[i]; if (c->node_type != QTC_NODE_PERSON) continue;
        if (!contact_matches(search, c)) continue;
        if (c->favorite) fav[fn++] = i; else normal[nn++] = i;
    }
    g_sort_state = state;
    qsort(fav, fn, sizeof(fav[0]), favorite_cmp);
    const char *last_group = NULL;
    for (size_t x = 0; x < fn && r->pinned_count < QTC_ARRAY_LEN(r->pinned); x++) {
        const qtc_contact *c = &state->contacts[fav[x]];
        const char *group = c->favorite_group[0] ? c->favorite_group : "Favorites";
        if (last_group == NULL || qtc_casecmp(group, last_group) != 0) {
            /* "FAVORITES" is already the section heading.  Rendering the
             * default group name immediately below it produced a visually
             * duplicated FAVORITES/Favorites header.  Named groups still get
             * their own subheading. */
            if (qtc_casecmp(group, "Favorites") != 0) {
                row = &r->pinned[r->pinned_count++]; row->kind = 4;
                snprintf(row->label, sizeof(row->label), "  %s", group);
            }
            last_group = group;
        }
        row = &r->pinned[r->pinned_count++]; row->kind = 2; row->source_index = (int)fav[x];
        snprintf(row->label, sizeof(row->label), "%s%s%s", c->unread ? "! " : "", display_name(c),
                 c->unread ? " [NEW]" : "");
    }
    row = &r->pinned[r->pinned_count++]; row->kind = 4; qtc_strlcpy(row->label, "-- CONTACTS --", sizeof(row->label));

    qsort(normal, nn, sizeof(normal[0]), contact_cmp);
    int last_route = -999; bool last_known = true;
    for (size_t x = 0; x < nn && r->scrollable_count < QTC_ARRAY_LEN(r->scrollable); x++) {
        const qtc_contact *c = &state->contacts[normal[x]];
        if (x == 0 || c->route_known != last_known || (c->route_known && c->route_hops != last_route)) {
            row = &r->scrollable[r->scrollable_count++]; row->kind = 4;
            if (!c->route_known) qtc_strlcpy(row->label, "-- flood --", sizeof(row->label));
            else if (c->route_hops == 0) qtc_strlcpy(row->label, "-- direct --", sizeof(row->label));
            else snprintf(row->label, sizeof(row->label), "-- %d hop%s --", c->route_hops, c->route_hops == 1 ? "" : "s");
            last_known = c->route_known; last_route = c->route_hops;
        }
        row = &r->scrollable[r->scrollable_count++]; row->kind = 3; row->source_index = (int)normal[x];
        row->route_hops = c->route_hops; row->route_known = c->route_known;
        snprintf(row->label, sizeof(row->label), "%s%s%s", c->unread ? "! " : "", display_name(c),
                 c->unread ? " [NEW]" : "");
    }
}

void qtc_roster_clamp(size_t selected, size_t count, size_t height, size_t *offset) {
    if (offset == NULL) return;
    if (count == 0 || height == 0) { *offset = 0; return; }
    if (selected >= count) selected = count - 1;
    if (*offset > selected) *offset = selected;
    if (selected >= *offset + height) *offset = selected - height + 1;
    size_t max = count > height ? count - height : 0;
    if (*offset > max) *offset = max;
}
