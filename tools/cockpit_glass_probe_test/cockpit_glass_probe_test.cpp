// TEMPORARY, with the probe: src\d3d11\cockpit_glass_probe_core.h held to its contract. The canopy hashes are the ones
// tools\effects_arc.py extracts for CockpitGlass_Forward; this holds the table's shape and the formatting a log reader depends on.
#include "../../src/d3d11/cockpit_glass_probe_core.h"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace edvr::glass_probe;

static int g_failures = 0;
static void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

static uint32_t bitsOf(float f) {
    uint32_t b;
    std::memcpy(&b, &f, sizeof b);
    return b;
}

int main() {
    // The table: the flown pair is where the port starts, every hash is distinct, and nothing else matches.
    check(canopyVsIndex(0x8C091FFD08644E02ull) == 0, "the flown canopy VS is the intact one");
    check(canopyPsIndex(0x4E4FF61E8A08FC7Eull) == 2, "the flown canopy PS is intact q3");
    check(canopyPsIndex(0x8C091FFD08644E02ull) == -1, "a VS hash is not a canopy PS");
    check(canopyVsIndex(0) == -1 && canopyPsIndex(0) == -1, "an unbound shader is not the canopy");
    for (int i = 0; i < kCanopyPsCount; ++i)
        for (int j = 0; j < kCanopyVsCount; ++j) check(kCanopyPs[i].hash != kCanopyVs[j].hash, "PS and VS hashes distinct");
    for (int i = 0; i < kCanopyPsCount; ++i)
        for (int j = i + 1; j < kCanopyPsCount; ++j) check(kCanopyPs[i].hash != kCanopyPs[j].hash, "PS hashes distinct");

    // Every logged register is inside the copied range.
    for (const RegSpan& s : kCb1Spans) check(s.count > 0 && s.first + s.count <= kCb1Regs, "a cb1 span inside the copy");

    // Components: floats as %.6g, a denormal-patterned value as the integer it is.
    char t[32];
    formatComponent(bitsOf(1.0f), t, sizeof t);
    check(std::strcmp(t, "1") == 0, "1.0f prints 1");
    formatComponent(bitsOf(-0.5f), t, sizeof t);
    check(std::strcmp(t, "-0.5") == 0, "-0.5f prints -0.5");
    formatComponent(3u, t, sizeof t);
    check(std::strcmp(t, "#3") == 0, "integer 3 prints #3");
    formatComponent(0u, t, sizeof t);
    check(std::strcmp(t, "0") == 0, "zero prints 0");

    // Registers: a full one, and one past the end that says so instead of reading as zeros.
    uint8_t data[32] = {};
    const float r0[4] = {1.0f, 2.0f, 0.25f, 0.0f};
    std::memcpy(data, r0, sizeof r0);
    const uint32_t two = 2;
    std::memcpy(data + 16 + 4, &two, 4);
    char line[256];
    formatRegs(data, sizeof data, 0, 3, line, sizeof line);
    check(std::strcmp(line, "[0] 1 2 0.25 0 [1] 0 #2 0 0 [2] -") == 0, "registers print, a short buffer reads short");
    formatRegs(data, sizeof data, 0, 2, line, 12);
    check(std::strlen(line) == 11, "a small output buffer truncates, terminated");
    formatRegs(nullptr, 0, 290, 1, line, sizeof line);
    check(std::strcmp(line, "[290] -") == 0, "no data reads as missing");

    // The key and the sampling rule.
    check(probeKeyOn("on") && !probeKeyOn("off") && !probeKeyOn("") && !probeKeyOn(nullptr) && !probeKeyOn("1"), "only on arms it");
    check(sampleDue(false, 0, 12, 5, 0, 30000), "the first canopy draw is sampled at once");
    check(!sampleDue(false, 1, 12, 20000, 0, 30000), "not again inside the interval");
    check(sampleDue(false, 1, 12, 30000, 0, 30000), "again once the interval passes");
    check(!sampleDue(true, 1, 12, 90000, 0, 30000), "never while a readback is out");
    check(!sampleDue(false, 12, 12, 900000, 0, 30000), "never past the cap");

    // The light lists. The layout read from the game's shaders fits the stride with no gap: nine u32 counts, then each list straight after the last.
    check(kLightLists[0].listAt == 36, "the lists start after nine u32 counts");
    for (int i = 0; i < kLightListCount; ++i) {
        check(kLightLists[i].countAt == i * 4, "the counts run in order");
        const uint32_t end = kLightLists[i].listAt + kLightLists[i].capacity * 4u;
        check(end == (i + 1 < kLightListCount ? kLightLists[i + 1].listAt : kClusterStride), "each list ends where the next begins, and the last at the stride");
    }
    check(std::strcmp(kLightLists[kListPoint].name, "point") == 0 && std::strcmp(kLightLists[kListSpot].name, "spot") == 0, "the indices name the point and spot lists");
    check(kLightLists[7].countAt == 28 && kLightLists[7].listAt == 2212, "the probe list is where the canopy's shader reads it");
    bool spansRecords = false, spansGrid = false;
    for (const RegSpan& s : kCb1Spans) {
        spansRecords = spansRecords || (s.first <= 249 && s.first + s.count >= 255);
        spansGrid = spansGrid || (s.first <= 227 && s.first + s.count >= 229);
    }
    check(spansRecords && spansGrid, "the log carries the record offsets and the grid");

    check(clustersHeld(2, 2, 3, kClusterStride * 12) == 12 && clustersHeld(2, 2, 3, kClusterStride * 5 + 7) == 5 && clustersHeld(0, 2, 3, kClusterStride * 5) == 0,
          "the clusters held are the grid's, clipped to the bytes");
    check(sliceOfDepth(1.0f, 2.0f, 5.0f, 16) == 5 && sliceOfDepth(1e-6f, 2.0f, 5.0f, 16) == 0 && sliceOfDepth(1e30f, 2.0f, 5.0f, 16) == 15, "a depth's slice, clamped to the grid");
    check(sliceOfDepth(0.0f, 2.0f, 5.0f, 16) == -1 && sliceOfDepth(1.0f, 2.0f, 5.0f, 0) == -1, "no depth or no slices is no slice");

    // A 2x2x3 grid: spot 7 in two clusters of slice 1, spot 9 in slice 2, a count past capacity in slice 0, an id past the table.
    std::vector<uint8_t> t0(kClusterStride * 12, 0);
    const auto put = [](std::vector<uint8_t>& b, size_t at, uint32_t v) { std::memcpy(b.data() + at, &v, 4); };
    const LightList& spot = kLightLists[kListSpot];
    const size_t c4 = kClusterStride * 4u, c5 = kClusterStride * 5u, c8 = kClusterStride * 8u, c0 = 0;
    put(t0, c4 + spot.countAt, 1);
    put(t0, c4 + spot.listAt, 7);
    put(t0, c5 + spot.countAt, 2);
    put(t0, c5 + spot.listAt, 7);
    put(t0, c5 + spot.listAt + 4, 9);
    put(t0, c8 + spot.countAt, 1);
    put(t0, c8 + spot.listAt, kIdSpace + 1);
    put(t0, c0 + spot.countAt, spot.capacity + 5u);
    uint32_t wanted[4] = {};
    uint32_t wantedCount = 99;
    ListCensus sc = censusList(t0.data(), t0.size(), 2, 2, 3, spot, 1, 1, wanted, 4, &wantedCount);
    check(sc.clusters == 4 && sc.overCapacity == 1 && sc.idsPast == 1, "clusters holding any, one past capacity, one id past the table");
    check(sc.ids == spot.capacity + 4u && sc.most == spot.capacity, "a count past capacity is read as the capacity");
    check(sc.bySlice[0] == 1 && sc.bySlice[1] == 2 && sc.bySlice[2] == 1, "clusters by slice");
    check(wantedCount == 2 && wanted[0] == 7 && wanted[1] == 9, "the ids at the glass's depths, each once, in the order seen");
    check(sc.distinct == 3, "distinct lights: 7, 9 and the 0 ids of the overfull cluster");
    wantedCount = 99;
    sc = censusList(t0.data(), t0.size(), 2, 2, 3, spot, 2, 2, wanted, 1, &wantedCount);
    check(wantedCount == 0, "slice 2 held only an id past the table, which is not printed");
    ListCensus empty = censusList(t0.data(), t0.size(), 2, 2, 3, kLightLists[kListPoint], 0, 2, wanted, 4, &wantedCount);
    check(empty.clusters == 0 && empty.ids == 0 && empty.distinct == 0 && wantedCount == 0, "an empty list reads as zeros");
    empty = censusList(nullptr, 0, 2, 2, 3, spot, 0, 2, wanted, 4, &wantedCount);
    check(empty.clusters == 0 && wantedCount == 0, "no data reads as zeros");
    ListCensus clipped = censusList(t0.data(), kClusterStride * 5u, 2, 2, 3, spot, 0, 2, nullptr, 0, nullptr);
    check(clipped.bySlice[1] == 1 && clipped.bySlice[2] == 0, "a short copy stops at the bytes it holds, and no output for the ids is needed");

    // The records, as the shaders unpack them.
    std::vector<uint8_t> t2(4096, 0);
    const auto putF = [](std::vector<uint8_t>& b, size_t at, float v) { std::memcpy(b.data() + at, &v, 4); };
    const uint32_t spotA = 1024, spotB = 2048, id = 3;
    const size_t sa = spotA + (id << 5), sb = spotB + (id << 5);
    putF(t2, sa, 1.5f);
    putF(t2, sa + 4, -2.0f);
    putF(t2, sa + 8, 3.0f);
    putF(t2, sa + 24, 40.0f);
    putF(t2, sb, 1000.0f);
    put(t2, sb + 4, 1023u | (0u << 10) | (511u << 20));
    putF(t2, sb + 8, 0.25f);
    put(t2, sb + 12, 32768u | (65535u << 16));
    put(t2, sb + 20, 0u | (5u << 16));
    LightRecord r;
    check(readSpot(t2.data(), t2.size(), spotA, spotB, id, &r), "a spot record inside the buffer reads");
    check(r.x == 1.5f && r.y == -2.0f && r.z == 3.0f && r.radius == 40.0f && r.intensity == 1000.0f && r.sourceRadius == 0.25f, "the spot's position, radius, intensity and source radius");
    check(r.r == 1.0f && r.g == 0.0f && r.b > 0.49f && r.b < 0.51f, "the colour is 10:10:10 over 1023");
    check(r.cosOuter > 0.49f && r.cosOuter < 0.51f && r.cosInner == 1.0f, "the cone is two 16-bit fractions, outer in the low half");
    check(r.shadow == -1 && r.cookie == 4, "0 is no shadow; an index is stored plus one");
    check(!readSpot(t2.data(), t2.size(), spotA, spotB, 200, &r) && !readSpot(nullptr, 0, 0, 0, 0, &r), "a record past the buffer, or no buffer, is refused");
    const uint32_t pointA = 512, pointB = 768;
    putF(t2, pointA + (id << 4) + 12, 12.0f);
    putF(t2, pointB + (id << 4), 77.0f);
    put(t2, pointB + (id << 4) + 12, 0xFFFFFFFFu);
    LightRecord pr;
    check(readPoint(t2.data(), t2.size(), pointA, pointB, id, &pr) && pr.radius == 12.0f && pr.intensity == 77.0f && pr.shadow == -1, "a point record: radius in .w, a signed shadow index with -1 none");
    check(!readPoint(t2.data(), t2.size(), pointA, pointB, 4000, &pr), "a point record past the buffer is refused");

    std::printf("cockpit glass probe test: %s\n", g_failures ? "FAILED" : "passed");
    return g_failures ? 1 : 0;
}
