// Explorer Cam's per-frame settings, parsed once per configuration change (docs\design-explorer-cam-free-camera-2026-10-07.md; the review of 2026-10-09, "cache parsed frame
// settings").
//
// explorerCamFrameBoundary used to read nine keys from Config on every frame: a lock and a map lookup each, and seven numeric parses. The values only change when the ini is
// re-read (a hand edit, or the menu's write followed by the refresh it asks for) or when something sets one in memory; Config::generation() moves exactly then. So the settings
// are read once per generation, and a frame whose generation is the one they were read under takes them from here.
//
// What stays per frame, and is not cached here: the hotkey KEEPER. This holds the key the config says (`hotkey`); ecm::HotkeyKeeper::step still sees it every frame together
// with the session state, so a change made while a session is on is still held back until it ends and the key that started the session stays the exit.
//
// Pure: the source of values is an interface (Config in production, a script in tools\explorer_cam_fade_test).
#pragma once
#include <cstdint>
#include <string>

#include "explorer_cam_core.h"
#include "explorer_cam_follow_core.h"

namespace edvr {
namespace ecm {

// Where the values come from. Config answers these four questions the same way.
struct SettingsSource {
    virtual ~SettingsSource() = default;
    virtual float getFloat(const char* key, float def) const = 0;
    virtual int getInt(const char* key, int def) const = 0;
    virtual bool getBool(const char* key, bool def) const = 0;
    virtual std::string getString(const char* key, const char* def) const = 0;
};

struct FrameSettings {
    float up = kEyeUpDefault, forward = kEyeForwardDefault, right = kEyeRightDefault;   // the fixed eye (fix.explorer_cam_eye_*)
    float trimRight = kTrimRightDefault, trimUp = kTrimUpDefault, trimForward = kTrimForwardDefault;   // the follow trims
    float smoothingMs = static_cast<float>(kSmoothingMsDefault);
    std::string hotkey = "F5";      // hotkey.explorer_cam as the config says it now (not the key the keeper holds in force)
    bool readBindings = true;       // hotkey.read_game_bindings
};

class SettingsCache {
public:
    // The settings for a frame whose configuration is `generation` (Config::generation(), read BEFORE this call). Read from `src` the first time and whenever the
    // generation differs from the one the settings were read under; otherwise the stored ones, with no read of `src` at all.
    const FrameSettings& get(uint32_t generation, const SettingsSource& src) {
        if (!m_have || generation != m_generation) {
            m_s.up = src.getFloat("fix.explorer_cam_eye_up", kEyeUpDefault);
            m_s.forward = src.getFloat("fix.explorer_cam_eye_forward", kEyeForwardDefault);
            m_s.right = src.getFloat("fix.explorer_cam_eye_right", kEyeRightDefault);
            m_s.trimRight = src.getFloat("fix.explorer_cam_eye_trim_right", kTrimRightDefault);
            m_s.trimUp = src.getFloat("fix.explorer_cam_eye_trim_up", kTrimUpDefault);
            m_s.trimForward = src.getFloat("fix.explorer_cam_eye_trim_forward", kTrimForwardDefault);
            m_s.smoothingMs = static_cast<float>(src.getInt("fix.explorer_cam_follow_smoothing_ms", kSmoothingMsDefault));
            m_s.hotkey = src.getString("hotkey.explorer_cam", "F5");
            m_s.readBindings = src.getBool("hotkey.read_game_bindings", true);
            m_generation = generation;
            m_have = true;
            ++m_reads;
        }
        return m_s;
    }
    uint32_t reads() const { return m_reads; }   // how many times the source was read through (a counter for the rig and the log)
    void reset() { *this = SettingsCache(); }

private:
    FrameSettings m_s;
    uint32_t m_generation = 0;
    uint32_t m_reads = 0;
    bool m_have = false;
};

}  // namespace ecm
}  // namespace edvr
