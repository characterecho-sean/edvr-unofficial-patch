#pragma once

// Draw-time evidence for an explicitly armed eye run. Celestial draws use
// cb0[4..7] for clip position, outside the instance pool; dim holo strokes
// need the original t2 alpha to distinguish them from the panel background.
// Keep each draw (including both eyes and repeated VS hashes). Buffer
// addresses identify bindings within a run, never objects across frames.
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <utility>
#include <map>
#include <mutex>

namespace edvr {
class EyeDrawSnapshot {
    using Buffer = Microsoft::WRL::ComPtr<ID3D11Buffer>;
    using Texture = Microsoft::WRL::ComPtr<ID3D11Texture2D>;
public:
    static constexpr uint32_t kMaxDraws = 4096;
    static constexpr uint32_t kMaxTextures = 24;
    static constexpr uint32_t kMaxTextureBytes = 16 * 1024 * 1024;
    static constexpr uint32_t kTotalTextureBytes = 64 * 1024 * 1024;
    static constexpr uint64_t kHolo = 0x81216C77F90DEDD6ull;
    static constexpr uint64_t kHud = 0xB7790CBFC6554097ull, kSprite=0xE508648660A352B2ull;
    static constexpr uint64_t kPanel=0xA888D51024D9798Eull,kScreen=0x4EF6DDB075A927FAull;
    static bool watches(uint64_t vs) {
        switch (vs) {
        case kHolo:
        case kHud:
        case kSprite:
        case kPanel:
        case kScreen:
        case 0xACE405F428C17EF6ull: // matching 2304/104448-index depth/colour draws
        case 0x72BDD292154158ADull:
        case 0x19F70CE80DA3242Bull: // sphere draw using cb0[9..11], cb1[270..273]
        case 0x26FC402B1274EE7Bull: // planetary pipeline, including the surface
        case 0x9FFA5D5E79F04873ull:
        case 0xB12F7A618E1BDE98ull:
        case 0x203DF51758AADC4Dull:
        case 0xC7FA0C0F5DD49180ull: // six 8194-vertex instances beside the bodies
        case 0xA47A3315FFF5E2E4ull: // line candidate; identity is not yet proven
            return true;
        default: return false;
        }
    }
    struct Draw {
        uint64_t vs = 0, ps = 0, target = 0;
        uint32_t frame = 0, ordinal = 0, kind = 0, count = 0, instances = 0, startInstance = 0;
        uint32_t width = 0, height = 0, texture = UINT32_MAX;
        uint64_t source[4] = {}; // VS b0,b1,b2; PS b2
        uint32_t whole[4] = {}, copied[4] = {};
        Buffer stage[4];
        uint32_t start=0;int32_t base=0;
        struct Stream {uint32_t offset=0,stride=0,whole=0,copied=0,captureOffset=0;Buffer stage;};
        Stream streams[3]; // VB0 instance IDs, VB1 packed vertices, index buffer
    };
    struct Surface {
        Texture source, stage;
        uint32_t frame = 0, width = 0, height = 0, format = 0, bytes = 0;
    };
    std::vector<Draw> draws;
    std::vector<Surface> surfaces;
    uint32_t dropped = 0, failures = 0, textureBytes = 0;
    uint32_t firstFrame=0,vertexBytes=0,vertexDraws=0,vertexDeclined=0;

    // The game may create these before a dump is armed. Retain only this
    // bounded VS set, then write only shaders seen in the requested run.
    // No broad shader-dump setting or startup disk writes are necessary.
    static void rememberShader(uint64_t hash, const void* bytes, size_t size) {
        if (!watches(hash) || !bytes || !size || size > 256*1024) return;
        std::lock_guard<std::mutex> lock(shaderMutex());
        auto& shaders = shaderBytes();
        if (shaders.count(hash)) return;
        const auto* p = static_cast<const uint8_t*>(bytes);
        shaders[hash] = std::vector<uint8_t>(p, p + size);
    }
    uint32_t writeShaders(const wchar_t* directory) const {
        std::lock_guard<std::mutex> lock(shaderMutex());
        uint32_t missing = 0;
        std::map<uint64_t, bool> seen;
        for (const Draw& d : draws) {
            if (!seen.emplace(d.vs, true).second) continue;
            const auto it = shaderBytes().find(d.vs);
            if (it == shaderBytes().end()) { ++missing; continue; }
            wchar_t path[MAX_PATH];
            _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\vs_%016llX.dxbc", directory,
                          static_cast<unsigned long long>(d.vs));
            FILE* f = nullptr;
            if (_wfopen_s(&f, path, L"wb") || !f) { ++missing; continue; }
            const bool ok = fwrite(it->second.data(), 1, it->second.size(), f) == it->second.size();
            const int closed = fclose(f);
            if (!ok || closed) ++missing;
        }
        return missing;
    }

    void reset() {
        draws.clear(); surfaces.clear(); dropped = failures = textureBytes = 0;
        firstFrame=vertexBytes=vertexDraws=vertexDeclined=0;
    }

