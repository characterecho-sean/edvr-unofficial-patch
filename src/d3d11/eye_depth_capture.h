#pragma once
// The eye run's completed depth, per eye per frame (advanced.eye_depth_capture).
// Nothing on screen captures depth today: the eye dumps are colour-only. This
// instrument pairs with an armed eye run and, for each of the two eye passes,
// copies the pass's depth-stencil texture the moment the pass ends -- the
// first eye draw whose DSV differs from the pass's is, by command-stream
// order, after every draw of the ended pass and before anything new touches
// its texture, so the staged copy holds the depth every submitted draw left
// behind. The readback and the disk write happen at the ledger's own late
// sync point (writeLedger), with the same no-wait Map and explicit failure
// counts the draw snapshot uses.
//
// Eye A/B is the frame's depth targets in first-seen (scene) order -- the
// same ordering depth_probe's scene pick calls first/second.
//
// The constants block is VS b1 floats [256, 592) from the pass's FIRST
// pool-carrying draw (the ledger's own t33==g_pool test): view-projection
// rows live at cb1[270..273], the camera-relative origin at cb1[275]; the
// 336-float window survives small layout shifts. A pass with no pool draw
// keeps an explicit zero-float constants block.
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace edvr {
class EyeDepthCapture {
    using Texture = Microsoft::WRL::ComPtr<ID3D11Texture2D>;
    using Buffer = Microsoft::WRL::ComPtr<ID3D11Buffer>;
    struct PendingConst {   // captured at the first pool draw, joined at pass end
        Buffer stage;
        uint32_t frame = 0, eye = 0, bytes = 0, whole = 0;
        bool live = false;
    };
    struct Record {
        Texture stage;   // the pass-end copy; the source texture is not held past enqueue
        Buffer constStage;
        uint32_t frame = 0, eye = 0, width = 0, height = 0, format = 0;
        uint32_t bytes = 0, constBytes = 0, constWhole = 0;
    };
    std::vector<Record> records_;
    PendingConst pending_[2];   // per eye slot, this frame
    bool on_ = false;
    // Per-frame DSV interning, by pointer, in first-seen (scene) order --
    // the same interning model the census uses for views, without its table.
    ID3D11DepthStencilView* slotDsv_[2] = {};
    int slotCount_ = 0;
    uint32_t frame_ = 0;
    ID3D11DepthStencilView* lastDsv_ = nullptr;   // the open pass
    uint32_t lastFrame_ = 0;
    int lastSlot_ = -1;
    bool openEnqueued_ = false;   // the open pass's record is staged

    void newFrame(uint32_t frame) {
        frame_ = frame;
        slotCount_ = 0;
        slotDsv_[0] = slotDsv_[1] = nullptr;
        // A pending constants block belongs to one frame; its pass either
        // joined a record at enqueue or never ended, so a new frame starts
        // clean rather than letting a stale block block this one. The join
        // happens in noteEyeDraw BEFORE this runs: the pass that ended on
        // this draw may belong to the frame that is closing.
        pending_[0] = PendingConst{};
        pending_[1] = PendingConst{};
    }
    void resetSlots() {
        newFrame(0);
        lastDsv_ = nullptr;
        lastFrame_ = 0;
        lastSlot_ = -1;
        openEnqueued_ = false;
    }
    // The mapped-row copy runs under SEH: a faulting staging pointer must
    // count, not crash the late write. POD locals only -- /EHs units cannot
    // unwind C++ objects through __try (kinematic_eval_hook.cpp's rule).
    static bool guardedRows(FILE* f, const uint8_t* p, int64_t pitch, uint32_t row,
                            uint32_t rows, uint32_t* faults) {
        bool ok = true;
        __try {
            for (uint32_t y = 0; y < rows; ++y)
                ok = fwrite(p + static_cast<size_t>(y) * pitch, 1, row, f) == row && ok;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { ++*faults; ok = false; }
        return ok;
    }
    static bool guardedBytes(FILE* f, const void* p, uint32_t bytes, uint32_t* faults) {
        bool ok = true;
        __try { ok = fwrite(p, 1, bytes, f) == bytes; }
        __except (EXCEPTION_EXECUTE_HANDLER) { ++*faults; ok = false; }
        return ok;
    }
    void enqueue(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, uint32_t frame, int eye) {
        openEnqueued_ = true;
        bool keptFrame = false;
        uint32_t keptFrames = 0;
        for (size_t i = 0; i < records_.size(); ++i) {
            const Record& r = records_[i];
            if (r.frame == frame) { keptFrame = true; continue; }
            bool counted = false;
            for (size_t k = 0; k < i; ++k) counted = counted || records_[k].frame == r.frame;
            if (!counted) ++keptFrames;
        }
        if (!keptFrame && keptFrames >= kMaxFrames) { ++declined; return; }
        Microsoft::WRL::ComPtr<ID3D11Resource> res;
        dsv->GetResource(&res);
        Texture tex;
        if (!res || FAILED(res.As(&tex))) { ++failures; return; }
        D3D11_TEXTURE2D_DESC td{};
        tex->GetDesc(&td);
        // The census reports the eye pair as D32_FLOAT. The depth-reading
        // copy wants the plain 4-byte family; anything else (MSAA, the
        // 8-byte S8 layouts) is declined explicitly, never half-copied.
        const uint64_t bytes = static_cast<uint64_t>(td.Width) * td.Height * 4;
        if ((td.Format != DXGI_FORMAT_R32_TYPELESS && td.Format != DXGI_FORMAT_D32_FLOAT) ||
            td.SampleDesc.Count != 1 || td.ArraySize != 1 || !bytes ||
            bytes > kMaxTextureBytes || bytes > kMaxTotalBytes - bytes_) {
            ++declined;
            return;
        }
        Microsoft::WRL::ComPtr<ID3D11Device> dev;
        ctx->GetDevice(&dev);
        if (!dev) { ++failures; return; }
        Record r;
        r.frame = frame;
        r.eye = static_cast<uint32_t>(eye);
        r.width = td.Width;
        r.height = td.Height;
        r.format = static_cast<uint32_t>(td.Format);
        r.bytes = static_cast<uint32_t>(bytes);
        td.MipLevels = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = td.MiscFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &r.stage))) { ++failures; return; }
        ctx->CopySubresourceRegion(r.stage.Get(), 0, 0, 0, 0, tex.Get(), 0, nullptr);
        bytes_ += bytes;
        if (pending_[eye].live && pending_[eye].frame == frame) {
            r.constStage = pending_[eye].stage;
            r.constBytes = pending_[eye].bytes;
            r.constWhole = pending_[eye].whole;
        }
        records_.push_back(std::move(r));
    }
