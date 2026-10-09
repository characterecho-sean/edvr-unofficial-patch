#pragma once

// The terrain-culling arc's projection-caller census and selective-lie probe
// (docs\terrain-culling.md). TEMPORARY: both go when the arc closes.
//
// Elite asks the runtime for its projection from six call sites in three
// wrapper functions, and static analysis of build 332841 cannot say which one
// builds the terrain culler's frustum. EDVR implements GetProjectionRaw itself,
// so it can see who is asking (the census) and answer chosen callers with the
// per-axis symmetric superset while everyone else gets the truth (the probe).
//
// Pure bookkeeping, no runtime state beyond a fixed table: nothing here
// allocates, waits on anything but a few-instruction spin flag, or logs under
// that flag. The stack capture and the log sink are passed in, so a rig drives
// every case.
#include "projection_math.h"
#include <intrin.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr::openxr {

// advanced.cull_probe, in the numbering EdvrNativeFrameOutput::cullProbe carries.
enum class CullProbe : uint32_t { Off = 0, All = 1, Camera = 2, Ui = 3, Sky = 4, Sizes = 5, Other = 6 };
constexpr uint32_t kCullProbeCodes = 7;
inline CullProbe cullProbeFromCode(uint32_t code) {
  return code < kCullProbeCodes ? static_cast<CullProbe>(code) : CullProbe::Off;
}
inline const char* cullProbeName(CullProbe probe) {
  switch (probe) {
    case CullProbe::All: return "all";
    case CullProbe::Camera: return "camera";
    case CullProbe::Ui: return "ui";
    case CullProbe::Sky: return "sky";
    case CullProbe::Sizes: return "sizes";
    case CullProbe::Other: return "other";
    default: return "off";
  }
}

// What advanced.cull_probe comes to: it only acts with no cull guard configured and
// only on build 332841, because its groups are that build's return addresses.
enum class CullProbeStatus { Off, Ignored, StoodDown, Active };
inline CullProbeStatus cullProbeStatus(CullProbe requested, bool guardConfigured, bool build332841) {
  if (requested == CullProbe::Off) return CullProbeStatus::Off;
  if (guardConfigured) return CullProbeStatus::Ignored;
  return build332841 ? CullProbeStatus::Active : CullProbeStatus::StoodDown;
}
// The one log line a status change makes.
inline const char* cullProbeLine(CullProbeStatus status, CullProbe requested, char (&buffer)[96]) {
  switch (status) {
    case CullProbeStatus::Active:
      std::snprintf(buffer, sizeof(buffer), "cull probe: answering %s callers wide, everyone else the truth", cullProbeName(requested));
      return buffer;
    case CullProbeStatus::Ignored: return "cull probe: ignored while the cull guard runs";
    case CullProbeStatus::StoodDown: return "cull probe: standing down -- not build 332841";
    default: return "cull probe: off";
  }
}

// The game's executable, as its own PE headers say. No shared helper exists:
// every build-keyed hook in this repo re-reads the pair from its own copy of
// the constants (explorer_cam_core.h, transition_flash_eye_base.cpp, ...).
struct ExeModule {
  uintptr_t base = 0, size = 0;
  uint32_t stamp = 0, imageSize = 0;
};
// Build 332841: EliteDangerous64.exe, PE TimeDateStamp and SizeOfImage.
constexpr uint32_t kBuild332841Stamp = 1788384820u;
constexpr uint32_t kBuild332841ImageSize = 104894464u;
inline bool isBuild332841(const ExeModule& module) {
  return module.base && module.stamp == kBuild332841Stamp && module.imageSize == kBuild332841ImageSize;
}
// This process's executable (openvr_system.cpp: the Windows headers stay out of
// this one, whose `near` macro would break the rigs that include it).
ExeModule readExeModule() noexcept;

// A return address as an RVA in the game's executable. 0 is "not captured".
constexpr uint32_t kFrameOutside = 0xFFFFFFFFu, kFrameUnknown = 0xFFFFFFFEu;
inline uint32_t frameRva(const ExeModule& module, uintptr_t address) {
  if (!address) return kFrameUnknown;
  if (!module.base || address < module.base || address - module.base >= module.size) return kFrameOutside;
  return static_cast<uint32_t>(address - module.base);
}

