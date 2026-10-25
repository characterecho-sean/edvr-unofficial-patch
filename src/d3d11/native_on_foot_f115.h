// F115 (native on-foot VR): boarding undoes every on-foot camera change. Header only, no game or D3D11 state, so native_frame_test runs the rules the frame thread
// uses. Journal: the arc doc, 2026-10-25, F115.
//
// The pilot's report (log edvr_gfx_20261025_071049, v0.21.1-236-g33e05706, Pimax Crystal Super on Pimax OpenXR, Epic): boarding put the view at the standing
// height, then it sank toward the neck and ended too low. In that flight the cockpit was drawn from 07:27:29.83 and native stereo on foot ended at 07:27:30.816
// (Status.json's on-foot bit, the same instant the runtime's seat memory heard "boarded"). The cockpit carried the on-foot raise of 34.6 cm (the eye alignment's
// y -0.346 m: the standing-height hold +16.9 cm and the neck reference's raise) and look to aim's seated yaw -5.84 deg and pitch +4.86 deg for that second. At
// the flag's fall the look went back to zero at once, but the hold and the neck reference ramped out over 0.5 s (f87::kHoldRampS, f94::kRampS), and nothing
// withheld or faded the view meanwhile.
//
// The rule: one exit edge for the on-foot camera. It falls at the first frame the cockpit's own panels draw while native stereo is still on foot, or when native
// stereo on foot's flag falls (Status.json's on-foot bit), whichever comes first. From that frame every value the frame thread hands the runtime and the pin for
// the camera (the eye alignment, the ADS shift, the look's reset serial, the ADS pitch, the rotation calibration, the left eye's share) is an exact zero, with no
// ramp, and the filters behind them start again from zero; nothing on foot publishes until the flag rises again. At the reset serial the runtime puts the seated
// space back on its origin (the seat memory's, while it holds).
//
// The cockpit's panels. The journal's Embark comes too late to use: at the four boardings the logs hold (VR 10-23 09:58:52.35 and 10-25 07:27:29.83; flat 10-24
// 04:21:00.48 and 15:58:33.72) the cockpit holo panels' first draw led Status.json's bit by 1.06, 0.98, 1.09 and 1.03 s, and Embark followed that draw by 13.7,
// 6.2, 4.5 and 4.3 s. In the pilot's four VR on-foot sessions of 10-24 and 10-25 (59 min on foot, among them the carrier's management screen and the HMD
// camera's resets at 04:55:25, 04:58:55, 05:00:23, 05:59:23, 06:00:28, 06:00:35, 06:06:46 and 06:06:55) no 30 s ui quality window counted a cockpit holo panel
// draw. The pilot flies with Elite's "Disable GUI effects" on, so the evidence is for that pair's vertex shader (holo_material.h kHoloGuiFxOffVs, the second of
// ui_layer_math.h's uiVsIsHoloPanel); the stock one (GUI effects on) has no flight, so its draws on foot are counted and not used. No SRV boarding is in the
// logs: an SRV ends on the flag.
#pragma once

#include <cmath>
#include <cstdint>

