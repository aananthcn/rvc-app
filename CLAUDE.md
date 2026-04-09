# CLAUDE.md — rvc_app

This file is read automatically by the `claude` CLI (Claude Code) whenever you
run it inside this directory. It tells Claude everything it needs to know about
this module so you can make changes with short, natural instructions.

---

## Project overview

`rvc_app` is a **native C++ system service** for Android Automotive OS (AAOS).
It is one half of a two-process rear-view camera pipeline:

| Process | Binary | Role |
|---|---|---|
| `rvc_service` | `/vendor/bin/rvc_service` | Monitors VHAL `GEAR_SELECTION` via HIDL; writes `vendor.rvc.camera.active=1/0` |
| `rvc_app` | `/system/bin/rvc_app` | Watches the property; controls camera, encoder, and RTP streamer |

The two processes communicate via a **vendor system property**:

```
rvc_service  (vendor)  ──  vendor.rvc.camera.active = "1"/"0"  ──►  rvc_app  (system)
  VHAL gear monitor                                                   Camera + encoder + RTP
```

**Why a property instead of a socket?**
Android 16 neverallows prohibit vendor domains from `connectto` coredomain
Unix stream sockets.  System properties are the canonical, policy-approved
channel for vendor-to-system signalling.  They are also sub-millisecond
latency, which meets the automotive requirement.

**Why is rvc_app a system binary (no `vendor: true`)?**
`libcamera2ndk` lives in the system linker namespace and cannot be loaded by
vendor binaries.  Installing `rvc_app` to `/system/bin` keeps it in the system
namespace where the library resolves cleanly.

---

## AOSP tree placement

```
<AOSP_ROOT>/vendor/brcm/rvc-app/
```

- Binary installs to `/system/bin/rvc_app`
- RC file installs to `/system/etc/init/rvc_app.rc`
- Registered in the device build via `PRODUCT_PACKAGES += rvc_app` in `device.mk`

---

## File map

```
rvc-app/
├── CLAUDE.md                     ← you are here
├── Android.bp                    ← Soong build rules; cc_binary "rvc_app", no vendor: true
├── rvc_app.rc                    ← Android Init Language: user/group, class main
├── src/
│   ├── main.cpp                  ← property watcher loop; dispatches open/close to CameraStreamManager
│   ├── CameraStreamManager.cpp   ← Camera2 NDK + AMediaCodec H.264 encoding pipeline
│   └── RtpStreamer.cpp           ← Annex-B → NAL → RTP/UDP packetiser (RFC 3984)
└── include/
    ├── CameraConfig.h            ← ALL tuneable constants (IP, port, resolution, …)
    ├── CameraStreamManager.h
    └── RtpStreamer.h
```

### Single source of truth for configuration

**`include/CameraConfig.h`** — edit this file for any hardware or network
change. Nothing else needs to be touched.

| Constant | Default | Meaning |
|---|---|---|
| `CAMERA_ID` | `"0"` | Camera HAL device ID |
| `VIDEO_WIDTH` / `VIDEO_HEIGHT` | `1280` / `720` | Capture resolution |
| `CAPTURE_FPS` | `30` | Target frame rate |
| `MIME_TYPE` | `"video/avc"` | H.264 codec MIME |
| `VIDEO_BITRATE` | `2'000'000` | Encoder bitrate (bps) |
| `I_FRAME_INTERVAL` | `1` | IDR keyframe interval (seconds) |
| `RTP_DEST_IP` | `"192.168.10.10"` | Instrument Cluster IP |
| `RTP_DEST_PORT` | `5004` | Destination UDP port |
| `RTP_MTU` | `1400` | Max RTP packet payload (bytes) |
| `RTP_PAYLOAD_TYPE` | `96` | RTP dynamic payload type for H.264 |

The VHAL property constants and the property name (`RVC_PROP_CAMERA_ACTIVE`)
live in **`rvc-service/include/GearConfig.h`** — do not duplicate them here.
The property name is also hardcoded as `RVC_PROP` in `src/main.cpp`; keep the
two in sync if you rename it.

---

## C++ namespace

All classes live in the `rearview` namespace. Do not change the namespace
name — it is internal and not exposed to any other module.

---

## Architecture and data flow

```
rvc_service (vendor)
  │  android::base::SetProperty("vendor.rvc.camera.active", "1"/"0")
  │
  ▼  [system property propagated by property_service]
  │
rvc_app (system)                            [src/main.cpp]
  │  __system_property_find("vendor.rvc.camera.active")
  │  __system_property_wait()  ← blocks up to 500 ms, then loops
  │
  │  on "1":
  │    CameraStreamManager::open()           [src/CameraStreamManager.cpp]
  │      RtpStreamer::start()                open UDP → 192.168.10.10:5004
  │      setupEncoder()
  │        AMediaCodec_createEncoderByType("video/avc")
  │        AMediaCodec_createInputSurface()  → mEncoderSurface
  │        AMediaCodec_start()
  │      setupCameraSession()
  │        ACameraManager_openCamera(CAMERA_ID)
  │        ACameraDevice_createCaptureSession()
  │        ANativeWindow ← mEncoderSurface   (no buffer copy)
  │        ACameraCaptureSession_setRepeatingRequest()
  │      encoderLoop() thread
  │        AMediaCodec_dequeueOutputBuffer()
  │          RtpStreamer::sendAnnexB()        [src/RtpStreamer.cpp]
  │            strip start codes → NAL units
  │            single-NAL  if NAL ≤ MTU
  │            FU-A frags  if NAL >  MTU    (RFC 3984 §5.8)
  │            sendto() UDP ─────────────► 192.168.10.10:5004
  │
  │  on "0":
  │    CameraStreamManager::close()
  │      teardown(): session → device → manager → codec → surface
  │      RtpStreamer::stop()
  │
  └── __system_property_wait() again …
```

