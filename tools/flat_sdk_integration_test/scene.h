// Controlled draw state around immutable, hash-checked game shader bytecode.
// Included inside the bench translation unit's anonymous namespace.
struct SceneVertex { uint32_t a[4], b[4], c[4]; };
struct SceneEvidence {
    Snapshot world{}, alternate{}, writer{}, consumer{};
    unsigned depthPixels=0, poolPixels=0, hdrPixels=0;
    unsigned gpuWorldPixelsBeforeH=0, gpuForeignPixelsBeforeH=0;
    unsigned gpuWorldPixelsAtH=0, gpuForeignPixelsAtH=0;
    unsigned inertDepthPixels=0, inertStencilPixels=0, inertColorPixels=0;
    unsigned validDepthPixels=0;
    float depthMin=1.f, depthMax=0.f;
    Snapshot inert{};
};

bool readShader(const wchar_t* directory, const wchar_t* leaf, uint64_t expected,
                std::vector<BYTE>& bytes) {
    const std::wstring path=std::wstring(directory)+L"\\"+leaf;
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file==INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER length{};
    const bool sized=GetFileSizeEx(file,&length) && length.QuadPart>0 && length.QuadPart<65536;
    if (sized) bytes.resize(static_cast<size_t>(length.QuadPart));
    DWORD read=0;
    const bool loaded=sized && ReadFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr) && read==bytes.size();
    CloseHandle(file);
    if (!loaded) return false;
    uint64_t hash=1469598103934665603ull;
    for (BYTE b:bytes) { hash^=b; hash*=1099511628211ull; }
    return hash==expected;
}

bool makeSceneTexture(ID3D11Device* device, DXGI_FORMAT format, DXGI_FORMAT viewFormat,
                      UINT bind, ComPtr<ID3D11Texture2D>& texture,
                      ComPtr<ID3D11RenderTargetView>& view) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width=desc.Height=64;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
    desc.Format=format;desc.BindFlags=bind;
    if (!ok(device->CreateTexture2D(&desc,nullptr,&texture),"scene target texture")) return false;
    D3D11_RENDER_TARGET_VIEW_DESC vd{};
    vd.Format=viewFormat;vd.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
    return ok(device->CreateRenderTargetView(texture.Get(),&vd,&view),"scene target RTV");
}

std::vector<BYTE> readPixels(ID3D11Device* device, ID3D11DeviceContext* context,
                            ID3D11Texture2D* original, unsigned bytesPerPixel) {
    D3D11_TEXTURE2D_DESC desc{};original->GetDesc(&desc);
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stage;
    if (FAILED(device->CreateTexture2D(&desc,nullptr,&stage))) return {};
    context->CopyResource(stage.Get(),original);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(stage.Get(),0,D3D11_MAP_READ,0,&mapped))) return {};
    std::vector<BYTE> result(desc.Width*desc.Height*bytesPerPixel);
    for (unsigned y=0;y<desc.Height;++y) for (unsigned x=0;x<desc.Width;++x) {
        const BYTE* pixel=static_cast<const BYTE*>(mapped.pData)+y*mapped.RowPitch+x*bytesPerPixel;
        std::memcpy(result.data()+(y*desc.Width+x)*bytesPerPixel,pixel,bytesPerPixel);
    }
    context->Unmap(stage.Get(),0);
    return result;
}
unsigned changedPixels(const std::vector<BYTE>& before,const std::vector<BYTE>& after,unsigned pixelBytes) {
    if (before.empty() || before.size()!=after.size()) return 0;
    unsigned changed=0;
    for(size_t offset=0;offset<before.size();offset+=pixelBytes)
        if(std::memcmp(before.data()+offset,after.data()+offset,pixelBytes)!=0)++changed;
    return changed;
}

struct OwnerPixelCounts {unsigned world=0, foreign=0;};
OwnerPixelCounts readOwnerPixels(ID3D11Device* device, ID3D11DeviceContext* context,
                                 unsigned int (__cdecl* ownerView)(ID3D11ShaderResourceView**)) {
    OwnerPixelCounts counts{};
    ComPtr<ID3D11ShaderResourceView> view;
    if(!ownerView(view.GetAddressOf()) || !view)return counts;
    ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if(!resource || FAILED(resource.As(&texture)))return counts;
    const auto pixels=readPixels(device,context,texture.Get(),16);
    for(size_t offset=0;offset+16<=pixels.size();offset+=16) {
        float marker=0;std::memcpy(&marker,pixels.data()+offset,4);
        // Supported pool draws use the positive odd slot code (5 -> 11);
        // the foreign patch writes a negative odd owner code, and -1 is empty.
        if(std::isfinite(marker) && marker>.5f)++counts.world;
        else if(std::isfinite(marker) && marker<-1.5f)++counts.foreign;
    }
    return counts;
}

