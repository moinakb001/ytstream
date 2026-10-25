#pragma once
#include <glib.h>
#include <stddef.h>

typedef struct {
    int status;     /* HTTP status, or 0 on transport failure */
    GBytes *body;   /* may be NULL */
    char *error;    /* transport error message, or NULL */
} http_response;

typedef struct http_client http_client;

/* A client owns one libsoup session (persistent HTTPS connections). Use it from one thread. */
http_client *http_client_new(const char *user_agent, int timeout_sec);
void http_client_free(http_client *c);

/* Performs a request. `bearer` may be NULL. `body` may be NULL. */
http_response http_request(http_client *c, const char *method, const char *url, const char *bearer,
                           const char *content_type, GBytes *body);
void http_response_clear(http_response *r);

/* Body as a NUL-terminated string (owned by caller). */
char *http_response_text(const http_response *r);

void http_set_user_agent(const char *ua);
const char *http_user_agent(void);
