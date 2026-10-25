/* Host stub: no AAudio, so the engine falls back to its silent track. */
#include <stdio.h>
#include "engine.h"

audio_capture *audio_capture_open(int device_id, int *rate, int *channels, char *err, size_t errlen) {
    (void) device_id; (void) rate; (void) channels;
    snprintf(err, errlen, "no AAudio on host");
    return NULL;
}
void audio_capture_start(audio_capture *a, GstElement *appsrc, GstElement *pipeline) {
    (void) a; (void) appsrc; (void) pipeline;
}
void audio_capture_close(audio_capture *a) { (void) a; }
