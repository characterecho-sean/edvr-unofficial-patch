// The supercruise draws in the HDR layer, on the real census and the real dump (docs/ui-layer-2026-09-23.md, "2026-10-07: orbit lines,
// supercruise bars and space dust in the layer"). Pure C++, no device: ui_layer_math.h's family rule and decision, and the layer's seed rules,
// driven by the rows of a field census and the pixels of a field dump, both kept in tools/supercruise_census_test/fixtures (made by
// tools/layer_fixtures.py from the Frontier install's edvr_gfx_20261007_054519.log, census 1 at 05:48:01, frame 1, and eyes\eye_054804_*):
//
//   THE CENSUS TUPLE. Frame 1 of that census holds 244 draw rows, every one into a 2016x1949 target: 120 into the R10G10B10A2 g-buffer, 110 into
//   the lit R11G11B10 HDR target, 8 into the 8-bit post-tonemap target. Three of them, two eyes each, are the supercruise draws: rows #145/#194
//   (vs 9BFC7FD232328391 / ps DBF1725726018F52, the space dust: 1800 vertices, additive ONE/ONE, depth GEQUAL no write, stencil ALWAYS/REPLACE
//   under mask 0x40), the supercruise bars (vs A47A3315FFF5E2E4 / ps 869FFF43E875906E) and the orbit lines (vs C7FA0C0F5DD49180 / ps
//   6EEF165A350DA30F). The rows' own columns (ds, st, so, bm, bl, the target ids) are turned into the facts the DLL gathers, and the family rule
//   and the decision run on each: the dust rows are named the space dust and are kRedirect; one-token mutants of the row -- the pixel shader the
//   flat route pairs the vertex shader with, the blend operation MIN, an 8-bit target, a depth state the layer cannot reproduce, a late tonemap --
//   are each refused.
//   THE DIFFERENTIAL. Every one of the 244 rows through the family rule and decision as they were on main (a frozen copy of the rule) and as they
//   are: exactly the six supercruise rows change decision, from "not UI" to redirected, and the other 238 are decided as before; against the rule
//   with the orbit lines and the bars but not the dust, only the two dust rows change. The frozen copies are held to the production rule where
//   they must agree (the whole frame, with the dust included).
//   THE SEEDS. The layer's depth-stencil seed rules (stale after a game write that is not a preserved stencil-only one, short of depth or stencil
//   bits, ui_layer.cpp beginInner and uiLayerNoteOther, with ui_layer_math.h's uiLayerSeedWriterInvalidates) replayed over the frame's rows per
//   eye, with the families main takes and with the three more: the seeds a frame needs before and after, on a GPU with the pixel shader's stencil
//   reference (one pass copies depth and every stencil bit) and without (one pass per bit asked). Printed; the flight's 30 s line counts the
//   real ones.
//   THE DUST PIXELS (eyes\eye_054804, SceneZ and the C/D crops of the first dumped frame): every one of the 2043 streak pixels lies on sky (stencil 5,
//   no occluder), about one percent within one render pixel of an occluder (the depth edges a layer drawn without the game's depth would put the
//   dust across); and the parity bound of the additive composite after the tonemap, T(F) + T(L) - T(F + L) <= T(F), over the 8-bit background of
//   4143 sampled dust pixels: the 90th percentile is at most 8 of 255. Mutants of both (the pixels moved onto the cockpit, a bright background)
//   are caught by the same assertions.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../../src/d3d11/ui_layer_math.h"

using namespace edvr;

static unsigned g_checks = 0;
static void check(bool ok, const char* why) {
    ++g_checks;
    if (!ok) throw std::runtime_error(why);
}

static std::string readFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return t;
}

// ---------------------------------------------------------------- the census rows
struct IdDesc {
    uint32_t w = 0, h = 0, fmt = 0, view = 0;
};
struct Row {
    uint32_t ordinal = 0, count = 0, instances = 0;
    char kind = 0;
    int rtv = -1, dsv = -1;
    uint64_t vs = 0, ps = 0;
    std::string ds, st, bm, bl, so;
};

static std::string tokenOf(const std::string& row, const std::string& key) {
    const std::string needle = " " + key + "=";
    const size_t at = row.find(needle);
    if (at == std::string::npos) return "";
    const size_t from = at + needle.size(), to = row.find(' ', from);
    return row.substr(from, to == std::string::npos ? std::string::npos : to - from);
}
static int idOf(const std::string& tok) { return tok.size() > 1 && tok[0] == '@' ? std::atoi(tok.c_str() + 1) : -1; }

