#pragma once

// A late color overlay is removed from the temporal input by taking a clean
// copy of H before its first draw. MRT7 records the fragments that survive
// the game's own PS, raster, depth and stencil tests. The runtime must prove
// that every following H write up to the consumer is one of these draws.
#include "dxbc_flat_overlay.h"
#include "flat_compute_readback.h"
#include <d3d11_1.h>
#include <wrl/client.h>
#include <array>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace edvr {
class FlatOverlayLayer {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    struct Shader {
        Ptr<ID3D11PixelShader> original; // retain identity; a COM pointer cannot be reused while cached
        std::vector<BYTE> bytes;
        Ptr<ID3D11PixelShader> patched;
    };
    struct Registry {
        std::mutex mutex;
        std::unordered_map<ID3D11PixelShader*, Shader> shaders;
        size_t bytes = 0;
    };
    struct Blend {
        Ptr<ID3D11BlendState> original; // retain identity as for shaders
        Ptr<ID3D11BlendState> derived;
    };
    struct Saved {
        std::array<Ptr<ID3D11RenderTargetView>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> rtv;
        Ptr<ID3D11DepthStencilView> dsv;
        Ptr<ID3D11PixelShader> ps;
        Ptr<ID3D11BlendState> blend;
        Ptr<ID3D11DeviceContext> context;
        FLOAT factors[4]{};
        UINT sampleMask = ~0u;
    };
    static Registry& registry() { static Registry r; return r; }
    static bool& creatingPatched() { static thread_local bool b = false; return b; }
    static const GUID& reasonKey() {
        static const GUID key = {0xc8881753,0x101c,0x4ac8,{0x9f,0x37,0x44,0xe9,0x58,0x5a,0x0f,0x91}};
        return key;
    }
    static void markShaderReason(ID3D11PixelShader* shader, const char* text) {
        char reasonText[96]{};
        strncpy_s(reasonText, text ? text : "unsupported-PS", _TRUNCATE);
        shader->SetPrivateData(reasonKey(), UINT(std::strlen(reasonText) + 1), reasonText);
    }
    static bool src1Blend(D3D11_BLEND v) {
        return v == D3D11_BLEND_SRC1_COLOR || v == D3D11_BLEND_INV_SRC1_COLOR ||
               v == D3D11_BLEND_SRC1_ALPHA || v == D3D11_BLEND_INV_SRC1_ALPHA;
    }
    static bool validateBlend(const D3D11_BLEND_DESC& d) {
        for (const auto& t : d.RenderTarget)
            if (src1Blend(t.SrcBlend) || src1Blend(t.DestBlend) ||
                src1Blend(t.SrcBlendAlpha) || src1Blend(t.DestBlendAlpha)) return false;
        return true;
    }
    static D3D11_BLEND_DESC defaultBlend() {
        D3D11_BLEND_DESC d{};
        for (auto& t : d.RenderTarget) {
            t.BlendEnable = FALSE;
            t.SrcBlend = t.SrcBlendAlpha = D3D11_BLEND_ONE;
            t.DestBlend = t.DestBlendAlpha = D3D11_BLEND_ZERO;
            t.BlendOp = t.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            t.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        }
        return d;
    }
    static bool sameViewSize(ID3D11RenderTargetView* view, UINT w, UINT h) {
        if (!view) return true;
        D3D11_RENDER_TARGET_VIEW_DESC vd{}; view->GetDesc(&vd);
        if (vd.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || vd.Texture2D.MipSlice != 0) return false;
        Ptr<ID3D11Resource> resource; view->GetResource(&resource);
        Ptr<ID3D11Texture2D> texture;
        if (!resource || FAILED(resource.As(&texture))) return false;
        D3D11_TEXTURE2D_DESC d{}; texture->GetDesc(&d);
        return d.Width == w && d.Height == h && d.SampleDesc.Count == 1 && d.ArraySize == 1;
    }
    static bool sameDepthSize(ID3D11DepthStencilView* view, UINT w, UINT h) {
        if (!view) return false;
        D3D11_DEPTH_STENCIL_VIEW_DESC vd{}; view->GetDesc(&vd);
        if (vd.ViewDimension != D3D11_DSV_DIMENSION_TEXTURE2D || vd.Texture2D.MipSlice != 0) return false;
        Ptr<ID3D11Resource> resource; view->GetResource(&resource);
        Ptr<ID3D11Texture2D> texture;
        if (!resource || FAILED(resource.As(&texture))) return false;
        D3D11_TEXTURE2D_DESC d{}; texture->GetDesc(&d);
        return d.Width == w && d.Height == h && d.SampleDesc.Count == 1 && d.ArraySize == 1;
    }
    static bool omUavBound(ID3D11DeviceContext* ctx) {
        ID3D11UnorderedAccessView* uavs[D3D11_PS_CS_UAV_REGISTER_COUNT]{};
        ctx->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,D3D11_PS_CS_UAV_REGISTER_COUNT,uavs);
        bool any = false;
        for (auto* u : uavs) if (u) { any = true; u->Release(); }
        return any;
    }
    bool refuse(const char* why, const char** out) {
        if (refusal_.empty()) refusal_ = why ? why : "overlay-refused";
        if (out) *out = refusal_.c_str();
        return false;
    }
    static Ptr<ID3D11PixelShader> patchedShader(ID3D11Device* dev, ID3D11PixelShader* original,
                                                  std::string& why) {
        Ptr<ID3D11PixelShader> result;
        std::vector<BYTE> bytes;
        {
            auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex);
            auto it = r.shaders.find(original);
            if (it == r.shaders.end()) {
                char noted[96]{}; UINT n=sizeof(noted);
                if (SUCCEEDED(original->GetPrivateData(reasonKey(),&n,noted)) && n && n<=sizeof(noted))
                    why = noted;
                else why = "PS bytecode not retained";
                return result;
            }
            if (it->second.patched) return it->second.patched;
            bytes = it->second.bytes;
        }
        creatingPatched() = true;
        const HRESULT hr = dev->CreatePixelShader(bytes.data(),bytes.size(),nullptr,&result);
        creatingPatched() = false;
        if (FAILED(hr) || !result) { why = "patched PS creation failed"; return {}; }
        {
            auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex);
            auto it = r.shaders.find(original);
            if (it == r.shaders.end()) { why = "PS cache changed"; return {}; }
            if (!it->second.patched) it->second.patched = result;
            return it->second.patched;
        }
    }
    Ptr<ID3D11BlendState> derivedBlend(ID3D11Device* dev, ID3D11BlendState* original,
                                        const char** why) {
        auto found = blends_.find(original);
        if (found != blends_.end()) return found->second.derived;
        Ptr<ID3D11BlendState> out;
        if (blends_.size() >= 128) { if (why) *why="blend-cache-cap"; return out; }
        D3D11_BLEND_DESC d = defaultBlend();
        if (original) {
            Ptr<ID3D11BlendState1> state1;
            if (SUCCEEDED(original->QueryInterface(IID_PPV_ARGS(&state1))) && state1) {
                D3D11_BLEND_DESC1 d1{}; state1->GetDesc1(&d1);
                for (const auto& t : d1.RenderTarget) if (t.LogicOpEnable) {
                    if (why) *why="blend-logic-op"; return out;
                }
            }
            original->GetDesc(&d);
        }
        if (!validateBlend(d)) { if (why) *why="dual-source-blend"; return out; }
        if (!d.IndependentBlendEnable)
            for (UINT i=1;i<D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;++i) d.RenderTarget[i]=d.RenderTarget[0];
        d.IndependentBlendEnable=TRUE; // preserve each game target's effective blend
        auto& t=d.RenderTarget[kFlatOverlayTarget];
        t.BlendEnable=FALSE;
        t.SrcBlend=t.SrcBlendAlpha=D3D11_BLEND_ONE;
        t.DestBlend=t.DestBlendAlpha=D3D11_BLEND_ZERO;
        t.BlendOp=t.BlendOpAlpha=D3D11_BLEND_OP_ADD;
        t.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_RED;
        if (FAILED(dev->CreateBlendState(&d,&out))) { if (why) *why="derived-blend-create"; return {}; }
        Blend entry; entry.original=original; entry.derived=out;
        blends_.emplace(original,std::move(entry));
        return out;
    }
    bool ensureResources(ID3D11Device* dev, ID3D11Texture2D* hdr, const D3D11_TEXTURE2D_DESC& source,
                         DXGI_FORMAT viewFormat, bool coverageOnly, const char** reason) {
        if (cleanSource_ && cleanSource_.Get()!=hdr) return refuse("HDR-resource-changed",reason);
        if (coverage_ && resourceCoverageOnly_!=coverageOnly && completedDraws_)
            return refuse("coverage-mode-changed-mid-frame",reason);
        if (coverage_ && ((!coverageOnly && (!clean_ || !cleanView_)) || !coverageRtv_ || !coverageView_ ||
                          resourceCoverageOnly_!=coverageOnly)) releaseResources();
        if (coverage_ && (resourceWidth_!=source.Width || resourceHeight_!=source.Height || resourceFormat_!=source.Format)) {
            if (completedDraws_) return refuse("HDR-shape-changed-mid-frame",reason);
            releaseResources();
        }
        if (coverage_) { cleanSource_=hdr; return true; }
        D3D11_TEXTURE2D_DESC cd=source;
        cd.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        cd.CPUAccessFlags=0; cd.MiscFlags=0; cd.Usage=D3D11_USAGE_DEFAULT;
        if (!coverageOnly) {
            if (FAILED(dev->CreateTexture2D(&cd,nullptr,&clean_))) { releaseResources(); return refuse("clean-HDR-create",reason); }
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format=viewFormat; sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels=1;
            if (FAILED(dev->CreateShaderResourceView(clean_.Get(),&sd,&cleanView_))) { releaseResources(); return refuse("clean-HDR-SRV-create",reason); }
        }
        D3D11_TEXTURE2D_DESC md=cd;
        md.Format=DXGI_FORMAT_R8_UNORM; md.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&md,nullptr,&coverage_))) { releaseResources(); return refuse("coverage-create",reason); }
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format=DXGI_FORMAT_R8_UNORM; rd.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(dev->CreateRenderTargetView(coverage_.Get(),&rd,&coverageRtv_)) ||
            FAILED(dev->CreateShaderResourceView(coverage_.Get(),nullptr,&coverageView_))) {
            releaseResources(); return refuse("coverage-view-create",reason);
        }
        resourceWidth_=source.Width; resourceHeight_=source.Height; resourceFormat_=source.Format;
        resourceCoverageOnly_=coverageOnly;
        cleanSource_=hdr;
        return true;
    }
    void releaseResources() {
        cleanSource_.Reset();clean_.Reset();cleanView_.Reset();coverage_.Reset();coverageView_.Reset();coverageRtv_.Reset();
        resourceWidth_=resourceHeight_=0;resourceFormat_=DXGI_FORMAT_UNKNOWN;resourceCoverageOnly_=false;
    }
    void restore() {
        if (!active_) return;
        FlatComputeInternalScope internal;
        auto* ctx=saved_.context.Get();
        if (ctx) {
            ctx->PSSetShader(saved_.ps.Get(),nullptr,0);
            ID3D11RenderTargetView* rt[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
            for (UINT i=0;i<D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;++i) rt[i]=saved_.rtv[i].Get();
            ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,rt,saved_.dsv.Get());
            ctx->OMSetBlendState(saved_.blend.Get(),saved_.factors,saved_.sampleMask);
        }
        saved_={}; active_=false;
    }
    uint64_t frame_=0;
    uint32_t completedDraws_=0;
    std::string refusal_;
    bool active_=false;
    Saved saved_{};
    Ptr<ID3D11Device> device_;
    Ptr<ID3D11Texture2D> cleanSource_,clean_,coverage_;
    Ptr<ID3D11ShaderResourceView> cleanView_,coverageView_;
    Ptr<ID3D11RenderTargetView> coverageRtv_;
    UINT resourceWidth_=0,resourceHeight_=0;
    DXGI_FORMAT resourceFormat_=DXGI_FORMAT_UNKNOWN;
    bool resourceCoverageOnly_=false;
    std::unordered_map<ID3D11BlendState*,Blend> blends_;
