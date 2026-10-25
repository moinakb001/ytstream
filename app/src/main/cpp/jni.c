/* JNI entry points for com.ytstream.Native. */
#include <android/native_window_jni.h>
#include <gst/gst.h>
#include <jni.h>
#include <string.h>

#include "engine.h"
#include "http.h"
#include "log.h"
#include "session.h"

static int g_initialized;

static jstring to_jstring(JNIEnv *env, const char *s) {
    return s ? (*env)->NewStringUTF(env, s) : NULL;
}

static char *dup_jstring(JNIEnv *env, jstring s) {
    if (!s) return NULL;
    const char *c = (*env)->GetStringUTFChars(env, s, NULL);
    char *r = g_strdup(c);
    (*env)->ReleaseStringUTFChars(env, s, c);
    return r;
}

/* Returns a Java string for err (NULL means success) and frees it. */
static jstring take_error(JNIEnv *env, char *err) {
    jstring j = to_jstring(env, err);
    g_free(err);
    return j;
}

JNIEXPORT jstring JNICALL
Java_com_ytstream_Native_init(JNIEnv *env, jclass cls, jstring user_agent, jstring cache_dir) {
    (void) cls; (void) cache_dir;
    if (g_initialized) return NULL;
    if (!gst_is_initialized()) return to_jstring(env, "GStreamer is not initialized");
    char *ua = dup_jstring(env, user_agent);
    http_set_user_agent(ua);
    g_free(ua);
    engine_init();
    session_init();
    g_initialized = 1;
    LOGI("engine initialized, %s", gst_version_string());
    return NULL;
}

JNIEXPORT void JNICALL
Java_com_ytstream_Native_setPreviewSurface(JNIEnv *env, jclass cls, jobject surface) {
    (void) cls;
    ANativeWindow *w = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
    engine_set_preview_window(w);
    if (w) ANativeWindow_release(w); /* engine holds its own reference */
}

JNIEXPORT jstring JNICALL
Java_com_ytstream_Native_openUvcCamera(JNIEnv *env, jclass cls, jint fd, jint w, jint h, jint fps) {
    (void) cls;
    ndk_camera_close();
    char err[256];
    if (uvc_source_open(fd, w, h, fps, err, sizeof err) != 0) return to_jstring(env, err);
    return NULL;
}

JNIEXPORT jstring JNICALL
Java_com_ytstream_Native_openNdkCamera(JNIEnv *env, jclass cls, jstring id, jint w, jint h, jint fps) {
    (void) cls;
    uvc_source_close();
    char *cid = dup_jstring(env, id);
    char err[256];
    int r = ndk_camera_open(cid, w, h, fps, err, sizeof err);
    g_free(cid);
    return r != 0 ? to_jstring(env, err) : NULL;
}

JNIEXPORT void JNICALL
Java_com_ytstream_Native_closeVideo(JNIEnv *env, jclass cls) {
    (void) env; (void) cls;
    uvc_source_close();
    ndk_camera_close();
}

JNIEXPORT void JNICALL
Java_com_ytstream_Native_setTransform(JNIEnv *env, jclass cls, jint rotation, jboolean mirror) {
    (void) env; (void) cls;
    engine_set_transform(rotation, mirror);
}

JNIEXPORT jstring JNICALL
Java_com_ytstream_Native_startStream(JNIEnv *env, jclass cls, jstring config) {
    (void) cls;
    char *c = dup_jstring(env, config);
    char *err = session_start(c);
    g_free(c);
    return take_error(env, err);
}

JNIEXPORT void JNICALL
Java_com_ytstream_Native_stopStream(JNIEnv *env, jclass cls) {
    (void) env; (void) cls;
    session_stop();
}

JNIEXPORT void JNICALL
Java_com_ytstream_Native_setAccessToken(JNIEnv *env, jclass cls, jstring token) {
    (void) cls;
    char *t = dup_jstring(env, token);
    session_set_token(t);
    g_free(t);
}

JNIEXPORT jstring JNICALL
Java_com_ytstream_Native_updateBroadcast(JNIEnv *env, jclass cls, jstring title, jstring description) {
    (void) cls;
    char *t = dup_jstring(env, title);
    char *d = dup_jstring(env, description);
    char *err = session_update_broadcast(t, d);
    g_free(t);
    g_free(d);
    return take_error(env, err);
}

JNIEXPORT jstring JNICALL
Java_com_ytstream_Native_getStatus(JNIEnv *env, jclass cls) {
    (void) cls;
    if (!g_initialized) return to_jstring(env, "{\"phase\":\"idle\"}");
    char *s = session_status_json();
    jstring j = to_jstring(env, s);
    g_free(s);
    return j;
}
