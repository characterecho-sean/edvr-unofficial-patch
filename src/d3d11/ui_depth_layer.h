#pragma once

#include <d3d11.h>

namespace edvr {

// UI coverage must never alter the depth used by later game draws. Seed a
// private target from the scene once per eye/frame, keeping its occlusion
// test, and let only the temporal pass read the result. The final pass also
// reads the current scene depth, so later nearer geometry still wins.
class UiDepthLayer {
public:
    UiDepthLayer() = default;
    UiDepthLayer(const UiDepthLayer&) = delete;
    UiDepthLayer& operator=(const UiDepthLayer&) = delete;
    ~UiDepthLayer() { release(); }

    void frameBoundary() { prepared_ = false; }

    void release() {
        if (sourceSrv_) sourceSrv_->Release();
        if (srv_) srv_->Release();
        if (dsv_) dsv_->Release();
        if (tex_) tex_->Release();
        if (source_) source_->Release();
        srv_ = nullptr;
        sourceSrv_ = nullptr;
        sourceReadTried_ = false;
        dsv_ = nullptr;
        tex_ = source_ = nullptr;
        prepared_ = false;
        w_ = h_ = 0;
    }

    ID3D11DepthStencilView* acquire(ID3D11DeviceContext* ctx, ID3D11Texture2D* scene) {
        if (!ctx || !scene) return nullptr;
        D3D11_TEXTURE2D_DESC td{};
        scene->GetDesc(&td);
        if (td.SampleDesc.Count != 1 || td.ArraySize != 1 || td.MipLevels != 1 ||
            !(td.BindFlags & D3D11_BIND_DEPTH_STENCIL)) return nullptr;
        if (scene != source_ || !dsv_) {
            release();
            DXGI_FORMAT depth = DXGI_FORMAT_UNKNOWN, read = DXGI_FORMAT_UNKNOWN;
            switch (td.Format) {
                case DXGI_FORMAT_R32G8X24_TYPELESS:
                case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
                    td.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
                    depth = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
                    read = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
                    break;
                case DXGI_FORMAT_R24G8_TYPELESS:
                case DXGI_FORMAT_D24_UNORM_S8_UINT:
                    td.Format = DXGI_FORMAT_R24G8_TYPELESS;
                    depth = DXGI_FORMAT_D24_UNORM_S8_UINT;
                    read = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
                    break;
                case DXGI_FORMAT_R32_TYPELESS:
                case DXGI_FORMAT_D32_FLOAT:
                    td.Format = DXGI_FORMAT_R32_TYPELESS;
                    depth = DXGI_FORMAT_D32_FLOAT;
                    read = DXGI_FORMAT_R32_FLOAT;
                    break;
                default: return nullptr;
            }
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
            td.CPUAccessFlags = td.MiscFlags = 0;
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            if (!dev) return nullptr;
            HRESULT hr = dev->CreateTexture2D(&td, nullptr, &tex_);
            D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
            dd.Format = depth;
            dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            if (SUCCEEDED(hr)) hr = dev->CreateDepthStencilView(tex_, &dd, &dsv_);
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = read;
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
            sourceReadFormat_ = read;
            if (SUCCEEDED(hr)) hr = dev->CreateShaderResourceView(tex_, &sd, &srv_);
            dev->Release();
            if (FAILED(hr)) { release(); return nullptr; }
            source_ = scene;
            source_->AddRef();
            w_ = td.Width;
            h_ = td.Height;
        }
        if (!prepared_) {
            ctx->CopyResource(tex_, scene);
            prepared_ = true;
        }
        return dsv_;
    }

    // Borrowed view. Identity as well as size must match: a depth-probe
    // switch, another eye, or an old frame must not contribute UI depth.
    ID3D11ShaderResourceView* view(ID3D11Texture2D* scene, UINT w, UINT h) const {
        return prepared_ && source_ == scene && w_ == w && h_ == h ? srv_ : nullptr;
    }

    // Floating HUD strokes preserve the scene's device depth verbatim.
    // The original DSV must be unbound before this view is bound; the
    // coverage pass writes tex_, so it never reads its own output.
    ID3D11ShaderResourceView* sourceView(ID3D11DeviceContext* ctx) {
        if (!prepared_ || !source_ || !ctx) return nullptr;
        if (!sourceReadTried_) {
            sourceReadTried_ = true;
            D3D11_TEXTURE2D_DESC td{}; source_->GetDesc(&td);
            if (td.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
                ID3D11Device* dev = nullptr; ctx->GetDevice(&dev);
                D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.Format = sourceReadFormat_;
                sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                sd.Texture2D.MipLevels = 1;
                if (dev) { dev->CreateShaderResourceView(source_, &sd, &sourceSrv_); dev->Release(); }
            }
        }
        return sourceSrv_;
    }

private:
    ID3D11Texture2D* source_ = nullptr;
    ID3D11Texture2D* tex_ = nullptr;
    ID3D11DepthStencilView* dsv_ = nullptr;
    ID3D11ShaderResourceView* srv_ = nullptr;
    ID3D11ShaderResourceView* sourceSrv_ = nullptr;
    DXGI_FORMAT sourceReadFormat_ = DXGI_FORMAT_UNKNOWN;
    bool sourceReadTried_ = false;
    UINT w_ = 0, h_ = 0;
    bool prepared_ = false;
};

}  // namespace edvr
