#include "test.h"
#include "qtc/invite.h"
#include <errno.h>

int main(void) {
    uint8_t key[16], parsed[16]; for (int i = 0; i < 16; i++) key[i] = (uint8_t)i;
    char uri[QTC_MAX_URI], name[33], message[QTC_MAX_TEXT], parsed_uri[QTC_MAX_URI];
    ASSERT_EQ_INT(qtc_channel_uri_build("Family & Friends", key, uri, sizeof(uri)), 0);
    ASSERT_TRUE(strstr(uri, "name=Family%20%26%20Friends") != NULL);
    ASSERT_EQ_INT(qtc_channel_uri_parse(uri, name, sizeof(name), parsed), 0);
    ASSERT_STREQ(name, "Family & Friends"); ASSERT_TRUE(memcmp(key, parsed, 16) == 0);
    ASSERT_EQ_INT(qtc_invite_message_build(uri, message, sizeof(message)), 0);
    ASSERT_EQ_INT(qtc_invite_message_parse(message, parsed_uri, sizeof(parsed_uri), name, sizeof(name), parsed), 0);
    ASSERT_STREQ(parsed_uri, uri); ASSERT_STREQ(name, "Family & Friends");
    ASSERT_EQ_INT(qtc_channel_join_parse("000102030405060708090a0b0c0d0e0f", name, sizeof(name), parsed), 0);
    ASSERT_STREQ(name, "Private-00010203"); ASSERT_TRUE(memcmp(key, parsed, 16) == 0);
    ASSERT_EQ_INT(qtc_channel_join_parse("Workshop:000102030405060708090a0b0c0d0e0f", name, sizeof(name), parsed), 0);
    ASSERT_STREQ(name, "Workshop"); ASSERT_TRUE(memcmp(key, parsed, 16) == 0);
    ASSERT_TRUE(qtc_channel_join_parse("not-a-key", name, sizeof(name), parsed) != 0);
    ASSERT_TRUE(qtc_channel_uri_parse("meshcore://channel/add?name=x&secret=1234", name, sizeof(name), parsed) != 0);
    qtc_state state = {0}; state.channel_count = 2; state.channels[0].index = 0; state.channels[0].configured = true;
    state.channels[1].index = 1; state.channels[1].configured = true;
    ASSERT_EQ_INT(qtc_find_free_channel_slot(&state, 8), 2);
    puts("invite tests passed"); return 0;
}