    void capture(ID3D11DeviceContext* ctx, uint32_t frame, uint32_t ordinal,
                 uint64_t vs, uint64_t ps, char kind, uint32_t count,
                 uint32_t instances, uint32_t startInstance,uint32_t start=0,int32_t base=0) {
        if (!ctx || !watches(vs)) return;
        if (draws.size() >= kMaxDraws) { ++dropped; return; }
        Draw d;
        d.frame = frame; d.ordinal = ordinal; d.vs = vs; d.ps = ps;
        d.kind = static_cast<uint8_t>(kind); d.count = count;
        d.instances = instances; d.startInstance = startInstance;
        d.start=start;d.base=base;if(!firstFrame)firstFrame=frame;
        Microsoft::WRL::ComPtr<ID3D11Device> dev; ctx->GetDevice(&dev);
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rt;
        ctx->OMGetRenderTargets(1, &rt, nullptr);
        if (rt) {
            Microsoft::WRL::ComPtr<ID3D11Resource> res; rt->GetResource(&res);
            Texture tex;
            if (SUCCEEDED(res.As(&tex))) {
                D3D11_TEXTURE2D_DESC td{}; tex->GetDesc(&td);
                d.target = reinterpret_cast<uint64_t>(tex.Get());
                d.width = td.Width; d.height = td.Height;
            }
        }
        ID3D11Buffer* buffers[4] = {};
        ctx->VSGetConstantBuffers(0, 3, buffers);
        ctx->PSGetConstantBuffers(2, 1, buffers + 3);
        for (int i = 0; i < 4; ++i) {
            Buffer src; src.Attach(buffers[i]);
            if (!src) continue;
            d.source[i] = reinterpret_cast<uint64_t>(src.Get());
            D3D11_BUFFER_DESC bd{}; src->GetDesc(&bd);
            d.whole[i] = bd.ByteWidth;
            const uint32_t cap = i == 0 ? 1024u : 8192u;
            bd.ByteWidth = bd.ByteWidth < cap ? bd.ByteWidth : cap;
            bd.Usage = D3D11_USAGE_STAGING; bd.BindFlags = bd.MiscFlags = bd.StructureByteStride = 0;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(dev->CreateBuffer(&bd, nullptr, &d.stage[i]))) { ++failures; continue; }
            D3D11_BOX box{0, 0, 0, bd.ByteWidth, 1, 1};
            ctx->CopySubresourceRegion(d.stage[i].Get(), 0, 0, 0, 0, src.Get(), 0, &box);
            d.copied[i] = bd.ByteWidth;
        }
        if (vs==kHolo || vs==kSprite || vs==kPanel || vs==kScreen)
            d.texture=captureSurface(ctx,dev.Get(),frame,vs==kHolo?2:vs==kPanel?1:0);
        // Target labels and vector widgets can move inside their dynamic
        // vertex streams. Preserve each draw, not the first binding of a VS.
        // Three frames and 32 MiB bound this explicit diagnostic's cost.
        if((vs==kHud || vs==kSprite) && frame-firstFrame<3) {
            ID3D11Buffer* raw[3]{};UINT strides[2]{},offsets[2]{},ibOffset=0;DXGI_FORMAT fmt{};
            ctx->IAGetVertexBuffers(0,2,raw,strides,offsets);ctx->IAGetIndexBuffer(raw+2,&fmt,&ibOffset);
            bool copied=false;
            for(int i=0;i<3;++i) {
                Buffer src;src.Attach(raw[i]);if(!src)continue;
                auto& s=d.streams[i];s.offset=i==2?ibOffset:offsets[i];s.stride=i==2?(fmt==DXGI_FORMAT_R16_UINT?2u:fmt==DXGI_FORMAT_R32_UINT?4u:0u):strides[i];
                D3D11_BUFFER_DESC bd{};src->GetDesc(&bd);s.whole=bd.ByteWidth;
                if(s.offset>=s.whole || !s.stride)continue;
                // These watched packed families use VB0 for per-instance
                // IDs and VB1 for vertices. Shared buffers can place a
                // six-index sprite megabytes beyond the binding offset.
                // Keep original bindings separately from the copied window.
                const bool indexed=kind=='X'||kind=='I';
                uint64_t begin=s.offset;
                if(i==2 && indexed)begin+=uint64_t(start)*s.stride;
                if(i==1)begin+=uint64_t(base>0?base:0)*s.stride;
                if(begin>=s.whole){++vertexDeclined;continue;}
                s.captureOffset=static_cast<uint32_t>(begin);
                UINT bytes=s.whole-s.captureOffset;if(bytes>256*1024)bytes=256*1024;
                if(i==2 && indexed && uint64_t(count)*s.stride<bytes)bytes=count*s.stride;
                if(!bytes)continue;
                if(vertexBytes+bytes>32*1024*1024){++vertexDeclined;continue;}
                bd.ByteWidth=bytes;bd.Usage=D3D11_USAGE_STAGING;bd.BindFlags=bd.MiscFlags=bd.StructureByteStride=0;bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                if(FAILED(dev->CreateBuffer(&bd,nullptr,&s.stage))){++failures;continue;}
                D3D11_BOX box{s.captureOffset,0,0,s.captureOffset+bytes,1,1};ctx->CopySubresourceRegion(s.stage.Get(),0,0,0,0,src.Get(),0,&box);
                s.copied=bytes;vertexBytes+=bytes;copied=true;
            }
            vertexDraws+=copied?1u:0u;
        }
        draws.push_back(std::move(d));
    }

    // Called after the ledger's readback grace period. No waits/flushes:
    // unavailable copies get zero payload and an explicit failure count.
    // On-disk integers are little-endian, individually written (no ABI padding).
    bool write(ID3D11DeviceContext* ctx, const wchar_t* path) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, path, L"wb") || !f) return false;
        bool ok = fwrite("EDVRDRW1", 1, 8, f) == 8;
        auto u32 = [&](uint32_t v) { ok = fwrite(&v, 4, 1, f) == 1 && ok; };
        auto u64 = [&](uint64_t v) { ok = fwrite(&v, 8, 1, f) == 1 && ok; };
        u32(3); u32(static_cast<uint32_t>(draws.size())); u32(static_cast<uint32_t>(surfaces.size())); u32(dropped);
        auto payload = [&](ID3D11Resource* resource, uint32_t bytes, uint32_t row, uint32_t height) {
            D3D11_MAPPED_SUBRESOURCE m{};
            const bool mapped = resource && SUCCEEDED(ctx->Map(resource, 0, D3D11_MAP_READ,
                                                 D3D11_MAP_FLAG_DO_NOT_WAIT, &m));
            const bool valid = mapped && m.pData && (!row || m.RowPitch >= row);
            u32(valid ? bytes : 0);
            if (valid) {
                if (!row) ok = fwrite(m.pData, 1, bytes, f) == bytes && ok;
                else for (uint32_t y = 0; y < height; ++y)
                    ok = fwrite(static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch,
                                1, row, f) == row && ok;
            } else if (bytes) ++failures;
            if (mapped) ctx->Unmap(resource, 0);
        };
        for (Draw& d : draws) {
            u64(d.vs); u64(d.ps); u64(d.target);
            u32(d.frame); u32(d.ordinal); u32(d.kind); u32(d.count); u32(d.instances); u32(d.startInstance);
            u32(d.width); u32(d.height); u32(d.texture);
            for (int i = 0; i < 4; ++i) {
                u64(d.source[i]); u32(d.whole[i]);
                payload(d.stage[i].Get(), d.copied[i], 0, 0);
            }
            u32(d.start);u32(static_cast<uint32_t>(d.base));
            for(auto& s:d.streams){u32(s.offset);u32(s.stride);u32(s.whole);u32(s.captureOffset);payload(s.stage.Get(),s.copied,0,0);}
        }
        for (Surface& s : surfaces) {
            u32(s.frame); u32(s.width); u32(s.height); u32(s.format);
            payload(s.stage.Get(), s.bytes, s.height ? s.bytes / s.height : 0, s.height);
        }
        u32(failures);
        ok = !ferror(f) && ok;
        return fclose(f) == 0 && ok;
    }