public:
    FlatOverlayLayer()=default;
    ~FlatOverlayLayer(){ restore(); }
    FlatOverlayLayer(const FlatOverlayLayer&)=delete;
    FlatOverlayLayer& operator=(const FlatOverlayLayer&)=delete;
    static void rememberPixelShader(ID3D11PixelShader* shader,const void* bytecode,size_t bytes,bool linked) {
        if (creatingPatched() || !shader || !bytecode || !bytes || bytes>1024u*1024u) return;
        if (linked) { markShaderReason(shader,"PS class linkage"); return; }
        std::vector<BYTE> patched; std::string why;
        if (!flatOverlayPatchPs(bytecode,bytes,patched,why)) { markShaderReason(shader,why.c_str()); return; }
        auto& r=registry(); std::lock_guard<std::mutex> lock(r.mutex);
        if (r.shaders.count(shader)) return;
        if (r.shaders.size()>=2048 || patched.size()>32u*1024u*1024u-r.bytes) {
            markShaderReason(shader,"PS cache cap"); return;
        }
        Shader entry; entry.original=shader; entry.bytes=std::move(patched);
        r.bytes+=entry.bytes.size(); r.shaders.emplace(shader,std::move(entry));
    }
    void beginFrame(uint64_t frame) {
        if (frame_==frame) return;
        if (active_) restore();
        frame_=frame; completedDraws_=0; refusal_.clear();
        cleanSource_.Reset(); // reuse private textures/views when the next H has the same shape
    }
    void reset() {
        restore(); frame_=0; completedDraws_=0; refusal_.clear();
        releaseResources();
        blends_.clear(); device_.Reset();
    }
    bool beginDraw(ID3D11DeviceContext* ctx,uint64_t frame,ID3D11Texture2D* hdr,
                    ID3D11DepthStencilView* expectedDsv,const char** reason=nullptr,
                    bool diagnosticTypelessPool=false,bool diagnosticCoverageOnly=false) {
        if (reason) *reason=nullptr;
        if (!ctx || !hdr || !expectedDsv || !frame || frame_!=frame) return refuse("draw-frame-or-source",reason);
        if (!refusal_.empty()) return refuse(refusal_.c_str(),reason);
        if (active_) return refuse("nested-overlay-draw",reason);
        if (ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) return refuse("non-immediate-context",reason);
        FlatComputeInternalScope internal;
        Ptr<ID3D11Device> dev; ctx->GetDevice(&dev);
        if (!dev) return refuse("device-unavailable",reason);
        if (device_ && device_.Get()!=dev.Get()) {
            if (cleanSource_) return refuse("device-changed-mid-frame",reason);
            releaseResources();
            blends_.clear(); device_.Reset();
        }
        device_=dev;
        D3D11_TEXTURE2D_DESC hd{};hdr->GetDesc(&hd);
        if (!hd.Width || !hd.Height || hd.MipLevels!=1 || hd.ArraySize!=1 || hd.SampleDesc.Count!=1 ||
            !(hd.BindFlags&D3D11_BIND_RENDER_TARGET) || hd.Usage!=D3D11_USAGE_DEFAULT)
            return refuse("unsupported-HDR-shape",reason);
        // The foreground *diagnostic* observes the game's typeless format-23
        // pool target through its typed format-24 RTV/SRV. This option never
        // widens the production late-HDR overlay admission policy.
        const bool poolProbe=diagnosticTypelessPool && hd.Format==DXGI_FORMAT_R10G10B10A2_TYPELESS;
        if (diagnosticCoverageOnly && !poolProbe) return refuse("coverage-only-source-not-pool",reason);
        const DXGI_FORMAT viewFormat=poolProbe?DXGI_FORMAT_R10G10B10A2_UNORM:hd.Format;
        const uint64_t colorBytes=hd.Format==DXGI_FORMAT_R16G16B16A16_FLOAT?8u:
            (hd.Format==DXGI_FORMAT_R11G11B10_FLOAT || hd.Format==DXGI_FORMAT_R8G8B8A8_UNORM || poolProbe?4u:0u);
        if (!colorBytes) return refuse("unsupported-HDR-format",reason);
        if (uint64_t(hd.Width)*hd.Height*(diagnosticCoverageOnly?1u:colorBytes+1u)>128ull*1024*1024)
            return refuse("overlay-resource-budget",reason);
        Saved game{};game.context=ctx;
        ID3D11RenderTargetView* raw[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        ID3D11DepthStencilView* rawDepth=nullptr;
        ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,raw,&rawDepth);
        for (UINT i=0;i<D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;++i) game.rtv[i].Attach(raw[i]);
        game.dsv.Attach(rawDepth);
        if (!game.rtv[0] || game.rtv[kFlatOverlayTarget] || game.dsv.Get()!=expectedDsv)
            return refuse("HDR-RTV-DSV-or-MRT7-binding",reason);
        Ptr<ID3D11Resource> boundColor;game.rtv[0]->GetResource(&boundColor);
        if (boundColor.Get()!=hdr) return refuse("HDR-RTV-resource-mismatch",reason);
        D3D11_RENDER_TARGET_VIEW_DESC hdrView{}; game.rtv[0]->GetDesc(&hdrView);
        if (hdrView.Format!=viewFormat) return refuse("HDR-RTV-format-mismatch",reason);
        for (auto& view:game.rtv) if (!sameViewSize(view.Get(),hd.Width,hd.Height))
            return refuse("MRT-size-or-view-mismatch",reason);
        if (!sameDepthSize(game.dsv.Get(),hd.Width,hd.Height)) return refuse("DSV-size-or-view-mismatch",reason);
        if (omUavBound(ctx)) return refuse("OM-UAV-bound",reason);
        Ptr<ID3D11Predicate> predicate;BOOL predicateValue=FALSE;
        ctx->GetPredication(&predicate,&predicateValue);
        if (predicate) return refuse("predication-bound",reason);
        ID3D11ClassInstance* classes[256]{};UINT classCount=256;
        ctx->PSGetShader(&game.ps,classes,&classCount);
        for (UINT i=0;i<classCount;++i) if (classes[i]) classes[i]->Release();
        if (!game.ps || classCount) return refuse("PS-linkage-or-null",reason);
        ctx->OMGetBlendState(&game.blend,game.factors,&game.sampleMask);
        const char* blendWhy=nullptr;
        Ptr<ID3D11BlendState> blend=derivedBlend(dev.Get(),game.blend.Get(),&blendWhy);
        if (!blend) return refuse(blendWhy?blendWhy:"blend-unavailable",reason);
        std::string shaderWhy;
        Ptr<ID3D11PixelShader> patched=patchedShader(dev.Get(),game.ps.Get(),shaderWhy);
        if (!patched) return refuse(shaderWhy.c_str(),reason);
        if (!ensureResources(dev.Get(),hdr,hd,viewFormat,diagnosticCoverageOnly,reason)) return false;
        if (!completedDraws_ || diagnosticCoverageOnly) {
            const FLOAT zero[4]{};
            ctx->ClearRenderTargetView(coverageRtv_.Get(),zero);
            if (!diagnosticCoverageOnly) ctx->CopyResource(clean_.Get(),hdr);
        }
        ID3D11RenderTargetView* attach[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        for (UINT i=0;i<D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;++i) attach[i]=game.rtv[i].Get();
        attach[kFlatOverlayTarget]=coverageRtv_.Get();
        ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,attach,game.dsv.Get());
        ctx->OMSetBlendState(blend.Get(),game.factors,game.sampleMask);
        ctx->PSSetShader(patched.Get(),nullptr,0);
        // Some translation layers silently drop incompatible targets. Verify
        // the private target before allowing the original game Draw to run.
        ID3D11RenderTargetView* check[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        ID3D11DepthStencilView* checkDepth=nullptr;
        ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,check,&checkDepth);
        bool kept=checkDepth==game.dsv.Get();
        for (UINT i=0;i<D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;++i) {
            if (check[i]!=attach[i]) kept=false;
            if (check[i]) check[i]->Release();
        }
        if (checkDepth) checkDepth->Release();
        Ptr<ID3D11PixelShader> checkPs;
        ctx->PSGetShader(&checkPs,nullptr,nullptr);
        if (checkPs.Get()!=patched.Get()) kept=false;
        Ptr<ID3D11BlendState> checkBlend; FLOAT checkFactors[4]{}; UINT checkMask=0;
        ctx->OMGetBlendState(&checkBlend,checkFactors,&checkMask);
        if (checkBlend.Get()!=blend.Get() || checkMask!=game.sampleMask ||
            std::memcmp(checkFactors,game.factors,sizeof(checkFactors))!=0) kept=false;
        if (!kept) {
            ctx->PSSetShader(game.ps.Get(),nullptr,0);
            ID3D11RenderTargetView* restoreRt[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
            for (UINT i=0;i<D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;++i) restoreRt[i]=game.rtv[i].Get();
            ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,restoreRt,game.dsv.Get());
            ctx->OMSetBlendState(game.blend.Get(),game.factors,game.sampleMask);
            return refuse("private-MRT7-PS-or-blend-dropped",reason);
        }
        saved_=std::move(game);active_=true;
        return true;
    }
    void endDraw(ID3D11DeviceContext* ctx) {
        if (!active_) { invalidate("draw-end-without-begin"); return; }
        const bool same=ctx==saved_.context.Get();
        restore();
        if (same) ++completedDraws_; else invalidate("draw-context-changed");
    }
    void invalidate(const char* why) { if (refusal_.empty()) refusal_=why?why:"overlay-invalidated"; }
    bool ready(uint64_t frame,ID3D11Texture2D* hdr) const {
        return frame_==frame && !active_ && refusal_.empty() && completedDraws_ && cleanSource_.Get()==hdr &&
                !resourceCoverageOnly_ && clean_ && cleanView_ && coverageView_;
    }
    bool coverageReady(uint64_t frame,ID3D11Texture2D* source) const {
        return frame_==frame && !active_ && refusal_.empty() && completedDraws_ &&
            resourceCoverageOnly_ && cleanSource_.Get()==source && coverageView_;
    }
    ID3D11Texture2D* cleanHdr() const { return clean_.Get(); }
    ID3D11ShaderResourceView* cleanHdrView() const { return cleanView_.Get(); }
    ID3D11ShaderResourceView* coverageView() const { return coverageView_.Get(); }
    const char* refusal() const { return refusal_.empty()?nullptr:refusal_.c_str(); }
    uint32_t markedDraws() const { return completedDraws_; }
};
} // namespace edvr
