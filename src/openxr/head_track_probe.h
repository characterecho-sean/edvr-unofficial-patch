// F87h (native on-foot VR, read only): the headset's tracking as the runtime sees it, one second at a time. The F86 flight's head dropped 0.846 -> 0.744 m in three
// seconds with no recentre and no space-change event; whether the headset's own tracking moved (a SteamVR floor or origin shift) or the head did is told by the head in
// the natural space (STAGE on SteamVR) beside where the runtime's LOCAL space sits in it. Samples only; nothing is changed.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace edvr {
namespace headtrack {

struct Probe {
    uint32_t posTracked = 0, posN = 0, oriTracked = 0, oriN = 0;
    double view[3] = {}, pitchDeg = 0.0, local[3] = {}, localYawDeg = 0.0, localAtLine[3] = {}, maxDy = 0.0, lastViewY = 0.0;
    bool haveView = false, haveLocal = false, haveLocalAtLine = false, haveY = false;
    // One VIEW-in-natural-space sample (a frame's poll).
    void noteView(bool posOk, bool oriOk, const double pos[3], double pitch) {
        ++posN;
        ++oriN;
        if (posOk) ++posTracked;
        if (oriOk) ++oriTracked;
        for (int i = 0; i < 3; ++i) view[i] = pos[i];
        pitchDeg = pitch;
        haveView = true;
        if (haveY) maxDy = std::max(maxDy, std::fabs(pos[1] - lastViewY));
        lastViewY = pos[1];
        haveY = true;
    }
    // One LOCAL-in-natural-space sample.
    void noteLocal(const double pos[3], double yawDeg) {
        for (int i = 0; i < 3; ++i) local[i] = pos[i];
        localYawDeg = yawDeg;
        haveLocal = true;
    }
    // How far LOCAL's origin moved in the natural space since the last line (0 until two lines have a sample).
    double localMovedMm() const {
        if (!haveLocal || !haveLocalAtLine) return 0.0;
        const double d[3] = {local[0] - localAtLine[0], local[1] - localAtLine[1], local[2] - localAtLine[2]};
        return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) * 1000.0;
    }
    // After a line: the counters and the largest step restart; the last positions stay.
    void lineDone() {
        posTracked = posN = oriTracked = oriN = 0;
        maxDy = 0.0;
        if (haveLocal) {
            for (int i = 0; i < 3; ++i) localAtLine[i] = local[i];
            haveLocalAtLine = true;
        }
    }
};

}  // namespace headtrack
}  // namespace edvr
