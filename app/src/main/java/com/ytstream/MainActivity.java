package com.ytstream;

import android.Manifest;
import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.SurfaceHolder;
import android.view.View;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.RadioGroup;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import org.freedesktop.gstreamer.GStreamer;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.List;

public class MainActivity extends Activity {
    private static final String ACTION_USB_PERMISSION = "com.ytstream.USB_PERMISSION";
    private static final int RC_PERMISSIONS = 1;
    private static final int RC_AUTH = 2;
    private static final int RC_THUMBNAIL = 3;

    private static final String[] RESOLUTIONS = {"1920x1080", "1280x720", "854x480", "640x360"};
    private static final int[] DEFAULT_BITRATES = {6000, 4000, 2000, 1000};
    private static final String[] FPS = {"24", "25", "30", "50", "60"};
    private static final String[] AUDIO_BITRATES = {"96 kbps", "128 kbps", "160 kbps", "192 kbps"};
    private static final int[] AUDIO_BITRATE_VALUES = {96, 128, 160, 192};
    private static final String[] SEGMENTS = {"1 s", "2 s", "3 s", "4 s"};
    private static final String[] ROTATIONS = {"0°", "90°", "180°", "270°"};
    private static final String[] PRIVACY_LABELS = {"Public", "Unlisted", "Private"};
    private static final String[] PRIVACY_VALUES = {"public", "unlisted", "private"};
    private static final String[] LATENCY_LABELS = {"Normal", "Low", "Ultra low"};
    private static final String[] LATENCY_VALUES = {"normal", "low", "ultraLow"};
    private static final String[] ENCODER_LABELS = {"Auto (prefer hardware)", "Hardware (MediaCodec)", "Software (x264/OpenH264)"};
    private static final String[] ENCODER_VALUES = {"auto", "hardware", "software"};
    // Common YouTube video category ids.
    private static final String[] CATEGORY_LABELS = {"People & Blogs", "Gaming", "Education", "Science & Technology",
            "Entertainment", "Music", "Sports", "News & Politics", "Howto & Style", "Film & Animation",
            "Autos & Vehicles", "Pets & Animals", "Travel & Events", "Comedy", "Nonprofits & Activism"};
    private static final String[] CATEGORY_VALUES = {"22", "20", "27", "28", "24", "10", "17", "25", "26", "1",
            "2", "15", "19", "23", "29"};

    private final Handler handler = new Handler(Looper.getMainLooper());
    private SharedPreferences prefs;
    private UsbManager usbManager;

    private Spinner videoSource, audioSource, rotation, privacy, category, latency;
    private Spinner resolution, fps, audioBitrate, segment, encoder;
    private Switch mirror, madeForKids, dvr, autoStop;
    private EditText title, description, tags, streamKey, videoBitrate;
    private RadioGroup mode;
    private TextView status, authStatus, thumbnailStatus;
    private Button goLive, openYouTube, updateMetadata;
    private View accountGroup, keyGroup;

    private List<Sources.Video> videos = new ArrayList<>();
    private List<Sources.Audio> audios = new ArrayList<>();
    // Process-wide: the native engine outlives this activity while streaming in the background,
    // and a UsbDeviceConnection closes its fd when garbage-collected.
    private static UsbDeviceConnection usbConnection;
    private static String openedVideoKey;
    private static String accessToken;
    private String watchUrl;
    private boolean live;
    private boolean stopping;
    private boolean suppressSpinnerEvents;

    private final BroadcastReceiver usbReceiver = new BroadcastReceiver() {
        @Override public void onReceive(Context context, Intent intent) {
            String action = intent.getAction();
            UsbDevice device = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
            if (ACTION_USB_PERMISSION.equals(action)) {
                if (intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) {
                    openSelectedVideo();
                } else {
                    toast("USB permission denied");
                }
            } else if (UsbManager.ACTION_USB_DEVICE_DETACHED.equals(action)) {
                if (device != null && openedVideoKey != null
                        && openedVideoKey.equals(Sources.Video.UVC + ":" + device.getDeviceName())) {
                    closeVideo();
                    toast("USB camera disconnected");
                }
                refreshSources();
            } else if (UsbManager.ACTION_USB_DEVICE_ATTACHED.equals(action)) {
                refreshSources();
            }
        }
    };

