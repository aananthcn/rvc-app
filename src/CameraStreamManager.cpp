#include "CameraStreamManager.h"
#include "CameraConfig.h"

#include <log/log.h>
#include <media/NdkMediaError.h>
#include <android/native_window.h>

#include <chrono>
#include <cstring>

namespace rearview {

// ── Constructor / Destructor ──────────────────────────────────────────────────

CameraStreamManager::CameraStreamManager() = default;

CameraStreamManager::~CameraStreamManager() {
    close();
}

// ── Public API ────────────────────────────────────────────────────────────────

bool CameraStreamManager::open() {
    if (mStreaming.load()) return true;
    ALOGI("CameraStreamMgr: opening…");

    if (!mRtpStreamer.start())  { return false; }
    if (!setupEncoder())        { mRtpStreamer.stop(); return false; }
    if (!setupCameraSession())  { teardown(); return false; }

    mStreaming.store(true);

    // Dedicated thread drains MediaCodec output and feeds RtpStreamer.
    mEncoderThread = std::thread(&CameraStreamManager::encoderLoop, this);

    ALOGI("CameraStreamMgr: running");
    return true;
}

void CameraStreamManager::close() {
    if (!mStreaming.exchange(false)) return;
    ALOGI("CameraStreamMgr: closing…");
    teardown();
    if (mEncoderThread.joinable()) mEncoderThread.join();
    ALOGI("CameraStreamMgr: closed");
}

// ── Encoder setup ─────────────────────────────────────────────────────────────

bool CameraStreamManager::setupEncoder() {
    mCodec = AMediaCodec_createEncoderByType(MIME_TYPE);
    if (!mCodec) {
        ALOGE("CameraStreamMgr: AMediaCodec_createEncoderByType(%s) failed", MIME_TYPE);
        return false;
    }

    mFormat = AMediaFormat_new();
    AMediaFormat_setString(mFormat, AMEDIAFORMAT_KEY_MIME,             MIME_TYPE);
    AMediaFormat_setInt32 (mFormat, AMEDIAFORMAT_KEY_WIDTH,            VIDEO_WIDTH);
    AMediaFormat_setInt32 (mFormat, AMEDIAFORMAT_KEY_HEIGHT,           VIDEO_HEIGHT);
    AMediaFormat_setInt32 (mFormat, AMEDIAFORMAT_KEY_BIT_RATE,         VIDEO_BITRATE);
    AMediaFormat_setInt32 (mFormat, AMEDIAFORMAT_KEY_FRAME_RATE,       CAPTURE_FPS);
    AMediaFormat_setInt32 (mFormat, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, I_FRAME_INTERVAL);
    AMediaFormat_setInt32 (mFormat, AMEDIAFORMAT_KEY_COLOR_FORMAT,
                           0x7F000789); // COLOR_FormatSurface

    media_status_t status = AMediaCodec_configure(
        mCodec, mFormat,
        /*surface=*/nullptr,
        /*crypto=*/nullptr,
        AMEDIACODEC_CONFIGURE_FLAG_ENCODE);

    if (status != AMEDIA_OK) {
        ALOGE("CameraStreamMgr: AMediaCodec_configure failed: %d", status);
        return false;
    }

    // Camera2 renders directly into this surface — no intermediate copy.
    status = AMediaCodec_createInputSurface(mCodec, &mEncoderSurface);
    if (status != AMEDIA_OK || !mEncoderSurface) {
        ALOGE("CameraStreamMgr: AMediaCodec_createInputSurface failed: %d", status);
        return false;
    }

    status = AMediaCodec_start(mCodec);
    if (status != AMEDIA_OK) {
        ALOGE("CameraStreamMgr: AMediaCodec_start failed: %d", status);
        return false;
    }

    ALOGI("CameraStreamMgr: H.264 encoder started (%dx%d @ %d fps, %d bps)",
          VIDEO_WIDTH, VIDEO_HEIGHT, CAPTURE_FPS, VIDEO_BITRATE);
    return true;
}

// ── Camera session ────────────────────────────────────────────────────────────

bool CameraStreamManager::setupCameraSession() {
    mCameraManager = ACameraManager_create();
    if (!mCameraManager) {
        ALOGE("CameraStreamMgr: ACameraManager_create failed");
        return false;
    }

    ACameraDevice_StateCallbacks deviceCbs{};
    deviceCbs.context        = this;
    deviceCbs.onDisconnected = onCameraDisconnected;
    deviceCbs.onError        = onCameraError;

    camera_status_t camStatus = ACameraManager_openCamera(
        mCameraManager, CAMERA_ID, &deviceCbs, &mCameraDevice);
    if (camStatus != ACAMERA_OK) {
        ALOGE("CameraStreamMgr: ACameraManager_openCamera(%s) failed: %d",
              CAMERA_ID, camStatus);
        return false;
    }
    ALOGI("CameraStreamMgr: camera %s opened", CAMERA_ID);

    ACameraOutputTarget_create(mEncoderSurface, &mOutputTarget);
    ACaptureSessionOutputContainer_create(&mOutputContainer);
    ACaptureSessionOutput_create(mEncoderSurface, &mSessionOutput);
    ACaptureSessionOutputContainer_add(mOutputContainer, mSessionOutput);

    ACameraDevice_createCaptureRequest(mCameraDevice, TEMPLATE_RECORD, &mCaptureRequest);
    ACaptureRequest_addTarget(mCaptureRequest, mOutputTarget);

    ACameraCaptureSession_stateCallbacks sessionCbs{};
    sessionCbs.context  = this;
    sessionCbs.onActive = onSessionActive;
    sessionCbs.onClosed = onSessionClosed;
    sessionCbs.onReady  = onSessionReady;

    camStatus = ACameraDevice_createCaptureSession(
        mCameraDevice, mOutputContainer, &sessionCbs, &mCaptureSession);
    if (camStatus != ACAMERA_OK) {
        ALOGE("CameraStreamMgr: ACameraDevice_createCaptureSession failed: %d", camStatus);
        return false;
    }

    camStatus = ACameraCaptureSession_setRepeatingRequest(
        mCaptureSession, /*callbacks=*/nullptr, 1, &mCaptureRequest, nullptr);
    if (camStatus != ACAMERA_OK) {
        ALOGE("CameraStreamMgr: ACameraCaptureSession_setRepeatingRequest failed: %d",
              camStatus);
        return false;
    }

    ALOGI("CameraStreamMgr: capture session started → encoder surface");
    return true;
}

// ── Encoder drain loop ────────────────────────────────────────────────────────

void CameraStreamManager::encoderLoop() {
    ALOGI("CameraStreamMgr: encoder thread started");

    while (mStreaming.load()) {
        AMediaCodecBufferInfo info;
        const ssize_t idx = AMediaCodec_dequeueOutputBuffer(mCodec, &info, 10'000 /*µs*/);

        if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            continue;
        }
        if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat* fmt = AMediaCodec_getOutputFormat(mCodec);
            ALOGI("CameraStreamMgr: output format changed: %s", AMediaFormat_toString(fmt));
            AMediaFormat_delete(fmt);
            continue;
        }
        if (idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            continue;
        }
        if (idx < 0) {
            ALOGW("CameraStreamMgr: dequeueOutputBuffer returned %zd", idx);
            continue;
        }

        size_t   bufSize = 0;
        uint8_t* buf     = AMediaCodec_getOutputBuffer(
            mCodec, static_cast<size_t>(idx), &bufSize);

        if (buf && info.size > 0) {
            mRtpStreamer.sendAnnexB(buf + info.offset,
                                   static_cast<size_t>(info.size),
                                   info.presentationTimeUs);
        }

        AMediaCodec_releaseOutputBuffer(mCodec, static_cast<size_t>(idx), false);
    }

