// Exercise the production temporal shader with the screen map bound.
// Reflection locates cbuffer fields so a layout change cannot silently
// leave this testing different parameters from the shipping shader.
#include <d3d11shader.h>
void testScreenConsumers(ID3D11Device* dev,ID3D11DeviceContext* ctx) {
    std::ifstream input("src/d3d11/temporal_pass.cpp");
    std::string source((std::istreambuf_iterator<char>(input)),{}),hlsl;
    auto cursor=source.find("constexpr char kTemporalCsHlsl[]");
    auto end=source.find(")HLSL\";",cursor);
    check(cursor!=std::string::npos && end!=std::string::npos,"production temporal shader found");
    for(;;) {
        auto begin=source.find("R\"HLSL(",cursor);
        if(begin==std::string::npos || begin>end)break;
        begin+=7;auto close=source.find(")HLSL\"",begin);
        check(close!=std::string::npos && close<=end,"complete temporal shader literal");
        hlsl+=source.substr(begin,close-begin);cursor=close+6;
    }
    hlsl+="\n[numthreads(8,8,1)]void screenTest(uint3 id:SV_DispatchThreadID){"
          "float3 hy;uint world,depthN;float2 motion;float depth;"
          "bool ok=fetchHistoryT(float2(id.xy),dR0.xyz,dR1.xyz,dR2.xyz,tvUsed.xyz,"
          "false,true,world,motion,depth,depthN,hy);"
          "O[id.xy]=float4(motion,zSceneAt(region.xy+int2(id.xy)),ok?1:0);}";
    auto mvCode=compile(hlsl.c_str(),"cs_5_0","mv");
    auto nativeCode=compile(hlsl.c_str(),"cs_5_0","screenTest");
    ComPtr<ID3D11ComputeShader> mv,native;
    hr(dev->CreateComputeShader(mvCode->GetBufferPointer(),mvCode->GetBufferSize(),nullptr,&mv));
    hr(dev->CreateComputeShader(nativeCode->GetBufferPointer(),nativeCode->GetBufferSize(),nullptr,&native));
    ComPtr<ID3D11ShaderReflection> reflection;
    hr(D3DReflect(mvCode->GetBufferPointer(),mvCode->GetBufferSize(),__uuidof(ID3D11ShaderReflection),&reflection));
    auto* parameters=reflection->GetConstantBufferByName("P");
    D3D11_SHADER_BUFFER_DESC pd{};hr(parameters->GetDesc(&pd));
    std::vector<char> data(pd.Size,0);
    auto set=[&](const char* name,const void* value,UINT bytes){
        D3D11_SHADER_VARIABLE_DESC vd{};hr(parameters->GetVariableByName(name)->GetDesc(&vd));
        check(bytes<=vd.Size && vd.StartOffset+bytes<=data.size(),"reflected temporal field extent");
        std::memcpy(data.data()+vd.StartOffset,value,bytes);
    };
    auto floats=[&](const char* name,float a,float b,float c,float d){float v[4]={a,b,c,d};set(name,v,16);};
    int region[4]={2,3,10,11},size[2]={8,8},texSize[2]={16,16};
    set("region",region,16);set("size",size,8);set("texSize",texSize,8);
    floats("tanNow",-1,1,-1,1);floats("tanPrev",-1,1,-1,1);
    floats("dR0",1,0,0,0);floats("dR1",0,1,0,0);floats("dR2",0,0,1,0);
    floats("knobs",0,1,.025f,0);floats("jit",.25f,-.375f,0,.5f);
    floats("holoJitter",.25f,-.375f,1,0);
    // A deliberately wrong eye-space depth predecessor would reject every
    // screen pixel if its mover mask survived the screen override.
    floats("movers",1,.1f,1,0);
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=pd.Size;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> cb;hr(dev->CreateBuffer(&bd,nullptr,&cb));
    auto texture=[&](UINT n,DXGI_FORMAT fmt,UINT bind){
        D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=n;td.Format=fmt;
        td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.BindFlags=bind;
        ComPtr<ID3D11Texture2D> t;hr(dev->CreateTexture2D(&td,nullptr,&t));return t;
    };
    auto map=texture(16,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_BIND_SHADER_RESOURCE);
    auto scene=texture(16,DXGI_FORMAT_R32_FLOAT,D3D11_BIND_SHADER_RESOURCE);
    auto motion=texture(8,DXGI_FORMAT_R32G32_FLOAT,D3D11_BIND_UNORDERED_ACCESS);
    auto depth=texture(8,DXGI_FORMAT_R32_FLOAT,D3D11_BIND_UNORDERED_ACCESS);
    auto mask=texture(8,DXGI_FORMAT_R32_FLOAT,D3D11_BIND_UNORDERED_ACCESS);
    auto result=texture(8,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_BIND_UNORDERED_ACCESS);
    ComPtr<ID3D11ShaderResourceView> ms,zs;
    hr(dev->CreateShaderResourceView(map.Get(),nullptr,&ms));hr(dev->CreateShaderResourceView(scene.Get(),nullptr,&zs));
    ComPtr<ID3D11UnorderedAccessView> mu,zu,ku,ru;
    hr(dev->CreateUnorderedAccessView(motion.Get(),nullptr,&mu));hr(dev->CreateUnorderedAccessView(depth.Get(),nullptr,&zu));
    hr(dev->CreateUnorderedAccessView(mask.Get(),nullptr,&ku));hr(dev->CreateUnorderedAccessView(result.Get(),nullptr,&ru));
    std::vector<float> pixels(16*16*4,0),z(16*16,.25f);
    ctx->UpdateSubresource(scene.Get(),0,nullptr,z.data(),16*4,0);
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;hr(dev->CreateSamplerState(&sd,&sampler));
    for(int enabled:{0,32}) for(int valid:{0,1,2}) {
        std::fill(pixels.begin(),pixels.end(),0.0f);
        // The first column is outside screen coverage. This also verifies
        // that region offsets do not bleed screen motion onto other pixels.
        for(int y=3;y<11;++y)for(int x=3;x<10;++x){unsigned i=(y*16+x)*4;
            pixels[i]=.75f;pixels[i+1]=-.625f;pixels[i+2]=.005f;pixels[i+3]=float(valid);}
        ctx->UpdateSubresource(map.Get(),0,nullptr,pixels.data(),16*16,0);
        floats("probe",1,0,0,float(enabled));ctx->UpdateSubresource(cb.Get(),0,nullptr,data.data(),0,0);
        ctx->ClearState();ctx->CSSetConstantBuffers(0,1,cb.GetAddressOf());
        ctx->CSSetShaderResources(2,1,zs.GetAddressOf());ctx->CSSetShaderResources(14,1,ms.GetAddressOf());
        ctx->CSSetSamplers(0,1,sampler.GetAddressOf());
        ID3D11UnorderedAccessView* uavs[3]={mu.Get(),zu.Get(),ku.Get()};ctx->CSSetUnorderedAccessViews(3,3,uavs,nullptr);
        ctx->CSSetShader(mv.Get(),nullptr,0);ctx->Dispatch(1,1,1);
        auto m=read(dev,ctx,motion.Get()),d=read(dev,ctx,depth.Get()),k=read(dev,ctx,mask.Get());
        for(int y=0;y<8;++y)for(int x=0;x<8;++x){unsigned i=y*8+x;bool screen=enabled && valid && x>0;
            float mx=screen?(valid==1?1.0f:16.0f):0.0f,my=screen?(valid==1?-1.0f:16.0f):0.0f;
            check(std::fabs(m[2*i]-mx)<1e-5f && std::fabs(m[2*i+1]-my)<1e-5f,"DLSS consumes screen motion once with correct jitter and coverage");
            check(std::fabs(d[i]-(screen?.005f:.25f))<1e-6f,"DLSS consumes exact source depth only inside screen");
            if(screen)check(k[i]==0,"screen bypasses unrelated eye-space mover rejection");
        }
        ID3D11UnorderedAccessView* empty[3]{};ctx->CSSetUnorderedAccessViews(3,3,empty,nullptr);
        ctx->CSSetUnorderedAccessViews(0,1,ru.GetAddressOf(),nullptr);ctx->CSSetShader(native.Get(),nullptr,0);ctx->Dispatch(1,1,1);
        auto n=read(dev,ctx,result.Get());
        for(int y=1;y<7;++y)for(int x=0;x<7;++x){unsigned i=y*8+x;bool screen=enabled && valid && x>0;
            if(screen && valid==2)check(n[4*i+3]==0,"native TAA rejects screen disocclusion");
            else {
                check(n[4*i+3]==1,"native TAA accepts valid history");
                check(std::fabs(n[4*i]-(screen?1.0f:0))<1e-5f && std::fabs(n[4*i+1]-(screen?-1.0f:0))<1e-5f,"native TAA uses same screen motion and jitter");
            }
        }
    }
    ctx->ClearState();
}
