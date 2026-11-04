#pragma once
#include <windows.h>
#include <cstdint>
// Stable metadata/control ABI. Video pixels never cross the managed UI boundary.
struct CortexStudioState {
    int32_t running{}, ready{}, hasFrame{}, virtualEnabled{}, mode{}, width{}, height{},
        outputFps{};
    double incomingFps{};
    uint64_t frames{}, packets{}, gaps{}, virtualSent{}, virtualSkipped{};
    wchar_t status[256]{}, error[512]{}, discovery[256]{}, peer[64]{};
    wchar_t diagnostics[512]{};
    int32_t phoneFps{}, phoneSyncAvailable{}, requestedPhoneFps{}, frameRatePending{},
        phoneRateFallback{};
    uint64_t frameRateRequestId{}, frameRateAckId{};
    wchar_t deviceName[128]{};
};
enum CortexAvatarControl {
    CortexAvatarEnabled = 19,
    CortexAvatarTestMotion = 20,
    CortexAvatarMode = 21,
    CortexAvatarMotion = 22,
    CortexAvatarExpression = 23,
    CortexAvatarScale = 24,
    CortexAvatarPositionX = 25,
    CortexAvatarPositionY = 26,
    CortexAvatarSmoothing = 27,
    CortexAvatarRecenter = 28,
    CortexAvatarSensitivity = 29,
    CortexAvatarArms = 30,
    CortexAvatarMirror = 31,
    CortexAvatarFollowPosition = 32,
    CortexAvatarFollowDistance = 33
};
extern "C" {
#ifdef CORTEX_STUDIO_NATIVE_BUILD
#define CORTEX_STUDIO_API __declspec(dllexport)
#else
#define CORTEX_STUDIO_API __declspec(dllimport)
#endif
CORTEX_STUDIO_API void* __cdecl cortex_studio_create(HWND preview, const wchar_t* model);
CORTEX_STUDIO_API void __cdecl cortex_studio_destroy(void* handle);
CORTEX_STUDIO_API int __cdecl cortex_studio_get_state(void*, CortexStudioState*, int bytes);
CORTEX_STUDIO_API void __cdecl cortex_studio_set(void*, int property, double value);
CORTEX_STUDIO_API void __cdecl cortex_studio_path(void*, int kind, const wchar_t* path);
#undef CORTEX_STUDIO_API
}
