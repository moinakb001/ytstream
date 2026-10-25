#include "engine.h"

#include <gio/gio.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <string.h>
#include <time.h>

#include "log.h"

#define PREVIEW_W 640
#define PREVIEW_H 360

static struct {
    GMutex lock;
    char *video_caps;
    char *video_desc;
    int rotation, mirror;

    GstElement *pipeline;
    GstElement *vsrc;
    GstElement *flip;
    GstElement *giosink;
    GstClockTime base_time;
    int live;
    engine_live_params lp;
    char enc_desc[256];
    audio_capture *audio;

    /* Segmenting (touched from GStreamer streaming threads). */
    GMutex seg_lock;
    GOutputStream *cur_stream;
    GstClockTime frag_start;

    /* EOS wait during shutdown. */
    GMutex eos_lock;
    GCond eos_cond;
    int eos;

    /* Preview window. */
    GMutex win_lock;
    ANativeWindow *window;
} E;

int64_t engine_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

void engine_init(void) {
    g_mutex_init(&E.lock);
    g_mutex_init(&E.seg_lock);
    g_mutex_init(&E.eos_lock);
    g_cond_init(&E.eos_cond);
    g_mutex_init(&E.win_lock);
}

/* ------------------------------------------------------------------ preview */

void engine_set_preview_window(ANativeWindow *window) {
    g_mutex_lock(&E.win_lock);
    if (E.window) ANativeWindow_release(E.window);
    E.window = window;
    if (window) {
        ANativeWindow_acquire(window);
        ANativeWindow_setBuffersGeometry(window, PREVIEW_W, PREVIEW_H, WINDOW_FORMAT_RGBA_8888);
    }
    g_mutex_unlock(&E.win_lock);
}

static GstFlowReturn on_preview_sample(GstAppSink *sink, gpointer user) {
    (void) user;
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;
    GstBuffer *buf = gst_sample_get_buffer(sample);
    GstMapInfo map;
    g_mutex_lock(&E.win_lock);
    if (E.window && gst_buffer_map(buf, &map, GST_MAP_READ)) {
        ANativeWindow_Buffer out;
        if (ANativeWindow_lock(E.window, &out, NULL) == 0) {
            int rows = out.height < PREVIEW_H ? out.height : PREVIEW_H;
            int cols = out.width < PREVIEW_W ? out.width : PREVIEW_W;
            for (int y = 0; y < rows; y++) {
                memcpy((uint8_t *) out.bits + (size_t) y * out.stride * 4,
                       map.data + (size_t) y * PREVIEW_W * 4, (size_t) cols * 4);
            }
            ANativeWindow_unlockAndPost(E.window);
        }
        gst_buffer_unmap(buf, &map);
    }
    g_mutex_unlock(&E.win_lock);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

/* ------------------------------------------------------------------ transform */

static const char *video_direction(int rotation, int mirror) {
    /* Rotation is applied first, then a horizontal mirror in output space. */
    switch (((rotation % 360) + 360) % 360) {
        case 90: return mirror ? "ul-lr" : "90r";
        case 180: return mirror ? "vert" : "180";
        case 270: return mirror ? "ur-ll" : "90l";
        default: return mirror ? "horiz" : "identity";
    }
}

void engine_set_transform(int rotation_degrees, int mirror) {
    g_mutex_lock(&E.lock);
    E.rotation = rotation_degrees;
    E.mirror = mirror;
    if (E.flip) gst_util_set_object_arg(G_OBJECT(E.flip), "video-direction", video_direction(rotation_degrees, mirror));
    g_mutex_unlock(&E.lock);
}

/* ------------------------------------------------------------------ encoder selection */

static int has_prop(GstElement *e, const char *name) {
    return g_object_class_find_property(G_OBJECT_GET_CLASS(e), name) != NULL;
}

static void set_arg(GstElement *e, const char *name, const char *value) {
    if (has_prop(e, name)) gst_util_set_object_arg(G_OBJECT(e), name, value);
}

static void set_argf(GstElement *e, const char *name, const char *fmt, ...) G_GNUC_PRINTF(3, 4);
static void set_argf(GstElement *e, const char *name, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *v = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    set_arg(e, name, v);
    g_free(v);
}

static int is_software_amc(const char *name) {
    /* MediaCodec also exposes Google's software codecs; prefer real hardware. */
    return strstr(name, "google") || strstr(name, "android") || strstr(name, "sw");
}

/* Returns a newly allocated factory name or NULL. */
static char *pick_video_encoder(const char *pref) {
    int want_hw = !pref || strcmp(pref, "software") != 0;
    int want_sw = !pref || strcmp(pref, "hardware") != 0;
    char *hw = NULL, *hw_fallback = NULL;

    if (want_hw) {
        GstCaps *h264 = gst_caps_from_string("video/x-h264");
        GList *list = gst_element_factory_list_get_elements(GST_ELEMENT_FACTORY_TYPE_VIDEO_ENCODER, GST_RANK_NONE);
        list = g_list_sort(list, gst_plugin_feature_rank_compare_func);
        for (GList *l = list; l; l = l->next) {
            GstElementFactory *f = l->data;
            const char *name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(f));
            if (!g_str_has_prefix(name, "amcvidenc")) continue;
            if (!gst_element_factory_can_src_any_caps(f, h264)) continue;
            if (!is_software_amc(name)) {
                if (!hw) hw = g_strdup(name);
            } else if (!hw_fallback) {
                hw_fallback = g_strdup(name);
            }
        }
        gst_plugin_feature_list_free(list);
        gst_caps_unref(h264);
    }
    if (hw) {
        g_free(hw_fallback);
        return hw;
    }
    if (want_sw) {
        static const char *sw[] = {"x264enc", "openh264enc"};
        for (size_t i = 0; i < G_N_ELEMENTS(sw); i++) {
            GstElementFactory *f = gst_element_factory_find(sw[i]);
            if (f) {
                gst_object_unref(f);
                g_free(hw_fallback);
                return g_strdup(sw[i]);
            }
        }
    }
    return hw_fallback; /* MediaCodec software encoder, or NULL */
}

