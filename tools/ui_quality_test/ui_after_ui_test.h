// Included after the rig's helpers (check, blend, the UiPx blend model).
//
// THE STATION MENU'S ORDER (2026-09-30; docs/ui-layer-2026-09-23.md, "2026-09-30:
// the station menu's frosted base under the HUD").
//
// The defect: docked, with the station services menu open, the cockpit HUD
// showed over the menu panel; with the game's own menu over it, the station
// menu showed through. Both were one draw left in the game's frame, under the
// layer: the frosted base (vs C4B4B334B26E81A9, n=360) the game draws right
// before the panel's content. In stock it is drawn after the tonemap and hides
// the HUD; the crisp re-issue had moved the HUD into the layer, so the layer
// held HUD, then panel content, and the base sat below both. The after-the-UI
// rule that should have taken the base was keyed to the tonemap's output, and
// the game's post pass carries the eye on into another target before any
// interface draw (uiLayerFollowReader, ui_layer_math.h).
//
// What this proves, each with the real functions of ui_layer_math.h:
//   1. the three pure pieces of the identity (uiLayerAfterWriteEye,
//      uiLayerAfterReadEye, uiLayerFollowReader): the truth table;
//   2. the RECORDED post-tonemap tails of the two censuses the defect was
//      diagnosed from (edvr_gfx_20260930_060935.log, census 2 = station
//      services with the layer live, census 3 = the game menu over it; frame
//      0, both eyes, transcribed from the DC lines: q= numbers, shader hashes,
//      vertex counts, targets and SRVs by resource identity, blend, depth and
//      stencil state), each behind one row per HUD family run the census lists
//      before the tonemap. Routed draw by draw through uiLayerFamilyFor,
//      uiLayerDecide, the after-UI gate and the follow, in the order
//      uiLayerNoteOther calls them (a source scan below keeps the two orders
//      together): NO interface draw is left under the layer. Routed the way
//      the code did before the follow, the same tails leave exactly the frosted
//      bases under it -- the fixture reproduces the field's "after the UI the
//      game drew 0 times";
//   3. the pixels: those recorded draw structures, given synthetic colours,
//      composited stock (every draw into one frame in the game's order)
//      against layered (the routing's answer, converted blends, layer then
//      composite over the frame) with the CPU blend model. Layered with the
//      follow equals stock; layered without it does not (the HUD leaks
//      through the base, the station panel through the game menu: the teeth);
//   4. the KNOWN LIMIT, pinned so it stays honest: a draw the layer does not
//      take, issued inside the HDR phase AFTER a taken HUD draw, was over that
//      HUD draw in stock and is under it now. The layered picture differs
//      from stock there, with or without the follow;
//   5. the wiring: ui_layer.cpp's uiLayerNoteOther calls the pieces in the
//      order the routing above assumes, the follow BEFORE the once-per-pair
//      note that returns on every later frame (a follow behind it would run
//      once a session), and the re-issue opens the chain. A rig cannot link
//      ui_layer.cpp (nothing does), so the order is checked in its text.
//
// What it does not prove: the D3D side (state reads, the tonemap's structural
// admission, the eye table's order) and pixels beyond the blend algebra. The
// flight says the rest: the gates line's "after the UI ... taken into the
// layer" nonzero and its eye check "matched".
namespace afterui {

constexpr uint32_t kEyeW = 2620, kEyeH = 2533;

// Resources, by identity: any distinct non-null pointer.
enum Res : int {
    kNone = 0,
    kHdr0, kHdr1,   // the lit HDR eye targets (R11G11B10): @98 / @419 in census 2
    kA0, kA1,       // the tonemap's outputs (R8G8B8A8, UNORM view): @783 / @815
    kB0, kB1,       // the post pass's outputs, where every interface draw lands: @791 / @821
    kBlur0, kBlur1, // the blurred scene the frosted base samples: 187x180 R11G11B10
    kAtlas512, kAtlas640a, kAtlas640b, kAtlas256,  // the interface's BC1 / BC7 art
    kStation,       // the station services surface, 3890x2188 (a learned interface surface)
    kEscSurface,    // the game menu's surface, 3890x2188 (learned)
    kMini,          // 486x289 (learned)
    kLoaderTex,     // 1037x1037 (learned)
    kUniform,       // loading_dim's 1x1 uniform texture
    kResCount
};
struct ResDesc {
    const char* name;
    uint32_t w, h;
    bool ldrView;  // a target viewed as 8-bit UNORM (uiLayerTargetKind() == 2 when eye-sized)
    bool learned;  // a learned interface surface (ui_depth's)
};
const ResDesc kRes[kResCount] = {
    {"none", 0, 0, false, false},
    {"HDR0", kEyeW, kEyeH, false, false},    {"HDR1", kEyeW, kEyeH, false, false},
    {"A0", kEyeW, kEyeH, true, false},       {"A1", kEyeW, kEyeH, true, false},
    {"B0", kEyeW, kEyeH, true, false},       {"B1", kEyeW, kEyeH, true, false},
    {"blur0", 187, 180, false, false},       {"blur1", 187, 180, false, false},
    {"atlas512", 512, 512, false, false},    {"atlas640a", 640, 360, false, false},
    {"atlas640b", 640, 360, false, false},   {"atlas256", 256, 256, false, false},
    {"station", 3890, 2188, true, true},     {"escSurface", 3890, 2188, true, true},
    {"mini", 486, 289, true, true},          {"loaderTex", 1037, 1037, true, true},
    {"uniform", 1, 1, true, false},
};
inline const void* rp(int r) { return r ? reinterpret_cast<const void*>(static_cast<uintptr_t>(r)) : nullptr; }
inline bool eyeSized(int r) { return r && kRes[r].w == kEyeW && kRes[r].h == kEyeH; }
// uiLayerTargetKind(): 0 not an eye target, 1 the lit HDR one, 2 8-bit UNORM.
inline int kindOf(int r) { return !eyeSized(r) ? 0 : kRes[r].ldrView ? 2 : 1; }
inline int eyeOf(int r) {
    switch (r) {
        case kHdr0: case kA0: case kB0: return 0;
        case kHdr1: case kA1: case kB1: return 1;
        default: return -1;
    }
}

// The hashes the census names.
constexpr uint64_t kFrostVs = 0xC4B4B334B26E81A9ull, kFrostPs = 0x0146ABCC53240479ull;
constexpr uint64_t kPanelPsTinted = 0x015EF9349EC097E8ull, kPanelPsEsc = 0x9107E72CB016CC02ull;
constexpr uint64_t kWashPs = 0x8ADB2A81A45E8A4Bull;
constexpr uint64_t kToneVs = 0x2D78DC3FD2C0C543ull, kTonePs = 0x99C21CEB7A699821ull;
constexpr uint64_t kPostVs = 0x20F383BBAC05C031ull, kPostPs = 0xDED8796049C7BB4Aull;
constexpr uint64_t kHudPs = 0x8DEF46452FA459F5ull, kHoloPs = 0xA2965EC2931A39C8ull;
constexpr uint64_t kSpherePs = 0xEA02FAC2BD6C643Cull;
constexpr uint64_t kQuadVs = 0x2CECEC3065EF0D4Aull, kQuadPs = 0xA4D03619D631B186ull;

// One recorded draw, as the census line prints it (DC ... vh= ph= n= r= s= ds= st=
// so= bm= bl=), plus a pixel model for the synthetic scenes.
struct GDraw {
    const char* what = "";
    uint32_t q = 0;  // the census's own q=, for the messages
    uint64_t vs = 0, ps = 0;
    uint32_t n = 0;
    int target = kNone;
    int srv[4] = {kNone, kNone, kNone, kNone};
    UiBlendRt bl;
    UiDsState ds;
    bool dsv = false;      // a depth-stencil view is bound (census d= not "-")
    bool tonemap = false;  // the structure tonemapAdmitStructure admits: 3 vertices, no DSV, b2 >= 256
    uint32_t mask = 0;     // pixels the synthetic scenes give it
    UiPx src;              // its source colour there (premultiplied where the blend is)
};

// bl= / bm=, decoded: "12,6,1/2,6,1" with bm=7 is a premultiplied over into RGB.
UiBlendRt bOff() { return blend(false, uiblend::kOne, uiblend::kZero, uiblend::kWriteAll); }
UiBlendRt bPremul(uint8_t mask = uiblend::kWriteRgb) {
    return blend(true, uiblend::kOne, uiblend::kInvSrcAlpha, mask);
}
UiBlendRt bMultiply() { return blend(true, uiblend::kZero, uiblend::kSrcColor, uiblend::kWriteRgb); }
// ds= / st= / so=, decoded (comparison 7 = GREATER_EQUAL, 8 = ALWAYS; ops 1 = KEEP, 3 = REPLACE).
UiDsState dsNone() {  // ds=02wA st=00: depth off, stencil off
    UiDsState s;
    s.depthEnable = false;
    s.depthFunc = 2;
    s.depthWriteAll = true;
    return s;
}
UiDsState dsStencil(uint8_t writeMask) {  // ds=02wA st=14 so=r00/w<mask>/f8/1,1,3: depth off, stencil written
    UiDsState s = dsNone();
    s.stencilEnable = true;
    s.readMask = 0x00;
    s.writeMask = writeMask;
    s.front.pass = 3;
    s.back.pass = 3;
    return s;
}
UiDsState dsGreaterEqual() {  // ds=17wZ st=00: the HUD's test against the scene depth, no write
    UiDsState s;
    s.depthEnable = true;
    s.depthFunc = 7;
    s.depthWriteAll = false;
    return s;
}
UiDsState dsHolo() {  // ds=17wZ st=14 so=r00/w04/f8/1,1,3+f8/1,1,1
    UiDsState s = dsGreaterEqual();
    s.stencilEnable = true;
    s.readMask = 0x00;
    s.writeMask = 0x04;
    s.front.pass = 3;
    return s;
}

GDraw row(const char* what, uint32_t q, uint64_t vs, uint64_t ps, uint32_t n, int target,
          std::initializer_list<int> srv, const UiBlendRt& bl, const UiDsState& ds, bool dsv) {
    GDraw d;
    d.what = what;
    d.q = q;
    d.vs = vs;
    d.ps = ps;
    d.n = n;
    d.target = target;
    int i = 0;
    for (int s : srv) d.srv[i++] = s;
    d.bl = bl;
    d.ds = ds;
    d.dsv = dsv;
    return d;
}

// The HDR phase, one row standing for each run the census lists (FLIGHTHUD x18,
// HOLOPANEL, the frosted label quad, the hologram, HOLOPANEL n=279): the HUD
// families the layer takes, and the non-family draw among them it does not.
void hdrPhase(std::vector<GDraw>& out, int eye, const uint32_t (&q)[5]) {
    const int hdr = eye ? kHdr1 : kHdr0, blur = eye ? kBlur1 : kBlur0;
    out.push_back(row("flight HUD", q[0], kUiVsFlightHud, kHudPs, 663, hdr, {}, bPremul(), dsGreaterEqual(), true));
    out.push_back(row("holo panel", q[1], kUiVsHolo, kHoloPs, 12, hdr, {}, bPremul(), dsHolo(), true));
    out.push_back(row("frosted label quad", q[2], kQuadVs, kQuadPs, 6, hdr, {blur, kAtlas512}, bPremul(),
                      dsStencil(0x04), true));
    out.push_back(row("hologram", q[3], kHoloTargetSphere, kSpherePs, 6, hdr, {}, bPremul(uiblend::kWriteAll),
                      dsStencil(0x04), true));
    out.push_back(row("holo panel n=279", q[4], kUiVsHolo, kHoloPs, 279, hdr, {}, bPremul(), dsHolo(), true));
}

// tonemap -> post pass, per eye: A = the tonemap's output, B = the post pass's.
void toneAndPost(std::vector<GDraw>& out, int eye, uint32_t qTone, uint32_t qPost) {
    const int hdr = eye ? kHdr1 : kHdr0, a = eye ? kA1 : kA0, b = eye ? kB1 : kB0;
    GDraw t = row("tonemap", qTone, kToneVs, kTonePs, 3, a, {kNone, hdr}, bOff(), dsNone(), false);
    t.tonemap = true;
    out.push_back(t);
    out.push_back(row("post pass", qPost, kPostVs, kPostPs, 4, b, {a}, bOff(), dsNone(), false));
}

// The interface draws of one eye, as the census lists them after the post pass.
GDraw frostBase(uint32_t q, int eye, int art) {
    return row("frosted base", q, kFrostVs, kFrostPs, 360, eye ? kB1 : kB0,
               {eye ? kBlur1 : kBlur0, kAtlas512, art, kAtlas256}, bPremul(), dsNone(), true);
}
GDraw menuPanel(const char* what, uint32_t q, int eye, uint64_t ps, uint32_t n, int s0, int s1) {
    return row(what, q, kUiVsPanel, ps, n, eye ? kB1 : kB0, {s0, s1}, bPremul(), dsStencil(0x04), true);
}
GDraw washMultiply(uint32_t q, int eye) {
    return row("wash multiply", q, kUiVsLoader, kWashPs, 360, eye ? kB1 : kB0, {kLoaderTex}, bMultiply(),
               dsStencil(0x00), true);
}

// Census 2 (log 060935, 06:12:13, DLSS + layer, station services open), frame 0.
std::vector<GDraw> census2() {
    std::vector<GDraw> f;
    hdrPhase(f, 0, {780, 837, 843, 858, 866});
    hdrPhase(f, 1, {906, 961, 970, 982, 990});
    toneAndPost(f, 0, 1028, 1033);
    toneAndPost(f, 1, 1041, 1046);
    f.push_back(frostBase(1049, 0, kAtlas640a));
    f.push_back(menuPanel("menu panel", 1050, 0, kPanelPsTinted, 360, kAtlas512, kStation));
    f.push_back(menuPanel("mini menu", 1053, 0, kPanelPsTinted, 6, kAtlas512, kMini));
    f.push_back(frostBase(1058, 1, kAtlas640a));
    f.push_back(menuPanel("menu panel", 1059, 1, kPanelPsTinted, 360, kAtlas512, kStation));
    f.push_back(menuPanel("mini menu", 1062, 1, kPanelPsTinted, 6, kAtlas512, kMini));
    return f;
}

// Census 3 (06:12:27, the game's own menu open over station services), frame 0.
std::vector<GDraw> census3() {
    std::vector<GDraw> f;
    hdrPhase(f, 0, {767, 840, 846, 861, 869});
    hdrPhase(f, 1, {911, 984, 993, 1005, 1013});
    toneAndPost(f, 0, 1054, 1059);
    toneAndPost(f, 1, 1067, 1072);
    for (int eye = 0; eye < 2; ++eye) {
        const uint32_t q0 = eye ? 1095 : 1077;
        f.push_back(frostBase(q0, eye, kAtlas640a));
        f.push_back(menuPanel("menu panel", q0 + 1, eye, kPanelPsTinted, 360, kAtlas512, kStation));
        f.push_back(washMultiply(q0 + 4, eye));
        GDraw base2 = frostBase(q0 + 9, eye, kAtlas640b);
        base2.what = "frosted base (game menu)";
        f.push_back(base2);
        f.push_back(menuPanel("menu panel (game menu)", q0 + 10, eye, kPanelPsEsc, 360, kUniform, kEscSurface));
        f.push_back(menuPanel("mini menu", q0 + 13, eye, kPanelPsTinted, 6, kAtlas512, kMini));
    }
    return f;
}

// ------------------------------------------------------------- the routing --

enum class Verdict : uint8_t {
    kHdrTaken,      // a HUD family draw, taken into the HDR layer
    kReissue,       // the admitted tonemap: its own draw stays, the re-issue opens the 8-bit layer
    kTakenFamily,   // an interface family draw, taken into the 8-bit layer
    kTakenAfterUi,  // a write into the eye's identity, taken into it after the UI
    kLeftRead,      // a small pass reading the identity (the post pass): left, and it may carry the identity on
    kLeftPostPass,  // an after-UI write with an eye-sized input: left
    kLeftDeclined,  // a family or after-UI draw the layer's rules decline: left
    kLeftFrame,     // anything else: the game's frame, under the layer
};
inline bool taken(Verdict v) {
    return v == Verdict::kHdrTaken || v == Verdict::kTakenFamily || v == Verdict::kTakenAfterUi;
}

// What vscreen.cpp's forwardWithVerdict and ui_layer.cpp do with one eye draw,
// in their order, calling ui_layer_math.h for each decision they take there:
// the tonemap's admission and re-issue, the family rule and decide, then the
// after-the-UI gate (uiLayerNoteOther). The state is the eye's identity and
// chain, exactly the fields Eye::target, Eye::chainOpen and Eye::draws.
struct Router {
    struct EyeState {
        int target = kNone;
        bool chainOpen = false;
        uint32_t draws = 0;
        bool hdrTaken = false;
    } eye[2];
    bool follow = true;  // false: the routing before the follow existed
    bool watching = false;
    uint32_t watchBudget = 64;
    unsigned reads = 0, followed = 0, afterWrites = 0;

