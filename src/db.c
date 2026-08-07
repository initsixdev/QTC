#include "qtc/db.h"
#include "qtc/util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int exec_sql(sqlite3 *db, const char *sql) {
    char *error = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &error);
    if (rc != SQLITE_OK) {
        qtc_log(QTC_LOG_ERROR, "sqlite: %s", error != NULL ? error : sqlite3_errmsg(db));
        sqlite3_free(error);
        return -1;
    }
    return 0;
}

static int bind_text(sqlite3_stmt *st, int index, const char *value) {
    return sqlite3_bind_text(st, index, value != NULL ? value : "", -1, SQLITE_TRANSIENT);
}

int qtc_db_open(qtc_db *db, const char *path) {
    if (db == NULL || path == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(db, 0, sizeof(*db));
    qtc_strlcpy(db->path, path, sizeof(db->path));
    int rc = sqlite3_open_v2(path, &db->db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                             NULL);
    if (rc != SQLITE_OK) {
        qtc_log(QTC_LOG_ERROR, "cannot open database %s: %s", path,
                db->db != NULL ? sqlite3_errmsg(db->db) : "unknown error");
        qtc_db_close(db);
        return -1;
    }
    sqlite3_busy_timeout(db->db, 5000);
    if (exec_sql(db->db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; "
                         "PRAGMA foreign_keys=ON; PRAGMA temp_store=MEMORY;") != 0) {
        qtc_db_close(db);
        return -1;
    }
    return 0;
}

void qtc_db_close(qtc_db *db) {
    if (db != NULL && db->db != NULL) {
        sqlite3_close(db->db);
        db->db = NULL;
    }
}

static bool table_exists(sqlite3 *db, const char *table) {
    sqlite3_stmt *st = NULL;
    bool found = false;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", -1, &st, NULL) == SQLITE_OK) {
        (void)sqlite3_bind_text(st, 1, table, -1, SQLITE_TRANSIENT);
        found = sqlite3_step(st) == SQLITE_ROW;
    }
    sqlite3_finalize(st);
    return found;
}

static bool table_has_column(sqlite3 *db, const char *table, const char *column) {
    char sql[160];
    int n = snprintf(sql, sizeof(sql), "PRAGMA table_info(\"%s\")", table);
    if (n < 0 || (size_t)n >= sizeof(sql)) return false;
    sqlite3_stmt *st = NULL;
    bool found = false;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *name = (const char *)sqlite3_column_text(st, 1);
            if (name != NULL && strcmp(name, column) == 0) { found = true; break; }
        }
    }
    sqlite3_finalize(st);
    return found;
}

static int ensure_column(sqlite3 *db, const char *table, const char *column, const char *definition) {
    if (table_has_column(db, table, column)) return 0;
    char sql[512];
    int n = snprintf(sql, sizeof(sql), "ALTER TABLE \"%s\" ADD COLUMN \"%s\" %s", table, column, definition);
    if (n < 0 || (size_t)n >= sizeof(sql)) return -1;
    return exec_sql(db, sql);
}

static int migrate_legacy_v2_tables(qtc_db *db) {
    sqlite3 *sql = db->db;
    bool legacy_contacts = table_exists(sql, "contacts") &&
                           table_has_column(sql, "contacts", "public_key") &&
                           !table_has_column(sql, "contacts", "id");
    bool legacy_channels = table_exists(sql, "channels") &&
                           table_has_column(sql, "channels", "idx") &&
                           !table_has_column(sql, "channels", "channel_index");
    bool legacy_messages = table_exists(sql, "messages") &&
                           table_has_column(sql, "messages", "kind") &&
                           !table_has_column(sql, "messages", "conversation_kind");

    if (legacy_contacts && exec_sql(sql, "ALTER TABLE contacts RENAME TO legacy_contacts_v2;") != 0) return -1;
    if (legacy_channels && exec_sql(sql, "ALTER TABLE channels RENAME TO legacy_channels_v2;") != 0) return -1;
    if (legacy_messages && exec_sql(sql, "ALTER TABLE messages RENAME TO legacy_messages_v2;") != 0) return -1;
    return 0;
}

