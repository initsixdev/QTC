#ifndef QTC_DB_H
#define QTC_DB_H

#include "qtc/qtc.h"

int qtc_db_open(qtc_db *db, const char *path);
void qtc_db_close(qtc_db *db);
int qtc_db_migrate(qtc_db *db);
int qtc_db_load_state(qtc_db *db, qtc_state *state);
int qtc_db_seed_demo(qtc_db *db);
int qtc_db_upsert_contact(qtc_db *db, const qtc_contact *contact);
int qtc_db_set_contact_favorite(qtc_db *db, const char *id, bool favorite, const char *group);
int qtc_db_set_contact_alias(qtc_db *db, const char *id, const char *alias);
int qtc_db_mark_contact_read(qtc_db *db, const char *id);
int qtc_db_upsert_channel(qtc_db *db, const qtc_channel *channel);
int qtc_db_remove_channel(qtc_db *db, int channel_index);
int qtc_db_mark_channel_read(qtc_db *db, int channel_index);
int qtc_db_insert_message(qtc_db *db, qtc_message *message, bool *inserted);
int qtc_db_update_message_status(qtc_db *db, const char *message_key, qtc_message_status status);
int qtc_db_update_message_send_state(qtc_db *db, const char *message_key,
                                     qtc_message_status status, int attempt,
                                     uint32_t ack_code, int64_t ack_deadline);
int qtc_db_record_message_ack(qtc_db *db, const char *message_key,
                              uint32_t ack_code, int attempt);
int qtc_db_mark_ack_delivered(qtc_db *db, uint32_t ack_code, int *changed);
int qtc_db_get_ack_message_keys(qtc_db *db, uint32_t ack_code,
                                char keys[][160], size_t max_keys, size_t *count);
int qtc_db_save_setting(qtc_db *db, const char *key, const char *value);
int qtc_db_get_setting(qtc_db *db, const char *key, char *value, size_t value_len);
int qtc_db_load_settings(qtc_db *db, qtc_settings *settings);
int qtc_db_insert_invitation(qtc_db *db, qtc_invitation *invitation);
int qtc_db_update_invitation_status(qtc_db *db, int64_t id, qtc_invite_status status);

#endif