    private final Runnable poll = new Runnable() {
        @Override public void run() {
            updateStatus();
            handler.postDelayed(this, 1000);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        try {
            System.loadLibrary("gstreamer_android"); // GStreamer.init() needs it loaded
            GStreamer.init(this);
        } catch (Exception e) {
            Toast.makeText(this, "GStreamer init failed: " + e.getMessage(), Toast.LENGTH_LONG).show();
            finish();
            return;
        }
        String err = Native.init(userAgent(), getCacheDir().getAbsolutePath());
        if (err != null) toast(err);

        setContentView(R.layout.activity_main);
        prefs = getSharedPreferences("settings", MODE_PRIVATE);
        usbManager = (UsbManager) getSystemService(USB_SERVICE);
        bindViews();
        setupSpinners();
        loadPrefs();
        setupListeners();

        IntentFilter f = new IntentFilter(ACTION_USB_PERMISSION);
        f.addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED);
        f.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);
        registerReceiver(usbReceiver, f, Context.RECEIVER_EXPORTED);

        ((AspectSurfaceView) findViewById(R.id.preview)).getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder h) { Native.setPreviewSurface(h.getSurface()); }
            @Override public void surfaceChanged(SurfaceHolder h, int fmt, int w, int ht) { Native.setPreviewSurface(h.getSurface()); }
            @Override public void surfaceDestroyed(SurfaceHolder h) { Native.setPreviewSurface(null); }
        });

        requestRuntimePermissions();
        handleUsbAttachIntent(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        handleUsbAttachIntent(intent);
    }

    @Override
    protected void onStart() {
        super.onStart();
        updateStatus(); // learn whether a stream is already running before touching sources
        if (accessToken != null) authStatus.setText(R.string.signed_in);
        refreshSources();
        handler.post(poll);
    }

    @Override
    protected void onStop() {
        super.onStop();
        handler.removeCallbacks(poll);
        savePrefs();
        // Keep the camera only while streaming; otherwise release it when not visible.
        if (!live && !isChangingConfigurations()) closeVideo();
    }

    @Override
    protected void onDestroy() {
        try { unregisterReceiver(usbReceiver); } catch (IllegalArgumentException ignored) {}
        super.onDestroy();
    }

    // ---------------------------------------------------------------- setup

    private void bindViews() {
        videoSource = findViewById(R.id.video_source);
        audioSource = findViewById(R.id.audio_source);
        rotation = findViewById(R.id.rotation);
        mirror = findViewById(R.id.mirror);
        mode = findViewById(R.id.mode);
        accountGroup = findViewById(R.id.account_group);
        keyGroup = findViewById(R.id.key_group);
        authStatus = findViewById(R.id.auth_status);
        title = findViewById(R.id.title);
        description = findViewById(R.id.description);
        privacy = findViewById(R.id.privacy);
        category = findViewById(R.id.category);
        tags = findViewById(R.id.tags);
        latency = findViewById(R.id.latency);
        madeForKids = findViewById(R.id.made_for_kids);
        dvr = findViewById(R.id.dvr);
        autoStop = findViewById(R.id.auto_stop);
        thumbnailStatus = findViewById(R.id.thumbnail_status);
        streamKey = findViewById(R.id.stream_key);
        resolution = findViewById(R.id.resolution);
        fps = findViewById(R.id.fps);
        videoBitrate = findViewById(R.id.video_bitrate);
        audioBitrate = findViewById(R.id.audio_bitrate);
        segment = findViewById(R.id.segment_duration);
        encoder = findViewById(R.id.encoder);
        status = findViewById(R.id.status);
        goLive = findViewById(R.id.go_live);
        openYouTube = findViewById(R.id.open_youtube);
        updateMetadata = findViewById(R.id.update_metadata);
    }

    private void setupSpinners() {
        setItems(rotation, ROTATIONS);
        setItems(privacy, PRIVACY_LABELS);
        setItems(category, CATEGORY_LABELS);
        setItems(latency, LATENCY_LABELS);
        setItems(resolution, RESOLUTIONS);
        setItems(fps, FPS);
        setItems(audioBitrate, AUDIO_BITRATES);
        setItems(segment, SEGMENTS);
        setItems(encoder, ENCODER_LABELS);
    }

    private void setItems(Spinner s, Object[] items) {
        ArrayAdapter<Object> a = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, items);
        a.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        s.setAdapter(a);
    }

    private void setupListeners() {
        goLive.setOnClickListener(v -> {
            if (live) stopStreaming(); else startStreaming();
        });
        openYouTube.setOnClickListener(v -> {
            if (watchUrl != null) startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(watchUrl)));
        });
        findViewById(R.id.refresh_sources).setOnClickListener(v -> refreshSources());
        findViewById(R.id.sign_in).setOnClickListener(v -> signIn(true));
        findViewById(R.id.thumbnail).setOnClickListener(v -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("image/*");
            startActivityForResult(i, RC_THUMBNAIL);
        });
        findViewById(R.id.thumbnail).setOnLongClickListener(v -> {
            if (thumbnailFile().delete()) thumbnailStatus.setText(R.string.no_thumbnail);
            return true;
        });
        updateMetadata.setOnClickListener(v -> {
            String t = title.getText().toString().trim();
            String d = description.getText().toString();
            updateMetadata.setEnabled(false);
            new Thread(() -> {
                String err = Native.updateBroadcast(t, d);
                runOnUiThread(() -> {
                    updateMetadata.setEnabled(live);
                    toast(err == null ? "Broadcast updated" : "Update failed: " + err);
                });
            }).start();
        });
        mode.setOnCheckedChangeListener((g, id) -> updateModeVisibility());

        AdapterView.OnItemSelectedListener reopen = new SimpleItemSelected(() -> {
            if (!suppressSpinnerEvents && !live) openSelectedVideo();
        });
        videoSource.setOnItemSelectedListener(reopen);
        resolution.setOnItemSelectedListener(new SimpleItemSelected(() -> {
            if (suppressSpinnerEvents) return;
            videoBitrate.setText(String.valueOf(DEFAULT_BITRATES[resolution.getSelectedItemPosition()]));
            if (!live) openSelectedVideo();
        }));
        fps.setOnItemSelectedListener(reopen);
        AdapterView.OnItemSelectedListener transform = new SimpleItemSelected(this::applyTransform);
        rotation.setOnItemSelectedListener(transform);
        mirror.setOnCheckedChangeListener((b, c) -> applyTransform());
    }

    private void updateModeVisibility() {
        boolean account = mode.getCheckedRadioButtonId() == R.id.mode_account;
        accountGroup.setVisibility(account ? View.VISIBLE : View.GONE);
        keyGroup.setVisibility(account ? View.GONE : View.VISIBLE);
    }

    private void applyTransform() {
        Native.setTransform(rotation.getSelectedItemPosition() * 90, mirror.isChecked());
    }

    // ---------------------------------------------------------------- permissions

    private void requestRuntimePermissions() {
        List<String> need = new ArrayList<>();
        need.add(Manifest.permission.CAMERA);
        need.add(Manifest.permission.RECORD_AUDIO);
        if (Build.VERSION.SDK_INT >= 33) need.add(Manifest.permission.POST_NOTIFICATIONS);
        List<String> missing = new ArrayList<>();
        for (String p : need) {
            if (checkSelfPermission(p) != PackageManager.PERMISSION_GRANTED) missing.add(p);
        }
        if (!missing.isEmpty()) requestPermissions(missing.toArray(new String[0]), RC_PERMISSIONS);
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        refreshSources();
        openSelectedVideo();
    }

    private boolean has(String permission) {
        return checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED;
    }

    // ---------------------------------------------------------------- sources

    private void handleUsbAttachIntent(Intent intent) {
        if (intent == null || !UsbManager.ACTION_USB_DEVICE_ATTACHED.equals(intent.getAction())) return;
        UsbDevice d = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
        if (d == null || !Sources.isVideoDevice(d)) return;
        prefs.edit().putString("video_key", Sources.Video.UVC + ":" + d.getDeviceName()).apply();
        refreshSources();
    }

    private void refreshSources() {
        suppressSpinnerEvents = true;
        String videoKey = selectedVideo() != null ? selectedVideo().key() : prefs.getString("video_key", null);
        int audioId = selectedAudio() != null ? selectedAudio().deviceId : prefs.getInt("audio_device", Sources.Audio.DEFAULT);

        videos = Sources.listVideo(this);
        audios = Sources.listAudio(this);
        setItems(videoSource, videos.toArray());
        setItems(audioSource, audios.toArray());
        for (int i = 0; i < videos.size(); i++) {
            if (videos.get(i).key().equals(videoKey)) videoSource.setSelection(i);
        }
        for (int i = 0; i < audios.size(); i++) {
            if (audios.get(i).deviceId == audioId) audioSource.setSelection(i);
        }
        suppressSpinnerEvents = false;

        Sources.Video v = selectedVideo();
        if (!live && v != null && !v.key().equals(openedVideoKey)) openSelectedVideo();
    }

    private Sources.Video selectedVideo() {
        int i = videoSource == null ? -1 : videoSource.getSelectedItemPosition();
        return i >= 0 && i < videos.size() ? videos.get(i) : null;
    }

    private Sources.Audio selectedAudio() {
        int i = audioSource == null ? -1 : audioSource.getSelectedItemPosition();
        return i >= 0 && i < audios.size() ? audios.get(i) : null;
    }

    private int[] selectedSize() {
        String[] p = RESOLUTIONS[resolution.getSelectedItemPosition()].split("x");
        return new int[]{Integer.parseInt(p[0]), Integer.parseInt(p[1])};
    }

    private int selectedFps() {
        return Integer.parseInt(FPS[fps.getSelectedItemPosition()]);
    }

    private void openSelectedVideo() {
        Sources.Video v = selectedVideo();
        if (v == null || !has(Manifest.permission.CAMERA)) return;
        int[] size = selectedSize();
        int rate = selectedFps();

        if (Sources.Video.UVC.equals(v.kind)) {
            if (!usbManager.hasPermission(v.usbDevice)) {
                Intent i = new Intent(ACTION_USB_PERMISSION).setPackage(getPackageName());
                PendingIntent pi = PendingIntent.getBroadcast(this, 0, i, PendingIntent.FLAG_MUTABLE);
                usbManager.requestPermission(v.usbDevice, pi);
                return;
            }
            closeVideo();
            usbConnection = usbManager.openDevice(v.usbDevice);
            if (usbConnection == null) {
                toast("Could not open USB device");
                return;
            }
            String err = Native.openUvcCamera(usbConnection.getFileDescriptor(), size[0], size[1], rate);
            if (err != null) {
                toast("USB camera: " + err);
                closeVideo();
                return;
            }
        } else {
            closeVideo();
            String err = Native.openNdkCamera(v.id, size[0], size[1], rate);
            if (err != null) {
                toast("Camera: " + err);
                return;
            }
        }
        openedVideoKey = v.key();
        applyTransform();
    }

    private void closeVideo() {
        Native.closeVideo();
        openedVideoKey = null;
        if (usbConnection != null) {
            usbConnection.close();
            usbConnection = null;
        }
    }

    // ---------------------------------------------------------------- auth

    private void signIn(boolean interactive) {
        YouTubeAuth.authorize(this, new YouTubeAuth.Callback() {
            @Override public void onToken(String token) {
                accessToken = token;
                Native.setAccessToken(token);
                authStatus.setText(R.string.signed_in);
            }
            @Override public void onError(String message) {
                authStatus.setText("Sign-in failed: " + message);
            }
            @Override public void onNeedsConsent(PendingIntent intent) {
                if (!interactive) return;
                try {
                    startIntentSenderForResult(intent.getIntentSender(), RC_AUTH, null, 0, 0, 0);
                } catch (Exception e) {
                    authStatus.setText("Sign-in failed: " + e.getMessage());
                }
            }
        });
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == RC_AUTH) {
            String token = resultCode == RESULT_OK ? YouTubeAuth.tokenFromConsentResult(this, data) : null;
            if (token != null) {
                accessToken = token;
                Native.setAccessToken(token);
                authStatus.setText(R.string.signed_in);
            } else {
                authStatus.setText(R.string.not_signed_in);
            }
        } else if (requestCode == RC_THUMBNAIL && resultCode == RESULT_OK && data != null && data.getData() != null) {
            saveThumbnail(data.getData());
        }
    }

    /** YouTube thumbnails: JPEG/PNG up to 2 MB; we store a 1280x720-bounded JPEG. */
    private void saveThumbnail(Uri uri) {
        try (InputStream in = getContentResolver().openInputStream(uri)) {
            Bitmap b = BitmapFactory.decodeStream(in);
            if (b == null) throw new IllegalArgumentException("not an image");
            float scale = Math.min(1f, Math.min(1280f / b.getWidth(), 720f / b.getHeight()));
            if (scale < 1f) {
                b = Bitmap.createScaledBitmap(b, Math.round(b.getWidth() * scale), Math.round(b.getHeight() * scale), true);
            }
            try (FileOutputStream out = new FileOutputStream(thumbnailFile())) {
                b.compress(Bitmap.CompressFormat.JPEG, 88, out);
            }
            thumbnailStatus.setText(b.getWidth() + "×" + b.getHeight() + " JPEG · " + getString(R.string.thumbnail_hint));
        } catch (Exception e) {
            toast("Thumbnail: " + e.getMessage());
        }
    }

    private File thumbnailFile() {
        return new File(getFilesDir(), "thumbnail.jpg");
    }

    // ---------------------------------------------------------------- streaming

    private void startStreaming() {
        boolean account = mode.getCheckedRadioButtonId() == R.id.mode_account;
        if (openedVideoKey == null) {
            toast("Select and open a video source first");
            return;
        }
        if (account && accessToken == null) {
            toast("Sign in to YouTube first");
            signIn(true);
            return;
        }
        if (!account && streamKey.getText().toString().trim().isEmpty()) {
            toast("Enter a stream key");
            return;
        }
        if (account && title.getText().toString().trim().isEmpty()) {
            toast("Enter a title");
            return;
        }

        Sources.Audio a = selectedAudio();
        int audioDevice = a == null ? Sources.Audio.NONE : a.deviceId;
        if (audioDevice != Sources.Audio.NONE && !has(Manifest.permission.RECORD_AUDIO)) {
            toast("Microphone permission missing; streaming a silent track");
            audioDevice = Sources.Audio.NONE;
        }

        String config;
        try {
            config = buildConfig(account, audioDevice).toString();
        } catch (JSONException e) {
            toast(e.getMessage());
            return;
        }
        savePrefs();
        String err = Native.startStream(config);
        if (err != null) {
            toast(err);
            return;
        }
        live = true;
        StreamService.start(this, audioDevice != Sources.Audio.NONE, account);
        refreshControls();
    }

    private JSONObject buildConfig(boolean account, int audioDevice) throws JSONException {
        int[] size = selectedSize();
        JSONObject c = new JSONObject();
        c.put("mode", account ? "account" : "key");
        c.put("accessToken", accessToken);
        c.put("streamKey", streamKey.getText().toString().trim());
        c.put("title", title.getText().toString().trim());
        c.put("description", description.getText().toString());
        c.put("privacy", PRIVACY_VALUES[privacy.getSelectedItemPosition()]);
        c.put("categoryId", CATEGORY_VALUES[category.getSelectedItemPosition()]);
        JSONArray tagArray = new JSONArray();
        for (String t : tags.getText().toString().split(",")) {
            if (!t.trim().isEmpty()) tagArray.put(t.trim());
        }
        c.put("tags", tagArray);
        c.put("latency", LATENCY_VALUES[latency.getSelectedItemPosition()]);
        c.put("madeForKids", madeForKids.isChecked());
        c.put("dvr", dvr.isChecked());
        c.put("autoStop", autoStop.isChecked());
        if (thumbnailFile().exists()) c.put("thumbnailPath", thumbnailFile().getAbsolutePath());
        c.put("width", size[0]);
        c.put("height", size[1]);
        c.put("fps", selectedFps());
        c.put("videoBitrateKbps", parseInt(videoBitrate.getText().toString(), DEFAULT_BITRATES[resolution.getSelectedItemPosition()]));
        c.put("audioBitrateKbps", AUDIO_BITRATE_VALUES[audioBitrate.getSelectedItemPosition()]);
        c.put("segmentSeconds", segment.getSelectedItemPosition() + 1);
        c.put("encoder", ENCODER_VALUES[encoder.getSelectedItemPosition()]);
        c.put("audioDeviceId", audioDevice);
        return c;
    }

    private void stopStreaming() {
        Native.stopStream();
        stopping = true;
        refreshControls();
    }

    private void updateStatus() {
        String json = Native.getStatus();
        try {
            JSONObject s = new JSONObject(json);
            String phase = s.optString("phase", "idle");
            boolean nowLive = phase.equals("starting") || phase.equals("live") || phase.equals("stopping");
            if (!nowLive) {
                stopping = false;
                if (live) StreamService.stop(this);
            }
            live = nowLive;
            watchUrl = s.optString("watchUrl", null);
            if (watchUrl != null && watchUrl.isEmpty()) watchUrl = null;

            StringBuilder b = new StringBuilder();
            b.append(phase.toUpperCase());
            String msg = s.optString("message");
            if (!msg.isEmpty()) b.append(" — ").append(msg);
            String video = s.optString("video");
            if (!video.isEmpty()) b.append("\nVideo: ").append(video);
            String enc = s.optString("encoders");
            if (!enc.isEmpty()) b.append("\nEncoders: ").append(enc);
            if (nowLive) {
                b.append(String.format("\nSegments: %d sent, %d queued, %d dropped · %d kbps",
                        s.optInt("segmentsSent"), s.optInt("segmentsQueued"),
                        s.optInt("segmentsDropped"), s.optInt("uploadKbps")));
                String yt = s.optString("youtube");
                if (!yt.isEmpty()) b.append("\nYouTube: ").append(yt);
            }
            String error = s.optString("error");
            if (!error.isEmpty()) b.append("\nError: ").append(error);
            status.setText(b.toString());
        } catch (JSONException e) {
            status.setText(json);
        }
        refreshControls();
    }

    private void refreshControls() {
        if (stopping) {
            goLive.setText(R.string.stopping);
        } else {
            goLive.setText(live ? R.string.stop : R.string.go_live);
        }
        goLive.setEnabled(!stopping);
        openYouTube.setEnabled(watchUrl != null);
        boolean account = mode.getCheckedRadioButtonId() == R.id.mode_account;
        updateMetadata.setEnabled(live && account);
        // Source/encoding changes apply when not live.
        for (View v : new View[]{videoSource, audioSource, resolution, fps, videoBitrate, audioBitrate, segment, encoder}) {
            v.setEnabled(!live);
        }
        for (int i = 0; i < mode.getChildCount(); i++) mode.getChildAt(i).setEnabled(!live);
    }

    // ---------------------------------------------------------------- prefs

    private void loadPrefs() {
        suppressSpinnerEvents = true;
        mode.check(prefs.getBoolean("account_mode", true) ? R.id.mode_account : R.id.mode_key);
        updateModeVisibility();
        title.setText(prefs.getString("title", "Live from my USB camera"));
        description.setText(prefs.getString("description", ""));
        tags.setText(prefs.getString("tags", ""));
        streamKey.setText(prefs.getString("stream_key", ""));
        privacy.setSelection(prefs.getInt("privacy", 1));
        category.setSelection(prefs.getInt("category", 0));
        latency.setSelection(prefs.getInt("latency", 0));
        madeForKids.setChecked(prefs.getBoolean("made_for_kids", false));
        dvr.setChecked(prefs.getBoolean("dvr", true));
        autoStop.setChecked(prefs.getBoolean("auto_stop", true));
        resolution.setSelection(prefs.getInt("resolution", 1));
        fps.setSelection(prefs.getInt("fps", 2));
        videoBitrate.setText(String.valueOf(prefs.getInt("video_bitrate", DEFAULT_BITRATES[resolution.getSelectedItemPosition()])));
        audioBitrate.setSelection(prefs.getInt("audio_bitrate", 1));
        segment.setSelection(prefs.getInt("segment", 1));
        encoder.setSelection(prefs.getInt("encoder", 0));
        rotation.setSelection(prefs.getInt("rotation", 0));
        mirror.setChecked(prefs.getBoolean("mirror", false));
        if (thumbnailFile().exists()) thumbnailStatus.setText("thumbnail.jpg");
        // Re-enable spinner listeners after the selections above have been dispatched.
        handler.post(() -> suppressSpinnerEvents = false);
    }

    private void savePrefs() {
        if (prefs == null) return;
        SharedPreferences.Editor e = prefs.edit();
        e.putBoolean("account_mode", mode.getCheckedRadioButtonId() == R.id.mode_account);
        e.putString("title", title.getText().toString());
        e.putString("description", description.getText().toString());
        e.putString("tags", tags.getText().toString());
        e.putString("stream_key", streamKey.getText().toString());
        e.putInt("privacy", privacy.getSelectedItemPosition());
        e.putInt("category", category.getSelectedItemPosition());
        e.putInt("latency", latency.getSelectedItemPosition());
        e.putBoolean("made_for_kids", madeForKids.isChecked());
        e.putBoolean("dvr", dvr.isChecked());
        e.putBoolean("auto_stop", autoStop.isChecked());
        e.putInt("resolution", resolution.getSelectedItemPosition());
        e.putInt("fps", fps.getSelectedItemPosition());
        e.putInt("video_bitrate", parseInt(videoBitrate.getText().toString(), 4000));
        e.putInt("audio_bitrate", audioBitrate.getSelectedItemPosition());
        e.putInt("segment", segment.getSelectedItemPosition());
        e.putInt("encoder", encoder.getSelectedItemPosition());
        e.putInt("rotation", rotation.getSelectedItemPosition());
        e.putBoolean("mirror", mirror.isChecked());
        Sources.Video v = selectedVideo();
        if (v != null) e.putString("video_key", v.key());
        Sources.Audio a = selectedAudio();
        if (a != null) e.putInt("audio_device", a.deviceId);
        e.apply();
    }

    // ---------------------------------------------------------------- util

    private static int parseInt(String s, int def) {
        try { return Integer.parseInt(s.trim()); } catch (NumberFormatException e) { return def; }
    }

    private String userAgent() {
        return Build.MANUFACTURER + " / " + Build.MODEL + " / ytstream-1.0";
    }

    private void toast(String s) {
        Toast.makeText(this, s, Toast.LENGTH_LONG).show();
    }

    private static final class SimpleItemSelected implements AdapterView.OnItemSelectedListener {
        private final Runnable r;
        SimpleItemSelected(Runnable r) { this.r = r; }
        @Override public void onItemSelected(AdapterView<?> p, View v, int pos, long id) { r.run(); }
        @Override public void onNothingSelected(AdapterView<?> p) {}
    }
}