    UiLayerDecision decide(const GDraw& d, UiLayerFamily family, int eyeIndex) const {
        UiLayerDrawFacts f;
        f.family = family;
        const int kind = kindOf(d.target);
        f.eyeTarget = kind != 0;
        f.ldrView = kind == 2;
        f.crispHdr = kind == 1 && uiLayerFamilyTakesHdr(family);  // the one list ui_layer.cpp asks (it names the orbit lines and the bars too)
        f.eye = eyeIndex;
        f.armed = true;
        f.ds = uiLayerDsEffect(d.ds, d.dsv);
        f.dsReproducible = true;
        f.blend = uiLayerBlendShape(d.bl);
        // ui_layer.cpp uiLayerDecide: a multiply is drawn twice, so one that writes depth or stencil is refused.
        if (f.blend == UiBlendShape::kMultiply && f.ds.writes()) f.blend = UiBlendShape::kRefused;
        return uiLayerDecide(f);
    }

    void takeIn(const GDraw& d, int eyeIndex, bool hdr) {
        watching = true;
        EyeState& e = eye[eyeIndex];
        if (hdr) {
            e.hdrTaken = true;
            return;
        }
        if (e.draws == 0) e.target = d.target;  // the layer's clear branch: the first UI draw names the target
        ++e.draws;
        e.chainOpen = false;  // the UI has started
    }