static char *pick_audio_encoder(void) {
    static const char *names[] = {"fdkaacenc", "avenc_aac", "voaacenc"};
    for (size_t i = 0; i < G_N_ELEMENTS(names); i++) {
        GstElementFactory *f = gst_element_factory_find(names[i]);
        if (f) {
            gst_object_unref(f);
            return g_strdup(names[i]);
        }
    }
    return NULL;
}

static void configure_video_encoder(GstElement *enc, const engine_live_params *p) {
    const char *name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(gst_element_get_factory(enc)));
    int gop = p->fps * p->segment_seconds;
    if (g_str_has_prefix(name, "amcvidenc")) {
        set_argf(enc, "bitrate", "%d", p->video_bitrate_kbps * 1000);
        if (has_prop(enc, "i-frame-interval-float"))
            set_argf(enc, "i-frame-interval-float", "%d", p->segment_seconds);
        else
            set_argf(enc, "i-frame-interval", "%d", p->segment_seconds);
    } else if (!strcmp(name, "x264enc")) {
        set_argf(enc, "bitrate", "%d", p->video_bitrate_kbps);
        set_argf(enc, "key-int-max", "%d", gop);
        set_arg(enc, "tune", "zerolatency");
        set_arg(enc, "speed-preset", "veryfast");
        set_arg(enc, "bframes", "0");
    } else if (!strcmp(name, "openh264enc")) {
        set_argf(enc, "bitrate", "%d", p->video_bitrate_kbps * 1000);
        set_argf(enc, "gop-size", "%d", gop);
        set_arg(enc, "rate-control", "bitrate");
        set_arg(enc, "usage-type", "camera");
    }
}

/* ------------------------------------------------------------------ segments & bus */

static gchar *on_format_location(GstElement *splitmux, guint fragment_id, GstSample *first, gpointer user) {
    (void) splitmux; (void) first; (void) user;
    GOutputStream *stream = g_memory_output_stream_new_resizable();
    g_mutex_lock(&E.seg_lock);
    if (E.cur_stream) g_object_unref(E.cur_stream);
    E.cur_stream = g_object_ref(stream);
    g_mutex_unlock(&E.seg_lock);
    g_object_set(E.giosink, "stream", stream, NULL);
    g_object_unref(stream);
    return g_strdup_printf("segment%05u.ts", fragment_id);
}

