// Elite's Bloom graphics option, read for the Star glow hint (src/common/star_glow_bloom.h has the words and the condition).
//
// WHAT IS READ. BloomQuality (0 = off) from the ACTIVE preset's file, for every preset: the highest Custom.<major>.<minor>.fxcfg for Custom, the
// game's OptionDefaults\<Preset>.fxcfg for a stock one (VRHigh and the rest; every stock VR preset sets 0). The file pick, the stock-preset
// path, the comment cutting and the failing-soft reasons are terrain_checkerboard_reader.h's readPresetTag, called here and not copied: the
// terrain toggle is read through the same function. The answer is on, off or unknown (sgb::Bloom); a missing folder, file or tag, a preset that
// is not Custom or a stock one, and a value that is no whole number are unknown, with the reason in words.
//
// WHEN IT IS READ. Never per frame, and on no worker: a Watch reads when the setting rises above 100, when the setting changes while it is above
// 100, and when a menu opens while it is above 100 (the files are a KB or two each and the game rewrites them only at Apply). At the stock 100
// nothing is read. The caller (menu.cpp) owns one Watch; a rig drives it on fixture folders.
//
// WHAT IT SAYS. A line when the setting rises above 100 (whatever Bloom is), and a line when the Bloom state changes while it stays above 100.
// Header-only; it reads files and never writes one.
#pragma once

#include "../common/star_glow_bloom.h"
#include "terrain_checkerboard_reader.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace edvr {
namespace sgb {

// The tag, as the files spell it.
inline constexpr char kTag[] = "BloomQuality";

// One read of the active preset's Bloom.
inline Reading readEliteBloom(const std::wstring& folder, const std::wstring& gameDir) {
    tcn::Reading raw;
    const bool found = tcn::readPresetTag(folder, gameDir, kTag, &raw);
    Reading r;
    std::snprintf(r.preset, sizeof(r.preset), "%s", raw.preset);
    std::snprintf(r.source, sizeof(r.source), "%s", raw.source);
    std::snprintf(r.value, sizeof(r.value), "%s", raw.value);
    if (!found) {
        std::snprintf(r.reason, sizeof(r.reason), "%s", raw.reason);
        return r;
    }
    r.state = bloomOfText(r.value);
    if (r.state == Bloom::Unknown)
        std::snprintf(r.reason, sizeof(r.reason), "%s holds %s=%s, which is not a whole number of 0 or more", r.source, kTag, r.value);
    return r;
}

// The menu's one owner of the answer: the setting last seen, what the files said when it was last asked, and which state was last logged.
class Watch {
public:
    void configure(const std::wstring& folder, const std::wstring& gameDir) {
        folder_ = folder;
        gameDir_ = gameDir;
    }
    // The setting as the configure sweep reads it. True when a line is due (in *line): the setting rose above 100, or it moved while above
    // 100 and the files now say another state. At 100, or unchanged, nothing is read.
    bool noteSetting(int percent, std::string* line) {
        const int before = percent_;
        percent_ = percent;
        if (percent <= kStockPercent || percent == before) return false;
        return readNow(before <= kStockPercent, line);
    }
    // A menu opened. With the setting above 100 the files are read again, and a changed state is said.
    bool noteMenuOpened(std::string* line) {
        if (percent_ <= kStockPercent) return false;
        return readNow(false, line);
    }
    int percent() const { return percent_; }
    Bloom state() const { return reading_.state; }
    const Reading& reading() const { return reading_; }
    // The hint the setting and the last read call for, null when there is none.
    const char* hint() const { return sgb::hint(percent_, reading_.state); }
    // How many times the files were read; a rig holds it to the reads the rules above allow.
    uint32_t reads() const { return reads_; }

private:
    bool readNow(bool rise, std::string* line) {
        reading_ = readEliteBloom(folder_, gameDir_);
        ++reads_;
        if (!rise && logged_ && reading_.state == loggedState_) return false;
        logged_ = true;
        loggedState_ = reading_.state;
        char text[600];
        formatLog(text, sizeof(text), reading_);
        if (line) *line = text;
        return true;
    }

    std::wstring folder_, gameDir_;
    int percent_ = kStockPercent;
    Reading reading_;
    bool logged_ = false;
    Bloom loggedState_ = Bloom::Unknown;
    uint32_t reads_ = 0;
};

}  // namespace sgb
}  // namespace edvr
