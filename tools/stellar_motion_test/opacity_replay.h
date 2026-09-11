#pragma once
// Compare the transcribed alpha directly with the original local game PS.
void verifyRingOpacity(ID3D11Device* dev,ID3D11DeviceContext* ctx,const char* path){
    std::ifstream f(path,std::ios::binary);std::vector<char> bytes((std::istreambuf_iterator<char>(f)),{});check(bytes.size()>32,"ring pixel shader fixture");
    ComPtr<ID3D11PixelShader> ps[2];hr(dev->CreatePixelShader(bytes.data(),bytes.size(),nullptr,&ps[0]));
    std::string source=edvr::kRingCoverage;auto at=source.find("float4 main(");check(at!=std::string::npos,"ring coverage entry");source.resize(at);source+="float4 main(In i):SV_Target{return ringAlpha(i);}";
    auto code=compile(source.c_str(),"ps_5_0");hr(dev->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&ps[1]));
    auto vc=compile("cbuffer C:register(b0){float4 nr;float4 pb;}struct O{float4 n:TEXCOORD1;float4 p:TEXCOORD2;float3 t:TEXCOORD3;float2 uv:TEXCOORD4;float4 pos:SV_Position;};O main(uint id:SV_VertexID){O o;float2 v=float2((id<<1)&2,id&2);o.pos=float4(v*float2(2,-2)+float2(-1,1),.5,1);o.n=nr;o.p=pb;o.t=float3(1,0,0);o.uv=.5;return o;}","vs_5_0");
    ComPtr<ID3D11VertexShader> vs;hr(dev->CreateVertexShader(vc->GetBufferPointer(),vc->GetBufferSize(),nullptr,&vs));
    auto buffer=[&](UINT n,const void* p){D3D11_BUFFER_DESC d{};d.ByteWidth=n;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;D3D11_SUBRESOURCE_DATA init{p,0,0};ComPtr<ID3D11Buffer> b;hr(dev->CreateBuffer(&d,&init,&b));return b;};
    float scene[294*4]{};scene[137*4]=100;scene[137*4+1]=1000;scene[141*4+3]=.5f;scene[291*4+1]=1;scene[90*4+1]=1;
    float material[28]={200000,2000000,-.5f,20,200000,.5f,.5f,.025f,150000,800000,.3f,50,1,.1f,5,1,5,.2f,5,30,.000006f,1.75f,35,50,.25f,0,0,0};
    auto cb1=buffer(sizeof(scene),scene),cb2=buffer(sizeof(material),material);float params[8]={0,1,0,500,0,-1e7f,0,.5f};auto cb0=buffer(sizeof(params),params);
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=1;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> tex[3];ComPtr<ID3D11ShaderResourceView> srv[3];for(int i=0;i<3;++i){hr(dev->CreateTexture2D(&td,nullptr,&tex[i]));hr(dev->CreateShaderResourceView(tex[i].Get(),nullptr,&srv[i]));}
    td.Width=td.Height=8;td.BindFlags=D3D11_BIND_RENDER_TARGET;ComPtr<ID3D11Texture2D> target,stage;ComPtr<ID3D11RenderTargetView> rtv;hr(dev->CreateTexture2D(&td,nullptr,&target));hr(dev->CreateRenderTargetView(target.Get(),nullptr,&rtv));
    td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;hr(dev->CreateTexture2D(&td,nullptr,&stage));
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;ComPtr<ID3D11SamplerState> sampler;hr(dev->CreateSamplerState(&sd,&sampler));
    float maximum=0;UINT cases=0;
    for(float distance:{100000.0f,300000.0f,1e7f})for(float band:{0.0f,.2f,.8f})for(float radial:{0.0f,.36f,.8f})for(float detail:{0.0f,.8f}){
        float sample[3][4]={{.3f,.4f,.2f,band},{radial,.69f,.14f,.7f},{.1f,.2f,.3f,detail}};
        for(int i=0;i<3;++i)ctx->UpdateSubresource(tex[i].Get(),0,nullptr,sample[i],16,0);params[5]=-distance;ctx->UpdateSubresource(cb0.Get(),0,nullptr,params,0,0);
        float alpha[2]{};
        for(int i=0;i<2;++i){ctx->ClearState();ctx->VSSetShader(vs.Get(),nullptr,0);ctx->VSSetConstantBuffers(0,1,cb0.GetAddressOf());ctx->PSSetShader(ps[i].Get(),nullptr,0);
            ID3D11Buffer* cb[]={cb1.Get(),cb2.Get()};ctx->PSSetConstantBuffers(1,2,cb);ID3D11ShaderResourceView* views[]={srv[0].Get(),srv[1].Get(),nullptr,srv[2].Get()};ctx->PSSetShaderResources(0,4,views);ctx->PSSetSamplers(1,1,sampler.GetAddressOf());
            D3D11_VIEWPORT vp{0,0,8,8,0,1};ctx->RSSetViewports(1,&vp);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->OMSetRenderTargets(1,rtv.GetAddressOf(),nullptr);ctx->Draw(3,0);ctx->ClearState();ctx->CopyResource(stage.Get(),target.Get());
            D3D11_MAPPED_SUBRESOURCE m{};hr(ctx->Map(stage.Get(),0,D3D11_MAP_READ,0,&m));alpha[i]=static_cast<float*>(m.pData)[3];ctx->Unmap(stage.Get(),0);}
        float error=std::fabs(alpha[0]-alpha[1]);if(error>maximum)maximum=error;if(error>=1e-5f)std::printf("ring alpha %.8f vs %.8f\n",alpha[0],alpha[1]);check(error<1e-5f,"ring coverage opacity matches original pixel shader");++cases;
    }
    std::printf("ring opacity: %u original shader comparisons, max error %.8f\n",cases,maximum);
}
