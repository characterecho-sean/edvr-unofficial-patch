// Elite's terrain checkerboard rendering in VR, said in the headset (design doc docs/design-flat-temporal-aa-2026-09-23.md,
// section 84, the VR hint).
//
// WHY. Elite's graphics option TerrainCheckerboardRenderingEnabled draws the distant terrain with half its horizontal
// samples. With it on, the neighbours in the raw input pair up beyond about 20 m (not in y, not in the cockpit, not in the
// sky), DLSS cannot steady an input paired like that, and the distant hills shimmer in VR. Turning the option off removed
// the shimmer on the rig that found it, with nothing else changed. Every stock VR preset ships it on (OptionDefaults\VRLow,
// VRMedium, VRHigh and VRUltra all say true), so a VR player who never opened the Custom preset has the shimmer too. The
// flat game did not shimmer with the same settings folder, so this is a VR notice only: the flat profile never starts the
// reader, shows the words or writes the line.
//
// WHAT IS READ. Elite's ACTIVE graphics preset, by src/d3d11/terrain_checkerboard_reader.h, on a worker thread of its own
// (src/d3d11/terrain_checkerboard.cpp): Settings.xml names the preset; for Custom the toggle is the tag in the highest
// Custom.<major>.<minor>.fxcfg, for a stock preset it is the tag in <game folder>\OptionDefaults\<Preset>.fxcfg. Settings.xml
// carries a tag of the same name too, and it is NOT the toggle (it reads true on every machine seen, including the one where the
// option is off in the game). Anything the reader does not understand is Unknown, and Unknown says nothing: only On raises the
// notice, never a guess. The render thread never touches a file: it loads one atomic word (pack/unpack below).
//
// WHERE IT SHOWS. The settings menu's own notice path, the way vr_supersample_notice.h does: a toast once per RAISE of the
// option, and the Status page's hint (which replaces the page's existing hint text and adds no line: the menu bitmap is refused
// above 2048 px, see vr_supersample_notice.h). Unlike the supersampling notice this one can END: the option is read live, so the
// hint follows the file and the toast is said again when the option is turned off and on again.
//
// THE LABEL. The in-game option's label could not be confirmed from any game file (its UI strings are in compressed assets;
// the exe has the words only as a GPU pass name, the XML tag and a UI key), so the words say "terrain checkerboard rendering",
// which is what the tag and the pass call it.
//
// Pure and dependency-free: tools\terrain_checkerboard_test drives every function here, and the DLL calls the very same ones.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace tcn {

// What the reader knows. Unknown is the answer to every doubt: no folder, no file, no preset it understands, a tag that is not
// there or says something else than true or false. Off and Unknown both say nothing; only On raises the notice.
enum class State : uint8_t { Unknown = 0, Off = 1, On = 2 };

// How often the worker reads the files (a game rewrites them when options are applied, which is seconds apart at best).
constexpr uint32_t kPollMs = 3000;
// A known state is left for Unknown only when this many reads in a row say Unknown: the game rewrites Settings.xml and the preset
// IN PLACE at Apply, so one read can catch a half-written file, and the last good answer stands until the next read agrees.
constexpr int kUnknownReads = 2;
// When both hints hold (this one and the supersampling one), the Status page's single hint slot alternates every this long.
constexpr uint64_t kHintAlternateMs = 6000;
// The log's bound: the first read and each change, this many lines a session, then one line saying the rest are not logged.
constexpr int kLogMax = 16;
// The lengths of the two short forms: a toast is one line of the toast card, the hint two lines of the Status page's hint box.
constexpr size_t kToastMax = 55;
constexpr size_t kHintMax = 78;

inline const char* stateWord(State s) { return s == State::On ? "ON" : s == State::Off ? "OFF" : "unknown"; }

// ---- the published word ---------------------------------------------------------------------------------------------------
// The state and a version in ONE atomic word, so the render thread reads a consistent pair (two atomics could tear: a state from
// one publish with the version of the next, and one raise toasted twice). The version counts the publishes: 0 until the first read
// finishes, 1 for the first answer (whatever it is), then up by one at every change of the state. 0 in the version is never
// published again, so a latch that starts at 0 has never said anything.
inline uint32_t pack(State state, uint32_t version) { return (version & 0xFFFFFFu) << 8 | static_cast<uint32_t>(state); }
inline State unpackState(uint32_t word) {
    const uint32_t s = word & 0xFFu;
    return s == static_cast<uint32_t>(State::On) ? State::On : s == static_cast<uint32_t>(State::Off) ? State::Off : State::Unknown;
}
inline uint32_t unpackVersion(uint32_t word) { return word >> 8 & 0xFFFFFFu; }
inline uint32_t nextVersion(uint32_t word) {
    const uint32_t v = (unpackVersion(word) + 1u) & 0xFFFFFFu;
    return v ? v : 1u;
}

// ---- the toast's latch ----------------------------------------------------------------------------------------------------
// Whether to queue the toast now: once per RAISE. A raise is a published version whose state is On, and the latch is the last
// version that toasted (0 = never), set HERE so the flag is flipped before the caller tests menu.toasts. A version only comes
// from a change of the state, so a new version in the state On means it was not On before: turning the option off and on again
// toasts again, and an option that stays on toasts once, however many frames look at it. Off and Unknown never toast.
inline bool toastOnRaise(uint32_t* toasted, State state, uint32_t version) {
    if (state != State::On || version == *toasted) return false;
    *toasted = version;
    return true;
}

