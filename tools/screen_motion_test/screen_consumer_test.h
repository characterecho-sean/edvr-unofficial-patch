// Exercise the production temporal shader with the screen map bound.
// Reflection locates cbuffer fields so a layout change cannot silently
// leave this testing different parameters from the shipping shader.
#include <d3d11shader.h>
#include "temporal_shader_bytecode.h"
void testScreenConsumers(ID3D11Device* dev,ID3D11DeviceContext* ctx) {
    // Validate every shipping blob on WARP, including the AA entry point
    // that the screen-motion fixture below does not dispatch.
    struct EmbeddedShader { const void* data; size_t size; const char* name; };
    const EmbeddedShader embedded[] = {
        {edvr::kTemporalMvFastBytecode, sizeof(edvr::kTemporalMvFastBytecode), "temporal_mv_fast_cs"},
        {edvr::kTemporalMvBytecode, sizeof(edvr::kTemporalMvBytecode), "temporal_mv_cs"},
        {edvr::kTemporalMvTraceBytecode, sizeof(edvr::kTemporalMvTraceBytecode), "temporal_mv_trace_cs"},
        {edvr::kTemporalAaBytecode, sizeof(edvr::kTemporalAaBytecode), "temporal_aa_cs"},
        {edvr::kTemporalAaFastBytecode, sizeof(edvr::kTemporalAaFastBytecode), "temporal_aa_fast_cs"}
    };
    for (const auto& blob : embedded) {
        ComPtr<ID3D11ComputeShader> shader;
        hr(dev->CreateComputeShader(blob.data, blob.size, nullptr, &shader));
        check(shader.Get() != nullptr, blob.name);
        std::printf("PASS: embedded %s creates on WARP (%zu bytes)\n", blob.name, blob.size);
    }
    std::ifstream input("src/d3d11/temporal_shader_source.h");
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
    hlsl+="\n[numthreads(8,8,1)]void mode5Test(uint3 id:SV_DispatchThreadID){"
          "float2 pp;float zp;bool ok=holoPixel(float2(id.xy),jit.xy,pp,zp);"
          "O[id.xy]=float4(pp,zp,ok?1:0);}";
    auto mvCode=compile(hlsl.c_str(),"cs_5_0","mv");
    const std::string traceHlsl="#define EDVR_TEMPORAL_TRACE 1\n"+hlsl;
    auto traceCode=compile(traceHlsl.c_str(),"cs_5_0","mv");
    auto nativeCode=compile(hlsl.c_str(),"cs_5_0","screenTest");
    auto mode5Code=compile(hlsl.c_str(),"cs_5_0","mode5Test");
    ComPtr<ID3D11ComputeShader> mv,mvTrace,native,mode5;
    hr(dev->CreateComputeShader(mvCode->GetBufferPointer(),mvCode->GetBufferSize(),nullptr,&mv));
    hr(dev->CreateComputeShader(traceCode->GetBufferPointer(),traceCode->GetBufferSize(),nullptr,&mvTrace));
    hr(dev->CreateComputeShader(nativeCode->GetBufferPointer(),nativeCode->GetBufferSize(),nullptr,&native));
    hr(dev->CreateComputeShader(mode5Code->GetBufferPointer(),mode5Code->GetBufferSize(),nullptr,&mode5));
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
    auto decision=texture(8,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_BIND_UNORDERED_ACCESS);
    ComPtr<ID3D11ShaderResourceView> ms,zs;
    hr(dev->CreateShaderResourceView(map.Get(),nullptr,&ms));hr(dev->CreateShaderResourceView(scene.Get(),nullptr,&zs));
    ComPtr<ID3D11UnorderedAccessView> mu,zu,ku,ru,du;
    hr(dev->CreateUnorderedAccessView(motion.Get(),nullptr,&mu));hr(dev->CreateUnorderedAccessView(depth.Get(),nullptr,&zu));
    hr(dev->CreateUnorderedAccessView(mask.Get(),nullptr,&ku));hr(dev->CreateUnorderedAccessView(result.Get(),nullptr,&ru));
    hr(dev->CreateUnorderedAccessView(decision.Get(),nullptr,&du));
    std::vector<float> pixels(16*16*4,0),z(16*16,.25f);
    ctx->UpdateSubresource(scene.Get(),0,nullptr,z.data(),16*4,0);
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;hr(dev->CreateSamplerState(&sd,&sampler));
    for(int enabled:{0,32}) for(int valid:{0,1,2,3}) {
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
        ctx->ClearState();ctx->CSSetConstantBuffers(0,1,cb.GetAddressOf());
        ctx->CSSetShaderResources(2,1,zs.GetAddressOf());ctx->CSSetShaderResources(14,1,ms.GetAddressOf());
        ctx->CSSetSamplers(0,1,sampler.GetAddressOf());
        ID3D11UnorderedAccessView* traceOutputs[5]={mu.Get(),zu.Get(),ku.Get(),nullptr,du.Get()};
        ctx->CSSetUnorderedAccessViews(3,5,traceOutputs,nullptr);ctx->CSSetShader(mvTrace.Get(),nullptr,0);ctx->Dispatch(1,1,1);
        auto tracedMotion=read(dev,ctx,motion.Get()),decisions=read(dev,ctx,decision.Get());
        check(tracedMotion==m,"capture-only trace variant preserves production motion output");
        for(int y=0;y<8;++y)for(int x=0;x<8;++x){unsigned i=y*8+x;bool screen=enabled && valid && x>0;
            float mx=screen?(valid!=2?1.0f:16.0f):0.0f,my=screen?(valid!=2?-1.0f:16.0f):0.0f;
            check(std::fabs(m[2*i]-mx)<1e-5f && std::fabs(m[2*i+1]-my)<1e-5f,"DLSS consumes screen motion once with correct jitter and coverage");
            check(std::fabs(d[i]-(screen?.005f:.25f))<1e-6f,"DLSS consumes exact source depth only inside screen");
            if(screen)check(k[i]==0,"screen pixels keep a clear reject mask");
            const UINT bits=UINT(decisions[4*i+3]+.5f);
            if(screen) {
                check((bits&15u)==10u,"trace labels the final screen path");
                check(((bits&32u)!=0)==(valid==2),"trace distinguishes invalid screen history");
                check(((bits&1024u)!=0)==(valid!=2),"trace projection-valid bit follows screen validity");
                check(std::fabs(decisions[4*i]-(valid==2?0.0f:1.0f))<1e-5f &&
                      std::fabs(decisions[4*i+1]-(valid==2?0.0f:-1.0f))<1e-5f && decisions[4*i+2]==0,
                      "trace records physical screen motion and zeroes invalid correspondence");
            } else check((bits&15u)==1u && (bits&1024u)!=0,"trace labels valid head fallback");
        }
        ID3D11UnorderedAccessView* empty[3]{};ctx->CSSetUnorderedAccessViews(3,3,empty,nullptr);
        ctx->CSSetUnorderedAccessViews(0,1,ru.GetAddressOf(),nullptr);ctx->CSSetShader(native.Get(),nullptr,0);ctx->Dispatch(1,1,1);
        auto n=read(dev,ctx,result.Get());
        for(int y=1;y<7;++y)for(int x=0;x<7;++x){unsigned i=y*8+x;bool screen=enabled && valid && x>0;
            if(screen && valid>1)check(n[4*i+3]==0,"native TAA rejects source disocclusion and source UI history");
            else {
                check(n[4*i+3]==1,"native TAA accepts valid history");
                check(std::fabs(n[4*i]-(screen?1.0f:0))<1e-5f && std::fabs(n[4*i+1]-(screen?-1.0f:0))<1e-5f,"native TAA uses same screen motion and jitter");
            }
        }
    }
    // The same shipping MV shader must reject background which was hidden
    // by a hull, without changing UI/screen motion or valid sky history.
    auto previous=texture(8,DXGI_FORMAT_R32_FLOAT,D3D11_BIND_SHADER_RESOURCE);
    auto ui=texture(16,DXGI_FORMAT_R32_FLOAT,D3D11_BIND_SHADER_RESOURCE);
    auto privateUiDepth=texture(16,DXGI_FORMAT_R32_FLOAT,D3D11_BIND_SHADER_RESOURCE);
    auto smokeDepth=texture(16,DXGI_FORMAT_R32_FLOAT,D3D11_BIND_SHADER_RESOURCE);
    ComPtr<ID3D11ShaderResourceView> previousView,uiView,privateUiDepthView,smokeDepthView;
    hr(dev->CreateShaderResourceView(previous.Get(),nullptr,&previousView));
    hr(dev->CreateShaderResourceView(ui.Get(),nullptr,&uiView));
    hr(dev->CreateShaderResourceView(privateUiDepth.Get(),nullptr,&privateUiDepthView));
    hr(dev->CreateShaderResourceView(smokeDepth.Get(),nullptr,&smokeDepthView));
    std::vector<float> prior(64,0),marks(256,0);
    auto coverage=texture(16,DXGI_FORMAT_R32G32_FLOAT,D3D11_BIND_SHADER_RESOURCE);
    ComPtr<ID3D11ShaderResourceView> coverageView,recordView;
    hr(dev->CreateShaderResourceView(coverage.Get(),nullptr,&coverageView));
    float record[60]{};reinterpret_cast<UINT*>(record)[15]=1;
    record[44]=record[49]=record[54]=1;record[46]=.15625f;record[50]=.03125f;
    record[56]=record[59]=1;record[57]=record[58]=16;
    D3D11_BUFFER_DESC rb{};rb.ByteWidth=rb.StructureByteStride=sizeof(record);
    rb.BindFlags=D3D11_BIND_SHADER_RESOURCE;rb.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    D3D11_SUBRESOURCE_DATA init{record,0,0};ComPtr<ID3D11Buffer> rigidRecord;
    hr(dev->CreateBuffer(&rb,&init,&rigidRecord));hr(dev->CreateShaderResourceView(rigidRecord.Get(),nullptr,&recordView));
    std::vector<float> coveragePixels(16*16*2,0);
    coveragePixels[(7*16+6)*2]=1;coveragePixels[(7*16+6)*2+1]=.000025f;
    ctx->UpdateSubresource(coverage.Get(),0,nullptr,coveragePixels.data(),16*8,0);
    floats("dR0",1,0,-.5f,0); // two input pixels horizontally
    floats("holoJitter",.75f,.25f,1,1);
    for(int variant=0;variant<3;++variant) {
        ComPtr<ID3D11ComputeShader> tested=mv;
        if(variant)hr(dev->CreateComputeShader(embedded[variant-1].data,embedded[variant-1].size,nullptr,&tested));
        for(int test=0;test<23;++test) {
            if(test==12)continue;   // the mesh records' path (t15/t16), retired 2026-09-23
            std::fill(z.begin(),z.end(),0.f);std::fill(prior.begin(),prior.end(),0.f);
            std::fill(pixels.begin(),pixels.end(),0.f);std::fill(marks.begin(),marks.end(),0.f);
            floats("probe",1,0,1,32);floats("holoJitter",.75f,.25f,1,1);
            // At (4,4), motion goes to (6,4); prior raw raster is (5.25,3.75).
            // A hull at (6,4) covers its filter footprint. (7,4) does not.
            prior[4*8+6]=.025f;
            if(test==0)prior[4*8+6]=0;
            if(test==2){prior[4*8+6]=0;prior[4*8+7]=.025f;}
            if(test==3)floats("holoJitter",.75f,.25f,0,1);
            if(test==4)floats("holoJitter",.75f,.25f,1,0);
            // The reactive/UI-kind input is region-sized, unlike scene depth.
            if(test==5 || test==6)marks[(4*16)+4]=float(test-4)/255.f;
            if(test==7){unsigned at=(7*16+6)*4;pixels[at]=1.25f;pixels[at+1]=-.25f;pixels[at+2]=.005f;pixels[at+3]=1;}
            // A distant surface behind the hull is also newly exposed;
            // a surface at the same depth, or in front, keeps its history.
            if(test>=8 && test<=10)std::fill(z.begin(),z.end(),test==8?.000025f:test==9?.025f:.05f);
            if(test==11)floats("dR0",1,0,-4,0); // off-image history
            if(test==13){std::fill(z.begin(),z.end(),.000025f);floats("probe",1,0,1,48);}
            if(test==14 || test==15){std::fill(z.begin(),z.end(),.025f);prior[4*8+6]=.025f*(test==14?1.02f:1.04f);}
            reinterpret_cast<UINT*>(record)[15]=test>=16 && test<20?4:1;
            record[46]=test>=16 && test<20?.5f:.15625f;
            if(test>=16 && test<20) {
                std::fill(z.begin(),z.end(),test==19?.025f:.000025f);floats("probe",1,0,1,48);
                if(test==17 || test==18)marks[4*16+4]=float(test-16)/255.f;
            }
            // The still scene (2026-09-17): the nearer surface that filled the
            // previous footprint is beside the pixel NOW as well -- a rock
            // beside the terrain (20), a crater rim beside the sky (21) -- so
            // the history it filtered into is as this frame's own, and nothing
            // was hidden. The previous (6,4) is (4.75,4.25) in this raster, so
            // the scene texel (region (2,3) on) is (7,7); rasterised at (6,4),
            // scene (8,7), a texel past the footprint the rounding directs, it
            // has moved on (22): hidden.
            if(test>=20){std::fill(z.begin(),z.end(),test==21?0.f:.000025f);z[7*16+(test==22?8:7)]=.025f;}
            ctx->UpdateSubresource(rigidRecord.Get(),0,nullptr,record,0,0);
            ctx->UpdateSubresource(scene.Get(),0,nullptr,z.data(),16*4,0);
            ctx->UpdateSubresource(previous.Get(),0,nullptr,prior.data(),8*4,0);
            ctx->UpdateSubresource(ui.Get(),0,nullptr,marks.data(),16*4,0);
            ctx->UpdateSubresource(map.Get(),0,nullptr,pixels.data(),16*16,0);
            ctx->UpdateSubresource(cb.Get(),0,nullptr,data.data(),0,0);
            ctx->ClearState();ctx->CSSetConstantBuffers(0,1,cb.GetAddressOf());
            ctx->CSSetShaderResources(2,1,zs.GetAddressOf());ctx->CSSetShaderResources(3,1,previousView.GetAddressOf());
            ctx->CSSetShaderResources(4,1,uiView.GetAddressOf());ctx->CSSetShaderResources(14,1,ms.GetAddressOf());
            if(test==13 || (test>=16 && test<20)){ID3D11ShaderResourceView* exact[2]={coverageView.Get(),recordView.Get()};ctx->CSSetShaderResources(12,2,exact);}
            ctx->CSSetSamplers(0,1,sampler.GetAddressOf());
            ID3D11UnorderedAccessView* outputs[3]={mu.Get(),zu.Get(),ku.Get()};ctx->CSSetUnorderedAccessViews(3,3,outputs,nullptr);
            ctx->CSSetShader(tested.Get(),nullptr,0);ctx->Dispatch(1,1,1);
            auto m=read(dev,ctx,motion.Get()),depths=read(dev,ctx,depth.Get());unsigned at=4*8+4;
            const bool rejected=test==1 || test==8 || test==15 || test==22;
            float mx=rejected || test==11?16.f:2.f,my=rejected?16.f:0.f;
            if(test==16)mx=4.75f;
            if(std::fabs(m[2*at]-mx)>=1e-5 || std::fabs(m[2*at+1]-my)>=1e-5)
                std::printf("background case %d variant %d: got %g,%g expected %g,%g\n",test,variant,m[2*at],m[2*at+1],mx,my);
            check(std::fabs(m[2*at]-mx)<1e-5 && std::fabs(m[2*at+1]-my)<1e-5,
                  "DLSS background rejection respects depth, jitter, continuity, bounds, UI and screens");
            check(std::fabs(depths[at]-(test==7?.005f:z[7*16+6]))<1e-6,
                  "history rejection never changes current DLSS depth");
            if(variant==0 && (test==1 || test==5 || test==6 || test==7)) {
                ctx->ClearState();ctx->CSSetConstantBuffers(0,1,cb.GetAddressOf());
                ctx->CSSetShaderResources(2,1,zs.GetAddressOf());ctx->CSSetShaderResources(3,1,previousView.GetAddressOf());
                ctx->CSSetShaderResources(4,1,uiView.GetAddressOf());ctx->CSSetShaderResources(14,1,ms.GetAddressOf());
                ctx->CSSetSamplers(0,1,sampler.GetAddressOf());
                ID3D11UnorderedAccessView* traced[5]={mu.Get(),zu.Get(),ku.Get(),nullptr,du.Get()};
                ctx->CSSetUnorderedAccessViews(3,5,traced,nullptr);ctx->CSSetShader(mvTrace.Get(),nullptr,0);ctx->Dispatch(1,1,1);
                auto tracedMv=read(dev,ctx,motion.Get()),decisionPixels=read(dev,ctx,decision.Get());
                check(tracedMv==m,"trace side channel leaves hidden/UI/screen production MV unchanged");
                const UINT bits=UINT(decisionPixels[4*at+3]+.5f);
                if(test==1) {
                    check((bits&16u)!=0 && (bits&15u)==1u,"trace exposes forced hidden-history miss on head path");
                    check(std::fabs(decisionPixels[4*at]-2.0f)<1e-5f && std::fabs(decisionPixels[4*at+1])<1e-5f,
                          "trace keeps physical motion beside hidden sentinel production MV");
                } else if(test==5 || test==6) {
                    check((bits&64u)!=0 && (bits&16u)==0,"trace labels current UI and its hidden-history exemption");
                } else {
                    check((bits&15u)==10u && (bits&512u)!=0,"trace labels tracked screen override");
                }
            }
            floats("dR0",1,0,-.5f,0);
        }
    }
    // Mode 5 is the corona streak path.  Its R32G32 coverage is accepted
    // only when the merged depth is exact, the affine record matched, and
    // the source pixel is the smoke/corona UI kind (3).  Probe holoPixel
    // directly so UI-kind and depth gates cannot be masked by the ordinary
    // camera fallback in mv.
    std::vector<float> privateDepth(16*16,0),smoke(16*16,0);
    ID3D11ShaderResourceView* coronaInputs[2]={coverageView.Get(),recordView.Get()};
    auto mode5Probe=[&](int kind,int nearer,int matched) {
        std::fill(z.begin(),z.end(),.000025f);
        std::fill(privateDepth.begin(),privateDepth.end(),0.0f);
        std::fill(smoke.begin(),smoke.end(),0.0f);
        std::fill(marks.begin(),marks.end(),0.0f);
        marks[4*16+4]=float(kind)/255.0f;
        if(nearer==0) z[7*16+6]=.025f;
        if(nearer==1) privateDepth[7*16+6]=.025f;
        if(nearer==2) smoke[7*16+6]=.025f;
        reinterpret_cast<UINT*>(record)[15]=5;
        for(int i=16;i<28;++i) record[i]=1.0f; // mode-5 width/axis key
        record[44]=record[49]=record[54]=1.0f;
        record[46]=.25f;record[50]=.03125f;
        record[56]=1.0f;record[59]=matched?1.0f:0.0f;
        // Keep a valid affine clip predecessor available to the corona path.
        record[32]=1.0f;record[37]=1.0f;record[42]=1.0f;record[43]=1.0f;
        ctx->UpdateSubresource(scene.Get(),0,nullptr,z.data(),16*4,0);
        ctx->UpdateSubresource(privateUiDepth.Get(),0,nullptr,privateDepth.data(),16*4,0);
        ctx->UpdateSubresource(smokeDepth.Get(),0,nullptr,smoke.data(),16*4,0);
        ctx->UpdateSubresource(ui.Get(),0,nullptr,marks.data(),16*4,0);
        ctx->UpdateSubresource(rigidRecord.Get(),0,nullptr,record,0,0);
        floats("jit",.25f,-.375f,0,.5f);floats("probe",1,0,1,16);
        floats("holoJitter",.75f,.25f,1,1);
        ctx->UpdateSubresource(cb.Get(),0,nullptr,data.data(),0,0);
        ctx->ClearState();ctx->CSSetConstantBuffers(0,1,cb.GetAddressOf());
        ctx->CSSetShaderResources(2,1,zs.GetAddressOf());ctx->CSSetShaderResources(4,1,uiView.GetAddressOf());
        ctx->CSSetShaderResources(6,1,smokeDepthView.GetAddressOf());ctx->CSSetShaderResources(7,1,privateUiDepthView.GetAddressOf());
        ctx->CSSetShaderResources(12,2,coronaInputs);
        ctx->CSSetUnorderedAccessViews(0,1,ru.GetAddressOf(),nullptr);
        ctx->CSSetShader(mode5.Get(),nullptr,0);ctx->Dispatch(1,1,1);
        auto out=read(dev,ctx,result.Get());ctx->ClearState();return out;
    };
    auto corona=mode5Probe(3,-1,1);unsigned coronaAt=4*8+4;
    check(corona[4*coronaAt+3]==1,"mode5 accepts exact merged depth, matched record and UI kind 3");
    for(int kind=0;kind<3;++kind) {
        auto out=mode5Probe(kind,-1,1);
        check(out[4*coronaAt+3]==0,"mode5 rejects UI kinds 0, 1 and 2");
    }
    check(mode5Probe(3,-1,0)[4*coronaAt+3]==0,"mode5 rejects an unmatched affine record");
    for(int nearer=0;nearer<3;++nearer) {
        auto out=mode5Probe(3,nearer,1);
        check(out[4*coronaAt+3]==0,"mode5 rejects nearer original, private UI or smoke depth");
    }
    // Mode 2 is the orbital-instance path. Its HC record remains tracked
    // when it is in front of or equal to the physical scene depth, while
    // ordinary text kinds must not claim the record. A zero mask is valid
    // orbital coverage, and smoke kind 3 is also valid on this path.
    auto mode2Probe=[&](int kind,int depthCase,int matched) {
        std::fill(z.begin(),z.end(),.000025f);
        std::fill(marks.begin(),marks.end(),0.0f);marks[4*16+4]=float(kind)/255.0f;
        if(depthCase==1) z[7*16+6]=0.0f;       // farther sky accepts the line
        if(depthCase==2) z[7*16+6]=.025f;     // nearer physical hull rejects it
        if(depthCase==3) z[7*16+6]=.0000125f; // finite farther surface accepts it
        reinterpret_cast<UINT*>(record)[15]=2;
        for(int i=16;i<28;++i) record[i]=1.0f;
        record[44]=record[49]=record[54]=1.0f;
        record[46]=.25f;record[50]=.03125f;
        record[56]=1.0f;record[59]=matched?1.0f:0.0f;
        record[32]=1.0f;record[37]=1.0f;record[42]=1.0f;record[43]=1.0f;
        ctx->UpdateSubresource(scene.Get(),0,nullptr,z.data(),16*4,0);
        ctx->UpdateSubresource(ui.Get(),0,nullptr,marks.data(),16*4,0);
        ctx->UpdateSubresource(rigidRecord.Get(),0,nullptr,record,0,0);
        floats("jit",.25f,-.375f,0,.5f);floats("probe",1,0,1,16);
        floats("holoJitter",.75f,.25f,1,1);ctx->UpdateSubresource(cb.Get(),0,nullptr,data.data(),0,0);
        ctx->ClearState();ctx->CSSetConstantBuffers(0,1,cb.GetAddressOf());
        ctx->CSSetShaderResources(2,1,zs.GetAddressOf());ctx->CSSetShaderResources(4,1,uiView.GetAddressOf());
        ctx->CSSetShaderResources(12,2,coronaInputs);
        ctx->CSSetUnorderedAccessViews(0,1,ru.GetAddressOf(),nullptr);
        ctx->CSSetShader(mode5.Get(),nullptr,0);ctx->Dispatch(1,1,1);
        auto out=read(dev,ctx,result.Get());ctx->ClearState();return out;
    };
    for(int kind : {0,3})
        check(mode2Probe(kind,0,1)[4*coronaAt+3]==1,"mode2 accepts equal depth, matched record and non-text coverage kind");
    check(mode2Probe(0,1,1)[4*coronaAt+3]==1,"mode2 accepts a matched orbital line in farther sky");
    check(mode2Probe(0,3,1)[4*coronaAt+3]==1,"mode2 accepts a line over a finite farther surface");
    for(int kind : {1,2})
        check(mode2Probe(kind,0,1)[4*coronaAt+3]==0,"mode2 rejects ordinary text coverage kinds");
    check(mode2Probe(0,2,1)[4*coronaAt+3]==0,"mode2 rejects a nearer physical occluder");
    check(mode2Probe(0,0,0)[4*coronaAt+3]==0,"mode2 rejects an unmatched orbital record");
    // The source and both shipping MV blobs must consume an accepted mode-2
    // record as tracked foreground, preserving its affine vector and depth.
    std::fill(prior.begin(),prior.end(),.025f);std::fill(z.begin(),z.end(),.000025f);
    std::fill(marks.begin(),marks.end(),0.0f);marks[4*16+4]=0.0f/255.0f;
    reinterpret_cast<UINT*>(record)[15]=2;record[56]=record[59]=1.0f;
    ctx->UpdateSubresource(previous.Get(),0,nullptr,prior.data(),8*4,0);
    ctx->UpdateSubresource(scene.Get(),0,nullptr,z.data(),16*4,0);
    ctx->UpdateSubresource(ui.Get(),0,nullptr,marks.data(),16*4,0);
    ctx->UpdateSubresource(rigidRecord.Get(),0,nullptr,record,0,0);
    floats("jit",.25f,-.375f,0,.5f);floats("probe",1,0,1,16);floats("holoJitter",.75f,.25f,1,1);
    ctx->UpdateSubresource(cb.Get(),0,nullptr,data.data(),0,0);
    ctx->ClearState();ctx->CSSetConstantBuffers(0,1,cb.GetAddressOf());
    ctx->CSSetShaderResources(2,1,zs.GetAddressOf());ctx->CSSetShaderResources(3,1,previousView.GetAddressOf());
    ctx->CSSetShaderResources(4,1,uiView.GetAddressOf());ctx->CSSetShaderResources(12,2,coronaInputs);
    ctx->CSSetSamplers(0,1,sampler.GetAddressOf());
    ID3D11UnorderedAccessView* orbitalOutputs[3]={mu.Get(),zu.Get(),ku.Get()};ctx->CSSetUnorderedAccessViews(3,3,orbitalOutputs,nullptr);
    for(int variant=0;variant<3;++variant) {
        ComPtr<ID3D11ComputeShader> tested=mv;
        if(variant)hr(dev->CreateComputeShader(embedded[variant-1].data,embedded[variant-1].size,nullptr,&tested));
        ctx->CSSetShader(tested.Get(),nullptr,0);ctx->Dispatch(1,1,1);
        auto orbitalMotion=read(dev,ctx,motion.Get()),orbitalDepth=read(dev,ctx,depth.Get());
        check(std::fabs(orbitalMotion[2*coronaAt]-2.75f)<1e-5f && std::fabs(orbitalMotion[2*coronaAt+1])<1e-5f,
              "mode2 tracked foreground keeps affine motion through source and shipping MV variants");
        check(std::fabs(orbitalDepth[coronaAt]-.000025f)<1e-6f,"mode2 tracked foreground preserves exact merged depth");
    }
    // The same accepted record remains finite when its current physical
    // scene depth is sky (the HC line is farther than no scene surface).
    // Exercise source and both embedded variants, since stale blobs must not
    // silently retain the old exact-depth rejection.
    std::fill(z.begin(),z.end(),0.0f);ctx->UpdateSubresource(scene.Get(),0,nullptr,z.data(),16*4,0);
    for(int variant=0;variant<3;++variant) {
        ComPtr<ID3D11ComputeShader> tested=mv;
        if(variant)hr(dev->CreateComputeShader(embedded[variant-1].data,embedded[variant-1].size,nullptr,&tested));
        ctx->ClearState();ctx->CSSetConstantBuffers(0,1,cb.GetAddressOf());
        ctx->CSSetShaderResources(2,1,zs.GetAddressOf());ctx->CSSetShaderResources(3,1,previousView.GetAddressOf());
        ctx->CSSetShaderResources(4,1,uiView.GetAddressOf());ctx->CSSetShaderResources(12,2,coronaInputs);
        ctx->CSSetSamplers(0,1,sampler.GetAddressOf());ctx->CSSetUnorderedAccessViews(3,3,orbitalOutputs,nullptr);
        ctx->CSSetShader(tested.Get(),nullptr,0);ctx->Dispatch(1,1,1);
        auto farMotion=read(dev,ctx,motion.Get()),farDepth=read(dev,ctx,depth.Get());
        check(std::isfinite(farMotion[2*coronaAt])&&std::isfinite(farMotion[2*coronaAt+1])&&
              std::fabs(farMotion[2*coronaAt]-2.75f)<1e-5f&&std::fabs(farMotion[2*coronaAt+1])<1e-5f,
              "mode2 source and shipping variants retain finite affine motion over sky");
        check(farDepth[coronaAt]==0.0f,"mode2 sky keeps physical ZC depth at zero");
    }
    // Orbital depth is coverage metadata, not a physical occluder in the
    // previous-depth history. A sky neighbour beside the current line must
    // keep its ordinary camera vector when ZP has no physical depth, while a
    // real hull depth in the same previous footprint still invalidates it.
    const unsigned orbitalNeighbour=4*8+3;
    for(int variant=0;variant<3;++variant) {
        ComPtr<ID3D11ComputeShader> tested=mv;
        if(variant)hr(dev->CreateComputeShader(embedded[variant-1].data,embedded[variant-1].size,nullptr,&tested));
        std::fill(prior.begin(),prior.end(),0.0f);std::fill(z.begin(),z.end(),0.0f);
        ctx->UpdateSubresource(previous.Get(),0,nullptr,prior.data(),8*4,0);
        ctx->UpdateSubresource(scene.Get(),0,nullptr,z.data(),16*4,0);
        ctx->ClearState();ctx->CSSetConstantBuffers(0,1,cb.GetAddressOf());
        ctx->CSSetShaderResources(2,1,zs.GetAddressOf());ctx->CSSetShaderResources(3,1,previousView.GetAddressOf());
        ctx->CSSetShaderResources(4,1,uiView.GetAddressOf());ctx->CSSetShaderResources(12,2,coronaInputs);
        ctx->CSSetSamplers(0,1,sampler.GetAddressOf());
        ctx->CSSetUnorderedAccessViews(3,3,orbitalOutputs,nullptr);ctx->CSSetShader(tested.Get(),nullptr,0);ctx->Dispatch(1,1,1);
        auto skyMotion=read(dev,ctx,motion.Get()),skyDepth=read(dev,ctx,depth.Get());
        check(std::fabs(skyMotion[2*orbitalNeighbour]-2.0f)<1e-5f && std::fabs(skyMotion[2*orbitalNeighbour+1])<1e-5f,
              "orbital coverage depth absent from ZP preserves the adjacent sky camera vector");
        check(skyDepth[coronaAt]==0.0f && skyDepth[orbitalNeighbour]==0.0f,
              "orbital HC depth does not enter the physical ZC copy");
        std::fill(prior.begin(),prior.end(),.025f);
        ctx->UpdateSubresource(previous.Get(),0,nullptr,prior.data(),8*4,0);
        ctx->CSSetShader(tested.Get(),nullptr,0);ctx->Dispatch(1,1,1);
        auto hullMotion=read(dev,ctx,motion.Get());
        check(std::fabs(hullMotion[2*orbitalNeighbour]-16.0f)<1e-5f && std::fabs(hullMotion[2*orbitalNeighbour+1]-16.0f)<1e-5f,
              "physical hull depth in ZP still invalidates adjacent background history");
    }
    // A valid corona record is tracked foreground.  Even when its previous
    // raster location contains a nearer hull, mv must keep the affine vector
    // instead of replacing it with background-hidden's size*2 sentinel.
    std::fill(prior.begin(),prior.end(),.025f);
    ctx->UpdateSubresource(previous.Get(),0,nullptr,prior.data(),8*4,0);
    reinterpret_cast<UINT*>(record)[15]=5;record[56]=record[59]=1.0f;
    ctx->UpdateSubresource(rigidRecord.Get(),0,nullptr,record,0,0);
    std::fill(marks.begin(),marks.end(),0.0f);marks[4*16+4]=3.0f/255.0f;
    ctx->UpdateSubresource(ui.Get(),0,nullptr,marks.data(),16*4,0);
    std::fill(z.begin(),z.end(),.000025f);ctx->UpdateSubresource(scene.Get(),0,nullptr,z.data(),16*4,0);
    std::fill(privateDepth.begin(),privateDepth.end(),0.0f);ctx->UpdateSubresource(privateUiDepth.Get(),0,nullptr,privateDepth.data(),16*4,0);
    std::fill(smoke.begin(),smoke.end(),0.0f);ctx->UpdateSubresource(smokeDepth.Get(),0,nullptr,smoke.data(),16*4,0);
    floats("jit",.25f,-.375f,0,.5f);floats("probe",1,0,1,16);floats("holoJitter",.75f,.25f,1,1);
    ctx->UpdateSubresource(cb.Get(),0,nullptr,data.data(),0,0);
    ctx->ClearState();ctx->CSSetConstantBuffers(0,1,cb.GetAddressOf());
    ctx->CSSetShaderResources(2,1,zs.GetAddressOf());ctx->CSSetShaderResources(3,1,previousView.GetAddressOf());
    ctx->CSSetShaderResources(4,1,uiView.GetAddressOf());ctx->CSSetShaderResources(6,1,smokeDepthView.GetAddressOf());
    ctx->CSSetShaderResources(7,1,privateUiDepthView.GetAddressOf());
    ctx->CSSetShaderResources(12,2,coronaInputs);ctx->CSSetSamplers(0,1,sampler.GetAddressOf());
    ID3D11UnorderedAccessView* coronaOutputs[3]={mu.Get(),zu.Get(),ku.Get()};ctx->CSSetUnorderedAccessViews(3,3,coronaOutputs,nullptr);
    for(int variant=0;variant<3;++variant) {
        ComPtr<ID3D11ComputeShader> tested=mv;
        if(variant)hr(dev->CreateComputeShader(embedded[variant-1].data,embedded[variant-1].size,nullptr,&tested));
        ctx->CSSetShader(tested.Get(),nullptr,0);ctx->Dispatch(1,1,1);
        auto coronaMotion=read(dev,ctx,motion.Get()),coronaDepth=read(dev,ctx,depth.Get());
        check(std::fabs(coronaMotion[2*coronaAt]-2.75f)<1e-5f && std::fabs(coronaMotion[2*coronaAt+1])<1e-5f,
              "mode5 tracked foreground skips background-hidden rejection with affine motion");
        check(std::fabs(coronaDepth[coronaAt]-.000025f)<1e-6f,"mode5 rejection preserves current merged depth");
    }
    // The absent-layer gate (zSceneAt: probe.w bit 64 = t6 not bound, 256 = t7 not bound, skipped rather than fetched
    // as the +0.0 a null view returns). Every shipping blob, mv and main, with t6 and/or t7 null, writes the same bytes
    // to every output it has (MV, ZC, MK, DT; O, N; Stats) with the bits clear -- the null views fetched, as before --
    // and set. The control: t7 BOUND with a panel nearer than the scene and the ship's radius, and bit 256 set anyway,
    // must change both entries' outputs: the blob honours the bit (one built before the gate would pass the parity
    // vacuously). Depth is bound and the world/ship split is on, so the merged depth decides the path and ZC.
    {
        auto colourTex=texture(16,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_BIND_SHADER_RESOURCE);
        auto historyTex=texture(8,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_BIND_SHADER_RESOURCE);
        auto outTex=texture(8,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_BIND_UNORDERED_ACCESS);
        auto nextTex=texture(8,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_BIND_UNORDERED_ACCESS);
        ComPtr<ID3D11ShaderResourceView> colourView,historyView;ComPtr<ID3D11UnorderedAccessView> outUav,nextUav;
        hr(dev->CreateShaderResourceView(colourTex.Get(),nullptr,&colourView));hr(dev->CreateShaderResourceView(historyTex.Get(),nullptr,&historyView));
        hr(dev->CreateUnorderedAccessView(outTex.Get(),nullptr,&outUav));hr(dev->CreateUnorderedAccessView(nextTex.Get(),nullptr,&nextUav));
        std::vector<float> colour(16*16*4),history(8*8*4);
        for(int i=0;i<16*16;++i){colour[4*i]=float(i%16)/15.f;colour[4*i+1]=float(i/16)/15.f;colour[4*i+2]=float((i*7)%16)/15.f;colour[4*i+3]=1;}
        for(int i=0;i<8*8;++i){history[4*i]=float(i%8)/7.f;history[4*i+1]=1-float(i/8)/7.f;history[4*i+2]=float((i*5)%8)/7.f;history[4*i+3]=1;}
        ctx->UpdateSubresource(colourTex.Get(),0,nullptr,colour.data(),16*16,0);ctx->UpdateSubresource(historyTex.Get(),0,nullptr,history.data(),8*16,0);
        D3D11_BUFFER_DESC sb{};sb.ByteWidth=128*4;sb.BindFlags=D3D11_BIND_UNORDERED_ACCESS;sb.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;sb.StructureByteStride=4;
        ComPtr<ID3D11Buffer> statsBuf;hr(dev->CreateBuffer(&sb,nullptr,&statsBuf));
        ComPtr<ID3D11UnorderedAccessView> statsUav;hr(dev->CreateUnorderedAccessView(statsBuf.Get(),nullptr,&statsUav));
        sb.ByteWidth=128*4;sb.BindFlags=0;sb.MiscFlags=0;sb.StructureByteStride=0;sb.Usage=D3D11_USAGE_STAGING;sb.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> statsStage;hr(dev->CreateBuffer(&sb,nullptr,&statsStage));
        // Sky on the left, a surface 5 m out (past the 1 m radius: the camera's path) on the right, a hull 0.5 m away
        // in one corner; the smoke a patch at 0.3 m; the UI depth the scene with a panel at 0.6 m stamped across rows 4-6.
        std::vector<float> sceneZ(16*16),smokeZ(16*16,0.f),uiZ(16*16),priorZ(8*8,.025f/5);
        for(int y=0;y<16;++y)for(int x=0;x<16;++x){int i=y*16+x;sceneZ[i]=x<7?0.f:.025f/5;if(x>10&&y>9)sceneZ[i]=.025f/.5f;
            if(x>4&&x<9&&y>5&&y<9)smokeZ[i]=.025f/.3f;uiZ[i]=(y>=7&&y<=9)?.025f/.6f:sceneZ[i];}
        ctx->UpdateSubresource(scene.Get(),0,nullptr,sceneZ.data(),16*4,0);
        ctx->UpdateSubresource(smokeDepth.Get(),0,nullptr,smokeZ.data(),16*4,0);
        ctx->UpdateSubresource(privateUiDepth.Get(),0,nullptr,uiZ.data(),16*4,0);
        ctx->UpdateSubresource(previous.Get(),0,nullptr,priorZ.data(),8*4,0);
        // The head turns one way, the camera the other, each with a translation; the box wide enough never to clip, so a
        // changed path shows in N and O. Every candidate of the registration instrument runs (diagnostic main).
        floats("tanNow",-1,1,-1,1);floats("tanPrev",-1,1,-1,1);floats("jit",.25f,-.375f,0,.5f);
        for(const char* r:{"dR","c0R","c1R","c3R"}){std::string n(r);floats((n+"0").c_str(),1,0,-.1f,0);floats((n+"1").c_str(),0,1,0,0);floats((n+"2").c_str(),.1f,0,1,0);}
        floats("c2R0",1,0,.15f,0);floats("c2R1",0,1,0,0);floats("c2R2",-.15f,0,1,0);
        floats("knobs",0,1,.025f,0);floats("tvUsed",.01f,0,0,1);floats("tvCand",-.01f,0,0,0);floats("tvCam",.02f,0,0,1);floats("split",1,0,0,0);
        floats("holoJitter",.25f,-.375f,1,1);
        const float blend=.9f,gamma=100.f;const int haveHistory=1,candMask=15;
        set("blend",&blend,4);set("gamma",&gamma,4);set("haveHistory",&haveHistory,4);set("candMask",&candMask,4);
        auto statsRead=[&](){ctx->CopyResource(statsStage.Get(),statsBuf.Get());D3D11_MAPPED_SUBRESOURCE m{};hr(ctx->Map(statsStage.Get(),0,D3D11_MAP_READ,0,&m));
            std::vector<float> v(128);std::memcpy(v.data(),m.pData,128*4);ctx->Unmap(statsStage.Get(),0);return v;};
        ComPtr<ID3D11ComputeShader> blobs[5];
        for(int variant=0;variant<5;++variant)hr(dev->CreateComputeShader(embedded[variant].data,embedded[variant].size,nullptr,&blobs[variant]));
        // Every output of one dispatch, concatenated: mv's MV, ZC, MK (and DT for the trace blob), main's O and N; then Stats.
        auto run=[&](int variant,bool t6,bool t7,UINT bits){
            const bool isMv=variant<3,traced=variant==2;
            floats("probe",1,0,0,float(bits));ctx->UpdateSubresource(cb.Get(),0,nullptr,data.data(),0,0);
            ID3D11ComputeShader* tested=blobs[variant].Get();
            const UINT zero[4]={};const float nothing[4]={};
            ctx->ClearUnorderedAccessViewUint(statsUav.Get(),zero);
            for(auto* u:{mu.Get(),zu.Get(),ku.Get(),du.Get(),outUav.Get(),nextUav.Get()})ctx->ClearUnorderedAccessViewFloat(u,nothing);
            ctx->ClearState();ctx->CSSetConstantBuffers(0,1,cb.GetAddressOf());
            ID3D11ShaderResourceView* in[8]={colourView.Get(),historyView.Get(),zs.Get(),previousView.Get(),nullptr,nullptr,
                                            t6?smokeDepthView.Get():nullptr,t7?privateUiDepthView.Get():nullptr};
            ctx->CSSetShaderResources(0,8,in);ctx->CSSetSamplers(0,1,sampler.GetAddressOf());
            ID3D11UnorderedAccessView* out[8]={isMv?nullptr:outUav.Get(),isMv?nullptr:nextUav.Get(),statsUav.Get(),isMv?mu.Get():nullptr,
                                               isMv?zu.Get():nullptr,isMv?ku.Get():nullptr,nullptr,traced?du.Get():nullptr};
            ctx->CSSetUnorderedAccessViews(0,8,out,nullptr);ctx->CSSetShader(tested,nullptr,0);ctx->Dispatch(1,1,1);
            std::vector<float> all;
            auto add=[&](const std::vector<float>& v){all.insert(all.end(),v.begin(),v.end());};
            if(isMv){add(read(dev,ctx,motion.Get()));add(read(dev,ctx,depth.Get()));add(read(dev,ctx,mask.Get()));if(traced)add(read(dev,ctx,decision.Get()));}
            else{add(read(dev,ctx,outTex.Get()));add(read(dev,ctx,nextTex.Get()));}
            add(statsRead());ctx->ClearState();return all;
        };
        auto same=[](const std::vector<float>& a,const std::vector<float>& b){return a.size()==b.size() && std::memcmp(a.data(),b.data(),a.size()*4)==0;};
        for(int variant=0;variant<5;++variant){
            for(int combo=0;combo<3;++combo){   // t6 and t7 null, t6 null, t7 null
                const bool t6=combo==2,t7=combo==1;
                const UINT absent=(t6?0u:64u)|(t7?0u:256u);
                const auto fetched=run(variant,t6,t7,0),skipped=run(variant,t6,t7,absent);
                if(!same(fetched,skipped))std::printf("absent-layer gate: %s, t6 %s, t7 %s: outputs differ\n",embedded[variant].name,t6?"bound":"null",t7?"bound":"null");
                check(same(fetched,skipped),"absent-layer gate: a shipping blob writes the same bytes to every output with an absent layer's bit clear (fetched) or set (skipped)");
            }
            // The control: t7 bound and nearer, bit 256 set anyway (t6 null, bit 64 as it should be).
            const auto honest=run(variant,false,true,64u),lie=run(variant,false,true,64u|256u),nothingBound=run(variant,false,false,64u|256u);
            check(!same(honest,lie),"absent-layer gate control: a shipping blob honours bit 256 (a bound, nearer UI depth skipped changes the outputs)");
            check(same(lie,nothingBound),"absent-layer gate control: bit 256 over a bound UI depth reads exactly as no UI depth");
        }
        // The same fetched-versus-skipped parity over the depth classes the max chain must carry bit for bit: -0, +0,
        // both quiet NaNs, the smallest and largest denormals and a negative one, -1 and -0.01, with ordinary depths
        // between, in Z and in whichever of ZS / ZUI is bound, the patterns staggered so the three layers' classes meet
        // in many pairings. Both arms run the same shipping blob (the null-view fetch against the literal +0.0 the bit
        // substitutes, through the same max instruction), so this proves skipping a layer equals fetching a null one for
        // those inputs. It does NOT prove the new max operand order matches the old shader's: that comparison, the
        // production zSceneAt against the expression it replaced, is the zscene gate harness
        // (tools\ui_depth_test\zscene_gate_test.h). WARP (this rig's device).
        {
            const uint32_t kClasses[]={0x80000000u,0x7FC00000u,0xFFC00000u,0x00000001u,0x007FFFFFu,0x80000001u,
                                       0xBF800000u,0xBC23D70Au,0x00000000u,0x3BA3D70Au,0x3D4CCCCDu};
            constexpr int kN=int(sizeof(kClasses)/sizeof(kClasses[0]));
            std::vector<uint32_t> sceneC(16*16),smokeC(16*16),uiC(16*16);
            for(int i=0;i<16*16;++i){const int row=i/16;
                sceneC[i]=kClasses[(i*7+row)%kN];smokeC[i]=kClasses[(i*5+3+row*2)%kN];uiC[i]=kClasses[(i*3+7+row*3)%kN];}
            ctx->UpdateSubresource(scene.Get(),0,nullptr,sceneC.data(),16*4,0);
            ctx->UpdateSubresource(smokeDepth.Get(),0,nullptr,smokeC.data(),16*4,0);
            ctx->UpdateSubresource(privateUiDepth.Get(),0,nullptr,uiC.data(),16*4,0);
            for(int variant=0;variant<5;++variant)
                for(int combo=0;combo<3;++combo){   // t6 and t7 null, t6 null (t7 bound), t7 null (t6 bound)
                    const bool t6=combo==2,t7=combo==1;
                    const UINT absent=(t6?0u:64u)|(t7?0u:256u);
                    const auto fetched=run(variant,t6,t7,0),skipped=run(variant,t6,t7,absent);
                    if(!same(fetched,skipped))std::printf("absent-layer gate, special depths: %s, t6 %s, t7 %s: outputs differ\n",embedded[variant].name,t6?"bound":"null",t7?"bound":"null");
                    check(same(fetched,skipped),"absent-layer gate: a shipping blob writes the same bytes with -0, NaN, denormal and negative depths in Z and the bound layer, bits clear or set");
                }
        }
        std::printf("PASS: absent-layer gate: the five shipping temporal blobs are byte-identical with t6/t7 null and the bits clear or set "
                    "(ordinary depths, and -0/NaN/denormal/negative in Z and the bound layer), and honour bit 256\n");
    }
    ctx->ClearState();
}
