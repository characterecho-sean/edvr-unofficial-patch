#pragma once
// engine_velocity_test: the views from nothing (engineVelocityPrepareSourceFree; design doc section 104, the pool-less view), on WARP,
// through the functions the flat runtime calls.
//
// A scene that drew no pool-family draw at all (open ground and sky) has no draw to substitute, so no draw makes the source's views. The
// selector takes such a scene source-free, and the runtime asks the engine to make them from nothing: the slot target (the flat marker
// plane, cleared this frame, so no pixel names a record), a one-record pool nothing points into, and the scene constants of the selected
// camera with this frame's stamp. What can go wrong, and what is held here:
//   - the views are given only from the second frame, as after any gap: the previous frame's constants are the source's own;
//   - a slot target that still holds the last pool frame's markers when a source-free frame follows it (the turn from a pool-bearing view
//     to open ground): every texel must be the empty marker, or the camera term is replaced by a stale record's motion;
//   - the constants: this frame's rows and stamp in the current buffer, the predecessor's in the other, in either order of the two kinds of
//     frame, so the walk onto open ground and back keeps the chain unbroken;
//   - a pool draw that already made the frame's views is not overwritten;
//   - nothing is created per frame: the pool and its view are the same objects frame after frame, the two constant buffers alternate;
//   - outside the flat profile, or with the depth refused, nothing is made;
//   - the source is let go after kSourceIdleFrames frames without a refresh, and made again when the next source-free frame comes.
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../../src/common/runtime_profile.h"
#include "../../src/d3d11/engine_velocity.h"
#include "../../src/d3d11/engine_velocity_emit.h"
#include "lifecycle_tests.h"

