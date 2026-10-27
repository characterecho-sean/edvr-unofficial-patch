// F122 checks (native on-foot VR): the on-foot panel mouse cursor hidden by default (native_on_foot_f122.h). Header only, run by native_frame_test (run122).
// A controller as a block of memory with the fields the placement leaves (the mouse NDC at +0x84, the hit byte at +0x100, the sprite's position at +0x140),
// and the hide's one write against it: the sprite goes out of sight, and every other byte, the NDC and the hit byte among them, stays.
#pragma once

#include "../../src/d3d11/native_on_foot_f122.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace f122test {

namespace h = edvr::nofoot::f122;
namespace p = edvr::nofoot::f111;

template <class Check>
inline void run122(Check&& check) {
    {   // the key
        check(h::keyOn("on") && !h::keyOn("off") && !h::keyOn("") && !h::keyOn("ON") && !h::keyOn("true") && !h::keyOn(nullptr),
              "F122: the key is on only for the word on; off, anything else and no value hide the cursor");
    }
    {   // what a placement does
        check(h::decide(false, false, true) == h::Act::kHide && h::decide(false, false, false) == h::Act::kNoHit && h::decide(true, false, true) == h::Act::kLeave &&
                  h::decide(true, false, false) == h::Act::kNoHit && h::decide(false, true, true) == h::Act::kFlat && h::decide(true, true, true) == h::Act::kLeave,
              "F122: off in VR hides a sprite the placement set (a hit); no hit leaves it; on leaves the game's cursor (F117 and F120 place it); the flat profile never hides");
    }
    {   // the hidden position cannot be seen under any of the draw's reads; the cursor's usual positions can
        const float typical[3] = {-0.02f, 0.05f, 0.65f}, behindAxis[3] = {0.0f, 0.0f, -1.0f}, farAhead[3] = {1.0e5f, 1.0e5f, 1.0f};
        check(h::unseen(h::kHidden) && !h::unseen(typical) && !h::unseen(behindAxis) && !h::unseen(farAhead) && h::kHidden[2] < 0.0f,
              "F122: the hidden position is behind the pick camera and 1e5 times further off its axis than it is behind; a cursor on the panel, a point behind on the axis "
              "and a point far to the side but ahead are not taken for unseen");
    }
    {   // the one write, against a controller's memory
        std::vector<uint8_t> mem(0x200);
        for (size_t i = 0; i < mem.size(); ++i) mem[i] = static_cast<uint8_t>(i * 7u + 3u);
        const float ndc[2] = {0.25f, -0.4f}, pos[3] = {-0.02f, 0.05f, 0.65f};
        std::memcpy(&mem[p::kOffMouse], ndc, sizeof(ndc));
        std::memcpy(&mem[p::kOffPos], pos, sizeof(pos));
        mem[p::kOffHit] = 1;
        const std::vector<uint8_t> before = mem;
        const uintptr_t ctl = reinterpret_cast<uintptr_t>(mem.data());
        std::vector<uintptr_t> at;
        const bool ok = h::writeHidden(ctl, [&](uintptr_t address, float v) {
            at.push_back(address - ctl);
            std::memcpy(&mem[address - ctl], &v, sizeof(v));
            return true;
        });
        float now[3];
        std::memcpy(now, &mem[p::kOffPos], sizeof(now));
        size_t changed = 0, outside = 0;
        for (size_t i = 0; i < mem.size(); ++i)
            if (mem[i] != before[i]) {
                ++changed;
                if (i < p::kOffPos || i >= p::kOffPos + 12) ++outside;
            }
        float ndcNow[2];
        std::memcpy(ndcNow, &mem[p::kOffMouse], sizeof(ndcNow));
        check(ok && at.size() == 3 && at[0] == p::kOffPos + 8 && at[1] == p::kOffPos && at[2] == p::kOffPos + 4 && now[0] == h::kHidden[0] && now[1] == h::kHidden[1] &&
                  now[2] == h::kHidden[2] && h::unseen(now) && changed > 0 && outside == 0 && mem[p::kOffHit] == 1 && ndcNow[0] == ndc[0] && ndcNow[1] == ndc[1],
              "F122: the hide writes three floats at the sprite's position (z first, then x and y) and no other byte: the hit byte and the mouse NDC are as they were, "
              "so the clicks land where the game's pointer is");
    }
    {   // a refused write stops the hide, and is counted as a failure, not a hide
        std::vector<uintptr_t> at;
        const bool ok = h::writeHidden(0x1000, [&](uintptr_t address, float) {
            at.push_back(address);
            return at.size() < 2;
        });
        const h::Delta d = h::delta(h::Act::kHide, ok);
        check(!ok && at.size() == 2 && at[0] == 0x1000 + p::kOffPos + 8 && d.seen == 1 && d.hidden == 0 && d.failed == 1,
              "F122: a refused write ends the hide after the sprite's z (already behind the camera) and counts a failure, not a hidden sprite");
    }
    {   // the counts
        const h::Delta none = h::delta(h::Act::kNoHit, true), left = h::delta(h::Act::kLeave, true), flat = h::delta(h::Act::kFlat, true), hid = h::delta(h::Act::kHide, true);
        check(none.seen == 0 && none.hidden == 0 && none.failed == 0 && left.seen == 1 && left.hidden == 0 && flat.seen == 1 && flat.hidden == 0 && hid.seen == 1 &&
                  hid.hidden == 1 && hid.failed == 0,
              "F122: a placement with no hit counts nothing; a panel reached counts as seen; only a hide whose write held counts as hidden");
    }
    {   // the hooks the key opens
        const h::Gates offVr = h::gates(true, false, false), onVr = h::gates(true, true, false), offFlat = h::gates(true, false, true), onFlat = h::gates(true, true, true),
                       closed = h::gates(false, false, false), closedOn = h::gates(false, true, false);
        check(!offVr.instruments && offVr.placement && onVr.instruments && onVr.placement && !offFlat.instruments && !offFlat.placement && onFlat.instruments &&
                  onFlat.placement && !closed.instruments && !closed.placement && !closedOn.instruments && !closedOn.placement,
              "F122: off in VR opens the placement hook alone (the instruments and the corrections stay idle); on opens all; off in flat opens none; off the foot none");
    }
}

}  // namespace f122test
