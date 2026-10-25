cmake_minimum_required(VERSION 3.22)
project(ytstream C CXX)

set(CMAKE_C_STANDARD 11)

# ---------------------------------------------------------------- GStreamer SDK
# GSTREAMER_ROOT_ANDROID points at the extracted gstreamer-1.0-android-universal tarball
# (passed by Gradle from local.properties `gstreamer.dir` or $GSTREAMER_ROOT_ANDROID).
if(NOT GSTREAMER_ROOT_ANDROID)
    message(FATAL_ERROR "GSTREAMER_ROOT_ANDROID is not set (see README)")
endif()
if(ANDROID_ABI STREQUAL "armeabi-v7a")
    set(GSTREAMER_ABI armv7)
elseif(ANDROID_ABI STREQUAL "arm64-v8a")
    set(GSTREAMER_ABI arm64)
elseif(ANDROID_ABI STREQUAL "x86")
    set(GSTREAMER_ABI x86)
elseif(ANDROID_ABI STREQUAL "x86_64")
    set(GSTREAMER_ABI x86_64)
else()
    message(FATAL_ERROR "Unsupported ABI ${ANDROID_ABI}")
endif()
set(GStreamer_ROOT_DIR "${GSTREAMER_ROOT_ANDROID}/${GSTREAMER_ABI}")
list(APPEND CMAKE_MODULE_PATH "${GStreamer_ROOT_DIR}/share/cmake")

# The Java glue and CA bundle are staged by Gradle (app/build.gradle.kts); keep the copies
# FindGStreamerMobile makes out of the source tree.
set(GStreamer_JAVA_SRC_DIR "../../build/gstreamer-cmake/java")
set(GStreamer_ASSETS_DIR "../../build/gstreamer-cmake/assets")
set(G_IO_MODULES openssl)
set(GStreamer_EXTRA_DEPS libsoup-3.0)

find_package(GStreamerMobile REQUIRED COMPONENTS
    coreelements      # queue, tee
    app               # appsrc, appsink
    api_app
    videoconvertscale # videoconvert, videoscale
    videofilter       # videoflip
    videorate
    jpeg              # jpegdec (UVC MJPEG)
    audioconvert
    audioresample
    audiotestsrc      # silent audio track
    androidmedia      # MediaCodec hardware H.264 encoder
    x264              # software H.264 fallback
    openh264
    voaacenc          # AAC
    videoparsersbad   # h264parse
    audioparsers      # aacparse
    mpegtsmux
    multifile         # splitmuxsink
    gio               # giostreamsink
    ca_certificates
)

# pkg-config lists glib once for gstreamer-1.0 and again for libsoup-3.0; whole-archiving it
# twice produces duplicate symbols, so keep only the first of each whole-archive entry.
get_target_property(_gst_link GStreamerMobile LINK_LIBRARIES)
set(_gst_link_dedup)
foreach(_lib IN LISTS _gst_link)
    if(_lib MATCHES "^-Wl,--whole-archive" AND _lib IN_LIST _gst_link_dedup)
        continue()
    endif()
    list(APPEND _gst_link_dedup "${_lib}")
endforeach()
set_property(TARGET GStreamerMobile PROPERTY LINK_LIBRARIES "${_gst_link_dedup}")

# Private static dependencies of libsoup and the OpenSSL GIO TLS module.
foreach(_dep psl nghttp2 sqlite3 ssl crypto)
    target_link_libraries(GStreamerMobile PRIVATE "${GStreamer_ROOT_DIR}/lib/lib${_dep}.a")
endforeach()

# ---------------------------------------------------------------- libusb (Android backend)
set(LIBUSB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libusb)
add_library(usb1 STATIC
    ${LIBUSB_DIR}/libusb/core.c
    ${LIBUSB_DIR}/libusb/descriptor.c
    ${LIBUSB_DIR}/libusb/hotplug.c
    ${LIBUSB_DIR}/libusb/io.c
    ${LIBUSB_DIR}/libusb/sync.c
    ${LIBUSB_DIR}/libusb/strerror.c
    ${LIBUSB_DIR}/libusb/os/linux_usbfs.c
    ${LIBUSB_DIR}/libusb/os/events_posix.c
    ${LIBUSB_DIR}/libusb/os/threads_posix.c
    ${LIBUSB_DIR}/libusb/os/linux_netlink.c
)
target_include_directories(usb1
    PRIVATE ${LIBUSB_DIR}/android ${LIBUSB_DIR}/libusb/os
    PUBLIC ${LIBUSB_DIR}/libusb)
target_compile_options(usb1 PRIVATE -fvisibility=hidden -pthread -Wno-everything)
target_link_libraries(usb1 PRIVATE log)

# ---------------------------------------------------------------- libuvc
set(LIBUVC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libuvc)
set(LIBUVC_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/libuvc_gen)
file(WRITE ${LIBUVC_GEN_DIR}/libuvc/libuvc_config.h
"#ifndef LIBUVC_CONFIG_H
#define LIBUVC_CONFIG_H
#define LIBUVC_VERSION_MAJOR 0
#define LIBUVC_VERSION_MINOR 0
#define LIBUVC_VERSION_PATCH 7
#define LIBUVC_VERSION_STR \"0.0.7\"
#define LIBUVC_VERSION_INT ((0 << 16) | (0 << 8) | 7)
#define LIBUVC_VERSION_GTE(major, minor, patch) (LIBUVC_VERSION_INT >= (((major) << 16) | ((minor) << 8) | (patch)))
#endif
")
add_library(uvc STATIC
    ${LIBUVC_DIR}/src/ctrl.c
    ${LIBUVC_DIR}/src/ctrl-gen.c
    ${LIBUVC_DIR}/src/device.c
    ${LIBUVC_DIR}/src/diag.c
    ${LIBUVC_DIR}/src/frame.c
    ${LIBUVC_DIR}/src/init.c
    ${LIBUVC_DIR}/src/stream.c
    ${LIBUVC_DIR}/src/misc.c
)
target_include_directories(uvc PUBLIC ${LIBUVC_DIR}/include ${LIBUVC_GEN_DIR})
target_compile_options(uvc PRIVATE -Wno-everything)
target_link_libraries(uvc PUBLIC usb1)

# ---------------------------------------------------------------- cJSON
add_library(cjson STATIC third_party/cJSON/cJSON.c)
target_include_directories(cjson PUBLIC third_party/cJSON)

# ---------------------------------------------------------------- app
# Our code is compiled into the GStreamer shared library itself so that GLib, GStreamer and
# libsoup exist exactly once in the process.
target_sources(GStreamerMobile PRIVATE
    engine.c
    uvc_source.c
    ndk_camera.c
    audio_capture.c
    http.c
    hls_uploader.c
    youtube.c
    session.c
    jni.c
)
target_include_directories(GStreamerMobile PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${GStreamer_ROOT_DIR}/include/libsoup-3.0)
target_link_libraries(GStreamerMobile PRIVATE
    uvc cjson
    android log camera2ndk mediandk aaudio
)
