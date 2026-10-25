#include "hls_uploader.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "http.h"
#include "log.h"

#define WINDOW 3          /* segments listed per playlist */
#define MAX_QUEUE 15      /* drop the oldest unsent segments beyond this backlog */
#define RATE_WINDOW_US (10 * G_USEC_PER_SEC)

typedef struct {
    GBytes *data;
    double duration;
} item;

typedef struct {
    int seq;
    char name[64];
    double duration;
} entry;

typedef struct {
    gint64 t;
    gsize bytes;
} sample;

struct hls_uploader {
    char *base;
    char prefix[32];
    int target_duration;

    GThread *thread;
    GMutex lock;
    GCond cond;
    GQueue queue;
    int stop;
    int abort_retries;

    entry window[WINDOW];
    int window_len;
    int next_seq;

    hls_stats stats;
    GQueue rate; /* of sample* */
};

static void record_rate(hls_uploader *u, gsize bytes) {
    gint64 now = g_get_monotonic_time();
    sample *s = g_new(sample, 1);
    s->t = now;
    s->bytes = bytes;
    g_queue_push_tail(&u->rate, s);
    while (!g_queue_is_empty(&u->rate) && ((sample *) g_queue_peek_head(&u->rate))->t < now - RATE_WINDOW_US) {
        g_free(g_queue_pop_head(&u->rate));
    }
}

static int compute_kbps(hls_uploader *u) {
    gint64 now = g_get_monotonic_time();
    guint64 total = 0;
    for (GList *l = u->rate.head; l; l = l->next) {
        sample *s = l->data;
        if (s->t >= now - RATE_WINDOW_US) total += s->bytes;
    }
    return (int) (total * 8 / 1000 / (RATE_WINDOW_US / G_USEC_PER_SEC));
}

static char *build_playlist(hls_uploader *u) {
    GString *s = g_string_new("#EXTM3U\n#EXT-X-VERSION:3\n");
    g_string_append_printf(s, "#EXT-X-TARGETDURATION:%d\n", u->target_duration);
    g_string_append_printf(s, "#EXT-X-MEDIA-SEQUENCE:%d\n", u->window_len ? u->window[0].seq : 0);
    for (int i = 0; i < u->window_len; i++) {
        g_string_append_printf(s, "#EXTINF:%.3f,\n%s\n", u->window[i].duration, u->window[i].name);
    }
    return g_string_free(s, FALSE);
}

/* PUT with retries. Returns the final HTTP status (0 on transport failure). */
static int put_with_retry(hls_uploader *u, http_client *http, const char *file, const char *ctype,
                          GBytes *body, int attempts) {
    char *url = g_strconcat(u->base, file, NULL);
    int status = 0;
    int delay_ms = 500;
    for (int i = 0; i < attempts; i++) {
        http_response r = http_request(http, "PUT", url, NULL, ctype, body);
        status = r.status;
        if (status == 200 || status == 202) {
            http_response_clear(&r);
            break;
        }
        LOGW("PUT %s -> %d %s", file, status, r.error ? r.error : "");
        g_mutex_lock(&u->lock);
        if (status == 0) {
            snprintf(u->stats.error, sizeof u->stats.error, "upload: %s", r.error ? r.error : "network error");
        } else {
            snprintf(u->stats.error, sizeof u->stats.error, "upload: HTTP %d", status);
        }
        int abort_now = u->abort_retries;
        g_mutex_unlock(&u->lock);
        http_response_clear(&r);
        /* 400/401/403/405 will not fix themselves. */
        if (status == 400 || status == 401 || status == 403 || status == 405 || abort_now) break;
        g_usleep((gulong) delay_ms * 1000);
        if (delay_ms < 4000) delay_ms *= 2;
    }
    g_free(url);
    return status;
}

