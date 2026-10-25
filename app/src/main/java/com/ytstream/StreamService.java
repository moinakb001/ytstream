package com.ytstream;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;

/**
 * Foreground service that keeps the process (and therefore the native engine) alive and
 * allowed to use the camera and microphone while the app is in the background. It also
 * refreshes the YouTube access token so the broadcast can be completed after hours of streaming.
 */
public class StreamService extends Service {
    static final String EXTRA_USE_MIC = "use_mic";
    static final String EXTRA_ACCOUNT_MODE = "account_mode";
    private static final String CHANNEL = "stream";
    private static final int NOTIFICATION_ID = 1;
    private static final long TOKEN_REFRESH_MS = 40 * 60 * 1000L;

    private PowerManager.WakeLock wakeLock;
    private WifiManager.WifiLock wifiLock;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private boolean accountMode;

    private final Runnable refreshToken = new Runnable() {
        @Override public void run() {
            YouTubeAuth.authorize(StreamService.this, new YouTubeAuth.Callback() {
                @Override public void onToken(String token) { Native.setAccessToken(token); }
                @Override public void onError(String message) {}
                @Override public void onNeedsConsent(PendingIntent intent) {}
            });
            handler.postDelayed(this, TOKEN_REFRESH_MS);
        }
    };

    static void start(Context ctx, boolean useMic, boolean accountMode) {
        Intent i = new Intent(ctx, StreamService.class)
                .putExtra(EXTRA_USE_MIC, useMic)
                .putExtra(EXTRA_ACCOUNT_MODE, accountMode);
        ctx.startForegroundService(i);
    }

    static void stop(Context ctx) {
        ctx.stopService(new Intent(ctx, StreamService.class));
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        boolean useMic = intent != null && intent.getBooleanExtra(EXTRA_USE_MIC, false);
        accountMode = intent != null && intent.getBooleanExtra(EXTRA_ACCOUNT_MODE, false);

        NotificationManager nm = getSystemService(NotificationManager.class);
        nm.createNotificationChannel(new NotificationChannel(CHANNEL, "Live stream",
                NotificationManager.IMPORTANCE_LOW));
        PendingIntent open = PendingIntent.getActivity(this, 0,
                new Intent(this, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
                PendingIntent.FLAG_IMMUTABLE);
        Notification n = new Notification.Builder(this, CHANNEL)
                .setSmallIcon(android.R.drawable.presence_video_online)
                .setContentTitle("Streaming to YouTube")
                .setContentText("Tap to open")
                .setContentIntent(open)
                .setOngoing(true)
                .build();

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            // camera/microphone service types exist from API 30 and are mandatory from 34.
            int types = ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA;
            if (useMic) types |= ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE;
            startForeground(NOTIFICATION_ID, n, types);
        } else {
            startForeground(NOTIFICATION_ID, n);
        }

        if (wakeLock == null) {
            wakeLock = getSystemService(PowerManager.class)
                    .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "ytstream:stream");
            wakeLock.acquire();
            WifiManager wm = (WifiManager) getApplicationContext().getSystemService(WIFI_SERVICE);
            if (wm != null) {
                wifiLock = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "ytstream:stream");
                wifiLock.acquire();
            }
        }
        handler.removeCallbacks(refreshToken);
        if (accountMode) handler.postDelayed(refreshToken, TOKEN_REFRESH_MS);
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        handler.removeCallbacks(refreshToken);
        if (wakeLock != null && wakeLock.isHeld()) wakeLock.release();
        if (wifiLock != null && wifiLock.isHeld()) wifiLock.release();
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }
}