// ---- the Status page's one hint slot ---------------------------------------------------------------------------------------
// The page has one hint (two lines under its rows) and no room for a second: this notice and vr_supersample_notice.h both write
// it. Which one shows: the one that applies; when both do, they take turns every kHintAlternateMs, the supersampling hint first
// (the page rebuilds every 500 ms, so a turn lands within half a second of its time).
enum class Hint : uint8_t { None, Supersampling, Checkerboard };
inline Hint pickStatusHint(bool supersamplingApplies, bool checkerboardApplies, uint64_t nowMs) {
    if (supersamplingApplies && checkerboardApplies)
        return (nowMs / kHintAlternateMs) % 2u == 0u ? Hint::Supersampling : Hint::Checkerboard;
    if (checkerboardApplies) return Hint::Checkerboard;
    if (supersamplingApplies) return Hint::Supersampling;
    return Hint::None;
}

// ---- the log's bound ------------------------------------------------------------------------------------------------------
// One line for the first read and one for each change, at most kLogMax; the first change past that gets the limit line, and the
// rest are silent (the notice still follows the file).
enum class LogDue : uint8_t { Line, Limit, Nothing };
inline LogDue logDue(int* logged) {
    if (*logged < kLogMax) { ++*logged; return LogDue::Line; }
    if (*logged == kLogMax) { ++*logged; return LogDue::Limit; }
    return LogDue::Nothing;
}

// ---- the words ------------------------------------------------------------------------------------------------------------
// The full sentence: what it does and what to set. It is the log's tail on an ON line; the toast and the hint are its short forms.
inline int formatSentence(char* out, size_t size) {
    return std::snprintf(out, size,
        "Elite's terrain checkerboard rendering makes distant terrain shimmer with DLSS. Turn it off in Elite's graphics options.");
}
// The headset toast: one short line (kToastMax characters at most), which says what the player sees and what to turn off.
inline int formatToast(char* out, size_t size) {
    return std::snprintf(out, size, "Distant terrain shimmers: turn off terrain checkerboard");
}
// The Status page's hint, at most kHintMax characters: it replaces the 78-character hint the page has, so the page never wraps
// to a third line and gains none.
inline int formatStatusHint(char* out, size_t size) {
    return std::snprintf(out, size, "Turn off terrain checkerboard rendering in Elite's graphics options.");
}

// The log line for a read: the state, what it rests on, and (On) the sentence. One line (the log's buffer is 1200 bytes). `preset`,
// `source` (the file the answer came from: Custom.4.4.fxcfg, or OptionDefaults\VRHigh.fxcfg), `value` (the tag's text as written)
// and `reason` (an Unknown's cause) come from the reader. No parenthesis in a reason or the sentence: the reader of the log
// (tools\edvr_log.py --terrain-checkerboard) takes the last one as the end of the detail.
inline int formatLog(char* out, size_t size, State state, const char* preset, const char* source, const char* value, const char* reason) {
    if (state == State::Unknown)
        return std::snprintf(out, size, "vr terrain checkerboard: unknown (%s): no notice.", reason ? reason : "no reason");
    char sentence[200];
    formatSentence(sentence, sizeof(sentence));
    if (state == State::On)
        return std::snprintf(out, size, "vr terrain checkerboard: ON (preset %s, %s: TerrainCheckerboardRenderingEnabled=%s): %s",
                             preset ? preset : "", source ? source : "", value ? value : "", sentence);
    return std::snprintf(out, size, "vr terrain checkerboard: OFF (preset %s, %s: TerrainCheckerboardRenderingEnabled=%s): no notice.",
                         preset ? preset : "", source ? source : "", value ? value : "");
}
// The worker's own lines: it started (so no line at all means it never did), could not start, faulted for good, or hit the log bound.
inline int formatStartLine(char* out, size_t size, unsigned long threadId, uint32_t cadenceMs) {
    return std::snprintf(out, size,
        "vr terrain checkerboard: reading Elite's graphics settings on its own thread (%lu), every %u s, off the render thread.",
        threadId, static_cast<unsigned>((cadenceMs + 500u) / 1000u));
}
inline int formatStartFailedLine(char* out, size_t size) {
    return std::snprintf(out, size,
        "vr terrain checkerboard: could not start the reader thread, so Elite's terrain checkerboard rendering is not being read: no notice.");
}
inline int formatFaultLine(char* out, size_t size) {
    return std::snprintf(out, size,
        "vr terrain checkerboard: the reader faulted repeatedly and has stopped; the notice keeps the last state it published.");
}
inline int formatLimitLine(char* out, size_t size) {
    return std::snprintf(out, size,
        "vr terrain checkerboard: %d lines logged; further changes are followed by the notice but no longer logged.", kLogMax);
}
// The menu's note that the notice was queued, once per raise: the toast, or the reason there is none. The Status page's hint is
// there either way while the menu is open.
inline int formatQueuedLog(char* out, size_t size, bool toasted, const char* toast) {
    if (toasted)
        return std::snprintf(out, size,
            "vr terrain checkerboard: the headset notice is queued as a toast (\"%s\"); the Status page shows the advice as its hint "
            "while the menu is open.", toast ? toast : "");
    return std::snprintf(out, size,
        "vr terrain checkerboard: menu.toasts is off, so no toast; the Status page shows the advice as its hint while the menu is open.");
}

}  // namespace tcn
}  // namespace edvr
