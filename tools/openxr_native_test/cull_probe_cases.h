#pragma once
// The terrain-culling arc's projection-caller census and selective-lie probe (docs\terrain-culling.md): the group classifier at every RVA the 2026-10-09
// disassembly names and one either side of it, the symmetric superset, the census table (first sight, when a stack is captured, counts, summaries, overflow), and
// what the host decides from the ini (stand down, ignored under a guard, active). The call-site end of it -- that the answers reach the game -- is in
// tools\openxr_system_test.
#include "launch_centre_cases.h"
#include "../../src/openxr/projection_callers.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace edvr::openxr::test {
namespace probe_fixture {
constexpr uintptr_t kBase = 0x140000000ull;
inline ExeModule build332841() { return ExeModule{kBase, 104894464u, 1788384820u, 104894464u}; }
inline uintptr_t at(uint32_t rva) { return kBase + rva; }
struct Capture {
  unsigned calls = 0;
  uintptr_t second = 0, third = 0;
  void operator()(ProjectionCallers::Frames& frames) { ++calls; frames.second = second; frames.third = third; }
};
struct Lines {
  std::vector<std::string> lines;
  void operator()(const char* line) { lines.emplace_back(line); }
  bool has(const std::string& text) const {
    for (const auto& line : lines) if (line == text) return true;
    return false;
  }
  bool contains(const std::string& text) const {
    for (const auto& line : lines) if (line.find(text) != std::string::npos) return true;
    return false;
  }
};
using Census = ProjectionCallers;
inline unsigned countOf(const Census& census, Census::Method method, unsigned eye, uint32_t rva1, uint32_t rva2) {
  Census::Entry entries[Census::kCapacity];
  const unsigned n = census.snapshot(entries, Census::kCapacity);
  for (unsigned i = 0; i < n; ++i)
    if (entries[i].method == method && entries[i].eye == eye && entries[i].rva1 == rva1 && entries[i].rva2 == rva2)
      return unsigned(entries[i].count);
  return 0;
}
}
template<class Check> void runCullProbeCases(Check&& check) {
  using namespace probe_fixture;
  using namespace cull_rva;
  const uint32_t unknown = kFrameUnknown;
  // ---- the group classifier --------------------------------------------------------------------------------------------------------------------------------
  {
    // Every caller the disassembly names, with the group it belongs to.
    struct Caller { uint32_t rva1, rva2; CallerGroup group; };
    const Caller callers[] = {
      {kEyeFov, kCameraSetter, CallerGroup::Camera}, {kEyeFov, kUiScale, CallerGroup::Ui},
      {kSkyFov, unknown, CallerGroup::Sky}, {kSkyFov, kCameraSetter, CallerGroup::Sky},
      {kSizes[0], unknown, CallerGroup::Sizes}, {kSizes[1], unknown, CallerGroup::Sizes},
      {kSizes[2], unknown, CallerGroup::Sizes}, {kSizes[3], unknown, CallerGroup::Sizes}, {kSizes[3], kUiScale, CallerGroup::Sizes},
      // Anything else is other, the first site with any other frame 2 included.
      {kEyeFov, 0x1234, CallerGroup::Other}, {kEyeFov, unknown, CallerGroup::Other}, {kEyeFov, kFrameOutside, CallerGroup::Other},
      {0x4E2F50, kCameraSetter, CallerGroup::Other}, {0x1000, unknown, CallerGroup::Other}, {kFrameOutside, kCameraSetter, CallerGroup::Other},
      {kFrameUnknown, unknown, CallerGroup::Other}, {0, 0, CallerGroup::Other},
    };
    bool classified = true;
    for (const auto& c : callers) classified = classified && classifyProjectionRawCaller(c.rva1, c.rva2) == c.group;
    check(classified, "the classifier puts every named GetProjectionRaw caller in its group (camera and ui split on frame 2; sky and sizes on frame 1 alone)");
    // A near miss, one either side of every RVA, belongs to nobody but other.
    bool nearMisses = true;
    for (const uint32_t rva : {kSkyFov, kSizes[0], kSizes[1], kSizes[2], kSizes[3]})
      for (const int delta : {-1, 1})
        nearMisses = nearMisses && classifyProjectionRawCaller(rva + delta, unknown) == CallerGroup::Other &&
                     classifyProjectionRawCaller(rva + delta, kCameraSetter) == CallerGroup::Other;
    for (const int delta : {-1, 1}) {
      nearMisses = nearMisses && classifyProjectionRawCaller(kEyeFov + delta, kCameraSetter) == CallerGroup::Other &&
                   classifyProjectionRawCaller(kEyeFov + delta, kUiScale) == CallerGroup::Other &&
                   classifyProjectionRawCaller(kEyeFov, kCameraSetter + delta) == CallerGroup::Other &&
                   classifyProjectionRawCaller(kEyeFov, kUiScale + delta) == CallerGroup::Other;
    }
    check(nearMisses, "a near miss (RVA +-1) is other, for frame 1 of every site and for both frame 2 values of the first");
    // Which probe answers which caller: exactly the group it names, all answers everyone, off nobody.
    const CullProbe probes[] = {CullProbe::Camera, CullProbe::Ui, CullProbe::Sky, CullProbe::Sizes, CullProbe::Other};
    const CallerGroup groups[] = {CallerGroup::Camera, CallerGroup::Ui, CallerGroup::Sky, CallerGroup::Sizes, CallerGroup::Other};
    bool selects = true;
    for (const auto& c : callers) {
      selects = selects && probeSelects(CullProbe::All, c.rva1, c.rva2) && !probeSelects(CullProbe::Off, c.rva1, c.rva2);
      for (unsigned i = 0; i < 5; ++i) selects = selects && probeSelects(probes[i], c.rva1, c.rva2) == (c.group == groups[i]);
    }
    check(selects, "each probe group answers exactly its own callers; all answers every caller; off answers none");
    check(probeNeedsFrame2(CullProbe::Camera, kEyeFov) && probeNeedsFrame2(CullProbe::Ui, kEyeFov) && probeNeedsFrame2(CullProbe::Other, kEyeFov) &&
          !probeNeedsFrame2(CullProbe::All, kEyeFov) && !probeNeedsFrame2(CullProbe::Sky, kEyeFov) && !probeNeedsFrame2(CullProbe::Sizes, kEyeFov) &&
          !probeNeedsFrame2(CullProbe::Off, kEyeFov) && !probeNeedsFrame2(CullProbe::Camera, kSkyFov) && !probeNeedsFrame2(CullProbe::Other, kEyeFov + 1) &&
          !probeNeedsFrame2(CullProbe::Camera, kEyeFov - 1),
          "only the first site, and only under the groups that split it (camera, ui, other), needs frame 2 on every call");
    check(cullProbeFromCode(0) == CullProbe::Off && cullProbeFromCode(1) == CullProbe::All && cullProbeFromCode(2) == CullProbe::Camera && cullProbeFromCode(3) == CullProbe::Ui &&
          cullProbeFromCode(4) == CullProbe::Sky && cullProbeFromCode(5) == CullProbe::Sizes && cullProbeFromCode(6) == CullProbe::Other && cullProbeFromCode(7) == CullProbe::Off &&
          cullProbeFromCode(0xFFFFFFFFu) == CullProbe::Off && std::strcmp(cullProbeName(CullProbe::Sizes), "sizes") == 0 && std::strcmp(cullProbeName(CullProbe::Off), "off") == 0,
          "the probe codes name off, all, camera, ui, sky, sizes, other; anything else is off");
  }
  // ---- the superset ------------------------------------------------------------------------------------------------------------------------------------------
  {
    const RawFov truth{-1.5293f, 1.0324f, -1.2000f, 1.2648f};
    const RawFov wide = widenedRaw(truth);
    check(wide.left == -1.5293f && wide.right == 1.5293f && wide.top == -1.2648f && wide.bottom == 1.2648f,
          "the superset is per axis: l' = -max(|l|,|r|), r' = +max, t' = -max(|t|,|b|), b' = +max");
    check(wide.left <= truth.left && wide.right >= truth.right && wide.top <= truth.top && wide.bottom >= truth.bottom && wide.left == -wide.right && wide.top == -wide.bottom,
          "...it holds the frustum it was made from and is symmetric");
    const RawFov flipped{-0.4f, 0.9f, -0.2f, 0.1f};
    const RawFov w2 = widenedRaw(flipped);
    check(w2.left == -0.9f && w2.right == 0.9f && w2.top == -0.2f && w2.bottom == 0.2f, "...whichever side is the larger");
    const RawFov already = widenedRaw(RawFov{-1.0f, 1.0f, -0.5f, 0.5f});
    check(already.left == -1.0f && already.right == 1.0f && already.top == -0.5f && already.bottom == 0.5f, "...a symmetric frustum is returned as it was");
    const RawFov zero = widenedRaw(RawFov{}), zeroReference{};
    check(std::memcmp(&zero, &zeroReference, sizeof(zero)) == 0, "...and a zero stays +0, bit for bit");
    // The jitter tangent shift rides in the input: a shifted frustum widens from its shifted sides.
    const RawFov shifted{truth.left + 0.01f, truth.right + 0.01f, truth.top - 0.02f, truth.bottom - 0.02f};
    const RawFov w3 = widenedRaw(shifted);
    const float horizontal = (std::max)(std::fabs(shifted.left), std::fabs(shifted.right)), vertical = (std::max)(std::fabs(shifted.top), std::fabs(shifted.bottom));
    check(w3.left == -horizontal && w3.right == horizontal && w3.top == -vertical && w3.bottom == vertical && horizontal == std::fabs(shifted.left) && vertical == std::fabs(shifted.bottom),
          "...the shift is part of what is widened");
  }
  // ---- the census -------------------------------------------------------------------------------------------------------------------------------------------
  {
    ProjectionCallers census(build332841());
    Capture capture;capture.second = at(kCameraSetter);capture.third = at(0x283D744);
    Lines sink;
    const bool wide = census.note(ProjectionCallers::Raw, 0, at(kEyeFov), CullProbe::Off, 1000, 77, capture, sink);
    check(!wide && capture.calls == 1 && sink.lines.size() == 1 &&
          sink.lines[0] == "projection callers: GetProjectionRaw exe+0x4E2FA5 <- exe+0x2878E1B <- exe+0x283D744 eye 0 tid 77",
          "first sight of a caller takes one stack capture and logs one line: method, frames 1-3, eye, thread");
    census.note(ProjectionCallers::Raw, 0, at(kEyeFov), CullProbe::Off, 1010, 77, capture, sink);
    census.note(ProjectionCallers::Raw, 1, at(kEyeFov), CullProbe::Off, 1020, 78, capture, sink);
    check(capture.calls == 1 && sink.lines.size() == 1, "the same frame 1 again, either eye, takes no capture and logs nothing");
    check(countOf(census, ProjectionCallers::Raw, 0, kEyeFov, kCameraSetter) == 1 && countOf(census, ProjectionCallers::Raw, 0, kEyeFov, unknown) == 1 &&
          countOf(census, ProjectionCallers::Raw, 1, kEyeFov, unknown) == 1,
          "...and is counted per (method, frame 1, frame 2, eye); a call that was not captured has no frame 2");
    census.note(ProjectionCallers::Matrix, 0, at(kEyeFov), CullProbe::Off, 1030, 77, capture, sink);
    census.note(ProjectionCallers::EyeToHead, 1, at(0x4E25FE), CullProbe::Off, 1040, 77, capture, sink);
    check(capture.calls == 3 && sink.lines.size() == 3 && sink.lines[1].find("GetProjectionMatrix exe+0x4E2FA5") != std::string::npos &&
          sink.lines[2].find("GetEyeToHeadTransform exe+0x4E25FE") != std::string::npos && sink.lines[2].find("eye 1") != std::string::npos,
          "a method has its own frame 1: the same address under GetProjectionMatrix, and GetEyeToHeadTransform, is a first sight each");
    // A frame 1 outside the game's image is named so, and an unwind that gave nothing is a question mark.
    Capture none;
    census.note(ProjectionCallers::Raw, 0, kBase - 16, CullProbe::Off, 1050, 5, none, sink);
    census.note(ProjectionCallers::Raw, 0, kBase + 104894464u, CullProbe::Off, 1060, 5, none, sink);
    check(sink.lines.size() == 4 && sink.lines[3] == "projection callers: GetProjectionRaw outside <- ? <- ? eye 0 tid 5" &&
          none.calls == 1 && countOf(census, ProjectionCallers::Raw, 0, kFrameOutside, unknown) == 2,
          "a frame 1 below the image or at its end is outside (one caller, counted together), and a capture that unwound nothing logs ?");
    census.note(ProjectionCallers::Raw, 9, at(0x4E3C93), CullProbe::Off, 1070, 5, none, sink);
    check(countOf(census, ProjectionCallers::Raw, 255, 0x4E3C93, unknown) == 1, "an eye that is neither 0 nor 1 is kept apart");
  }
  // ---- when a stack is captured, and what the probe is told ----------------------------------------------------------------------------------
  {
    ProjectionCallers census(build332841());
    Capture capture;Lines sink;
    capture.second = at(kCameraSetter);
    // camera: the first site is captured on every call and answered only when frame 2 is the camera setter.
    bool answered = census.note(ProjectionCallers::Raw, 0, at(kEyeFov), CullProbe::Camera, 1, 1, capture, sink);
    answered = answered && census.note(ProjectionCallers::Raw, 0, at(kEyeFov), CullProbe::Camera, 2, 1, capture, sink);
    check(answered && capture.calls == 2 && sink.lines.size() == 1, "camera: the first site is captured on every call, answered when frame 2 is the camera setter, logged once");
    census.note(ProjectionCallers::Raw, 1, at(kEyeFov), CullProbe::Camera, 2, 1, capture, sink);
    check(sink.lines.size() == 1, "...the other eye reaching the same frames is the same caller: no second line");
    capture.second = at(kUiScale);
    check(!census.note(ProjectionCallers::Raw, 0, at(kEyeFov), CullProbe::Camera, 3, 1, capture, sink) && capture.calls == 4 && sink.lines.size() == 2 &&
          sink.lines[1] == "projection callers: GetProjectionRaw exe+0x4E2FA5 <- exe+0x8D269A <- ? eye 0 tid 1",
          "...the same site reached from the ui scale is not answered, and its frame 2 is a first sight of its own");
    check(census.note(ProjectionCallers::Raw, 0, at(kEyeFov), CullProbe::Ui, 4, 1, capture, sink) && capture.calls == 5, "ui: the ui scale is answered");
    capture.second = at(0x5555);
    check(!census.note(ProjectionCallers::Raw, 1, at(kEyeFov), CullProbe::Ui, 5, 1, capture, sink) && census.note(ProjectionCallers::Raw, 1, at(kEyeFov), CullProbe::Other, 6, 1, capture, sink),
          "other: the first site reached from anywhere else is answered, and is not a ui caller");
    capture.second = 0;
    check(census.note(ProjectionCallers::Raw, 0, at(kEyeFov), CullProbe::Other, 7, 1, capture, sink) && !census.note(ProjectionCallers::Raw, 0, at(kEyeFov), CullProbe::Camera, 8, 1, capture, sink),
          "a capture that gave no frame 2 is other: answered under other, never under camera");
    // sky, sizes and all: no capture on the first site, whatever frame 2 would have said.
    Capture quiet;quiet.second = at(kCameraSetter);
    ProjectionCallers census2(build332841());
    census2.note(ProjectionCallers::Raw, 0, at(kEyeFov), CullProbe::Off, 1, 1, quiet, sink);
    const unsigned afterFirst = quiet.calls;
    bool sky = census2.note(ProjectionCallers::Raw, 0, at(kSkyFov), CullProbe::Sky, 2, 1, quiet, sink);
    sky = sky && census2.note(ProjectionCallers::Raw, 0, at(kSkyFov), CullProbe::Sky, 3, 1, quiet, sink);
    const unsigned afterSky = quiet.calls;
    check(sky && !census2.note(ProjectionCallers::Raw, 0, at(kEyeFov), CullProbe::Sky, 4, 1, quiet, sink) && quiet.calls == afterSky && afterSky == afterFirst + 1,
          "sky: the sky site is answered; the first site takes no capture, and the sky site only its first-sight one");
    bool sizes = true;
    for (const uint32_t rva : kSizes) sizes = sizes && census2.note(ProjectionCallers::Raw, 0, at(rva), CullProbe::Sizes, 5, 1, quiet, sink);
    check(sizes && !census2.note(ProjectionCallers::Raw, 0, at(kSizes[0] + 1), CullProbe::Sizes, 6, 1, quiet, sink) &&
          !census2.note(ProjectionCallers::Raw, 0, at(kSkyFov), CullProbe::Sizes, 7, 1, quiet, sink), "sizes: all four size sites are answered, a near miss and the sky site are not");
    const unsigned beforeAll = quiet.calls;
    bool all = true;
    for (const uint32_t rva : {kEyeFov, kSkyFov, kSizes[2], 0x1234u}) all = all && census2.note(ProjectionCallers::Raw, 0, at(rva), CullProbe::All, 8, 1, quiet, sink);
    check(all && quiet.calls == beforeAll + 1, "all: every caller is answered, and only a new frame 1 is captured");
    // Honest for everyone else: only GetProjectionRaw is ever answered, and off answers nobody.
    check(!census2.note(ProjectionCallers::Matrix, 0, at(kEyeFov), CullProbe::All, 9, 1, quiet, sink) && !census2.note(ProjectionCallers::EyeToHead, 0, at(kEyeFov), CullProbe::All, 10, 1, quiet, sink) &&
          !census2.note(ProjectionCallers::Raw, 0, at(kEyeFov), CullProbe::Off, 11, 1, quiet, sink),
          "GetProjectionMatrix and GetEyeToHeadTransform are never answered, whatever the probe, and off answers nobody");
  }
  // ---- the table is fixed, and full is counted, not grown -----------------------------------------------------------------------------
  {
    ProjectionCallers census(build332841());
    Capture capture;Lines sink;
    for (unsigned i = 0; i < ProjectionCallers::kCapacity; ++i)
      census.note(ProjectionCallers::Matrix, 0, at(0x100000 + i), CullProbe::Off, 1, 1, capture, sink);
    check(census.used() == ProjectionCallers::kCapacity && census.overflowTotal() == 0 && capture.calls == ProjectionCallers::kCapacity && sink.lines.size() == ProjectionCallers::kCapacity,
          "the table takes its capacity of distinct callers, a capture and a line each");
    const size_t linesBefore = sink.lines.size();
    const bool wide = census.note(ProjectionCallers::Raw, 0, at(0x200000), CullProbe::All, 2, 1, capture, sink);
    check(census.used() == ProjectionCallers::kCapacity && census.overflowTotal() == 1 && capture.calls == ProjectionCallers::kCapacity && sink.lines.size() == linesBefore && wide,
          "one more is counted as an overflow: no entry, no capture, no line, and the probe still answers it");
    census.note(ProjectionCallers::Matrix, 1, at(0x100000), CullProbe::Off, 3, 1, capture, sink);
    check(census.overflowTotal() == 2 && countOf(census, ProjectionCallers::Matrix, 1, 0x100000, unknown) == 0,
          "...a known caller whose count under the other eye has no room is an overflow too; it does not displace an entry");
    const unsigned before = capture.calls;
    capture.second = at(kCameraSetter);
    const bool camera = census.note(ProjectionCallers::Raw, 0, at(kEyeFov), CullProbe::Camera, 4, 1, capture, sink);
    check(camera && capture.calls == before + 1, "...and a full table still captures a frame 2 the probe needs, so the answer is right");
    Lines out;
    census.summary("test", out);
    unsigned items = 0;
    bool shortLines = true, prefixed = true;
    for (const auto& line : out.lines) {
      shortLines = shortLines && line.size() < 1000;
      prefixed = prefixed && line.rfind("projection callers: counts test", 0) == 0;
      for (size_t pos = line.find("GetProjectionMatrix"); pos != std::string::npos; pos = line.find("GetProjectionMatrix", pos + 1)) ++items;
    }
    check(out.lines.size() > 1 && shortLines && prefixed && items == ProjectionCallers::kCapacity && out.contains("table full, not recorded x3"),
          "a summary of a full table is several lines, each short and each headed, carrying every entry and the overflow count");
  }
  // ---- summaries -----------------------------------------------------------------------------------------------------------------------------------------------
  {
    ProjectionCallers census(build332841());
    Capture capture;capture.second = at(kCameraSetter);Lines sink;
    census.note(ProjectionCallers::Raw, 0, at(kEyeFov), CullProbe::Off, 1000, 1, capture, sink);
    census.note(ProjectionCallers::Raw, 0, at(kEyeFov), CullProbe::Off, 1001, 1, capture, sink);
    census.note(ProjectionCallers::Raw, 1, at(kSkyFov), CullProbe::Off, 1002, 1, capture, sink);
    Lines out;
    census.summary("by hand", out);
    check(out.lines.size() == 1 && out.lines[0].rfind("projection callers: counts by hand:", 0) == 0 &&
          out.contains("GetProjectionRaw exe+0x4E2FA5 <- exe+0x2878E1B eye 0 x1;") && out.contains("GetProjectionRaw exe+0x4E2FA5 eye 0 x1;") &&
          out.contains("GetProjectionRaw exe+0x4E3C93 <- ") ,
          "a summary lists each entry as method, frames, eye and its count");
    Lines again;
    census.summary("by hand", again);
    check(again.lines.size() == 1 && again.lines[0] == "projection callers: counts by hand: none;", "...counts are the change since the last summary: the next one says none");
    census.note(ProjectionCallers::Raw, 0, at(kEyeFov), CullProbe::Off, 1003, 1, capture, sink);
    Lines third;
    census.summary("by hand", third);
    check(third.contains("GetProjectionRaw exe+0x4E2FA5 eye 0 x1;") && !third.contains("exe+0x2878E1B"), "...and counting resumes from zero");
  }
  {
    // The clock: a first summary 30 s after the first call, then every 5 minutes.
    ProjectionCallers census(build332841());
    Capture capture;Lines sink;
    census.note(ProjectionCallers::Raw, 0, at(kEyeFov), CullProbe::Off, 5000, 1, capture, sink);
    const size_t first = sink.lines.size();
    census.note(ProjectionCallers::Raw, 0, at(kEyeFov), CullProbe::Off, 34999, 1, capture, sink);
    check(sink.lines.size() == first, "no summary before 30 s");
    census.note(ProjectionCallers::Raw, 0, at(kEyeFov), CullProbe::Off, 35000, 1, capture, sink);
    check(sink.lines.size() == first + 1 && sink.lines.back().rfind("projection callers: counts at 30 s:", 0) == 0, "the first summary is at 30 s");
    census.note(ProjectionCallers::Raw, 0, at(kEyeFov), CullProbe::Off, 334999, 1, capture, sink);
    check(sink.lines.size() == first + 1, "...the next is 5 minutes after it, not before");
    census.note(ProjectionCallers::Raw, 0, at(kEyeFov), CullProbe::Off, 335000, 1, capture, sink);
    check(sink.lines.size() == first + 2 && sink.lines.back().rfind("projection callers: counts every 5 min:", 0) == 0, "...then every 5 minutes");
  }
  {
    // Many threads counting the same callers lose nothing and never deadlock.
    ProjectionCallers census(build332841());
    std::atomic<unsigned> captures{0};
    std::atomic<unsigned> logged{0};
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 4; ++t)
      threads.emplace_back([&, t] {
        for (unsigned i = 0; i < 5000; ++i)
          census.note(ProjectionCallers::Raw, t & 1, at(i % 2 ? kEyeFov : kSkyFov), CullProbe::Off, 1, t,
                      [&](ProjectionCallers::Frames& f) { ++captures; f.second = at(kCameraSetter); },
                      [&](const char*) { ++logged; });
      });
    for (auto& thread : threads) thread.join();
    uint64_t total = 0;
    ProjectionCallers::Entry entries[ProjectionCallers::kCapacity];
    const unsigned n = census.snapshot(entries, ProjectionCallers::kCapacity);
    for (unsigned i = 0; i < n; ++i) total += entries[i].count;
    check(total == 20000 && census.overflowTotal() == 0, "four threads counting the same two callers lose no count");
  }
  // ---- what the host decides from the ini ------------------------------------------------------------------------------------------------------------
  {
    char line[96];
    check(cullProbeStatus(CullProbe::Off, false, true) == CullProbeStatus::Off && cullProbeStatus(CullProbe::Off, true, false) == CullProbeStatus::Off &&
          cullProbeStatus(CullProbe::Camera, true, true) == CullProbeStatus::Ignored && cullProbeStatus(CullProbe::All, true, false) == CullProbeStatus::Ignored &&
          cullProbeStatus(CullProbe::Sky, false, false) == CullProbeStatus::StoodDown && cullProbeStatus(CullProbe::Other, false, true) == CullProbeStatus::Active,
          "the probe acts only with no cull guard configured and only on build 332841; a guard wins over a wrong build; off is off");
    check(std::strcmp(cullProbeLine(CullProbeStatus::Active, CullProbe::Camera, line), "cull probe: answering camera callers wide, everyone else the truth") == 0 &&
          std::strcmp(cullProbeLine(CullProbeStatus::Active, CullProbe::Sizes, line), "cull probe: answering sizes callers wide, everyone else the truth") == 0 &&
          std::strcmp(cullProbeLine(CullProbeStatus::Off, CullProbe::Off, line), "cull probe: off") == 0 &&
          std::strcmp(cullProbeLine(CullProbeStatus::Ignored, CullProbe::All, line), "cull probe: ignored while the cull guard runs") == 0 &&
          std::strcmp(cullProbeLine(CullProbeStatus::StoodDown, CullProbe::All, line), "cull probe: standing down -- not build 332841") == 0,
          "the four log lines");
    check(isBuild332841(build332841()) && !isBuild332841(ExeModule{kBase, 104894464u, 1788384821u, 104894464u}) && !isBuild332841(ExeModule{kBase, 104894464u, 1788384820u, 104894465u}) &&
          !isBuild332841(ExeModule{}) && !isBuild332841(ExeModule{0, 0, 1788384820u, 104894464u}),
          "build 332841 is the PE stamp 1788384820 with image size 104894464, both; a stamp or a size one off, or no image, is not it");
    using namespace launch_fixture;
    Fixture f;auto& h = f.host;
    // On this rig's own executable (not the game's) the probe stands down and the census still stands.
    h.featureFrame.cullProbe = 2;h.featureFrame.cullMode = 0;
    h.publishCullProbe();
    check(h.cullProbeNote.noted && h.cullProbeNote.status == CullProbeStatus::StoodDown && h.geometry.read().cullProbe == 0u,
          "on another build the probe stands down: nothing is published to the game's reads");
    h.systemInterface.callers().useModule(build332841());
    h.publishCullProbe();
    check(h.cullProbeNote.status == CullProbeStatus::Active && h.geometry.read().cullProbe == 2u, "on build 332841, with no guard, camera is published as group 2");
    Capture capture;Lines sink;
    h.systemInterface.callers().note(ProjectionCallers::Raw, 0, at(kEyeFov), CullProbe::Off, 1, 1, capture, sink);
    h.publishCullProbe();
    check(countOf(h.systemInterface.callers(), ProjectionCallers::Raw, 0, kEyeFov, unknown) == 1 && h.geometry.read().cullProbe == 2u,
          "...an unchanged probe publishes again and summarises nothing: the count stands");
    h.featureFrame.cullMode = 2;
    h.publishCullProbe();
    check(h.cullProbeNote.status == CullProbeStatus::Ignored && h.geometry.read().cullProbe == 0u, "with a cull guard configured the probe is ignored, and nothing reaches the game's reads");
    check(countOf(h.systemInterface.callers(), ProjectionCallers::Raw, 0, kEyeFov, unknown) == 0 && h.systemInterface.callers().used() == 1,
          "...a change of probe summarises what was counted under the old one, and the counts start again");
    h.featureFrame.cullMode = 0;h.featureFrame.cullProbe = 5;
    h.publishCullProbe();
    check(h.cullProbeNote.status == CullProbeStatus::Active && h.cullProbeNote.requested == CullProbe::Sizes && h.geometry.read().cullProbe == 5u, "back to no guard, and sizes is group 5");
    h.featureFrame.cullProbe = 99;
    h.publishCullProbe();
    check(h.cullProbeNote.status == CullProbeStatus::Off && h.geometry.read().cullProbe == 0u, "a probe code outside 0..6 is off");
  }
}
} // namespace edvr::openxr::test
