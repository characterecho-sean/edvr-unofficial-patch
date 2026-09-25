void testPlanetCoverage(ID3D11Device* dev,ID3D11DeviceContext* ctx) {
    ctx->ClearState();
    auto target=depth(dev,DXGI_FORMAT_R32_TYPELESS,DXGI_FORMAT_D32_FLOAT);
    auto buffer=[&](UINT bytes,UINT bind,const void* data) {
        D3D11_BUFFER_DESC b{};b.ByteWidth=bytes;b.BindFlags=bind;D3D11_SUBRESOURCE_DATA init{data,0,0};
        ComPtr<ID3D11Buffer> out;hr(dev->CreateBuffer(&b,data?&init:nullptr,&out));return out;
    };
    float model[32]{},scene[1104]{};model[16]=model[21]=model[30]=1;model[27]=.025f;model[31]=5;
    auto cb0=buffer(sizeof(model),D3D11_BIND_CONSTANT_BUFFER,model),cb1=buffer(sizeof(scene),D3D11_BIND_CONSTANT_BUFFER,scene);
    auto originalInfo=buffer(16,D3D11_BIND_CONSTANT_BUFFER,model);
    float vertices[]={-5,-5,0,5,-5,0,0,5,0};uint16_t indices[]={0,1,2};
    auto vb=buffer(sizeof(vertices),D3D11_BIND_VERTEX_BUFFER,vertices),ib=buffer(sizeof(indices),D3D11_BIND_INDEX_BUFFER,indices);
    float solarVertices[]={-1,-1,0,1,-1,0,-1,1,0,1,1,0};uint16_t solarIndices[]={0,1,2,2,1,3};
    auto solarVb=buffer(sizeof(solarVertices),D3D11_BIND_VERTEX_BUFFER,solarVertices),solarIb=buffer(sizeof(solarIndices),D3D11_BIND_INDEX_BUFFER,solarIndices);
    auto vsCode=compile(R"HLSL(
cbuffer Model:register(b0){float4 m[8];}
struct Out {float4 p:SV_Position;float3 a:TEXCOORD0;float3 b:TEXCOORD1;float3 c:TEXCOORD2;float3 d:TEXCOORD3;
float3 e:TEXCOORD4;nointerpolation uint id:TEXCOORD5;float3 n:TEXCOORD6;float3 t:TEXCOORD7;};
Out main(float3 p:POSITION){Out o=(Out)0;float4 v=float4(p,1);o.a=o.b=o.c=o.d=o.e=o.n=o.t=p;
o.p=float4(dot(m[4],v),dot(m[5],v),dot(m[6],v),dot(m[7],v));return o;}
)HLSL","vs_5_0");
    auto psCode=compile("float4 main():SV_Target{return float4(.2,.4,.6,1);}","ps_5_0");
    auto solarPsCode=compile("struct I{float4 p:SV_Position;};float4 main(I i):SV_Target{if(i.p.x<4)discard;return float4(.2,.4,.6,.5);}","ps_5_0");
    ComPtr<ID3D11PixelShader> solarPs;hr(dev->CreatePixelShader(solarPsCode->GetBufferPointer(),solarPsCode->GetBufferSize(),nullptr,&solarPs));
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
    hr(dev->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs));
    hr(dev->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps));
    D3D11_INPUT_ELEMENT_DESC input{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0};
    ComPtr<ID3D11InputLayout> layout;hr(dev->CreateInputLayout(&input,1,vsCode->GetBufferPointer(),vsCode->GetBufferSize(),&layout));
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=8;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> colour,surface;ComPtr<ID3D11RenderTargetView> rtv;ComPtr<ID3D11ShaderResourceView> srv;
    hr(dev->CreateTexture2D(&td,nullptr,&colour));hr(dev->CreateRenderTargetView(colour.Get(),nullptr,&rtv));
    hr(dev->CreateTexture2D(&td,nullptr,&surface));hr(dev->CreateShaderResourceView(surface.Get(),nullptr,&srv));
    D3D11_DEPTH_STENCIL_DESC dd{};dd.DepthEnable=TRUE;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;dd.DepthFunc=D3D11_COMPARISON_GREATER;
    ComPtr<ID3D11DepthStencilState> ds;hr(dev->CreateDepthStencilState(&dd,&ds));
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> rs;hr(dev->CreateRasterizerState(&rd,&rs));
    D3D11_BLEND_DESC blendDesc{};
    auto& blendTarget=blendDesc.RenderTarget[0];
    blendTarget.BlendEnable=TRUE;blendTarget.SrcBlend=D3D11_BLEND_SRC_ALPHA;
    blendTarget.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;blendTarget.BlendOp=D3D11_BLEND_OP_ADD;
    blendTarget.SrcBlendAlpha=D3D11_BLEND_ONE;blendTarget.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;
    blendTarget.BlendOpAlpha=D3D11_BLEND_OP_ADD;blendTarget.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> alphaBlend;hr(dev->CreateBlendState(&blendDesc,&alphaBlend));
    PlanetCoverage coverage;HoloMotion motion;
    for(int occluded=0;occluded<2;++occluded) {
        ctx->ClearState();ctx->RSSetState(rs.Get());D3D11_VIEWPORT vp{0,0,8,8,0,1};ctx->RSSetViewports(1,&vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->IASetInputLayout(layout.Get());
        UINT stride=12,offset=0;ctx->IASetVertexBuffers(0,1,vb.GetAddressOf(),&stride,&offset);ctx->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R16_UINT,0);
        ctx->VSSetShader(vs.Get(),nullptr,0);ID3D11Buffer* cbs[]={cb0.Get(),cb1.Get()};ctx->VSSetConstantBuffers(0,2,cbs);
        ctx->PSSetShader(ps.Get(),nullptr,0);ctx->PSSetShaderResources(0,1,srv.GetAddressOf());ctx->PSSetConstantBuffers(12,1,originalInfo.GetAddressOf());
        ctx->OMSetRenderTargets(1,rtv.GetAddressOf(),target.dsv.Get());ctx->OMSetDepthStencilState(ds.Get(),7);
        float clear[4]{};ctx->ClearRenderTargetView(rtv.Get(),clear);ctx->ClearDepthStencilView(target.dsv.Get(),D3D11_CLEAR_DEPTH,0,0);
        ctx->DrawIndexed(3,0,0);
        if(occluded)ctx->ClearDepthStencilView(target.dsv.Get(),D3D11_CLEAR_DEPTH,.01f,0);
        auto beforeColour=read(dev,ctx,colour.Get()),beforeDepth=read(dev,ctx,target.tex.Get());
        float bf[4]={1,1,1,1};ctx->OMSetBlendState(alphaBlend.Get(),bf,0xffffffff);
        check(!coverage.begin(ctx,target.tex.Get(),motion,{'X',3,1,0,0,0},false),"default planet coverage declines blended colour");
        ctx->OMSetBlendState(nullptr,nullptr,0xffffffff);
        check(coverage.begin(ctx,target.tex.Get(),motion,{'X',3,1,0,0,0},false),"planet coverage starts for original opaque draw");
        ctx->DrawIndexed(3,0,0);coverage.end(ctx);
        check(beforeColour==read(dev,ctx,colour.Get()) && beforeDepth==read(dev,ctx,target.tex.Get()),"planet capture preserves every game colour and depth sample");
        ComPtr<ID3D11Resource> resource;motion.target()->GetResource(&resource);auto ids=read(dev,ctx,resource.Get());unsigned visible=0;
        for(float id:ids){check(id==0 || id==1,"planet coverage contains exact record IDs");visible+=id>0;}
        check(occluded?visible==0:visible>0,"planet visibility excludes nearer foreground depth");
        ComPtr<ID3D11PixelShader> restored;ComPtr<ID3D11Buffer> info;ComPtr<ID3D11RenderTargetView> rt;ComPtr<ID3D11DepthStencilView> depthView;
        ctx->PSGetShader(&restored,nullptr,nullptr);ctx->PSGetConstantBuffers(12,1,&info);ctx->OMGetRenderTargets(1,&rt,&depthView);
        check(restored==ps && info==originalInfo && rt==rtv && depthView==target.dsv,"planet coverage restores PS, constants and output bindings");
        motion.frameBoundary();
    }
    // Solar case: shifted prepass, alpha/discarding colour, and partial nearer cockpit.
    ctx->ClearState();ctx->RSSetState(rs.Get());
    D3D11_VIEWPORT solarVp{0,0,8,8,0,1};ctx->RSSetViewports(1,&solarVp);
    UINT solarStride=12,solarOffset=0;
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->IASetInputLayout(layout.Get());
    ctx->IASetVertexBuffers(0,1,solarVb.GetAddressOf(),&solarStride,&solarOffset);
    ctx->IASetIndexBuffer(solarIb.Get(),DXGI_FORMAT_R16_UINT,0);ctx->VSSetShader(vs.Get(),nullptr,0);
    ID3D11Buffer* solarCbs[]={cb0.Get(),cb1.Get()};ctx->VSSetConstantBuffers(0,2,solarCbs);
    ctx->PSSetShaderResources(1,1,srv.GetAddressOf());ctx->PSSetConstantBuffers(12,1,originalInfo.GetAddressOf());
    ctx->OMSetRenderTargets(1,rtv.GetAddressOf(),target.dsv.Get());ctx->OMSetDepthStencilState(ds.Get(),37);
    float clear[4]{};ctx->ClearRenderTargetView(rtv.Get(),clear);
    ctx->ClearDepthStencilView(target.dsv.Get(),D3D11_CLEAR_DEPTH,0,0);

    for(auto& value:model)value=0;
    model[16]=model[21]=model[31]=1e8f;model[27]=.024f;model[30]=1;
    ctx->UpdateSubresource(cb0.Get(),0,nullptr,model,0,0);
    ctx->PSSetShader(nullptr,nullptr,0);ctx->DrawIndexed(6,0,0);
    model[27]=.025f;ctx->UpdateSubresource(cb0.Get(),0,nullptr,model,0,0);
    ctx->PSSetShader(solarPs.Get(),nullptr,0);
    float blendFactors[4]={.2f,.4f,.6f,.8f};const UINT solarMask=0xa5a5a5a5;
    ctx->OMSetBlendState(alphaBlend.Get(),blendFactors,solarMask);ctx->DrawIndexed(6,0,0);

    D3D11_VIEWPORT cockpitVp{4,0,2,8,0,1};ctx->RSSetViewports(1,&cockpitVp);
    model[16]=model[21]=model[31]=1;model[27]=.9f;
    ctx->UpdateSubresource(cb0.Get(),0,nullptr,model,0,0);
    ctx->PSSetShader(nullptr,nullptr,0);ctx->OMSetBlendState(nullptr,nullptr,0xffffffff);
    ctx->DrawIndexed(6,0,0);
    ctx->RSSetViewports(1,&solarVp);model[16]=model[21]=model[31]=1e8f;model[27]=.025f;
    ctx->UpdateSubresource(cb0.Get(),0,nullptr,model,0,0);
    ctx->PSSetShader(solarPs.Get(),nullptr,0);ctx->OMSetBlendState(alphaBlend.Get(),blendFactors,solarMask);

    std::vector<float> solarColourBefore[4];
    for(UINT channel=0;channel<4;++channel)solarColourBefore[channel]=read(dev,ctx,colour.Get(),channel);
    auto solarDepthBefore=read(dev,ctx,target.tex.Get());
    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x) {
        const float expectedDepth=x<4?.024f/1e8f:x<6?.9f:.025f/1e8f;
        check(std::fabs(solarDepthBefore[y*8+x]-expectedDepth)<=expectedDepth*1e-6f,
              "solar discard retains shifted prepass, survivors and cockpit write distinct depths");
        check(std::fabs(solarColourBefore[0][y*8+x]-(x<4?0:.1f))<1e-6f,
              "solar original colour has actual alpha-blended survivors and discarded samples");
    }
    check(!coverage.begin(ctx,target.tex.Get(),motion,{'X',6,1,0,0,0}),"default planet path declines solar blend");
    check(coverage.begin(ctx,target.tex.Get(),motion,{'X',6,1,0,0,0},true),"solar coverage accepts blended draw");
    ctx->DrawIndexed(6,0,0);coverage.end(ctx);
    for(UINT channel=0;channel<4;++channel)
        check(solarColourBefore[channel]==read(dev,ctx,colour.Get(),channel),"solar coverage preserves every game colour channel");
    check(solarDepthBefore==read(dev,ctx,target.tex.Get()),"solar coverage preserves exact game depth");
    ComPtr<ID3D11Resource> solarResource;motion.target()->GetResource(&solarResource);
    auto solarIds=read(dev,ctx,solarResource.Get()),solarCoverageDepth=read(dev,ctx,solarResource.Get(),1);
    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x) {
        check(solarIds[y*8+x]==(x>=6?1.0f:0.0f),"solar coverage excludes discard and cockpit pixels");
        check(solarCoverageDepth[y*8+x]==(x>=6?solarDepthBefore[y*8+x]:0),"solar coverage carries exact colour-pass depth");
    }
    ComPtr<ID3D11PixelShader> solarPsRestored;ComPtr<ID3D11Buffer> solarCbRestored;
    ComPtr<ID3D11RenderTargetView> solarRtRestored;ComPtr<ID3D11DepthStencilView> solarDsvRestored;
    ComPtr<ID3D11DepthStencilState> solarDsRestored;ComPtr<ID3D11BlendState> solarBlendRestored;
    UINT solarRefRestored=0,solarMaskRestored=0;float solarFactorsRestored[4]{};
    ctx->PSGetShader(&solarPsRestored,nullptr,nullptr);ctx->PSGetConstantBuffers(12,1,&solarCbRestored);
    ctx->OMGetRenderTargets(1,&solarRtRestored,&solarDsvRestored);
    ctx->OMGetDepthStencilState(&solarDsRestored,&solarRefRestored);
    ctx->OMGetBlendState(&solarBlendRestored,solarFactorsRestored,&solarMaskRestored);
    check(solarPsRestored==solarPs && solarCbRestored==originalInfo && solarRtRestored==rtv &&
          solarDsvRestored==target.dsv && solarDsRestored==ds && solarBlendRestored==alphaBlend &&
          solarRefRestored==37 && solarMaskRestored==solarMask,"solar coverage restores shader, constants and output state");
    for(UINT i=0;i<4;++i)check(solarFactorsRestored[i]==blendFactors[i],"solar coverage restores blend factors");
    ctx->ClearState();
}
