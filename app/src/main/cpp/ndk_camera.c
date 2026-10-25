/*
 * Camera capture via the NDK Camera2 API (built-in cameras, and USB cameras on devices whose
 * camera HAL exposes them as LENS_FACING_EXTERNAL). Frames arrive as YUV_420_888 through an
 * AImageReader and are repacked to I420.
 */
#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>
#include <media/NdkImageReader.h>
#include <pthread.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "log.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static struct {
    ACameraManager *mgr;
    ACameraDevice *device;
    AImageReader *reader;
    ACaptureSessionOutputContainer *outputs;
    ACaptureSessionOutput *output;
    ACameraOutputTarget *target;
    ACaptureRequest *request;
    ACameraCaptureSession *session;
    int width, height;
} cam;

static void on_disconnected(void *ctx, ACameraDevice *dev) {
    (void) ctx; (void) dev;
    LOGW("camera disconnected");
}

static void on_error(void *ctx, ACameraDevice *dev, int error) {
    (void) ctx; (void) dev;
    LOGE("camera error %d", error);
}

static void on_session_closed(void *ctx, ACameraCaptureSession *s) { (void) ctx; (void) s; }
static void on_session_ready(void *ctx, ACameraCaptureSession *s) { (void) ctx; (void) s; }
static void on_session_active(void *ctx, ACameraCaptureSession *s) { (void) ctx; (void) s; }

static void copy_plane(uint8_t *dst, int dst_w, int dst_h, const uint8_t *src, int row_stride, int pixel_stride) {
    for (int y = 0; y < dst_h; y++) {
        const uint8_t *s = src + (size_t) y * row_stride;
        uint8_t *d = dst + (size_t) y * dst_w;
        if (pixel_stride == 1) {
            memcpy(d, s, dst_w);
        } else {
            for (int x = 0; x < dst_w; x++) d[x] = s[x * pixel_stride];
        }
    }
}

static void on_image(void *ctx, AImageReader *reader) {
    (void) ctx;
    AImage *img = NULL;
    if (AImageReader_acquireLatestImage(reader, &img) != AMEDIA_OK || !img) return;

    int32_t w = 0, h = 0;
    AImage_getWidth(img, &w);
    AImage_getHeight(img, &h);
    w &= ~1;
    h &= ~1;
    int cw = w / 2, ch = h / 2;
    size_t ysize = (size_t) w * h, csize = (size_t) cw * ch;

    GstBuffer *buf = gst_buffer_new_allocate(NULL, ysize + 2 * csize, NULL);
    GstMapInfo map;
    if (gst_buffer_map(buf, &map, GST_MAP_WRITE)) {
        for (int p = 0; p < 3; p++) {
            uint8_t *data = NULL;
            int len = 0, row = 0, pix = 1;
            AImage_getPlaneData(img, p, &data, &len);
            AImage_getPlaneRowStride(img, p, &row);
            AImage_getPlanePixelStride(img, p, &pix);
            uint8_t *dst = map.data + (p == 0 ? 0 : ysize + (p - 1) * csize);
            copy_plane(dst, p == 0 ? w : cw, p == 0 ? h : ch, data, row, pix);
        }
        gst_buffer_unmap(buf, &map);
        engine_push_video(buf, engine_now_ns());
    } else {
        gst_buffer_unref(buf);
    }
    AImage_delete(img);
}

static void close_locked(void) {
    if (cam.session) {
        ACameraCaptureSession_stopRepeating(cam.session);
        ACameraCaptureSession_close(cam.session);
        cam.session = NULL;
    }
    if (cam.request) { ACaptureRequest_free(cam.request); cam.request = NULL; }
    if (cam.target) { ACameraOutputTarget_free(cam.target); cam.target = NULL; }
    if (cam.device) { ACameraDevice_close(cam.device); cam.device = NULL; }
    if (cam.outputs) {
        if (cam.output) ACaptureSessionOutputContainer_remove(cam.outputs, cam.output);
        ACaptureSessionOutputContainer_free(cam.outputs);
        cam.outputs = NULL;
    }
    if (cam.output) { ACaptureSessionOutput_free(cam.output); cam.output = NULL; }
    if (cam.reader) { AImageReader_delete(cam.reader); cam.reader = NULL; }
    if (cam.mgr) { ACameraManager_delete(cam.mgr); cam.mgr = NULL; }
}

static void choose_size(const ACameraMetadata *meta, int want_w, int want_h, int *out_w, int *out_h) {
    ACameraMetadata_const_entry e;
    *out_w = want_w;
    *out_h = want_h;
    if (ACameraMetadata_getConstEntry(meta, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &e) != ACAMERA_OK) return;
    long want_area = (long) want_w * want_h;
    long best_score = LONG_MIN;
    for (uint32_t i = 0; i + 3 < e.count; i += 4) {
        int32_t fmt = e.data.i32[i], w = e.data.i32[i + 1], h = e.data.i32[i + 2], input = e.data.i32[i + 3];
        if (input || fmt != AIMAGE_FORMAT_YUV_420_888) continue;
        long area = (long) w * h;
        long score = 0;
        if (w == want_w && h == want_h) score += 1000000;
        if ((long) w * want_h == (long) h * want_w) score += 100000;
        score -= labs(area - want_area) / 100;
        if (area > 2 * want_area) score -= 50000; /* avoid needless huge frames */
        if (score > best_score) {
            best_score = score;
            *out_w = w;
            *out_h = h;
        }
    }
}