static void handle_fragment_closed(const GstStructure *s) {
    GstClockTime end = GST_CLOCK_TIME_NONE;
    gst_structure_get_clock_time(s, "running-time", &end);

    g_mutex_lock(&E.seg_lock);
    GOutputStream *stream = E.cur_stream;
    E.cur_stream = NULL;
    GstClockTime start = E.frag_start;
    g_mutex_unlock(&E.seg_lock);
    if (!stream) return;

    g_output_stream_close(stream, NULL, NULL);
    GBytes *bytes = g_memory_output_stream_steal_as_bytes(G_MEMORY_OUTPUT_STREAM(stream));
    g_object_unref(stream);

    double dur = (GST_CLOCK_TIME_IS_VALID(end) && GST_CLOCK_TIME_IS_VALID(start) && end > start)
                 ? (double) (end - start) / GST_SECOND : 0.0;
    if (g_bytes_get_size(bytes) > 0 && E.lp.on_segment) {
        E.lp.on_segment(bytes, dur, E.lp.user);
    }
    g_bytes_unref(bytes);
}

static GstBusSyncReply bus_handler(GstBus *bus, GstMessage *msg, gpointer user) {
    (void) bus; (void) user;
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ELEMENT: {
            const GstStructure *s = gst_message_get_structure(msg);
            if (gst_structure_has_name(s, "splitmuxsink-fragment-opened")) {
                GstClockTime t = GST_CLOCK_TIME_NONE;
                gst_structure_get_clock_time(s, "running-time", &t);
                g_mutex_lock(&E.seg_lock);
                E.frag_start = t;
                g_mutex_unlock(&E.seg_lock);
            } else if (gst_structure_has_name(s, "splitmuxsink-fragment-closed")) {
                handle_fragment_closed(s);
            }
            break;
        }
        case GST_MESSAGE_ERROR: {
            GError *err = NULL;
            gchar *dbg = NULL;
            gst_message_parse_error(msg, &err, &dbg);
            LOGE("GStreamer error from %s: %s (%s)", GST_OBJECT_NAME(msg->src), err->message, dbg ? dbg : "");
            if (E.lp.on_error) {
                char *m = g_strdup_printf("%s: %s", GST_OBJECT_NAME(msg->src), err->message);
                E.lp.on_error(m, E.lp.user);
                g_free(m);
            }
            g_error_free(err);
            g_free(dbg);
            /* Unblock a pending shutdown. */
            g_mutex_lock(&E.eos_lock);
            E.eos = 1;
            g_cond_broadcast(&E.eos_cond);
            g_mutex_unlock(&E.eos_lock);
            break;
        }
        case GST_MESSAGE_WARNING: {
            GError *err = NULL;
            gst_message_parse_warning(msg, &err, NULL);
            LOGW("GStreamer warning from %s: %s", GST_OBJECT_NAME(msg->src), err->message);
            g_error_free(err);
            break;
        }
        case GST_MESSAGE_EOS:
            g_mutex_lock(&E.eos_lock);
            E.eos = 1;
            g_cond_broadcast(&E.eos_cond);
            g_mutex_unlock(&E.eos_lock);
            break;
        default:
            break;
    }
    /* No main loop pops this bus; everything is handled synchronously here. */
    return GST_BUS_DROP;
}

/* ------------------------------------------------------------------ pipeline */

static const char *decode_stage(const char *caps) {
    /* Corrupt MJPEG frames are common on cheap webcams: never let them abort the stream. */
    return g_str_has_prefix(caps, "image/jpeg") ? "jpegdec max-errors=-1 ! " : "";
}

#define PREVIEW_TAIL \
    "videoscale ! videoconvert ! " \
    "video/x-raw,format=RGBA,width=" G_STRINGIFY(PREVIEW_W) ",height=" G_STRINGIFY(PREVIEW_H) ",pixel-aspect-ratio=1/1 ! " \
    "appsink name=preview sync=false max-buffers=1 drop=true"

