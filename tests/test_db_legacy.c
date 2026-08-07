#define _GNU_SOURCE
#include "test.h"
#include "qtc/db.h"

#include <sqlite3.h>
#include <unistd.h>

static void create_legacy_database(const char *path) {
    static const char *schema =
        "CREATE TABLE meta(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
        "CREATE TABLE contacts("
        " public_key BLOB PRIMARY KEY, type INTEGER, flags INTEGER, out_path_len INTEGER,"
        " name TEXT NOT NULL, alias TEXT NOT NULL DEFAULT '', last_advert INTEGER,"
        " lat REAL, lon REAL, unread INTEGER NOT NULL DEFAULT 0,"
        " favorite INTEGER NOT NULL DEFAULT 0, favorite_group TEXT NOT NULL DEFAULT '');"
        "CREATE TABLE channels(idx INTEGER PRIMARY KEY, configured INTEGER, name TEXT NOT NULL,"
        " unread INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE messages(id INTEGER PRIMARY KEY, kind INTEGER NOT NULL, peer_key BLOB,"
        " channel_idx INTEGER, direction INTEGER NOT NULL, status INTEGER NOT NULL,"
        " timestamp INTEGER NOT NULL, snr REAL NOT NULL DEFAULT 0, ack BLOB, text TEXT NOT NULL);"
        "CREATE INDEX messages_time_idx ON messages(timestamp);"
        "CREATE TABLE settings(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
        "INSERT INTO meta(key,value) VALUES('schema','2');"
        "INSERT INTO contacts(public_key,type,flags,out_path_len,name,alias,last_advert,lat,lon,unread,favorite,favorite_group) VALUES("
        " X'00112233445566778899AABBCCDDEEFF00112233445566778899AABBCCDDEEFF',1,5,2,'Ana','Mum',1700000000,45.555,18.666,3,1,'Family');"
        "INSERT INTO contacts(public_key,type,flags,out_path_len,name,alias,last_advert,lat,lon,unread,favorite,favorite_group) VALUES("
        " X'FFEEDDCCBBAA99887766554433221100FFEEDDCCBBAA99887766554433221100',2,1,255,'Darda Repeater','',1699999000,45.600,18.700,0,0,'');"
        "INSERT INTO channels(idx,configured,name,unread) VALUES(0,1,'Public',2);"
        "INSERT INTO channels(idx,configured,name,unread) VALUES(2,1,'Family',4);"
        "INSERT INTO messages(id,kind,peer_key,channel_idx,direction,status,timestamp,snr,ack,text) VALUES("
        " 101,1,X'00112233445566778899AABBCCDDEEFF00112233445566778899AABBCCDDEEFF',NULL,0,3,1700000001,7.25,X'01020304','hello from legacy');"
        "INSERT INTO messages(id,kind,peer_key,channel_idx,direction,status,timestamp,snr,ack,text) VALUES("
        " 102,2,NULL,2,0,3,1700000002,4.5,NULL,'legacy channel message');"
        "INSERT INTO settings(key,value) VALUES('sound_enabled','0');"
        "INSERT INTO settings(key,value) VALUES('theme','2');"
        "INSERT INTO settings(key,value) VALUES('serial_device','/dev/ttyACM9');";

    sqlite3 *db = NULL;
    ASSERT_EQ_INT(sqlite3_open(path, &db), SQLITE_OK);
    char *error = NULL;
    int rc = sqlite3_exec(db, schema, NULL, NULL, &error);
    if (rc != SQLITE_OK) fprintf(stderr, "legacy fixture error: %s\n", error != NULL ? error : "unknown");
    sqlite3_free(error);
    ASSERT_EQ_INT(rc, SQLITE_OK);
    ASSERT_EQ_INT(sqlite3_close(db), SQLITE_OK);
}

static const qtc_contact *find_contact(const qtc_state *state, const char *id) {
    for (size_t i = 0; i < state->contact_count; i++) {
        if (strcmp(state->contacts[i].id, id) == 0) return &state->contacts[i];
    }
    return NULL;
}

static const qtc_channel *find_channel(const qtc_state *state, int index) {
    for (size_t i = 0; i < state->channel_count; i++) {
        if (state->channels[i].index == index) return &state->channels[i];
    }
    return NULL;
}

static int scalar_int(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    ASSERT_EQ_INT(sqlite3_prepare_v2(db, sql, -1, &st, NULL), SQLITE_OK);
    ASSERT_EQ_INT(sqlite3_step(st), SQLITE_ROW);
    int value = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return value;
}

