#include "menu_door.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#include "../common/frame_flag.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "system_hook.h"

namespace edvr {
namespace {

typedef void* (*PFN_EdvrMenuPanel)(void*, int, const float*, const float*);

// The theater's half-IPD, the fallback when the runtime has not answered
// GetEyeToHeadTransform for this eye.
constexpr float kHalfIpd = 0.0315f;

// How many of this half's frames a visibility heartbeat may be old before
// it reads as "the d3d11 half stopped saying".
constexpr uint32_t kStaleFrames = 8;

struct State {
    PFN_EdvrMenuPanel fn = nullptr;
    bool     fnTried = false;
    bool     standDown = false;
    uint32_t visStamp = 0;
    uint32_t visSeenAt = 0;
    uint32_t frame = 0;
    uint32_t treats = 0;
    uint32_t faults = 0;
    bool     engagedNoted = false;
    bool     noAnchorNoted = false;
    float    anchor[12] = {};
    uint32_t anchorSeq = 0;
    bool     anchorValid = false;
};
State g_s;

void standDown(const char* why) {
    if (g_s.standDown) return;
    g_s.standDown = true;
    Log::get().note("menu door STANDING DOWN: %s. The panel cannot be drawn for the rest of "
                    "this session, so the menu closes itself and the keyboard stays the game's.",
                    why);
}

}  // namespace

void menuDoorXform(const float a[12], const float c[12], const float e[3], float xf[12]) {
    // D = Ra^T * Rc, row-major: D[i][j] = sum_k a[k][i] * c[k][j].
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            xf[i * 3 + j] = a[0 * 4 + i] * c[0 * 4 + j] + a[1 * 4 + i] * c[1 * 4 + j] +
                            a[2 * 4 + i] * c[2 * 4 + j];
        }
    }
    // The eye's origin in anchor space: the head's translation since the
    // anchor, rotated in, plus the eye's offset carried by D (it is a
    // head-space vector).
    const float dt[3] = {c[3] - a[3], c[7] - a[7], c[11] - a[11]};
    for (int i = 0; i < 3; ++i) {
        xf[9 + i] = a[0 * 4 + i] * dt[0] + a[1 * 4 + i] * dt[1] + a[2 * 4 + i] * dt[2] +
                    xf[i * 3 + 0] * e[0] + xf[i * 3 + 1] * e[1] + xf[i * 3 + 2] * e[2];
    }
}

bool menuDoorWanted() {
    State& s = g_s;
    if (s.standDown) return false;
    float alpha = 0.0f;
    uint32_t stamp = 0;
    const bool vis = menuVisible(&alpha, &stamp);
    // One frame per LEFT-eye call is close enough for a staleness count:
    // this is asked twice per frame, and eight of those is still well
    // under a tenth of a second.
    ++s.frame;
    if (stamp != s.visStamp) {
        s.visStamp = stamp;
        s.visSeenAt = s.frame;
    }
    if (!vis) return false;
    return s.frame - s.visSeenAt <= kStaleFrames * 2;
}

