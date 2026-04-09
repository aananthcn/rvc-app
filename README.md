# rvc_app — Rear View Camera Streaming Service

A native C++ system service that watches the `vendor.rvc.camera.active`
property and streams the rear camera as **H.264 over RTP/UDP** to the
Instrument Cluster (`192.168.10.10:5004`) when the value is `"1"`.

`rvc_app` is one half of a two-process architecture:

```
rvc_service  (vendor)  ── vendor.rvc.camera.active ──►  rvc_app  (system)
  VHAL gear monitor                                  Camera + encoder + RTP
```

The split exists because `libcamera2ndk` is a system-namespace library and
cannot be loaded by vendor binaries.  `rvc_app` runs in `/system/bin` so the
system linker namespace resolves it without any hacks.

---

## Where to place this code in the AOSP tree

```
<AOSP_ROOT>/
└── vendor/
    └── brcm/
        └── rvc-app/                 ← this folder
            ├── Android.bp
            ├── rvc_app.rc
            ├── src/
            │   ├── main.cpp
            │   ├── CameraStreamManager.cpp
            │   └── RtpStreamer.cpp
            └── include/
                ├── CameraConfig.h
                ├── CameraStreamManager.h
                └── RtpStreamer.h
```

**Why not `vendor: true`?**
`libcamera2ndk` is an App NDK library that lives in the system linker
namespace.  Vendor binaries run in the vendor namespace and cannot access it.
Installing `rvc_app` to `/system/bin` (the default when `vendor: true` is
absent) keeps it in the system namespace where `libcamera2ndk` loads cleanly.

---

## How to build

```bash
# Set up the AOSP build environment (once per shell session)
source build/envsetup.sh
lunch <your_target>

# Build only this module
mmm vendor/brcm/rvc-app

# Or by module name
m rvc_app

# Output binary lands at:
# out/target/product/<device>/system/bin/rvc_app
```

---

## How to add this service to the device build

In `device/<oem>/<board>/device.mk`:

```makefile
PRODUCT_PACKAGES += \
    rvc_service \
    rvc_app
```

Both entries are required.  `rvc_app.rc` is installed to
`/system/etc/init/` automatically via the `init_rc` field in `Android.bp`.

---

## How to push and test during development (without a full flash)

```bash
adb root && adb remount

# Push the new binary
adb push out/target/product/<device>/system/bin/rvc_app /system/bin/rvc_app

# Push the RC file if changed
adb push vendor/brcm/rvc-app/rvc_app.rc /system/etc/init/rvc_app.rc

# Restart both services (rvc_app first so the socket exists)
adb shell stop rvc_app
adb shell stop rvc_service
adb shell start rvc_app
adb shell start rvc_service

# Watch logs
adb logcat -s RvcApp CameraStreamMgr RtpStreamer
```

---

## How to simulate a reverse gear event

Gear events are injected via `rvc_service`, which forwards them to `rvc_app`:

```bash
# Engage reverse (triggers rvc_app to open camera and stream)
adb shell cmd car_service inject-vhal-event 0x11400400 8

# Leave reverse (triggers rvc_app to stop stream)
adb shell cmd car_service inject-vhal-event 0x11400400 4
```

To test `rvc_app` directly without `rvc_service`, write the property:

```bash
# Start streaming
adb shell setprop vendor.rvc.camera.active 1

# Stop streaming
adb shell setprop vendor.rvc.camera.active 0
```

---

## How to receive the stream on the Instrument Cluster

### GStreamer (recommended)

```bash
gst-launch-1.0 \
  udpsrc port=5004 \
  caps="application/x-rtp,media=video,encoding-name=H264,payload=96" \
  ! rtph264depay ! avdec_h264 ! videoconvert ! autovideosink
```

### FFplay (quick test)

```bash
cat > rearview.sdp << 'EOF'
v=0
o=- 0 0 IN IP4 127.0.0.1
s=RearCamera
c=IN IP4 0.0.0.0
t=0 0
m=video 5004 RTP/AVP 96
a=rtpmap:96 H264/90000
EOF
ffplay -protocol_whitelist file,udp,rtp rearview.sdp
```

---

## Configuration

All tuneable values are in `include/CameraConfig.h`:

| Constant | Default | Description |
|---|---|---|
| `CAMERA_ID` | `"0"` | Camera HAL device ID |
| `VIDEO_WIDTH` / `VIDEO_HEIGHT` | `1280` / `720` | Capture resolution |
| `CAPTURE_FPS` | `30` | Target frame rate |
| `VIDEO_BITRATE` | `2'000'000` | H.264 bitrate (bps) |
| `I_FRAME_INTERVAL` | `1` | IDR keyframe interval (seconds) |
| `RTP_DEST_IP` | `"192.168.10.10"` | Instrument Cluster IP |
| `RTP_DEST_PORT` | `5004` | Destination UDP port |
| `RTP_MTU` | `1400` | Max RTP packet payload (bytes) |
| `RTP_PAYLOAD_TYPE` | `96` | RTP dynamic payload type for H.264 |

---

## Architecture and data flow

```
init (rvc_app.rc)
  └── main()
        │  __system_property_wait("vendor.rvc.camera.active")
        │  listen() + accept()
        │
        │  on "start\n" from rvc_service:
        │    CameraStreamManager::open()
        │      RtpStreamer::start()           UDP socket → 192.168.10.10:5004
        │      AMediaCodec (H.264 encoder, Surface input)
        │      ACameraManager_openCamera()
        │      ACameraCaptureSession → ANativeWindow (encoder surface)
        │      encoderLoop() thread
        │        AMediaCodec_dequeueOutputBuffer()
        │          RtpStreamer::sendAnnexB()
        │            strip start codes → NAL units
        │            single-NAL / FU-A packetisation (RFC 3984)
        │            sendto() UDP ──────────► 192.168.10.10:5004
        │
        │  on "stop\n" from rvc_service (or client disconnect):
        │    CameraStreamManager::close()
        │      teardown(): session → device → manager → codec → surface
        │      RtpStreamer::stop()
        │
        └── accept() next connection …
```

---

## Troubleshooting

| Symptom | Likely cause & fix |
|---|---|
| `rvc_app` exits immediately on start | Check logcat: `adb logcat -s RvcApp`. Normally it starts with `class main` automatically. |
| No stream when reverse engaged | Check `rvc_service` is running: `adb logcat -s RearViewCameraSvc` should show "set vendor.rvc.camera.active=1"; check `rvc_app` logcat for "REVERSE — opening camera" |
| `ACameraManager_openCamera failed` | Wrong `CAMERA_ID` in `CameraConfig.h`, or camera already in use by another process |
| No UDP packets at cluster | Verify route: `adb shell ping 192.168.10.10`; check `RTP_DEST_IP` in `CameraConfig.h` |
| Blurry / corrupted video | Increase `VIDEO_BITRATE` or check for packet loss: `tcpdump -i eth0 udp port 5004` |
| `libcamera2ndk.so` not found | Binary was built with `vendor: true` by mistake — check `Android.bp` has no `vendor:` flag |
