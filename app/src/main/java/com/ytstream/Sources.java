package com.ytstream;

import android.content.Context;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraManager;
import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import android.media.AudioDeviceInfo;
import android.media.AudioManager;

import java.util.ArrayList;
import java.util.List;

/** Enumerates selectable video and audio sources. Video and audio are chosen independently. */
final class Sources {
    private Sources() {}

    static final class Video {
        static final String UVC = "uvc";
        static final String CAMERA = "camera";

        final String kind;
        final String id;      // UsbDevice name for UVC, camera id for Camera2
        final String label;
        final UsbDevice usbDevice;

        Video(String kind, String id, String label, UsbDevice usbDevice) {
            this.kind = kind;
            this.id = id;
            this.label = label;
            this.usbDevice = usbDevice;
        }

        String key() { return kind + ":" + id; }

        @Override public String toString() { return label; }
    }

    static final class Audio {
        static final int NONE = -1;     // silent track (YouTube expects an audio track)
        static final int DEFAULT = 0;   // let Android pick

        final int deviceId;
        final String label;

        Audio(int deviceId, String label) {
            this.deviceId = deviceId;
            this.label = label;
        }

        @Override public String toString() { return label; }
    }

    static List<Video> listVideo(Context ctx) {
        List<Video> out = new ArrayList<>();
        UsbManager usb = (UsbManager) ctx.getSystemService(Context.USB_SERVICE);
        if (usb != null) {
            for (UsbDevice d : usb.getDeviceList().values()) {
                if (!isVideoDevice(d)) continue;
                String name = d.getProductName();
                if (name == null || name.isEmpty()) {
                    name = String.format("%04x:%04x", d.getVendorId(), d.getProductId());
                }
                out.add(new Video(Video.UVC, d.getDeviceName(), "USB: " + name, d));
            }
        }
        CameraManager cm = (CameraManager) ctx.getSystemService(Context.CAMERA_SERVICE);
        if (cm != null) {
            try {
                for (String id : cm.getCameraIdList()) {
                    Integer facing = cm.getCameraCharacteristics(id).get(CameraCharacteristics.LENS_FACING);
                    String where = "camera";
                    if (facing != null) {
                        switch (facing) {
                            case CameraCharacteristics.LENS_FACING_BACK: where = "back"; break;
                            case CameraCharacteristics.LENS_FACING_FRONT: where = "front"; break;
                            case CameraCharacteristics.LENS_FACING_EXTERNAL: where = "external"; break;
                        }
                    }
                    out.add(new Video(Video.CAMERA, id, "Camera " + id + " (" + where + ")", null));
                }
            } catch (Exception ignored) {
                // Camera service unavailable or permission not yet granted.
            }
        }
        return out;
    }

    static boolean isVideoDevice(UsbDevice d) {
        if (d.getDeviceClass() == UsbConstants.USB_CLASS_VIDEO) return true;
        for (int i = 0; i < d.getInterfaceCount(); i++) {
            if (d.getInterface(i).getInterfaceClass() == UsbConstants.USB_CLASS_VIDEO) return true;
        }
        return false;
    }

    static List<Audio> listAudio(Context ctx) {
        List<Audio> out = new ArrayList<>();
        out.add(new Audio(Audio.NONE, "None (silent track)"));
        out.add(new Audio(Audio.DEFAULT, "Default microphone"));
        AudioManager am = (AudioManager) ctx.getSystemService(Context.AUDIO_SERVICE);
        if (am != null) {
            for (AudioDeviceInfo d : am.getDevices(AudioManager.GET_DEVICES_INPUTS)) {
                String product = d.getProductName() == null ? "" : d.getProductName().toString();
                String label = typeName(d.getType());
                if (!product.isEmpty()) label += ": " + product;
                if (d.getAddress() != null && !d.getAddress().isEmpty()) label += " (" + d.getAddress() + ")";
                out.add(new Audio(d.getId(), label));
            }
        }
        return out;
    }

    private static String typeName(int type) {
        switch (type) {
            case AudioDeviceInfo.TYPE_BUILTIN_MIC: return "Built-in mic";
            case AudioDeviceInfo.TYPE_USB_DEVICE: return "USB audio";
            case AudioDeviceInfo.TYPE_USB_HEADSET: return "USB headset";
            case AudioDeviceInfo.TYPE_USB_ACCESSORY: return "USB accessory";
            case AudioDeviceInfo.TYPE_WIRED_HEADSET: return "Wired headset";
            case AudioDeviceInfo.TYPE_BLUETOOTH_SCO: return "Bluetooth";
            case AudioDeviceInfo.TYPE_BLE_HEADSET: return "Bluetooth LE";
            case AudioDeviceInfo.TYPE_TELEPHONY: return "Telephony";
            case AudioDeviceInfo.TYPE_FM_TUNER: return "FM tuner";
            case AudioDeviceInfo.TYPE_TV_TUNER: return "TV tuner";
            case AudioDeviceInfo.TYPE_LINE_ANALOG: return "Line in";
            case AudioDeviceInfo.TYPE_LINE_DIGITAL: return "Digital in";
            case AudioDeviceInfo.TYPE_HDMI: return "HDMI";
            case AudioDeviceInfo.TYPE_REMOTE_SUBMIX: return "Remote submix";
            default: return "Input type " + type;
        }
    }
}
