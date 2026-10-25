/*
 * Host harness: drives the app's real engine.c / hls_uploader.c / http.c with synthetic
 * camera frames (raw I420 like the NDK camera path, or MJPEG like the UVC path) and
 * uploads to a local fake ingestion server (fake_ingest.py).
 *
 *   host-test <ingest-base-url> <seconds> <i420|mjpeg> [rotation] [mirror]
 */
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "hls_uploader.h"
#include "http.h"

#define W 640
#define H 480
#define FPS 30

static volatile int g_run = 1;
static GPtrArray *g_jpegs;

static void on_segment(GBytes *data, double dur, void *user) {
    fprintf(stderr, "segment: %zu bytes, %.3f s\n", g_bytes_get_size(data), dur);
    hls_uploader_push(user, data, dur);
}

static void on_error(const char *msg, void *user) {
    (void) user;
    fprintf(stderr, "ENGINE ERROR: %s\n", msg);
    g_run = 0;
}

/* Pre-encode some JPEG frames to emulate a UVC MJPEG camera. */
static void make_jpegs(void) {
    g_jpegs = g_ptr_array_new_with_free_func((GDestroyNotify) gst_buffer_unref);
    GstElement *p = gst_parse_launch(
        "videotestsrc pattern=ball num-buffers=60 ! video/x-raw,width=640,height=480,format=I420 ! "
        "jpegenc ! appsink name=s sync=false", NULL);
    GstElement *s = gst_bin_get_by_name(GST_BIN(p), "s");
    gst_element_set_state(p, GST_STATE_PLAYING);
    GstSample *smp;
    while ((smp = gst_app_sink_pull_sample(GST_APP_SINK(s)))) {
        g_ptr_array_add(g_jpegs, gst_buffer_copy_deep(gst_sample_get_buffer(smp)));
        gst_sample_unref(smp);
    }
    gst_element_set_state(p, GST_STATE_NULL);
    gst_object_unref(s);
    gst_object_unref(p);
}

static gpointer camera_thread(gpointer arg) {
    int mjpeg = GPOINTER_TO_INT(arg);
    gint64 start = g_get_monotonic_time();
    for (guint64 n = 0; g_run; n++) {
        GstBuffer *buf;
        if (mjpeg) {
            buf = gst_buffer_copy(g_ptr_array_index(g_jpegs, n % g_jpegs->len));
        } else {
            buf = gst_buffer_new_allocate(NULL, W * H * 3 / 2, NULL);
            GstMapInfo m;
            gst_buffer_map(buf, &m, GST_MAP_WRITE);
            memset(m.data, 16, W * H);
            int x = (int) (n * 8 % W);
            for (int y = 0; y < H; y++) memset(m.data + y * W + x, 235, x + 40 < W ? 40 : W - x);
            memset(m.data + W * H, 128, W * H / 2);
            gst_buffer_unmap(buf, &m);
        }
        engine_push_video(buf, engine_now_ns());
        /* Slight jitter, like a real camera. */
        gint64 next = start + (gint64) (n + 1) * G_USEC_PER_SEC / FPS + g_random_int_range(-3000, 3000);
        gint64 now = g_get_monotonic_time();
        if (next > now) g_usleep((gulong) (next - now));
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <ingest-base> <seconds> <i420|mjpeg> [rotation] [mirror]\n", argv[0]);
        return 2;
    }
    gst_init(&argc, &argv);
    const char *base = argv[1];
    int seconds = atoi(argv[2]);
    int mjpeg = !strcmp(argv[3], "mjpeg");
    int rotation = argc > 4 ? atoi(argv[4]) : 0;
    int mirror = argc > 5 ? atoi(argv[5]) : 0;

    http_set_user_agent("ytstream / host-test / 1.0");
    engine_init();
    engine_set_transform(rotation, mirror);
    if (mjpeg) {
        make_jpegs();
        engine_video_opened("image/jpeg,width=640,height=480,framerate=30/1,parsed=false", "test MJPEG");
    } else {
        engine_video_opened("video/x-raw,format=I420,width=640,height=480,framerate=30/1,pixel-aspect-ratio=1/1",
                            "test I420");
    }
    GThread *cam = g_thread_new("camera", camera_thread, GINT_TO_POINTER(mjpeg));
    g_usleep(G_USEC_PER_SEC / 2); /* preview-only phase */

    hls_uploader *up = hls_uploader_new(base, 2);
    engine_live_params p = {
        .width = 1280, .height = 720, .fps = 30, .video_bitrate_kbps = 2500, .audio_bitrate_kbps = 128,
        .segment_seconds = 2, .encoder_pref = "software", .audio_device_id = -1,
        .on_segment = on_segment, .on_error = on_error, .user = up};
    char err[256];
    if (engine_start_live(&p, err, sizeof err) != 0) {
        fprintf(stderr, "start failed: %s\n", err);
        return 1;
    }
    char *enc = engine_describe_encoders();
    fprintf(stderr, "encoders: %s\n", enc);
    g_free(enc);

    for (int i = 0; i < seconds && g_run; i++) {
        g_usleep(G_USEC_PER_SEC);
        hls_stats st;
        hls_uploader_get_stats(up, &st);
        fprintf(stderr, "t=%2ds sent=%d queued=%d dropped=%d failed=%d kbps=%d %s\n", i + 1, st.sent, st.queued,
                st.dropped, st.failed, st.kbps, st.error);
        if (i == seconds / 2 && rotation == 0) engine_set_transform(0, 1); /* live transform change */
    }
    engine_stop_live();
    hls_uploader_finish(up, 10000);
    g_run = 0;
    g_thread_join(cam);
    fprintf(stderr, "done\n");
    return 0;
}
