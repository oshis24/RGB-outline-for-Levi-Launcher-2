#include "outline/uniform_probe.hpp"

#include <android/log.h>

#include <chrono>
#include <thread>

#define LOG_TAG "OutlineRGB"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

void initializeRuntime() {
    constexpr int maxAttempts = 100;

    for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
        LOGI("uniform_probe init attempt %d/%d", attempt, maxAttempts);

        if (outline::uniform_probe::install()) {
            LOGI("================================");
            LOGI("OutlineRGB (uniform_probe mode) initialized");
            LOGI("================================");
            return;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    LOGE("uniform_probe: failed to install after all attempts "
         "(libGLESv2.so or libminecraftpe.so not ready yet?)");
}

}

__attribute__((constructor))
static void onLoad() {
    LOGI("OutlineRGB loading (uniform_probe mode, no engine hooks)");
    std::thread(initializeRuntime).detach();
}

__attribute__((destructor))
static void onUnload() {
    outline::uniform_probe::uninstall();
    LOGI("OutlineRGB unloaded");
}
