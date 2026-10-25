package com.ytstream;

import android.view.Surface;

/**
 * JNI bridge to the native streaming engine (app/src/main/cpp).
 *
 * Everything media- and network-related (capture, GStreamer pipeline, HLS upload,
 * YouTube Live API) lives in C. Methods that return String return null on success
 * and an error message on failure, unless documented otherwise.
 */
public final class Native {
    static {
        // The app's C code is linked into the GStreamer library (see cpp/CMakeLists.txt).
        System.loadLibrary("gstreamer_android");
    }

    private Native() {}

    /** One-time engine init. Call after GStreamer.init(). */
    public static native String init(String userAgent, String cacheDir);

    /** Attach (or detach with null) the preview surface. */
    public static native void setPreviewSurface(Surface surface);

    /** Open a UVC camera from a UsbDeviceConnection file descriptor (the connection must stay open). */
    public static native String openUvcCamera(int fd, int width, int height, int fps);

    /** Open a camera through the NDK Camera2 API (built-in or external-HAL cameras). */
    public static native String openNdkCamera(String cameraId, int width, int height, int fps);

    /** Close the current video source. */
    public static native void closeVideo();

    /** Rotation in degrees (0/90/180/270) and horizontal mirroring applied to the outgoing video. */
    public static native void setTransform(int rotation, boolean mirror);

    /**
     * Start streaming. {@code configJson} is described in cpp/session.c. Returns immediately;
     * progress is reported through {@link #getStatus()}.
     */
    public static native String startStream(String configJson);

    /** Stop streaming (and complete the YouTube broadcast when one was created). Non-blocking. */
    public static native void stopStream();

    /** Supply a fresh OAuth access token (tokens expire after ~1h). */
    public static native void setAccessToken(String token);

    /** Update the live broadcast's title/description. Blocking network call: run off the UI thread. */
    public static native String updateBroadcast(String title, String description);

    /** JSON snapshot of engine state. */
    public static native String getStatus();
}
