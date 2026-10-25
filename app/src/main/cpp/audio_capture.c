/*
 * Microphone capture via AAudio. Any Android input device can be selected by its
 * AudioDeviceInfo id (built-in mic, USB camera mic, USB/wired/Bluetooth headset, ...).
 * PCM is pushed into an appsrc with capture-time timestamps on GStreamer's clock.
 */
#include <aaudio/AAudio.h>
#include <gst/app/gstappsrc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "engine.h"
#include "log.h"

#define CHUNK_FRAMES 480 /* 10 ms at 48 kHz */

struct audio_capture {
    AAudioStream *stream;
    int rate, channels;
    GstElement *appsrc;
    GstElement *pipeline;
    pthread_t thread;
    volatile int running;
};

audio_capture *audio_capture_open(int device_id, int *rate, int *channels, char *err, size_t errlen) {
    AAudioStreamBuilder *b = NULL;
    aaudio_result_t r = AAudio_createStreamBuilder(&b);
    if (r != AAUDIO_OK) {
        snprintf(err, errlen, "AAudio builder: %s", AAudio_convertResultToText(r));
        return NULL;
    }
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_INPUT);
    if (device_id > 0) AAudioStreamBuilder_setDeviceId(b, device_id);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setSampleRate(b, 48000);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setInputPreset(b, AAUDIO_INPUT_PRESET_CAMCORDER);

    AAudioStream *s = NULL;
    r = AAudioStreamBuilder_openStream(b, &s);
    if (r != AAUDIO_OK) {
        /* Some devices refuse stereo or 48 kHz; let AAudio pick. */
        AAudioStreamBuilder_setChannelCount(b, AAUDIO_UNSPECIFIED);
        AAudioStreamBuilder_setSampleRate(b, AAUDIO_UNSPECIFIED);
        r = AAudioStreamBuilder_openStream(b, &s);
    }
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) {
        snprintf(err, errlen, "open input %d: %s", device_id, AAudio_convertResultToText(r));
        return NULL;
    }

    audio_capture *a = calloc(1, sizeof *a);
    a->stream = s;
    a->rate = AAudioStream_getSampleRate(s);
    a->channels = AAudioStream_getChannelCount(s);
    *rate = a->rate;
    *channels = a->channels;
    LOGI("audio input %d opened: %d Hz, %d ch (device %d)", device_id, a->rate, a->channels,
         AAudioStream_getDeviceId(s));
    return a;
}

static void *capture_thread(void *arg) {
    audio_capture *a = arg;
    const size_t frame_bytes = (size_t) a->channels * 2;
    int64_t frames_read = 0;
    int64_t anchor_ns = -1, anchor_frame = 0;

    while (a->running) {
        GstBuffer *buf = gst_buffer_new_allocate(NULL, CHUNK_FRAMES * frame_bytes, NULL);
        GstMapInfo map;
        gst_buffer_map(buf, &map, GST_MAP_WRITE);
        aaudio_result_t n = AAudioStream_read(a->stream, map.data, CHUNK_FRAMES, 100 * 1000000LL);
        gst_buffer_unmap(buf, &map);
        if (n < 0) {
            LOGE("AAudio read: %s", AAudio_convertResultToText(n));
            gst_buffer_unref(buf);
            break;
        }
        if (n == 0) {
            gst_buffer_unref(buf);
            continue;
        }
        gst_buffer_set_size(buf, (gsize) n * frame_bytes);

        /* Map frame index -> capture time using the stream's presentation timestamp, so audio
         * lines up with video stamped at capture. Re-anchor if drift exceeds 20 ms. */
        int64_t ts_frame, ts_ns;
        int64_t est_ns;
        if (AAudioStream_getTimestamp(a->stream, CLOCK_MONOTONIC, &ts_frame, &ts_ns) == AAUDIO_OK) {
            est_ns = ts_ns + (frames_read - ts_frame) * 1000000000LL / a->rate;
        } else {
            est_ns = engine_now_ns() - (int64_t) n * 1000000000LL / a->rate;
        }
        int64_t pts_ns;
        if (anchor_ns < 0) {
            anchor_ns = est_ns;
            anchor_frame = frames_read;
        }
        pts_ns = anchor_ns + (frames_read - anchor_frame) * 1000000000LL / a->rate;
        if (llabs(pts_ns - est_ns) > 20000000LL) {
            anchor_ns = est_ns;
            anchor_frame = frames_read;
            pts_ns = est_ns;
        }
        frames_read += n;

        GstClockTime base = gst_element_get_base_time(a->pipeline);
        if (base == GST_CLOCK_TIME_NONE || pts_ns < (int64_t) base) {
            gst_buffer_unref(buf);
            continue;
        }
        GST_BUFFER_PTS(buf) = (GstClockTime) pts_ns - base;
        GST_BUFFER_DURATION(buf) = gst_util_uint64_scale(n, GST_SECOND, a->rate);
        if (gst_app_src_push_buffer(GST_APP_SRC(a->appsrc), buf) != GST_FLOW_OK) break;
    }
    return NULL;
}

void audio_capture_start(audio_capture *a, GstElement *appsrc, GstElement *pipeline) {
    a->appsrc = gst_object_ref(appsrc);
    a->pipeline = gst_object_ref(pipeline);
    AAudioStream_requestStart(a->stream);
    a->running = 1;
    pthread_create(&a->thread, NULL, capture_thread, a);
}

void audio_capture_close(audio_capture *a) {
    if (!a) return;
    if (a->running) {
        a->running = 0;
        pthread_join(a->thread, NULL);
    }
    AAudioStream_requestStop(a->stream);
    AAudioStream_close(a->stream);
    if (a->appsrc) gst_object_unref(a->appsrc);
    if (a->pipeline) gst_object_unref(a->pipeline);
    free(a);
}