    Verdict noteOther(const GDraw& d, int kind) {
        const void* takenT[2] = {eye[0].draws ? rp(eye[0].target) : nullptr, eye[1].draws ? rp(eye[1].target) : nullptr};
        if (!takenT[0] && !takenT[1]) return Verdict::kLeftFrame;
        int takenEye = -1;
        char how = 0;
        const int writeEye = kind != 0 ? uiLayerAfterWriteEye(true, rp(d.target), takenT[0], takenT[1]) : -1;
        if (writeEye >= 0) {
            how = 'W';
            takenEye = writeEye;
            ++afterWrites;
        } else if (d.n <= 6 && watchBudget) {
            --watchBudget;
            for (int slot = 0; slot < 2; ++slot) {
                const int readEye = uiLayerAfterReadEye(rp(d.srv[slot]), takenT[0], takenT[1]);
                if (readEye >= 0) {
                    how = 'R';
                    takenEye = readEye;
                    ++reads;
                    break;
                }
            }
        }
        if (!how) return Verdict::kLeftFrame;
        if (how == 'R') {
            if (follow && uiLayerFollowReader(eye[takenEye].chainOpen, takenT[takenEye], takenT[1 - takenEye],
                                              kind == 2, rp(d.target))) {
                eye[takenEye].target = d.target;
                eye[takenEye].chainOpen = false;
                ++followed;
            }
            return Verdict::kLeftRead;
        }
        bool eyeSizedInput = false;
        for (int s : d.srv) eyeSizedInput = eyeSizedInput || (s && eyeSized(s));
        if (!uiLayerAfterWritePreserved(false, false, false)) return Verdict::kLeftDeclined;
        if (uiLayerAfterWriteDecide(eyeSizedInput) == UiAfterWriteDecision::kPostPass) return Verdict::kLeftPostPass;
        if (decide(d, UiLayerFamily::kAfterUi, takenEye) != UiLayerDecision::kRedirect) return Verdict::kLeftDeclined;
        takeIn(d, takenEye, false);
        return Verdict::kTakenAfterUi;
    }