private:
    static std::mutex& shaderMutex() { static std::mutex m; return m; }
    static std::map<uint64_t, std::vector<uint8_t>>& shaderBytes() {
        static std::map<uint64_t, std::vector<uint8_t>> s; return s;
    }
    uint32_t captureSurface(ID3D11DeviceContext* ctx, ID3D11Device* dev, uint32_t frame,UINT slot) {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        ctx->PSGetShaderResources(slot, 1, &srv);
        if (!srv) return UINT32_MAX;
        Microsoft::WRL::ComPtr<ID3D11Resource> res; srv->GetResource(&res);
        Texture tex;
        if (FAILED(res.As(&tex))) { ++failures; return UINT32_MAX; }
        for (uint32_t i = 0; i < surfaces.size(); ++i)
            if (surfaces[i].source.Get() == tex.Get()) return i;
        D3D11_TEXTURE2D_DESC td{}; tex->GetDesc(&td);
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; srv->GetDesc(&sd);
        uint32_t bpp = 0;
        switch (sd.Format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: bpp = 4; break;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: bpp = 8; break;
        default: break;
        }
        const uint64_t bytes = static_cast<uint64_t>(td.Width) * td.Height * bpp;
        if (!bpp || td.SampleDesc.Count != 1 || td.ArraySize != 1 ||
            sd.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || sd.Texture2D.MostDetailedMip != 0 ||
            bytes > kMaxTextureBytes || bytes + textureBytes > kTotalTextureBytes || surfaces.size() >= kMaxTextures) {
            ++failures; return UINT32_MAX;
        }
        Surface s; s.source = tex; s.frame = frame; s.width = td.Width; s.height = td.Height;
        s.format = sd.Format; s.bytes = static_cast<uint32_t>(bytes);
        td.MipLevels = 1; td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = td.MiscFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &s.stage))) { ++failures; return UINT32_MAX; }
        ctx->CopySubresourceRegion(s.stage.Get(), 0, 0, 0, 0, tex.Get(), 0, nullptr);
        textureBytes += s.bytes;
        surfaces.push_back(std::move(s));
        return static_cast<uint32_t>(surfaces.size() - 1);
    }
};
} // namespace edvr