public:
    static constexpr uint32_t kConstFirstFloat = 256;  // VS b1 floats from here...
    static constexpr uint32_t kConstFloats = 336;      // ...for this many
    static constexpr uint32_t kConstBytes = kConstFloats * 4;
    static constexpr uint32_t kMaxFrames = 4;          // armed frames kept, from the run's first
    static constexpr uint64_t kMaxTotalBytes = 192ull * 1024 * 1024;
    static constexpr uint64_t kMaxTextureBytes = 64ull * 1024 * 1024;
    uint32_t declined = 0, failures = 0, faults = 0;
    bool enabled() const { return on_; }
    void configure(bool on) {
        on_ = on;
        if (!on_) reset();
    }
    void reset() {
        records_.clear();
        pending_[0] = PendingConst{};
        pending_[1] = PendingConst{};
        resetSlots();
        declined = 0;
        failures = 0;
        faults = 0;
        bytes_ = 0;
    }
    uint32_t count() const { return static_cast<uint32_t>(records_.size()); }
    uint64_t bytes() const { return bytes_; }
    // One eye draw of an armed frame. `dsv` is the draw's depth-stencil view
    // (the caller's single OMGetRenderTargets, which the ledger already runs
    // for the RTV); `poolDraw` is the ledger's own t33==g_pool verdict for
    // the draw. One bool while off; a pointer compare per draw while on.
    void noteEyeDraw(ID3D11DeviceContext* ctx, uint32_t frame, ID3D11DepthStencilView* dsv, bool poolDraw) {
        if (!on_ || !ctx || !dsv) return;
        // The open pass ended: its DSV just changed and nothing new for its
        // texture is enqueued before this point in the stream. This runs
        // BEFORE the frame rolls below, while the ended pass's pending
        // constants still belong to the closing frame.
        if (dsv != lastDsv_ && lastDsv_ && !openEnqueued_ && lastSlot_ >= 0)
            enqueue(ctx, lastDsv_, lastFrame_, lastSlot_);
        if (frame != frame_) newFrame(frame);
        int slot = -1;
        for (int i = 0; i < slotCount_; ++i)
            if (slotDsv_[i] == dsv) { slot = i; break; }
        if (slot < 0) {
            // A third distinct depth target in one frame is not the two-eye
            // scene (a HUD phase with its own depth, say): decline it rather
            // than guess which pass it belongs to.
            if (slotCount_ >= 2) { ++declined; return; }
            slot = slotCount_++;
            slotDsv_[slot] = dsv;
        }
        if (dsv != lastDsv_) {
            lastDsv_ = dsv;
            lastSlot_ = slot;
            openEnqueued_ = false;
        }
        lastFrame_ = frame;
        if (!poolDraw || pending_[slot].live) return;
        // First pool-carrying draw of this pass: keep VS b1's frame block.
        // cb1[256..592) holds the view-projection rows (270..273) and the
        // camera-relative origin (275) with room for a small layout shift.
        ID3D11Buffer* b1 = nullptr;
        ctx->VSGetConstantBuffers(1, 1, &b1);
        if (!b1) return;
        D3D11_BUFFER_DESC bd{};
        b1->GetDesc(&bd);
        const uint32_t off = kConstFirstFloat * 4;
        if (bd.ByteWidth >= off + kConstBytes) {
            Microsoft::WRL::ComPtr<ID3D11Device> dev;
            ctx->GetDevice(&dev);
            if (dev) {
                Buffer stage;
                D3D11_BUFFER_DESC sd = bd;
                sd.ByteWidth = kConstBytes;
                sd.Usage = D3D11_USAGE_STAGING;
                sd.BindFlags = sd.MiscFlags = sd.StructureByteStride = 0;
                sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                if (SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &stage))) {
                    D3D11_BOX box{off, 0, 0, off + kConstBytes, 1, 1};
                    ctx->CopySubresourceRegion(stage.Get(), 0, 0, 0, 0, b1, 0, &box);
                    pending_[slot].stage = stage;
                    pending_[slot].frame = frame;
                    pending_[slot].bytes = kConstBytes;
                    pending_[slot].whole = bd.ByteWidth;
                    pending_[slot].live = true;
                } else ++failures;
            }
        }
        b1->Release();
    }
    // The ledger's late sync point: no waits, unavailable copies write an
    // explicit zero-byte payload and count a failure. One file per eye per
    // kept frame: depth_<stamp>_f<frame>_<A|B>.bin.
    uint32_t write(ID3D11DeviceContext* ctx, const wchar_t* dir, const wchar_t* stamp) {
        if (!ctx || !dir || !stamp) return 0;
        uint32_t written = 0;
        wchar_t path[MAX_PATH];
        for (Record& r : records_) {
            _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\depth_%s_f%u_%c.bin", dir, stamp,
                         r.frame, r.eye ? 'B' : 'A');
            FILE* f = nullptr;
            if (_wfopen_s(&f, path, L"wb") || !f) { ++failures; continue; }
            bool ok = fwrite("EDVRDEPT", 1, 8, f) == 8;
            auto u32 = [&](uint32_t v) { ok = fwrite(&v, 4, 1, f) == 1 && ok; };
            u32(1);                       // version
            u32(r.frame);
            u32(r.eye);
            u32(r.width);
            u32(r.height);
            u32(r.format);
            const uint32_t fileFaults = faults;
            D3D11_MAPPED_SUBRESOURCE mc{};
            const bool constOk = r.constBytes && r.constStage &&
                                 SUCCEEDED(ctx->Map(r.constStage.Get(), 0, D3D11_MAP_READ,
                                                    D3D11_MAP_FLAG_DO_NOT_WAIT, &mc)) && mc.pData;
            u32(constOk ? r.constBytes / 4 : 0);
            if (constOk) {
                ok = guardedBytes(f, mc.pData, r.constBytes, &faults) && ok;
                ctx->Unmap(r.constStage.Get(), 0);
            } else if (r.constBytes) ++failures;
            D3D11_MAPPED_SUBRESOURCE m{};
            const bool mapped = r.stage &&
                                SUCCEEDED(ctx->Map(r.stage.Get(), 0, D3D11_MAP_READ,
                                                   D3D11_MAP_FLAG_DO_NOT_WAIT, &m)) &&
                                m.pData && m.RowPitch >= static_cast<int>(r.width * 4);
            u32(mapped ? r.bytes : 0);
            if (mapped) {
                ok = guardedRows(f, static_cast<const uint8_t*>(m.pData), m.RowPitch, r.width * 4,
                                 r.height, &faults) && ok;
                ctx->Unmap(r.stage.Get(), 0);
            } else ++failures;
            u32(faults - fileFaults);
            ok = !ferror(f) && ok;
            if (fclose(f) != 0 || !ok) ++failures;
            else ++written;
        }
        return written;
    }
private:
    uint64_t bytes_ = 0;
};
} // namespace edvr