    Verdict step(const GDraw& d) {
        const int kind = kindOf(d.target);
        const int eyeIndex = eyeOf(d.target);
        // The admitted tonemap (uiLayerCrispNoteEyeDraw, then uiLayerCrispToneEnd's publish).
        if (d.tonemap && kind == 2 && eyeIndex >= 0 && eye[eyeIndex].hdrTaken) {
            EyeState& e = eye[eyeIndex];
            e.target = d.target;
            e.chainOpen = true;
            e.draws = 1;
            watching = true;
            return Verdict::kReissue;
        }
        if (kind != 0) {
            UiFamilyFacts ff;
            ff.targetKind = kind;
            ff.vs = d.vs;
            if (kind == 1 && d.vs == kHoloTargetSphere) ff.ps = d.ps;
            if (kind == 2) {
                for (int s : d.srv) ff.learnedSurface = ff.learnedSurface || (s && kRes[s].learned);
                if (!ff.learnedSurface && (d.vs == kUiVsPanel || d.vs == kUiVsLoader)) ff.ps = d.ps;
            }
            const UiLayerFamily family = uiLayerFamilyFor(ff);
            if (family != UiLayerFamily::kNone) {
                if (decide(d, family, eyeIndex) != UiLayerDecision::kRedirect) return Verdict::kLeftDeclined;
                takeIn(d, eyeIndex, kind == 1);
                return kind == 1 ? Verdict::kHdrTaken : Verdict::kTakenFamily;
            }
        }
        return watching ? noteOther(d, kind) : Verdict::kLeftFrame;
    }
};

std::vector<Verdict> route(const std::vector<GDraw>& frame, bool follow, Router* out = nullptr) {
    Router r;
    r.follow = follow;
    std::vector<Verdict> v;
    for (const GDraw& d : frame) v.push_back(r.step(d));
    if (out) *out = r;
    return v;
}

// The interface draws of a recorded frame: every draw after a post pass into
// the post pass's own target, the post pass excluded.
bool isInterfaceDraw(const std::vector<GDraw>& frame, size_t i) {
    for (size_t k = 0; k < i; ++k) {
        const GDraw& p = frame[k];
        if (p.vs == kPostVs && p.target == frame[i].target && frame[i].vs != kPostVs) return true;
    }
    return false;
}

// ------------------------------------------------------------ 1. the truth table

void testIdentity() {
    const void* A = rp(kA0);
    const void* B = rp(kB0);
    const void* A1 = rp(kA1);
    const void* B1 = rp(kB1);
    // The write case.
    check(uiLayerAfterWriteEye(true, B, B, A1) == 0 && uiLayerAfterWriteEye(true, B1, B, B1) == 1,
          "a write into an eye's identity names that eye");
    check(uiLayerAfterWriteEye(true, B, A, A1) == -1, "a write into anything else is no write");
    check(uiLayerAfterWriteEye(false, B, B, B1) == -1, "a target that is not an eye target is never a write");
    check(uiLayerAfterWriteEye(true, nullptr, nullptr, nullptr) == -1 &&
              uiLayerAfterWriteEye(true, B, nullptr, nullptr) == -1,
          "nothing taken this frame: nothing matches, a null target included");
    // The read case.
    check(uiLayerAfterReadEye(A, A, A1) == 0 && uiLayerAfterReadEye(A1, A, A1) == 1,
          "a pass sampling an eye's identity names that eye");
    check(uiLayerAfterReadEye(B, A, A1) == -1 && uiLayerAfterReadEye(nullptr, A, A1) == -1,
          "sampling anything else, or nothing, is no read");
    // The follow: the reader of the identity carries it into its own eye-sized 8-bit target, once.
    check(uiLayerFollowReader(true, A, A1, true, B), "A -> B: the post pass carries the eye's identity on");
    check(!uiLayerFollowReader(true, A, A1, false, B),
          "a reader without an eye-sized 8-bit target never carries it (a mirror pass, a blur, an HDR pass)");
    check(!uiLayerFollowReader(true, A, A1, true, nullptr), "no target, no follow");
    check(!uiLayerFollowReader(true, A, A1, true, A), "a reader that draws into the identity itself does not");
    check(!uiLayerFollowReader(true, A, B1, true, B1),
          "a reader drawing into the OTHER eye's identity never carries this eye's there");
    check(!uiLayerFollowReader(false, A, A1, true, B), "once the chain is closed nothing carries it: the UI has started");
    check(!uiLayerFollowReader(true, nullptr, A1, true, B), "an eye with no identity has nothing to carry");
    check(uiLayerFollowReader(true, A, nullptr, true, B), "the other eye holding nothing does not stop the follow");
    // Once per eye-frame: the chain closes with the follow, and the identity is B, not A, from then on.
    {
        const void* identity = A;
        bool chain = true;
        int carried = 0;
        for (const void* reader : {B, rp(kBlur0), B}) {  // a second pass after the first
            if (uiLayerFollowReader(chain, identity, A1, reader != rp(kBlur0), reader)) {
                identity = reader;
                chain = false;
                ++carried;
            }
        }
        check(carried == 1 && identity == B, "a chain carries the identity once an eye-frame, to the first eye-sized 8-bit pass");
        check(uiLayerAfterWriteEye(true, B, identity, A1) == 0 && uiLayerAfterWriteEye(true, A, identity, A1) == -1,
              "after the follow the interface's target is the identity and the tonemap's output no longer is");
    }
}

// ------------------------------------------- 2. the recorded tails, routed

void testRecordedTails() {
    struct Case {
        const char* name;
        std::vector<GDraw> frame;
        unsigned baseDraws;  // the frosted bases in the frame, both eyes
    } cases[2] = {{"census 2 (station services, layer live)", census2(), 2},
                  {"census 3 (the game's menu over it)", census3(), 4}};
    for (Case& c : cases) {
        char what[240];
        // The routing with the follow: every interface draw is taken.
        Router with;
        const std::vector<Verdict> v = route(c.frame, true, &with);
        unsigned interfaceDraws = 0, left = 0, basesTaken = 0;
        for (size_t i = 0; i < c.frame.size(); ++i) {
            if (!isInterfaceDraw(c.frame, i)) continue;
            ++interfaceDraws;
            if (!taken(v[i])) {
                ++left;
                std::snprintf(what, sizeof(what), "%s: %s (q=%u) is left under the layer", c.name, c.frame[i].what,
                              c.frame[i].q);
                check(false, what);
            }
            if (c.frame[i].vs == kFrostVs && v[i] == Verdict::kTakenAfterUi) ++basesTaken;
        }
        std::snprintf(what, sizeof(what), "%s: every interface draw after the post pass is taken into the layer (%u of them)",
                      c.name, interfaceDraws);
        check(interfaceDraws >= 6 && left == 0, what);
        std::snprintf(what, sizeof(what), "%s: the %u frosted bases are taken after the UI", c.name, c.baseDraws);
        check(basesTaken == c.baseDraws, what);
        std::snprintf(what, sizeof(what), "%s: the follow ran once an eye (%u), the post passes stay in the frame", c.name,
                      with.followed);
        check(with.followed == 2 && with.reads == 2, what);
        // The HUD went into the layer, the HDR-phase non-family draw did not.
        bool hudTaken = false, quadLeft = false;
        for (size_t i = 0; i < c.frame.size(); ++i) {
            hudTaken = hudTaken || (c.frame[i].vs == kUiVsFlightHud && v[i] == Verdict::kHdrTaken);
            quadLeft = quadLeft || (c.frame[i].vs == kQuadVs && v[i] == Verdict::kLeftFrame);
        }
        check(hudTaken && quadLeft, "the flight HUD is taken into the HDR layer, the frosted label quad stays in the frame");
        // The routing before the follow: the frosted bases stay under the layer, and nothing else does.
        Router without;
        const std::vector<Verdict> u = route(c.frame, false, &without);
        unsigned basesLeft = 0, othersLeft = 0;
        for (size_t i = 0; i < c.frame.size(); ++i) {
            if (!isInterfaceDraw(c.frame, i) || taken(u[i])) continue;
            (c.frame[i].vs == kFrostVs ? basesLeft : othersLeft) += 1;
        }
        std::snprintf(what, sizeof(what),
                      "%s, routed as before the follow: the %u frosted bases are left under the layer, the "
                      "field's 'after the UI ... 0' (got %u, and %u others)",
                      c.name, c.baseDraws, basesLeft, othersLeft);
        check(basesLeft == c.baseDraws && othersLeft == 0 && without.afterWrites == 0 && without.followed == 0, what);
    }
}

// ------------------------------------------------------- 3. the pixels

// Sample pixels. 0: HUD, base and tile; 1: HUD and base; 2: HUD alone (the gap
// outside the panel); 3: base and tile, no HUD; 4: nothing. In the game menu's
// scene the wash, the second base and the game menu's content cover 0-3.
constexpr int kPixels = 5;
UiPx premul(float r, float g, float b, float a) { return UiPx{r * a, g * a, b * a, a}; }

void giveColours(std::vector<GDraw>& frame) {
    for (GDraw& d : frame) {
        if (eyeOf(d.target) != 0) continue;  // one eye's pixels: the other eye's rows stay colourless
        if (std::strcmp(d.what, "flight HUD") == 0) {
            d.mask = 0x07;  // pixels 0, 1, 2
            d.src = premul(0.60f, 0.40f, 0.10f, 0.80f);
        } else if (std::strcmp(d.what, "frosted base") == 0) {
            d.mask = 0x0B;  // 0, 1, 3
            d.src = premul(0.05f, 0.05f, 0.08f, 1.0f);
        } else if (std::strcmp(d.what, "menu panel") == 0) {
            d.mask = 0x09;  // 0, 3
            d.src = premul(0.50f, 0.25f, 0.05f, 0.60f);
        } else if (std::strcmp(d.what, "wash multiply") == 0) {
            d.mask = 0x0F;
            d.src = UiPx{0.50f, 0.50f, 0.50f, 1.0f};
        } else if (std::strcmp(d.what, "frosted base (game menu)") == 0) {
            d.mask = 0x0F;
            d.src = premul(0.02f, 0.02f, 0.03f, 1.0f);
        } else if (std::strcmp(d.what, "menu panel (game menu)") == 0) {
            d.mask = 0x0F;
            d.src = premul(0.30f, 0.30f, 0.40f, 0.50f);
        }
    }
}

struct Pixels {
    UiPx direct[kPixels], frame[kPixels], layer[kPixels], mult[kPixels], out[kPixels];
    void init() {
        const UiPx scene[kPixels] = {{0.10f, 0.12f, 0.15f, 1}, {0.20f, 0.20f, 0.20f, 1}, {0.15f, 0.10f, 0.05f, 1},
                                     {0.10f, 0.10f, 0.10f, 1}, {0.30f, 0.25f, 0.20f, 1}};
        for (int p = 0; p < kPixels; ++p) {
            direct[p] = frame[p] = scene[p];
            layer[p] = UiPx{0, 0, 0, 1};
            mult[p] = UiPx{1, 1, 1, 1};
        }
    }
};

// Stock: every colour-writing draw into one frame, in the game's order. Layered:
// the routing's verdict decides -- taken draws go into the layer with the
// converted blend (a multiply also into the per-channel transmittance), the
// rest into the game's frame -- and the door composites layer over frame.
Pixels renderPixels(const std::vector<GDraw>& frame, bool follow) {
    Pixels px;
    px.init();
    const std::vector<Verdict> v = route(frame, follow);
    for (size_t i = 0; i < frame.size(); ++i) {
        const GDraw& d = frame[i];
        if (!d.mask) continue;
        for (int p = 0; p < kPixels; ++p) {
            if (!(d.mask & (1u << p))) continue;
            px.direct[p] = uiBlendApply(d.bl, d.src, px.direct[p]);
            if (taken(v[i])) {
                UiBlendRt conv, second;
                if (!uiLayerConvertBlend(d.bl, &conv)) {
                    check(false, "a taken draw's blend converts");
                    continue;
                }
                px.layer[p] = uiBlendApply(conv, d.src, px.layer[p]);
                if (uiLayerMultiplyBlend(d.bl, &second)) px.mult[p] = uiBlendApply(second, d.src, px.mult[p]);
            } else {
                px.frame[p] = uiBlendApply(d.bl, d.src, px.frame[p]);
            }
        }
    }
    for (int p = 0; p < kPixels; ++p) px.out[p] = uiLayerCompositePx(px.layer[p], px.mult[p], px.frame[p]);
    return px;
}

float worstRgb(const Pixels& a, int from = 0, int to = kPixels - 1) {
    float worst = 0.0f;
    for (int p = from; p <= to; ++p) {
        worst = (std::max)(worst, std::fabs(a.out[p].r - a.direct[p].r));
        worst = (std::max)(worst, std::fabs(a.out[p].g - a.direct[p].g));
        worst = (std::max)(worst, std::fabs(a.out[p].b - a.direct[p].b));
    }
    return worst;
}

void testStationPixels() {
    // Station services (the recorded census-2 draw structure, synthetic colours).
    {
        std::vector<GDraw> frame = census2();
        giveColours(frame);
        const Pixels fixedUp = renderPixels(frame, true), before = renderPixels(frame, false);
        check(worstRgb(fixedUp) < 1e-5f,
              "station services: layered with the follow equals stock at every sample pixel");
        check(worstRgb(before, 0, 1) > 0.05f,
              "station services, before the follow: the HUD shows through the base the game draws over it (the teeth)");
        check(worstRgb(before, 2, 4) < 1e-5f,
              "...and only where the base is: the HUD in the gap and the untouched pixels agree with stock");
    }
    // The game's menu over station services (census 3's draw structure).
    {
        std::vector<GDraw> frame = census3();
        giveColours(frame);
        const Pixels fixedUp = renderPixels(frame, true), before = renderPixels(frame, false);
        check(worstRgb(fixedUp) < 1e-5f,
              "the game's menu: layered with the follow equals stock -- the wash, the second base, the content in order");
        check(worstRgb(before, 0, 3) > 0.05f,
              "the game's menu, before the follow: the station menu and the HUD show through the game's own base (the teeth)");
    }
    // The known limit, pinned: a draw the layer does not take, inside the HDR phase, after a taken HUD draw.
    {
        std::vector<GDraw> frame = census2();
        giveColours(frame);
        for (GDraw& d : frame) {
            if (std::strcmp(d.what, "frosted label quad") == 0 && d.target == kHdr0) {
                d.mask = 0x04;  // pixel 2, the HUD alone: an opaque draw in stock over the HUD drawn before it
                d.src = premul(0.05f, 0.30f, 0.05f, 1.0f);
                d.bl = bPremul();
            }
        }
        const Pixels with = renderPixels(frame, true), without = renderPixels(frame, false);
        check(worstRgb(with, 2, 2) > 0.05f && worstRgb(without, 2, 2) > 0.05f,
              "KNOWN LIMIT: an HDR-phase draw the layer does not take, issued after a taken HUD draw, sits under it "
              "(stock has it over): the picture differs from stock there, with the follow or without");
    }
}

// ------------------------- 6. the cockpit with Disable GUI effects on (2026-10-01)

// The cockpit's side and centre panels with Elite's Disable GUI effects on: vs 1989E6D3B405FDE0 ps EAB8A1C95A13FFBE instead of the stock vs
// 81216C77F90DEDD6 / ps A2965EC2931A39C8 (holo_material.h; docs/ui-layer-2026-09-23.md, "2026-10-01: Disable GUI effects"). The holo-panel
// draws of the NumLock census at 13:54:24 in edvr_gfx_20261001_135211.log (build 73e02a7b, Sean's Frontier install, eye 2016x1949), frame 0,
// both eyes -- r=@117 and r=@745, the two eyes' lit HDR targets, twelve draws each -- transcribed from the DC lines: n= and q=, and the draw
// state of every one of them, ds=17wZ st=14 so=r00/w04/f8/1,1,3+f8/1,1,1 bm=7 bl=12,6,1/2,6,1, which is the stock pair's own (the 11:28
// census of build 08b48036 draws vs 81216C77F90DEDD6 with the same fields). The build that census came from named no family for them:
// "cockpit holo panels" never appeared in its 30 s lines, and ui depth counted 22 draws a frame "left alone".
struct PanelRow {
    uint32_t n, q;
};
const PanelRow kGuiFxOffEyeA[12] = {{12, 1432}, {12, 1433}, {6, 1466}, {279, 1467}, {435, 1468}, {435, 1469},
                                    {6, 1470},  {12, 1471}, {6, 1486}, {6, 1488},   {18, 1489},  {6, 1490}};
const PanelRow kGuiFxOffEyeB[12] = {{12, 1588}, {12, 1589}, {6, 1622}, {279, 1623}, {435, 1624}, {435, 1625},
                                    {6, 1626},  {12, 1627}, {6, 1642}, {6, 1644},   {18, 1645},  {6, 1646}};

std::vector<GDraw> guiFxOffFrame(uint64_t vs, uint64_t ps) {
    std::vector<GDraw> f;
    for (int eye = 0; eye < 2; ++eye) {
        for (const PanelRow& p : eye ? kGuiFxOffEyeB : kGuiFxOffEyeA)
            f.push_back(row("holo panel (GUI effects off)", p.q, vs, ps, p.n, eye ? kHdr1 : kHdr0, {}, bPremul(), dsHolo(), true));
    }
    return f;
}

void testGuiFxOffCockpit() {
    unsigned counted = 0;
    auto takenCount = [&](const std::vector<GDraw>& frame, Router* out) {
        Router r;
        const std::vector<Verdict> v = route(frame, true, &r);
        unsigned n = 0;
        for (Verdict x : v) n += x == Verdict::kHdrTaken ? 1u : 0u;
        if (out) *out = r;
        counted += n;
        return n;
    };
    // The recorded frame: every one of the 24 panel draws is taken into the HDR layer, both eyes.
    {
        const std::vector<GDraw> frame = guiFxOffFrame(kUiVsHoloGuiFxOff, kHoloGuiFxOffPs);
        Router r;
        check(frame.size() == 24, "the recorded Disable-GUI-effects cockpit frame has 12 panel draws an eye (the census's 24)");
        check(takenCount(frame, &r) == 24 && r.eye[0].hdrTaken && r.eye[1].hdrTaken,
              "Disable GUI effects on: every one of the 24 recorded panel draws is named by the family rule, decided a redirect and taken into the "
              "HDR layer, both eyes (the field's build named none of them)");
    }
    // The stock pair through the same structure: taken the same way, so the two are one family to the layer.
    check(takenCount(guiFxOffFrame(kUiVsHolo, kHoloPs), nullptr) == 24,
          "the stock pair through the same recorded structure is taken the same way: 24 of 24");
    // The pair's shaders are not interchangeable halves: the new vertex shader is named by itself (the pixel shader is not asked on the
    // HDR target), so a variant pixel shader of it -- the unlit material, say -- is the family too.
    check(takenCount(guiFxOffFrame(kUiVsHoloGuiFxOff, kHoloUnlitPs), nullptr) == 24,
          "the new vertex shader names the family by itself on the HDR target, whichever pixel shader it draws with");
    // The controls: a vertex shader one bit away from either names nothing, so nothing is taken and every draw stays in the game's frame --
    // the field's picture, "cockpit holo panels" absent and 22-24 draws a frame left in the scene.
    for (uint64_t vs : {kUiVsHoloGuiFxOff ^ 1ull, kUiVsHolo ^ 1ull, kHoloGuiFxOffPs}) {
        Router r;
        const std::vector<GDraw> frame = guiFxOffFrame(vs, kHoloGuiFxOffPs);
        const std::vector<Verdict> v = route(frame, true, &r);
        unsigned left = 0;
        for (Verdict x : v) left += x == Verdict::kLeftFrame ? 1u : 0u;
        check(left == 24 && !r.eye[0].hdrTaken && !r.eye[1].hdrTaken,
              "control: a vertex shader the rule does not name leaves all 24 draws in the game's frame (the teeth)");
    }
    check(counted == 72, "(the three taken frames counted 24 each)");
}

// --------------------------------------------------------- 5. the wiring

// The code with its comments and whitespace removed, so a check reads statements,
// not layout (string and character literals are kept whole).
std::string squeeze(const std::string& s) {
    std::string t;
    enum { kCode, kString, kChar, kLine, kBlock } state = kCode;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i], next = i + 1 < s.size() ? s[i + 1] : '\0';
        switch (state) {
            case kCode:
                if (c == '/' && next == '/') {
                    state = kLine;
                    ++i;
                } else if (c == '/' && next == '*') {
                    state = kBlock;
                    ++i;
                } else if (c == '"') {
                    state = kString;
                    t += c;
                } else if (c == '\'') {
                    state = kChar;
                    t += c;
                } else if (!std::isspace(static_cast<unsigned char>(c))) {
                    t += c;
                }
                break;
            case kString:
                t += c;
                if (c == '\\' && next) {
                    t += next;
                    ++i;
                } else if (c == '"') {
                    state = kCode;
                }
                break;
            case kChar:
                t += c;
                if (c == '\\' && next) {
                    t += next;
                    ++i;
                } else if (c == '\'') {
                    state = kCode;
                }
                break;
            case kLine:
                if (c == '\n') state = kCode;
                break;
            case kBlock:
                if (c == '*' && next == '/') {
                    state = kCode;
                    ++i;
                }
                break;
        }
    }
    return t;
}