struct Census {
    std::map<int, IdDesc> ids;
    std::vector<Row> rows;
};
static Census loadCensus(const char* path) {
    const std::string text = readFile(path);
    check(text.size() > 10000, "the census fixture is readable");
    Census c;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        if (line.compare(0, 4, "ID @") == 0) {
            int id = 0;
            IdDesc d;
            unsigned w = 0, h = 0, fmt = 0, vf = 0;
            check(std::sscanf(line.c_str(), "ID @%d tex %ux%u fmt=%u res=%*s vf=%u", &id, &w, &h, &fmt, &vf) == 5, "an ID line is 'ID @n tex WxH fmt=F res=.. vf=V'");
            d.w = w;
            d.h = h;
            d.fmt = fmt;
            d.view = vf;
            c.ids[id] = d;
        } else if (line.compare(0, 4, "ROW ") == 0) {
            const std::string row = " " + line.substr(4);   // a leading space: every token is " key=value"
            Row r;
            unsigned frame = 0, ordinal = 0;
            char kind = 0;
            check(std::sscanf(line.c_str(), "ROW DC %u #%u %c ", &frame, &ordinal, &kind) == 3, "a ROW line is 'ROW DC <frame> #<ordinal> <kind> ...'");
            r.ordinal = ordinal;
            r.kind = kind;
            r.count = static_cast<uint32_t>(std::strtoul(tokenOf(row, "n").c_str(), nullptr, 10));
            r.instances = static_cast<uint32_t>(std::strtoul(tokenOf(row, "i").c_str(), nullptr, 10));
            r.rtv = idOf(tokenOf(row, "r"));
            r.dsv = idOf(tokenOf(row, "d"));
            r.vs = std::strtoull(tokenOf(row, "vh").c_str(), nullptr, 16);
            r.ps = std::strtoull(tokenOf(row, "ph").c_str(), nullptr, 16);
            r.ds = tokenOf(row, "ds");
            r.st = tokenOf(row, "st");
            r.bm = tokenOf(row, "bm");
            r.bl = tokenOf(row, "bl");
            r.so = tokenOf(row, "so");
            // (a depth-only draw has no pixel shader: ph=0000000000000000)
            check(r.vs && !r.ds.empty() && !r.st.empty() && !r.bm.empty() && !r.bl.empty() && !r.so.empty() && r.rtv >= 0,
                  "every row names its vertex shader, its depth, stencil, mask, blend and stencil-op columns, and its target");
            c.rows.push_back(r);
        }
    }
    return c;
}

// The census's columns as the types ui_layer_math.h takes. ds=%c%uw%c: depth enabled, the function, write ALL (A) or none (Z); st=%c%u: stencil
// enabled and the reference; so=r%02X/w%02X/f%u/%u,%u,%u[+f%u/%u,%u,%u]: read mask, write mask, the front face's function and its three
// operations (fail, depth fail, pass), the back face's after a + only when it differs.
static UiDsState dsStateOf(const Row& r, const IdDesc* dsv) {
    UiDsState s;
    s.depthEnable = r.ds[0] == '1';
    s.depthFunc = static_cast<uint8_t>(r.ds[1] - '0');
    s.depthWriteAll = r.ds.back() == 'A';
    s.stencilEnable = r.st[0] == '1';
    unsigned rd = 0xFF, wr = 0xFF, f = 8, fail = 1, dfail = 1, pass = 1;
    char rest[64] = "";
    check(std::sscanf(r.so.c_str(), "r%x/w%x/f%u/%u,%u,%u%63s", &rd, &wr, &f, &fail, &dfail, &pass, rest) >= 6, "the stencil-op column is r../w../f./.,.,.");
    s.readMask = static_cast<uint8_t>(rd);
    s.writeMask = static_cast<uint8_t>(wr);
    s.front.func = static_cast<uint8_t>(f);
    s.front.fail = static_cast<uint8_t>(fail);
    s.front.depthFail = static_cast<uint8_t>(dfail);
    s.front.pass = static_cast<uint8_t>(pass);
    s.back = s.front;
    unsigned bf = 0, bfail = 0, bdfail = 0, bpass = 0;
    if (rest[0] == '+' && std::sscanf(rest, "+f%u/%u,%u,%u", &bf, &bfail, &bdfail, &bpass) == 4) {
        s.back.func = static_cast<uint8_t>(bf);
        s.back.fail = static_cast<uint8_t>(bfail);
        s.back.depthFail = static_cast<uint8_t>(bdfail);
        s.back.pass = static_cast<uint8_t>(bpass);
    }
    s.stencilPlane = !dsv || dsv->view == 20;   // D32_FLOAT_S8X24_UINT
    return s;
}
static UiBlendRt blendOf(const Row& r) {
    UiBlendRt b;
    char flag = 0;
    unsigned src = 0, dst = 0, op = 0, srcA = 0, dstA = 0, opA = 0;
    check(std::sscanf(r.bl.c_str(), "%1c%u,%u,%u/%u,%u,%u", &flag, &src, &dst, &op, &srcA, &dstA, &opA) == 7, "the blend column is <flag><src>,<dst>,<op>/<srcA>,<dstA>,<opA>");
    b.enable = flag == '1';
    b.src = static_cast<uint8_t>(src);
    b.dst = static_cast<uint8_t>(dst);
    b.op = static_cast<uint8_t>(op);
    b.srcA = static_cast<uint8_t>(srcA);
    b.dstA = static_cast<uint8_t>(dstA);
    b.opA = static_cast<uint8_t>(opA);
    b.mask = static_cast<uint8_t>(std::strtoul(r.bm.c_str(), nullptr, 16));
    return b;
}

