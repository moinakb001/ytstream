#pragma once
#include <glib.h>

/*
 * Uploads MPEG-TS segments and a sliding Media Playlist to YouTube's HLS ingestion endpoint
 * (https://developers.google.com/youtube/v3/live/guides/hls-ingestion):
 *  - HTTPS PUT of each file to <ingestion address><file name> over a persistent connection
 *  - first playlist starts at media sequence 0; an updated playlist is sent for every segment
 *  - each playlist lists the new segment plus two previous ones (<= 5 outstanding)
 *  - segment names are unique across restarts
 */
typedef struct hls_uploader hls_uploader;

typedef struct {
    int sent, queued, dropped, failed;
    int kbps;
    int fatal;          /* non-zero: the endpoint rejected us (e.g. bad/expired stream key) */
    char error[256];
} hls_stats;

hls_uploader *hls_uploader_new(const char *ingest_base, int segment_seconds);
/* Thread-safe; takes a reference to data. */
void hls_uploader_push(hls_uploader *u, GBytes *data, double duration_sec);
/* Waits up to timeout_ms for queued segments to be sent, then stops and frees. */
void hls_uploader_finish(hls_uploader *u, int timeout_ms);
void hls_uploader_get_stats(hls_uploader *u, hls_stats *out);
