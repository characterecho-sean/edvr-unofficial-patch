// F198: the eye source 'S' (src/d3d11/submitted_eye.h) and targetEye's order: the pure parts, run as the draw path and treat run them.
//   - the stability rule: both eyes' submitted textures known, distinct, each the same at the same size for two submits;
//   - the previous-frame timing: a frame's draws precede its treat, so they read the frames before it;
//   - a recreate answers -1 until two stable submits of the new pair; an edge and a size change clear;
//   - the order L, T, then S only with the temporal path off: with it on, S is never asked.
#include "../../src/d3d11/submitted_eye.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {
unsigned checks = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) throw std::runtime_error(what);
}

using edvr::submittedeye::Named;
using edvr::submittedeye::Tracker;
using edvr::submittedeye::nameTargetEye;

// The four textures the game could submit: identities only, never dereferenced.
int a0, b0, a1, b1, other;
const void* const A = &a0;     // eye 0's texture
const void* const B = &b0;     // eye 1's texture
const void* const A2 = &a1;    // eye 0's texture after a recreate
const void* const B2 = &b1;    // eye 1's
const void* const O = &other;  // a render target that was never submitted

constexpr uint32_t W = 2016, H = 2017;

// One frame's treat: both eyes are submitted, eye 0 first (native on foot renders the right eye first, but Submit names the eye; the order does not matter here).
void submit(Tracker& t, const void* l, const void* r, uint64_t edge = 7, uint32_t w = W, uint32_t h = H) {
    t.note(0, l, w, h, edge);
    t.note(1, r, w, h, edge);
}