// ---------------------------------------------------------------- the family rule as it stood on main, frozen
// uiLayerFamilyFor at b22f241f (the tree this work started from), with a selector for which of the three new families a copy knows: 0 none (main),
// 1 the orbit lines and the bars, 2 all three (which must equal the production rule on every row: the control that the copy is a copy).
static UiLayerFamily frozenFamilyFor(const UiFamilyFacts& f, int set) {
    UiLayerFamily out = UiLayerFamily::kNone;
    if (f.targetKind == 0) {
    } else if (f.targetKind == 1) {
        out = uiVsIsHoloPanel(f.vs)    ? UiLayerFamily::kHolo
              : f.vs == kUiVsFlightHud ? UiLayerFamily::kFlightHud
              : f.vs == kUiVsSprite    ? UiLayerFamily::kSprite
              : (set >= 1 && f.vs == kUiVsOrbitLines && f.ps == kUiPsOrbitLines)           ? UiLayerFamily::kOrbitLines
              : (set >= 1 && f.vs == kUiVsSupercruiseBars && f.ps == kUiPsSupercruiseBars) ? UiLayerFamily::kSupercruiseBars
              : (set >= 2 && f.vs == kUiVsSpaceDust && f.ps == kUiPsSpaceDust)             ? UiLayerFamily::kSpaceDust
              : (uiHoloGenericHash(f.vs) ||
                 (f.vs == kHoloTargetSphere && (f.ps == 0xEA02FAC2BD6C643Cull || f.ps == 0xE95634B0F61D218Full)))
                  ? UiLayerFamily::kHoloGeneric : UiLayerFamily::kNone;
    } else if (f.excluded) {
    } else if (f.panelSized) {
        out = UiLayerFamily::kScreen;
    } else if (f.learnedSurface) {
        out = f.vs == kUiVsPanel       ? UiLayerFamily::kPanel
              : f.vs == kUiVsLoader    ? UiLayerFamily::kLoader
              : uiVsIsHoloPanel(f.vs)  ? UiLayerFamily::kHolo
              : f.vs == kUiVsSprite    ? UiLayerFamily::kSprite
                                       : UiLayerFamily::kSurface;
    } else if (f.vs == kUiVsPanel && uiKnownPs(kUiPanelPs, sizeof(kUiPanelPs) / sizeof(kUiPanelPs[0]), f.ps)) {
        out = UiLayerFamily::kPanel;
    } else if (f.vs == kUiVsLoader && uiKnownPs(kUiLoaderPs, sizeof(kUiLoaderPs) / sizeof(kUiLoaderPs[0]), f.ps)) {
        out = UiLayerFamily::kLoader;
    } else if (f.vs == kUiVsGuiVector || f.vs == kUiVsGuiText || f.vs == kUiVsGuiIcons) {
        out = UiLayerFamily::kGuiDirect;
    } else if (f.vs == kUiVsFlightHud) {
        out = UiLayerFamily::kFlightHud;
    }
    return out;
}

// What a row is to the layer: the facts the DLL gathers from it, the family the rule names, the decision.
struct Tweak {
    bool overridePs = false;
    uint64_t ps = 0;
    int blendOp = -1;           // a different blend operation for the colour and alpha equations
    int kind = -1;              // the target kind forced (2: the 8-bit post-tonemap target)
    bool dsUnreproducible = false;
    bool lateTone = false;
};
struct Verdict {
    UiLayerFamily family = UiLayerFamily::kNone;
    UiLayerDecision decision = UiLayerDecision::kNotUi;
    UiDsEffect effect;
    int kind = 1;
};
static int kindOf(const IdDesc& t) { return t.view == 28 || t.view == 87 ? 2 : 1; }   // uiLayerLdrView: an 8-bit UNORM view, else the lit target's kind

