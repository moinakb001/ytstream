/* Host shim: route Android logging to stderr. */
#pragma once
#include <stdarg.h>
#include <stdio.h>

enum { ANDROID_LOG_DEBUG = 3, ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR };

static inline int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    static const char *names = "??VDIWEF";
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%c/%s: ", names[prio & 7], tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    return 0;
}
