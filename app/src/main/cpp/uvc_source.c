/*
 * USB Video Class capture via libusb + libuvc.
 *
 * Android apps cannot enumerate /dev/bus/usb themselves, so Java obtains permission and a
 * UsbDeviceConnection and hands us its file descriptor, which libusb wraps
 * (LIBUSB_OPTION_NO_DEVICE_DISCOVERY + uvc_wrap()).
 *
 * MJPEG is preferred (every UVC webcam offers it at HD resolutions; uncompressed YUYV is
 * usually limited to low frame rates by USB 2.0 bandwidth). Compressed frames are pushed to
 * GStreamer as image/jpeg and decoded there by jpegdec.
 */
#include <libusb.h>
#include <libuvc/libuvc.h>
#include <pthread.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "log.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uvc_context_t *g_ctx;
static uvc_device_handle_t *g_devh;
static int g_streaming;

typedef struct {
    enum uvc_frame_format format;
    int width, height, fps;
    int score;
} candidate;

static int is_yuy2(const uvc_format_desc_t *f) {
    return f->bDescriptorSubtype == UVC_VS_FORMAT_UNCOMPRESSED && memcmp(f->fourccFormat, "YUY2", 4) == 0;
}

/* Pick the best fps <= wanted that the frame descriptor supports; returns 0 if none. */
static int best_fps(const uvc_frame_desc_t *fr, int wanted) {
    int best = 0;
    if (fr->intervals) {
        for (const uint32_t *iv = fr->intervals; *iv; iv++) {
            int f = (int) (10000000u / *iv);
            if (f <= wanted && f > best) best = f;
        }
        if (best == 0) {
            /* Every listed rate exceeds the request: take the slowest one. */
            for (const uint32_t *iv = fr->intervals; *iv; iv++) {
                int f = (int) (10000000u / *iv);
                if (best == 0 || f < best) best = f;
            }
        }
    } else if (fr->dwMinFrameInterval) {
        /* Continuous range. */
        int max_fps = (int) (10000000u / fr->dwMinFrameInterval);
        int min_fps = fr->dwMaxFrameInterval ? (int) (10000000u / fr->dwMaxFrameInterval) : 1;
        best = wanted > max_fps ? max_fps : (wanted < min_fps ? min_fps : wanted);
    }
    return best;
}

static candidate choose_mode(uvc_device_handle_t *devh, int width, int height, int fps) {
    candidate best = {0};
    best.score = INT_MIN;
    long want_area = (long) width * height;
    for (const uvc_format_desc_t *f = uvc_get_format_descs(devh); f; f = f->next) {
        int mjpeg = f->bDescriptorSubtype == UVC_VS_FORMAT_MJPEG;
        if (!mjpeg && !is_yuy2(f)) continue;
        for (const uvc_frame_desc_t *fr = f->frame_descs; fr; fr = fr->next) {
            int rate = best_fps(fr, fps);
            if (rate <= 0) continue;
            long area = (long) fr->wWidth * fr->wHeight;
            /* Prefer: exact size, then larger-than-requested (downscale), then smaller.
             * Then the closest frame rate, then MJPEG over YUYV. */
            int score = 0;
            if (fr->wWidth == width && fr->wHeight == height) score += 100000;
            else if (area >= want_area) score += 50000 - (int) ((area - want_area) / 1000);
            else score += 20000 - (int) ((want_area - area) / 1000);
            /* Same aspect ratio is worth a lot (no letterboxing). */
            if ((long) fr->wWidth * height == (long) fr->wHeight * width) score += 10000;
            score -= abs(fps - rate) * 300;
            if (mjpeg) score += 50;
            if (score > best.score) {
                best.score = score;
                best.format = mjpeg ? UVC_FRAME_FORMAT_MJPEG : UVC_FRAME_FORMAT_YUYV;
                best.width = fr->wWidth;
                best.height = fr->wHeight;
                best.fps = rate;
            }
        }
    }
    return best;
}

static void on_frame(uvc_frame_t *frame, void *user) {
    (void) user;
    if (!frame->data || frame->data_bytes == 0) return;
    if (frame->frame_format == UVC_FRAME_FORMAT_YUYV &&
        frame->data_bytes < (size_t) frame->width * frame->height * 2) {
        return; /* incomplete frame */
    }
    GstBuffer *buf = gst_buffer_new_memdup(frame->data, frame->data_bytes);
    engine_push_video(buf, engine_now_ns());
}

static void close_locked(void) {
    if (g_devh) {
        if (g_streaming) uvc_stop_streaming(g_devh);
        uvc_close(g_devh);
        g_devh = NULL;
    }
    g_streaming = 0;
    if (g_ctx) {
        uvc_exit(g_ctx);
        g_ctx = NULL;
    }
}

int uvc_source_open(int fd, int width, int height, int fps, char *err, size_t errlen) {
    pthread_mutex_lock(&g_lock);
    close_locked();

    /* Applies to every libusb context created afterwards, including libuvc's own. */
    libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);

    uvc_error_t r = uvc_init(&g_ctx, NULL);
    if (r < 0) {
        snprintf(err, errlen, "uvc_init: %s", uvc_strerror(r));
        g_ctx = NULL;
        goto fail;
    }
    r = uvc_wrap(fd, g_ctx, &g_devh);
    if (r < 0) {
        snprintf(err, errlen, "uvc_wrap: %s", uvc_strerror(r));
        g_devh = NULL;
        goto fail;
    }

    candidate c = choose_mode(g_devh, width, height, fps);
    if (c.width == 0) {
        snprintf(err, errlen, "camera offers no MJPEG or YUYV modes");
        goto fail;
    }

    uvc_stream_ctrl_t ctrl;
    r = uvc_get_stream_ctrl_format_size(g_devh, &ctrl, c.format, c.width, c.height, c.fps);
    if (r < 0) {
        snprintf(err, errlen, "no stream for %dx%d@%d: %s", c.width, c.height, c.fps, uvc_strerror(r));
        goto fail;
    }

    char caps[256], desc[128];
    if (c.format == UVC_FRAME_FORMAT_MJPEG) {
        snprintf(caps, sizeof caps, "image/jpeg,width=%d,height=%d,framerate=%d/1,parsed=false",
                 c.width, c.height, c.fps);
    } else {
        snprintf(caps, sizeof caps,
                 "video/x-raw,format=YUY2,width=%d,height=%d,framerate=%d/1,pixel-aspect-ratio=1/1",
                 c.width, c.height, c.fps);
    }
    snprintf(desc, sizeof desc, "USB %dx%d %s @ %d fps", c.width, c.height,
             c.format == UVC_FRAME_FORMAT_MJPEG ? "MJPEG" : "YUYV", c.fps);
    engine_video_opened(caps, desc);

    r = uvc_start_streaming(g_devh, &ctrl, on_frame, NULL, 0);
    if (r < 0) {
        snprintf(err, errlen, "start streaming: %s", uvc_strerror(r));
        engine_video_closed();
        goto fail;
    }
    g_streaming = 1;
    LOGI("UVC streaming: %s", desc);
    pthread_mutex_unlock(&g_lock);
    return 0;

fail:
    LOGE("UVC open failed: %s", err);
    close_locked();
    pthread_mutex_unlock(&g_lock);
    return -1;
}

void uvc_source_close(void) {
    pthread_mutex_lock(&g_lock);
    int was_open = g_devh != NULL;
    close_locked();
    pthread_mutex_unlock(&g_lock);
    if (was_open) engine_video_closed();
}