static Verdict verdictOf(const Census& c, const Row& r, int set, const Tweak& t = Tweak{}, bool productionRule = false) {
    Verdict v;
    const IdDesc& target = c.ids.at(r.rtv);
    const IdDesc* dsv = r.dsv >= 0 ? &c.ids.at(r.dsv) : nullptr;
    v.kind = t.kind >= 0 ? t.kind : kindOf(target);
    UiFamilyFacts ff;
    ff.targetKind = v.kind;
    ff.vs = r.vs;
    ff.ps = t.overridePs ? t.ps : r.ps;
    v.family = productionRule ? uiLayerFamilyFor(ff) : frozenFamilyFor(ff, set);
    UiBlendRt blend = blendOf(r);
    if (t.blendOp >= 0) blend.op = blend.opA = static_cast<uint8_t>(t.blendOp);
    UiLayerDrawFacts g;
    g.family = v.family;
    g.verdictForwards = true;
    g.eyeTarget = true;
    g.ldrView = v.kind == 2;
    g.crispHdr = v.kind == 1 && uiLayerFamilyTakesHdr(v.family);
    g.eye = 0;
    g.armed = true;
    g.lateTone = t.lateTone && g.crispHdr;
    v.effect = uiLayerDsEffect(dsStateOf(r, dsv), dsv != nullptr);
    g.ds = v.effect;
    g.dsReproducible = !t.dsUnreproducible;
    g.blend = uiLayerBlendShape(blend);
    if (g.blend == UiBlendShape::kMultiply && v.effect.writes()) g.blend = UiBlendShape::kRefused;   // ui_layer.cpp uiLayerDecide: a multiply is drawn twice
    v.decision = uiLayerDecide(g);
    return v;
}

// ---------------------------------------------------------------- the seed model
// The layer's depth-stencil seed for one eye's HDR layer, as ui_layer.cpp beginInner and uiLayerNoteOther keep it. A taken draw that TESTS
// seeds when the layer's copy is not this frame's (stale: the first draw, or after a game write that invalidated it), lacks the depth the test needs,
// or lacks stencil bits the draw reads (wantMask = the read mask when the stencil is tested); a seed copies the depth and the bits asked, and on a GPU
// whose pixel shader can write the stencil reference every bit and the depth in one pass. A game draw that is NOT taken and writes the same depth
// target invalidates the copy unless it writes stencil only into a depth-only seed (uiLayerSeedWriterInvalidates). A taken draw's own write lands in
// the layer's copy as well (the layer's depth-stencil is bound for it) and in the game's buffer by the colourless re-issue: nothing goes stale.
struct SeedModel {
    bool specified = true;
    bool valid = false;
    uint8_t seededMask = 0;
    bool seededDepth = false;
    unsigned seeds = 0, passes = 0, stale = 0;
    void taken(const UiDsEffect& e, uint8_t readMask) {
        if (!e.tests()) return;
        const uint8_t want = e.stencilTest ? readMask : 0;
        const bool isStale = !valid, shortBits = (want & ~seededMask) != 0, shortDepth = e.depthTest && !seededDepth;
        if (!isStale && !shortBits && !shortDepth) return;
        const uint8_t mask = static_cast<uint8_t>(want | (isStale ? 0 : seededMask));
        const bool depth = e.depthTest || (!isStale && seededDepth);
        ++seeds;
        int bits = 0;
        for (unsigned b = 0; b < 8; ++b) bits += (mask >> b) & 1;
        passes += specified ? 1u : (depth ? 1u : 0u) + static_cast<unsigned>(bits);
        const bool full = specified && (depth || mask);
        seededMask = full ? 0xFF : mask;
        seededDepth = depth || full;
        valid = true;
    }
    void gameWrite(const UiDsEffect& e) {
        if (!valid) return;
        if (uiLayerSeedWriterInvalidates(e, seededMask, seededDepth, false)) {
            valid = false;
            ++stale;
        }
    }
};

struct SeedCount {
    unsigned perEye[2] = {0, 0}, total = 0, stale = 0, passes = 0;
    std::string left;   // the left eye's (the first depth target's) seeds and invalidations in draw order: "#<row> <family>" a seed, "x#<row>" an invalidation by that game draw
};
// The rows in order, grouped by the depth target they use (one per eye): the rows into the lit HDR target, taken or not, and every row that writes
// that depth target.
static SeedCount replaySeeds(const Census& c, int set, bool specified) {
    std::map<int, SeedModel> models;
    SeedCount out;
    for (const Row& r : c.rows) {
        if (r.dsv < 0) continue;
        const Verdict v = verdictOf(c, r, set);
        SeedModel& m = models[r.dsv];
        m.specified = specified;
        const bool hdrTarget = c.ids.at(r.rtv).fmt == 26;
        const bool takenRow = v.decision == UiLayerDecision::kRedirect && v.family != UiLayerFamily::kNone;
        const unsigned seedsBefore = m.seeds, staleBefore = m.stale;
        if (takenRow) {
            // A taken draw into the lit HDR target seeds (or reuses) the HDR layer's depth; one into the 8-bit target uses the other layer's
            // depth and, being taken, never reaches uiLayerNoteOther. Neither invalidates the HDR seed.
            if (hdrTarget) m.taken(v.effect, dsStateOf(r, &c.ids.at(r.dsv)).readMask);
        } else if (v.effect.writes()) {
            m.gameWrite(v.effect);
        }
        if (r.dsv == c.rows.front().dsv) {
            char note[96];
            if (m.seeds != seedsBefore) {
                std::snprintf(note, sizeof(note), "%s#%u %s", out.left.empty() ? "" : ", ", r.ordinal, uiLayerFamilyName(v.family));
                out.left += note;
            }
            if (m.stale != staleBefore) {
                std::snprintf(note, sizeof(note), "%sx#%u", out.left.empty() ? "" : ", ", r.ordinal);
                out.left += note;
            }
        }
    }
    unsigned eye = 0;
    for (auto& kv : models) {
        out.perEye[eye < 2 ? eye : 1] += kv.second.seeds;
        out.total += kv.second.seeds;
        out.stale += kv.second.stale;
        out.passes += kv.second.passes;
        ++eye;
    }
    return out;
}

