// src/uniform_probe.cpp
#include "outline/uniform_probe.hpp"

#include <android/log.h>
#include <dlfcn.h>
#include <link.h>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <array>

#include <pl/memory/Hook.hpp>

#define LOG_TAG "OutlineRGB"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace outline::uniform_probe {

namespace {

// RVA dari RE_UnViableTweaks_V2_3_Final.docx, section "glUniform4fv Callers"
constexpr std::uintptr_t kKnownCallerRVA[] = {
    0x108C8E40, 0x108C8EC4, 0x108C936C, 0x108C9D44,
    0x108C9DC8, 0x108CA268, 0x108CBDD0,
};

constexpr std::size_t kCallerCount = 7;
constexpr std::uint64_t kLogThrottleMs = 500; // max 2 log/detik per caller

std::uintptr_t gMcBase = 0;
void* gOriginal = nullptr;

// State per-caller buat correlation manual: kamu toggle lewat command
// atau cukup baca logcat sambil gerakin pandangan ke block.
bool gSelectionVisibleHint = false; // isi manual lewat command in-game kalau ada

// Timestamp (ms) terakhir kali tiap caller berhasil log.
std::array<std::uint64_t, kCallerCount> gLastLogMs{};

std::uint64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()
    ).count();
}

std::uintptr_t findLibraryBase(const char* soName) {
    std::uintptr_t base = 0;
    struct Ctx { const char* name; std::uintptr_t* out; } ctx{soName, &base};

    dl_iterate_phdr([](dl_phdr_info* info, size_t, void* data) -> int {
        auto* c = static_cast<Ctx*>(data);
        if (info->dlpi_name && std::strstr(info->dlpi_name, c->name)) {
            *c->out = static_cast<std::uintptr_t>(info->dlpi_addr);
            return 1;
        }
        return 0;
    }, &ctx);

    return base;
}

// Log semua library ter-load yang relevan sama rendering backend,
// supaya bisa dikonfirmasi apakah game ini pakai Vulkan atau GLES
// murni (atau translation layer seperti ANGLE/SwiftShader/gxcore).
void logLoadedRenderLibraries() {
    static const char* kInteresting[] = {
        "vulkan",
        "GLES",
        "angle",
        "swiftshader",
        "gxcore",
    };

    dl_iterate_phdr([](dl_phdr_info* info, size_t, void*) -> int {
        if (!info->dlpi_name || info->dlpi_name[0] == '\0') {
            return 0;
        }

        for (const char* keyword : kInteresting) {
            if (std::strstr(info->dlpi_name, keyword)) {
                LOGI(
                    "[uniform_probe] loaded render-related lib: %s (base=%p)",
                    info->dlpi_name,
                    reinterpret_cast<void*>(info->dlpi_addr)
                );
                break;
            }
        }

        return 0; // lanjut ke library berikutnya, jangan berhenti
    }, nullptr);
}

int matchCaller(std::uintptr_t returnAddr) {
    if (!gMcBase || returnAddr < gMcBase) return -1;
    const std::uintptr_t rva = returnAddr - gMcBase;

    for (int i = 0; i < static_cast<int>(kCallerCount); ++i) {
        // toleransi kecil karena LR biasa nunjuk instruksi SETELAH BL,
        // sedangkan RVA di tabel adalah alamat instruksi BL itu sendiri.
        if (rva >= kKnownCallerRVA[i] && rva <= kKnownCallerRVA[i] + 8) {
            return i;
        }
    }
    return -1;
}

void glUniform4fvDetour(int32_t location, int32_t count, const float* value) {
    const std::uintptr_t returnAddr =
        reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));

    const int idx = matchCaller(returnAddr);

    if (idx >= 0) {
        const std::uint64_t t = nowMs();

        if (t - gLastLogMs[idx] >= kLogThrottleMs) {
            gLastLogMs[idx] = t;

            LOGI(
                "[uniform_probe] caller#%d rva=0x%llx loc=%d count=%d "
                "v=(%.3f %.3f %.3f %.3f) selectionHint=%d",
                idx,
                static_cast<unsigned long long>(returnAddr - gMcBase),
                location, count,
                value[0], value[1], value[2], value[3],
                gSelectionVisibleHint
            );
        }
    }

    using Fn = void (*)(int32_t, int32_t, const float*);
    reinterpret_cast<Fn>(gOriginal)(location, count, value);
}

}

bool install() {
    // Diagnostik: cek dulu library render apa saja yang dipakai
    // proses ini, sebelum coba hook GLES.
    logLoadedRenderLibraries();

    gMcBase = findLibraryBase("libminecraftpe.so");
    if (!gMcBase) {
        LOGE("[uniform_probe] libminecraftpe.so base not found");
        return false;
    }

    void* glesHandle = dlopen("libGLESv2.so", RTLD_NOW | RTLD_NOLOAD);
    if (!glesHandle) {
        LOGE("[uniform_probe] libGLESv2.so not loaded yet");
        return false;
    }

    void* target = dlsym(glesHandle, "glUniform4fv");
    if (!target) {
        LOGE("[uniform_probe] glUniform4fv symbol not found");
        return false;
    }

    const int rc = pl::memory::hook(
        target,
        reinterpret_cast<void*>(glUniform4fvDetour),
        &gOriginal
    );

    if (rc != 0) {
        LOGE("[uniform_probe] hook failed rc=%d", rc);
        return false;
    }

    LOGI("[uniform_probe] installed, mcBase=%p", reinterpret_cast<void*>(gMcBase));
    return true;
}

void uninstall() {
    if (gOriginal) {
        void* glesHandle = dlopen("libGLESv2.so", RTLD_NOW | RTLD_NOLOAD);
        if (glesHandle) {
            void* target = dlsym(glesHandle, "glUniform4fv");
            if (target) {
                pl::memory::unhook(target, reinterpret_cast<void*>(glUniform4fvDetour));
            }
        }
        gOriginal = nullptr;
    }
}

}
