/*
 * Live session orchestration: YouTube API setup, engine + uploader lifecycle, status.
 *
 * startStream() config JSON (all optional unless noted):
 *   mode            "account" (create broadcast via API) | "key" (use an HLS stream key)
 *   accessToken     OAuth token with youtube scope (account mode)
 *   streamKey       HLS stream key from YouTube Studio (key mode)
 *   title, description, privacy, categoryId, tags[], latency, madeForKids, dvr, autoStop,
 *   thumbnailPath   broadcast metadata (account mode)
 *   width, height, fps, videoBitrateKbps, audioBitrateKbps, segmentSeconds, encoder
 *   audioDeviceId   -1 silent, 0 default input, >0 AudioDeviceInfo id
 */
#include "session.h"

#include <cJSON.h>
#include <glib.h>
#include <string.h>

#include "engine.h"
#include "hls_uploader.h"
#include "log.h"
#include "youtube.h"

typedef struct {
    int account;
    char *stream_key;
    char *title, *description, *privacy, *category_id, *latency, *thumbnail;
    gchar **tags;
    int made_for_kids, dvr, auto_stop;
    int width, height, fps, vbitrate, abitrate, segment;
    char *encoder;
    int audio_device;
} config;

static struct {
    GMutex lock;
    GCond cond;
    const char *phase;
    char message[256];
    char error[512];
    char youtube[256];
    char watch_url[128];
    char *token;
    int stop_requested;
    int running;
    GThread *thread;
    config cfg;
    yt_live live;
    hls_uploader *up;
    char engine_error[256];
} S = {.phase = "idle"};

void session_init(void) {
    g_mutex_init(&S.lock);
    g_cond_init(&S.cond);
}

static void config_clear(config *c) {
    g_free(c->stream_key);
    g_free(c->title);
    g_free(c->description);
    g_free(c->privacy);
    g_free(c->category_id);
    g_free(c->latency);
    g_free(c->thumbnail);
    g_strfreev(c->tags);
    g_free(c->encoder);
    memset(c, 0, sizeof *c);
}

static char *jdup(cJSON *o, const char *k, const char *def) {
    cJSON *v = cJSON_GetObjectItem(o, k);
    return g_strdup(cJSON_IsString(v) ? v->valuestring : def);
}