void* menuDoorTreat(vr::EVREye eye, void* handle, const vr::VRTextureBounds_t* bounds,
                    vr::VRTextureBounds_t* outBounds, const vr::HmdMatrix34_t& renderPose,
                    bool poseValid) {
    State& s = g_s;
    if (s.standDown || !handle || !outBounds || !poseValid) return nullptr;
    const int e = eye == vr::Eye_Left ? 0 : 1;

    if (!s.fn && !s.fnTried) {
        s.fnTried = true;
        HMODULE m = GetModuleHandleW(L"d3d11.dll");
        if (m) s.fn = reinterpret_cast<PFN_EdvrMenuPanel>(GetProcAddress(m, "edvrMenuPanel"));
        if (s.fn) {
            Log::get().note("menu door: the d3d11 half is linked.");
        } else {
            standDown("d3d11.dll exports no menu panel (mismatched pair?)");
        }
    }
    if (!s.fn) return nullptr;

    float current[12];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) current[r * 4 + c] = renderPose.m[r][c];
    }

    // The anchor: head-locked, built from THIS frame's pose turned by the
    // overlay's yaw and pitch (R * Ry * Rx), or the world anchor the d3d11
    // half published at summon.
    float yaw = 0.0f, pitch = 0.0f;
    float lockAnchor[12];
    const float* anchorUsed = s.anchor;
    if (menuHeadLock(&yaw, &pitch)) {
        const float ty = yaw * 0.0174532925f, tp = pitch * 0.0174532925f;
        const float cy = cosf(ty), sy = sinf(ty), cp = cosf(tp), sp = sinf(tp);
        const float m[9] = {cy, sy * sp, sy * cp, 0.0f, cp, -sp, -sy, cy * sp, cy * cp};
        for (int r = 0; r < 3; ++r) {
            const float x = current[r * 4 + 0], y = current[r * 4 + 1], z = current[r * 4 + 2];
            lockAnchor[r * 4 + 0] = x * m[0] + y * m[3] + z * m[6];
            lockAnchor[r * 4 + 1] = x * m[1] + y * m[4] + z * m[7];
            lockAnchor[r * 4 + 2] = x * m[2] + y * m[5] + z * m[8];
            lockAnchor[r * 4 + 3] = current[r * 4 + 3];
        }
        anchorUsed = lockAnchor;
    } else {
        uint32_t seq = 0;
        float anchor[12];
        if (!menuAnchor(anchor, &seq)) {
            if (!s.noAnchorNoted) {
                s.noAnchorNoted = true;
                Log::get().note("menu door: the panel is visible but no anchor has been published; "
                                "nothing is drawn until one is.");
            }
            return nullptr;
        }
        if (seq != s.anchorSeq || !s.anchorValid) {
            s.anchorSeq = seq;
            memcpy(s.anchor, anchor, sizeof(anchor));
            s.anchorValid = true;
        }
    }

    float b4[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    bool haveBounds = false;
    if (bounds) {
        haveBounds = guarded("menuDoor/bounds", [&] {
            b4[0] = bounds->uMin;
            b4[1] = bounds->vMin;
            b4[2] = bounds->uMax;
            b4[3] = bounds->vMax;
        });
        if (!haveBounds) {
            if (++s.faults > 8) standDown("reading the submitted bounds faulted repeatedly");
            return nullptr;
        }
    }

    float eyeOff[3] = {e == 0 ? -kHalfIpd : kHalfIpd, 0.0f, 0.0f};
    float e2h[12];
    if (systemHookEyeToHead(eye, e2h)) {
        eyeOff[0] = e2h[3];
        eyeOff[1] = e2h[7];
        eyeOff[2] = e2h[11];
    }
    float xf[12];
    menuDoorXform(anchorUsed, current, eyeOff, xf);

    void* out = s.fn(handle, e, haveBounds ? b4 : nullptr, xf);
    if (!out) return nullptr;   // nothing to draw yet, or refused and said so

    const bool flipU = haveBounds && b4[0] > b4[2];
    const bool flipV = haveBounds && b4[1] > b4[3];
    outBounds->uMin = flipU ? 1.0f : 0.0f;
    outBounds->uMax = flipU ? 0.0f : 1.0f;
    outBounds->vMin = flipV ? 1.0f : 0.0f;
    outBounds->vMax = flipV ? 0.0f : 1.0f;
    ++s.treats;
    if (!s.engagedNoted) {
        s.engagedNoted = true;
        Log::get().note("menu door: engaged -- the settings panel is composited onto every "
                        "outgoing eye frame while it is up, the last pass before the frame "
                        "leaves.");
    }
    return out;
}

void menuDoorShutdown() {
    if (g_s.treats) {
        Log::get().note("menu door: %u eye-submits carried the panel this session.", g_s.treats);
    }
}

}  // namespace edvr