static void teardown_locked(void) {
    if (!E.pipeline) return;
    gst_element_set_state(E.pipeline, GST_STATE_NULL);
    gst_object_unref(E.pipeline);
    E.pipeline = NULL;
    if (E.vsrc) gst_object_unref(E.vsrc);
    if (E.flip) gst_object_unref(E.flip);
    if (E.giosink) gst_object_unref(E.giosink);
    E.vsrc = E.flip = E.giosink = NULL;
    g_mutex_lock(&E.seg_lock);
    if (E.cur_stream) g_object_unref(E.cur_stream);
    E.cur_stream = NULL;
    g_mutex_unlock(&E.seg_lock);
}

static void attach_common(void) {
    E.vsrc = gst_bin_get_by_name(GST_BIN(E.pipeline), "vsrc");
    E.flip = gst_bin_get_by_name(GST_BIN(E.pipeline), "flip");
    GstElement *preview = gst_bin_get_by_name(GST_BIN(E.pipeline), "preview");
    GstAppSinkCallbacks cbs = {0};
    cbs.new_sample = on_preview_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(preview), &cbs, NULL, NULL);
    gst_object_unref(preview);

    /* Fix the base time up front (instead of letting PLAYING pick it) so capture threads can
     * timestamp and push frames immediately; splitmuxsink only completes its state change
     * once data flows. The system clock is CLOCK_MONOTONIC, like engine_now_ns(). */
    GstClock *clock = gst_system_clock_obtain();
    gst_pipeline_use_clock(GST_PIPELINE(E.pipeline), clock);
    gst_object_unref(clock);
    gst_element_set_start_time(E.pipeline, GST_CLOCK_TIME_NONE);
    E.base_time = (GstClockTime) engine_now_ns();
    gst_element_set_base_time(E.pipeline, E.base_time);
    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(E.pipeline));
    gst_bus_set_sync_handler(bus, bus_handler, NULL, NULL);
    gst_object_unref(bus);
}

static void build_preview_locked(void) {
    teardown_locked();
    if (!E.video_caps) return;
    char *desc = g_strdup_printf(
        "appsrc name=vsrc is-live=true format=time do-timestamp=false max-buffers=4 leaky-type=downstream caps=\"%s\" ! "
        "queue max-size-buffers=3 max-size-bytes=0 max-size-time=0 leaky=downstream ! "
        "%svideoconvert ! videoflip name=flip video-direction=%s ! " PREVIEW_TAIL,
        E.video_caps, decode_stage(E.video_caps), video_direction(E.rotation, E.mirror));
    GError *err = NULL;
    E.pipeline = gst_parse_launch(desc, &err);
    g_free(desc);
    if (!E.pipeline) {
        LOGE("preview pipeline: %s", err ? err->message : "?");
        g_clear_error(&err);
        return;
    }
    g_clear_error(&err);
    memset(&E.lp, 0, sizeof E.lp);
    attach_common();
    gst_element_set_state(E.pipeline, GST_STATE_PLAYING);
}

void engine_video_opened(const char *caps, const char *description) {
    g_mutex_lock(&E.lock);
    g_free(E.video_caps);
    g_free(E.video_desc);
    E.video_caps = g_strdup(caps);
    E.video_desc = g_strdup(description);
    if (E.live) {
        /* Same source kind re-opened mid-stream (e.g. USB camera replugged): just update caps. */
        GstCaps *c = gst_caps_from_string(caps);
        gst_app_src_set_caps(GST_APP_SRC(E.vsrc), c);
        gst_caps_unref(c);
    } else {
        build_preview_locked();
    }
    g_mutex_unlock(&E.lock);
}

void engine_video_closed(void) {
    g_mutex_lock(&E.lock);
    g_free(E.video_desc);
    E.video_desc = NULL;
    if (!E.live) {
        g_free(E.video_caps);
        E.video_caps = NULL;
        teardown_locked();
    }
    g_mutex_unlock(&E.lock);
}

void engine_push_video(GstBuffer *buf, int64_t ts_ns) {
    g_mutex_lock(&E.lock);
    GstElement *src = E.vsrc ? gst_object_ref(E.vsrc) : NULL;
    GstClockTime base = E.pipeline ? E.base_time : GST_CLOCK_TIME_NONE;
    g_mutex_unlock(&E.lock);

    if (!src || !GST_CLOCK_TIME_IS_VALID(base) || ts_ns < (int64_t) base) {
        gst_buffer_unref(buf);
        if (src) gst_object_unref(src);
        return;
    }
    GST_BUFFER_PTS(buf) = (GstClockTime) ts_ns - base;
    gst_app_src_push_buffer(GST_APP_SRC(src), buf);
    gst_object_unref(src);
}