// The text of a top-level function: from its signature to the closing brace in column 0.
bool functionBody(const std::string& text, const char* signature, std::string* body) {
    const size_t at = text.find(signature);
    if (at == std::string::npos) return false;
    const size_t end = text.find("\n}\n", at);
    if (end == std::string::npos) return false;
    *body = text.substr(at, end - at + 2);
    return true;
}

void testWiring() {
    std::ifstream in("src/d3d11/ui_layer.cpp", std::ios::binary);
    check(bool(in), "src/d3d11/ui_layer.cpp is readable from the working directory (the rig runs from the repo root)");
    if (!in) return;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string other, tone;
    check(functionBody(text, "bool uiLayerNoteOther(", &other), "uiLayerNoteOther is found");
    check(functionBody(text, "void uiLayerCrispToneEnd(", &tone), "uiLayerCrispToneEnd is found");
    const std::string o = squeeze(other), t = squeeze(tone);
    const char* order[] = {"uiLayerAfterWriteEye(", "uiLayerAfterReadEye(", "uiLayerFollowReader(",
                           "g_afterSeen[i].kind=='R'", "uiLayerAfterWritePreserved(", "uiLayerAfterWriteDecide(",
                           "uiLayerDecide(ctx"};
    size_t prev = 0;
    bool ordered = true;
    for (const char* piece : order) {
        const size_t at = o.find(piece);
        if (at == std::string::npos || at < prev) {
            char what[300];
            std::snprintf(what, sizeof(what),
                          "uiLayerNoteOther: '%s' is missing or out of order; the routing model assumes write, read, "
                          "follow, the read case's once-per-pair note, the write path's exclusions, decide -- the follow "
                          "BEFORE the note that returns on every later frame",
                          piece);
            check(false, what);
            ordered = false;
            break;
        }
        prev = at;
    }
    check(ordered, "uiLayerNoteOther keeps the order the routing model assumes");
    check(o.find("g_eye[takenEye].target=g_tc.info.resource;") != std::string::npos &&
              o.find("g_eye[takenEye].chainOpen=false;") != std::string::npos,
          "uiLayerNoteOther adopts the reader's target and closes the chain");
    check(t.find("e.target=rtvRes;e.chainOpen=true;") != std::string::npos,
          "the tonemap re-issue publishes the tonemap's output as the identity and opens the chain");
    const std::string all = squeeze(text);
    check(all.find("e.target=g_draw.targetRes;e.chainOpen=false;") != std::string::npos,
          "the layer's first UI draw of a frame names its own target and closes the chain");
    check(all.find("++e.draws;e.chainOpen=false;") != std::string::npos,
          "every taken UI draw closes the chain");
}

}  // namespace afterui
