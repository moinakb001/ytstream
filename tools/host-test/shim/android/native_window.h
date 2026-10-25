/* Host shim: no preview window on the host. */
#pragma once
#include <stdint.h>

typedef struct ANativeWindow ANativeWindow;
typedef struct {
    int32_t width, height, stride, format;
    void *bits;
    uint32_t reserved[6];
} ANativeWindow_Buffer;

#define WINDOW_FORMAT_RGBA_8888 1

static inline void ANativeWindow_acquire(ANativeWindow *w) { (void) w; }
static inline void ANativeWindow_release(ANativeWindow *w) { (void) w; }
static inline int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *w, int32_t a, int32_t b, int32_t c) {
    (void) w; (void) a; (void) b; (void) c;
    return 0;
}
static inline int32_t ANativeWindow_lock(ANativeWindow *w, ANativeWindow_Buffer *b, void *r) {
    (void) w; (void) b; (void) r;
    return -1;
}
static inline int32_t ANativeWindow_unlockAndPost(ANativeWindow *w) { (void) w; return 0; }
