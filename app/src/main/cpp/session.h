#pragma once

void session_init(void);
/* Returns NULL on success or an error message (caller frees). */
char *session_start(const char *config_json);
void session_stop(void);
void session_set_token(const char *token);
char *session_update_broadcast(const char *title, const char *description);
char *session_status_json(void);