static int jint(cJSON *o, const char *k, int def) {
    cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

static int jbool(cJSON *o, const char *k, int def) {
    cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int valid_stream_key(const char *k) {
    if (!k || !*k) return 0;
    for (const char *p = k; *p; p++) {
        if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_') return 0;
    }
    return 1;
}

static void set_phase(const char *phase, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void set_phase(const char *phase, const char *fmt, ...) {
    g_mutex_lock(&S.lock);
    S.phase = phase;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        g_vsnprintf(S.message, sizeof S.message, fmt, ap);
        va_end(ap);
    } else {
        S.message[0] = 0;
    }
    g_mutex_unlock(&S.lock);
}

static void set_error(const char *msg) {
    g_mutex_lock(&S.lock);
    g_strlcpy(S.error, msg, sizeof S.error);
    g_mutex_unlock(&S.lock);
    LOGE("session error: %s", msg);
}

static char *token_copy(void) {
    g_mutex_lock(&S.lock);
    char *t = g_strdup(S.token);
    g_mutex_unlock(&S.lock);
    return t;
}

static void on_segment(GBytes *data, double dur, void *user) {
    (void) user;
    hls_uploader_push(S.up, data, dur);
}

static void on_engine_error(const char *msg, void *user) {
    (void) user;
    g_mutex_lock(&S.lock);
    if (!S.engine_error[0]) g_strlcpy(S.engine_error, msg, sizeof S.engine_error);
    g_cond_broadcast(&S.cond);
    g_mutex_unlock(&S.lock);
}

static yt_broadcast_settings settings_from(const config *c) {
    yt_broadcast_settings s = {
        .title = c->title, .description = c->description, .privacy = c->privacy,
        .latency = c->latency, .category_id = c->category_id, .tags = (const char *const *) c->tags,
        .made_for_kids = c->made_for_kids, .dvr = c->dvr, .auto_stop = c->auto_stop};
    return s;
}

/* Waits up to `ms` for a stop request or engine error. Returns non-zero if we should stop. */
static int wait_or_stop(int ms) {
    gint64 deadline = g_get_monotonic_time() + (gint64) ms * 1000;
    g_mutex_lock(&S.lock);
    while (!S.stop_requested && !S.engine_error[0]) {
        if (!g_cond_wait_until(&S.cond, &S.lock, deadline)) break;
    }
    int stop = S.stop_requested || S.engine_error[0];
    g_mutex_unlock(&S.lock);
    return stop;
}

static gpointer session_thread(gpointer arg) {
    (void) arg;
    config *c = &S.cfg;
    char *ingest = NULL;
    char *err = NULL;
    int engine_started = 0;

    if (c->account) {
        set_phase("starting", "Creating YouTube broadcast…");
        char *tok = token_copy();
        yt_broadcast_settings bs = settings_from(c);
        err = yt_create_live(tok, &bs, &S.live);
        if (!err) {
            g_mutex_lock(&S.lock);
            g_snprintf(S.watch_url, sizeof S.watch_url, "https://www.youtube.com/watch?v=%s", S.live.broadcast_id);
            g_mutex_unlock(&S.lock);
            char *w = yt_update_video(tok, S.live.broadcast_id, &bs);
            if (w) { LOGW("%s", w); set_error(w); g_free(w); }
            if (c->thumbnail) {
                w = yt_set_thumbnail(tok, S.live.broadcast_id, c->thumbnail);
                if (w) { LOGW("%s", w); set_error(w); g_free(w); }
            }
            ingest = g_strdup(S.live.ingestion_address);
        }
        g_free(tok);
        if (err) goto done;
    } else {
        ingest = g_strdup_printf("https://a.upload.youtube.com/http_upload_hls?cid=%s&copy=0&file=", c->stream_key);
    }

    if (wait_or_stop(0)) goto done;
    set_phase("starting", "Starting encoder…");
    g_mutex_lock(&S.lock);
    S.up = hls_uploader_new(ingest, c->segment);
    g_mutex_unlock(&S.lock);

    engine_live_params p = {
        .width = c->width, .height = c->height, .fps = c->fps,
        .video_bitrate_kbps = c->vbitrate, .audio_bitrate_kbps = c->abitrate,
        .segment_seconds = c->segment, .encoder_pref = c->encoder,
        .audio_device_id = c->audio_device,
        .on_segment = on_segment, .on_error = on_engine_error, .user = NULL};
    char eerr[256];
    if (engine_start_live(&p, eerr, sizeof eerr) != 0) {
        err = g_strdup(eerr);
        goto done;
    }
    engine_started = 1;
    set_phase("live", c->account ? "Waiting for YouTube to receive the stream…" : "Streaming");

    gint64 started = g_get_monotonic_time();
    gint64 last_transition_try = 0;
    int went_live = 0;
    while (!wait_or_stop(c->account ? 10000 : 2000)) {
        hls_stats st;
        hls_uploader_get_stats(S.up, &st);
        if (st.fatal) {
            err = g_strdup(st.error);
            break;
        }
        if (!c->account) {
            set_phase("live", st.sent ? "Streaming" : "Uploading first segments…");
            continue;
        }
        char *tok = token_copy();
        char *lc = NULL, *ss = NULL, *health = NULL;
        char *e = yt_get_status(tok, &S.live, &lc, &ss, &health);
        if (!e) {
            g_mutex_lock(&S.lock);
            g_snprintf(S.youtube, sizeof S.youtube, "broadcast %s · stream %s (%s)",
                       lc ? lc : "?", ss ? ss : "?", health ? health : "?");
            g_mutex_unlock(&S.lock);
            if (lc && !strcmp(lc, "live")) {
                if (!went_live) set_phase("live", "On air");
                went_live = 1;
            } else if (lc && (!strcmp(lc, "complete") || !strcmp(lc, "revoked"))) {
                err = g_strdup_printf("broadcast ended on YouTube (%s)", lc);
            } else if (lc && ss && !strcmp(ss, "active") && (!strcmp(lc, "ready") || !strcmp(lc, "testing"))) {
                /* enableAutoStart normally flips it; nudge if it has not after a while. */
                gint64 now = g_get_monotonic_time();
                if (now - started > 20 * G_USEC_PER_SEC && now - last_transition_try > 30 * G_USEC_PER_SEC) {
                    last_transition_try = now;
                    char *te = yt_transition(tok, S.live.broadcast_id, "live");
                    if (te) LOGW("transition live: %s", te);
                    g_free(te);
                }
            }
        } else {
            LOGW("status poll: %s", e);
            g_free(e);
        }
        g_free(lc);
        g_free(ss);
        g_free(health);
        g_free(tok);
        if (err) break;
    }

done:
    g_mutex_lock(&S.lock);
    if (!err && S.engine_error[0]) err = g_strdup(S.engine_error);
    g_mutex_unlock(&S.lock);

    set_phase("stopping", "Finishing stream…");
    if (engine_started) engine_stop_live();
    if (S.up) {
        hls_uploader_finish(S.up, 15000);
        g_mutex_lock(&S.lock);
        S.up = NULL;
        g_mutex_unlock(&S.lock);
    }
    if (c->account && S.live.broadcast_id && c->auto_stop) {
        char *tok = token_copy();
        char *te = yt_transition(tok, S.live.broadcast_id, "complete");
        if (te) LOGW("transition complete: %s", te);
        g_free(te);
        g_free(tok);
    }
    g_free(ingest);

    if (err) {
        set_error(err);
        set_phase("error", "Stream stopped");
        g_free(err);
    } else {
        set_phase("idle", "Stream ended");
    }
    g_mutex_lock(&S.lock);
    S.running = 0;
    g_mutex_unlock(&S.lock);
    return NULL;
}

char *session_start(const char *json) {
    cJSON *o = cJSON_Parse(json);
    if (!o) return g_strdup("invalid config");

    g_mutex_lock(&S.lock);
    if (S.running) {
        g_mutex_unlock(&S.lock);
        cJSON_Delete(o);
        return g_strdup("already streaming");
    }
    g_mutex_unlock(&S.lock);

    /* Previous session's thread has finished (running == 0); reap it. */
    if (S.thread) {
        g_thread_join(S.thread);
        S.thread = NULL;
    }

    config c = {0};
    char *mode = jdup(o, "mode", "key");
    c.account = !strcmp(mode, "account");
    g_free(mode);
    c.stream_key = jdup(o, "streamKey", "");
    c.title = jdup(o, "title", "Live");
    c.description = jdup(o, "description", "");
    c.privacy = jdup(o, "privacy", "unlisted");
    c.category_id = jdup(o, "categoryId", "22");
    c.latency = jdup(o, "latency", "normal");
    c.thumbnail = jdup(o, "thumbnailPath", NULL);
    c.made_for_kids = jbool(o, "madeForKids", 0);
    c.dvr = jbool(o, "dvr", 1);
    c.auto_stop = jbool(o, "autoStop", 1);
    c.width = clampi(jint(o, "width", 1280), 160, 3840) & ~1;
    c.height = clampi(jint(o, "height", 720), 120, 2160) & ~1;
    c.fps = clampi(jint(o, "fps", 30), 1, 60);
    c.vbitrate = clampi(jint(o, "videoBitrateKbps", 4000), 300, 51000);
    c.abitrate = clampi(jint(o, "audioBitrateKbps", 128), 32, 320);
    c.segment = clampi(jint(o, "segmentSeconds", 2), 1, 4);
    c.encoder = jdup(o, "encoder", "auto");
    c.audio_device = jint(o, "audioDeviceId", -1);
    cJSON *tags = cJSON_GetObjectItem(o, "tags");
    int n = cJSON_IsArray(tags) ? cJSON_GetArraySize(tags) : 0;
    c.tags = g_new0(gchar *, n + 1);
    for (int i = 0, j = 0; i < n; i++) {
        cJSON *t = cJSON_GetArrayItem(tags, i);
        if (cJSON_IsString(t)) c.tags[j++] = g_strdup(t->valuestring);
    }
    char *token = jdup(o, "accessToken", NULL);
    cJSON_Delete(o);

    if (c.account && !token) {
        config_clear(&c);
        return g_strdup("not signed in");
    }
    if (!c.account && !valid_stream_key(c.stream_key)) {
        config_clear(&c);
        g_free(token);
        return g_strdup("stream key must contain only letters, digits, '-' and '_'");
    }

    g_mutex_lock(&S.lock);
    if (token) {
        g_free(S.token);
        S.token = token;
    }
    config_clear(&S.cfg);
    S.cfg = c;
    yt_live_clear(&S.live);
    S.stop_requested = 0;
    S.engine_error[0] = 0;
    S.error[0] = 0;
    S.youtube[0] = 0;
    S.watch_url[0] = 0;
    S.phase = "starting";
    g_strlcpy(S.message, "Starting…", sizeof S.message);
    S.running = 1;
    S.thread = g_thread_new("session", session_thread, NULL);
    g_mutex_unlock(&S.lock);
    return NULL;
}

void session_stop(void) {
    g_mutex_lock(&S.lock);
    S.stop_requested = 1;
    g_cond_broadcast(&S.cond);
    g_mutex_unlock(&S.lock);
}

void session_set_token(const char *token) {
    g_mutex_lock(&S.lock);
    g_free(S.token);
    S.token = g_strdup(token);
    g_mutex_unlock(&S.lock);
}

char *session_update_broadcast(const char *title, const char *description) {
    g_mutex_lock(&S.lock);
    int ok = S.running && S.cfg.account && S.live.broadcast_id;
    yt_live live = {0};
    if (ok) {
        live.broadcast_id = g_strdup(S.live.broadcast_id);
        live.scheduled_start = g_strdup(S.live.scheduled_start);
    }
    char *tok = g_strdup(S.token);
    g_mutex_unlock(&S.lock);
    if (!ok) {
        g_free(tok);
        return g_strdup("no live broadcast");
    }
    char *err = yt_update_broadcast(tok, &live, title, description);
    yt_live_clear(&live);
    g_free(tok);
    return err;
}

char *session_status_json(void) {
    cJSON *o = cJSON_CreateObject();
    g_mutex_lock(&S.lock);
    cJSON_AddStringToObject(o, "phase", S.phase);
    cJSON_AddStringToObject(o, "message", S.message);
    cJSON_AddStringToObject(o, "error", S.error);
    cJSON_AddStringToObject(o, "youtube", S.youtube);
    cJSON_AddStringToObject(o, "watchUrl", S.watch_url);
    if (S.up) {
        hls_stats st;
        hls_uploader_get_stats(S.up, &st);
        cJSON_AddNumberToObject(o, "segmentsSent", st.sent);
        cJSON_AddNumberToObject(o, "segmentsQueued", st.queued);
        cJSON_AddNumberToObject(o, "segmentsDropped", st.dropped + st.failed);
        cJSON_AddNumberToObject(o, "uploadKbps", st.kbps);
        if (st.error[0] && !S.error[0]) cJSON_ReplaceItemInObject(o, "error", cJSON_CreateString(st.error));
    }
    g_mutex_unlock(&S.lock);
    char *v = engine_describe_video();
    char *e = engine_describe_encoders();
    cJSON_AddStringToObject(o, "video", v);
    cJSON_AddStringToObject(o, "encoders", e);
    g_free(v);
    g_free(e);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    char *out = g_strdup(s);
    cJSON_free(s);
    return out;
}
