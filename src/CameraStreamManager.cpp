#include "CameraStreamManager.h"
#include "CameraConfig.h"

#include <log/log.h>
#include <media/NdkMediaError.h>

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

    if (!mRtpStreamer.start())   { return false; }
    if (!setupEncoder())         { mRtpStreamer.stop(); return false; }
    if (!setupImageReader())     { teardown(); return false; }

    // Set mStreaming true BEFORE the camera session so onImageAvailable
    // processes frames immediately when setRepeatingRequest fires.
    mStreaming.store(true);

    if (!setupCameraSession())   { mStreaming.store(false); teardown(); return false; }

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

// ── Encoder setup (buffer-input mode) ────────────────────────────────────────

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
    // Buffer-input mode: NV12 (YUV420SemiPlanar) — most hardware encoders prefer this.
    AMediaFormat_setInt32 (mFormat, AMEDIAFORMAT_KEY_COLOR_FORMAT,     21);

    media_status_t status = AMediaCodec_configure(
        mCodec, mFormat,
        /*surface=*/nullptr,
        /*crypto=*/nullptr,
        AMEDIACODEC_CONFIGURE_FLAG_ENCODE);

    if (status != AMEDIA_OK) {
        ALOGE("CameraStreamMgr: AMediaCodec_configure failed: %d", status);
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

// ── Image reader setup ────────────────────────────────────────────────────────

bool CameraStreamManager::setupImageReader() {
    // maxImages=4: allow camera to buffer a few frames while the encoder is busy.
    media_status_t status = AImageReader_new(
        VIDEO_WIDTH, VIDEO_HEIGHT,
        AIMAGE_FORMAT_YUV_420_888,
        /*maxImages=*/4,
        &mImageReader);

    if (status != AMEDIA_OK || !mImageReader) {
        ALOGE("CameraStreamMgr: AImageReader_new(%dx%d) failed: %d",
              VIDEO_WIDTH, VIDEO_HEIGHT, status);
        return false;
    }

    AImageReader_ImageListener listener{};
    listener.context          = this;
    listener.onImageAvailable = CameraStreamManager::onImageAvailable;
    AImageReader_setImageListener(mImageReader, &listener);

    if (AImageReader_getWindow(mImageReader, &mReaderWindow) != AMEDIA_OK || !mReaderWindow) {
        ALOGE("CameraStreamMgr: AImageReader_getWindow failed");
        return false;
    }

    ALOGI("CameraStreamMgr: image reader ready (%dx%d YUV_420_888)",
          VIDEO_WIDTH, VIDEO_HEIGHT);
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

    // Use the AImageReader's native window as the camera output target.
    ACameraOutputTarget_create(mReaderWindow, &mOutputTarget);
    ACaptureSessionOutputContainer_create(&mOutputContainer);
    ACaptureSessionOutput_create(mReaderWindow, &mSessionOutput);
    ACaptureSessionOutputContainer_add(mOutputContainer, mSessionOutput);

    ACameraDevice_createCaptureRequest(mCameraDevice, TEMPLATE_PREVIEW, &mCaptureRequest);
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

    ACameraCaptureSession_captureCallbacks captureCbs{};
    captureCbs.context           = this;
    captureCbs.onCaptureCompleted = onCaptureCompleted;
    captureCbs.onCaptureFailed    = onCaptureFailed;

    camStatus = ACameraCaptureSession_setRepeatingRequest(
        mCaptureSession, &captureCbs, 1, &mCaptureRequest, nullptr);
    if (camStatus != ACAMERA_OK) {
        ALOGE("CameraStreamMgr: ACameraCaptureSession_setRepeatingRequest failed: %d",
              camStatus);
        return false;
    }

    ALOGI("CameraStreamMgr: capture session started → image reader");
    return true;
}

// ── Image available callback (camera HAL thread) ──────────────────────────────

void CameraStreamManager::onImageAvailable(void* ctx, AImageReader* reader) {
    auto* mgr = static_cast<CameraStreamManager*>(ctx);
    if (!mgr->mStreaming.load()) {
        ALOGW("CameraStreamMgr: onImageAvailable — dropped (mStreaming=false)");
        return;
    }

    AImage* image = nullptr;
    // acquireLatestImage drops stale frames — keeps latency minimal.
    if (AImageReader_acquireLatestImage(reader, &image) != AMEDIA_OK || !image) {
        ALOGW("CameraStreamMgr: acquireLatestImage failed");
        return;
    }

    mgr->feedEncoder(image);
    AImage_delete(image);
}

// ── Feed one YUV_420_888 frame into the encoder ───────────────────────────────

void CameraStreamManager::feedEncoder(AImage* image) {
    // Wait up to 33ms for an encoder input buffer (one frame period at 30fps).
    ssize_t inputIdx = AMediaCodec_dequeueInputBuffer(mCodec, 33'000 /*µs*/);
    if (inputIdx < 0) {
        ALOGW("CameraStreamMgr: no encoder input buffer (idx=%zd) — dropping frame", inputIdx);
        return;
    }
    size_t   bufCapacity = 0;
    uint8_t* encBuf = AMediaCodec_getInputBuffer(
        mCodec, static_cast<size_t>(inputIdx), &bufCapacity);
    if (!encBuf) {
        AMediaCodec_queueInputBuffer(mCodec, static_cast<size_t>(inputIdx), 0, 0, 0, 0);
        return;
    }

    int32_t width = 0, height = 0;
    AImage_getWidth(image, &width);
    AImage_getHeight(image, &height);

    const size_t needed = static_cast<size_t>(width * height * 3 / 2);
    if (bufCapacity < needed) {
        ALOGE("CameraStreamMgr: encoder buffer too small (%zu < %zu)", bufCapacity, needed);
        AMediaCodec_queueInputBuffer(mCodec, static_cast<size_t>(inputIdx), 0, 0, 0, 0);
        return;
    }

    // ── Y plane ──────────────────────────────────────────────────────────────
    uint8_t* yData = nullptr; int yLen = 0; int yStride = 0;
    AImage_getPlaneData     (image, 0, &yData, &yLen);
    AImage_getPlaneRowStride(image, 0, &yStride);

    for (int row = 0; row < height; ++row) {
        std::memcpy(encBuf + row * width, yData + row * yStride, static_cast<size_t>(width));
    }

    // ── UV plane → NV12 (interleaved U then V) ────────────────────────────────
    uint8_t* uData = nullptr; int uLen = 0; int uStride = 0; int uPixStride = 0;
    AImage_getPlaneData      (image, 1, &uData, &uLen);
    AImage_getPlaneRowStride (image, 1, &uStride);
    AImage_getPlanePixelStride(image, 1, &uPixStride);

    uint8_t* vData = nullptr; int vLen = 0; int vStride = 0; int vPixStride = 0;
    AImage_getPlaneData      (image, 2, &vData, &vLen);
    AImage_getPlaneRowStride (image, 2, &vStride);
    AImage_getPlanePixelStride(image, 2, &vPixStride);

    uint8_t* uvDst = encBuf + width * height;
    for (int row = 0; row < height / 2; ++row) {
        for (int col = 0; col < width / 2; ++col) {
            uvDst[row * width + col * 2 + 0] =
                uData[row * uStride + col * uPixStride]; // U (Cb)
            uvDst[row * width + col * 2 + 1] =
                vData[row * vStride + col * vPixStride]; // V (Cr)
        }
    }

    int64_t timestamp = 0;
    AImage_getTimestamp(image, &timestamp);

    AMediaCodec_queueInputBuffer(mCodec, static_cast<size_t>(inputIdx),
                                 /*offset=*/0, needed, timestamp, 0);
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
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) {
                // SPS+PPS parameter sets — cache for injection before IDR frames.
                mRtpStreamer.cacheParameterSets(buf + info.offset,
                                               static_cast<size_t>(info.size));
            } else {
                mRtpStreamer.sendAnnexB(buf + info.offset,
                                       static_cast<size_t>(info.size),
                                       info.presentationTimeUs);
            }
        }

        AMediaCodec_releaseOutputBuffer(mCodec, static_cast<size_t>(idx), false);
    }

    ALOGI("CameraStreamMgr: encoder thread exiting");
}

// ── Cleanup ───────────────────────────────────────────────────────────────────

void CameraStreamManager::teardown() {
    // Stop camera capture before releasing any surfaces.
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

    // Release image reader after camera is stopped.
    if (mImageReader) {
        AImageReader_delete(mImageReader);
        mImageReader  = nullptr;
        mReaderWindow = nullptr; // owned by mImageReader — already freed
    }

    // Release encoder.
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

void CameraStreamManager::onCaptureCompleted(void* /*ctx*/,
                                             ACameraCaptureSession* /*session*/,
                                             ACaptureRequest* /*request*/,
                                             const ACameraMetadata* /*result*/) {
}

void CameraStreamManager::onCaptureFailed(void* /*ctx*/,
                                          ACameraCaptureSession* /*session*/,
                                          ACaptureRequest* /*request*/,
                                          ACameraCaptureFailure* failure) {
    ALOGE("CameraStreamMgr: capture FAILED — reason=%d sequenceId=%d dropped=%d",
          failure ? failure->reason : -1,
          failure ? failure->sequenceId : -1,
          failure ? (int)failure->wasImageCaptured : -1);
}

} // namespace rearview
