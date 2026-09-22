void testCoronaCoverage(ID3D11Device* dev,ID3D11DeviceContext* ctx) {
    ctx->ClearState();
    auto buffer=[&](UINT bytes,UINT bind,const void* init) {
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=bytes;bd.BindFlags=bind;
        D3D11_SUBRESOURCE_DATA data{init,0,0};ComPtr<ID3D11Buffer> out;
        hr(dev->CreateBuffer(&bd,init?&data:nullptr,&out));return out;
    };
    auto texture=[&](UINT bind) {
        D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=8;
        td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
        td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.BindFlags=bind;
        ComPtr<ID3D11Texture2D> out;hr(dev->CreateTexture2D(&td,nullptr,&out));return out;
    };
    auto makePs=[&](const char* source) {
        auto code=compile(source,"ps_5_0");ComPtr<ID3D11PixelShader> out;
        hr(dev->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&out));return out;
    };
    auto scene=depth(dev,DXGI_FORMAT_R32_TYPELESS,DXGI_FORMAT_D32_FLOAT);
    auto colour=texture(D3D11_BIND_RENDER_TARGET),linear=texture(D3D11_BIND_SHADER_RESOURCE),streak=texture(D3D11_BIND_SHADER_RESOURCE);
    ComPtr<ID3D11RenderTargetView> colourRt;hr(dev->CreateRenderTargetView(colour.Get(),nullptr,&colourRt));
    ComPtr<ID3D11ShaderResourceView> linearSrv,streakSrv;
    hr(dev->CreateShaderResourceView(linear.Get(),nullptr,&linearSrv));hr(dev->CreateShaderResourceView(streak.Get(),nullptr,&streakSrv));
    std::vector<float> linearPixels(8*8*4,1e9f);ctx->UpdateSubresource(linear.Get(),0,nullptr,linearPixels.data(),8*16,0);
    auto fullCode=compile("float4 main(uint id:SV_VertexID):SV_Position{float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);}","vs_5_0");
    ComPtr<ID3D11VertexShader> fullVs;hr(dev->CreateVertexShader(fullCode->GetBufferPointer(),fullCode->GetBufferSize(),nullptr,&fullVs));
    auto scenePs=makePs("float4 main(float4 p:SV_Position,out float z:SV_Depth):SV_Target{z=p.x<4?.0005:0;return float4(.2,.4,.6,.8);}");
    auto seedPs=makePs("float2 main():SV_Target{return float2(1,.0005);}");
    auto gamePs=makePs("float4 main():SV_Target{return float4(.9,.1,.2,.7);}");
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster;hr(dev->CreateRasterizerState(&rd,&raster));
    D3D11_VIEWPORT vp{0,0,8,8,0,1};
    D3D11_DEPTH_STENCIL_DESC dd{};dd.DepthEnable=TRUE;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;dd.DepthFunc=D3D11_COMPARISON_GREATER_EQUAL;
    ComPtr<ID3D11DepthStencilState> writeDepth,gameDepth;hr(dev->CreateDepthStencilState(&dd,&writeDepth));
    dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;hr(dev->CreateDepthStencilState(&dd,&gameDepth));
    ctx->RSSetState(raster.Get());ctx->RSSetViewports(1,&vp);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->OMSetRenderTargets(1,colourRt.GetAddressOf(),scene.dsv.Get());ctx->OMSetDepthStencilState(writeDepth.Get(),0);
    ctx->ClearDepthStencilView(scene.dsv.Get(),D3D11_CLEAR_DEPTH,0,0);
    ctx->VSSetShader(fullVs.Get(),nullptr,0);ctx->PSSetShader(scenePs.Get(),nullptr,0);ctx->Draw(3,0);
    auto originalZ=read(dev,ctx,scene.tex.Get());
    for(UINT y=0;y<8;++y)for(UINT x=0;x<8;++x)
        check(originalZ[y*8+x]==(x<4?.0005f:0.f),"corona fixture has nearer sun and empty sky zones");
    auto coronaCode=compile(R"HLSL(
cbuffer Model:register(b0){float4 m[8];}
struct V{float3 p:POSITION;float2 uv:TEXCOORD0;};
struct O{float3 a:TEXCOORD0;float3 b:TEXCOORD1;float3 c:TEXCOORD2;float2 uv:TEXCOORD3;float4 p:SV_Position;};
O main(V v){O o=(O)0;float4 p=float4(v.p,1);
o.p=float4(dot(m[4],p),dot(m[5],p),dot(m[6],p)+15.01,dot(m[7],p));
o.b=o.p.xyw;o.c=float3(0,0,100);o.uv=v.uv;return o;}
)HLSL","vs_5_0");
    ComPtr<ID3D11VertexShader> coronaVs;hr(dev->CreateVertexShader(coronaCode->GetBufferPointer(),coronaCode->GetBufferSize(),nullptr,&coronaVs));
    D3D11_INPUT_ELEMENT_DESC elements[2]={{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,36,D3D11_INPUT_PER_VERTEX_DATA,0}};
    ComPtr<ID3D11InputLayout> layout;hr(dev->CreateInputLayout(elements,2,coronaCode->GetBufferPointer(),coronaCode->GetBufferSize(),&layout));
    EyeDrawSnapshot::rememberLayout(layout.Get(),elements,2,kSmokeVs);
    struct Vertex{float p[3],padding[6],uv[2];};
    const Vertex vertices[]={{{-100,100,0},{},{.5f,0}},{{300,100,0},{},{.5f,0}},{{-100,-300,0},{},{.5f,0}}};
    const uint16_t indices[]={0,1,2};
    auto vb=buffer(sizeof(vertices),D3D11_BIND_VERTEX_BUFFER,vertices),ib=buffer(sizeof(indices),D3D11_BIND_INDEX_BUFFER,indices);
    float model[32]{},sceneData[1104]{},material[16]{},zero[4]{};
    model[16]=model[21]=model[30]=1;model[27]=.025f;model[31]=100;
    sceneData[126*4]=1;sceneData[126*4+2]=1;
    material[3]=1;material[4]=.1f;material[5]=.35f;material[6]=1;
    auto cb0=buffer(sizeof(model),D3D11_BIND_CONSTANT_BUFFER,model),cb1=buffer(sizeof(sceneData),D3D11_BIND_CONSTANT_BUFFER,sceneData),cb2=buffer(sizeof(material),D3D11_BIND_CONSTANT_BUFFER,material);
    auto sentinel12=buffer(16,D3D11_BIND_CONSTANT_BUFFER,zero),sentinel13=buffer(16,D3D11_BIND_CONSTANT_BUFFER,zero);
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;hr(dev->CreateSamplerState(&sd,&sampler));
    D3D11_BLEND_DESC bd{};auto& b=bd.RenderTarget[0];b.BlendEnable=TRUE;b.SrcBlend=D3D11_BLEND_SRC_ALPHA;b.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;b.BlendOp=D3D11_BLEND_OP_ADD;
    b.SrcBlendAlpha=D3D11_BLEND_ONE;b.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;b.BlendOpAlpha=D3D11_BLEND_OP_ADD;b.RenderTargetWriteMask=15;
    ComPtr<ID3D11BlendState> gameBlend;hr(dev->CreateBlendState(&bd,&gameBlend));
    const float blendFactors[]={.1f,.2f,.3f,.4f};const UINT sampleMask=0xa5a5a5a5;
    auto bind=[&](UINT stride) {
        ctx->ClearState();ctx->RSSetState(raster.Get());ctx->RSSetViewports(1,&vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->IASetInputLayout(layout.Get());
        UINT offset=0;ctx->IASetVertexBuffers(0,1,vb.GetAddressOf(),&stride,&offset);ctx->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R16_UINT,0);
        ID3D11Buffer* cb[]={cb0.Get(),cb1.Get(),cb2.Get()};ctx->VSSetConstantBuffers(0,3,cb);ctx->PSSetConstantBuffers(1,2,cb+1);
        ctx->VSSetShader(coronaVs.Get(),nullptr,0);ctx->PSSetShader(gamePs.Get(),nullptr,0);
        ctx->PSSetConstantBuffers(12,1,sentinel12.GetAddressOf());ctx->PSSetConstantBuffers(13,1,sentinel13.GetAddressOf());
        ID3D11ShaderResourceView* srv[]={linearSrv.Get(),streakSrv.Get(),streakSrv.Get()};ctx->PSSetShaderResources(0,3,srv);
        ctx->PSSetSamplers(0,1,sampler.GetAddressOf());ctx->PSSetSamplers(1,1,sampler.GetAddressOf());
        ctx->OMSetRenderTargets(1,colourRt.GetAddressOf(),scene.dsv.Get());ctx->OMSetDepthStencilState(gameDepth.Get(),23);ctx->OMSetBlendState(gameBlend.Get(),blendFactors,sampleMask);
    };
    struct Capture{std::vector<float> smoke,mask;};
    for(bool fringe:{false,true}) {
        Capture results[2];
        for(int enabled=0;enabled<2;++enabled) {
            ctx->ClearState();uiDepthFrameBoundary(ctx);g_holoMotion[0]=HoloMotion{};
            detail::g_uiDepthOn=true;detail::g_uiDepthStoodDown=false;g_smokeOn=true;g_reactive=0;g_smokeReactive=1;g_smokeFloor=.08f;
            std::vector<float> art(8*8*4,fringe?.005f:.5f);ctx->UpdateSubresource(streak.Get(),0,nullptr,art.data(),8*16,0);
            bind(12);check(g_holoMotion[0].prepare(ctx,scene.tex.Get(),{'X',3,1,0,0,0},1,4,0),"seed affine surface record before corona");
            auto* hc=g_holoMotion[0].target();
            ctx->ClearState();ctx->OMSetRenderTargets(1,&hc,nullptr);ctx->RSSetState(raster.Get());
            D3D11_VIEWPORT left{0,0,4,8,0,1};ctx->RSSetViewports(1,&left);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(fullVs.Get(),nullptr,0);ctx->PSSetShader(seedPs.Get(),nullptr,0);ctx->Draw(3,0);
            ctx->ClearState();check(g_uiDepth[0].acquire(ctx,scene.tex.Get())!=nullptr,"seed separate UI depth");
            auto* privateView=g_uiDepth[0].view(scene.tex.Get(),8,8);ComPtr<ID3D11Resource> privateResource;privateView->GetResource(&privateResource);
            auto privateBefore=read(dev,ctx,privateResource.Get());std::vector<float> colourBefore[4];
            for(UINT channel=0;channel<4;++channel)colourBefore[channel]=read(dev,ctx,colour.Get(),channel);
            bind(44);
            g_coronaPending=enabled!=0;g_coronaMotion=false;g_holoDraw={'X',3,1,0,0,0};detail::g_uiDepthMode=Mode::kReissueScene;
            g_reissueShader=&g_depthShaders[4];g_drawEye=0;g_reissueMaskSlot=3;g_reissueMaskOffset=0;
            g_rebindW=g_rebindH=8;g_wantRebind=false;g_wantMask=true;
            check(uiDepthReissueBegin(ctx),"production smoke/corona coverage begins");
            check(g_coronaMotion==(enabled!=0),"corona test actually selects the optional production motion shader");
            ctx->DrawIndexedInstanced(3,1,0,0,0);uiDepthReissueEnd(ctx);
            ComPtr<ID3D11PixelShader> restoredPs;ComPtr<ID3D11ShaderResourceView> restoredSrv;
            ComPtr<ID3D11Buffer> restored12,restored13;ctx->PSGetShader(&restoredPs,nullptr,nullptr);ctx->PSGetShaderResources(2,1,&restoredSrv);
            ctx->PSGetConstantBuffers(12,1,&restored12);ctx->PSGetConstantBuffers(13,1,&restored13);
            check(restoredPs==gamePs&&restoredSrv==streakSrv&&restored12==sentinel12&&restored13==sentinel13,"corona restores PS, original t2 and b12/b13");
            ComPtr<ID3D11BlendState> blend;ComPtr<ID3D11DepthStencilState> ds;ComPtr<ID3D11RenderTargetView> rt;ComPtr<ID3D11DepthStencilView> dsv;
            float factors[4]{};UINT mask=0,reference=0;ctx->OMGetBlendState(&blend,factors,&mask);ctx->OMGetDepthStencilState(&ds,&reference);ctx->OMGetRenderTargets(1,&rt,&dsv);
            check(blend==gameBlend&&ds==gameDepth&&rt==colourRt&&dsv==scene.dsv&&reference==23&&mask==sampleMask,"corona restores output, blend and depth state");
            for(UINT i=0;i<4;++i)check(factors[i]==blendFactors[i],"corona restores blend factors");
            for(UINT channel=0;channel<4;++channel)check(colourBefore[channel]==read(dev,ctx,colour.Get(),channel),"corona leaves every game colour channel unchanged");
            check(originalZ==read(dev,ctx,scene.tex.Get()),"corona leaves exact original scene depth unchanged");
            check(privateBefore==read(dev,ctx,privateResource.Get()),"corona leaves separate private UI depth unchanged");
            ID3D11ShaderResourceView* smokeView=nullptr;check(uiDepthSmokeDepth(8,8,0,&smokeView),"production smoke depth published");
            ComPtr<ID3D11Resource> smokeResource,hcResource;smokeView->GetResource(&smokeResource);hc->GetResource(&hcResource);
            results[enabled]={read(dev,ctx,smokeResource.Get()),read(dev,ctx,g_mask[0].tex)};
            auto ids=read(dev,ctx,hcResource.Get()),depths=read(dev,ctx,hcResource.Get(),1);
            for(UINT y=0;y<8;++y)for(UINT x=0;x<8;++x) {
                const UINT at=y*8+x;const bool replace=enabled&&!fringe&&x>=4;
                check(ids[at]==(replace?2.f:x<4?1.f:0.f),"visible corona replaces coverage; hidden cores and fringes preserve sun/sky");
                check(depths[at]==(replace?.025f/100:x<4?.0005f:0.f),"corona coverage preserves exact old depth or stores its actual view depth");
                check(results[enabled].smoke[at]==(fringe?0.f:.025f/100),"coverage test exercises every core and fringe raster sample");
                check(results[enabled].mask[at]==(fringe?7.f/255:1.f),"fringe has nonzero reactivity and reaches the preservation blend");
            }
        }
        check(results[0].smoke==results[1].smoke&&results[0].mask==results[1].mask,"motion extension preserves legacy private depth and classification bit for bit");
    }
    ctx->ClearState();uiDepthFrameBoundary(ctx);g_smokeReactive=0;g_coronaPending=g_coronaMotion=false;g_holoDraw={};
}
