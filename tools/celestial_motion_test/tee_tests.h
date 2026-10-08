// (d) The CPU tee and the draw capture of src/d3d11/celestial_motion.cpp, on a WARP device with real constant buffers.
// The rig calls the module exactly where vscreen.cpp's hooks do: a Map's pointer after the real Map, the copy out of
// it BEFORE the real Unmap, UpdateSubresource's whole write, the copies and command lists that invalidate, the draw
// before the game's own issue, the boundary once a frame, the pass's consumer once an eye. Included by
// celestial_motion_test.cpp after its helpers and the module itself.
#pragma once

namespace tee {
using Microsoft::WRL::ComPtr;
using namespace edvr;

void hr(HRESULT h, const char* what) { check(SUCCEEDED(h), fmt("%s failed (0x%08X)", what, static_cast<unsigned>(h))); }

struct Dev {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
};
Dev makeDevice() {
    Dev d;
    D3D_FEATURE_LEVEL level{};
    hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d.dev, &level, &d.ctx), "D3D11CreateDevice(WARP)");
    return d;
}

// The two blocks a patch draw reads, as the game writes them: VS b2 (944 bytes, rows 2 4-8 10 12 23 carry the patch) and
// VS b0 (208 bytes, rows 9-11 the camera rows' 3x3).
std::vector<uint8_t> b2Block(const cel::Patch& p, uint32_t bytes = 944) {
    std::vector<uint8_t> b(bytes, 0);
    float f[4 * cel::kB2Rows] = {};
    std::memcpy(&f[2 * 4], p.c, 12);
    std::memcpy(&f[4 * 4], p.rows, 64);
    std::memcpy(&f[8 * 4], p.o, 12);
    std::memcpy(&f[10 * 4], p.q, 16);
    std::memcpy(&f[12 * 4], p.body, 16);
    std::memcpy(&f[23 * 4], p.q2, 16);
    std::memcpy(b.data(), f, std::min<size_t>(sizeof f, b.size()));
    return b;
}
std::vector<uint8_t> b0Block(const cel::Patch& p, uint32_t bytes = 208) {
    std::vector<uint8_t> b(bytes, 0);
    float f[4 * 13] = {};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) f[(9 + i) * 4 + j] = p.A[i * 3 + j];
    std::memcpy(b.data(), f, std::min<size_t>(sizeof f, b.size()));
    return b;
}

struct Bufs {
    ComPtr<ID3D11Buffer> b0, b1, b2;
};
Bufs makeBuffers(Dev& d, bool dynamic, uint32_t b0Bytes = 208, uint32_t b2Bytes = 944) {
    Bufs b;
    auto make = [&](uint32_t bytes, ComPtr<ID3D11Buffer>& out) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = bytes;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (dynamic) { bd.Usage = D3D11_USAGE_DYNAMIC; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE; }
        hr(d.dev->CreateBuffer(&bd, nullptr, &out), "CreateBuffer");
    };
    make(b0Bytes, b.b0);
    make(5376, b.b1);
    make(b2Bytes, b.b2);
    return b;
}
void bind(Dev& d, Bufs& b) {
    ID3D11Buffer* cb[3] = {b.b0.Get(), b.b1.Get(), b.b2.Get()};
    d.ctx->VSSetConstantBuffers(0, 3, cb);
}

// hookedMap / hookedUnmap, as written: the pointer after the real Map, the copy before the real Unmap.
void mapWrite(Dev& d, ID3D11Buffer* b, const std::vector<uint8_t>& bytes) {
    D3D11_MAPPED_SUBRESOURCE m{};
    hr(d.ctx->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m), "Map");
    if (celestialMotionAnyWatched()) celestialMotionConstantsMapped(b, m.pData);
    std::memcpy(m.pData, bytes.data(), bytes.size());
    if (celestialMotionAnyWatched()) celestialMotionConstantsUnmapped(b);
    d.ctx->Unmap(b, 0);
}
// hookedUpdateSubresource: a DEFAULT constant buffer's whole write.
void updateWrite(Dev& d, ID3D11Buffer* b, const std::vector<uint8_t>& bytes) {
    d.ctx->UpdateSubresource(b, 0, nullptr, bytes.data(), 0, 0);
    if (celestialMotionAnyWatched()) celestialMotionConstantsWritten(b, bytes.data(), nullptr);
}