int scene(const wchar_t* proxyPath, const wchar_t* fixturePath, D3D_DRIVER_TYPE driver,
          const char* name) {
    HMODULE proxy=LoadLibraryExW(proxyPath,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!proxy) {std::fprintf(stderr,"flat SDK bench: proxy load failed %lu\n",GetLastError());return 2;}
    const auto snapshot=reinterpret_cast<SnapshotFn>(GetProcAddress(proxy,"edvr_selftest_flat_sdk_snapshot"));
    using OwnerViewFn=unsigned int (__cdecl*)(ID3D11ShaderResourceView**);
    const auto ownerView=reinterpret_cast<OwnerViewFn>(GetProcAddress(proxy,"edvr_selftest_flat_sdk_owner_view"));
    if (!snapshot || !ownerView) {std::fputs("flat SDK bench: snapshot or owner export absent\n",stderr);return 2;}
    edvr::openxr::PresentDevice present;
    if (!ok(present.initialize(proxy,driver),"hidden proxy device")) return 2;
    for(unsigned i=0;i<3;++i) if(!ok(present.present(),"arm flat frame"))return 2;
    Snapshot before{};
    if(!snapshot(&before,sizeof(before)) || !before.live || !before.owner) {
        std::fprintf(stderr,"flat SDK bench: scene frame not armed live=%u owner=%u mode=%s\n",before.live,before.owner,before.mode);
        return 2;
    }
    Snapshot incompatible{};
    incompatible.version=2;
    if(snapshot(&incompatible,sizeof(incompatible))) {
        std::fputs("flat SDK bench: snapshot accepted a future ABI version\n",stderr);return 2;
    }
    incompatible.version=1;incompatible.size=sizeof(incompatible)-1;
    if(snapshot(&incompatible,sizeof(incompatible))) {
        std::fputs("flat SDK bench: snapshot accepted a wrong ABI size\n",stderr);return 2;
    }
    auto* device=present.device();auto* context=present.context();
    const struct ShaderName {const wchar_t* name;uint64_t hash;} names[]={
        {L"vs_EB5234DB6ADB491D.dxbc",0xEB5234DB6ADB491Dull},
        {L"ps_DC603C35BBE74B31.dxbc",0xDC603C35BBE74B31ull},
        {L"vs_AACFDCF2FB9AD809.dxbc",0xAACFDCF2FB9AD809ull},
        {L"ps_CF534B32F491561A.dxbc",0xCF534B32F491561Aull}};
    std::vector<BYTE> code[4];
    for(unsigned i=0;i<4;++i) if(!readShader(fixturePath,names[i].name,names[i].hash,code[i])) {
        std::fwprintf(stderr,L"flat SDK bench: fixture missing or hash mismatch: %ls\n",names[i].name);return 2;
    }
    ComPtr<ID3D11VertexShader> vs[2];ComPtr<ID3D11PixelShader> ps[2];
    for(unsigned i=0;i<2;++i) {
        if(!ok(device->CreateVertexShader(code[2*i].data(),code[2*i].size(),nullptr,&vs[i]),"captured VS") ||
           !ok(device->CreatePixelShader(code[2*i+1].data(),code[2*i+1].size(),nullptr,&ps[i]),"captured PS"))return 2;
    }
    const D3D11_INPUT_ELEMENT_DESC inputs[]={
        {"INSTANCEANDMODELDATAINDEX",0,DXGI_FORMAT_R32G32_UINT,0,0,D3D11_INPUT_PER_INSTANCE_DATA,1},
        {"PACKEDVERTEXDATAA",0,DXGI_FORMAT_R32G32B32A32_UINT,1,0,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"PACKEDVERTEXDATAB",0,DXGI_FORMAT_R32G32B32A32_UINT,1,16,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"PACKEDVERTEXDATAC",0,DXGI_FORMAT_R32G32B32A32_UINT,1,32,D3D11_INPUT_PER_VERTEX_DATA,0}};
    ComPtr<ID3D11InputLayout> layout[2];
    for(unsigned i=0;i<2;++i) if(!ok(device->CreateInputLayout(inputs,4,code[2*i].data(),code[2*i].size(),&layout[i]),"captured VS layout"))return 2;
    ComPtr<ID3D11Texture2D> pool,hdr,consumerColor;
    ComPtr<ID3D11RenderTargetView> poolRtv,hdrRtv,consumerRtv;
    if(!makeSceneTexture(device,DXGI_FORMAT_R10G10B10A2_TYPELESS,DXGI_FORMAT_R10G10B10A2_UNORM,
        D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE,pool,poolRtv) ||
       !makeSceneTexture(device,DXGI_FORMAT_R11G11B10_FLOAT,DXGI_FORMAT_R11G11B10_FLOAT,
        D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE,hdr,hdrRtv) ||
       !makeSceneTexture(device,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM,
        D3D11_BIND_RENDER_TARGET,consumerColor,consumerRtv))return 2;
    D3D11_TEXTURE2D_DESC depthDesc{};depthDesc.Width=depthDesc.Height=64;
    depthDesc.MipLevels=depthDesc.ArraySize=depthDesc.SampleDesc.Count=1;
    depthDesc.Format=DXGI_FORMAT_R32G8X24_TYPELESS;
    depthDesc.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> depth;ComPtr<ID3D11DepthStencilView> dsv;
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};dsvDesc.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    dsvDesc.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
    if(!ok(device->CreateTexture2D(&depthDesc,nullptr,&depth),"scene depth") ||
       !ok(device->CreateDepthStencilView(depth.Get(),&dsvDesc,&dsv),"scene depth view"))return 2;
    D3D11_SHADER_RESOURCE_VIEW_DESC hSrvDesc{};hSrvDesc.Format=DXGI_FORMAT_R11G11B10_FLOAT;
    hSrvDesc.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;hSrvDesc.Texture2D.MipLevels=1;
    ComPtr<ID3D11ShaderResourceView> hSrv;
    if(!ok(device->CreateShaderResourceView(hdr.Get(),&hSrvDesc,&hSrv),"H SRV"))return 2;
    D3D11_DEPTH_STENCIL_DESC depthStateDesc{};depthStateDesc.DepthEnable=TRUE;
    depthStateDesc.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;depthStateDesc.DepthFunc=D3D11_COMPARISON_ALWAYS;
    ComPtr<ID3D11DepthStencilState> depthState;
    if(!ok(device->CreateDepthStencilState(&depthStateDesc,&depthState),"scene depth state"))return 2;
    D3D11_RASTERIZER_DESC rasterDesc{};rasterDesc.FillMode=D3D11_FILL_SOLID;
    rasterDesc.CullMode=D3D11_CULL_NONE;rasterDesc.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster;
    if(!ok(device->CreateRasterizerState(&rasterDesc,&raster),"scene raster state"))return 2;
    const auto packed=[](int x,int y) {const uint32_t px=uint32_t((x+1000)*65535/2000);
        const uint32_t py=uint32_t((y+1000)*65535/2000);return px|(py<<16);};
    const SceneVertex vertices[3]={
        {{packed(-800,-800),4096u<<16|32768u,64u<<24,0},{},{}} ,
        {{packed(800,-800),4096u<<16|32768u,64u<<24,0},{},{}} ,
        {{packed(0,800),4096u<<16|32768u,64u<<24,0},{},{}} };
    const uint32_t instance[2][2]={{5,0},{9,0}};const uint16_t indices[3]={0,1,2};
    ComPtr<ID3D11Buffer> verticesBuffer,instanceBuffer[2],indexBuffer;
    auto createDataBuffer=[&](const void* data,UINT size,UINT bind,ComPtr<ID3D11Buffer>& out) {
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=size;bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=bind;
        D3D11_SUBRESOURCE_DATA init{data,0,0};return ok(device->CreateBuffer(&bd,&init,&out),"scene geometry buffer");};
    if(!createDataBuffer(vertices,sizeof(vertices),D3D11_BIND_VERTEX_BUFFER,verticesBuffer) ||
       !createDataBuffer(instance[0],sizeof(instance[0]),D3D11_BIND_VERTEX_BUFFER,instanceBuffer[0]) ||
       !createDataBuffer(instance[1],sizeof(instance[1]),D3D11_BIND_VERTEX_BUFFER,instanceBuffer[1]) ||
       !createDataBuffer(indices,sizeof(indices),D3D11_BIND_INDEX_BUFFER,indexBuffer))return 2;
    struct PoolRecord {uint32_t words[84];};
    PoolRecord records[16]{};
    auto lane=[](float c){return static_cast<uint32_t>((c+1.f)*32767.f);};
    float scale=1.f, forward=2.f;std::memcpy(&records[5].words[1],&scale,4);
    std::memcpy(&records[5].words[6],&forward,4);
    records[5].words[2]=lane(0.f)|(lane(0.f)<<16);
    records[5].words[3]=lane(0.f)|(lane(1.f)<<16);
    records[9]=records[5];
    float alternateX=.65f;std::memcpy(&records[9].words[4],&alternateX,4);
    uint32_t t38Data[12*16]{};
    auto makeStructured=[&](const void* data,UINT stride,UINT count,ComPtr<ID3D11ShaderResourceView>& srv) {
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=stride*count;bd.Usage=D3D11_USAGE_DEFAULT;
        bd.BindFlags=D3D11_BIND_SHADER_RESOURCE;bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride=stride;D3D11_SUBRESOURCE_DATA init{data,0,0};
        ComPtr<ID3D11Buffer> buffer;
        return ok(device->CreateBuffer(&bd,&init,&buffer),"scene structured buffer") &&
            ok(device->CreateShaderResourceView(buffer.Get(),nullptr,&srv),"scene structured SRV");};
    ComPtr<ID3D11ShaderResourceView> poolSrv,t38Srv;
    if(!makeStructured(records,336,16,poolSrv) || !makeStructured(t38Data,48,16,t38Srv))return 2;
    // Captured PS bindings beyond b1 use benign reconstructed resources.
    uint32_t structured160[40*16]{};uint32_t structured96[24*16]{};
    ComPtr<ID3D11ShaderResourceView> psStructured160,psStructured96;
    if(!makeStructured(structured160,160,16,psStructured160) ||
       !makeStructured(structured96,96,16,psStructured96))return 2;
    D3D11_TEXTURE2D_DESC sampleDesc{};sampleDesc.Width=sampleDesc.Height=4;
    sampleDesc.MipLevels=1;sampleDesc.ArraySize=4;sampleDesc.SampleDesc.Count=1;
    sampleDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;sampleDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> samples;ComPtr<ID3D11ShaderResourceView> arraySrv,textureSrv;
    if(!ok(device->CreateTexture2D(&sampleDesc,nullptr,&samples),"scene sample textures") ||
       !ok(device->CreateShaderResourceView(samples.Get(),nullptr,&arraySrv),"scene array SRV"))return 2;
    sampleDesc.ArraySize=1;ComPtr<ID3D11Texture2D> texture2d;
    if(!ok(device->CreateTexture2D(&sampleDesc,nullptr,&texture2d),"scene sample texture") ||
       !ok(device->CreateShaderResourceView(texture2d.Get(),nullptr,&textureSrv),"scene texture SRV"))return 2;
    D3D11_SAMPLER_DESC samplerDesc{};samplerDesc.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU=samplerDesc.AddressV=samplerDesc.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.MaxLOD=D3D11_FLOAT32_MAX;ComPtr<ID3D11SamplerState> sampler;
    if(!ok(device->CreateSamplerState(&samplerDesc,&sampler),"scene sampler"))return 2;
    float material[6][4]{};material[0][0]=-1000.f;
    ComPtr<ID3D11Buffer> materialBuffer;
    if(!createDataBuffer(material,sizeof(material),D3D11_BIND_CONSTANT_BUFFER,materialBuffer))return 2;
    ComPtr<ID3D11Buffer> camera[2];
    for(unsigned c=0;c<2;++c) {
        float rows[280][4]{};rows[270][0]=1;rows[271][1]=1;rows[272][3]=1;
        rows[273][2]=c?.0675f:.025f;rows[274][2]=1;
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(rows);bd.Usage=D3D11_USAGE_DYNAMIC;
        bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        if(!ok(device->CreateBuffer(&bd,nullptr,&camera[c]),"scene camera"))return 2;
        ID3D11Buffer* bound=camera[c].Get();context->VSSetConstantBuffers(1,1,&bound);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if(!ok(context->Map(camera[c].Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"scene camera upload"))return 2;
        std::memcpy(mapped.pData,rows,sizeof(rows));context->Unmap(camera[c].Get(),0);
    }
    const D3D11_VIEWPORT vp{0,0,64,64,0,1};context->RSSetViewports(1,&vp);
    context->RSSetState(raster.Get());context->OMSetDepthStencilState(depthState.Get(),0);
    context->OMSetBlendState(nullptr,nullptr,0xffffffffu);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer* vb[2]={instanceBuffer[0].Get(),verticesBuffer.Get()};
    const UINT strides[2]={sizeof(instance[0]),sizeof(SceneVertex)},offsets[2]={0,0};
    context->IASetVertexBuffers(0,2,vb,strides,offsets);
    context->IASetIndexBuffer(indexBuffer.Get(),DXGI_FORMAT_R16_UINT,0);
    ID3D11ShaderResourceView* vsRes=poolSrv.Get();context->VSSetShaderResources(33,1,&vsRes);
    vsRes=t38Srv.Get();context->VSSetShaderResources(38,1,&vsRes);
    ID3D11Buffer* materialPtr=materialBuffer.Get();context->VSSetConstantBuffers(2,1,&materialPtr);
    context->PSSetConstantBuffers(2,1,&materialPtr);
    ID3D11SamplerState* samplePtr=sampler.Get();context->PSSetSamplers(0,1,&samplePtr);
    const float clearPool[4]={.1f,.2f,.3f,1.f};
    const float clearHdr[4]={.1f,.2f,.3f,1.f};
    context->ClearRenderTargetView(poolRtv.Get(),clearPool);
    context->ClearRenderTargetView(hdrRtv.Get(),clearHdr);
    context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,1,0);
    SceneEvidence evidence;
    auto originalDraw=[&](unsigned family,ID3D11RenderTargetView* target,Snapshot& snap) {
        ID3D11RenderTargetView* view=target;context->OMSetRenderTargets(1,&view,dsv.Get());
        ID3D11Buffer* cb=camera[family].Get();context->VSSetConstantBuffers(1,1,&cb);
        context->PSSetConstantBuffers(1,1,&cb);
        context->IASetInputLayout(layout[family].Get());
        vb[0]=instanceBuffer[family].Get();context->IASetVertexBuffers(0,2,vb,strides,offsets);
        context->IASetIndexBuffer(indexBuffer.Get(),DXGI_FORMAT_R16_UINT,0);
        context->VSSetShader(vs[family].Get(),nullptr,0);context->PSSetShader(ps[family].Get(),nullptr,0);
        ID3D11ShaderResourceView* resources[7]{};
        if(family==0) {resources[0]=psStructured160.Get();resources[1]=textureSrv.Get();resources[2]=psStructured96.Get();
            for(unsigned i=3;i<=6;++i)resources[i]=arraySrv.Get();}
        else {resources[0]=textureSrv.Get();resources[1]=psStructured96.Get();
            for(unsigned i=2;i<=5;++i)resources[i]=arraySrv.Get();}
        context->PSSetShaderResources(0,7,resources);
        context->DrawIndexedInstanced(3,1,0,0,0);
        return snapshot(&snap,sizeof(snap))!=0;
    };
    // The engine source has a previous-frame scene CB and slot plane by
    // contract. Produce one ordinary world frame before the measured frame.
    Snapshot warmup{};
    for(unsigned frame=0;frame<2;++frame) {
        if(!originalDraw(0,poolRtv.Get(),warmup) || !ok(present.present(),"source warmup Present"))return 2;
        for(unsigned c=0;c<2;++c) {
            float rows[280][4]{};rows[270][0]=1;rows[271][1]=1;rows[272][3]=1;
            rows[273][2]=c?.0675f:.025f;rows[274][2]=1;
            ID3D11Buffer* bound=camera[c].Get();context->VSSetConstantBuffers(1,1,&bound);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if(!ok(context->Map(camera[c].Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"next-frame camera upload"))return 2;
            std::memcpy(mapped.pData,rows,sizeof(rows));context->Unmap(camera[c].Get(),0);
        }
        context->ClearRenderTargetView(poolRtv.Get(),clearPool);
        context->ClearRenderTargetView(hdrRtv.Get(),clearHdr);
        context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,1,0);
    }
    if(!snapshot(&before,sizeof(before)) || !before.live)return 2;
    const auto poolBefore=readPixels(device,context,pool.Get(),4);
    const auto hdrBefore=readPixels(device,context,hdr.Get(),4);
    const auto depthBefore=readPixels(device,context,depth.Get(),8);
    if(!originalDraw(0,poolRtv.Get(),evidence.world) ||
       !originalDraw(1,poolRtv.Get(),evidence.alternate))return 2;
    const auto ownerBeforeH=readOwnerPixels(device,context,ownerView);
    evidence.gpuWorldPixelsBeforeH=ownerBeforeH.world;
    evidence.gpuForeignPixelsBeforeH=ownerBeforeH.foreign;
    const bool inert=std::strncmp(name,"inert_",6)==0;
    if(inert) {
        std::vector<BYTE> inertVsCode,inertPsCode;
        if(!readShader(fixturePath,L"vs_FC1193AFFC596F74.dxbc",0xFC1193AFFC596F74ull,inertVsCode) ||
           !readShader(fixturePath,L"ps_258B95AC99520C1F.dxbc",0x258B95AC99520C1Full,inertPsCode))return 2;
        ComPtr<ID3D11VertexShader> inertVs;ComPtr<ID3D11PixelShader> inertPs;
        if(!ok(device->CreateVertexShader(inertVsCode.data(),inertVsCode.size(),nullptr,&inertVs),"captured inert VS") ||
           !ok(device->CreatePixelShader(inertPsCode.data(),inertPsCode.size(),nullptr,&inertPs),"captured inert PS"))return 2;
        const float clipVertices[3][8]={{-1,-1,.5f,1},{-1,3,.5f,1},{3,-1,.5f,1}};
        ComPtr<ID3D11Buffer> clipBuffer;
        if(!createDataBuffer(clipVertices,sizeof(clipVertices),D3D11_BIND_VERTEX_BUFFER,clipBuffer))return 2;
        ComPtr<ID3D11InputLayout> clipLayout;
        ComPtr<ID3D11ShaderReflection> reflected;
        if(!ok(D3DReflect(inertVsCode.data(),inertVsCode.size(),IID_PPV_ARGS(&reflected)),"inert VS reflection"))return 2;
        D3D11_SHADER_DESC shaderDesc{};reflected->GetDesc(&shaderDesc);
        if(shaderDesc.InputParameters!=2) {
            std::fprintf(stderr,"flat SDK bench: inert VS inputs=%u\n",shaderDesc.InputParameters);
            for(unsigned i=0;i<shaderDesc.InputParameters;++i) {
                D3D11_SIGNATURE_PARAMETER_DESC input{};reflected->GetInputParameterDesc(i,&input);
                std::fprintf(stderr,"flat SDK bench: input%u semantic=%s%u mask=%u system=%u\n",i,
                    input.SemanticName,input.SemanticIndex,input.Mask,input.SystemValueType);
            }
            return 2;
        }
        D3D11_SIGNATURE_PARAMETER_DESC signature[2]{};
        reflected->GetInputParameterDesc(0,&signature[0]);
        reflected->GetInputParameterDesc(1,&signature[1]);
        const D3D11_INPUT_ELEMENT_DESC clipInput[2]={
            {signature[0].SemanticName,signature[0].SemanticIndex,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
            {signature[1].SemanticName,signature[1].SemanticIndex,DXGI_FORMAT_R32G32B32A32_FLOAT,0,16,D3D11_INPUT_PER_VERTEX_DATA,0}};
        if(!ok(device->CreateInputLayout(clipInput,2,inertVsCode.data(),inertVsCode.size(),&clipLayout),"inert input layout")) {
            std::fprintf(stderr,"flat SDK bench: inert VS semantics=%s%u/%s%u\n",
                signature[0].SemanticName,signature[0].SemanticIndex,signature[1].SemanticName,signature[1].SemanticIndex);
            return 2;
        }
        D3D11_DEPTH_STENCIL_DESC inertDepth{};
        const bool stencilOnly=std::strcmp(name,"inert_no_write")==0;
        const bool depthWrite=std::strcmp(name,"inert_depth_write")==0;
        if(stencilOnly) {
            inertDepth.StencilEnable=TRUE;inertDepth.StencilReadMask=0xff;inertDepth.StencilWriteMask=0xff;
            inertDepth.FrontFace.StencilFunc=D3D11_COMPARISON_ALWAYS;
            inertDepth.FrontFace.StencilPassOp=D3D11_STENCIL_OP_REPLACE;
            inertDepth.FrontFace.StencilFailOp=D3D11_STENCIL_OP_KEEP;
            inertDepth.FrontFace.StencilDepthFailOp=D3D11_STENCIL_OP_KEEP;
            inertDepth.BackFace=inertDepth.FrontFace;
        }
        if(depthWrite) {inertDepth.DepthEnable=TRUE;inertDepth.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
            inertDepth.DepthFunc=D3D11_COMPARISON_ALWAYS;}
        ComPtr<ID3D11DepthStencilState> inertDepthState;
        if(!ok(device->CreateDepthStencilState(&inertDepth,&inertDepthState),"inert depth state"))return 2;
        D3D11_BLEND_DESC noColor{};
        noColor.RenderTarget[0].RenderTargetWriteMask=0;
        ComPtr<ID3D11BlendState> noColorState;
        if(!ok(device->CreateBlendState(&noColor,&noColorState),"inert color mask"))return 2;
        auto poolAtInert=readPixels(device,context,pool.Get(),4);
        auto depthAtInert=readPixels(device,context,depth.Get(),8);
        ID3D11RenderTargetView* target=poolRtv.Get();context->OMSetRenderTargets(1,&target,dsv.Get());
        context->OMSetDepthStencilState(inertDepthState.Get(),stencilOnly?8:0);
        context->OMSetBlendState(stencilOnly||depthWrite?noColorState.Get():nullptr,nullptr,0xffffffffu);
        context->IASetInputLayout(clipLayout.Get());
        ID3D11Buffer* clip=clipBuffer.Get();const UINT clipStride=sizeof(clipVertices[0]),zero=0;
        context->IASetVertexBuffers(0,1,&clip,&clipStride,&zero);
        context->VSSetShader(inertVs.Get(),nullptr,0);context->PSSetShader(inertPs.Get(),nullptr,0);
        context->Draw(3,0);
        if(!snapshot(&evidence.inert,sizeof(evidence.inert)))return 2;
        const auto poolAfterInert=readPixels(device,context,pool.Get(),4);
        const auto depthAfterInert=readPixels(device,context,depth.Get(),8);
        evidence.inertColorPixels=changedPixels(poolAtInert,poolAfterInert,4);
        if(depthAtInert.size()==depthAfterInert.size())
            for(size_t offset=0;offset<depthAtInert.size();offset+=8) {
                if(std::memcmp(depthAtInert.data()+offset,depthAfterInert.data()+offset,4)!=0)++evidence.inertDepthPixels;
                if(std::memcmp(depthAtInert.data()+offset+4,depthAfterInert.data()+offset+4,4)!=0)++evidence.inertStencilPixels;
            }
        context->OMSetDepthStencilState(depthState.Get(),0);
        context->OMSetBlendState(nullptr,nullptr,0xffffffffu);
    }
    if(!originalDraw(0,hdrRtv.Get(),evidence.writer))return 2;
    const auto ownerAtH=readOwnerPixels(device,context,ownerView);
    evidence.gpuWorldPixelsAtH=ownerAtH.world;
    evidence.gpuForeignPixelsAtH=ownerAtH.foreign;
    ID3D11RenderTargetView* consumerTarget=consumerRtv.Get();
    context->OMSetRenderTargets(1,&consumerTarget,nullptr);
    ID3D11ShaderResourceView* hView=hSrv.Get();context->PSSetShaderResources(0,1,&hView);
    constexpr char consumerVs[]="float4 main(uint id:SV_VertexID):SV_Position{float2 p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};return float4(p[id],.5,1);}";
    constexpr char consumerPs[]="Texture2D<float4> h:register(t0);float4 main(float4 pos:SV_Position):SV_Target{return h.Load(int3(pos.xy,0));}";
    ComPtr<ID3DBlob> consumerVsCode,consumerPsCode,errors;
    if(!ok(D3DCompile(consumerVs,sizeof(consumerVs)-1,nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&consumerVsCode,&errors),"consumer VS compile") ||
       !ok(D3DCompile(consumerPs,sizeof(consumerPs)-1,nullptr,nullptr,nullptr,"main","ps_5_0",0,0,&consumerPsCode,&errors),"consumer PS compile"))return 2;
    ComPtr<ID3D11VertexShader> consumerVertex;ComPtr<ID3D11PixelShader> consumerPixel;
    if(!ok(device->CreateVertexShader(consumerVsCode->GetBufferPointer(),consumerVsCode->GetBufferSize(),nullptr,&consumerVertex),"consumer VS") ||
       !ok(device->CreatePixelShader(consumerPsCode->GetBufferPointer(),consumerPsCode->GetBufferSize(),nullptr,&consumerPixel),"consumer PS"))return 2;
    context->IASetInputLayout(nullptr);context->VSSetShader(consumerVertex.Get(),nullptr,0);
    context->PSSetShader(consumerPixel.Get(),nullptr,0);context->Draw(3,0);
    if(!snapshot(&evidence.consumer,sizeof(evidence.consumer)))return 2;
    context->OMSetRenderTargets(0,nullptr,nullptr);
    const auto depthAfter=readPixels(device,context,depth.Get(),8);
    evidence.depthPixels=changedPixels(depthBefore,depthAfter,8);
    if(depthBefore.size()==depthAfter.size()) for(size_t offset=0;offset<depthBefore.size();offset+=8) {
        if(std::memcmp(depthBefore.data()+offset,depthAfter.data()+offset,4)==0)continue;
        float z=0;std::memcpy(&z,depthAfter.data()+offset,4);
        if(std::isfinite(z) && z>=0.f && z<=1.f) {
            ++evidence.validDepthPixels;
            if(z<evidence.depthMin)evidence.depthMin=z;
            if(z>evidence.depthMax)evidence.depthMax=z;
        }
    }
    evidence.poolPixels=changedPixels(poolBefore,readPixels(device,context,pool.Get(),4),4);
    evidence.hdrPixels=changedPixels(hdrBefore,readPixels(device,context,hdr.Get(),4),4);
    const auto& after=evidence.consumer;
    const bool taa=std::strcmp(after.mode,"on")==0;
    const bool hostGuard=std::strcmp(name,"unsupported_host")==0;
    const bool depthGuard=std::strcmp(name,"inert_depth_write")==0;
    const bool colorGuard=std::strcmp(name,"inert_color_write")==0;
    const bool guardCase=hostGuard||depthGuard||colorGuard;
    const bool inertNoWrite=std::strcmp(name,"inert_no_write")==0;
    const bool rasterReady=evidence.validDepthPixels>0 && evidence.poolPixels>0 && evidence.hdrPixels>0;
    const bool ownership=evidence.world.namedWorld && evidence.alternate.foreignSeen &&
        evidence.alternate.captured && evidence.alternate.gpuIdentitySubmitted &&
        evidence.gpuWorldPixelsAtH>0 && evidence.gpuForeignPixelsAtH>0;
    const bool route=after.hdrTriggered && after.hdrSelected && after.resolverCalls;
    const bool inertNoWriteMeasured=!inertNoWrite ||
        (evidence.inertStencilPixels>0 && !evidence.inertColorPixels && !evidence.inertDepthPixels &&
         !evidence.inert.firstFailureFrame);
    const bool upstream=rasterReady && evidence.world.namedWorld && route && inertNoWriteMeasured &&
        (taa || (ownership && after.hQualified>0));
    const bool inertWriterRefusal=inert && after.hAttempts>0 && !after.hQualified &&
        evidence.inert.firstFailureVs==0xFC1193AFFC596F74ull &&
        evidence.inert.firstFailurePs==0x258B95AC99520C1Full &&
        std::strcmp(evidence.inert.firstFailureStage,"inert-state")==0 &&
        std::strcmp(evidence.inert.firstFailureReason,depthGuard?
            "foreground-inert-depth-write":"foreground-inert-color-write")==0;
    const bool guardConfirmed=hostGuard?
        rasterReady && evidence.world.namedWorld && (taa || ownership) && after.hdrTriggered && !after.hAttempts &&
            std::strcmp(after.hdrVerdict,"engine-source-not-ready")==0:
        depthGuard?evidence.inertDepthPixels>0 && !evidence.inertColorPixels && inertWriterRefusal:
        colorGuard?evidence.inertColorPixels>0 && !evidence.inertDepthPixels && inertWriterRefusal:false;
    const char* verdict="FAIL";
    const char* cause="upstream-qualification-or-raster-refused";
    if(guardCase) {
        verdict=guardConfirmed?"PASS":"FAIL";
        cause=guardConfirmed?hostGuard?"host-guard-confirmed":depthGuard?"depth-write-refusal-confirmed":
            "color-write-refusal-confirmed":"guard-not-confirmed";
    } else if(upstream && after.hdrResolves && (taa || after.hdrBackendCompleted)) {
        verdict="PASS";cause="production-hdr-resolve-completed";
    } else if(upstream && !taa && driver==D3D_DRIVER_TYPE_WARP &&
              (std::strcmp(after.mode,"dlaa")==0 || std::strcmp(after.mode,"dlss")==0) &&
              after.backendFailures && !after.hdrBackendCompleted) {
        verdict="UNSUPPORTED";cause="configured-backend-unavailable-on-adapter";
    }
    const auto quote=[](const char* value) {
        std::string escaped="\"";
        for(const unsigned char* p=reinterpret_cast<const unsigned char*>(value?value:"");*p;++p) {
            if(*p=='"'||*p=='\\')escaped+='\\';
            if(*p>=32)escaped+=static_cast<char>(*p);
        }
        return escaped+'"';
    };
    const auto failure=[&](const Snapshot& s) {
        std::ostringstream f;
        f<<"{\"frame\":"<<s.firstFailureFrame<<",\"q\":"<<s.firstFailureSequence
         <<",\"vs\":\""<<std::hex<<std::uppercase<<std::setw(16)<<std::setfill('0')<<s.firstFailureVs
         <<"\",\"ps\":\""<<std::setw(16)<<s.firstFailurePs<<std::dec
         <<"\",\"stage\":"<<quote(s.firstFailureStage)<<",\"reason\":"<<quote(s.firstFailureReason)
         <<",\"selectedH\":"<<s.firstFailureSelectedH<<"}";
        return f.str();
    };
    std::ostringstream result;
    result<<"EDVR_BENCH_RESULT {\"schema\":\"edvr-flat-sdk-bench\",\"version\":1,\"case\":"<<quote(name)
          <<",\"mode\":"<<quote(taa?"taa":after.mode)
          <<",\"purpose\":"<<quote(guardCase?"guard":"renderer")
          <<",\"verdict\":"<<quote(verdict)<<",\"cause\":"<<quote(cause)
          <<",\"observed\":{\"configuredMode\":"<<quote(after.mode)
          <<",\"proxyFlavor\":"<<quote(hostGuard?"shipping":"reconstructed-emit")
          <<",\"profileFlat\":"<<after.flatProfile
          <<",\"scope\":{\"frame\":"<<after.frame<<",\"drawBefore\":"<<before.drawSequence
          <<",\"drawAfter\":"<<after.drawSequence<<",\"work\":"<<after.work<<"}"
          <<",\"rasterPixels\":"<<evidence.validDepthPixels
          <<",\"raster\":{\"depthPixels\":"<<evidence.depthPixels<<",\"validDepthPixels\":"<<evidence.validDepthPixels
          <<",\"depthMin\":"<<evidence.depthMin<<",\"depthMax\":"<<evidence.depthMax
          <<",\"poolPixels\":"<<evidence.poolPixels<<",\"hdrPixels\":"<<evidence.hdrPixels<<"}"
          <<",\"namedWorld\":"<<evidence.world.namedWorld<<",\"candidates\":"<<after.candidates
          <<",\"owner\":{\"foreignSeen\":"<<evidence.alternate.foreignSeen
          <<",\"captures\":"<<evidence.alternate.captured
          <<",\"gpuIdentitySubmitted\":"<<evidence.alternate.gpuIdentitySubmitted
          <<",\"worldMarkers\":"<<after.worldMarkers
          <<",\"gpuWorldPixelsBeforeH\":"<<evidence.gpuWorldPixelsBeforeH
          <<",\"gpuForeignPixelsBeforeH\":"<<evidence.gpuForeignPixelsBeforeH
          <<",\"gpuWorldPixelsAtH\":"<<evidence.gpuWorldPixelsAtH
          <<",\"gpuForeignPixelsAtH\":"<<evidence.gpuForeignPixelsAtH<<"}"
          <<",\"inert\":{\"drawn\":"<<(inert?"true":"false")
          <<",\"colorPixels\":"<<evidence.inertColorPixels
          <<",\"depthPixels\":"<<evidence.inertDepthPixels
          <<",\"stencilPixels\":"<<evidence.inertStencilPixels
          <<",\"firstFailure\":"<<failure(evidence.inert)<<"}"
          <<",\"firstFailure\":"<<failure(after)
          <<",\"lastH\":{\"attempts\":"<<after.hAttempts<<",\"qualified\":"<<after.hQualified
          <<",\"reason\":"<<quote(after.hRefusal)<<"}"
          <<",\"hdrTriggered\":"<<after.hdrTriggered<<",\"hdrSelected\":"<<after.hdrSelected
          <<",\"hdrVerdict\":"<<quote(after.hdrVerdict)
          <<",\"resolverCalls\":"<<after.resolverCalls<<",\"backendFailures\":"<<after.backendFailures
          <<",\"hdrResolves\":"<<after.hdrResolves<<",\"hdrSpatial\":"<<after.hdrSpatial
          <<",\"hdrCaptured\":"<<after.hdrCaptured<<",\"hdrCopied\":"<<after.hdrCopied
          <<",\"hdrPrepped\":"<<after.hdrPrepped<<",\"backendCalls\":"<<after.hdrBackendCompleted
          <<",\"hdrFinished\":"<<after.hdrFinished<<",\"hdrRestored\":"<<after.hdrRestored
          <<",\"guardConfirmed\":"<<(guardConfirmed?"true":"false")
          <<"},\"limitations\":[\"Controlled geometry, camera and OM state reconstructed; shader bytes captured\"";
    if(!hostGuard)result<<",\"Emit-hook availability reconstructed only in offline test-link proxy\"";
    result<<"]}";
    std::puts(result.str().c_str());
    return std::strcmp(verdict,"PASS")==0?0:std::strcmp(verdict,"UNSUPPORTED")==0?2:1;
}
