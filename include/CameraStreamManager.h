#pragma once

#include "RtpStreamer.h"

#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraCaptureSession.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <android/native_window.h>

#include <atomic>
#include <thread>

namespace rearview {

// Manages the Camera2 NDK capture session and H.264 MediaCodec encoder.
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
    bool setupCameraSession();
    void encoderLoop();
    void teardown();

    // Static camera device callbacks
    static void onCameraDisconnected(void* ctx, ACameraDevice* device);
    static void onCameraError(void* ctx, ACameraDevice* device, int error);

    // Static capture session callbacks
    static void onSessionActive(void* ctx, ACameraCaptureSession* session);
    static void onSessionClosed(void* ctx, ACameraCaptureSession* session);
    static void onSessionReady(void* ctx, ACameraCaptureSession* session);

    std::atomic<bool> mStreaming{false};
    std::thread       mEncoderThread;
    RtpStreamer       mRtpStreamer;

    // Encoder
    AMediaCodec*   mCodec{nullptr};
    AMediaFormat*  mFormat{nullptr};
    ANativeWindow* mEncoderSurface{nullptr};

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
