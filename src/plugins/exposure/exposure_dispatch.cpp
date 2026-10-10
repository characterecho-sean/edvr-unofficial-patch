#include "exposure_dispatch.h"

#include <iterator>

#include "../../common/config.h"
#include "../../common/log.h"
#include "../../d3d11/binding_shadow.h"
#include "../../d3d11/shader_registry.h"
#include "exposure_shape.h"

namespace edvr {
namespace {

using plugins::exposure::ExposureDispatchObserverState;

BindSlot uavSlot(uint32_t i) {
    return static_cast<BindSlot>(static_cast<uint32_t>(BindSlot::CsUav0) + i);
}

bool isExposureDispatch(ExposureDispatchObserverState* s) {
    if (!s->enabled || s->rejected) return false;

    const uint64_t h = lookupShaderHash(bindingGet(BindSlot::Cs));
    if (h == 0) return false;
    if (s->targetHash != 0) return h == s->targetHash;
    if (s->pinned) return false;   // pinned but not matching: do nothing

    auto it = s->shapeVerdict.find(h);
    if (it != s->shapeVerdict.end()) return it->second;

    const bool match = plugins::exposure::shapeLooksLikeExposure();
    s->everExamined.insert(h);
    s->shapeVerdict[h] = match;
    if (match) {
        Log::get().note("exposure fix: candidate compute shader %016llX matches the "
                        "exposure-state shape; confirming across frames",
                        static_cast<unsigned long long>(h));
    }
    return match;
}

}  // namespace

ExposureDispatchTicket exposurePluginBeginDispatch(void* state) {
    ExposureDispatchTicket ticket{};
    if (!state) return ticket;
    ticket.target = isExposureDispatch(
        static_cast<ExposureDispatchObserverState*>(state)) ? 1u : 0u;
    return ticket;
}

void exposurePluginCompleteDispatch(void* state, ExposureDispatchTicket ticket,
                                    ID3D11DeviceContext* context) {
    if (!state || !ticket.target) return;
    auto* s = static_cast<ExposureDispatchObserverState*>(state);
    ++s->seenThisFrame;
    if (s->seenThisFrame == 1) {
        for (uint32_t i = 0; i < 4; ++i) {
            s->firstEye[i] = static_cast<ID3D11UnorderedAccessView*>(
                bindingGet(uavSlot(i)));
        }
    } else if (s->seenThisFrame == 2) {
        // Running exactly twice a frame means one dispatch per eye. A shader
        // that merely has the right resource shape but runs once, or five
        // times, is something else. Detection waits for a few consecutive
        // frames of that before touching anything; a pinned hash is trusted
        // immediately.
        if (!s->pinned &&
            s->detectStreak < plugins::exposure::kExposureConfirmFrames) return;

        ID3D11UnorderedAccessView* second[4];
        for (uint32_t i = 0; i < 4; ++i) {
            second[i] = static_cast<ID3D11UnorderedAccessView*>(
                bindingGet(uavSlot(i)));
        }
        if (!s->announced) {
            s->announced = true;
            // The key is advanced.exposure_shader. It said fix.b1_exposure_cs,
            // which is this repo's predecessor's name for it and is read by
            // nothing here -- so anyone following the instruction was
            // silently ignored, on the support path where it matters most.
            Log::get().note("exposure fix: confirmed compute shader %016llX runs "
                            "once per eye. Pin it with exposure_shader under "
                            "[advanced] in %s if you want to skip detection.",
                            static_cast<unsigned long long>(
                                lookupShaderHash(bindingGet(BindSlot::Cs))),
                            Config::get().iniName());
        }
        exposureDispatchApplyPair(state, context, s->firstEye, second);
    }
}

void exposurePluginResetDispatchFrame(void* state) {
    if (!state) return;
    auto* s = static_cast<ExposureDispatchObserverState*>(state);
    // Exactly two dispatches means one per eye. Anything else breaks the streak,
    // so a shader that only sometimes runs twice never gets promoted.
    if (s->seenThisFrame == 2) {
        if (s->detectStreak < plugins::exposure::kExposureConfirmFrames)
            ++s->detectStreak;
    } else if (s->seenThisFrame != 0) {
        s->detectStreak = 0;
    }
    s->seenThisFrame = 0;
    for (uint32_t i = 0; i < 4; ++i) s->firstEye[i] = nullptr;
}

void exposurePluginExpireDispatchVerdicts(void* state) {
    if (!state) return;
    auto* s = static_cast<ExposureDispatchObserverState*>(state);
    // Drop the NO answers while detection is still looking.
    //
    // shapeVerdict was written once per shader and never revisited, so the real
    // exposure pass being probed once in a transient binding state -- the first
    // dispatch after a clear, say -- blacklisted it for the whole session. The
    // fix then never engaged, and the give-up notice went on to report that the
    // game is stock, which is a different and wrong thing.
    //
    // Yes answers are kept: those are confirmed across frames anyway, and a
    // shader that matched the shape once does not stop having matched it.
    if (!s->announced && !s->gaveUpNotice && s->targetHash == 0) {
        for (auto it = s->shapeVerdict.begin(); it != s->shapeVerdict.end();) {
            it = it->second ? std::next(it) : s->shapeVerdict.erase(it);
        }
    }
}

}  // namespace edvr
