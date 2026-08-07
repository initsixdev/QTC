#include "test.h"
#include "qtc/roster.h"

static void contact(qtc_contact *c, const char *id, const char *name,
                    bool known, int hops, bool fav, int unread) {
    strcpy(c->id, id);
    strcpy(c->name, name);
    c->node_type = QTC_NODE_PERSON;
    c->route_known = known;
    c->route_hops = hops;
    c->favorite = fav;
    c->unread = unread;
}

static bool roster_contains(const qtc_roster *r, const char *needle) {
    for (size_t i = 0; i < r->pinned_count; i++)
        if (strstr(r->pinned[i].label, needle) != NULL) return true;
    for (size_t i = 0; i < r->scrollable_count; i++)
        if (strstr(r->scrollable[i].label, needle) != NULL) return true;
    return false;
}

int main(void) {
    qtc_state s = {0};
    s.channel_count = 2;
    s.channels[0].configured = true;
    s.channels[0].index = 0;
    strcpy(s.channels[0].name, "Public");
    s.channels[1].configured = true;
    s.channels[1].index = 1;
    strcpy(s.channels[1].name, "Croatia Mesh");

    contact(&s.contacts[0], "a", "Zed", true, 2, false, 0);
    contact(&s.contacts[1], "b", "Ana", true, 1, true, 0);
    strcpy(s.contacts[1].alias, "Captain Ana");
    strcpy(s.contacts[1].favorite_group, "Home Team");
    strcpy(s.contacts[1].prefix, "aabbccddeeff");
    contact(&s.contacts[2], "c", "Marko", true, 1, false, 2);
    contact(&s.contacts[3], "d", "Ivana", false, 0, false, 0);
    contact(&s.contacts[4], "e", "Fav", true, 3, true, 0);
    strcpy(s.contacts[4].favorite_group, "Work");
    contact(&s.contacts[5], "f", "Default Fav", true, 0, true, 0);
    strcpy(s.contacts[5].favorite_group, "Favorites");
    s.contact_count = 6;

    qtc_roster r;
    qtc_roster_build(&s, "", &r);
    ASSERT_TRUE(r.pinned_count >= 7);
    ASSERT_STREQ(r.pinned[0].label, "CHANNELS");

    int favorites_headers = 0;
    int default_group_subheaders = 0;
    for (size_t i = 0; i < r.pinned_count; i++) {
        if (strcmp(r.pinned[i].label, "FAVORITES") == 0) favorites_headers++;
        if (strcmp(r.pinned[i].label, "  Favorites") == 0) default_group_subheaders++;
    }
    ASSERT_EQ_INT(favorites_headers, 1);
    ASSERT_EQ_INT(default_group_subheaders, 0);
    ASSERT_TRUE(roster_contains(&r, "Default Fav"));

    int marko = -1, zed = -1, flood = -1;
    for (size_t i = 0; i < r.scrollable_count; i++) {
        if (strstr(r.scrollable[i].label, "Marko")) marko = (int)i;
        if (strstr(r.scrollable[i].label, "Zed")) zed = (int)i;
        if (strcmp(r.scrollable[i].label, "-- flood --") == 0) flood = (int)i;
    }
    ASSERT_TRUE(marko >= 0 && zed >= 0 && flood >= 0);
    ASSERT_TRUE(marko < zed);
    ASSERT_TRUE(zed < flood);

    qtc_roster_build(&s, "mar*", &r);
    ASSERT_TRUE(roster_contains(&r, "Marko"));

    /* Search covers visible alias, original radio name, favorite group, key prefix,
     * and channel names. This is important when a local alias hides the radio name. */
    qtc_roster_build(&s, "captain", &r);
    ASSERT_TRUE(roster_contains(&r, "Captain Ana"));
    qtc_roster_build(&s, "ana", &r);
    ASSERT_TRUE(roster_contains(&r, "Captain Ana"));
    qtc_roster_build(&s, "home", &r);
    ASSERT_TRUE(roster_contains(&r, "Captain Ana"));
    qtc_roster_build(&s, "bbccdd", &r);
    ASSERT_TRUE(roster_contains(&r, "Captain Ana"));
    qtc_roster_build(&s, "croatia", &r);
    ASSERT_TRUE(roster_contains(&r, "Croatia Mesh"));

    size_t off = 0;
    qtc_roster_clamp(9, 20, 5, &off);
    ASSERT_EQ_INT(off, 5);
    puts("roster tests passed");
    return 0;
}