void reset() {
    celestialMotionShutdown();
    celestialMotionConfigure(true);
    celestialMotionNoteStatus(true, true);   // the supercruise gate open: the tee's cases run in supercruise (the gate has its own, gate_tests.h)
    g_logLines.clear();
    g_stubEyeDsv[0] = reinterpret_cast<ID3D11DepthStencilView*>(0x1000);
    g_stubEyeDsv[1] = reinterpret_cast<ID3D11DepthStencilView*>(0x2000);
    g_stubDsv = g_stubEyeDsv[0];
}

bool patchEquals(const cel::Patch& a, const cel::Patch& b) {
    return std::memcmp(a.c, b.c, 12) == 0 && std::memcmp(a.q, b.q, 16) == 0 && std::memcmp(a.rows, b.rows, 64) == 0 &&
           std::memcmp(a.o, b.o, 12) == 0 && std::memcmp(a.q2, b.q2, 16) == 0 && std::memcmp(a.body, b.body, 16) == 0 &&
           std::memcmp(a.A, b.A, 36) == 0 && a.hash == b.hash;
}

std::vector<uint8_t> readBuffer(Dev& d, ID3D11Buffer* src) {
    D3D11_BUFFER_DESC bd{};
    src->GetDesc(&bd);
    bd.Usage = D3D11_USAGE_STAGING;
    bd.BindFlags = 0;
    bd.MiscFlags = 0;
    bd.StructureByteStride = 0;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> stage;
    hr(d.dev->CreateBuffer(&bd, nullptr, &stage), "staging buffer");
    d.ctx->CopyResource(stage.Get(), src);
    D3D11_MAPPED_SUBRESOURCE m{};
    hr(d.ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m), "Map staging");
    std::vector<uint8_t> out(bd.ByteWidth);
    std::memcpy(out.data(), m.pData, bd.ByteWidth);
    d.ctx->Unmap(stage.Get(), 0);
    return out;
}

