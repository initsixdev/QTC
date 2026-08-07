#define _GNU_SOURCE
#include "test.h"
#include "qtc/db.h"
#include <unistd.h>

int main(void) {
    char dir[]="/tmp/qtc-db-test-XXXXXX"; ASSERT_TRUE(mkdtemp(dir)!=NULL); char path[512]; snprintf(path,sizeof(path),"%s/qtc.db",dir);
    qtc_db db; ASSERT_EQ_INT(qtc_db_open(&db,path),0); ASSERT_EQ_INT(qtc_db_migrate(&db),0); ASSERT_EQ_INT(qtc_db_migrate(&db),0);
    sqlite3_stmt *schema = NULL;
    ASSERT_EQ_INT(sqlite3_prepare_v2(db.db, "SELECT value FROM schema_meta WHERE key='schema_version'", -1, &schema, NULL), SQLITE_OK);
    ASSERT_EQ_INT(sqlite3_step(schema), SQLITE_ROW);
    ASSERT_EQ_INT(atoi((const char *)sqlite3_column_text(schema, 0)), QTC_DB_SCHEMA_VERSION);
    sqlite3_finalize(schema);
    qtc_contact c={0}; strcpy(c.id,"00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"); strcpy(c.prefix,"001122334455"); strcpy(c.name,"Ana"); c.node_type=QTC_NODE_PERSON; c.route_known=true; c.last_heard=42;
    ASSERT_EQ_INT(qtc_db_upsert_contact(&db,&c),0); ASSERT_EQ_INT(qtc_db_set_contact_favorite(&db,c.id,true,"Family"),0);
    ASSERT_EQ_INT(qtc_db_set_contact_alias(&db,c.id,"Ana Alias"),0);
    qtc_message m={0}; m.conversation_kind=QTC_CONV_CONTACT; strcpy(m.conversation_key,c.id); m.direction=QTC_MSG_INCOMING; m.sender_timestamp=100; m.status=QTC_MSG_DELIVERED; strcpy(m.message_key,"stable-message-key"); strcpy(m.text,"hello"); m.created_at=100;
    bool inserted=false; ASSERT_EQ_INT(qtc_db_insert_message(&db,&m,&inserted),0); ASSERT_TRUE(inserted);
    qtc_message duplicate=m; duplicate.id=0; inserted=true; ASSERT_EQ_INT(qtc_db_insert_message(&db,&duplicate,&inserted),0); ASSERT_TRUE(!inserted);
    qtc_message ackm=m; ackm.id=0; ackm.direction=QTC_MSG_OUTGOING; ackm.status=QTC_MSG_QUEUED; strcpy(ackm.message_key,"outgoing-ack-message"); strcpy(ackm.logical_key,"outgoing-ack-message"); inserted=false;
    ASSERT_EQ_INT(qtc_db_insert_message(&db,&ackm,&inserted),0); ASSERT_TRUE(inserted);
    ASSERT_EQ_INT(qtc_db_update_message_send_state(&db,"outgoing-ack-message",QTC_MSG_SENT,2,0x12345678U,9999),0);
    qtc_state state; ASSERT_EQ_INT(qtc_db_load_state(&db,&state),0); ASSERT_EQ_INT(state.contact_count,1); ASSERT_EQ_INT(state.message_count,2); ASSERT_TRUE(state.contacts[0].favorite); ASSERT_STREQ(state.contacts[0].favorite_group,"Family"); ASSERT_STREQ(state.contacts[0].alias,"Ana Alias"); ASSERT_EQ_INT(state.contacts[0].unread,1);
    ASSERT_EQ_INT(state.messages[1].status,QTC_MSG_SENT); ASSERT_EQ_INT(state.messages[1].attempt,2); ASSERT_EQ_INT(state.messages[1].ack_code,0x12345678U); ASSERT_EQ_INT(state.messages[1].ack_deadline,9999);
    ASSERT_EQ_INT(qtc_db_record_message_ack(&db,"outgoing-ack-message",0x12345678U,2),0);
    int changed=0; ASSERT_EQ_INT(qtc_db_mark_ack_delivered(&db,0x12345678U,&changed),0); ASSERT_EQ_INT(changed,1);
    ASSERT_EQ_INT(qtc_db_load_state(&db,&state),0); ASSERT_EQ_INT(state.messages[1].status,QTC_MSG_DELIVERED);
    qtc_message part=m; part.id=0; strcpy(part.logical_key,"incoming-long"); strcpy(part.message_key,"incoming-long:p1"); strcpy(part.text,"first"); part.part_index=1; part.part_total=2; inserted=false;
    ASSERT_EQ_INT(qtc_db_insert_message(&db,&part,&inserted),0); ASSERT_TRUE(inserted);
    strcpy(part.message_key,"incoming-long:p2"); strcpy(part.text,"second"); part.part_index=2; inserted=false;
    ASSERT_EQ_INT(qtc_db_insert_message(&db,&part,&inserted),0); ASSERT_TRUE(inserted);
    ASSERT_EQ_INT(qtc_db_load_state(&db,&state),0); ASSERT_EQ_INT(state.contacts[0].unread,2);
    ASSERT_EQ_INT(qtc_db_mark_contact_read(&db,c.id),0); ASSERT_EQ_INT(qtc_db_load_state(&db,&state),0); ASSERT_EQ_INT(state.contacts[0].unread,0);
    qtc_db_close(&db); unlink(path); rmdir(dir); puts("database tests passed"); return 0;
}
