#include <android/binder_process.h>
#include <log/log.h>
#include <sys/system_properties.h>
#include <signal.h>
#include <cerrno>
#include <cstring>
#include <atomic>
#include <thread>
#include <chrono>

#include "CameraStreamManager.h"

// ── Graceful shutdown ─────────────────────────────────────────────────────────

static std::atomic<bool> gRunning{true};

static void signalHandler(int /*sig*/) {
    gRunning.store(false);
}

// ── Property name ─────────────────────────────────────────────────────────────

static constexpr const char* RVC_PROP = "vendor.rvc.camera.active";

// ── main ──────────────────────────────────────────────────────────────────────

int main() {
    signal(SIGTERM, signalHandler);
    signal(SIGINT,  signalHandler);

    // Camera2 NDK delivers onCaptureCompleted / onImageAvailable via Binder
    // callbacks into this process. Without a thread pool, CameraService has
    // no thread to call into — all callbacks are silently dropped.
    ABinderProcess_setThreadPoolMaxThreadCount(2);
    ABinderProcess_startThreadPool();

    ALOGI("rvc_app: starting — watching property %s", RVC_PROP);

    rearview::CameraStreamManager camera;

    const prop_info* pi    = nullptr;
    uint32_t         serial = 0;

    while (gRunning.load()) {
        // Property is created when rvc_service first writes it.
        // Keep retrying until it appears.
        if (!pi) {
            pi = __system_property_find(RVC_PROP);
            if (!pi) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            ALOGI("rvc_app: property %s found — listening for changes", RVC_PROP);
        }

        // Block until the property changes or 500 ms pass (so we can check
        // gRunning without indefinitely blocking on a stopped rvc_service).
        const struct timespec timeout = {0, 500'000'000}; // 500 ms relative
        uint32_t newSerial = serial;
        __system_property_wait(pi, serial, &newSerial, &timeout);

        if (newSerial == serial) {
            continue; // timeout — no change, loop back
        }
        serial = newSerial;

        char value[PROP_VALUE_MAX] = {};
        __system_property_read(pi, nullptr, value);
        ALOGI("rvc_app: %s = '%s'", RVC_PROP, value);

        if (strcmp(value, "1") == 0) {
            ALOGI("rvc_app: REVERSE — opening camera");
            camera.open();
        } else {
            ALOGI("rvc_app: NOT REVERSE — closing camera");
            camera.close();
        }
    }

    ALOGI("rvc_app: shutting down");
    camera.close();
    return 0;
}