static void choose_fps_range(const ACameraMetadata *meta, int fps, int32_t range[2]) {
    ACameraMetadata_const_entry e;
    range[0] = range[1] = 0;
    if (ACameraMetadata_getConstEntry(meta, ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, &e) != ACAMERA_OK) return;
    int best = INT_MIN;
    for (uint32_t i = 0; i + 1 < e.count; i += 2) {
        int32_t lo = e.data.i32[i], hi = e.data.i32[i + 1];
        /* Prefer a fixed range at the requested rate, then the tightest range containing it. */
        int score = -abs(hi - fps) * 100 - (hi - lo);
        if (score > best) {
            best = score;
            range[0] = lo;
            range[1] = hi;
        }
    }
}

int ndk_camera_open(const char *id, int width, int height, int fps, char *err, size_t errlen) {
    pthread_mutex_lock(&g_lock);
    close_locked();

    cam.mgr = ACameraManager_create();
    ACameraMetadata *meta = NULL;
    if (ACameraManager_getCameraCharacteristics(cam.mgr, id, &meta) != ACAMERA_OK) {
        snprintf(err, errlen, "unknown camera %s", id);
        goto fail;
    }
    int w, h;
    int32_t range[2];
    choose_size(meta, width, height, &w, &h);
    choose_fps_range(meta, fps, range);
    ACameraMetadata_free(meta);

    static ACameraDevice_StateCallbacks dev_cbs = {
        .context = NULL, .onDisconnected = on_disconnected, .onError = on_error};
    camera_status_t s = ACameraManager_openCamera(cam.mgr, id, &dev_cbs, &cam.device);
    if (s != ACAMERA_OK) {
        snprintf(err, errlen, "openCamera failed (%d)%s", s,
                 s == ACAMERA_ERROR_PERMISSION_DENIED ? ": camera permission denied" : "");
        goto fail;
    }

    if (AImageReader_new(w, h, AIMAGE_FORMAT_YUV_420_888, 4, &cam.reader) != AMEDIA_OK) {
        snprintf(err, errlen, "AImageReader_new failed");
        goto fail;
    }
    static AImageReader_ImageListener listener = {.context = NULL, .onImageAvailable = on_image};
    AImageReader_setImageListener(cam.reader, &listener);
    ANativeWindow *win = NULL;
    AImageReader_getWindow(cam.reader, &win);

    ACaptureSessionOutputContainer_create(&cam.outputs);
    ACaptureSessionOutput_create(win, &cam.output);
    ACaptureSessionOutputContainer_add(cam.outputs, cam.output);
    ACameraOutputTarget_create(win, &cam.target);
    ACameraDevice_createCaptureRequest(cam.device, TEMPLATE_RECORD, &cam.request);
    ACaptureRequest_addTarget(cam.request, cam.target);
    if (range[1] > 0) ACaptureRequest_setEntry_i32(cam.request, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2, range);

    static ACameraCaptureSession_stateCallbacks session_cbs = {
        .context = NULL, .onClosed = on_session_closed, .onReady = on_session_ready, .onActive = on_session_active};
    if (ACameraDevice_createCaptureSession(cam.device, cam.outputs, &session_cbs, &cam.session) != ACAMERA_OK) {
        snprintf(err, errlen, "createCaptureSession failed");
        goto fail;
    }

    int rate = range[1] > 0 ? range[1] : fps;
    char caps[256], desc[128];
    snprintf(caps, sizeof caps,
             "video/x-raw,format=I420,width=%d,height=%d,framerate=%d/1,pixel-aspect-ratio=1/1",
             w & ~1, h & ~1, rate);
    snprintf(desc, sizeof desc, "Camera %s %dx%d @ %d-%d fps", id, w, h, range[0], range[1]);
    engine_video_opened(caps, desc);

    if (ACameraCaptureSession_setRepeatingRequest(cam.session, NULL, 1, &cam.request, NULL) != ACAMERA_OK) {
        snprintf(err, errlen, "setRepeatingRequest failed");
        engine_video_closed();
        goto fail;
    }
    cam.width = w;
    cam.height = h;
    LOGI("NDK camera streaming: %s", desc);
    pthread_mutex_unlock(&g_lock);
    return 0;

fail:
    LOGE("camera open failed: %s", err);
    close_locked();
    pthread_mutex_unlock(&g_lock);
    return -1;
}

void ndk_camera_close(void) {
    pthread_mutex_lock(&g_lock);
    int was_open = cam.device != NULL;
    close_locked();
    pthread_mutex_unlock(&g_lock);
    if (was_open) engine_video_closed();
}
