// F151 (native on-foot VR): the weapon readout's draw bound by its place. Header only, no game or D3D11 state, so native_frame_test runs the very rule the marker draw tee uses.
//
// The weapon readout is a marker composite draw whose record the tee names from the draw's own entry in the instance stream (F63). The stream is shadowed in windows about the
// entries recent draws used (F87c), and a draw whose entry lies outside every window is not named (the second's "outside" count): it reaches no decision and goes out with the game's
// rows, 8 to 12 degrees from where the readout is placed, on the gun body. In the flight edvr_gfx_20261028_060723 this was 9 lost eye-1 draws in a 26.5 s shotgun ADS hold, mostly
// while the head moved; eye 1's entries moved by 231 to 13671 entries at layout moves.
//
// A draw that is outside every window is bound to the weapon readout's record only when it sits at the place of a readout draw that was named in the same eye in the previous frame.
// The place is all of: the eye; the mesh (the draw's vertex buffer, index buffer, offsets, format, index or vertex count, start, base vertex and draw kind); the ordinal among the
// eye's draws of that mesh in the frame (counted at every marker composite draw that has a mesh, in the tee's order); the instance count. The record is then the one tagged record
// the part's key has in this frame's Unmap of the draw's pool, and only if there is exactly one, the key was not named by a draw of this eye in this frame, and no other draw of
// the eye took it in this frame. The decision that follows (f85bDecide) keeps all its own checks, so a wrong bind is refused there when the camera it solves is not a rotation or its
// origin is far from the game's.
//
// A place is remembered from a draw named by its stream entry, never from a draw bound by place: a bind is not evidence of where the readout is.
#pragma once

#include <cstdint>
#include <cstring>

namespace edvr {
namespace nofoot {
namespace f151 {

constexpr int kEyes = 2;
constexpr uint32_t kSlots = 8u;              // the weapon UI's part slots (f85::kWeaponKeySlots; native_on_foot.cpp asserts it)
constexpr uint32_t kMeshesPerFrame = 24u;    // distinct meshes counted in one eye-frame; the draw of a further mesh has no ordinal
constexpr uint32_t kKeysPerFrame = 16u;      // tag keys (a part's first and second post) named or bound in one eye-frame
constexpr uint16_t kNoOrdinal = 0xFFFFu;

// The geometry a marker composite draw reads: the F67d icon table's draw configuration. `valid` false (no icon table slot for the draw) never equals anything.
struct Mesh {
    const void* vb = nullptr;
    const void* ib = nullptr;
    uint32_t stride = 0, vbOffset = 0, ibOffset = 0, ibFmt = 0, count = 0, start = 0;
    int32_t base = 0;
    char kind = 0;
    bool valid = false;
    bool operator==(const Mesh& o) const noexcept {
        return valid && o.valid && vb == o.vb && ib == o.ib && stride == o.stride && vbOffset == o.vbOffset && ibOffset == o.ibOffset && ibFmt == o.ibFmt && count == o.count &&
               start == o.start && base == o.base && kind == o.kind;
    }
};

// Why a draw outside every stream window was or was not bound. kBound is the bind; the rest are the guards that refuse, in the order they are checked.
enum Verdict : int {
    kBound = 0,
    kNoMesh,          // the draw has no mesh (no icon table slot) or no ordinal in its eye-frame
    kNoPlace,         // the eye has kept no readout place at all
    kStale,           // every kept place is older than the previous frame (or from this one)
    kOtherMesh,       // no previous-frame place has this mesh
    kOtherOrdinal,    // a place has this mesh, at another ordinal among the eye's draws of it
    kOtherInstances,  // the mesh and ordinal match, the instance count does not
    kTwoDraws,        // the part drew twice in its eye-frame (or two places match): not one place
    kKeyNamedHere,    // the part's key was named by a draw of this eye in this frame
    kKeyBoundHere,    // another draw of this eye took the key by place in this frame
    kNoFind,          // the key has no tagged record in this frame's Unmap of the draw's pool
    kManyFinds,       // the key has several tagged records there
    kVerdictCount
};
inline const char* verdictName(int v) noexcept {
    switch (v) {
        case kBound: return "bound";
        case kNoMesh: return "no mesh";
        case kNoPlace: return "no place kept";
        case kStale: return "place not from the last frame";
        case kOtherMesh: return "other mesh";
        case kOtherOrdinal: return "other ordinal";
        case kOtherInstances: return "other instance count";
        case kTwoDraws: return "part drew twice";
        case kKeyNamedHere: return "key named in this eye";
        case kKeyBoundHere: return "key bound already";
        case kNoFind: return "no record in this Unmap";
        case kManyFinds: return "several records";
        default: return "?";
    }
}

// A find belongs to the draw's frame only when it came from the pool's newest write Unmap and that Unmap is this frame's (the draws of a frame read the Unmap of the frame).
inline bool findIsThisUnmap(uint32_t findFrame, uint32_t poolUnmapFrame, uint32_t drawFrame) noexcept {
    return poolUnmapFrame != ~0u && poolUnmapFrame == drawFrame && findFrame == poolUnmapFrame;
}

// One readout draw named by its stream entry: where it sat.
struct Place {
    bool have = false;
    uint32_t frame = 0;
    Mesh mesh;
    uint16_t ord = 0;
    uint32_t instances = 0;
    uint32_t key = 0;       // the find's tag key, bit 7 the part's second post of the frame
    uint32_t drawn = 0;     // draws of the part named in this eye in `frame` (one is a place)
};

// What one eye's marker composite draws of one frame have shown so far.
struct EyeFrame {
    uint32_t frame = ~0u;
    uint32_t nMesh = 0;
    Mesh mesh[kMeshesPerFrame];
    uint16_t count[kMeshesPerFrame] = {};
    uint32_t nNamed = 0, named[kKeysPerFrame] = {};
    uint32_t nBound = 0, bound[kKeysPerFrame] = {};
    void roll(uint32_t f) noexcept {
        if (frame == f) return;
        *this = EyeFrame{};
        frame = f;
    }
    static bool has(const uint32_t* list, uint32_t n, uint32_t key) noexcept {
        for (uint32_t i = 0; i < n; ++i)
            if (list[i] == key) return true;
        return false;
    }
    static void add(uint32_t* list, uint32_t& n, uint32_t key) noexcept {
        if (n < kKeysPerFrame) list[n++] = key;
    }
};

// The eyes that drew a part in a frame, to count the frame in which one eye drew it and the other did not.
struct Seen {
    uint32_t frame = ~0u;
    bool eye[kEyes] = {};
    // A draw of the part in eye e of `f`. Returns the eye that drew nothing in the frame just closed (the first draw of a later frame closes it), else -1.
    int see(int e, uint32_t f) noexcept {
        if (e < 0 || e >= kEyes) return -1;
        int lost = -1;
        if (f != frame) {
            if (frame != ~0u && eye[0] != eye[1]) lost = eye[0] ? 1 : 0;
            frame = f;
            eye[0] = eye[1] = false;
        }
        eye[e] = true;
        return lost;
    }
};

class Places {
  public:
    Seen seen[kSlots];