// The six GetProjectionRaw call sites named by the 2026-10-09 disassembly of
// build 332841 (analysis\decomp\verify_20261009_cull2_*), as the return RVAs
// the census sees as frame 1, and the two callers of the first site that frame 2
// tells apart.
namespace cull_rva {
constexpr uint32_t kEyeFov = 0x4E2FA5;       // wrapper slot 25 (0x4E2F50)
constexpr uint32_t kSkyFov = 0x4E3C93;       // wrapper slot 28 (via 0x4E3C50)
constexpr uint32_t kSizes[4] = {0x4E42FA, 0x4E4351, 0x4E43A6, 0x4E43F4};   // wrapper slot 24 (0x4E4270)
constexpr uint32_t kCameraSetter = 0x2878E1B;   // frame 2 of the eye camera
constexpr uint32_t kUiScale = 0x8D269A;         // frame 2 of the UI scale
}

enum class CallerGroup { Camera, Ui, Sky, Sizes, Other };
inline CallerGroup classifyProjectionRawCaller(uint32_t rva1, uint32_t rva2) {
  if (rva1 == cull_rva::kEyeFov) {
    if (rva2 == cull_rva::kCameraSetter) return CallerGroup::Camera;
    if (rva2 == cull_rva::kUiScale) return CallerGroup::Ui;
    return CallerGroup::Other;
  }
  if (rva1 == cull_rva::kSkyFov) return CallerGroup::Sky;
  for (const uint32_t size : cull_rva::kSizes) if (rva1 == size) return CallerGroup::Sizes;
  return CallerGroup::Other;
}
// Does the probe answer this GetProjectionRaw caller wide?
inline bool probeSelects(CullProbe probe, uint32_t rva1, uint32_t rva2) {
  switch (probe) {
    case CullProbe::Off: return false;
    case CullProbe::All: return true;
    case CullProbe::Camera: return classifyProjectionRawCaller(rva1, rva2) == CallerGroup::Camera;
    case CullProbe::Ui: return classifyProjectionRawCaller(rva1, rva2) == CallerGroup::Ui;
    case CullProbe::Sky: return classifyProjectionRawCaller(rva1, rva2) == CallerGroup::Sky;
    case CullProbe::Sizes: return classifyProjectionRawCaller(rva1, rva2) == CallerGroup::Sizes;
    case CullProbe::Other: return classifyProjectionRawCaller(rva1, rva2) == CallerGroup::Other;
  }
  return false;
}
// Only the first call site is split by its caller, so only it needs a stack
// capture on every call, and only while a probe group that splits it is on.
inline bool probeNeedsFrame2(CullProbe probe, uint32_t rva1) {
  return rva1 == cull_rva::kEyeFov &&
         (probe == CullProbe::Camera || probe == CullProbe::Ui || probe == CullProbe::Other);
}
// The per-axis symmetric superset of a raw frustum: l' = -max(|l|,|r|),
// r' = +max(|l|,|r|), t' = -max(|t|,|b|), b' = +max(|t|,|b|). 0 - x keeps a zero +0.
inline RawFov widenedRaw(const RawFov& raw) {
  const float horizontal = std::fmax(std::fabs(raw.left), std::fabs(raw.right));
  const float vertical = std::fmax(std::fabs(raw.top), std::fabs(raw.bottom));
  return {0.0f - horizontal, horizontal, 0.0f - vertical, vertical};
}