int main(void) {
    char dir[] = "/tmp/qtc-db-legacy-test-XXXXXX";
    ASSERT_TRUE(mkdtemp(dir) != NULL);
    char path[512];
    snprintf(path, sizeof(path), "%s/qtc.db", dir);
    create_legacy_database(path);

    qtc_db db;
    ASSERT_EQ_INT(qtc_db_open(&db, path), 0);
    ASSERT_EQ_INT(qtc_db_migrate(&db), 0);
    ASSERT_EQ_INT(qtc_db_migrate(&db), 0);

    qtc_state state;
    ASSERT_EQ_INT(qtc_db_load_state(&db, &state), 0);
    ASSERT_EQ_INT(state.contact_count, 2);
    ASSERT_EQ_INT(state.channel_count, 2);
    ASSERT_EQ_INT(state.message_count, 2);

    const char *ana_id = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    const qtc_contact *ana = find_contact(&state, ana_id);
    ASSERT_TRUE(ana != NULL);
    ASSERT_STREQ(ana->prefix, "001122334455");
    ASSERT_STREQ(ana->name, "Ana");
    ASSERT_STREQ(ana->alias, "Mum");
    ASSERT_EQ_INT(ana->node_type, QTC_NODE_PERSON);
    ASSERT_TRUE(ana->route_known);
    ASSERT_EQ_INT(ana->route_hops, 2);
    ASSERT_TRUE(ana->favorite);
    ASSERT_STREQ(ana->favorite_group, "Family");
    ASSERT_EQ_INT(ana->unread, 3);
    ASSERT_EQ_INT(ana->flags, 5);

    const char *repeater_id = "ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100";
    const qtc_contact *repeater = find_contact(&state, repeater_id);
    ASSERT_TRUE(repeater != NULL);
    ASSERT_EQ_INT(repeater->node_type, QTC_NODE_REPEATER);
    ASSERT_TRUE(!repeater->route_known);

    const qtc_channel *family = find_channel(&state, 2);
    ASSERT_TRUE(family != NULL);
    ASSERT_STREQ(family->name, "Family");
    ASSERT_TRUE(family->configured);
    ASSERT_EQ_INT(family->unread, 4);
    ASSERT_TRUE(!family->is_private);
    for (size_t i = 0; i < sizeof(family->secret); i++) ASSERT_EQ_INT(family->secret[i], 0);

    ASSERT_EQ_INT(state.messages[0].id, 101);
    ASSERT_EQ_INT(state.messages[0].conversation_kind, QTC_CONV_CONTACT);
    ASSERT_STREQ(state.messages[0].conversation_key, ana_id);
    ASSERT_STREQ(state.messages[0].text, "hello from legacy");
    ASSERT_EQ_INT(state.messages[1].id, 102);
    ASSERT_EQ_INT(state.messages[1].conversation_kind, QTC_CONV_CHANNEL);
    ASSERT_STREQ(state.messages[1].conversation_key, "2");
    ASSERT_STREQ(state.messages[1].text, "legacy channel message");

    ASSERT_TRUE(!state.settings.sound_enabled);
    ASSERT_EQ_INT(state.settings.theme, 2);
    ASSERT_STREQ(state.settings.serial_device, "/dev/ttyACM9");

    ASSERT_EQ_INT(scalar_int(db.db, "SELECT COUNT(*) FROM legacy_contacts_v2"), 2);
    ASSERT_EQ_INT(scalar_int(db.db, "SELECT COUNT(*) FROM legacy_channels_v2"), 2);
    ASSERT_EQ_INT(scalar_int(db.db, "SELECT COUNT(*) FROM legacy_messages_v2"), 2);
    ASSERT_EQ_INT(scalar_int(db.db, "SELECT COUNT(*) FROM contacts"), 2);
    ASSERT_EQ_INT(scalar_int(db.db, "SELECT COUNT(*) FROM channels"), 2);
    ASSERT_EQ_INT(scalar_int(db.db, "SELECT COUNT(*) FROM messages"), 2);
    ASSERT_EQ_INT(scalar_int(db.db, "SELECT CAST(value AS INTEGER) FROM schema_meta WHERE key='schema_version'"), QTC_DB_SCHEMA_VERSION);

    qtc_db_close(&db);
    unlink(path);
    rmdir(dir);
    puts("legacy database migration tests passed");
    return 0;
}
