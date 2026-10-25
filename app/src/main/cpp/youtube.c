#include "youtube.h"

#include <cJSON.h>
#include <glib.h>
#include <string.h>

#include "http.h"
#include "log.h"

#define API "https://www.googleapis.com/youtube/v3"
#define UPLOAD_API "https://www.googleapis.com/upload/youtube/v3"

/* Performs a JSON request; on success *out receives the parsed body (may be NULL). */
static char *call(const char *token, const char *method, const char *url, cJSON *body, cJSON **out) {
    http_client *c = http_client_new(NULL, 30);
    GBytes *b = NULL;
    if (body) {
        char *s = cJSON_PrintUnformatted(body);
        b = g_bytes_new_take(s, strlen(s));
    }
    http_response r = http_request(c, method, url, token, "application/json; charset=UTF-8", b);
    if (b) g_bytes_unref(b);
    http_client_free(c);

    char *text = http_response_text(&r);
    cJSON *json = cJSON_Parse(text);
    char *err = NULL;
    if (r.status < 200 || r.status >= 300) {
        const char *msg = NULL;
        cJSON *e = cJSON_GetObjectItem(json, "error");
        cJSON *m = cJSON_GetObjectItem(e, "message");
        if (cJSON_IsString(m)) msg = m->valuestring;
        if (r.status == 0) {
            err = g_strdup_printf("network error: %s", r.error ? r.error : "unknown");
        } else {
            err = g_strdup_printf("HTTP %d: %s", r.status, msg ? msg : text);
        }
        LOGW("%s %s -> %s", method, url, err);
        cJSON_Delete(json);
        json = NULL;
    }
    g_free(text);
    http_response_clear(&r);
    if (out) *out = json; else cJSON_Delete(json);
    return err;
}

static char *wrap(const char *what, char *err) {
    char *e = g_strdup_printf("%s: %s", what, err);
    g_free(err);
    return e;
}

static const char *jstr(cJSON *root, const char *path) {
    /* Dotted path lookup, e.g. "cdn.ingestionInfo.ingestionAddress". */
    char **parts = g_strsplit(path, ".", -1);
    cJSON *cur = root;
    for (int i = 0; parts[i] && cur; i++) cur = cJSON_GetObjectItem(cur, parts[i]);
    g_strfreev(parts);
    return cJSON_IsString(cur) ? cur->valuestring : NULL;
}

static char *now_iso8601(void) {
    GDateTime *dt = g_date_time_new_now_utc();
    char *s = g_date_time_format(dt, "%Y-%m-%dT%H:%M:%SZ");
    g_date_time_unref(dt);
    return s;
}

char *yt_create_live(const char *token, const yt_broadcast_settings *s, yt_live *out) {
    memset(out, 0, sizeof *out);
    char *err;

    /* 1. HLS stream (ingestion point). */
    cJSON *stream = cJSON_CreateObject();
    cJSON *sn = cJSON_AddObjectToObject(stream, "snippet");
    cJSON_AddStringToObject(sn, "title", s->title);
    cJSON *cdn = cJSON_AddObjectToObject(stream, "cdn");
    cJSON_AddStringToObject(cdn, "ingestionType", "hls");
    cJSON_AddStringToObject(cdn, "frameRate", "variable");
    cJSON_AddStringToObject(cdn, "resolution", "variable");
    cJSON *cd = cJSON_AddObjectToObject(stream, "contentDetails");
    cJSON_AddBoolToObject(cd, "isReusable", 0);
    cJSON *resp = NULL;
    err = call(token, "POST", API "/liveStreams?part=snippet,cdn,contentDetails,status", stream, &resp);
    cJSON_Delete(stream);
    if (err) return wrap("create stream", err);
    const char *sid = jstr(resp, "id");
    const char *addr = jstr(resp, "cdn.ingestionInfo.ingestionAddress");
    if (!sid || !addr) {
        cJSON_Delete(resp);
        return g_strdup("create stream: response missing id/ingestionAddress");
    }
    out->stream_id = g_strdup(sid);
    out->ingestion_address = g_strdup(addr);
    cJSON_Delete(resp);

    /* 2. Broadcast with the user's metadata. */
    out->scheduled_start = now_iso8601();
    cJSON *bc = cJSON_CreateObject();
    sn = cJSON_AddObjectToObject(bc, "snippet");
    cJSON_AddStringToObject(sn, "title", s->title);
    cJSON_AddStringToObject(sn, "description", s->description ? s->description : "");
    cJSON_AddStringToObject(sn, "scheduledStartTime", out->scheduled_start);
    cJSON *st = cJSON_AddObjectToObject(bc, "status");
    cJSON_AddStringToObject(st, "privacyStatus", s->privacy);
    cJSON_AddBoolToObject(st, "selfDeclaredMadeForKids", s->made_for_kids);
    cd = cJSON_AddObjectToObject(bc, "contentDetails");
    cJSON_AddBoolToObject(cd, "enableAutoStart", 1);
    cJSON_AddBoolToObject(cd, "enableAutoStop", s->auto_stop);
    cJSON_AddBoolToObject(cd, "enableDvr", s->dvr);
    cJSON_AddStringToObject(cd, "latencyPreference", s->latency);
    cJSON *mon = cJSON_AddObjectToObject(cd, "monitorStream");
    cJSON_AddBoolToObject(mon, "enableMonitorStream", 0);
    resp = NULL;
    err = call(token, "POST", API "/liveBroadcasts?part=snippet,status,contentDetails", bc, &resp);
    cJSON_Delete(bc);
    if (err) return wrap("create broadcast", err);
    const char *bid = jstr(resp, "id");
    if (!bid) {
        cJSON_Delete(resp);
        return g_strdup("create broadcast: response missing id");
    }
    out->broadcast_id = g_strdup(bid);
    cJSON_Delete(resp);

    /* 3. Bind stream to broadcast. */
    char *url = g_strdup_printf(API "/liveBroadcasts/bind?part=id,contentDetails&id=%s&streamId=%s",
                                out->broadcast_id, out->stream_id);
    err = call(token, "POST", url, NULL, NULL);
    g_free(url);
    if (err) return wrap("bind", err);
    return NULL;
}