static gpointer worker(gpointer arg) {
    hls_uploader *u = arg;
    http_client *http = http_client_new(NULL, 20);

    for (;;) {
        g_mutex_lock(&u->lock);
        while (g_queue_is_empty(&u->queue) && !u->stop) g_cond_wait(&u->cond, &u->lock);
        if (g_queue_is_empty(&u->queue)) {
            g_mutex_unlock(&u->lock);
            break;
        }
        item *it = g_queue_pop_head(&u->queue);
        u->stats.queued = (int) g_queue_get_length(&u->queue);
        int fatal = u->stats.fatal;
        g_mutex_unlock(&u->lock);
        if (fatal) {
            g_bytes_unref(it->data);
            g_free(it);
            continue;
        }

        /* Sequence numbers are assigned at send time so dropped backlog never leaves gaps. */
        entry e;
        e.seq = u->next_seq++;
        e.duration = it->duration > 0 ? it->duration : 1.0;
        snprintf(e.name, sizeof e.name, "%s_%06d.ts", u->prefix, e.seq);
        if (u->window_len == WINDOW) {
            memmove(&u->window[0], &u->window[1], sizeof(entry) * (WINDOW - 1));
            u->window_len--;
        }
        u->window[u->window_len++] = e;

        /* Announce the segment, then send it (the order in YouTube's guide). */
        char *pl = build_playlist(u);
        GBytes *plb = g_bytes_new_take(pl, strlen(pl));
        int ps = put_with_retry(u, http, "live.m3u8", "application/vnd.apple.mpegurl", plb, 3);
        g_bytes_unref(plb);

        int ss = 0;
        if (ps == 200 || ps == 202) {
            ss = put_with_retry(u, http, e.name, "video/MP2T", it->data, 5);
        }

        g_mutex_lock(&u->lock);
        int status = ps == 200 || ps == 202 ? ss : ps;
        if (status == 200 || status == 202) {
            u->stats.sent++;
            u->stats.error[0] = 0;
            record_rate(u, g_bytes_get_size(it->data));
        } else {
            u->stats.failed++;
            if (status == 401 || status == 403) {
                u->stats.fatal = 1;
                snprintf(u->stats.error, sizeof u->stats.error,
                         "ingestion rejected the stream (HTTP %d): stream key invalid or expired", status);
            }
        }
        g_mutex_unlock(&u->lock);

        g_bytes_unref(it->data);
        g_free(it);
    }

    http_client_free(http);
    return NULL;
}

hls_uploader *hls_uploader_new(const char *ingest_base, int segment_seconds) {
    hls_uploader *u = g_new0(hls_uploader, 1);
    u->base = g_strdup(ingest_base);
    /* Unique across encoder restarts. */
    snprintf(u->prefix, sizeof u->prefix, "s%" G_GINT64_FORMAT, g_get_real_time() / G_USEC_PER_SEC);
    /* EXTINF must not exceed the target duration; segments run slightly over the request. */
    u->target_duration = segment_seconds + 1;
    if (u->target_duration > 5) u->target_duration = 5;
    g_mutex_init(&u->lock);
    g_cond_init(&u->cond);
    g_queue_init(&u->queue);
    g_queue_init(&u->rate);
    u->thread = g_thread_new("hls-upload", worker, u);
    return u;
}

void hls_uploader_push(hls_uploader *u, GBytes *data, double duration_sec) {
    item *it = g_new(item, 1);
    it->data = g_bytes_ref(data);
    it->duration = duration_sec;
    g_mutex_lock(&u->lock);
    g_queue_push_tail(&u->queue, it);
    while (g_queue_get_length(&u->queue) > MAX_QUEUE) {
        item *old = g_queue_pop_head(&u->queue);
        g_bytes_unref(old->data);
        g_free(old);
        u->stats.dropped++;
    }
    u->stats.queued = (int) g_queue_get_length(&u->queue);
    g_cond_signal(&u->cond);
    g_mutex_unlock(&u->lock);
}

void hls_uploader_finish(hls_uploader *u, int timeout_ms) {
    if (!u) return;
    gint64 deadline = g_get_monotonic_time() + (gint64) timeout_ms * 1000;
    for (;;) {
        g_mutex_lock(&u->lock);
        int pending = (int) g_queue_get_length(&u->queue);
        g_mutex_unlock(&u->lock);
        if (pending == 0 || g_get_monotonic_time() > deadline) break;
        g_usleep(100 * 1000);
    }
    g_mutex_lock(&u->lock);
    u->stop = 1;
    u->abort_retries = 1;
    while (!g_queue_is_empty(&u->queue)) {
        item *it = g_queue_pop_head(&u->queue);
        g_bytes_unref(it->data);
        g_free(it);
    }
    g_cond_signal(&u->cond);
    g_mutex_unlock(&u->lock);
    g_thread_join(u->thread);

    g_queue_clear_full(&u->rate, g_free);
    g_mutex_clear(&u->lock);
    g_cond_clear(&u->cond);
    g_free(u->base);
    g_free(u);
}

void hls_uploader_get_stats(hls_uploader *u, hls_stats *out) {
    g_mutex_lock(&u->lock);
    *out = u->stats;
    out->kbps = compute_kbps(u);
    g_mutex_unlock(&u->lock);
}