int qtc_db_migrate(qtc_db *db) {
    static const char *schema =
        "CREATE TABLE IF NOT EXISTS schema_meta("
        " key TEXT PRIMARY KEY, value TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS contacts("
        " id TEXT PRIMARY KEY, prefix TEXT NOT NULL DEFAULT '', name TEXT NOT NULL DEFAULT '',"
        " alias TEXT NOT NULL DEFAULT '', node_type INTEGER NOT NULL DEFAULT 0,"
        " route_hops INTEGER NOT NULL DEFAULT 0, route_known INTEGER NOT NULL DEFAULT 0,"
        " favorite INTEGER NOT NULL DEFAULT 0, favorite_group TEXT NOT NULL DEFAULT '',"
        " unread INTEGER NOT NULL DEFAULT 0, last_heard INTEGER NOT NULL DEFAULT 0,"
        " latitude REAL NOT NULL DEFAULT 0, longitude REAL NOT NULL DEFAULT 0,"
        " flags INTEGER NOT NULL DEFAULT 0, updated_at INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE IF NOT EXISTS channels("
        " channel_index INTEGER PRIMARY KEY, name TEXT NOT NULL DEFAULT '', secret BLOB,"
        " configured INTEGER NOT NULL DEFAULT 0, is_private INTEGER NOT NULL DEFAULT 0,"
        " unread INTEGER NOT NULL DEFAULT 0, updated_at INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE IF NOT EXISTS messages("
        " id INTEGER PRIMARY KEY AUTOINCREMENT, conversation_kind INTEGER NOT NULL,"
        " conversation_key TEXT NOT NULL, direction INTEGER NOT NULL,"
        " sender_timestamp INTEGER NOT NULL DEFAULT 0, attempt INTEGER NOT NULL DEFAULT 0,"
        " status INTEGER NOT NULL DEFAULT 0, message_key TEXT NOT NULL UNIQUE,"
        " logical_key TEXT NOT NULL DEFAULT '', text TEXT NOT NULL DEFAULT '',"
        " part_index INTEGER NOT NULL DEFAULT 1, part_total INTEGER NOT NULL DEFAULT 1,"
        " ack_code INTEGER NOT NULL DEFAULT 0, ack_deadline INTEGER NOT NULL DEFAULT 0,"
        " snr_quarter_db INTEGER NOT NULL DEFAULT 0, path_len INTEGER NOT NULL DEFAULT -1,"
        " created_at INTEGER NOT NULL);"
        "CREATE INDEX IF NOT EXISTS messages_conv_idx ON messages(conversation_kind,conversation_key,id);"
        "CREATE INDEX IF NOT EXISTS messages_status_idx ON messages(status,direction);"
        "CREATE TABLE IF NOT EXISTS message_acks("
        " ack_code INTEGER NOT NULL, message_key TEXT NOT NULL, attempt INTEGER NOT NULL DEFAULT 0,"
        " created_at INTEGER NOT NULL, PRIMARY KEY(ack_code,message_key));"
        "CREATE INDEX IF NOT EXISTS message_acks_key_idx ON message_acks(message_key);"
        "CREATE TABLE IF NOT EXISTS invitations("
        " id INTEGER PRIMARY KEY AUTOINCREMENT, sender_contact_id TEXT NOT NULL DEFAULT '',"
        " channel_name TEXT NOT NULL, uri TEXT NOT NULL, source_message_id INTEGER NOT NULL DEFAULT 0,"
        " status INTEGER NOT NULL DEFAULT 0, received_at INTEGER NOT NULL, action_at INTEGER NOT NULL DEFAULT 0,"
        " UNIQUE(source_message_id,uri));"
        "CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY,value TEXT NOT NULL);";
    if (db == NULL || db->db == NULL) return -1;
    if (exec_sql(db->db, "BEGIN IMMEDIATE;") != 0) return -1;
    if (migrate_legacy_v2_tables(db) != 0 || exec_sql(db->db, schema) != 0) goto rollback;
    if (ensure_column(db->db, "contacts", "prefix", "TEXT NOT NULL DEFAULT ''") != 0 ||
        ensure_column(db->db, "contacts", "name", "TEXT NOT NULL DEFAULT ''") != 0 ||
        ensure_column(db->db, "contacts", "alias", "TEXT NOT NULL DEFAULT ''") != 0 ||
        ensure_column(db->db, "contacts", "node_type", "INTEGER NOT NULL DEFAULT 0") != 0 ||
        ensure_column(db->db, "contacts", "route_hops", "INTEGER NOT NULL DEFAULT 0") != 0 ||
        ensure_column(db->db, "contacts", "route_known", "INTEGER NOT NULL DEFAULT 0") != 0 ||
        ensure_column(db->db, "contacts", "favorite", "INTEGER NOT NULL DEFAULT 0") != 0 ||
        ensure_column(db->db, "contacts", "favorite_group", "TEXT NOT NULL DEFAULT ''") != 0 ||
        ensure_column(db->db, "contacts", "unread", "INTEGER NOT NULL DEFAULT 0") != 0 ||
        ensure_column(db->db, "contacts", "last_heard", "INTEGER NOT NULL DEFAULT 0") != 0 ||
        ensure_column(db->db, "contacts", "latitude", "REAL NOT NULL DEFAULT 0") != 0 ||
        ensure_column(db->db, "contacts", "longitude", "REAL NOT NULL DEFAULT 0") != 0 ||
        ensure_column(db->db, "contacts", "flags", "INTEGER NOT NULL DEFAULT 0") != 0 ||
        ensure_column(db->db, "contacts", "updated_at", "INTEGER NOT NULL DEFAULT 0") != 0 ||
        ensure_column(db->db, "channels", "secret", "BLOB") != 0 ||
        ensure_column(db->db, "channels", "configured", "INTEGER NOT NULL DEFAULT 0") != 0 ||
        ensure_column(db->db, "channels", "is_private", "INTEGER NOT NULL DEFAULT 0") != 0 ||
        ensure_column(db->db, "channels", "unread", "INTEGER NOT NULL DEFAULT 0") != 0 ||
        ensure_column(db->db, "channels", "updated_at", "INTEGER NOT NULL DEFAULT 0") != 0 ||
        ensure_column(db->db, "messages", "logical_key", "TEXT NOT NULL DEFAULT ''") != 0 ||
        ensure_column(db->db, "messages", "part_index", "INTEGER NOT NULL DEFAULT 1") != 0 ||
        ensure_column(db->db, "messages", "part_total", "INTEGER NOT NULL DEFAULT 1") != 0 ||
        ensure_column(db->db, "messages", "ack_code", "INTEGER NOT NULL DEFAULT 0") != 0 ||
        ensure_column(db->db, "messages", "ack_deadline", "INTEGER NOT NULL DEFAULT 0") != 0 ||
        ensure_column(db->db, "messages", "snr_quarter_db", "INTEGER NOT NULL DEFAULT 0") != 0 ||
        ensure_column(db->db, "messages", "path_len", "INTEGER NOT NULL DEFAULT -1") != 0) goto rollback;
    if (exec_sql(db->db,
        "CREATE INDEX IF NOT EXISTS contacts_route_idx ON contacts(node_type,route_known,route_hops);"
        "CREATE INDEX IF NOT EXISTS contacts_favorite_idx ON contacts(favorite,favorite_group);") != 0) goto rollback;

    /* QTC 2.3.1 schema 2 stored radio keys as blobs and had no channel secrets.
     * Preserve every row. Channel secrets are refreshed from the radio after connect. */
    if (table_exists(db->db, "legacy_contacts_v2")) {
        const char *import_contacts =
            "INSERT INTO contacts(id,prefix,name,alias,node_type,route_hops,route_known,favorite,"
            "favorite_group,unread,last_heard,latitude,longitude,flags,updated_at) "
            "SELECT lower(hex(public_key)),substr(lower(hex(public_key)),1,12),coalesce(name,''),"
            "coalesce(alias,''),CASE WHEN type BETWEEN 1 AND 4 THEN type ELSE 0 END,"
            "CASE WHEN out_path_len=255 THEN 0 ELSE (out_path_len & 63) END,"
            "CASE WHEN out_path_len=255 THEN 0 ELSE 1 END,coalesce(favorite,0),"
            "coalesce(favorite_group,''),coalesce(unread,0),coalesce(last_advert,0),"
            "coalesce(lat,0),coalesce(lon,0),coalesce(flags,0),coalesce(last_advert,0) "
            "FROM legacy_contacts_v2 WHERE public_key IS NOT NULL "
            "ON CONFLICT(id) DO UPDATE SET prefix=excluded.prefix,name=excluded.name,alias=excluded.alias,"
            "node_type=excluded.node_type,route_hops=excluded.route_hops,route_known=excluded.route_known,"
            "favorite=excluded.favorite,favorite_group=excluded.favorite_group,unread=excluded.unread,"
            "last_heard=excluded.last_heard,latitude=excluded.latitude,longitude=excluded.longitude,flags=excluded.flags;";
        if (exec_sql(db->db, import_contacts) != 0) goto rollback;
    }
    if (table_exists(db->db, "legacy_channels_v2")) {
        const char *import_channels =
            "INSERT INTO channels(channel_index,name,secret,configured,is_private,unread,updated_at) "
            "SELECT idx,coalesce(name,''),zeroblob(16),coalesce(configured,0),0,coalesce(unread,0),0 "
            "FROM legacy_channels_v2 WHERE 1 "
            "ON CONFLICT(channel_index) DO UPDATE SET name=excluded.name,configured=excluded.configured,"
            "unread=excluded.unread;";
        if (exec_sql(db->db, import_channels) != 0) goto rollback;
    }
    if (table_exists(db->db, "legacy_messages_v2")) {
        const char *import_messages =
            "INSERT OR IGNORE INTO messages(id,conversation_kind,conversation_key,direction,sender_timestamp,"
            "attempt,status,message_key,text,created_at) "
            "SELECT id,CASE WHEN peer_key IS NOT NULL AND length(peer_key)>0 THEN 1 ELSE 2 END,"
            "CASE WHEN peer_key IS NOT NULL AND length(peer_key)>0 THEN lower(hex(peer_key)) ELSE cast(channel_idx AS TEXT) END,"
            "coalesce(direction,0),coalesce(timestamp,0),0,"
            "CASE WHEN status BETWEEN 0 AND 5 THEN status ELSE 2 END,"
            "'legacy:'||id||':'||coalesce(timestamp,0),coalesce(text,''),coalesce(timestamp,0) "
            "FROM legacy_messages_v2;";
        if (exec_sql(db->db, import_messages) != 0) goto rollback;
    }
    if (exec_sql(db->db,
        "INSERT INTO schema_meta(key,value) VALUES('legacy_v2_imported','1') ON CONFLICT(key) DO NOTHING;"
        "UPDATE settings SET value='1' WHERE key='stored_poll_seconds' AND value='5' "
        "AND NOT EXISTS (SELECT 1 FROM schema_meta WHERE key='schema_version' AND CAST(value AS INTEGER)>=10);"
        "INSERT INTO schema_meta(key,value) VALUES('schema_version','10') "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value;"
        "COMMIT;") != 0) goto rollback_no_tx;
    return 0;
rollback:
    (void)exec_sql(db->db, "ROLLBACK;");
    return -1;
rollback_no_tx:
    return -1;
}