char *yt_update_video(const char *token, const char *video_id, const yt_broadcast_settings *s) {
    /* Category and tags live on the video resource (same id as the broadcast). videos.update
     * replaces the snippet, so title/description are re-sent. */
    cJSON *v = cJSON_CreateObject();
    cJSON_AddStringToObject(v, "id", video_id);
    cJSON *sn = cJSON_AddObjectToObject(v, "snippet");
    cJSON_AddStringToObject(sn, "title", s->title);
    cJSON_AddStringToObject(sn, "description", s->description ? s->description : "");
    cJSON_AddStringToObject(sn, "categoryId", s->category_id ? s->category_id : "22");
    if (s->tags && s->tags[0]) {
        cJSON *tags = cJSON_AddArrayToObject(sn, "tags");
        for (int i = 0; s->tags[i]; i++) cJSON_AddItemToArray(tags, cJSON_CreateString(s->tags[i]));
    }
    char *err = call(token, "PUT", API "/videos?part=snippet", v, NULL);
    cJSON_Delete(v);
    return err ? wrap("update video", err) : NULL;
}

char *yt_set_thumbnail(const char *token, const char *video_id, const char *jpeg_path) {
    gchar *data = NULL;
    gsize len = 0;
    GError *gerr = NULL;
    if (!g_file_get_contents(jpeg_path, &data, &len, &gerr)) {
        char *e = g_strdup_printf("thumbnail: %s", gerr->message);
        g_error_free(gerr);
        return e;
    }
    GBytes *b = g_bytes_new_take(data, len);
    char *url = g_strdup_printf(UPLOAD_API "/thumbnails/set?videoId=%s&uploadType=media", video_id);
    http_client *c = http_client_new(NULL, 60);
    http_response r = http_request(c, "POST", url, token, "image/jpeg", b);
    http_client_free(c);
    g_free(url);
    g_bytes_unref(b);
    char *err = NULL;
    if (r.status < 200 || r.status >= 300) {
        char *text = http_response_text(&r);
        err = g_strdup_printf("thumbnail: HTTP %d %s", r.status, r.error ? r.error : text);
        g_free(text);
    }
    http_response_clear(&r);
    return err;
}

char *yt_update_broadcast(const char *token, const yt_live *live, const char *title, const char *description) {
    cJSON *bc = cJSON_CreateObject();
    cJSON_AddStringToObject(bc, "id", live->broadcast_id);
    cJSON *sn = cJSON_AddObjectToObject(bc, "snippet");
    cJSON_AddStringToObject(sn, "title", title);
    cJSON_AddStringToObject(sn, "description", description ? description : "");
    cJSON_AddStringToObject(sn, "scheduledStartTime", live->scheduled_start);
    char *err = call(token, "PUT", API "/liveBroadcasts?part=snippet", bc, NULL);
    cJSON_Delete(bc);
    return err;
}

char *yt_get_status(const char *token, const yt_live *live, char **lifecycle, char **stream_status, char **health) {
    cJSON *resp = NULL;
    char *url = g_strdup_printf(API "/liveBroadcasts?part=status&id=%s", live->broadcast_id);
    char *err = call(token, "GET", url, NULL, &resp);
    g_free(url);
    if (err) return err;
    cJSON *item = cJSON_GetArrayItem(cJSON_GetObjectItem(resp, "items"), 0);
    *lifecycle = g_strdup(jstr(item, "status.lifeCycleStatus"));
    cJSON_Delete(resp);

    resp = NULL;
    url = g_strdup_printf(API "/liveStreams?part=status&id=%s", live->stream_id);
    err = call(token, "GET", url, NULL, &resp);
    g_free(url);
    if (err) return err;
    item = cJSON_GetArrayItem(cJSON_GetObjectItem(resp, "items"), 0);
    *stream_status = g_strdup(jstr(item, "status.streamStatus"));
    *health = g_strdup(jstr(item, "status.healthStatus.status"));
    cJSON_Delete(resp);
    return NULL;
}

char *yt_transition(const char *token, const char *broadcast_id, const char *status) {
    char *url = g_strdup_printf(API "/liveBroadcasts/transition?part=status&broadcastStatus=%s&id=%s",
                                status, broadcast_id);
    char *err = call(token, "POST", url, NULL, NULL);
    g_free(url);
    return err;
}

void yt_live_clear(yt_live *l) {
    g_free(l->stream_id);
    g_free(l->ingestion_address);
    g_free(l->broadcast_id);
    g_free(l->scheduled_start);
    memset(l, 0, sizeof *l);
}