namespace source_free_tests {
using Microsoft::WRL::ComPtr;

struct Rows { float v[6][4]; };

inline Rows rowsOf(const lifecycle_tests::Game& g, float shift) {
    Rows r{};
    std::memcpy(r.v, &g.rows[0][270 * 4], sizeof(r.v));
    r.v[5][0] += shift;   // the camera's position, so two frames differ
    return r;
}
// The game's cb1 as it would hold `r` (the watch follows the writes, as vscreen tees them).
inline std::vector<float> sceneOf(const lifecycle_tests::Game& g, const Rows& r) {
    std::vector<float> c = g.rows[0];
    std::memcpy(&c[270 * 4], r.v, sizeof(r.v));
    return c;
}

inline void run(const lifecycle_tests::Harness& h) {
    using namespace edvr;
    using lifecycle_tests::readBuffer;
    using lifecycle_tests::readTexture;
    using lifecycle_tests::release;
    const auto previous = g_runtimeProfile;
    size_t mark = lifecycle_fake::g_log.size();

    lifecycle_tests::Game game(h);
    game.setup();
    game.makeSource(40, 24);
    engineVelocityConfigure(true);
    // A source-free frame as the game draws it: it writes the world's constants, then the selection is made and the views prepared.
    const auto prep = [&](const Rows& rows) {
        game.writeScene(game.sceneA.Get(), sceneOf(game, rows));
        return engineVelocityPrepareSourceFree(h.context, game.sourceDepth.Get(), game.sceneA.Get(), rows.v);
    };

    // Whatever an earlier case left of the source is let go first, so the first frame below is the first.
    for (uint32_t i = 0; i < kSourceIdleFrames + 2; ++i) { game.beginFrame(); game.endFrame(); }
    mark = lifecycle_fake::g_log.size();

    // Outside the flat profile nothing is made, and nothing is counted.
    g_runtimeProfile = RuntimeProfile::Vr;
    game.beginFrame();
    const uint64_t before = engineVelocitySourceFreeFrames();
    h.check(!prep(rowsOf(game, 0.0f)) && engineVelocitySourceFreeFrames() == before,
            "source-free: outside the flat profile nothing is made");
    game.endFrame();
    g_runtimeProfile = RuntimeProfile::Flat;

    // The first source-free frame: made, but no previous constants to give.
    const Rows r1 = rowsOf(game, 0.00f), r2 = rowsOf(game, 0.05f), r3 = rowsOf(game, 0.11f);
    game.beginFrame();
    h.check(prep(r1), "source-free: the first frame's views are made");
    h.check(!game.sourceViews(), "source-free: but the first frame has no previous constants: nothing given");
    game.endFrame();
    h.check(lifecycle_tests::logged("engine motion: source-free views at present frame", mark),
            "source-free: the first made frame is logged once, with what it holds");

    // The second: given, with the slot target empty, one record in the pool, this frame's rows and the last frame's.
    game.beginFrame();
    h.check(prep(r2), "source-free: the second frame's views are made");
    EngineVelocityViews v2{};
    ID3D11ShaderResourceView* poolFirst = nullptr;
    ID3D11Buffer* nowFirst = nullptr;
    ID3D11Buffer* prevFirst = nullptr;
    {
        const bool given = game.sourceViews(&v2);
        h.check(given, "source-free: the second frame gives its views");
        if (given) {
            ComPtr<ID3D11Resource> slotsRes;
            v2.slots->GetResource(&slotsRes);
            ComPtr<ID3D11Texture2D> slotsTex;
            h.check(SUCCEEDED(slotsRes.As(&slotsTex)), "source-free: a slot texture");
            D3D11_TEXTURE2D_DESC sd{};
            if (slotsTex) slotsTex->GetDesc(&sd);
            h.check(sd.Width == 40 && sd.Height == 24 && sd.Format == DXGI_FORMAT_R32G32B32A32_FLOAT,
                    "source-free: the slot target is the flat marker plane, 40x24 RGBA32F");
            UINT w = 0;
            const auto slots = readTexture(h, slotsRes.Get(), 4, &w);
            unsigned empty = 0, other = 0;
            for (size_t i = 0; i + 3 < slots.size(); i += 4) (slots[i] == -1.0f ? empty : other) += 1;
            h.check(empty == 40u * 24u && other == 0, "source-free: every texel of the slot target is the empty marker");
            ComPtr<ID3D11Resource> poolRes;
            v2.pool->GetResource(&poolRes);
            D3D11_BUFFER_DESC pd{};
            ComPtr<ID3D11Buffer> poolBuf;
            if (SUCCEEDED(poolRes.As(&poolBuf))) poolBuf->GetDesc(&pd);
            h.check(pd.ByteWidth == engine_velocity_emit::kItemBytes && pd.StructureByteStride == engine_velocity_emit::kItemBytes,
                    "source-free: the pool is one structured record");
            const auto now = readBuffer(h, v2.sceneNow), prev = readBuffer(h, v2.scenePrev);
            h.check(now.size() >= 277 * 4 && prev.size() >= 277 * 4 &&
                        std::memcmp(&now[270 * 4], r2.v, sizeof(r2.v)) == 0 && std::memcmp(&prev[270 * 4], r1.v, sizeof(r1.v)) == 0,
                    "source-free: the current constants hold this frame's rows 270..275 and the other the last frame's");
            uint32_t nowStamp = 0, prevStamp = 0;
            if (now.size() >= 277 * 4) std::memcpy(&nowStamp, &now[276 * 4], 4);
            if (prev.size() >= 277 * 4) std::memcpy(&prevStamp, &prev[276 * 4], 4);
            h.check(nowStamp == game.frame && prevStamp == game.frame - 1,
                    "source-free: the stamps are this frame's and its consecutive predecessor's");
            poolFirst = v2.pool; nowFirst = v2.sceneNow; prevFirst = v2.scenePrev;
        }
    }
    // A second request in the same frame stands: nothing is remade or counted twice.
    const uint64_t counted = engineVelocitySourceFreeFrames();
    h.check(prep(r2) && engineVelocitySourceFreeFrames() == counted, "source-free: a second preparation in the same frame stands, uncounted");
    game.endFrame();

    // The third: no creation. The pool is the same object, the constants alternate, the last frame's current is this one's previous.
    game.beginFrame();
    h.check(prep(r3), "source-free: the third frame's views are made");
    {
        EngineVelocityViews v3{};
        const bool given = game.sourceViews(&v3);
        h.check(given, "source-free: the third frame gives its views");
        if (given) {
            h.check(v3.pool == poolFirst, "source-free: the pool and its view are the same objects frame after frame: nothing is created per frame");
            h.check(v3.scenePrev == nowFirst && v3.sceneNow == prevFirst,
                    "source-free: the two constant buffers alternate: this frame's previous is the last frame's current");
            const auto now = readBuffer(h, v3.sceneNow), prev = readBuffer(h, v3.scenePrev);
            h.check(now.size() >= 277 * 4 && std::memcmp(&now[270 * 4], r3.v, sizeof(r3.v)) == 0 &&
                        std::memcmp(&prev[270 * 4], r2.v, sizeof(r2.v)) == 0,
                    "source-free: and they hold the third frame's rows and the second's");
        }
        release(v3);
    }
    release(v2);
    game.endFrame();
    h.check(engineVelocitySourceFreeFrames() - before == 3, "source-free: three frames were made from nothing, counted once each");

    // The walk back: a pool-bearing frame follows, its constants the pool draw's; the chain holds, and its predecessor is the third frame's.
    game.sourceFrame();
    {
        EngineVelocityViews vp{};
        const bool given = game.sourceViews(&vp);
        h.check(given, "source-free: a pool-bearing frame after source-free ones gives its views");
        if (given) {
            const auto prev = readBuffer(h, vp.scenePrev);
            h.check(prev.size() >= 277 * 4 && std::memcmp(&prev[270 * 4], r3.v, sizeof(r3.v)) == 0,
                    "source-free: its previous constants are the source-free frame's");
            UINT w = 0;
            ComPtr<ID3D11Resource> slotsRes;
            vp.slots->GetResource(&slotsRes);
            const auto slots = readTexture(h, slotsRes.Get(), 4, &w);
            unsigned marked = 0;
            for (size_t i = 0; i + 3 < slots.size(); i += 4) marked += slots[i] != -1.0f;
            h.check(marked > 0, "source-free: the pool draw's markers are in the plane");
        }
        // A request after a pool draw made the frame's views does not overwrite them.
        const Rows ignored = rowsOf(game, 9.0f);
        h.check(engineVelocityPrepareSourceFree(h.context, game.sourceDepth.Get(), nullptr, ignored.v),
                "source-free: a preparation after a pool draw reports the views that stand");
        EngineVelocityViews again{};
        if (game.sourceViews(&again)) {
            const auto now = readBuffer(h, again.sceneNow);
            h.check(now.size() >= 277 * 4 && std::memcmp(&now[270 * 4], &game.rows[0][270 * 4], sizeof(r1.v)) == 0 &&
                        std::memcmp(&now[270 * 4], ignored.v, sizeof(ignored.v)) != 0,
                    "source-free: and leaves the pool draw's scene constants as they were");
        }
        release(again);
        release(vp);
    }
    game.endFrame();

    // The turn onto open ground after a pool-bearing frame: the plane still holds that frame's markers until it is cleared for this one.
    const Rows r4 = rowsOf(game, 0.2f);
    game.beginFrame();
    h.check(prep(r4), "source-free: the frame after a pool-bearing one is made from nothing");
    {
        EngineVelocityViews v{};
        const bool given = game.sourceViews(&v);
        h.check(given, "source-free: and gives its views");
        if (given) {
            UINT w = 0;
            ComPtr<ID3D11Resource> slotsRes;
            v.slots->GetResource(&slotsRes);
            const auto slots = readTexture(h, slotsRes.Get(), 4, &w);
            unsigned stale = 0;
            for (size_t i = 0; i + 3 < slots.size(); i += 4) stale += slots[i] != -1.0f;
            h.check(stale == 0, "source-free: the markers of the pool-bearing frame before are gone: the plane was cleared for this frame");
            const auto prev = readBuffer(h, v.scenePrev);
            h.check(prev.size() >= 277 * 4 && std::memcmp(&prev[270 * 4], &game.rows[0][270 * 4], sizeof(r1.v)) == 0,
                    "source-free: its previous constants are the pool-bearing frame's");
        }
        release(v);
    }
    game.endFrame();

    // A request for a depth no slot target fits is refused and nothing is claimed for it.
    {
        game.beginFrame();
        const uint64_t was = engineVelocitySourceFreeFrames();
        h.check(!engineVelocityPrepareSourceFree(h.context, nullptr, game.sceneA.Get(), r1.v) && engineVelocitySourceFreeFrames() == was,
                "source-free: no depth, nothing made");
        game.endFrame();
    }

    // Let go after kSourceIdleFrames frames with no refresh, made again by the next.
    mark = lifecycle_fake::g_log.size();
    for (uint32_t i = 0; i < kSourceIdleFrames + 2; ++i) { game.beginFrame(); game.endFrame(); }
    h.check(lifecycle_tests::logged("on-foot source slot target released", mark), "source-free: the source is let go when no frame refreshes it");
    h.check(!game.sourceViews(), "source-free: and nothing is given after");
    game.beginFrame();
    h.check(prep(r1), "source-free: the next source-free frame makes the source again");
    game.endFrame();
    game.beginFrame();
    h.check(prep(r2) && game.sourceViews(), "source-free: and the frame after it is given its views");
    game.endFrame();
    engineVelocityConfigure(false);
    h.context->ClearState();
    g_runtimeProfile = previous;
}
}  // namespace source_free_tests