int engine_start_live(const engine_live_params *p, char *err, size_t errlen) {
    g_mutex_lock(&E.lock);
    if (E.live) {
        snprintf(err, errlen, "already live");
        goto fail;
    }
    if (!E.video_caps) {
        snprintf(err, errlen, "no video source is open");
        goto fail;
    }

    char *venc = pick_video_encoder(p->encoder_pref);
    char *aenc = pick_audio_encoder();
    if (!venc || !aenc) {
        snprintf(err, errlen, "missing encoder (video: %s, audio: %s)", venc ? venc : "none", aenc ? aenc : "none");
        g_free(venc);
        g_free(aenc);
        goto fail;
    }

    /* Audio source: selected input device, or a silent track. */
    char audio_src[256];
    char audio_note[160] = "";
    int rate = 0, channels = 0;
    E.audio = NULL;
    if (p->audio_device_id >= 0) {
        char aerr[128];
        E.audio = audio_capture_open(p->audio_device_id, &rate, &channels, aerr, sizeof aerr);
        if (!E.audio) snprintf(audio_note, sizeof audio_note, " (mic unavailable: %s; silent)", aerr);
    }
    if (E.audio) {
        snprintf(audio_src, sizeof audio_src,
                 "appsrc name=asrc is-live=true format=time do-timestamp=false "
                 "caps=\"audio/x-raw,format=S16LE,layout=interleaved,rate=%d,channels=%d\"", rate, channels);
    } else {
        snprintf(audio_src, sizeof audio_src, "audiotestsrc wave=silence is-live=true samplesperbuffer=960");
    }

    char *desc = g_strdup_printf(
        "appsrc name=vsrc is-live=true format=time do-timestamp=false max-buffers=8 leaky-type=downstream caps=\"%s\" ! "
        "queue max-size-buffers=4 max-size-bytes=0 max-size-time=0 leaky=downstream ! "
        "%svideoconvert ! videoflip name=flip video-direction=%s ! "
        "videoscale ! video/x-raw,width=%d,height=%d,pixel-aspect-ratio=1/1 ! "
        "videorate ! video/x-raw,framerate=%d/1 ! tee name=t "
        "t. ! queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 leaky=downstream ! " PREVIEW_TAIL " "
        "t. ! queue max-size-buffers=0 max-size-bytes=0 max-size-time=3000000000 ! videoconvert ! "
        "%s name=venc ! h264parse config-interval=-1 ! "
        "video/x-h264,stream-format=byte-stream,alignment=au ! queue name=vq "
        "%s ! audioconvert ! audioresample ! audio/x-raw,rate=48000,channels=2 ! "
        "%s name=aenc bitrate=%d ! aacparse ! queue name=aq",
        E.video_caps, decode_stage(E.video_caps), video_direction(E.rotation, E.mirror),
        p->width, p->height, p->fps, venc, audio_src, aenc, p->audio_bitrate_kbps * 1000);

    teardown_locked();
    GError *gerr = NULL;
    E.pipeline = gst_parse_launch(desc, &gerr);
    g_free(desc);
    if (!E.pipeline || gerr) {
        snprintf(err, errlen, "pipeline: %s", gerr ? gerr->message : "parse failed");
        g_clear_error(&gerr);
        if (E.pipeline) gst_object_unref(E.pipeline);
        E.pipeline = NULL;
        g_free(venc);
        g_free(aenc);
        goto fail_audio;
    }

    E.lp = *p;
    E.lp.encoder_pref = NULL;
    attach_common();

    GstElement *enc = gst_bin_get_by_name(GST_BIN(E.pipeline), "venc");
    configure_video_encoder(enc, p);
    gst_object_unref(enc);

    /* splitmuxsink -> mpegtsmux -> giostreamsink writing each segment into memory.
     * The muxer is kept running across fragments (as hlssink2 does), so timestamps and
     * continuity counters are continuous and every fragment starts with PAT/PMT + IDR. */
    GstElement *mux = gst_element_factory_make("splitmuxsink", "mux");
    GstElement *tsmux = gst_element_factory_make("mpegtsmux", NULL);
    E.giosink = gst_element_factory_make("giostreamsink", NULL);
    if (!mux || !tsmux || !E.giosink) {
        snprintf(err, errlen, "missing splitmuxsink/mpegtsmux/giostreamsink");
        goto fail_pipeline;
    }
    gst_object_ref(E.giosink);
    g_object_set(mux,
                 "max-size-time", (guint64) p->segment_seconds * GST_SECOND,
                 "send-keyframe-requests", TRUE,
                 "reset-muxer", FALSE,
                 "muxer", tsmux,
                 "sink", E.giosink,
                 NULL);
    g_signal_connect(mux, "format-location-full", G_CALLBACK(on_format_location), NULL);
    gst_bin_add(GST_BIN(E.pipeline), mux);

    GstElement *vq = gst_bin_get_by_name(GST_BIN(E.pipeline), "vq");
    GstElement *aq = gst_bin_get_by_name(GST_BIN(E.pipeline), "aq");
    gboolean linked = gst_element_link_pads(vq, "src", mux, "video") &&
                      gst_element_link_pads(aq, "src", mux, "audio_%u");
    gst_object_unref(vq);
    gst_object_unref(aq);
    if (!linked) {
        snprintf(err, errlen, "could not link encoders to splitmuxsink");
        goto fail_pipeline;
    }

    g_mutex_lock(&E.eos_lock);
    E.eos = 0;
    g_mutex_unlock(&E.eos_lock);

    if (gst_element_set_state(E.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        snprintf(err, errlen, "pipeline failed to start (encoder %s)", venc);
        goto fail_pipeline;
    }
    if (E.audio) {
        GstElement *asrc = gst_bin_get_by_name(GST_BIN(E.pipeline), "asrc");
        audio_capture_start(E.audio, asrc, E.pipeline);
        gst_object_unref(asrc);
    }

    snprintf(E.enc_desc, sizeof E.enc_desc, "%s %d kbps, %s %d kbps%s", venc, p->video_bitrate_kbps, aenc,
             p->audio_bitrate_kbps, audio_note);
    LOGI("live pipeline started: %s", E.enc_desc);
    g_free(venc);
    g_free(aenc);
    E.live = 1;
    g_mutex_unlock(&E.lock);
    return 0;

fail_pipeline:
    g_free(venc);
    g_free(aenc);
    teardown_locked();
    memset(&E.lp, 0, sizeof E.lp);
fail_audio:
    audio_capture_close(E.audio);
    E.audio = NULL;
    build_preview_locked();
fail:
    g_mutex_unlock(&E.lock);
    return -1;
}

void engine_stop_live(void) {
    g_mutex_lock(&E.lock);
    if (!E.live) {
        g_mutex_unlock(&E.lock);
        return;
    }
    /* Stop feeding audio first so nothing is pushed after EOS. */
    audio_capture_close(E.audio);
    E.audio = NULL;
    GstElement *pipeline = gst_object_ref(E.pipeline);
    g_mutex_unlock(&E.lock);

    /* EOS makes splitmuxsink finish (and report) the last segment. */
    gst_element_send_event(pipeline, gst_event_new_eos());
    gint64 deadline = g_get_monotonic_time() + 6 * G_TIME_SPAN_SECOND;
    g_mutex_lock(&E.eos_lock);
    while (!E.eos) {
        if (!g_cond_wait_until(&E.eos_cond, &E.eos_lock, deadline)) {
            LOGW("timed out waiting for EOS");
            break;
        }
    }
    g_mutex_unlock(&E.eos_lock);
    gst_object_unref(pipeline);

    g_mutex_lock(&E.lock);
    teardown_locked();
    E.live = 0;
    memset(&E.lp, 0, sizeof E.lp);
    E.enc_desc[0] = 0;
    build_preview_locked();
    g_mutex_unlock(&E.lock);
}

char *engine_describe_video(void) {
    g_mutex_lock(&E.lock);
    char *s = g_strdup(E.video_desc ? E.video_desc : "");
    g_mutex_unlock(&E.lock);
    return s;
}

char *engine_describe_encoders(void) {
    g_mutex_lock(&E.lock);
    char *s = g_strdup(E.enc_desc);
    g_mutex_unlock(&E.lock);
    return s;
}
