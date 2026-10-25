// F111 checks (native on-foot VR): the panel pointer instrument's statistics and verdict (native_on_foot_f111.h). Header only, run by native_frame_test (run111).
// Each case drives a mouse path (up, then right, then back) through a modelled pick: the NDC (x right, y down), the direction the game unprojects into the pick's
// camera frame (x right, y up, z forward), the ray hit on a panel 0.5 m ahead and its uv, the sprite, and the OS cursor; one stage at a time is turned the
// other way, and the verdict must name that stage, as the instrument's 1 s line would.
#pragma once

#include "../../src/d3d11/native_on_foot_f111.h"

#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace f111test {

namespace p = edvr::nofoot::f111;

struct Model {
    bool unprojectFlip = false;   // K1: the direction's x and y against the NDC
    bool behind = false;          // K5: the direction's z negated (no hit; the sprite is the direction)
    bool uvFlip = false;          // K2: the hit's uv against the NDC
    bool windowMode = false;      // the 2D mode: no ray, the window's uv
    bool windowFlip = false;      // K3: the window's uv against the NDC
    bool osFlip = false;          // K4: the NDC against the OS cursor
};

inline p::Cycle cycleAt(const Model& m, float x, float y) {
    p::Cycle c;
    c.open = false;
    c.ndc[0] = x;
    c.ndc[1] = y;
    c.source = 1;
    const double tx = 0.8, ty = 0.9;   // the frustum's half-tangents
    double d[3] = {x * tx, -y * ty, 1.0};
    if (m.unprojectFlip) {
        d[0] = -d[0];
        d[1] = -d[1];
    }
    if (m.behind) d[2] = -d[2];
    const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (int i = 0; i < 3; ++i) c.dir[i] = static_cast<float>(d[i] / len);
    if (m.windowMode) {
        c.placements = 1;
        c.windowCalls = 1;
        c.win[0] = m.windowFlip ? 0.5f - 0.5f * x : 0.5f + 0.5f * x;
        c.win[1] = m.windowFlip ? 0.5f - 0.5f * y : 0.5f + 0.5f * y;
        c.winUv = true;
        c.hit = true;
        c.pos[0] = static_cast<float>(x * 0.3);   // the window's point on a panel 0.5 m ahead
        c.pos[1] = static_cast<float>(-y * 0.3);
        c.pos[2] = 0.5f;
    } else if (c.dir[2] > 0.0f) {   // the ray hits the panel at z = 0.5
        c.placements = 2;
        c.rayCalls = 2;
        c.rayHits = 1;
        const float t = 0.5f / c.dir[2];
        c.t = t;
        for (int i = 0; i < 3; ++i) c.pos[i] = c.dir[i] * t;
        c.u = m.uvFlip ? 0.5f - 0.5f * x : 0.5f + 0.5f * x;
        c.v = m.uvFlip ? 0.5f - 0.5f * y : 0.5f + 0.5f * y;
        c.hitUv = true;
        c.hit = true;
    } else {   // t < 0 is rejected: the sprite stays at the direction
        c.placements = 2;
        c.rayCalls = 2;
        for (int i = 0; i < 3; ++i) c.pos[i] = c.dir[i];
    }
    const double sx = m.osFlip ? -1.0 : 1.0;
    c.os[0] = static_cast<int32_t>(std::lround(960.0 + sx * x * 400.0));
    c.os[1] = static_cast<int32_t>(std::lround(540.0 + sx * y * 400.0));
    c.osOk = true;
    return c;
}

// The pilot's path: still, up 0.4, right 0.4, back down and left, a few frames each.
inline p::Sec run(const Model& m, bool still = false) {
    std::vector<std::pair<float, float>> path;
    float x = 0.0f, y = 0.0f;
    for (int i = 0; i < 4; ++i) path.push_back({x, y});
    if (!still) {
        for (int i = 0; i < 8; ++i) path.push_back({x, y -= 0.05f});   // up: the NDC's y falls
        for (int i = 0; i < 8; ++i) path.push_back({x += 0.05f, y});   // right
        for (int i = 0; i < 8; ++i) path.push_back({x -= 0.025f, y += 0.025f});
    }
    p::Sec s;
    p::Cycle prev;
    bool have = false;
    for (const auto& pt : path) {
        const p::Cycle c = cycleAt(m, pt.first, pt.second);
        p::addCycle(s, c);
        if (have) p::addStep(s, prev, c);
        prev = c;
        have = true;
    }
    return s;
}

