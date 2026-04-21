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

#include <gui/SurfaceComposerClient.h>
#include <gui/Surface.h>
#include <ui/DisplayState.h>

#include <atomic>
#include <thread>

namespace rearview {

// Manages the Camera2 NDK capture session and H.264 MediaCodec encoder.
// Dual-output: AImageReader feeds the H.264 encoder → RTP stream; a
// SurfaceComposerClient layer renders the same feed full-screen on the IVI.
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
    bool setupDisplay();       // create full-screen SurfaceControl layer
    bool setupCameraSession(); // registers ImageReader (and display if available) as outputs
    void encoderLoop();
    void feedEncoder(AImage* image);
    void teardown();
    void teardownCameraOnly();  // close camera device/session — leaves encoder/image-reader intact
    void teardownDisplayOnly(); // destroy SurfaceControl layer and display output targets

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

    // Image reader — camera writes YUV_420_888 frames here (→ encoder → RTP)
    AImageReader*  mImageReader{nullptr};
    ANativeWindow* mReaderWindow{nullptr};

    // Display layer — camera writes directly to SurfaceFlinger (→ IVI screen)
    android::sp<android::SurfaceComposerClient> mDisplayClient;
    android::sp<android::SurfaceControl>        mDisplaySurface;
    android::sp<android::Surface>               mDisplayWindow;
    ACameraOutputTarget*                        mDisplayOutputTarget{nullptr};
    ACaptureSessionOutput*                      mDisplaySessionOutput{nullptr};

    // Camera
    ACameraManager*                 mCameraManager{nullptr};
    ACameraDevice*                  mCameraDevice{nullptr};
    ACameraCaptureSession*          mCaptureSession{nullptr};
    ACaptureRequest*                mCaptureRequest{nullptr};
    ACameraOutputTarget*            mReaderOutputTarget{nullptr};
    ACaptureSessionOutputContainer* mOutputContainer{nullptr};
    ACaptureSessionOutput*          mReaderSessionOutput{nullptr};
};

} // namespace rearview