static void settings_defaults(qtc_settings *s) {
    memset(s, 0, sizeof(*s));
    s->desktop_notifications = true;
    s->notify_direct = true;
    s->notify_channel = true;
    s->suppress_open_conversation = true;
    s->sound_enabled = true;
    s->banner_enabled = true;
    s->show_signal = true;
    s->stored_poll_seconds = 1;
    s->retry_unconfirmed = true;
    s->max_direct_attempts = 3;
    s->reset_stale_route = true;
    s->theme = 0;
    s->tx_power = 0;
}

static bool parse_bool(const char *v, bool fallback) {
    if (v == NULL) return fallback;
    if (strcmp(v, "1") == 0 || strcasecmp(v, "true") == 0 || strcasecmp(v, "yes") == 0) return true;
    if (strcmp(v, "0") == 0 || strcasecmp(v, "false") == 0 || strcasecmp(v, "no") == 0) return false;
    return fallback;
}

int qtc_db_load_settings(qtc_db *db, qtc_settings *settings) {
    if (db == NULL || db->db == NULL || settings == NULL) return -1;
    settings_defaults(settings);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->db, "SELECT key,value FROM settings", -1, &st, NULL) != SQLITE_OK) return -1;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *k = (const char *)sqlite3_column_text(st, 0);
        const char *v = (const char *)sqlite3_column_text(st, 1);
        if (strcmp(k, "desktop_notifications") == 0) settings->desktop_notifications = parse_bool(v, settings->desktop_notifications);
        else if (strcmp(k, "notify_direct") == 0) settings->notify_direct = parse_bool(v, settings->notify_direct);
        else if (strcmp(k, "notify_channel") == 0) settings->notify_channel = parse_bool(v, settings->notify_channel);
        else if (strcmp(k, "suppress_open_conversation") == 0) settings->suppress_open_conversation = parse_bool(v, settings->suppress_open_conversation);
        else if (strcmp(k, "sound_enabled") == 0) settings->sound_enabled = parse_bool(v, settings->sound_enabled);
        else if (strcmp(k, "banner_enabled") == 0) settings->banner_enabled = parse_bool(v, settings->banner_enabled);
        else if (strcmp(k, "show_signal") == 0) settings->show_signal = parse_bool(v, settings->show_signal);
        else if (strcmp(k, "stored_poll_seconds") == 0) settings->stored_poll_seconds = atoi(v);
        else if (strcmp(k, "retry_unconfirmed") == 0) settings->retry_unconfirmed = parse_bool(v, settings->retry_unconfirmed);
        else if (strcmp(k, "max_direct_attempts") == 0) settings->max_direct_attempts = atoi(v);
        else if (strcmp(k, "reset_stale_route") == 0) settings->reset_stale_route = parse_bool(v, settings->reset_stale_route);
        else if (strcmp(k, "theme") == 0) settings->theme = atoi(v);
        else if (strcmp(k, "tx_power") == 0) settings->tx_power = atoi(v);
        else if (strcmp(k, "serial_device") == 0) qtc_strlcpy(settings->serial_device, v, sizeof(settings->serial_device));
    }
    sqlite3_finalize(st);
    if (settings->stored_poll_seconds < 1) settings->stored_poll_seconds = 1;
    if (settings->max_direct_attempts < 1) settings->max_direct_attempts = 3;
    return 0;
}

