#include "http.h"

#include <libsoup/soup.h>
#include <string.h>

struct http_client {
    SoupSession *session;
};

static char *g_user_agent;

void http_set_user_agent(const char *ua) {
    g_free(g_user_agent);
    g_user_agent = g_strdup(ua);
}

const char *http_user_agent(void) {
    return g_user_agent ? g_user_agent : "ytstream / Android / 1.0";
}

http_client *http_client_new(const char *user_agent, int timeout_sec) {
    http_client *c = g_new0(http_client, 1);
    c->session = soup_session_new_with_options(
        "user-agent", user_agent ? user_agent : http_user_agent(),
        "timeout", (guint) timeout_sec,
        "idle-timeout", (guint) 60,
        NULL);
    return c;
}

void http_client_free(http_client *c) {
    if (!c) return;
    soup_session_abort(c->session);
    g_object_unref(c->session);
    g_free(c);
}

http_response http_request(http_client *c, const char *method, const char *url, const char *bearer,
                           const char *content_type, GBytes *body) {
    http_response r = {0};
    SoupMessage *msg = soup_message_new(method, url);
    if (!msg) {
        r.error = g_strdup_printf("invalid URL");
        return r;
    }
    SoupMessageHeaders *h = soup_message_get_request_headers(msg);
    if (bearer) {
        char *auth = g_strdup_printf("Bearer %s", bearer);
        soup_message_headers_replace(h, "Authorization", auth);
        g_free(auth);
    }
    if (body) {
        soup_message_set_request_body_from_bytes(msg, content_type ? content_type : "application/octet-stream", body);
    } else if (!strcmp(method, "POST") || !strcmp(method, "PUT")) {
        soup_message_headers_set_content_length(h, 0);
    }

    GError *err = NULL;
    GBytes *resp = soup_session_send_and_read(c->session, msg, NULL, &err);
    if (err) {
        r.error = g_strdup(err->message);
        g_error_free(err);
    }
    /* A status is kept even when reading the body failed afterwards. */
    r.status = (int) soup_message_get_status(msg);
    if (r.status < 100) r.status = 0;
    r.body = resp;
    g_object_unref(msg);
    return r;
}

void http_response_clear(http_response *r) {
    if (r->body) g_bytes_unref(r->body);
    g_free(r->error);
    memset(r, 0, sizeof *r);
}

char *http_response_text(const http_response *r) {
    if (!r->body) return g_strdup("");
    gsize n = 0;
    const char *d = g_bytes_get_data(r->body, &n);
    return g_strndup(d, n);
}