// Who calls GetProjectionRaw, GetProjectionMatrix and GetEyeToHeadTransform, in a
// fixed table. Frame 1 is the game's own return address (the vtable call lands
// directly in OpenVRSystem, no adapter in between); frames 2 and 3 come from a
// stack capture taken on the first sight of a new frame 1, and whenever the
// probe needs frame 2, never on every call otherwise. Counts are kept per
// (method, frame 1, frame 2, eye) and reported as the change since the last
// summary; a call that could not be recorded for want of room is counted as an
// overflow and nothing is ever allocated.
class ProjectionCallers {
 public:
  enum Method : uint8_t { Raw = 0, Matrix = 1, EyeToHead = 2 };
  static constexpr unsigned kCapacity = 128;
  static constexpr uint64_t kFirstSummaryMs = 30000, kSummaryIntervalMs = 300000;
  struct Frames { uintptr_t second = 0, third = 0; };
  struct Entry {
    uint64_t count = 0;
    uint32_t rva1 = 0, rva2 = 0;
    uint8_t method = 0, eye = 0;
  };

  explicit ProjectionCallers(const ExeModule& module = readExeModule()) : module_(module) {}
  ProjectionCallers(const ProjectionCallers&) = delete;
  ProjectionCallers& operator=(const ProjectionCallers&) = delete;

  const ExeModule& module() const { return module_; }
  // For a rig, before any call: the executable the RVAs are relative to.
  void useModule(const ExeModule& module) { module_ = module; }

  static const char* methodName(unsigned method) {
    return method == Raw ? "GetProjectionRaw" : method == Matrix ? "GetProjectionMatrix" : "GetEyeToHeadTransform";
  }

  // Record one call. `capture(Frames&)` fills frames 2 and 3 (0 where it cannot),
  // `sink(const char*)` takes a finished log line. Returns whether `probe`
  // answers this caller wide (only GetProjectionRaw is ever answered).
  template <class Capture, class Sink>
  bool note(Method method, unsigned eyeIndex, uintptr_t frame1, CullProbe probe, uint64_t nowMs,
            uint32_t tid, Capture&& capture, Sink&& sink) {
    const uint8_t eye = eyeIndex > 1 ? uint8_t(255) : static_cast<uint8_t>(eyeIndex);
    const uint32_t rva1 = frameRva(module_, frame1);
    const bool needsFrame2 = method == Raw && probeNeedsFrame2(probe, rva1);
    bool attempt = needsFrame2;
    if (!attempt) {
      // First sight of this frame 1, and room to remember it.
      Guard guard(lock_);
      attempt = !hasFrame1(method, rva1) && used_ < kCapacity;
    }
    Frames frames{};
    if (attempt) capture(frames);
    const uint32_t rva2 = frameRva(module_, frames.second), rva3 = frameRva(module_, frames.third);
    bool first = false;
    {
      Guard guard(lock_);
      Entry* entry = find(method, eye, rva1, rva2);
      if (!entry) {
        if (used_ < kCapacity) {
          first = attempt && !hasTriple(method, rva1, rva2);
          entry = &entries_[used_++];
          *entry = Entry{0, rva1, rva2, static_cast<uint8_t>(method), eye};
        } else {
          ++overflowWindow_;
          ++overflowTotal_;
        }
      }
      if (entry) ++entry->count;
    }
    if (first) {
      char line[256];
      char a[24], b[24], c[24];
      std::snprintf(line, sizeof(line), "projection callers: %s %s <- %s <- %s eye %u tid %lu", methodName(method),
                    frameText(a, rva1), frameText(b, rva2), frameText(c, rva3), unsigned(eye),
                    static_cast<unsigned long>(tid));
      sink(line);
    }
    uint64_t due = nextSummaryMs_.load(std::memory_order_relaxed);
    if (!due) {
      uint64_t expected = 0;
      nextSummaryMs_.compare_exchange_strong(expected, nowMs + kFirstSummaryMs);
    } else if (nowMs >= due && nextSummaryMs_.compare_exchange_strong(due, nowMs + kSummaryIntervalMs)) {
      summary(summaries_.fetch_add(1, std::memory_order_relaxed) == 0 ? "at 30 s" : "every 5 min", sink);
    }
    return method == Raw && probeSelects(probe, rva1, rva2);
  }

