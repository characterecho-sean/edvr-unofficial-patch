// The VR Galaxy Map's hover point and the two relays that carry it (docs/galaxy-map-vr-2026-11-03.md, Fix 3). Pure: no device, no game memory, no log.
//
// THE POINT. The game hovers a star by projecting the pointer through the same projector it uses for every star and comparing viewport x, y. The
// projector (K) treats its input as a world position. The arrow is drawn in the UI frame, about one unit from the camera; the stars are 53 to 120 map
// units away. So the hover is replaced by a point 100000 units out along the arrow's direction, taken in the map camera's axes: at that distance the
// projection depends on direction only, and the star under the arrow is the star selected, whatever the head does.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace edvr {
namespace mappointer {

constexpr float kFarDistance = 100000.0f;
// The arrow's drawn position, measured in the UI frame: |P| = 1.03. Anything outside this band is not the arrow.
constexpr float kArrowMin = 0.1f;
constexpr float kArrowMax = 10.0f;

enum class Why : uint8_t { kOk, kBadPose, kBadArrow };

inline bool finite(float v) { return std::isfinite(v); }

// The map camera's rows (right, up, forward as the publisher reads them) and position, the arrow P in the UI frame and the game's fourth lane. out
// is the point to project instead: position + 100000 * (dir.x * rows[0] + dir.y * rows[1] + dir.z * rows[2]), dir = P / |P|, out[3] = w.
inline Why farPoint(const float rows[3][3], const float position[3], const float arrow[3], float w, float out[4]) {
    for (int i = 0; i < 3; ++i) {
        if (!finite(position[i]) || !finite(arrow[i])) return finite(arrow[i]) ? Why::kBadPose : Why::kBadArrow;
        for (int j = 0; j < 3; ++j)
            if (!finite(rows[i][j])) return Why::kBadPose;
    }
    const float length = std::sqrt(arrow[0] * arrow[0] + arrow[1] * arrow[1] + arrow[2] * arrow[2]);
    if (!(length > kArrowMin && length < kArrowMax)) return Why::kBadArrow;
    const float d[3] = {arrow[0] / length, arrow[1] / length, arrow[2] / length};
    for (int k = 0; k < 3; ++k)
        out[k] = position[k] + kFarDistance * (d[0] * rows[0][k] + d[1] * rows[1][k] + d[2] * rows[2][k]);
    out[3] = w;
    return Why::kOk;
}

// THE HELD POINTER, the System Map's click (docs/galaxy-map-vr-2026-11-03.md, "The System Map's click"). The game keeps the pointer's viewport x, y at the button's press and selects at its release only when the two are exactly equal, bit for bit. With a mouse at rest that holds in flat. In VR the arrow's
// position is smoothed through the head and differs every frame, so a click was never a click. So the answer is held: while the map's camera has not moved and the arrow's direction is within kHoldRadians of the direction it was taken at, the same answer is returned, and the mouse decides when it changes.
// Measured: the arrow's direction wanders by about 5e-6 radians over 19 frames of a near-still head (dump 050358167001) and by 1.7e-4 while the head yaws 8.7 degrees (dump 061416462001). The pick's own radius is at least 0.03 of the viewport's height, far above this.
constexpr float kHoldRadians = 5e-4f;
struct PointerHold {
    bool have = false;
    float dir[3] = {};
    uint64_t changes = 0;
    float out[4] = {};
    uint64_t ret = 0;
};
inline bool arrowDirection(const float arrow[3], float d[3]) {
    const float length = std::sqrt(arrow[0] * arrow[0] + arrow[1] * arrow[1] + arrow[2] * arrow[2]);
    if (!(length > kArrowMin && length < kArrowMax)) return false;
    for (int i = 0; i < 3; ++i) d[i] = arrow[i] / length;
    return true;
}
inline bool holdApplies(const PointerHold& h, const float arrow[3], uint64_t changes) {
    float d[3];
    if (!h.have || h.changes != changes || !arrowDirection(arrow, d)) return false;
    const float dx = d[0] - h.dir[0], dy = d[1] - h.dir[1], dz = d[2] - h.dir[2];
    return dx * dx + dy * dy + dz * dz < kHoldRadians * kHoldRadians;
}
inline void holdTake(PointerHold& h, const float arrow[3], uint64_t changes, const float out[4], uint64_t ret) {
    h.have = arrowDirection(arrow, h.dir);
    h.changes = changes;
    std::memcpy(h.out, out, sizeof(h.out));
    h.ret = ret;
}

// Whether the System Map's state is on: the pointer is armed, the build's System Map return was found, a map is open and it is not the Galaxy Map.
// Without the return the System Map stays as the game made it (no marker or reticule swap, no body table, no close-view smoothing).
inline bool systemMapOn(bool galaxyOpen, bool armed, uint64_t systemReturn, bool anyMapOpen) {
    return !galaxyOpen && armed && systemReturn != 0 && anyMapOpen;
}

// rel in the map's axes: a = rel . rows[0], b = rel . rows[1], c = rel . rows[2] (the rig checks the far point's direction with it).
inline void inMapAxes(const float rows[3][3], const float rel[3], float out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = rel[0] * rows[i][0] + rel[1] * rows[i][1] + rel[2] * rows[i][2];
}

// ---- the generated code ------------------------------------------------------------------------------------------------------------------------------
// Both relays jump, never call, so the detour's _ReturnAddress() is the game's caller. Each ends in `jmp [rip+0]` over an 8-byte cell the CodeHook
// prepare callback fills with the trampoline.

// The gate-first relay: mov rax, gate; cmp qword [rax], 0; je trampoline; jmp [callback]; jmp [trampoline]. 44 bytes; the trampoline cell is at 36.
constexpr uint32_t kGateRelayBytes = 44;
constexpr uint32_t kGateRelayTrampolineAt = 36;
inline void buildGateRelay(uint8_t* code, const void* gate, const void* callback) {
    const uint8_t body[kGateRelayBytes] = {
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0x48, 0x83, 0x38, 0,
        0x74, 0x0E, 0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    std::memcpy(code, body, sizeof(body));
    const uint64_t g = reinterpret_cast<uintptr_t>(gate), c = reinterpret_cast<uintptr_t>(callback);
    std::memcpy(code + 2, &g, 8);
    std::memcpy(code + 22, &c, 8);
}

// The capture stub: mov rax, slot; mov [rax], rcx; jmp [trampoline]. rax is scratch at a function's entry; every argument register is untouched.
// 27 bytes; the trampoline cell is at 19.
constexpr uint32_t kCaptureStubBytes = 27;
constexpr uint32_t kCaptureStubTrampolineAt = 19;
inline void buildCaptureStub(uint8_t* code, const void* slot) {
    const uint8_t body[kCaptureStubBytes] = {
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0x48, 0x89, 0x08,
        0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    std::memcpy(code, body, sizeof(body));
    const uint64_t s = reinterpret_cast<uintptr_t>(slot);
    std::memcpy(code + 2, &s, 8);
}

} // namespace mappointer
} // namespace edvr
