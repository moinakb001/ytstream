#pragma once
/*
 * Minimal YouTube Live Streaming API client (Data API v3).
 * All functions are blocking and return NULL on success or an error message (caller frees).
 */
#include <stddef.h>

typedef struct {
    const char *title;
    const char *description;
    const char *privacy;      /* public | unlisted | private */
    const char *latency;      /* normal | low | ultraLow */
    const char *category_id;  /* may be NULL */
    const char *const *tags;  /* NULL-terminated, may be NULL */
    int made_for_kids;
    int dvr;
    int auto_stop;
} yt_broadcast_settings;

typedef struct {
    char *stream_id;
    char *ingestion_address;   /* ends with "file=" */
    char *broadcast_id;
    char *scheduled_start;     /* kept for later snippet updates */
} yt_live;

char *yt_create_live(const char *token, const yt_broadcast_settings *s, yt_live *out);
char *yt_update_video(const char *token, const char *video_id, const yt_broadcast_settings *s);
char *yt_set_thumbnail(const char *token, const char *video_id, const char *jpeg_path);
char *yt_update_broadcast(const char *token, const yt_live *live, const char *title, const char *description);
char *yt_get_status(const char *token, const yt_live *live, char **lifecycle, char **stream_status, char **health);
char *yt_transition(const char *token, const char *broadcast_id, const char *status);
void yt_live_clear(yt_live *l);