  // The counts since the last summary, as log lines (a window that saw nothing
  // says so). Called on the clock above and by the host on a probe change.
  template <class Sink>
  void summary(const char* reason, Sink&& sink) {
    Entry window[kCapacity];
    unsigned n = 0;
    uint64_t overflow = 0;
    {
      Guard guard(lock_);
      for (unsigned i = 0; i < used_; ++i) {
        if (entries_[i].count) window[n++] = entries_[i];
        entries_[i].count = 0;
      }
      overflow = overflowWindow_;
      overflowWindow_ = 0;
    }
    char line[1000];
    const auto header = [&](bool more) {
      return std::snprintf(line, sizeof(line), "projection callers: counts %s%s:", reason, more ? " (more)" : "");
    };
    int used = header(false);
    const auto append = [&](const char* item) {
      const int length = static_cast<int>(std::strlen(item));
      if (used + length + 2 > 900) {
        sink(line);
        used = header(true);
      }
      used += std::snprintf(line + used, sizeof(line) - size_t(used), " %s;", item);
    };
    for (unsigned i = 0; i < n; ++i) {
      char item[160], a[24], b[24];
      const Entry& e = window[i];
      if (e.rva2 == kFrameUnknown)
        std::snprintf(item, sizeof(item), "%s %s eye %u x%llu", methodName(e.method), frameText(a, e.rva1),
                      unsigned(e.eye), static_cast<unsigned long long>(e.count));
      else
        std::snprintf(item, sizeof(item), "%s %s <- %s eye %u x%llu", methodName(e.method), frameText(a, e.rva1),
                      frameText(b, e.rva2), unsigned(e.eye), static_cast<unsigned long long>(e.count));
      append(item);
    }
    if (overflow) {
      char item[64];
      std::snprintf(item, sizeof(item), "table full, not recorded x%llu", static_cast<unsigned long long>(overflow));
      append(item);
    }
    if (!n && !overflow) append("none");
    sink(line);
  }

  // Rig and host views of the table.
  unsigned used() const { Guard guard(lock_); return used_; }
  uint64_t overflowTotal() const { Guard guard(lock_); return overflowTotal_; }
  unsigned snapshot(Entry* out, unsigned capacity) const {
    Guard guard(lock_);
    unsigned n = 0;
    for (unsigned i = 0; i < used_ && n < capacity; ++i) out[n++] = entries_[i];
    return n;
  }

  static const char* frameText(char (&buffer)[24], uint32_t rva) {
    if (rva == kFrameOutside) std::snprintf(buffer, sizeof(buffer), "outside");
    else if (rva == kFrameUnknown) std::snprintf(buffer, sizeof(buffer), "?");
    else std::snprintf(buffer, sizeof(buffer), "exe+0x%X", unsigned(rva));
    return buffer;
  }

 private:
  struct Guard {
    explicit Guard(std::atomic_flag& flag) : flag_(flag) {
      while (flag_.test_and_set(std::memory_order_acquire)) _mm_pause();
    }
    ~Guard() { flag_.clear(std::memory_order_release); }
    std::atomic_flag& flag_;
  };
  bool hasFrame1(unsigned method, uint32_t rva1) const {
    for (unsigned i = 0; i < used_; ++i) if (entries_[i].method == method && entries_[i].rva1 == rva1) return true;
    return false;
  }
  bool hasTriple(unsigned method, uint32_t rva1, uint32_t rva2) const {
    for (unsigned i = 0; i < used_; ++i)
      if (entries_[i].method == method && entries_[i].rva1 == rva1 && entries_[i].rva2 == rva2) return true;
    return false;
  }
  Entry* find(unsigned method, uint8_t eye, uint32_t rva1, uint32_t rva2) {
    for (unsigned i = 0; i < used_; ++i) {
      Entry& e = entries_[i];
      if (e.method == method && e.eye == eye && e.rva1 == rva1 && e.rva2 == rva2) return &e;
    }
    return nullptr;
  }

  ExeModule module_;
  mutable std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
  Entry entries_[kCapacity]{};
  unsigned used_ = 0;
  uint64_t overflowWindow_ = 0, overflowTotal_ = 0;
  std::atomic<uint64_t> nextSummaryMs_{0};
  std::atomic<unsigned> summaries_{0};
};

}  // namespace edvr::openxr