// ---------------------------------------------------------------- the dust pixels
struct Dust {
    uint32_t frameW = 0, frameH = 0, cropX = 0, cropY = 0, cropW = 0, cropH = 0;
    std::vector<uint8_t> stencil, occluder;
    std::vector<std::pair<uint16_t, uint16_t>> points;
    std::vector<uint8_t> background;
};
template <class T>
static T take(const std::string& b, size_t* at) {
    check(*at + sizeof(T) <= b.size(), "the dust fixture is complete");
    T v;
    std::memcpy(&v, b.data() + *at, sizeof(T));
    *at += sizeof(T);
    return v;
}
static Dust loadDust(const char* path) {
    const std::string b = readFile(path);
    check(b.size() > 1000 && b.compare(0, 8, "EDVRDU01") == 0, "the dust fixture is readable and EDVRDU01");
    size_t at = 8;
    check(take<uint32_t>(b, &at) == 1, "the dust fixture is version 1");
    Dust d;
    d.frameW = take<uint32_t>(b, &at);
    d.frameH = take<uint32_t>(b, &at);
    d.cropX = take<uint32_t>(b, &at);
    d.cropY = take<uint32_t>(b, &at);
    d.cropW = take<uint32_t>(b, &at);
    d.cropH = take<uint32_t>(b, &at);
    const size_t total = size_t(d.cropW) * d.cropH;
    const uint32_t runs = take<uint32_t>(b, &at);
    d.stencil.reserve(total);
    d.occluder.reserve(total);
    for (uint32_t i = 0; i < runs; ++i) {
        const uint32_t len = take<uint32_t>(b, &at);
        const uint8_t s = take<uint8_t>(b, &at), o = take<uint8_t>(b, &at);
        d.stencil.insert(d.stencil.end(), len, s);
        d.occluder.insert(d.occluder.end(), len, o);
    }
    check(d.stencil.size() == total, "the runs cover the crop");
    const uint32_t n = take<uint32_t>(b, &at);
    for (uint32_t i = 0; i < n; ++i) {
        const uint16_t x = take<uint16_t>(b, &at), y = take<uint16_t>(b, &at);
        d.points.push_back({x, y});
    }
    const uint32_t nb = take<uint32_t>(b, &at);
    check(at + nb == b.size(), "the dust fixture has no trailing bytes");
    d.background.assign(b.begin() + static_cast<std::ptrdiff_t>(at), b.end());
    return d;
}

