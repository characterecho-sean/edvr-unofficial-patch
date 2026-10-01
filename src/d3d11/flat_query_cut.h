// Questions the flat runtime used to put to the D3D immediate context, and now answers from what it already knows.
//
// Every Get* on the immediate context is a call the driver, or a graphics wrapper laid over the context (ReShade), must
// take in full, and the flat path made them per draw: the depth view behind the bound target (coverage classification,
// once per scene draw), the actual vertex and pixel shaders (the same, for the draws whose projection needs no patch),
// and engine motion's wrapper read the game's render targets, its blend state and the runtime's acceptance of MRT6
// each time it bound its substitution. On foot in a hangar that was thousands of calls a frame that only ever said
// what the runtime had been told already: every hooked setter records what it set (binding_shadow.h), and a state
// EDVR itself restored is the state it read.
//
// The answer from what is already known is a SHORTCUT, and a shortcut nothing ever checks is a guess, so a small
// share of them are checked. One frame in 64 puts up to four questions per state to the context as well and
// compares the two answers:
//   - the answers agree: nothing changes, the count of checks goes up;
//   - they differ: the shortcut is wrong on this rig (a setter path nothing hooks, a wrapper that rebinds under us).
//     The check's own answer is used for that question, the mismatch is counted, and that state asks the context
//     again for the rest of the session, with one log line that names it.
//
// This is the policy only, so a rig can drive it, and can instantiate it with the mistakes it must notice. It lives
// on the render thread and is not synchronised.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {

enum class FlatQuery : uint8_t {
    CoverageDepth = 0,   // the depth view coverage classification asks the context for (OMGetRenderTargets, depth only)
    ShaderIdentity,      // the actual vertex and pixel shaders of a draw whose projection needs no patch
    GameBlend,           // the game's blend state, factor and mask, when engine motion derives its own
    GameTargets,         // the game's render-target set engine motion adds MRT6 to
    TargetsKept,         // the read-back that says the runtime took the set with MRT6 in it
    Count
};
constexpr unsigned kFlatQueryCount = static_cast<unsigned>(FlatQuery::Count);

inline const char* flatQueryName(FlatQuery q) {
    switch (q) {
    case FlatQuery::CoverageDepth: return "coverage depth view";
    case FlatQuery::ShaderIdentity: return "coverage shader identity";
    case FlatQuery::GameBlend: return "game blend state";
    case FlatQuery::GameTargets: return "game render targets";
    case FlatQuery::TargetsKept: return "MRT6 read-back";
    default: return "?";
    }
}

enum class FlatQueryPlan : uint8_t {
    Shortcut,   // answer from what is known
    Ask,        // ask the context: this state fell back
    Sample      // ask the context AND answer from what is known, then compare (compared())
};

// A window's worth, as the census takes them.
struct FlatQueryCounts {
    uint64_t served[kFlatQueryCount] = {};       // answered from what is known
    uint64_t sampled[kFlatQueryCount] = {};      // asked of the context as well, and compared
    uint64_t asked[kFlatQueryCount] = {};        // asked of the context because the state fell back
    uint64_t mismatched[kFlatQueryCount] = {};   // compared, and the two answers differed
    uint32_t fellBack = 0;                       // bit per state: it asks the context for the rest of the session
};

// Nothing wrong. The rig instantiates the policy with each of these in turn, to see that its checks notice.
struct FlatQueryNoFaults {
    static constexpr bool neverSample = false,          // no question is ever put to the context
                          noFallback = false,           // a mismatch does not send the state back to asking
                          shortcutWhenFellBack = false, // a state that fell back is still answered from what is known
                          forgetMismatch = false;       // a mismatch is not counted
};

template <class Faults = FlatQueryNoFaults>
class FlatQueryCutT {
public:
    static constexpr unsigned kFramePeriod = 64;    // one frame in this many checks
    static constexpr unsigned kSamplesPerState = 4; // and puts this many questions per state to the context

    // Once per frame, at the Present, with the number of the frame that starts now.
    void beginFrame(uint64_t frame) noexcept {
        checking_ = !Faults::neverSample && frame % kFramePeriod == 0;
        for (Cell& c : cells_) c.left = checking_ ? kSamplesPerState : 0;
    }
    bool checkingThisFrame() const noexcept { return checking_; }

    // How to answer one question.
    FlatQueryPlan plan(FlatQuery q) noexcept {
        Cell& c = cells_[static_cast<unsigned>(q)];
        if (c.fellBack && !Faults::shortcutWhenFellBack) { ++c.asked; return FlatQueryPlan::Ask; }
        if (c.left) { --c.left; ++c.sampled; return FlatQueryPlan::Sample; }
        ++c.served;
        return FlatQueryPlan::Shortcut;
    }

    // Both answers of a sample are in: true exactly once per state, the first time they differed, so the caller
    // says it in the log once.
    bool compared(FlatQuery q, bool agree) noexcept {
        Cell& c = cells_[static_cast<unsigned>(q)];
        if (agree) return false;
        if (!Faults::forgetMismatch) ++c.mismatched;
        if (Faults::noFallback) return false;
        const bool first = !c.fellBack;
        c.fellBack = true;
        return first;
    }

    bool fellBack(FlatQuery q) const noexcept { return cells_[static_cast<unsigned>(q)].fellBack; }
    // For a rig, and for a diagnostic that wants every question asked.
    void fallBackAll() noexcept { for (Cell& c : cells_) c.fellBack = true; }
    void reset() noexcept { *this = FlatQueryCutT{}; }

    // The window's counts since the last take, zeroed; the fell-back bits stay (they are the session's).
    FlatQueryCounts take() noexcept {
        FlatQueryCounts out;
        for (unsigned i = 0; i < kFlatQueryCount; ++i) {
            Cell& c = cells_[i];
            out.served[i] = c.served;
            out.sampled[i] = c.sampled;
            out.asked[i] = c.asked;
            out.mismatched[i] = c.mismatched;
            if (c.fellBack) out.fellBack |= 1u << i;
            c.served = c.sampled = c.asked = c.mismatched = 0;
        }
        return out;
    }

private:
    struct Cell {
        uint64_t served = 0, sampled = 0, asked = 0, mismatched = 0;
        unsigned left = 0;
        bool fellBack = false;
    };
    Cell cells_[kFlatQueryCount];
    bool checking_ = false;
};
using FlatQueryCut = FlatQueryCutT<>;

// One instance for the flat runtime and the engine motion wrapper it drives, both on the render thread.
inline FlatQueryCut g_flatQueryCut;
inline FlatQueryCut& flatQueryCut() noexcept { return g_flatQueryCut; }

// The log line for the first mismatch of a state.
inline int flatQueryFallbackLine(char* out, size_t size, FlatQuery q, const char* detail) {
    return std::snprintf(out, size,
                         "flat query shortcut: the %s the runtime tracks disagreed with the context (%s). The check's answer was used, "
                         "and this session asks the context for it from now on.",
                         flatQueryName(q), detail && *detail ? detail : "no detail");
}

}  // namespace edvr