int qtc_db_load_state(qtc_db *db, qtc_state *state) {
    if (db == NULL || db->db == NULL || state == NULL) return -1;
    memset(state, 0, sizeof(*state));
    if (qtc_db_load_settings(db, &state->settings) != 0) return -1;

    sqlite3_stmt *st = NULL;
    const char *contact_sql = "SELECT id,prefix,name,alias,node_type,route_hops,route_known,favorite,"
                              "favorite_group,unread,last_heard,latitude,longitude,flags FROM contacts "
                              "ORDER BY id LIMIT ?";
    if (sqlite3_prepare_v2(db->db, contact_sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(st, 1, QTC_MAX_CONTACTS);
    while (state->contact_count < QTC_MAX_CONTACTS && sqlite3_step(st) == SQLITE_ROW) {
        qtc_contact *c = &state->contacts[state->contact_count++];
        qtc_strlcpy(c->id, (const char *)sqlite3_column_text(st, 0), sizeof(c->id));
        qtc_strlcpy(c->prefix, (const char *)sqlite3_column_text(st, 1), sizeof(c->prefix));
        qtc_strlcpy(c->name, (const char *)sqlite3_column_text(st, 2), sizeof(c->name));
        qtc_strlcpy(c->alias, (const char *)sqlite3_column_text(st, 3), sizeof(c->alias));
        c->node_type = (qtc_node_type)sqlite3_column_int(st, 4);
        c->route_hops = sqlite3_column_int(st, 5);
        c->route_known = sqlite3_column_int(st, 6) != 0;
        c->favorite = sqlite3_column_int(st, 7) != 0;
        qtc_strlcpy(c->favorite_group, (const char *)sqlite3_column_text(st, 8), sizeof(c->favorite_group));
        c->unread = sqlite3_column_int(st, 9);
        c->last_heard = sqlite3_column_int64(st, 10);
        c->latitude = sqlite3_column_double(st, 11);
        c->longitude = sqlite3_column_double(st, 12);
        c->flags = (uint8_t)sqlite3_column_int(st, 13);
    }
    sqlite3_finalize(st);

    const char *channel_sql = "SELECT channel_index,name,secret,configured,is_private,unread FROM channels "
                              "ORDER BY channel_index LIMIT ?";
    if (sqlite3_prepare_v2(db->db, channel_sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(st, 1, QTC_MAX_CHANNELS);
    while (state->channel_count < QTC_MAX_CHANNELS && sqlite3_step(st) == SQLITE_ROW) {
        qtc_channel *c = &state->channels[state->channel_count++];
        c->index = sqlite3_column_int(st, 0);
        qtc_strlcpy(c->name, (const char *)sqlite3_column_text(st, 1), sizeof(c->name));
        const void *secret = sqlite3_column_blob(st, 2);
        int n = sqlite3_column_bytes(st, 2);
        if (secret != NULL && n == 16) memcpy(c->secret, secret, 16);
        c->configured = sqlite3_column_int(st, 3) != 0;
        c->is_private = sqlite3_column_int(st, 4) != 0;
        c->unread = sqlite3_column_int(st, 5);
    }
    sqlite3_finalize(st);

    const char *message_sql = "SELECT id,conversation_kind,conversation_key,direction,sender_timestamp,attempt,"
                              "status,message_key,logical_key,text,part_index,part_total,ack_code,ack_deadline,"
                              "snr_quarter_db,path_len,created_at FROM (SELECT id,conversation_kind,conversation_key,"
                              "direction,sender_timestamp,attempt,status,message_key,logical_key,text,part_index,part_total,"
                              "ack_code,ack_deadline,snr_quarter_db,path_len,created_at FROM messages "
                              "ORDER BY id DESC LIMIT ?) ORDER BY id ASC";
    if (sqlite3_prepare_v2(db->db, message_sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(st, 1, QTC_MAX_MESSAGES);
    while (state->message_count < QTC_MAX_MESSAGES && sqlite3_step(st) == SQLITE_ROW) {
        qtc_message *m = &state->messages[state->message_count++];
        m->id = sqlite3_column_int64(st, 0);
        m->conversation_kind = (qtc_conversation_kind)sqlite3_column_int(st, 1);
        qtc_strlcpy(m->conversation_key, (const char *)sqlite3_column_text(st, 2), sizeof(m->conversation_key));
        m->direction = (qtc_message_direction)sqlite3_column_int(st, 3);
        m->sender_timestamp = sqlite3_column_int64(st, 4);
        m->attempt = sqlite3_column_int(st, 5);
        m->status = (qtc_message_status)sqlite3_column_int(st, 6);
        qtc_strlcpy(m->message_key, (const char *)sqlite3_column_text(st, 7), sizeof(m->message_key));
        qtc_strlcpy(m->logical_key, (const char *)sqlite3_column_text(st, 8), sizeof(m->logical_key));
        qtc_strlcpy(m->text, (const char *)sqlite3_column_text(st, 9), sizeof(m->text));
        m->part_index = sqlite3_column_int(st, 10);
        m->part_total = sqlite3_column_int(st, 11);
        m->ack_code = (uint32_t)sqlite3_column_int64(st, 12);
        m->ack_deadline = sqlite3_column_int64(st, 13);
        m->snr_quarter_db = sqlite3_column_int(st, 14);
        m->path_len = sqlite3_column_int(st, 15);
        m->created_at = sqlite3_column_int64(st, 16);
        if (m->logical_key[0] == 0) qtc_strlcpy(m->logical_key, m->message_key, sizeof(m->logical_key));
        if (m->part_index < 1) m->part_index = 1;
        if (m->part_total < 1) m->part_total = 1;
    }
    sqlite3_finalize(st);

    const char *inv_sql = "SELECT id,sender_contact_id,channel_name,uri,source_message_id,status,received_at,action_at "
                          "FROM invitations ORDER BY id DESC LIMIT ?";
    if (sqlite3_prepare_v2(db->db, inv_sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(st, 1, QTC_MAX_INVITATIONS);
    while (state->invitation_count < QTC_MAX_INVITATIONS && sqlite3_step(st) == SQLITE_ROW) {
        qtc_invitation *i = &state->invitations[state->invitation_count++];
        i->id = sqlite3_column_int64(st, 0);
        qtc_strlcpy(i->sender_contact_id, (const char *)sqlite3_column_text(st, 1), sizeof(i->sender_contact_id));
        qtc_strlcpy(i->channel_name, (const char *)sqlite3_column_text(st, 2), sizeof(i->channel_name));
        qtc_strlcpy(i->uri, (const char *)sqlite3_column_text(st, 3), sizeof(i->uri));
        i->source_message_id = sqlite3_column_int64(st, 4);
        i->status = (qtc_invite_status)sqlite3_column_int(st, 5);
        i->received_at = sqlite3_column_int64(st, 6);
        i->action_at = sqlite3_column_int64(st, 7);
    }
    sqlite3_finalize(st);
    state->revisions.contacts = 1;
    state->revisions.channels = 1;
    state->revisions.messages = 1;
    state->revisions.settings = 1;
    state->radio_max_channels = QTC_MAX_CHANNELS;
    state->radio_max_contacts = QTC_MAX_CONTACTS;
    return 0;
}

int qtc_db_upsert_contact(qtc_db *db, const qtc_contact *c) {
    const char *sql = "INSERT INTO contacts(id,prefix,name,alias,node_type,route_hops,route_known,favorite,"
                      "favorite_group,unread,last_heard,latitude,longitude,flags,updated_at)"
                      " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
                      " ON CONFLICT(id) DO UPDATE SET prefix=excluded.prefix,name=excluded.name,"
                      "node_type=excluded.node_type,route_hops=excluded.route_hops,route_known=excluded.route_known,"
                      "last_heard=MAX(contacts.last_heard,excluded.last_heard),latitude=excluded.latitude,"
                      "longitude=excluded.longitude,flags=excluded.flags,updated_at=excluded.updated_at";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    bind_text(st, 1, c->id); bind_text(st, 2, c->prefix); bind_text(st, 3, c->name); bind_text(st, 4, c->alias);
    sqlite3_bind_int(st, 5, c->node_type); sqlite3_bind_int(st, 6, c->route_hops);
    sqlite3_bind_int(st, 7, c->route_known); sqlite3_bind_int(st, 8, c->favorite);
    bind_text(st, 9, c->favorite_group); sqlite3_bind_int(st, 10, c->unread);
    sqlite3_bind_int64(st, 11, c->last_heard); sqlite3_bind_double(st, 12, c->latitude);
    sqlite3_bind_double(st, 13, c->longitude); sqlite3_bind_int(st, 14, c->flags);
    sqlite3_bind_int64(st, 15, qtc_now_seconds());
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int qtc_db_set_contact_favorite(qtc_db *db, const char *id, bool favorite, const char *group) {
    sqlite3_stmt *st = NULL;
    const char *sql = "UPDATE contacts SET favorite=?,favorite_group=? WHERE id=?";
    if (sqlite3_prepare_v2(db->db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(st, 1, favorite); bind_text(st, 2, favorite ? group : ""); bind_text(st, 3, id);
    int rc = sqlite3_step(st); sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int qtc_db_set_contact_alias(qtc_db *db, const char *id, const char *alias) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->db, "UPDATE contacts SET alias=? WHERE id=?", -1, &st, NULL) != SQLITE_OK) return -1;
    bind_text(st, 1, alias); bind_text(st, 2, id);
    int rc = sqlite3_step(st); sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int qtc_db_mark_contact_read(qtc_db *db, const char *id) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->db, "UPDATE contacts SET unread=0 WHERE id=?", -1, &st, NULL) != SQLITE_OK) return -1;
    bind_text(st, 1, id); int rc = sqlite3_step(st); sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int qtc_db_upsert_channel(qtc_db *db, const qtc_channel *c) {
    const char *sql = "INSERT INTO channels(channel_index,name,secret,configured,is_private,unread,updated_at)"
                      " VALUES(?,?,?,?,?,?,?) ON CONFLICT(channel_index) DO UPDATE SET name=excluded.name,"
                      "secret=excluded.secret,configured=excluded.configured,is_private=excluded.is_private,"
                      "updated_at=excluded.updated_at";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(st, 1, c->index); bind_text(st, 2, c->name);
    sqlite3_bind_blob(st, 3, c->secret, 16, SQLITE_TRANSIENT); sqlite3_bind_int(st, 4, c->configured);
    sqlite3_bind_int(st, 5, c->is_private); sqlite3_bind_int(st, 6, c->unread);
    sqlite3_bind_int64(st, 7, qtc_now_seconds());
    int rc = sqlite3_step(st); sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int qtc_db_remove_channel(qtc_db *db, int channel_index) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->db, "DELETE FROM channels WHERE channel_index=?", -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(st, 1, channel_index); int rc = sqlite3_step(st); sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int qtc_db_mark_channel_read(qtc_db *db, int channel_index) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->db, "UPDATE channels SET unread=0 WHERE channel_index=?", -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(st, 1, channel_index); int rc = sqlite3_step(st); sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int qtc_db_insert_message(qtc_db *db, qtc_message *m, bool *inserted) {
    if (inserted != NULL) *inserted = false;
    const char *sql = "INSERT OR IGNORE INTO messages(conversation_kind,conversation_key,direction,"
                      "sender_timestamp,attempt,status,message_key,logical_key,text,part_index,part_total,"
                      "ack_code,ack_deadline,snr_quarter_db,path_len,created_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(st, 1, m->conversation_kind); bind_text(st, 2, m->conversation_key);
    sqlite3_bind_int(st, 3, m->direction); sqlite3_bind_int64(st, 4, m->sender_timestamp);
    sqlite3_bind_int(st, 5, m->attempt); sqlite3_bind_int(st, 6, m->status);
    bind_text(st, 7, m->message_key);
    bind_text(st, 8, m->logical_key[0] ? m->logical_key : m->message_key);
    bind_text(st, 9, m->text);
    sqlite3_bind_int(st, 10, m->part_index > 0 ? m->part_index : 1);
    sqlite3_bind_int(st, 11, m->part_total > 0 ? m->part_total : 1);
    sqlite3_bind_int64(st, 12, (sqlite3_int64)m->ack_code);
    sqlite3_bind_int64(st, 13, m->ack_deadline);
    sqlite3_bind_int(st, 14, m->snr_quarter_db);
    sqlite3_bind_int(st, 15, m->path_len);
    sqlite3_bind_int64(st, 16, m->created_at != 0 ? m->created_at : qtc_now_seconds());
    int rc = sqlite3_step(st);
    if (rc == SQLITE_DONE && sqlite3_changes(db->db) > 0) {
        m->id = sqlite3_last_insert_rowid(db->db);
        if (inserted != NULL) *inserted = true;
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return -1;

    bool first_logical_part = true;
    if (inserted != NULL && *inserted && m->direction == QTC_MSG_INCOMING) {
        sqlite3_stmt *count_stmt = NULL;
        if (sqlite3_prepare_v2(db->db, "SELECT count(*) FROM messages WHERE logical_key=?", -1,
                               &count_stmt, NULL) == SQLITE_OK) {
            bind_text(count_stmt, 1, m->logical_key[0] ? m->logical_key : m->message_key);
            if (sqlite3_step(count_stmt) == SQLITE_ROW)
                first_logical_part = sqlite3_column_int(count_stmt, 0) == 1;
            sqlite3_finalize(count_stmt);
        }
    }
    if (inserted != NULL && *inserted && m->direction == QTC_MSG_INCOMING && first_logical_part) {
        sqlite3_stmt *u = NULL;
        if (m->conversation_kind == QTC_CONV_CONTACT) {
            if (sqlite3_prepare_v2(db->db, "UPDATE contacts SET unread=unread+1 WHERE id=?", -1, &u, NULL) == SQLITE_OK) {
                bind_text(u, 1, m->conversation_key); (void)sqlite3_step(u); sqlite3_finalize(u);
            }
        } else {
            if (sqlite3_prepare_v2(db->db, "UPDATE channels SET unread=unread+1 WHERE channel_index=?", -1, &u, NULL) == SQLITE_OK) {
                sqlite3_bind_int(u, 1, atoi(m->conversation_key)); (void)sqlite3_step(u); sqlite3_finalize(u);
            }
        }
    }
    return 0;
}

int qtc_db_update_message_status(qtc_db *db, const char *key, qtc_message_status status) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->db, "UPDATE messages SET status=? WHERE message_key=?", -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(st, 1, status); bind_text(st, 2, key);
    int rc = sqlite3_step(st); sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int qtc_db_update_message_send_state(qtc_db *db, const char *key,
                                     qtc_message_status status, int attempt,
                                     uint32_t ack_code, int64_t ack_deadline) {
    sqlite3_stmt *st = NULL;
    const char *sql = "UPDATE messages SET status=?,attempt=?,ack_code=?,ack_deadline=? WHERE message_key=?";
    if (sqlite3_prepare_v2(db->db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(st, 1, status);
    sqlite3_bind_int(st, 2, attempt);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)ack_code);
    sqlite3_bind_int64(st, 4, ack_deadline);
    bind_text(st, 5, key);
    int rc = sqlite3_step(st); sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int qtc_db_record_message_ack(qtc_db *db, const char *message_key,
                              uint32_t ack_code, int attempt) {
    if (db == NULL || db->db == NULL || message_key == NULL || *message_key == 0 || ack_code == 0)
        return -1;
    sqlite3_stmt *st = NULL;
    const char *sql = "INSERT INTO message_acks(ack_code,message_key,attempt,created_at) VALUES(?,?,?,?) "
                      "ON CONFLICT(ack_code,message_key) DO UPDATE SET attempt=excluded.attempt,"
                      "created_at=excluded.created_at";
    if (sqlite3_prepare_v2(db->db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)ack_code);
    bind_text(st, 2, message_key);
    sqlite3_bind_int(st, 3, attempt);
    sqlite3_bind_int64(st, 4, qtc_now_seconds());
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int qtc_db_mark_ack_delivered(qtc_db *db, uint32_t ack_code, int *changed) {
    if (changed != NULL) *changed = 0;
    if (db == NULL || db->db == NULL || ack_code == 0) return -1;
    sqlite3_stmt *st = NULL;
    const char *sql = "UPDATE messages SET status=?,ack_code=0,ack_deadline=0 "
                      "WHERE message_key IN (SELECT message_key FROM message_acks WHERE ack_code=?) "
                      "AND direction=? AND status IN (?,?,?,?,?)";
    if (sqlite3_prepare_v2(db->db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(st, 1, QTC_MSG_DELIVERED);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)ack_code);
    sqlite3_bind_int(st, 3, QTC_MSG_OUTGOING);
    sqlite3_bind_int(st, 4, QTC_MSG_SENT);
    sqlite3_bind_int(st, 5, QTC_MSG_UNCONFIRMED);
    sqlite3_bind_int(st, 6, QTC_MSG_FAILED);
    sqlite3_bind_int(st, 7, QTC_MSG_SENDING);
    sqlite3_bind_int(st, 8, QTC_MSG_QUEUED);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_DONE && changed != NULL) *changed = sqlite3_changes(db->db);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int qtc_db_get_ack_message_keys(qtc_db *db, uint32_t ack_code,
                                char keys[][160], size_t max_keys, size_t *count) {
    if (count != NULL) *count = 0;
    if (db == NULL || db->db == NULL || ack_code == 0 || keys == NULL ||
        max_keys == 0 || count == NULL) return -1;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->db,
                           "SELECT message_key FROM message_acks WHERE ack_code=? ORDER BY created_at",
                           -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)ack_code);
    while (*count < max_keys && sqlite3_step(st) == SQLITE_ROW) {
        const char *key = (const char *)sqlite3_column_text(st, 0);
        qtc_strlcpy(keys[*count], key != NULL ? key : "", 160);
        (*count)++;
    }
    sqlite3_finalize(st);
    return 0;
}

int qtc_db_save_setting(qtc_db *db, const char *key, const char *value) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->db, "INSERT INTO settings(key,value) VALUES(?,?) "
                                  "ON CONFLICT(key) DO UPDATE SET value=excluded.value", -1, &st, NULL) != SQLITE_OK) return -1;
    bind_text(st, 1, key); bind_text(st, 2, value); int rc = sqlite3_step(st); sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int qtc_db_get_setting(qtc_db *db, const char *key, char *value, size_t value_len) {
    if (db == NULL || db->db == NULL || key == NULL || value == NULL || value_len == 0)
        return -1;
    value[0] = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->db, "SELECT value FROM settings WHERE key=?", -1,
                           &st, NULL) != SQLITE_OK) return -1;
    bind_text(st, 1, key);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        const char *text = (const char *)sqlite3_column_text(st, 0);
        qtc_strlcpy(value, text != NULL ? text : "", value_len);
        sqlite3_finalize(st);
        return 0;
    }
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 1 : -1;
}

int qtc_db_insert_invitation(qtc_db *db, qtc_invitation *i) {
    const char *sql = "INSERT OR IGNORE INTO invitations(sender_contact_id,channel_name,uri,source_message_id,"
                      "status,received_at,action_at) VALUES(?,?,?,?,?,?,?)";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    bind_text(st, 1, i->sender_contact_id); bind_text(st, 2, i->channel_name); bind_text(st, 3, i->uri);
    sqlite3_bind_int64(st, 4, i->source_message_id); sqlite3_bind_int(st, 5, i->status);
    sqlite3_bind_int64(st, 6, i->received_at); sqlite3_bind_int64(st, 7, i->action_at);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_DONE && sqlite3_changes(db->db) > 0) i->id = sqlite3_last_insert_rowid(db->db);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int qtc_db_update_invitation_status(qtc_db *db, int64_t id, qtc_invite_status status) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->db, "UPDATE invitations SET status=?,action_at=? WHERE id=?", -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(st, 1, status); sqlite3_bind_int64(st, 2, qtc_now_seconds()); sqlite3_bind_int64(st, 3, id);
    int rc = sqlite3_step(st); sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

static int seed_contact(qtc_db *db, const char *id, const char *name, qtc_node_type type,
                        bool known, int hops, bool favorite, const char *group) {
    qtc_contact c = {0};
    qtc_strlcpy(c.id, id, sizeof(c.id));
    qtc_strlcpy(c.prefix, id, sizeof(c.prefix));
    qtc_strlcpy(c.name, name, sizeof(c.name));
    c.node_type = type; c.route_known = known; c.route_hops = hops; c.favorite = favorite;
    qtc_strlcpy(c.favorite_group, group, sizeof(c.favorite_group));
    c.last_heard = qtc_now_seconds() - hops * 420;
    return qtc_db_upsert_contact(db, &c);
}

int qtc_db_seed_demo(qtc_db *db) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->db, "SELECT COUNT(*) FROM contacts", -1, &st, NULL) != SQLITE_OK) return -1;
    int count = 0; if (sqlite3_step(st) == SQLITE_ROW) count = sqlite3_column_int(st, 0); sqlite3_finalize(st);
    if (count > 0) return 0;
    if (exec_sql(db->db, "BEGIN IMMEDIATE") != 0) return -1;
    int rc = 0;
    rc |= seed_contact(db, "a1b2c3d4e5f6", "Ana", QTC_NODE_PERSON, true, 0, true, "Family");
    rc |= seed_contact(db, "b1c2d3e4f5a6", "Marko", QTC_NODE_PERSON, true, 1, true, "Local Mesh");
    rc |= seed_contact(db, "c1d2e3f4a5b6", "HR OS Branimir", QTC_NODE_PERSON, true, 2, false, "");
    rc |= seed_contact(db, "d1e2f3a4b5c6", "Ivana", QTC_NODE_PERSON, false, 0, false, "");
    rc |= seed_contact(db, "111122223333", "Darda Repeater", QTC_NODE_REPEATER, true, 1, false, "");
    rc |= seed_contact(db, "444455556666", "OS Room", QTC_NODE_ROOM, true, 2, false, "");
    qtc_channel ch = {.index = 0, .configured = true, .is_private = false};
    qtc_strlcpy(ch.name, "Public", sizeof(ch.name)); rc |= qtc_db_upsert_channel(db, &ch);
    memset(&ch, 0, sizeof(ch)); ch.index = 1; ch.configured = true; ch.is_private = true;
    qtc_strlcpy(ch.name, "Family", sizeof(ch.name)); (void)qtc_secure_random(ch.secret, 16); rc |= qtc_db_upsert_channel(db, &ch);
    if (rc != 0 || exec_sql(db->db, "COMMIT") != 0) { (void)exec_sql(db->db, "ROLLBACK"); return -1; }
    return 0;
}