int count = 0;
Named order(int layer, int depth, bool temporal, int submitted, int* asked) {
    asked[0] = asked[1] = asked[2] = 0;
    return nameTargetEye([&] { ++asked[0]; return layer; }, [&] { ++asked[1]; return depth; }, temporal, [&] { ++asked[2]; return submitted; });
}
}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) return 2;
        if (!std::strcmp(argv[1], "--dry-run")) {
            std::puts("submitted_eye_test: dry-run");
            return 0;
        }
        if (std::strcmp(argv[1], "--self-test")) return 2;

        // Anti-aliasing off answers S after two stable submits, and not before.
        {
            Tracker t;
            check(t.eyeOf(A) == -1 && t.eyeOf(B) == -1, "nothing submitted yet: no eye");
            // frame 1: its draws precede its treat, so they see nothing; then the submits
            check(t.eyeOf(A) == -1, "frame 1 draws: nothing is known");
            submit(t, A, B);
            check(t.eyeOf(A) == -1 && t.eyeOf(B) == -1, "one submit of each: -1 (a single submit is not yet stable)");
            // frame 2: its draws see frame 1's submits only, which are one each: still -1
            check(t.eyeOf(A) == -1, "frame 2 draws: the one submit before them is not enough");
            submit(t, A, B);
            // frame 3: its draws see frames 1 and 2: S answers, before frame 3's own treat has run
            check(t.eyeOf(A) == 0 && t.eyeOf(B) == 1, "frame 3 draws: two stable submits name the eyes (the previous frames', by timing)");
            check(t.eyeOf(O) == -1 && t.eyeOf(nullptr) == -1, "a texture that was never submitted, and no texture, name no eye");
            submit(t, A, B);
            check(t.eyeOf(A) == 0 && t.eyeOf(B) == 1, "steady: the same answer a frame later");
        }
        // One submit: -1. And one eye alone: -1, whatever the other.
        {
            Tracker t;
            submit(t, A, B);
            check(t.eyeOf(A) == -1 && t.eyeOf(B) == -1, "one submit -> -1");
            Tracker u;
            u.note(0, A, W, H, 7);
            u.note(0, A, W, H, 7);
            u.note(0, A, W, H, 7);
            check(u.eyeOf(A) == -1 && u.eyeOf(B) == -1, "one eye submitted many times, the other never: -1 for both");
            u.note(1, B, W, H, 7);
            check(u.eyeOf(A) == -1, "the second eye's first submit is not stable yet: -1");
            u.note(1, B, W, H, 7);
            check(u.eyeOf(A) == 0 && u.eyeOf(B) == 1, "both eyes stable: named");
        }
        // A recreate: -1 until two stable submits of the new pair, then S again.
        {
            Tracker t;
            submit(t, A, B);
            submit(t, A, B);
            check(t.eyeOf(A) == 0 && t.eyeOf(B) == 1, "before the recreate: named");
            // the recreate's frame: its draws see the old pair; a draw into the NEW texture is -1 (unknown), as without S
            check(t.eyeOf(A2) == -1 && t.eyeOf(B2) == -1, "a draw into a texture not yet submitted: -1 (fail-safe: no eye, as today)");
            submit(t, A2, B2);
            check(t.eyeOf(A2) == -1 && t.eyeOf(B2) == -1 && t.eyeOf(A) == -1 && t.eyeOf(B) == -1, "the recreate's first submit: -1 for the new pair and for the old");
            submit(t, A2, B2);
            check(t.eyeOf(A2) == 0 && t.eyeOf(B2) == 1 && t.eyeOf(A) == -1 && t.eyeOf(B) == -1, "two stable submits of the new pair: S again, for the new pair only");
            // one eye's texture alone recreated
            submit(t, A, B2);
            check(t.eyeOf(A) == -1 && t.eyeOf(B2) == -1, "one eye recreated: both -1 until it is stable again");
            submit(t, A, B2);
            check(t.eyeOf(A) == 0 && t.eyeOf(B2) == 1, "...and named again after its second submit");
        }
        // Equal textures: -1 (the game submitted one texture for both eyes, or the pair collapsed).
        {
            Tracker t;
            submit(t, A, A);
            submit(t, A, A);
            submit(t, A, A);
            check(t.eyeOf(A) == -1, "the same texture for both eyes: -1");
        }
        // An edge (an on-foot or render-mode change) clears; two more stable submits name the eyes again.
        {
            Tracker t;
            submit(t, A, B, 7);
            submit(t, A, B, 7);
            check(t.eyeOf(A) == 0, "before the edge: named");
            submit(t, A, B, 8);   // the edge token moved: cleared, then this submit counts as the first
            check(t.eyeOf(A) == -1 && t.eyeOf(B) == -1, "the edge's frame: -1");
            submit(t, A, B, 8);
            check(t.eyeOf(A) == 0 && t.eyeOf(B) == 1, "two submits after the edge: named again");
            submit(t, A, B, 8);
            check(t.eyeOf(A) == 0, "and it holds");
        }
        // A size change with the same pointer (a freed texture's address reused for another size) starts the eye over.
        {
            Tracker t;
            submit(t, A, B, 7, W, H);
            submit(t, A, B, 7, W, H);
            check(t.eyeOf(A) == 0 && t.eyeOf(B) == 1, "before the size change: named");
            submit(t, A, B, 7, 3000, 3000);
            check(t.eyeOf(A) == -1 && t.eyeOf(B) == -1, "a size change: -1");
            submit(t, A, B, 7, 3000, 3000);
            check(t.eyeOf(A) == 0 && t.eyeOf(B) == 1, "two submits at the new size: named");
            // one eye changing size alone
            t.note(0, A, 1000, 1000, 7);
            check(t.eyeOf(A) == -1 && t.eyeOf(B) == -1, "one eye's size changed: both -1 until it is stable");
        }
        // Edge cases of the input: a bad eye is ignored, a null texture clears its eye.
        {
            Tracker t;
            t.note(2, A, W, H, 7);
            t.note(-1, A, W, H, 7);
            check(t.eyeOf(A) == -1, "an eye other than 0 or 1 is ignored");
            submit(t, A, B);
            submit(t, A, B);
            t.note(1, nullptr, 0, 0, 7);
            check(t.eyeOf(A) == -1 && t.eyeOf(B) == -1, "a null texture clears its eye: -1");
        }

        // targetEye's order: L, then T, then S only with the temporal path off.
        int asked[3];
        {
            Named n = order(0, 1, false, 1, asked);
            check(n.eye == 0 && n.src == 'L' && asked[0] == 1 && asked[1] == 0 && asked[2] == 0, "L answers: T and S are not asked");
            n = order(1, 0, true, 0, asked);
            check(n.eye == 1 && n.src == 'L' && asked[1] == 0 && asked[2] == 0, "L answers with the temporal path on: nothing else asked");
            n = order(-1, 1, false, 0, asked);
            check(n.eye == 1 && n.src == 'T' && asked[0] == 1 && asked[1] == 1 && asked[2] == 0, "T answers: S is not asked");
            n = order(-1, 0, true, 1, asked);
            check(n.eye == 0 && n.src == 'T' && asked[2] == 0, "T answers with the temporal path on: S is not asked");
            n = order(-1, -1, false, 1, asked);
            check(n.eye == 1 && n.src == 'S' && asked[0] == 1 && asked[1] == 1 && asked[2] == 1, "L and T answer -1, the temporal path off: S answers, last");
            n = order(-1, -1, false, 0, asked);
            check(n.eye == 0 && n.src == 'S', "S can name the left eye");
            // with the temporal path on S is NEVER asked, however the others answer
            for (int layer = -2; layer <= 2; ++layer)
                for (int depth = -2; depth <= 2; ++depth) {
                    n = order(layer, depth, true, 1, asked);
                    check(asked[2] == 0, "the temporal path on: S is never asked");
                    check(n.src != 'S', "the temporal path on: S never answers");
                    const bool lOk = layer == 0 || layer == 1, tOk = depth == 0 || depth == 1;
                    check((n.eye == layer && n.src == 'L') || (!lOk && tOk && n.eye == depth && n.src == 'T') || (!lOk && !tOk && n.eye == -1 && n.src == 0),
                          "the temporal path on: exactly the answers L and T give (byte-identical to the order before S)");
                }
            n = order(-1, -1, false, -1, asked);
            check(n.eye == -1 && n.src == 0 && asked[2] == 1, "nobody names the eye: none, for the caller's cache");
            n = order(2, 3, false, 7, asked);
            check(n.eye == -1 && n.src == 0, "values other than 0 and 1 are no eye");
        }

        std::printf("submitted_eye_test: %u checks passed\n", checks);
        (void)count;
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