namespace edvr {
namespace nofoot {
namespace f115 {

constexpr uint64_t kFollowupMs = 1000;   // the second line, this long after the later of the edge and the flag's fall
constexpr double kSeatOffM = 0.10;       // the head this far over or under the seated origin a second after boarding: a recentre's worth (--health reads it)
constexpr uint64_t kCockpitPanelVs = 0x1989E6D3B405FDE0ull;        // the cockpit holo panels, GUI effects off: the early edge
constexpr uint64_t kCockpitPanelStockVs = 0x81216C77F90DEDD6ull;   // the cockpit holo panels, GUI effects on: counted on foot only
constexpr uint64_t kFalseStartMs = 3000;   // the panels drawn on foot, then gone this long with the flag still up: not a boarding, the changes resume
constexpr uint64_t kArmMs = 2000;          // the panels' edge arms once the flag has been up this long with no panel drawn (a disembark's last cockpit frames)

// The on-foot camera: native stereo on foot's flag, ended early by the cockpit's own panels. One step a frame, before the edge.
struct CameraOnFoot {
    bool up = false, armed = false, cockpit = false;
    uint64_t quietSinceMs = 0;   // before the trigger arms: the flag's rise, or the last panel draw after it
    uint64_t lastPanelMs = 0;
    struct Out {
        bool on = false;
        bool falseStart = false;   // the panels stopped kFalseStartMs ago with the flag still up
    };
    Out step(bool flag, bool panelsDrew, uint64_t nowMs) noexcept {
        Out o;
        if (!flag) {
            up = armed = cockpit = false;
            return o;
        }
        if (!up) {
            up = true;
            quietSinceMs = nowMs;
        }
        if (!armed) {
            if (panelsDrew) quietSinceMs = nowMs;
            else if (nowMs >= quietSinceMs && nowMs - quietSinceMs >= kArmMs) armed = true;
            o.on = true;
            return o;
        }
        if (panelsDrew) {
            cockpit = true;
            lastPanelMs = nowMs;
        } else if (cockpit && nowMs >= lastPanelMs && nowMs - lastPanelMs >= kFalseStartMs) {
            cockpit = false;
            o.falseStart = true;
        }
        o.on = !cockpit;
        return o;
    }
};

// The flag's edges, one step a frame (the frame boundary, after the flag is computed and before the look's total is published).
struct ExitEdge {
    bool seen = false, last = false;
    uint32_t exits = 0, entries = 0;
    enum Edge : uint8_t { kNone = 0, kExit = 1, kEntry = 2 };
    Edge step(bool active) noexcept {
        if (!seen) {   // the first frame: a process that starts on foot has entered; one that starts elsewhere has nothing to undo
            seen = true;
            last = active;
            if (!active) return kNone;
            ++entries;
            return kEntry;
        }
        if (active == last) return kNone;
        last = active;
        if (active) {
            ++entries;
            return kEntry;
        }
        ++exits;
        return kExit;
    }
};

// An on-foot term as published: itself on foot, an exact zero anywhere else (not ramped).
inline double gate(bool onFoot, double v) noexcept { return onFoot ? v : 0.0; }

// What the frame thread hands the runtime and the pin for the camera, one snapshot.
struct Published {
    double eyeM[3] = {};          // the eye alignment (EdvrNativeFrameOutput::eyeAlign), seated metres
    double lookDeg = 0.0;         // the look's seated yaw the runtime still carries (since the last reset serial)
    double pitchDeg = 0.0;        // the ADS pitch overlay
    double rotDeg[2] = {};        // F61's rotation calibration (yaw, pitch)
    double shiftM[3] = {};        // the ADS shift S the pin and the head follow take out of the camera
    double weaponWorldM[3] = {};  // F74's world displacement for the pin
    double share = 0.0;           // F106's left eye share
    bool zero() const noexcept {
        bool z = lookDeg == 0.0 && pitchDeg == 0.0 && rotDeg[0] == 0.0 && rotDeg[1] == 0.0 && share == 0.0;
        for (int i = 0; i < 3; ++i) z = z && eyeM[i] == 0.0 && shiftM[i] == 0.0 && weaponWorldM[i] == 0.0;
        return z;
    }
    double shiftCm() const noexcept { return 100.0 * std::sqrt(shiftM[0] * shiftM[0] + shiftM[1] * shiftM[1] + shiftM[2] * shiftM[2]); }
};

// One exit of the on-foot camera: armed at the edge, due a second after the flag's fall (the edge itself when the flag made it).
struct Record {
    bool armed = false;
    bool fromPanels = false;    // the cockpit's panels made the edge (else the flag)
    bool panelsAfter = false;   // F115b: the cockpit's panels drew between the edge and the read-back
    uint64_t edgeMs = 0;
    uint64_t flagFellMs = 0;    // 0 until the flag has fallen
    uint64_t frameAtEdge = 0;   // F115b: the runtime's frame sequence the last frame input carried at the edge
    uint32_t serial = 0;        // the zero eye alignment's serial
    double lookTotalRad = 0.0;  // the look's published total at the reset serial (the runtime's base from then on)
    bool due(uint64_t nowMs) const noexcept { return armed && flagFellMs != 0 && nowMs >= flagFellMs && nowMs - flagFellMs >= kFollowupMs; }
    // How far the edge led Status.json's bit (ms; 0 when the flag made it, -1 while it has not fallen).
    long long leadMs() const noexcept { return flagFellMs == 0 ? -1 : flagFellMs >= edgeMs ? static_cast<long long>(flagFellMs - edgeMs) : 0; }
};

// F115b: what the exit was, read a second after the flag's fall. Native stereo on foot also ends when the commander quits to the menu, loads or leaves
// a mode with no ship (log edvr_gfx_20261025_085120: the on-foot training left for the menu at 09:01:25.894, the game stopped submitting frames 0.5 s
// earlier and shut down 30 s later, and F115 read it as a boarding). A boarding is known by the cockpit's own panels drawing (they made the edge or drew
// after it) or by Status.json saying seated; anything else is another exit, whose seat and pose are not a boarding's to judge.
enum class Exit : uint8_t { kBoardingPanels = 0, kBoardingSeated = 1, kOther = 2, kUnknown = 3 };
inline Exit exitKind(bool fromPanels, bool panelsAfter, bool statusSeated) noexcept {
    return fromPanels || panelsAfter ? Exit::kBoardingPanels : statusSeated ? Exit::kBoardingSeated : Exit::kOther;
}
inline const char* exitName(Exit e) noexcept {
    return e == Exit::kBoardingPanels ? "boarding (the cockpit's panels drew)" : e == Exit::kBoardingSeated ? "boarding (Status.json says seated)"
                                                                               : e == Exit::kUnknown ? "not yet known (read back a second later)"
                                                                               : "not a boarding (no cockpit panel, not seated: a quit, a load or a menu)";
}
// F115c: what the edge itself can say. The panels or Status.json's seated flag make a boarding at once, and a game session that has ended makes a quit or a
// load (log edvr_gfx_20261025_113157: native on foot ended at 11:38:19.215 with "the game session ended", the game stopped presenting 1.5 s later, and
// no read-back came, so the exit was left unclassified); otherwise the read-back a second later decides.
inline Exit exitAtEdge(bool fromPanels, bool statusSeated, bool session) noexcept {
    return fromPanels ? Exit::kBoardingPanels : statusSeated ? Exit::kBoardingSeated : !session ? Exit::kOther : Exit::kUnknown;
}
inline const char* exitNameAtEdge(Exit e) noexcept {
    return e == Exit::kOther ? "not a boarding (the game session ended)" : exitName(e);
}
// F115b: whether the runtime handed the d3d11 half a frame after the edge (a quit stops the game's frames, and the last pose still carries the on-foot
// alignment: the head read from it is not the cockpit's).
inline bool frameSinceEdge(uint64_t frameAtEdge, uint64_t frameNow) noexcept { return frameNow != frameAtEdge; }

// The head in the seated space (a rigid 3x4, row-major, as EdvrNativeNowPose::render): its height over the origin and its distance from the origin's vertical.
inline double headHeightM(const float m[12]) noexcept { return static_cast<double>(m[7]); }
inline double headOffsetM(const float m[12]) noexcept { return std::sqrt(static_cast<double>(m[3]) * m[3] + static_cast<double>(m[11]) * m[11]); }
inline bool seatOff(double heightM) noexcept { return !(std::fabs(heightM) < kSeatOffM); }

}  // namespace f115
}  // namespace nofoot
}  // namespace edvr
