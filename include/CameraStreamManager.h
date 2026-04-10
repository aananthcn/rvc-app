#pragma once

#include "RtpStreamer.h"

#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraCaptureSession.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <android/native_window.h>

#include <atomic>
#include <thread>

namespace rearview {

// Manages the Camera2 NDK capture session and H.264 MediaCodec encoder.
// Uses buffer-input mode: AImageReader captures YUV frames from the camera,
// which are converted to NV12 and fed directly into the encoder input buffers.
// Call open() when reverse is engaged, close() when leaving reverse.
class CameraStreamManager {
public:
    CameraStreamManager();
    ~CameraStreamManager();

    // Open camera, start encoder, and begin streaming to RtpStreamer.
    bool open();

    // Stop streaming, release encoder and camera resources.
    void close();

private:
    bool setupEncoder();
    bool setupImageReader();
    bool setupCameraSession();
    void encoderLoop();
    void feedEncoder(AImage* image);
    void teardown();

    // Static camera device callbacks
    static void onCameraDisconnected(void* ctx, ACameraDevice* device);
    static void onCameraError(void* ctx, ACameraDevice* device, int error);

    // Static capture session state callbacks
    static void onSessionActive(void* ctx, ACameraCaptureSession* session);
    static void onSessionClosed(void* ctx, ACameraCaptureSession* session);
    static void onSessionReady(void* ctx, ACameraCaptureSession* session);

    // Static capture result callbacks
    static void onCaptureCompleted(void* ctx, ACameraCaptureSession* session,
                                   ACaptureRequest* request, const ACameraMetadata* result);
    static void onCaptureFailed(void* ctx, ACameraCaptureSession* session,
                                ACaptureRequest* request, ACameraCaptureFailure* failure);

    // AImageReader callback — called on camera HAL thread when a frame arrives
    static void onImageAvailable(void* ctx, AImageReader* reader);

    std::atomic<bool> mStreaming{false};
    std::thread       mEncoderThread;
    RtpStreamer       mRtpStreamer;

    // Encoder (buffer-input mode — no input surface)
    AMediaCodec*  mCodec{nullptr};
    AMediaFormat* mFormat{nullptr};

    // Image reader — camera writes YUV_420_888 frames here
    AImageReader*  mImageReader{nullptr};
    ANativeWindow* mReaderWindow{nullptr};

    // Camera
    ACameraManager*                 mCameraManager{nullptr};
    ACameraDevice*                  mCameraDevice{nullptr};
    ACameraCaptureSession*          mCaptureSession{nullptr};
    ACaptureRequest*                mCaptureRequest{nullptr};
    ACameraOutputTarget*            mOutputTarget{nullptr};
    ACaptureSessionOutputContainer* mOutputContainer{nullptr};
    ACaptureSessionOutput*          mSessionOutput{nullptr};
};

} // namespace rearview
