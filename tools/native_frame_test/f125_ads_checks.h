#pragma once
#include "../../src/d3d11/native_on_foot_ads_log.h"
#include "../../src/d3d11/native_on_foot_f93.h"
#include <string>
#include <vector>

namespace f125adstest {
template<class Check> void run125Ads(Check check) {
    using namespace edvr::nofoot;
    logwin::setDetail(false);
    {
        adslog::Window w;
        std::vector<bool> oldModes;
        std::vector<uint32_t> spans;
        auto emit = [&](uint32_t n) noexcept { oldModes.push_back(w.detailed); spans.push_back(n); };
        for (uint64_t t = 100; t <= 9000; t += 100) w.poll(t, true, 1, []() noexcept {}, emit);
        logwin::setDetail(true);
        w.poll(9100, true, 1, []() noexcept {}, emit);
        logwin::setDetail(false);
        w.poll(9600, true, 1, []() noexcept {}, emit);
        check(oldModes == std::vector<bool>{false, true}, "F125 ADS: detail changes emit the old window's mode before adopting the new mode");
        check(spans == std::vector<uint32_t>{9, 1}, "F125 ADS: nine default seconds cannot become a one-second detail sample on a live toggle");
    }
    {
        adslog::Window w;
        std::vector<uint32_t> spans, items;
        std::vector<double> peaks;
        uint32_t lastItem = 0;
        double peak = 0.0;
        auto emit = [&](uint32_t n) noexcept { spans.push_back(n); items.push_back(lastItem); peaks.push_back(peak); peak = 0.0; };
        w.poll(100, true, 7, []() noexcept {}, emit);
        lastItem = 7; peak = 1.0;   // first observed frame, before any full second
        w.poll(200, true, 8, []() noexcept {}, emit);
        lastItem = 8; peak = 0.0;
        w.poll(300, false, 8, []() noexcept {}, emit);
        check(spans == std::vector<uint32_t>{1, 1} && items == std::vector<uint32_t>{7, 8}, "F125 ADS: an item change before the first second flushes the first observed item's metadata");
        check(peaks == std::vector<double>{1.0, 0.0}, "F125 ADS: a zero-request release cannot inherit the preceding partial window's maximum");
    }
    {
        adslog::Window w;
        adslog::HoldStates states;
        std::vector<uint32_t> spans, holds;
        int folds = 0;
        const char* state = "holding";
        auto fold = [&]() noexcept { ++folds; states.fold(state); };
        auto emit = [&](uint32_t n) noexcept { spans.push_back(n); holds.push_back(states.n[0]); states.reset(); };
        for (uint64_t t = 100; t <= 20100; t += 100) {
            // Ten holding seconds, then two frozen seconds, a held-item change
            // midway through a second, three holding seconds and an exit.
            const bool live = t < 15700;
            const uint64_t item = t < 12600 ? 7 : 8;
            w.poll(t, live, item, fold, emit);
            state = t < 10100 || t >= 12600 ? "holding" : "frozen (ADS)";
        }
        check(spans == std::vector<uint32_t>{10, 3, 4}, "F125 ADS: default ten-second window, partial item flush and partial exit flush");
        check(holds == std::vector<uint32_t>{10, 0, 4}, "F125 ADS: the old item's frozen seconds cannot be labelled by the new item's state");
        check(folds == 17, "F125 ADS: every full or final partial second is folded once, with no off-foot repeats");
    }
    {
        adslog::Window w; unsigned folds = 0, emitted = 0, seconds = 0;
        auto fold = [&]() noexcept { ++folds; };
        auto emit = [&](uint32_t n) noexcept { ++emitted; seconds += n; };
        w.poll(5000, true, 1, fold, emit);
        w.poll(6000, true, 1, fold, emit);
        logwin::setDetail(true);
        w.poll(6500, true, 1, fold, emit);
        w.poll(7500, true, 1, fold, emit);
        w.poll(8500, true, 1, fold, emit);
        w.poll(9000, false, 1, fold, emit);
        check(folds == 5 && emitted == 4 && seconds == 5, "F125 ADS: changing detail flushes the summary first; detail keeps one-second rows and the release tail");
        w.poll(10000, false, 1, fold, emit);
        check(emitted == 4, "F125 ADS: an inactive lifecycle cannot emit an old ADS window twice");
        logwin::setDetail(false);
    }
    {
        adslog::OffsetStates s;
        s.fold(true, false, 0.2, false);
        s.fold(true, true, 1.0, true);
        s.fold(false, false, 0.0, false);
        char text[96]; s.weightText(text, sizeof(text));
        check(s.armed == 2 && s.ads == 1 && s.high == 1 && s.moved == 1, "F125 ADS: armed, ADS, full-weight and moved seconds are independent counts");
        check(s.lo == 0.2 && s.hi == 1.0 && std::string(text) == "0.20,1.00", "F125 ADS: individual armed-second weights retain the global median and extrema");
        s.reset(); s.weightText(text, sizeof(text));
        check(s.armed == 0 && s.hi == 0.0 && text[0] == 0, "F125 ADS: release clears pooled weight evidence before the next context");
    }
    {
        f93::ChainSecond pooled, one;
        double ss = 0.0; unsigned steps = 0;
        for (unsigned frame = 0; frame < 900; ++frame) {
            const double move = frame % 7 * 0.0001;
            pooled.addSteps(move, move * 2, move * 3);
            one.addSteps(move, move * 2, move * 3);
            pooled.addAlong(true, frame % 3 * 0.00001);
            one.addAlong(true, frame % 3 * 0.00001);
            if (frame % 90 == 89) { ss += one.lockSs; steps += one.steps; one.reset(); }
        }
        check(pooled.steps == steps && std::fabs(pooled.lockSs - ss) < 1e-15, "F125 ADS: pooling diagnostic frames retains the weighted sum of squared one-second lock steps");
        check(pooled.acN == 897 && pooled.alongN == one.alongN, "F125 ADS: the chain's per-frame autocorrelation history survives the one-second reset unchanged");
    }
}
}
