#pragma once
// Native on-foot VR, flight F0 (docs\design-native-onfoot-vr-2026-10-13.md): the submit shape.
//
// One trace line a second, and only while the game submits frames (the line is written by the first Submit after each second, so a
// quiet runtime writes nothing). Per eye: the texture's size and the part of it the game handed over, and whether both eyes came
// from one texture. A stereo pair reads as two eye-sized textures; the on-foot Cinema panel reads as one 16:9 panel both eyes share,
// with bounds that cover a part of it. The size comes from the texture's description (metadata only, no GPU read). The line says
// what the game submitted; nothing here changes what is submitted.
//
//   submit shape: left=WxH bounds=(u0,v0)-(u1,v1) right=WxH bounds=(u0,v0)-(u1,v1) same_texture=yes|no
//
// "none" means that eye was not submitted in the second; "unsized" means the texture was not a Direct3D 11 Texture2D this could read;
// "unset" means the call passed no bounds (the whole texture).

#include <windows.h>
#include <d3d11.h>

#include <cstdint>
#include <cstdio>
#include <mutex>

#include "native_trace.h"

namespace edvr::openxr {
namespace submitshape {

struct EyeSeen {
    bool seen = false;
    uintptr_t handle = 0;
    bool sized = false;
    uint32_t w = 0, h = 0;
    bool bounds = false;
    float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
};

inline std::mutex& shapeMutex() {
    static std::mutex m;
    return m;
}
inline EyeSeen& shapeEye(int index) {
    static EyeSeen eyes[2];
    return eyes[index & 1];
}
inline uint64_t& shapeWindowStart() {
    static uint64_t start = 0;
    return start;
}

// The only read of the texture: its description. A fault (a pointer that is not a texture) is caught and reported as unsized.
__declspec(noinline) bool texture2DSize(void* handle, uint32_t* w, uint32_t* h) noexcept {
    __try {
        D3D11_TEXTURE2D_DESC desc{};
        static_cast<ID3D11Texture2D*>(handle)->GetDesc(&desc);
        *w = desc.Width;
        *h = desc.Height;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline void formatEyeSize(const EyeSeen& e, char* out, size_t cap) {
    if (!e.seen) std::snprintf(out, cap, "none");
    else if (!e.sized) std::snprintf(out, cap, "unsized");
    else std::snprintf(out, cap, "%ux%u", static_cast<unsigned>(e.w), static_cast<unsigned>(e.h));
}

inline void formatEyeBounds(const EyeSeen& e, char* out, size_t cap) {
    if (!e.seen) std::snprintf(out, cap, "none");
    else if (!e.bounds) std::snprintf(out, cap, "unset");
    else std::snprintf(out, cap, "(%.3f,%.3f)-(%.3f,%.3f)", static_cast<double>(e.u0), static_cast<double>(e.v0), static_cast<double>(e.u1),
                       static_cast<double>(e.v1));
}

// Called by the public Submit for each eye, before any admission. `handle` is the texture pointer as the game passed it; `directX` is
// whether its API convention is DirectX. Cheap: one description read, one lock, and at most one trace line a second.
inline void submitShapeNote(int eye, void* handle, bool directX, bool hasBounds, float u0, float v0, float u1, float v1, uint64_t nowMs) {
    uint32_t w = 0, h = 0;
    const bool sized = directX && handle && texture2DSize(handle, &w, &h);
    EyeSeen printed[2];
    bool print = false;
    {
        std::lock_guard<std::mutex> lock(shapeMutex());
        if (shapeWindowStart() == 0) shapeWindowStart() = nowMs;
        EyeSeen& e = shapeEye(eye);
        e.seen = true;
        e.handle = reinterpret_cast<uintptr_t>(handle);
        e.sized = sized;
        e.w = w;
        e.h = h;
        e.bounds = hasBounds;
        e.u0 = u0;
        e.v0 = v0;
        e.u1 = u1;
        e.v1 = v1;
        if (nowMs - shapeWindowStart() >= 1000) {
            print = true;
            printed[0] = shapeEye(0);
            printed[1] = shapeEye(1);
            shapeEye(0) = EyeSeen{};
            shapeEye(1) = EyeSeen{};
            shapeWindowStart() = nowMs;
        }
    }
    if (!print) return;
    char leftSize[32], leftBounds[96], rightSize[32], rightBounds[96];
    formatEyeSize(printed[0], leftSize, sizeof(leftSize));
    formatEyeBounds(printed[0], leftBounds, sizeof(leftBounds));
    formatEyeSize(printed[1], rightSize, sizeof(rightSize));
    formatEyeBounds(printed[1], rightBounds, sizeof(rightBounds));
    const bool same = printed[0].seen && printed[1].seen && printed[0].handle != 0 && printed[0].handle == printed[1].handle;
    nativeTracePrintf("submit shape: left=%s bounds=%s right=%s bounds=%s same_texture=%s\n", leftSize, leftBounds, rightSize, rightBounds,
                      same ? "yes" : "no");
}

}  // namespace submitshape
}  // namespace edvr::openxr
