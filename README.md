# rvc_app — Rear View Camera Streaming Service

A native C++ **system** service that watches the `vendor.rvc.camera.active`
property and streams the rear camera as **H.264 over RTP/UDP** to the
Instrument Cluster (`192.168.10.10:5004`) whenever the value is `"1"`.
It also renders the live camera feed full-screen on the IVI display via a
SurfaceComposerClient overlay layer.

`rvc_app` is part of a three-module RVC pipeline:

```
rvc_service  (vendor)  ── vendor.rvc.camera.active ──►  rvc_app  (system)
  VHAL gear monitor                                  Camera + encoder + RTP
                                                     + IVI display overlay
                                          rvc_evs_shim (satisfies CarEvsService watchdog)
```

**Why the split?**
`libcamera2ndk` lives in the system linker namespace and cannot be loaded
by vendor binaries. `rvc_app` installs to `/system/bin` (no `vendor: true`
in `Android.bp`) so the system linker resolves it cleanly.

---

## Getting Started

The RVC pipeline spans **four repositories** that must all be present in the
AOSP tree before building. Clone each into the path shown:

| Module | AOSP tree path | Repository |
|---|---|---|
| `rvc_service` | `vendor/brcm/rvc-service` | `<your-org>/rvc-service` |
| `rvc_app` | `vendor/brcm/rvc-app` | `<your-org>/rvc-app` |
| `rvc_evs_shim` | `vendor/brcm/rvc-evs-shim` | `<your-org>/rvc-evs-shim` |
| CarServiceRpiOverlay | `device/brcm/rpi5/overlay/CarServiceRpiOverlay` | `<your-org>/CarServiceRpiOverlay` |

```bash
# From the AOSP root — clone all four into their required paths:
git clone <your-org>/rvc-service          vendor/brcm/rvc-service
git clone <your-org>/rvc-app              vendor/brcm/rvc-app
git clone <your-org>/rvc-evs-shim         vendor/brcm/rvc-evs-shim
git clone <your-org>/CarServiceRpiOverlay device/brcm/rpi5/overlay/CarServiceRpiOverlay
```

> **CarServiceRpiOverlay** overrides `config_evsCameraActivity` in AOSP's
> CarService so that `CarEvsService` launches `rvc_evs_shim` (a no-op shim)
> instead of `CarEvsCameraPreviewActivity` on GEAR=REVERSE. This keeps the
> CarEvsService watchdog satisfied while leaving `/dev/video0` free for
> `rvc_app`'s Camera2 session.
>
> **rvc_evs_shim** is a minimal APK (single Activity that finishes immediately)
> installed to `/system/app`. It has no user-visible UI and exists solely to
> satisfy the CarEvsService state machine without holding the camera.

### Register in the device build

In `device/brcm/rpi5/device.mk`:

```makefile
PRODUCT_PACKAGES += \
    rvc_service \
    rvc_app \
    rvc_evs_shim

PRODUCT_PACKAGE_OVERLAYS += device/brcm/rpi5/overlay
```

In `device/brcm/rpi5/BoardConfig.mk`:

```makefile
BOARD_SEPOLICY_DIRS += vendor/brcm/rvc-app/sepolicy
```

### Build all RVC modules

```bash
source build/envsetup.sh
lunch <your_target>

m rvc_service rvc_app rvc_evs_shim CarServiceRpiOverlay
```

---

## Tree placement

```
<AOSP_ROOT>/vendor/brcm/rvc-app/
├── Android.bp
├── rvc_app.rc
├── sepolicy/
│   ├── rvc_app.te
│   └── file_contexts
├── src/
│   ├── main.cpp
│   ├── CameraStreamManager.cpp
│   └── RtpStreamer.cpp
└── include/
    ├── CameraConfig.h
    ├── CameraStreamManager.h
    └── RtpStreamer.h
```

`BoardConfig.mk` must include the sepolicy directory:

```makefile
BOARD_SEPOLICY_DIRS += vendor/brcm/rvc-app/sepolicy
```

---

## Build

```bash
source build/envsetup.sh
lunch <your_target>

# Module only (fast iteration)
mmm vendor/brcm/rvc-app

# Output
out/target/product/<device>/system/bin/rvc_app
```

---

## Device integration

In `device/<oem>/<board>/device.mk`:

```makefile
PRODUCT_PACKAGES += rvc_service rvc_app
```

Both are required. `rvc_app.rc` is installed to `/system/etc/init/`
automatically via the `init_rc` field in `Android.bp`.

---

## Push and test without reflashing

On RPi5, the system partition is mounted as root (`/`) — there is no
separate `/system` mount point. Remount both partitions writable first:

