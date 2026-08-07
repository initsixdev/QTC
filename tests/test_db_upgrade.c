#define _GNU_SOURCE
#include "test.h"
#include "qtc/db.h"

#include <sqlite3.h>
#include <unistd.h>

static int scalar_int(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    ASSERT_EQ_INT(sqlite3_prepare_v2(db, sql, -1, &st, NULL), SQLITE_OK);
    ASSERT_EQ_INT(sqlite3_step(st), SQLITE_ROW);
    int value = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return value;
}

int main(void) {
    char dir[] = "/tmp/qtc-db-upgrade-test-XXXXXX";
    ASSERT_TRUE(mkdtemp(dir) != NULL);
    char path[512];
    snprintf(path, sizeof(path), "%s/qtc.db", dir);

    sqlite3 *raw = NULL;
    ASSERT_EQ_INT(sqlite3_open(path, &raw), SQLITE_OK);
    const char *old =
        "CREATE TABLE contacts(id TEXT PRIMARY KEY,name TEXT NOT NULL);"
        "INSERT INTO contacts(id,name) VALUES('abc','Ana');"
        "CREATE TABLE settings(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
        "INSERT INTO settings(key,value) VALUES('stored_poll_seconds','5');"
        "CREATE TABLE schema_meta(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
        "INSERT INTO schema_meta(key,value) VALUES('schema_version','9');";
    ASSERT_EQ_INT(sqlite3_exec(raw, old, NULL, NULL, NULL), SQLITE_OK);
    ASSERT_EQ_INT(sqlite3_close(raw), SQLITE_OK);

    qtc_db db;
    ASSERT_EQ_INT(qtc_db_open(&db, path), 0);
    ASSERT_EQ_INT(qtc_db_migrate(&db), 0);
    ASSERT_EQ_INT(qtc_db_migrate(&db), 0);
    ASSERT_EQ_INT(scalar_int(db.db, "SELECT favorite FROM contacts WHERE id='abc'"), 0);
    ASSERT_EQ_INT(scalar_int(db.db, "SELECT COUNT(*) FROM pragma_table_info('contacts') WHERE name='favorite_group'"), 1);
    ASSERT_EQ_INT(scalar_int(db.db, "SELECT CAST(value AS INTEGER) FROM settings WHERE key='stored_poll_seconds'"), 1);
    ASSERT_EQ_INT(scalar_int(db.db, "SELECT CAST(value AS INTEGER) FROM schema_meta WHERE key='schema_version'"), QTC_DB_SCHEMA_VERSION);
    qtc_db_close(&db);
    unlink(path);
    rmdir(dir);
    puts("incremental database upgrade tests passed");
    return 0;
}
