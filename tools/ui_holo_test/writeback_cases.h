#pragma once
inline ComPtr<ID3D11DepthStencilView> gameDepth(ID3D11Device* d,ID3D11DeviceContext* c,unsigned w,unsigned h){
    D3D11_TEXTURE2D_DESC t{};t.Width=w;t.Height=h;t.MipLevels=t.ArraySize=1;t.Format=DXGI_FORMAT_R24G8_TYPELESS;
    t.SampleDesc.Count=1;t.BindFlags=D3D11_BIND_DEPTH_STENCIL;ComPtr<ID3D11Texture2D> texture;
    ck(d->CreateTexture2D(&t,nullptr,&texture));D3D11_DEPTH_STENCIL_VIEW_DESC vd{};vd.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;vd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11DepthStencilView> view;ck(d->CreateDepthStencilView(texture.Get(),&vd,&view));c->ClearDepthStencilView(view.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,.6f,0xa1);return view;
}
inline std::vector<uint32_t> depthBytes(ID3D11Device* d,ID3D11DeviceContext* c,ID3D11DepthStencilView* view){
    ComPtr<ID3D11Resource> r;view->GetResource(&r);ComPtr<ID3D11Texture2D> t;ck(r.As(&t));D3D11_TEXTURE2D_DESC td{};t->GetDesc(&td);
    td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> stage;ck(d->CreateTexture2D(&td,nullptr,&stage));
    c->OMSetRenderTargets(0,nullptr,nullptr);c->CopyResource(stage.Get(),t.Get());D3D11_MAPPED_SUBRESOURCE m{};ck(c->Map(stage.Get(),0,D3D11_MAP_READ,0,&m));
    std::vector<uint32_t> bytes(td.Width*td.Height);for(unsigned y=0;y<td.Height;++y)std::memcpy(bytes.data()+y*td.Width,static_cast<BYTE*>(m.pData)+y*m.RowPitch,td.Width*4);
    c->Unmap(stage.Get(),0);return bytes;
}
inline void writebackCases(ID3D11Device* d,ID3D11DeviceContext* c,ID3D11PixelShader* stock,ID3D11PixelShader* repaired,ID3D11Buffer* constants){
    // Observed state: depth disabled, ALWAYS stencil04 write, premultiplied
    // blend. Replay must preserve all other original stencil/depth bits.
    const Scene source{64,64,64,64,1,1,0,0,0,0,{0,0,64,64,0,1},{7,11,58,60}};
    auto scene=texture(d,64,64,depth(source,false),true);auto srv=scene.Get();c->PSSetShaderResources(1,1,&srv);
    check(edvr::ui_holo_remap::depthSource(srv,64,64),"actual TYPELESS/FLOAT t1 contract accepted");
    check(!edvr::ui_holo_remap::depthSource(srv,63,64)&&!edvr::ui_holo_remap::depthSource(nullptr,64,64),"wrong-size/missing depth refuses");
    auto rgba=texture(d,64,64,std::vector<float>(64*64*4,1));check(!edvr::ui_holo_remap::depthSource(rgba.Get(),64,64),"RGBA material cannot be mistaken for scene depth");
    D3D11_TEXTURE2D_DESC rt{};rt.Width=rt.Height=64;rt.MipLevels=rt.ArraySize=1;rt.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;rt.SampleDesc.Count=1;rt.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> colour;ck(d->CreateTexture2D(&rt,nullptr,&colour));ComPtr<ID3D11RenderTargetView> rtv;ck(d->CreateRenderTargetView(colour.Get(),nullptr,&rtv));auto target=rtv.Get();
    auto baseline=gameDepth(d,c,64,64);c->OMSetRenderTargets(1,&target,baseline.Get());c->RSSetViewports(1,&source.vp);c->RSSetScissorRects(1,&source.sc);c->PSSetShader(stock,nullptr,0);c->Draw(3,0);
    const auto expected=depthBytes(d,c,baseline.Get());unsigned written=0;for(auto q:expected)written+=((q>>24)==0xa5);
    check(written>0&&written<64*64,"stock stencil04 footprint is nontrivial and scissored");
    for(bool alreadySeeded:{false,true}){
        auto original=gameDepth(d,c,64,64),layer=gameDepth(d,c,160,160);rt.Width=rt.Height=160;
        ck(d->CreateTexture2D(&rt,nullptr,&colour));ck(d->CreateRenderTargetView(colour.Get(),nullptr,&rtv));target=rtv.Get();
        const D3D11_VIEWPORT vp{0,0,160,160,0,1};const D3D11_RECT sc{17,27,145,150};
        c->OMSetRenderTargets(1,&target,alreadySeeded?layer.Get():nullptr);c->RSSetViewports(1,&vp);c->RSSetScissorRects(1,&sc);
        edvr::ui_holo_remap::Binding binding;check(binding.begin(c,repaired,constants,{.4f,.4f,0,0}),"actual PS colour moves with original t1");c->Draw(3,0);
        check(binding.restore(c),"stock PS restored BEFORE colourless writeback");binding.clear();
        c->OMSetRenderTargets(0,nullptr,original.Get());c->RSSetViewports(1,&source.vp);c->RSSetScissorRects(1,&source.sc);c->Draw(3,0);
        check(depthBytes(d,c,original.Get())==expected,"actual stock colourless reissue equals original stencil/depth bytes");
    }
}