struct DustStats {
    double onSky = 0, onStencil5 = 0, withinOne = 0, withinTwo = 0;
    unsigned points = 0;
    unsigned p90 = 0, samples = 0;
};
static DustStats dustStats(const Dust& d) {
    DustStats s;
    s.points = static_cast<unsigned>(d.points.size());
    unsigned sky = 0, st5 = 0, one = 0, two = 0;
    for (const auto& p : d.points) {
        const size_t i = size_t(p.second) * d.cropW + p.first;
        if (!d.occluder[i]) ++sky;
        if (d.stencil[i] == 5) ++st5;
        auto near = [&](int rad) {
            for (int dy = -rad; dy <= rad; ++dy)
                for (int dx = -rad; dx <= rad; ++dx) {
                    const int x = p.first + dx, y = p.second + dy;
                    if (x >= 0 && y >= 0 && x < static_cast<int>(d.cropW) && y < static_cast<int>(d.cropH) && d.occluder[size_t(y) * d.cropW + x]) return true;
                }
            return false;
        };
        if (near(1)) ++one;
        if (near(2)) ++two;
    }
    const double n = s.points ? static_cast<double>(s.points) : 1.0;
    s.onSky = sky / n;
    s.onStencil5 = st5 / n;
    s.withinOne = one / n;
    s.withinTwo = two / n;
    std::vector<uint8_t> bg = d.background;
    std::sort(bg.begin(), bg.end());
    s.samples = static_cast<unsigned>(bg.size());
    s.p90 = bg.empty() ? 0 : bg[static_cast<size_t>(0.9 * static_cast<double>(bg.size() - 1))];
    return s;
}
// The acceptance the checks hold both the real data and each mutant to.
static bool dustAccepted(const DustStats& s) {
    return s.points > 1500 && s.onSky == 1.0 && s.onStencil5 >= 0.99 && s.withinOne <= 0.02 && s.samples > 3000 && s.p90 <= 8;
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && !std::strcmp(argv[1], "--dry-run")) {
            std::puts("supercruise_census_test: dry-run (no device, no files)");
            return 0;
        }
        if (argc != 2 || std::strcmp(argv[1], "--self-test") != 0) {
            std::puts("usage: supercruise_census_test --self-test | --dry-run");
            return 2;
        }
        const Census census = loadCensus("tools/supercruise_census_test/fixtures/census_054801_f1.txt");
        check(census.rows.size() == 244 && census.ids.size() == 18, "the census fixture is frame 1 of census 1: 244 draw rows, 18 target ids");
        unsigned gbuffer = 0, hdr = 0, ldr = 0, other = 0;
        for (const Row& r : census.rows) {
            const IdDesc& t = census.ids.at(r.rtv);
            check(t.w == 2016 && t.h == 1949, "every row is into a 2016x1949 target");
            (t.fmt == 23 ? gbuffer : t.fmt == 26 ? hdr : t.fmt == 27 ? ldr : other)++;
        }
        check(gbuffer == 120 && hdr == 110 && ldr == 8 && other == 6, "120 rows into the R10G10B10A2 g-buffer, 110 into the lit R11G11B10 target, 8 into the 8-bit one, 6 elsewhere");

        // THE TUPLE: the census's own rows, by the pairs.
        std::vector<const Row*> dustRows, barRows, orbitRows;
        for (const Row& r : census.rows) {
            if (r.vs == kUiVsSpaceDust && r.ps == kUiPsSpaceDust) dustRows.push_back(&r);
            if (r.vs == kUiVsSupercruiseBars && r.ps == kUiPsSupercruiseBars) barRows.push_back(&r);
            if (r.vs == kUiVsOrbitLines && r.ps == kUiPsOrbitLines) orbitRows.push_back(&r);
        }
        check(dustRows.size() == 2 && barRows.size() == 2 && orbitRows.size() == 2, "the frame holds each supercruise pair twice, once an eye");
        check(dustRows[0]->ordinal == 145 && dustRows[1]->ordinal == 194 && dustRows[0]->kind == 'N' && dustRows[0]->count == 1800 && dustRows[0]->instances == 1 &&
                  dustRows[0]->ds == "17wZ" && dustRows[0]->so == "r40/w40/f8/3,3,3" && dustRows[0]->bl == "12,2,1/2,2,1" && dustRows[0]->bm == "F",
              "the dust rows are #145 and #194: DrawInstanced of 1800 vertices, depth 17wZ, stencil r40/w40/f8/3,3,3, additive ONE ONE");
        for (const Row* r : dustRows) {
            const Verdict v = verdictOf(census, *r, 2, Tweak{}, true);
            check(v.kind == 1 && v.family == UiLayerFamily::kSpaceDust && v.decision == UiLayerDecision::kRedirect && v.effect.depthTest && !v.effect.stencilTest && v.effect.stencilWrite &&
                      !v.effect.depthWrite,
                  "a dust row on the lit HDR target is named the space dust and is kRedirect (it tests depth, writes stencil)");
            check(verdictOf(census, *r, 2, Tweak{}, true).family == UiLayerFamily::kSpaceDust && verdictOf(census, *r, 2, Tweak{}, true).decision == UiLayerDecision::kRedirect,
                  "...and so does the production rule, run on the same row");
            Tweak flatPs;
            flatPs.overridePs = true;
            flatPs.ps = 0xCB7AF179DF4E6A60ull;
            const Verdict a = verdictOf(census, *r, 2, flatPs, true);
            check(a.family == UiLayerFamily::kNone && a.decision == UiLayerDecision::kNotUi, "mutant: the flat route's pixel shader (CB7AF179) for the same vertex shader is not the family: not UI");
            Tweak minBlend;
            minBlend.blendOp = 4;   // D3D11_BLEND_OP_MIN
            check(verdictOf(census, *r, 2, minBlend, true).decision == UiLayerDecision::kBlendRefused, "mutant: the blend operation MIN is kBlendRefused");
            Tweak ldrTarget;
            ldrTarget.kind = 2;
            const Verdict l = verdictOf(census, *r, 2, ldrTarget, true);
            check(l.family == UiLayerFamily::kNone && l.decision == UiLayerDecision::kNotUi, "mutant: an 8-bit post-tonemap target is not the family either");
            Tweak noDepth;
            noDepth.dsUnreproducible = true;
            check(verdictOf(census, *r, 2, noDepth, true).decision == UiLayerDecision::kDepthStencilTest, "mutant: a depth state the layer cannot reproduce is kDepthStencilTest");
            Tweak late;
            late.lateTone = true;
            check(verdictOf(census, *r, 2, late, true).decision == UiLayerDecision::kToneLate, "mutant: a draw after the tonemap re-issue is kToneLate");
        }
        for (const Row* r : barRows) {
            const Verdict v = verdictOf(census, *r, 2, Tweak{}, true);
            check(v.family == UiLayerFamily::kSupercruiseBars && v.decision == UiLayerDecision::kRedirect && !v.effect.tests() && !v.effect.writes(), "a bars row is the bars, kRedirect, with no depth or stencil to reproduce");
        }
        for (const Row* r : orbitRows) {
            // the state tools/orbit_layer_test draws the strip with: depth GEQUAL no write, stencil EQUAL reference 1 under read mask 0x81, nothing written
            check(r->ds == "17wZ" && r->st == "11" && r->so == "r81/w00/f3/1,1,1" && r->count == 8194 && r->bl == "15,6,1/5,6,1",
                  "the orbit-lines rows carry the depth-stencil state the orbit-layer rig draws with: depth 17wZ, stencil reference 1, r81/w00/f3/1,1,1");
            const Verdict v = verdictOf(census, *r, 2, Tweak{}, true);
            check(v.family == UiLayerFamily::kOrbitLines && v.decision == UiLayerDecision::kRedirect && v.effect.depthTest && v.effect.stencilTest && !v.effect.writes(),
                  "an orbit-lines row is the orbit lines, kRedirect, testing depth and stencil and writing neither");
        }

        // THE DIFFERENTIAL, over every row.
        unsigned changedVsMain = 0, changedVsLines = 0, productionAgrees = 0;
        for (const Row& r : census.rows) {
            const Verdict main = verdictOf(census, r, 0), lines = verdictOf(census, r, 1), all = verdictOf(census, r, 2), prod = verdictOf(census, r, 2, Tweak{}, true);
            const bool supercruise = (r.vs == kUiVsSpaceDust && r.ps == kUiPsSpaceDust) || (r.vs == kUiVsSupercruiseBars && r.ps == kUiPsSupercruiseBars) ||
                                     (r.vs == kUiVsOrbitLines && r.ps == kUiPsOrbitLines);
            const bool dustRow = r.vs == kUiVsSpaceDust && r.ps == kUiPsSpaceDust;
            const bool changedMain = main.decision != all.decision || main.family != all.family;
            const bool changedLines = lines.decision != all.decision || lines.family != all.family;
            check(changedMain == supercruise, "against the rule on main, a row changes exactly when it is one of the six supercruise rows");
            check(changedLines == dustRow, "against the rule with the orbit lines and the bars, a row changes exactly when it is one of the two dust rows");
            if (supercruise) check(main.decision == UiLayerDecision::kNotUi && all.decision == UiLayerDecision::kRedirect, "a supercruise row was not UI on main and is redirected now");
            if (!supercruise) check(main.decision == all.decision && main.family == all.family, "every other row is decided exactly as on main");
            check(prod.family == all.family && prod.decision == all.decision, "the frozen copy of the rule agrees with the production rule on every row");
            changedVsMain += changedMain;
            changedVsLines += changedLines;
            productionAgrees += prod.family == all.family;
        }
        check(changedVsMain == 6 && changedVsLines == 2 && productionAgrees == census.rows.size(), "six rows change against main, two against the orbit lines and the bars alone");
        std::printf("census differential: %zu rows; %u change decision against main (the two orbit-line, two bars and two space-dust rows), %u against the rule with the orbit lines and bars\n",
                    census.rows.size(), changedVsMain, changedVsLines);

        // THE SEEDS, per eye, before (main's families) and after.
        for (bool specified : {true, false}) {
            const SeedCount before = replaySeeds(census, 0, specified), lines = replaySeeds(census, 1, specified), after = replaySeeds(census, 2, specified);
            std::printf("seed model (%s): main %u seeds a frame (left %u, right %u; %u passes; %u went stale), orbit lines + bars %u (%u passes), with the space dust %u (left %u, right %u; %u passes; %u went stale)\n",
                        specified ? "a GPU with the pixel shader's stencil reference: one pass a seed" : "no stencil reference: one pass per bit",
                        before.total, before.perEye[0], before.perEye[1], before.passes, before.stale, lines.total, lines.passes, after.total, after.perEye[0], after.perEye[1],
                        after.passes, after.stale);
            std::printf("seed model (%s), the left eye's seeds (#row family) and the game draws that invalidate one (x#row): main [%s]; with the three families [%s]\n",
                        specified ? "stencil reference" : "per bit", before.left.c_str(), after.left.c_str());
            check(before.total >= 2 && after.total >= before.total, "the model sees at least one seed an eye, and the new families never take one away");
        }

        // THE DUST PIXELS.
        const Dust dust = loadDust("tools/supercruise_census_test/fixtures/dust_054804.bin");
        const DustStats real = dustStats(dust);
        std::printf("dust pixels (eyes\\eye_054804, first dumped frame): %u on sky %.4f (stencil 5: %.4f), within 1 render px of an occluder %.2f%% (2 px: %.2f%%); background of %u sampled dust pixels: 90th percentile %u of 255\n",
                    real.points, real.onSky, real.onStencil5, 100.0 * real.withinOne, 100.0 * real.withinTwo, real.samples, real.p90);
        check(dustAccepted(real), "the dust pixels sit on sky (all on stencil 5), at most 2% within one render pixel of an occluder, and the 90th percentile of their 8-bit background is at most 8 of 255 (the parity bound T(F)+T(L)-T(F+L) <= T(F))");
        check(real.points > 2000 && real.withinOne > 0.005 && real.withinOne < 0.02, "...about one percent within one render pixel, not none (the measure sees an edge when there is one)");
        // Mutants: the dust moved down onto the cockpit, and a bright background.
        Dust moved = dust;
        for (auto& p : moved.points) p.second = static_cast<uint16_t>(std::min<uint32_t>(p.second + 600u, moved.cropH - 1));
        check(!dustAccepted(dustStats(moved)), "mutant: the dust pixels moved 600 pixels down (onto the cockpit and the planet) are caught");
        Dust bright = dust;
        for (size_t i = 0; i < bright.background.size(); i += 4) bright.background[i] = 90;
        check(!dustAccepted(dustStats(bright)), "mutant: a background that is bright under a quarter of the dust is caught (the additive composite would overshoot there)");
        // Mutant: 400 of the dust pixels put on the sky pixels that touch an occluder (the depth edges a layer drawn without the game's depth
        // would put dust across). The share within one render pixel of an occluder follows them and the acceptance refuses.
        Dust nearEdge = dust;
        {
            std::vector<std::pair<uint16_t, uint16_t>> edge;
            for (uint32_t y = 1; y + 1 < dust.cropH; ++y)
                for (uint32_t x = 1; x + 1 < dust.cropW; ++x) {
                    if (dust.occluder[size_t(y) * dust.cropW + x]) continue;
                    bool touches = false;
                    for (int dy = -1; dy <= 1 && !touches; ++dy)
                        for (int dx = -1; dx <= 1 && !touches; ++dx) touches = dust.occluder[size_t(y + dy) * dust.cropW + (x + dx)] != 0;
                    if (touches) edge.push_back({static_cast<uint16_t>(x), static_cast<uint16_t>(y)});
                }
            check(edge.size() > 400, "the crop has sky pixels that touch an occluder to move dust onto");
            for (size_t i = 0; i < 400; ++i) nearEdge.points[i] = edge[i * (edge.size() / 400)];
        }
        const DustStats edgy = dustStats(nearEdge);
        check(edgy.withinOne > 0.15 && !dustAccepted(edgy), "mutant: 400 dust pixels put on depth edges lift the within-one-pixel share past 15% and are refused");

        // THE GATE. The rigs of this work are only a gate while build.bat holds them: the three rigs' labels (build.bat runs every :rig_ label) and the
        // fixtures tool's self-test. A copy with one removed must fail the same scan.
        {
            std::string bat = readFile("build.bat");
            check(bat.size() > 100000, "build.bat is readable from the repo root");
            bat.erase(std::remove(bat.begin(), bat.end(), '\r'), bat.end());   // CRLF on disk
            const char* wanted[] = {"\n:rig_supercruise_census_test\n", "\n:rig_orbit_layer_test\n", "\n:rig_supercruise_bars_test\n",
                                    "\npython tools\\layer_fixtures.py --self-test || exit /b 1\n"};
            for (const char* w : wanted) {
                check(bat.find(w) != std::string::npos, "build.bat holds the rig label or the fixtures tool's gate");
                std::string without = bat;
                without.erase(without.find(w), std::strlen(w) - 1);
                check(without.find(w) == std::string::npos, "control: a copy of build.bat without it fails the scan");
            }
        }

        std::printf("PASS supercruise_census_test: %u checks\n", g_checks);
        return 0;
    } catch (const std::exception& e) {
        std::printf("FAIL %s (%u checks)\n", e.what(), g_checks);
        return 1;
    }
}