void all(const Fixture& fx) {
    Dev d = makeDevice();
    unsigned cases = 0;
    const EyeFrame* f1 = fx.find(23651, 0);
    const cel::Patch& p0 = f1->patches[0];
    const cel::Patch& p1 = f1->patches[1];
    const cel::Patch& p2 = f1->patches[2];

    // ---- Map/Unmap: nothing watched, then watched, then read -----------------------------------------------------------
    {
        reset();
        Bufs b = makeBuffers(d, true);
        bind(d, b);
        check(!celestialMotionAnyWatched() && celestialMotionLive(), "live, with nothing watched yet");
        celestialMotionNoteDraw(d.ctx.Get());
        check(g_win.draws == 1 && g_win.captured == 0 && g_win.declined[kDecUnwatched] == 1 && g_watched == 2 && celestialMotionAnyWatched(),
              "the first draw finds both buffers unwatched: counted, both now watched");
        check(!g_logLines.empty() && g_logLines.back().find("unwatched") != std::string::npos, "the first refusal says why, once");
        // the game writes both (b0 once for the pass, b2 for the patch), then draws
        mapWrite(d, b.b0.Get(), b0Block(p0));
        mapWrite(d, b.b2.Get(), b2Block(p0));
        celestialMotionNoteDraw(d.ctx.Get());
        check(g_win.captured == 1 && g_eye[0].n[g_eye[0].cur] == 1, "Map/Unmap, then a draw: one patch captured on eye 0");
        check(patchEquals(g_eye[0].patches[g_eye[0].cur][0], p0), "the captured patch is the written one, to the bit");
        // the next face: only b2 is rewritten, b0's shadow stands
        mapWrite(d, b.b2.Get(), b2Block(p1));
        celestialMotionNoteDraw(d.ctx.Get());
        check(g_win.captured == 2 && patchEquals(g_eye[0].patches[g_eye[0].cur][1], p1), "the next face rewrites b2 only: b0's shadow stands");
        // the same constants again, no write: the same patch is not captured twice
        celestialMotionNoteDraw(d.ctx.Get());
        check(g_win.captured == 2 && g_win.declined[kDecDuplicate] == 1, "an unchanged redraw is a duplicate, not a second patch");
        // another eye's depth: the other list
        g_stubDsv = g_stubEyeDsv[1];
        mapWrite(d, b.b2.Get(), b2Block(p0));
        celestialMotionNoteDraw(d.ctx.Get());
        check(g_eye[1].n[g_eye[1].cur] == 1, "the other eye's depth fills the other eye's list");
        // a draw into something that is not an eye's scene depth
        g_stubDsv = nullptr;
        celestialMotionNoteDraw(d.ctx.Get());
        check(g_win.declined[kDecOffEye] == 1, "a draw whose depth is no eye's is counted off-eye and read from nowhere");
        ++cases;
    }
    // ---- UpdateSubresource, whole and partial -----------------------------------------------------------------------
    {
        reset();
        Bufs b = makeBuffers(d, false);
        bind(d, b);
        celestialMotionNoteDraw(d.ctx.Get());   // registers both
        updateWrite(d, b.b0.Get(), b0Block(p0));
        updateWrite(d, b.b2.Get(), b2Block(p0));
        celestialMotionNoteDraw(d.ctx.Get());
        check(g_win.captured == 1 && patchEquals(g_eye[0].patches[g_eye[0].cur][0], p0), "UpdateSubresource's whole write is the shadow");
        // a boxed write over just the centre's row (cb2[2], bytes 32..44): the overlap lands, the rest stands
        cel::Patch moved = p0;
        moved.c[0] += 1234.0f;
        const std::vector<uint8_t> blk = b2Block(moved);
        D3D11_BOX box{32, 0, 0, 48, 1, 1};
        celestialMotionConstantsWritten(b.b2.Get(), blk.data() + 32, &box);
        celestialMotionNoteDraw(d.ctx.Get());
        check(g_win.captured == 2 && g_eye[0].patches[g_eye[0].cur][1].c[0] == moved.c[0] &&
              std::memcmp(g_eye[0].patches[g_eye[0].cur][1].q, p0.q, 16) == 0,
              "a boxed write updates exactly the bytes it covers and keeps the shadow valid");
        // an unknown write: invalid until a write covers the whole segment; a partial one does not revalidate
        celestialMotionConstantsUnknownWrite(b.b2.Get());
        celestialMotionNoteDraw(d.ctx.Get());
        check(g_win.captured == 2 && g_win.declined[kDecNoB2] == 1, "after an unknown write the shadow is not trusted");
        celestialMotionConstantsWritten(b.b2.Get(), blk.data() + 32, &box);
        celestialMotionNoteDraw(d.ctx.Get());
        check(g_win.captured == 2 && g_win.declined[kDecNoB2] == 2, "a partial write onto an invalid shadow leaves it invalid");
        D3D11_BOX whole{0, 0, 0, cel::kB2Bytes, 1, 1};
        const std::vector<uint8_t> blk1 = b2Block(p1);
        celestialMotionConstantsWritten(b.b2.Get(), blk1.data(), &whole);
        celestialMotionNoteDraw(d.ctx.Get());
        check(g_win.captured == 3 && patchEquals(g_eye[0].patches[g_eye[0].cur][2], p1), "a write over the whole segment revalidates it");
        // a write whose box leaves the buffer is no write we can use
        D3D11_BOX out{0, 0, 0, 4096, 1, 1};
        celestialMotionConstantsWritten(b.b2.Get(), blk.data(), &out);
        celestialMotionNoteDraw(d.ctx.Get());
        check(g_win.declined[kDecNoB2] == 3, "a box past the buffer's size invalidates");
        ++cases;
    }
    // ---- the copies and command lists that write unseen ------------------------------------------------------------------
    {
        reset();
        Bufs b = makeBuffers(d, true);
        bind(d, b);
        celestialMotionNoteDraw(d.ctx.Get());
        mapWrite(d, b.b0.Get(), b0Block(p0));
        mapWrite(d, b.b2.Get(), b2Block(p0));
        celestialMotionNoteDraw(d.ctx.Get());
        check(g_win.captured == 1, "armed");
        celestialMotionConstantsUnknownWrite(b.b2.Get());   // hookedCopyResource / hookedCopySubresourceRegion into b2
        mapWrite(d, b.b0.Get(), b0Block(p0));
        celestialMotionNoteDraw(d.ctx.Get());
        check(g_win.captured == 1 && g_win.declined[kDecNoB2] == 1, "a copy into b2 invalidates its shadow");
        mapWrite(d, b.b2.Get(), b2Block(p1));
        celestialMotionNoteDraw(d.ctx.Get());
        check(g_win.captured == 2, "and a Map/Unmap after it brings it back");
        celestialMotionConstantsUnknownWrite(nullptr);       // hookedExecuteCommandList: the game ran a list
        celestialMotionNoteDraw(d.ctx.Get());
        check(g_win.captured == 2 && g_win.declined[kDecNoB0] == 1, "an executed command list invalidates every watched buffer (b0 reports first)");
        mapWrite(d, b.b0.Get(), b0Block(p0));
        mapWrite(d, b.b2.Get(), b2Block(p2));
        celestialMotionNoteDraw(d.ctx.Get());
        check(g_win.captured == 3, "both rewritten: read again");
        // a Map the tee saw but whose Unmap carried nothing to copy (no pointer): Unmap with no Map is unknown
        celestialMotionConstantsUnmapped(b.b2.Get());
        celestialMotionNoteDraw(d.ctx.Get());
        check(g_win.captured == 3 && g_win.declined[kDecNoB2] == 2, "an Unmap with no Map seen invalidates (nothing is known about what was written)");
        // CreateBuffer at a watched address: invalidated and re-verified at the next draw
        mapWrite(d, b.b2.Get(), b2Block(p0));
        celestialMotionConstantsUnknownWrite(b.b2.Get());
        check(findWatch(b.b2.Get()) && findWatch(b.b2.Get())->bytes == 0 && !findWatch(b.b2.Get())->valid, "a re-created buffer's address is unverified");
        ++cases;
    }
    // ---- sizes ---------------------------------------------------------------------------------------------------------------
    {
        reset();
        Bufs b = makeBuffers(d, true, 208, 112);   // a VS b2 of 112 bytes is no patch's
        bind(d, b);
        celestialMotionNoteDraw(d.ctx.Get());
        mapWrite(d, b.b0.Get(), b0Block(p0));
        mapWrite(d, b.b2.Get(), b2Block(p0, 112));
        celestialMotionNoteDraw(d.ctx.Get());
        check(g_win.captured == 0 && g_win.declined[kDecSize] == 1, "a VS b2 smaller than the rows a patch needs is declined by size");
        Bufs c = makeBuffers(d, true, 64, 944);    // a VS b0 too small for rows 9-11
        bind(d, c);
        celestialMotionNoteDraw(d.ctx.Get());
        mapWrite(d, c.b0.Get(), b0Block(p0, 64));
        mapWrite(d, c.b2.Get(), b2Block(p0));
        celestialMotionNoteDraw(d.ctx.Get());
        check(g_win.captured == 0 && g_win.declined[kDecSize] == 2, "a VS b0 smaller than rows 9-11 is declined by size");
        ++cases;
    }
    // ---- a block that is not a patch's -----------------------------------------------------------------------------------
    {
        reset();
        Bufs b = makeBuffers(d, true);
        bind(d, b);
        celestialMotionNoteDraw(d.ctx.Get());
        cel::Patch bad = p0;
        bad.c[1] = std::numeric_limits<float>::infinity();
        mapWrite(d, b.b0.Get(), b0Block(p0));
        mapWrite(d, b.b2.Get(), b2Block(bad));
        celestialMotionNoteDraw(d.ctx.Get());
        check(g_win.captured == 0 && g_win.declined[kDecConstants] == 1, "an infinite centre is not captured");
        ++cases;
    }
    // ---- the cap ---------------------------------------------------------------------------------------------------------
    {
        reset();
        Bufs b = makeBuffers(d, true);
        bind(d, b);
        celestialMotionNoteDraw(d.ctx.Get());
        mapWrite(d, b.b0.Get(), b0Block(p0));
        for (uint32_t i = 0; i < cel::kMaxPatches + 3; ++i) {
            cel::Patch p = p0;
            p.c[0] += 64.0f * float(i);   // p0 is a far body's patch (c ~ 2e8 m, one float step 16 m): steps of one metre would not even register
            mapWrite(d, b.b2.Get(), b2Block(p));
            celestialMotionNoteDraw(d.ctx.Get());
        }
        std::string why;
        for (uint32_t k = 0; k < kDecCount; ++k) why += fmt(" %s=%llu", kDeclineName[k], static_cast<unsigned long long>(g_win.declined[k]));
        check(g_win.captured == cel::kMaxPatches && g_win.declined[kDecCap] == 3, fmt("%u patches kept, the rest counted (captured %llu; declined%s)", cel::kMaxPatches,
              static_cast<unsigned long long>(g_win.captured), why.c_str()));
        ++cases;
    }
    // ---- the whole pipeline: draws of two frames on both eyes, then the pass's consumer ---------------------------------------
    {
        reset();
        Bufs b = makeBuffers(d, true);
        bind(d, b);
        celestialMotionNoteDraw(d.ctx.Get());   // register (the first draw of a session can read nothing)
        const uint32_t frame = 23654;
        auto drawFrame = [&](uint32_t f) {
            for (uint32_t eye = 0; eye < 2; ++eye) {
                g_stubDsv = g_stubEyeDsv[eye];
                const EyeFrame* ef = fx.find(f, eye);
                mapWrite(d, b.b0.Get(), b0Block(ef->patches[0]));
                for (const cel::Patch& p : ef->patches) {
                    mapWrite(d, b.b2.Get(), b2Block(p));
                    celestialMotionNoteDraw(d.ctx.Get());
                }
            }
        };
        drawFrame(frame - 1);
        celestialMotionFrameBoundary();
        drawFrame(frame);
        // the consumer, before the boundary, eye by eye
        for (uint32_t eye = 0; eye < 2; ++eye) {
            const cel::EyeInput in = eyeOf(fx, frame, eye);
            CelestialEyeRecords rec;
            check(celestialMotionRecords(d.ctx.Get(), static_cast<int>(eye), in.tan, in.w, in.h, &rec) && rec.srv != nullptr,
                  fmt("eye %u: the consumer returns the record buffer", eye));
            // six bodies drawn, the moon alone in front of the eye: its six faces are what is matched (the five behind it are skipped first)
            check(rec.records == 1 && rec.bodies == 6 && rec.patches == 36 && rec.matched == 6, fmt("eye %u: one record, six bodies, 36 patches (records %u bodies %u patches %u matched %u)",
                  eye, rec.records, rec.bodies, rec.patches, rec.matched));
            // the same patches through the pure build: the same records, to the bit
            cel::BuildResult direct;
            cel::build(fx.find(frame, eye)->patches.data(), 36, fx.find(frame - 1, eye)->patches.data(), 36, in, direct, scratch());
            check(g_eye[eye].result.records == direct.records && std::memcmp(g_eye[eye].result.gpu, direct.gpu, sizeof direct.gpu) == 0,
                  fmt("eye %u: the module's records are the pure build's, to the bit", eye));
            // and the GPU buffer holds them
            ComPtr<ID3D11Resource> res;
            rec.srv->GetResource(&res);
            ComPtr<ID3D11Buffer> buf;
            hr(res.As(&buf), "the SRV's buffer");
            const std::vector<uint8_t> bytes = readBuffer(d, buf.Get());
            check(bytes.size() == sizeof direct.gpu && std::memcmp(bytes.data(), direct.gpu, sizeof direct.gpu) == 0, fmt("eye %u: the uploaded buffer is the records", eye));
            check(std::fabs(rec.translation[0] - direct.body[direct.order[0]].t[0]) < 1e-9 && rec.distance == direct.body[direct.order[0]].nearest, "the trace's nearest body is the first record");
            // asked again in the same frame: the same answer, no rebuild
            const uint64_t builds = g_win.eyeFrames;
            CelestialEyeRecords again;
            check(celestialMotionRecords(d.ctx.Get(), static_cast<int>(eye), in.tan, in.w, in.h, &again) && again.srv == rec.srv && g_win.eyeFrames == builds,
                  "a second ask in a frame answers from the first");
        }
        check(g_win.uploads == 2 && g_win.records == 2, "one upload and one record per eye");
        // the 5 s census, with everything in it: three lines now (the capture, the consumer, then the records, the tee, the cost and the gate)
        g_logLines.clear();
        report();
        check(g_logLines.size() == 3, fmt("the census is three lines (%zu)", g_logLines.size()));
        const std::string line1 = g_logLines[0], line2 = g_logLines[1], line3 = g_logLines[2];
        check(line1.rfind("celestial motion 5s: ", 0) == 0, "the census line starts 'celestial motion 5s: '");
        check(line2.rfind("celestial motion 5s (2/3): ", 0) == 0 && line3.rfind("celestial motion 5s (3/3): ", 0) == 0, "its later parts start 'celestial motion 5s (2/3): ' and '(3/3): '");
        check(line1.size() < 800 && line2.size() < 800 && line3.size() < 800, fmt("each part of the census is well under the 1000-character budget (%zu, %zu, %zu bytes)", line1.size(), line2.size(), line3.size()));
        std::printf("    the census of two eye-frames of real constants (the CPU figures are this rig's, WARP, one core):\n    %s\n    %s\n    %s\n", line1.c_str(), line2.c_str(), line3.c_str());
        for (const char* want : {"draws=145", "captured=144", "gated-off=0", "off-eye=0 unwatched=1"})
            check(line1.find(want) != std::string::npos, fmt("the census line carries '%s': %s", want, line1.c_str()));
        for (const char* want : {"eye-frames=2", "patches/frame=36.0", "bodies/frame=6.0", "behind=10", "no-previous-frame=0"})
            check(line2.find(want) != std::string::npos, fmt("the census line's second part carries '%s': %s", want, line2.c_str()));
        for (const char* want : {"records=2", "uploads=2", "0 with a shell", "pixels=n/a (diagnostics off)", "tee[map=", "cpu[capture=", "gate=on (0 of "})
            check(line3.find(want) != std::string::npos, fmt("the census line's third part carries '%s': %s", want, line3.c_str()));
        celestialMotionNotePixels(1234);
        g_win.frames = 1;
        report();
        check(g_logLines[g_logLines.size() - 1].find("pixels=1234") != std::string::npos, "the pass's pixel count reaches the line");
        // a frame later the eyes' captures are stale: no records until they draw again
        celestialMotionFrameBoundary();
        celestialMotionFrameBoundary();
        CelestialEyeRecords stale;
        const cel::EyeInput in = eyeOf(fx, frame, 0);
        check(!celestialMotionRecords(d.ctx.Get(), 0, in.tan, in.w, in.h, &stale) && stale.srv == nullptr, "a capture from an earlier frame is not this frame's");
        ++cases;
    }
    // ---- a frame with no previous: the fallback is counted and named once ------------------------------------------------------
    {
        reset();
        Bufs b = makeBuffers(d, true);
        bind(d, b);
        celestialMotionNoteDraw(d.ctx.Get());
        const EyeFrame* ef = fx.find(23654, 0);
        mapWrite(d, b.b0.Get(), b0Block(ef->patches[0]));
        for (const cel::Patch& p : ef->patches) {
            mapWrite(d, b.b2.Get(), b2Block(p));
            celestialMotionNoteDraw(d.ctx.Get());
        }
        const cel::EyeInput in = eyeOf(fx, 23654, 0);
        CelestialEyeRecords rec;
        check(!celestialMotionRecords(d.ctx.Get(), 0, in.tan, in.w, in.h, &rec) && rec.srv == nullptr, "no previous frame: no records");
        check(g_win.fallbacks[cel::kFbNoPrevFrame] == 1 && g_win.eyeFrames == 1, "no previous frame: counted by name");
        bool said = false;
        for (const std::string& l : g_logLines) said = said || l.find("no-previous-frame") != std::string::npos;
        check(said, "the first body without a record says why");
        ++cases;
    }
    // ---- the module off, and the module that never ran ---------------------------------------------------------------------------
    {
        reset();
        celestialMotionConfigure(false);
        Bufs b = makeBuffers(d, true);
        bind(d, b);
        check(!celestialMotionLive(), "configured off: not live");
        celestialMotionNoteDraw(d.ctx.Get());
        celestialMotionFrameBoundary();
        const cel::EyeInput in = eyeOf(fx, 23654, 0);
        CelestialEyeRecords rec;
        check(g_win.draws == 0 && !celestialMotionRecords(d.ctx.Get(), 0, in.tan, in.w, in.h, &rec), "off: no capture, no records, nothing counted");
        celestialMotionConfigure(true);
        celestialMotionNoteStatus(true, true);
        g_logLines.clear();
        report();
        check(g_logLines.size() == 3 && g_logLines[0].find("draws=0 captured=0") != std::string::npos && g_logLines[2].find("records=0") != std::string::npos,
              "live with no patch draws: the line says draws=0 (a line that never appears means the module never ran)");
        ++cases;
    }
    std::printf("(d) %u cases of the CPU tee: Map/Unmap, UpdateSubresource whole and boxed, the copies and command lists that invalidate, sizes, the cap, "
                "the pipeline to the GPU buffer, the census line\n", cases);
}

}  // namespace tee