template <class Check>
inline void run111(Check&& check) {
    {
        const p::Sec s = run(Model{});
        check(p::verdict(s) == p::Stage::kNone && s.moves == 24 && s.ndcDir[0].sign() > 0 && s.ndcDir[1].sign() > 0 && s.ndcSeen[0].sign() > 0 &&
                  s.ndcSeen[1].sign() > 0 && s.ndcUv[0].sign() > 0 && s.ndcUv[1].sign() > 0 && s.osNdc[0].sign() > 0 && s.osNdc[1].sign() > 0 &&
                  std::fabs(s.slope(0) - 0.8) < 1e-3 && std::fabs(s.slope(1) - 0.9) < 1e-3 && s.behind == 0 && s.rayHits == 28,
              "F111: a sane pick (the direction, the hit, the uv and the OS cursor all follow the NDC) reads 'none in the pick'; the slopes are the frustum's "
              "half-tangents 0.8 and 0.9");
    }
    {
        Model m;
        m.unprojectFlip = true;
        const p::Sec s = run(m);
        check(p::verdict(s) == p::Stage::kUnproject && s.ndcDir[0].sign() < 0 && s.ndcDir[1].sign() < 0 && s.slope(0) < 0.0 && s.slope(1) < 0.0,
              "F111: K1, the direction's x and y against the NDC: 'the unprojection', with negative slopes");
    }
    {
        Model m;
        m.behind = true;
        const p::Sec s = run(m);
        check(p::verdict(s) == p::Stage::kBehind && s.behind == s.cycles && s.rayHits == 0 && s.ndcDir[0].sign() > 0 && s.ndcDir[1].sign() > 0 &&
                  s.ndcSeen[0].sign() < 0 && s.ndcSeen[1].sign() < 0 && s.spriteBehind == s.cycles,
              "F111: K5, the direction behind the camera: no hit, and the sprite as the camera sees it moves against the mouse on both axes (a point "
              "reflection) while the direction's x and y follow it");
    }
    {
        Model m;
        m.uvFlip = true;
        const p::Sec s = run(m);
        check(p::verdict(s) == p::Stage::kHit && s.ndcDir[0].sign() > 0 && s.ndcUv[0].sign() < 0 && s.ndcUv[1].sign() < 0,
              "F111: K2, the hit's uv against the NDC with the direction right: 'the hit or the sprite'");
    }
    {
        Model m;
        m.windowMode = true;
        const p::Sec ok = run(m);
        m.windowFlip = true;
        const p::Sec s = run(m);
        check(p::verdict(ok) == p::Stage::kNone && ok.rayCalls == 0 && ok.windowCalls == 28 && p::verdict(s) == p::Stage::kWindow,
              "F111: the 2D mode: the window's uv following the NDC reads 'none', against it K3 'the cursor window'");
    }
    {
        Model m;
        m.osFlip = true;
        m.unprojectFlip = true;   // an earlier stage is named first
        const p::Sec s = run(m);
        check(p::verdict(s) == p::Stage::kMouse && s.osNdc[0].sign() < 0 && s.osNdc[1].sign() < 0,
              "F111: K4, the NDC against the OS cursor, is named before a later stage that also inverts");
    }
    {
        const p::Sec s = run(Model{}, true);
        p::Sec merged;
        merged.merge(s);
        merged.merge(run(Model{}));
        check(p::verdict(s) == p::Stage::kFewMoves && s.moves == 0 && s.cycles == 4 && p::verdict(merged) == p::Stage::kNone && merged.moves == 24,
              "F111: a still cursor reads 'too few moves'; merged seconds keep their moves (the 10 s line)");
    }
    {
        p::Corr c;
        c.add(1.0, 0.1);
        c.add(1.0, -0.1);
        check(c.sign() == 0 && c.r() == 0.0, "F111: two pairs give no sign (three needed)");
        c.add(1.0, 0.0);
        check(c.sign() == 0, "F111: no correlation, no sign");
    }
    {
        const uint8_t movField[8] = {0x48, 0x8B, 0x81, 0x30, 0x01, 0x00, 0x00, 0xC3};   // mov rax,[rcx+0x130]; ret
        const uint8_t leaShort[8] = {0x48, 0x8D, 0x41, 0x18, 0xC3, 0xCC, 0xCC, 0xCC};  // lea rax,[rcx+0x18]; ret
        const uint8_t self[8] = {0x48, 0x8B, 0xC1, 0xC3, 0xCC, 0xCC, 0xCC, 0xCC};      // mov rax,rcx; ret
        const uint8_t call[8] = {0x48, 0x83, 0xEC, 0x28, 0xE8, 0x00, 0x00, 0x00};      // sub rsp,0x28; call ...
        const uint8_t negShort[8] = {0x48, 0x8B, 0x41, 0xF8, 0xC3, 0, 0, 0};            // mov rax,[rcx-8]; ret
        int32_t d1 = 0, d2 = 0, d3 = 1, d4 = 0, d5 = 0;
        bool r1 = false, r2 = true, r3 = true, r4 = false, r5 = false;
        check(p::decodeAccessor(movField, 8, &d1, &r1) && d1 == 0x130 && r1 && p::decodeAccessor(leaShort, 8, &d2, &r2) && d2 == 0x18 && !r2 &&
                  p::decodeAccessor(self, 8, &d3, &r3) && d3 == 0 && !r3 && !p::decodeAccessor(call, 8, &d4, &r4) &&
                  p::decodeAccessor(negShort, 8, &d5, &r5) && d5 == -8 && r5,
              "F111: the camera getters are followed only as plain accessors (mov or lea from rcx, ret); a getter that calls is refused");
    }
}

}  // namespace f111test