### Key design decisions to preserve

- **Surface-input encoding**: Camera NDK renders directly into the
  `ANativeWindow` that `AMediaCodec` provides. There is no intermediate
  buffer copy. Do not change this to buffer-input mode.
- **Encoder drain thread**: `encoderLoop()` runs on a dedicated `std::thread`
  so it never blocks the property-watching loop or camera callbacks.
- **Property-based IPC**: Do not replace this with Unix sockets. Android 16
  neverallows prohibit vendor domains from connecting to coredomain sockets.
  System properties are the approved cross-partition signalling mechanism.
- **500 ms poll timeout**: `__system_property_wait` is called with a 500 ms
  relative timeout so `gRunning` is checked regularly and SIGTERM is handled
  promptly. Do not remove the timeout.
- **`class main` in rc**: `rvc_app` starts before `rvc_service` so it is
  already watching when the property is first written. Changing to `class hal`
  or `class late_start` risks a missed first update.

---

## Build instructions

```bash
# From the AOSP root (run once per shell):
source build/envsetup.sh
lunch <your_target>

# Build only this module:
mmm vendor/brcm/rvc-app

# Or by module name:
m rvc_app

# Output binary:
# out/target/product/<device>/system/bin/rvc_app
```

## Push and test without reflashing

```bash
adb root && adb remount
adb push out/target/product/<device>/system/bin/rvc_app /system/bin/rvc_app
adb shell stop rvc_app
adb shell start rvc_app
adb logcat -s RvcApp CameraStreamMgr RtpStreamer
```

## Direct property test (without rvc_service)

```bash
# Trigger camera open
adb shell setprop vendor.rvc.camera.active 1

# Trigger camera close
adb shell setprop vendor.rvc.camera.active 0
```

---

## Coding conventions

- **C++17**. Use `std::atomic`, `std::thread`, lambdas freely.
- **Error handling**: all NDK API failures must be logged with `ALOGE` and
  return `false` / return early. Never silently ignore a failure.
- **Logging**: the `LOG_TAG` macro is set to `"RvcApp"` in `Android.bp`.
  Add per-class prefixes in messages (`ALOGI("CameraStreamMgr: …")`) so
  logcat filters work independently.
- **Resource ownership**: every NDK object opened in `setupCameraSession()` /
  `setupEncoder()` must be released in `teardown()`. Follow the existing
  null-check-before-free pattern.
- **No exceptions**: AOSP native code does not use C++ exceptions. Use `bool`
  return codes and early returns.
- **No heap allocation in the hot path**: the encoder drain loop and
  `RtpStreamer::sendAnnexB` must not allocate per-frame. The `std::vector`
  inside `sendSingleNal` / `sendFuA` is acceptable for now but should be
  replaced with a pre-allocated ring buffer if latency becomes a concern.
- **Headers in `include/`**: all `.h` files live under `include/` and are
  picked up automatically via `local_include_dirs` in `Android.bp`.
- **No `vendor: true`**: do not add `vendor: true` to `Android.bp`. It would
  move the binary to `/vendor/bin` and break `libcamera2ndk` loading.

---

## Dependencies (declared in Android.bp)

| Library | Used for |
|---|---|
| `liblog` | `ALOGI`, `ALOGE`, `ALOGW`, `ALOGD` |
| `libutils` / `libcutils` | Android utility types |
| `libbase` | AOSP base utilities |
| `libcamera2ndk` | `ACameraManager`, `ACameraDevice`, `ACameraCaptureSession` |
| `libmediandk` | `AMediaCodec`, `AMediaFormat` |
| `libnativewindow` | `ANativeWindow_release` |
| `libc` | `__system_property_wait`, UDP socket |

---

## What NOT to do

- Do not add `vendor: true` to `Android.bp` — it breaks `libcamera2ndk` loading.
- Do not replace property-based IPC with Unix domain sockets — Android 16
  neverallows block vendor→coredomain socket connections.
- Do not add Java/Kotlin files. This is a pure native service.
- Do not add VHAL/HIDL dependencies here — gear monitoring belongs entirely
  in `rvc_service`.
- Do not use `std::cout` or `printf` for logging — always use `ALOGI` etc.

---

## Common tasks (examples for Claude)

> "Change the destination IP to 10.0.0.5"
→ Edit `RTP_DEST_IP` in `include/CameraConfig.h`.

> "Increase bitrate to 4 Mbps"
→ Edit `VIDEO_BITRATE` in `include/CameraConfig.h`.

> "Change resolution to 1920×1080"
→ Edit `VIDEO_WIDTH` and `VIDEO_HEIGHT` in `include/CameraConfig.h`.

> "Add support for a second camera (e.g. side view)"
→ Add a second `CameraStreamManager` instance in `main.cpp`, a new
  `CAMERA_ID_2` constant in `CameraConfig.h`, and a second property
  `vendor.rvc.side.active` watched in the same property loop.

> "Add a watchdog that restarts streaming if no frames arrive for 2 seconds"
→ Add a `std::chrono::steady_clock` timestamp updated in `encoderLoop()` and
  call `close()` + `open()` from a separate watchdog thread if it expires.

> "Stream to a second destination simultaneously"
→ Instantiate a second `RtpStreamer` in `CameraStreamManager`, configure it
  with a different IP/port, and call `sendAnnexB` on both in `encoderLoop()`.