```bash
adb root
adb shell mount -o remount,rw /
adb shell mount -o remount,rw /vendor
```

Push all three RVC modules:

```bash
# rvc_app — native binary (system partition)
adb push out/target/product/<device>/system/bin/rvc_app /system/bin/rvc_app

# rvc_evs_shim — APK that satisfies CarEvsService watchdog (system partition)
adb push out/target/product/<device>/system/app/rvc_evs_shim /system/app/rvc_evs_shim

# CarServiceRpiOverlay — resource overlay redirecting EVS activity (vendor partition)
adb push out/target/product/<device>/vendor/overlay/CarServiceRpiOverlay.apk /vendor/overlay/CarServiceRpiOverlay.apk
```

A reboot is required after pushing `rvc_evs_shim` and `CarServiceRpiOverlay` because
PackageManager scans new APKs at boot and CarService reads the overlay at startup:

```bash
adb reboot
```

After reboot, verify all three are active:

```bash
adb shell getprop init.svc.rvc_app           # expected: running
adb shell pm list packages | grep rvcshim    # expected: package:com.brcm.rvcshim
adb shell cmd overlay list | grep Rpi        # CarServiceRpiOverlay should show [x]
```

To iterate on `rvc_app` only (no reboot needed):

```bash
adb shell stop rvc_app
adb push out/target/product/<device>/system/bin/rvc_app /system/bin/rvc_app
adb shell start rvc_app
adb logcat -s RvcApp CameraStreamMgr RtpStreamer
```

For the vendor binary (`rvc_service`), `/vendor` is a separate partition:

```bash
adb push out/target/product/<device>/vendor/bin/rvc_service /vendor/bin/rvc_service
```

---

## Simulate a reverse gear event

Gear events flow through `rvc_service`, which writes the property that
`rvc_app` watches:

```bash
# Engage reverse — rvc_service writes vendor.rvc.camera.active=1
adb shell cmd car_service inject-vhal-event 0x11400400 8

# Leave reverse — rvc_service writes vendor.rvc.camera.active=0
adb shell cmd car_service inject-vhal-event 0x11400400 4
```

To test `rvc_app` directly without `rvc_service`:

```bash
adb shell setprop vendor.rvc.camera.active 1   # start streaming
adb shell setprop vendor.rvc.camera.active 0   # stop streaming
```

---

## Receive the stream on the Instrument Cluster

### GStreamer

```bash
gst-launch-1.0 \
  udpsrc port=5004 \
  caps="application/x-rtp,media=video,encoding-name=H264,payload=96" \
  ! rtph264depay ! avdec_h264 ! videoconvert ! autovideosink
```

### FFplay

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
init  (rvc_app.rc)   class main
  └── main()
        __system_property_find("vendor.rvc.camera.active")
        __system_property_wait()  ← blocks until property changes (500ms timeout)
        │
        │  value == "1"  →  CameraStreamManager::open()
        │                      AMediaCodec (H.264 encoder, Surface input)
        │                      ACameraManager_openCamera()
        │                      ACameraCaptureSession → ANativeWindow (encoder surface)
        │                      encoderLoop() thread
        │                        AMediaCodec_dequeueOutputBuffer()
        │                          RtpStreamer::sendAnnexB()
        │                            strip Annex-B start codes → NAL units
        │                            single-NAL / FU-A (RFC 3984)
        │                            sendto() UDP ──► 192.168.10.10:5004
        │
        │  value == "0"  →  CameraStreamManager::close()
        │                      teardown: session → device → manager → codec → surface
        │                      RtpStreamer::stop()
        │
        └── loop back to __system_property_wait()
```

---

## Troubleshooting

| Symptom | Likely cause & fix |
|---|---|
| `rvc_app` never starts | Check `adb shell getprop init.svc.rvc_app`; verify `class main` is in `rvc_app.rc` |
| Camera doesn't open when reverse engaged | Check `vendor.rvc.camera.active`: `adb shell getprop vendor.rvc.camera.active`; check `rvc_service` is running |
| `ACameraManager_openCamera failed` | Wrong `CAMERA_ID` in `CameraConfig.h`, or camera already in use |
| `libcamera2ndk.so` not found at runtime | Binary has `vendor: true` in `Android.bp` — remove it so it installs to `/system/bin` |
| No UDP packets at Instrument Cluster | Verify route: `adb shell ping 192.168.10.10`; check `RTP_DEST_IP` in `CameraConfig.h` |
| Blurry / corrupted video | Increase `VIDEO_BITRATE`; check packet loss: `tcpdump -i eth0 udp port 5004` |
