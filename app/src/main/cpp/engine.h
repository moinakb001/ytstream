#pragma once
/*
 * GStreamer media engine.
 *
 * Video sources (UVC or NDK camera) push frames into an appsrc. The engine runs either a
 * preview-only pipeline or a live pipeline:
 *
 *   appsrc(video) -> [jpegdec] -> videoconvert -> videoflip -> videoscale -> videorate -> tee
 *       tee -> preview (RGBA appsink, blitted to the ANativeWindow)
 *       tee -> H.264 encoder -> h264parse ----------------------------\
 *   appsrc(audio) | audiotestsrc(silence) -> AAC encoder -> aacparse --> splitmuxsink(mpegtsmux)
 *
 * splitmuxsink cuts closed-GOP MPEG-TS segments into memory; each finished segment is handed
 * to the segment callback (the HLS uploader).
 */
#include <android/native_window.h>
#include <gst/gst.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*engine_segment_cb)(GBytes *data, double duration_sec, void *user);
typedef void (*engine_error_cb)(const char *message, void *user);

typedef struct {
    int width, height, fps;
    int video_bitrate_kbps;
    int audio_bitrate_kbps;
    int segment_seconds;
    const char *encoder_pref;   /* "auto" | "hardware" | "software" */
    int audio_device_id;        /* -1: silence, 0: default input, >0: AudioDeviceInfo id */
    engine_segment_cb on_segment;
    engine_error_cb on_error;
    void *user;
} engine_live_params;

void engine_init(void);

/* Called by a video source once its output format is known (GStreamer caps string). */
void engine_video_opened(const char *caps, const char *description);
/* Called by a video source when it stops producing frames. */
void engine_video_closed(void);
/* Push one frame; takes ownership of buf. ts_ns is CLOCK_MONOTONIC capture time. */
void engine_push_video(GstBuffer *buf, int64_t ts_ns);

void engine_set_preview_window(ANativeWindow *window); /* takes a reference */
void engine_set_transform(int rotation_degrees, int mirror);

int engine_start_live(const engine_live_params *p, char *err, size_t errlen);
/* Drains (EOS, flushing the final segment) and tears the live pipeline down. Blocking. */
void engine_stop_live(void);

/* Human-readable descriptions for the status display; caller frees. */
char *engine_describe_video(void);
char *engine_describe_encoders(void);

/* Monotonic clock in ns (same clock as GStreamer's system clock). */
int64_t engine_now_ns(void);

/* Video sources (uvc_source.c, ndk_camera.c). Return 0 on success. */
int uvc_source_open(int fd, int width, int height, int fps, char *err, size_t errlen);
void uvc_source_close(void);
int ndk_camera_open(const char *id, int width, int height, int fps, char *err, size_t errlen);
void ndk_camera_close(void);

/* Audio capture (audio_capture.c): AAudio input pushed into the given appsrc. */
typedef struct audio_capture audio_capture;
audio_capture *audio_capture_open(int device_id, int *rate, int *channels, char *err, size_t errlen);
void audio_capture_start(audio_capture *a, GstElement *appsrc, GstElement *pipeline);
void audio_capture_close(audio_capture *a);