    ALOGI("CameraStreamMgr: encoder thread exiting");
}

// ── Cleanup ───────────────────────────────────────────────────────────────────

void CameraStreamManager::teardown() {
    if (mCaptureSession) {
        ACameraCaptureSession_stopRepeating(mCaptureSession);
        ACameraCaptureSession_close(mCaptureSession);
        mCaptureSession = nullptr;
    }
    if (mCaptureRequest) {
        ACaptureRequest_free(mCaptureRequest);
        mCaptureRequest = nullptr;
    }
    if (mOutputTarget) {
        ACameraOutputTarget_free(mOutputTarget);
        mOutputTarget = nullptr;
    }
    if (mOutputContainer) {
        ACaptureSessionOutputContainer_free(mOutputContainer);
        mOutputContainer = nullptr;
    }
    if (mSessionOutput) {
        ACaptureSessionOutput_free(mSessionOutput);
        mSessionOutput = nullptr;
    }
    if (mCameraDevice) {
        ACameraDevice_close(mCameraDevice);
        mCameraDevice = nullptr;
    }
    if (mCameraManager) {
        ACameraManager_delete(mCameraManager);
        mCameraManager = nullptr;
    }
    if (mCodec) {
        AMediaCodec_signalEndOfInputStream(mCodec);
        AMediaCodec_stop(mCodec);
        AMediaCodec_delete(mCodec);
        mCodec = nullptr;
    }
    if (mFormat) {
        AMediaFormat_delete(mFormat);
        mFormat = nullptr;
    }
    if (mEncoderSurface) {
        ANativeWindow_release(mEncoderSurface);
        mEncoderSurface = nullptr;
    }

    mRtpStreamer.stop();
}

// ── Static camera callbacks ───────────────────────────────────────────────────

void CameraStreamManager::onCameraDisconnected(void* ctx, ACameraDevice* /*device*/) {
    ALOGW("CameraStreamMgr: camera disconnected");
    static_cast<CameraStreamManager*>(ctx)->mStreaming.store(false);
}

void CameraStreamManager::onCameraError(void* ctx, ACameraDevice* /*device*/, int error) {
    ALOGE("CameraStreamMgr: camera error: %d", error);
    static_cast<CameraStreamManager*>(ctx)->mStreaming.store(false);
}

void CameraStreamManager::onSessionActive(void* /*ctx*/,
                                          ACameraCaptureSession* /*session*/) {
    ALOGI("CameraStreamMgr: capture session active");
}

void CameraStreamManager::onSessionClosed(void* /*ctx*/,
                                          ACameraCaptureSession* /*session*/) {
    ALOGI("CameraStreamMgr: capture session closed");
}

void CameraStreamManager::onSessionReady(void* /*ctx*/,
                                         ACameraCaptureSession* /*session*/) {
    ALOGI("CameraStreamMgr: capture session ready");
}

} // namespace rearview