    void reset() noexcept { *this = Places{}; }

    // Called once for every marker composite draw of eye e that has a mesh, in the tee's order: its ordinal among the eye's draws of that mesh in this frame (0 for the first).
    uint16_t ordinal(int e, uint32_t frame, const Mesh& m) noexcept {
        if (e < 0 || e >= kEyes || !m.valid) return kNoOrdinal;
        EyeFrame& ef = ef_[e];
        ef.roll(frame);
        for (uint32_t i = 0; i < ef.nMesh; ++i)
            if (ef.mesh[i] == m) {
                if (ef.count[i] >= kNoOrdinal - 1u) return kNoOrdinal;
                const uint16_t at = ef.count[i];
                ef.count[i] = static_cast<uint16_t>(at + 1u);
                return at;
            }
        if (ef.nMesh >= kMeshesPerFrame) return kNoOrdinal;
        ef.mesh[ef.nMesh] = m;
        ef.count[ef.nMesh] = 1;
        ++ef.nMesh;
        return 0;
    }

    // A draw of part `slot` that its stream entry named, with the tag key of its find: it is the place the next frame's lost draw is looked for at.
    void note(int e, uint32_t slot, uint32_t frame, const Mesh& m, uint16_t ord, uint32_t instances, uint32_t key) noexcept {
        if (e < 0 || e >= kEyes || slot >= kSlots) return;
        EyeFrame& ef = ef_[e];
        ef.roll(frame);
        if (!EyeFrame::has(ef.named, ef.nNamed, key)) EyeFrame::add(ef.named, ef.nNamed, key);
        Place& p = place_[e][slot];
        if (p.have && p.frame == frame) {   // the part named twice in one eye-frame: not one place
            ++p.drawn;
            return;
        }
        if (!m.valid || ord == kNoOrdinal) {   // named, but nowhere it can be found again
            p = Place{};
            return;
        }
        p.have = true;
        p.frame = frame;
        p.mesh = m;
        p.ord = ord;
        p.instances = instances;
        p.key = key;
        p.drawn = 1;
    }

    // A draw of eye e that no stream window held. findCount(key) says how many tagged records the key has in this frame's Unmap of the draw's pool (0, 1 or more); the draw is
    // bound only when it is 1. On kBound *slotOut and *keyOut say which part and tag key.
    template <class FindCount>
    Verdict bind(int e, uint32_t frame, const Mesh& m, uint16_t ord, uint32_t instances, FindCount&& findCount, uint32_t* slotOut = nullptr, uint32_t* keyOut = nullptr) noexcept {
        if (e < 0 || e >= kEyes) return kNoPlace;
        if (!m.valid || ord == kNoOrdinal) return kNoMesh;
        bool any = false, fresh = false, sameMesh = false, sameOrd = false, sameInst = false;
        uint32_t hits = 0, hit = 0;
        for (uint32_t s = 0; s < kSlots; ++s) {
            const Place& p = place_[e][s];
            if (!p.have) continue;
            any = true;
            if (p.frame + 1u != frame) continue;
            fresh = true;
            if (!(p.mesh == m)) continue;
            sameMesh = true;
            if (p.ord != ord) continue;
            sameOrd = true;
            if (p.instances != instances) continue;
            sameInst = true;
            ++hits;
            hit = s;
        }
        if (!any) return kNoPlace;
        if (!fresh) return kStale;
        if (!sameMesh) return kOtherMesh;
        if (!sameOrd) return kOtherOrdinal;
        if (!sameInst) return kOtherInstances;
        const Place& p = place_[e][hit];
        if (hits != 1 || p.drawn != 1) return kTwoDraws;
        EyeFrame& ef = ef_[e];
        ef.roll(frame);
        if (EyeFrame::has(ef.named, ef.nNamed, p.key)) return kKeyNamedHere;
        if (EyeFrame::has(ef.bound, ef.nBound, p.key)) return kKeyBoundHere;
        const uint32_t n = findCount(p.key);
        if (n == 0) return kNoFind;
        if (n > 1) return kManyFinds;
        EyeFrame::add(ef.bound, ef.nBound, p.key);
        if (slotOut) *slotOut = hit;
        if (keyOut) *keyOut = p.key;
        return kBound;
    }

  private:
    Place place_[kEyes][kSlots];
    EyeFrame ef_[kEyes];
};

}  // namespace f151
}  // namespace nofoot
}  // namespace edvr
