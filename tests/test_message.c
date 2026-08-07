#include "test.h"
#include "qtc/message.h"
#include <string.h>

int main(void) {
    char wire[256], chunk[128];
    uint32_t token = 0;
    int index = 0, total = 0;
    ASSERT_EQ_INT(qtc_long_wire_build(0x1234abcdU, 2, 3, "čć hello", wire, sizeof(wire)), 0);
    ASSERT_EQ_INT(qtc_long_wire_parse(wire, &token, &index, &total, chunk, sizeof(chunk)), 0);
    ASSERT_EQ_INT(token, 0x1234abcdU);
    ASSERT_EQ_INT(index, 2);
    ASSERT_EQ_INT(total, 3);
    ASSERT_STREQ(chunk, "čć hello");

    const char *utf8 = "12345č67890";
    size_t cut = qtc_utf8_chunk_length(utf8, 6);
    ASSERT_EQ_INT(cut, 5);

    qtc_message parts[3] = {0};
    for (int i = 0; i < 3; i++) {
        strcpy(parts[i].logical_key, "logical");
        parts[i].part_index = i + 1;
        parts[i].part_total = 3;
        parts[i].status = QTC_MSG_DELIVERED;
        parts[i].id = i + 1;
    }
    strcpy(parts[0].text, "one ");
    strcpy(parts[1].text, "two ");
    strcpy(parts[2].text, "three");
    parts[1].status = QTC_MSG_SENT;
    char assembled[128];
    qtc_message_status status;
    int part_total = 0, part_count = 0;
    ASSERT_EQ_INT(qtc_message_assemble(parts, 3, "logical", assembled, sizeof(assembled),
                                       &part_total, &part_count, &status), 0);
    ASSERT_STREQ(assembled, "one two three");
    ASSERT_EQ_INT(part_total, 3);
    ASSERT_EQ_INT(part_count, 3);
    ASSERT_EQ_INT(status, QTC_MSG_SENT);

    puts("message tests passed");
    return 0;
}
