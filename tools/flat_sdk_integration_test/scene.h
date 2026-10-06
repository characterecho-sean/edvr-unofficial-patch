// Controlled draw state around immutable, hash-checked game shader bytecode.
// Included inside the bench translation unit's anonymous namespace.
struct SceneVertex { uint32_t a[4], b[4], c[4]; };
struct SceneEvidence {
    Snapshot world{}, alternate{}, writer{}, consumer{};
    unsigned depthPixels=0, poolPixels=0, hdrPixels=0;
    // The sizes the case ran at, read from the targets themselves: the HDR scene target's (the render size) and the swap chain's
    // back buffer (the output size).
    unsigned renderW=0, renderH=0, outputW=0, outputH=0;
    unsigned gpuWorldPixelsBeforeH=0, gpuForeignPixelsBeforeH=0;
    unsigned gpuWorldPixelsAtH=0, gpuForeignPixelsAtH=0;
    unsigned inertDepthPixels=0, inertStencilPixels=0, inertColorPixels=0;
    unsigned stateColorPixels=0;
    unsigned validDepthPixels=0;
    float depthMin=1.f, depthMax=0.f;
    Snapshot inert{},stateDrawn{},stateLater{};
    // Settlement order: the first-person draw and the world-camera prepass run draw before the world is named.
    Snapshot firstPerson{}, cfcaFirstPerson{}, cfcaWorld{}, prepass{}, firstPersonHdr{};
    unsigned prepassDepthPixels=0;
    // The stale-mark case: a world-camera depth writer over a first-person surface, and the owner plane around it.
    Snapshot worldWriter{};
    bool planeUntouched=false;
    unsigned worldWriterDepthPixels=0;
    struct OwnerMarks {unsigned foreign=0,stale=0,fresh=0;} marksBefore{},marksAfter{},marksAtH{};
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

bool makeSceneTexture(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format, DXGI_FORMAT viewFormat,
                      UINT bind, ComPtr<ID3D11Texture2D>& texture,
                      ComPtr<ID3D11RenderTargetView>& view) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width=width;desc.Height=height;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
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
// The owner plane's own texels: RGBA32F, the marker, the raw depth it was written at, then draw metadata. Empty if unavailable.
std::vector<BYTE> readOwnerPlane(ID3D11Device* device, ID3D11DeviceContext* context,
                                 unsigned int (__cdecl* ownerView)(ID3D11ShaderResourceView**)) {
    ComPtr<ID3D11ShaderResourceView> view;
    if(!ownerView(view.GetAddressOf()) || !view)return {};
    ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if(!resource || FAILED(resource.As(&texture)))return {};
    return readPixels(device,context,texture.Get(),16);
}
// First-person marks (a marker below -1.5, as readOwnerPixels counts them) by whether the depth a mark carries is still the depth
// texture's at that pixel, bit for bit: the prep shader's own test. The depth texture's first dword per texel is the depth.
SceneEvidence::OwnerMarks classifyOwnerMarks(const std::vector<BYTE>& plane,const std::vector<BYTE>& depth8) {
    SceneEvidence::OwnerMarks marks{};
    if(plane.empty() || plane.size()/16!=depth8.size()/8)return marks;
    for(size_t i=0;i<plane.size()/16;++i) {
        float marker=0;uint32_t markDepth=0,rawDepth=0;
        std::memcpy(&marker,plane.data()+16*i,4);std::memcpy(&markDepth,plane.data()+16*i+4,4);
        std::memcpy(&rawDepth,depth8.data()+8*i,4);
        if(!std::isfinite(marker) || marker>=-1.5f)continue;
        ++marks.foreign;
        if(markDepth==rawDepth)++marks.fresh;else ++marks.stale;
    }
    return marks;
}

int scene(const wchar_t* proxyPath, const wchar_t* fixturePath, D3D_DRIVER_TYPE driver,
          const char* name) {
    HMODULE proxy=LoadLibraryExW(proxyPath,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!proxy) {std::fprintf(stderr,"flat SDK bench: proxy load failed %lu\n",GetLastError());return 2;}
    const auto snapshot=reinterpret_cast<SnapshotFn>(GetProcAddress(proxy,"edvr_selftest_flat_sdk_snapshot"));
    using OwnerViewFn=unsigned int (__cdecl*)(ID3D11ShaderResourceView**);
    const auto ownerView=reinterpret_cast<OwnerViewFn>(GetProcAddress(proxy,"edvr_selftest_flat_sdk_owner_view"));
    if (!snapshot || !ownerView) {std::fputs("flat SDK bench: snapshot or owner export absent\n",stderr);return 2;}
    // Sizes (section 104). Every case renders and presents at 64 x 64. The supersampled cases are the on-foot frame of a screen
    // rendered above it (the HDR route's R > D): 96 x 96 into 64 x 64 (1.5 x per axis), and, on hardware only, the real size that
    // the 16M-pixel bound of the foreground map once refused: 5760 x 3240 (4K at SS 1.5) into 3840 x 2160.
    const bool supersampled=std::strcmp(name,"supersampled_scene")==0;
    const bool supersampled4k=std::strcmp(name,"supersampled_scene_4k")==0;
    const bool supersampledAny=supersampled||supersampled4k;
    const UINT outputW=supersampled4k?3840u:64u,outputH=supersampled4k?2160u:64u;
    const UINT renderW=supersampled4k?5760u:supersampled?96u:64u,renderH=supersampled4k?3240u:supersampled?96u:64u;
    edvr::openxr::PresentDevice present;
    if (!ok(present.initialize(proxy,driver,outputW,outputH),"hidden proxy device")) return 2;
    for(unsigned i=0;i<3;++i) if(!ok(present.present(),"arm flat frame"))return 2;
    Snapshot before{};
    if(!snapshot(&before,sizeof(before)) || !before.live || !before.owner) {
        std::fprintf(stderr,"flat SDK bench: scene frame not armed live=%u owner=%u mode=%s\n",before.live,before.owner,before.mode);
        return 2;
    }
    Snapshot incompatible{};
    incompatible.version=4;
    if(snapshot(&incompatible,sizeof(incompatible))) {
        std::fputs("flat SDK bench: snapshot accepted a future ABI version\n",stderr);return 2;
    }
    incompatible.version=2;
    if(snapshot(&incompatible,sizeof(incompatible))) {
        std::fputs("flat SDK bench: snapshot accepted a retired ABI version\n",stderr);return 2;
    }
    incompatible.version=3;incompatible.size=sizeof(incompatible)-1;
    if(snapshot(&incompatible,sizeof(incompatible))) {
        std::fputs("flat SDK bench: snapshot accepted a wrong ABI size\n",stderr);return 2;
    }
    auto* device=present.device();auto* context=present.context();
    // The case table in flat_sdk_integration_test.cpp admits the names; these select the draw state.
    //
    // Cameras (section 104). Camera 0 is the world's, near .025: family 0 draws with it, and so do the prepass and
    // world-depth draws. Camera 1 is the first person's, near .0675: family 1 and the laser draw with it. Only a
    // first-person (foreign camera) depth writer is planned, captured and marked; a draw with the named world camera
    // (or, before naming, the last world's near) and a draw with no camera are never planned, marked or refused.
    // The domain's world-marker count therefore stays at zero in every case here; the engine producer's own slots
    // (the supported family-0 pair) are not that count.
    const bool partialState=std::strcmp(name,"state_partial_mask")==0;
    const bool blendedState=std::strcmp(name,"state_blended")==0;
    const bool blendedNoDepth=std::strcmp(name,"state_blended_no_depth")==0;
    const bool blendedHdr=std::strcmp(name,"state_blended_hdr")==0;
    const bool blendedHdrWorld=std::strcmp(name,"state_blended_hdr_world")==0;
    const bool settlement=std::strcmp(name,"settlement_prepass")==0;
    // The first-person camera that shares the world's near plane (camera 2, aiming down sights): another projection scale. At x1.25 the
    // scale tells it from the world (flatDomainPredictsWorld: near AND scale within 10%), so its pre-naming draw is a first-person draw,
    // captured, and the frame is admitted (`mismatch`, the case that used to guard the near-only prediction's refusal). At x1.03 it is
    // still the world's by every test the runtime has, so it is predicted, leaves a witness no world camera matches, and H refuses as it
    // always did (`closeScale`, the guard that the scale margin is not a loophole).
    const bool mismatch=std::strcmp(name,"predicted_world_mismatch")==0;
    const bool closeScale=std::strcmp(name,"predicted_world_close_scale")==0;
    const bool weaponCamera=mismatch||closeScale;
    const bool staleMark=std::strcmp(name,"stale_foreign_mark")==0;
    const bool inertWorld=std::strcmp(name,"inert_depth_write_world")==0;
    const bool prepassCase=settlement||weaponCamera;
    const bool stateDraw=partialState||blendedState||blendedNoDepth;
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
    // The world camera's depth prepass pair as the settlement draws it: the pool VS and an alpha-test PS with no
    // color output. Captured bytes; the input layout is the family's own. Not in the engine producer's families, so
    // it is a world-camera depth writer that nothing marks: the stale-mark case draws it over a first-person surface.
    ComPtr<ID3D11VertexShader> prepassVs;ComPtr<ID3D11PixelShader> prepassPs;ComPtr<ID3D11InputLayout> prepassLayout;
    if(prepassCase||staleMark) {
        std::vector<BYTE> prepassVsCode,prepassPsCode;
        if(!readShader(fixturePath,L"vs_F516BF0201303B87.dxbc",0xF516BF0201303B87ull,prepassVsCode) ||
           !readShader(fixturePath,L"ps_B40B0462256E31C2.dxbc",0xB40B0462256E31C2ull,prepassPsCode)) {
            std::fputs("flat SDK bench: fixture missing or hash mismatch: settlement prepass pair\n",stderr);return 2;
        }
        if(!ok(device->CreateVertexShader(prepassVsCode.data(),prepassVsCode.size(),nullptr,&prepassVs),"captured prepass VS") ||
           !ok(device->CreatePixelShader(prepassPsCode.data(),prepassPsCode.size(),nullptr,&prepassPs),"captured prepass PS") ||
           !ok(device->CreateInputLayout(inputs,4,prepassVsCode.data(),prepassVsCode.size(),&prepassLayout),"captured prepass layout"))return 2;
    }
    // The first-person laser's Gbuffer pair, drawn in both cameras: clip W into RT0 with MIN blend and a depth write.
    // It projects through CB0[4..7] by dp4 rather than the camera table, and has no PACKEDVERTEXDATAB input.
    ComPtr<ID3D11VertexShader> cfcaVs;ComPtr<ID3D11PixelShader> cfcaPs;ComPtr<ID3D11InputLayout> cfcaLayout;
    if(settlement) {
        std::vector<BYTE> cfcaVsCode,cfcaPsCode;
        if(!readShader(fixturePath,L"vs_CFCA8FFC6B058630.dxbc",0xCFCA8FFC6B058630ull,cfcaVsCode) ||
           !readShader(fixturePath,L"ps_8A08FF781272C5F6.dxbc",0x8A08FF781272C5F6ull,cfcaPsCode)) {
            std::fputs("flat SDK bench: fixture missing or hash mismatch: settlement CFCA pair\n",stderr);return 2;
        }
        const D3D11_INPUT_ELEMENT_DESC cfcaInputs[]={inputs[0],inputs[1],inputs[3]};
        if(!ok(device->CreateVertexShader(cfcaVsCode.data(),cfcaVsCode.size(),nullptr,&cfcaVs),"captured CFCA VS") ||
           !ok(device->CreatePixelShader(cfcaPsCode.data(),cfcaPsCode.size(),nullptr,&cfcaPs),"captured CFCA PS") ||
           !ok(device->CreateInputLayout(cfcaInputs,3,cfcaVsCode.data(),cfcaVsCode.size(),&cfcaLayout),"captured CFCA layout"))return 2;
    }
    ComPtr<ID3D11Texture2D> pool,hdr,consumerColor;
    ComPtr<ID3D11RenderTargetView> poolRtv,hdrRtv,consumerRtv;
    // The consumer's target is the game's tone pass target, rendered at the scene's size, as in the game.
    if(!makeSceneTexture(device,renderW,renderH,DXGI_FORMAT_R10G10B10A2_TYPELESS,DXGI_FORMAT_R10G10B10A2_UNORM,
        D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE,pool,poolRtv) ||
       !makeSceneTexture(device,renderW,renderH,DXGI_FORMAT_R11G11B10_FLOAT,DXGI_FORMAT_R11G11B10_FLOAT,
        D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE,hdr,hdrRtv) ||
       !makeSceneTexture(device,renderW,renderH,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM,
        D3D11_BIND_RENDER_TARGET,consumerColor,consumerRtv))return 2;
    D3D11_TEXTURE2D_DESC depthDesc{};depthDesc.Width=renderW;depthDesc.Height=renderH;
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
    // Records 5 and 9 are the world's and the first person's triangles (family 0 and 1); record 11 is the stale-mark
    // case's world-camera surface: small, centred right of the first person's, and in front of it.
    const uint32_t instance[3][2]={{5,0},{9,0},{11,0}};const uint16_t indices[3]={0,1,2};
    ComPtr<ID3D11Buffer> verticesBuffer,instanceBuffer[3],indexBuffer;
    auto createDataBuffer=[&](const void* data,UINT size,UINT bind,ComPtr<ID3D11Buffer>& out) {
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=size;bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=bind;
        D3D11_SUBRESOURCE_DATA init{data,0,0};return ok(device->CreateBuffer(&bd,&init,&out),"scene geometry buffer");};
    if(!createDataBuffer(vertices,sizeof(vertices),D3D11_BIND_VERTEX_BUFFER,verticesBuffer) ||
       !createDataBuffer(instance[0],sizeof(instance[0]),D3D11_BIND_VERTEX_BUFFER,instanceBuffer[0]) ||
       !createDataBuffer(instance[1],sizeof(instance[1]),D3D11_BIND_VERTEX_BUFFER,instanceBuffer[1]) ||
       !createDataBuffer(instance[2],sizeof(instance[2]),D3D11_BIND_VERTEX_BUFFER,instanceBuffer[2]) ||
       !createDataBuffer(indices,sizeof(indices),D3D11_BIND_INDEX_BUFFER,indexBuffer))return 2;
    // The prepass run: draw i starts at index 3*i, so no two prepass draws share a packet though they read the
    // same three vertices. More than the 64 draws one frame's foreground capture may hold.
    constexpr unsigned prepassDraws=72;
    ComPtr<ID3D11Buffer> prepassIndexBuffer;
    if(prepassCase) {
        std::vector<uint16_t> run(prepassDraws*3);
        for(unsigned i=0;i<prepassDraws;++i) {run[3*i]=0;run[3*i+1]=1;run[3*i+2]=2;}
        if(!createDataBuffer(run.data(),static_cast<UINT>(run.size()*sizeof(uint16_t)),D3D11_BIND_INDEX_BUFFER,prepassIndexBuffer))return 2;
    }
    struct PoolRecord {uint32_t words[84];};
    PoolRecord records[16]{};
    auto lane=[](float c){return static_cast<uint32_t>((c+1.f)*32767.f);};
    float scale=1.f, forward=2.f;std::memcpy(&records[5].words[1],&scale,4);
    std::memcpy(&records[5].words[6],&forward,4);
    records[5].words[2]=lane(0.f)|(lane(0.f)<<16);
    records[5].words[3]=lane(0.f)|(lane(1.f)<<16);
    records[9]=records[5];
    float alternateX=.65f;std::memcpy(&records[9].words[4],&alternateX,4);
    // Record 11 at the world's near: scale .1, forward .5 (depth .025/.5 = .05, nearer than the first person's .03375 since
    // depth is reversed), offset .225 (centre x .225/.5 = .45 in NDC, inside the first person's triangle and mostly
    // outside the world family's).
    records[11]=records[5];
    float smallScale=.1f,smallX=.225f,smallForward=.5f;
    std::memcpy(&records[11].words[1],&smallScale,4);std::memcpy(&records[11].words[4],&smallX,4);
    std::memcpy(&records[11].words[6],&smallForward,4);
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
    // Camera 0 is the world (near .025), 1 the first person (near .0675). Camera 2 exists only for the two weapon-camera
    // cases and is their first-person camera: the world's near with another projection scale (x1.25 in `mismatch`, x1.03 in
    // `closeScale`), so a draw taken for the world by its near alone is not the camera H selects. It replaces camera 1 rather
    // than joining it: measured 2026-10-06, with the world, camera 1 and a mid-run prepass draw at this one all on the depth,
    // the selector refused the frame first as source-camera-or-depth-not-unique (no H attempt, no failure). With only the
    // world and this camera the pending-witness check is reached.
    const unsigned cameraCount=weaponCamera?3u:2u;
    const float weaponScale=closeScale?1.03f:1.25f;
    const auto cameraRows=[weaponScale](unsigned c,float (&rows)[280][4]) {
        rows[270][0]=c==2?weaponScale:1.f;rows[271][1]=1;rows[272][3]=1;
        rows[273][2]=c==1?.0675f:.025f;rows[274][2]=1;
    };
    ComPtr<ID3D11Buffer> camera[3];
    for(unsigned c=0;c<cameraCount;++c) {
        float rows[280][4]{};cameraRows(c,rows);
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(rows);bd.Usage=D3D11_USAGE_DYNAMIC;
        bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        if(!ok(device->CreateBuffer(&bd,nullptr,&camera[c]),"scene camera"))return 2;
        ID3D11Buffer* bound=camera[c].Get();context->VSSetConstantBuffers(1,1,&bound);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if(!ok(context->Map(camera[c].Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"scene camera upload"))return 2;
        std::memcpy(mapped.pData,rows,sizeof(rows));context->Unmap(camera[c].Get(),0);
    }
    // CB0[4..7] of the CFCA draws: each camera's clip rows transposed for the shader's dp4 projection, with the same
    // near plane as its camera table. Index 0 is the world camera, 1 the first-person one.
    ComPtr<ID3D11Buffer> cfcaClip[2];
    const auto uploadClip=[&](unsigned c)->bool {
        float rows[8][4]{};rows[4][0]=1;rows[5][1]=1;rows[6][3]=c?.0675f:.025f;rows[7][2]=1;
        if(!cfcaClip[c]) {
            D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(rows);bd.Usage=D3D11_USAGE_DYNAMIC;
            bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            if(!ok(device->CreateBuffer(&bd,nullptr,&cfcaClip[c]),"CFCA clip rows"))return false;
        }
        ID3D11Buffer* bound=cfcaClip[c].Get();context->VSSetConstantBuffers(0,1,&bound);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if(!ok(context->Map(cfcaClip[c].Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"CFCA clip rows upload"))return false;
        std::memcpy(mapped.pData,rows,sizeof(rows));context->Unmap(cfcaClip[c].Get(),0);
        ID3D11Buffer* unbound=nullptr;context->VSSetConstantBuffers(0,1,&unbound);
        return true;
    };
    if(settlement && (!uploadClip(0) || !uploadClip(1)))return 2;
    const D3D11_VIEWPORT vp{0,0,static_cast<float>(renderW),static_cast<float>(renderH),0,1};context->RSSetViewports(1,&vp);
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
    // Family 0 draws with the world camera, family 1 with the first-person one: camera 1 (near .0675), or in the
    // weapon-camera cases camera 2, whose near equals the world's.
    const unsigned familyCamera[2]={0u,weaponCamera?2u:1u};
    auto originalDraw=[&](unsigned family,ID3D11RenderTargetView* target,Snapshot& snap) {
        ID3D11RenderTargetView* view=target;context->OMSetRenderTargets(1,&view,dsv.Get());
        ID3D11Buffer* cb=camera[familyCamera[family]].Get();context->VSSetConstantBuffers(1,1,&cb);
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
    // The laser's Gbuffer draw in the camera `view` (0 world with record 5, 1 first person with record 9): MIN blend on
    // RT0 with IndependentBlendEnable, and a GREATER_EQUAL depth test with the depth write on, as the settlement has it.
    ComPtr<ID3D11BlendState> cfcaBlendState;ComPtr<ID3D11DepthStencilState> cfcaDepthState;
    if(settlement) {
        D3D11_BLEND_DESC minBlend{};minBlend.IndependentBlendEnable=TRUE;
        for(auto& target:minBlend.RenderTarget)target.RenderTargetWriteMask=15;
        auto& rt0=minBlend.RenderTarget[0];rt0.BlendEnable=TRUE;
        rt0.SrcBlend=rt0.DestBlend=rt0.SrcBlendAlpha=rt0.DestBlendAlpha=D3D11_BLEND_ONE;
        rt0.BlendOp=rt0.BlendOpAlpha=D3D11_BLEND_OP_MIN;
        D3D11_DEPTH_STENCIL_DESC greaterEqual{};greaterEqual.DepthEnable=TRUE;
        greaterEqual.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;greaterEqual.DepthFunc=D3D11_COMPARISON_GREATER_EQUAL;
        if(!ok(device->CreateBlendState(&minBlend,&cfcaBlendState),"CFCA MIN blend") ||
           !ok(device->CreateDepthStencilState(&greaterEqual,&cfcaDepthState),"CFCA depth state"))return 2;
    }
    auto cfcaDraw=[&](unsigned view,Snapshot& snap) {
        ID3D11RenderTargetView* target=poolRtv.Get();context->OMSetRenderTargets(1,&target,dsv.Get());
        ID3D11Buffer* clip=cfcaClip[view].Get();context->VSSetConstantBuffers(0,1,&clip);
        ID3D11Buffer* table=camera[view].Get();context->VSSetConstantBuffers(1,1,&table);
        context->PSSetConstantBuffers(1,1,&table);
        context->IASetInputLayout(cfcaLayout.Get());
        vb[0]=instanceBuffer[view].Get();context->IASetVertexBuffers(0,2,vb,strides,offsets);
        context->IASetIndexBuffer(indexBuffer.Get(),DXGI_FORMAT_R16_UINT,0);
        context->VSSetShader(cfcaVs.Get(),nullptr,0);context->PSSetShader(cfcaPs.Get(),nullptr,0);
        ID3D11ShaderResourceView* none[7]{};context->PSSetShaderResources(0,7,none);
        context->OMSetBlendState(cfcaBlendState.Get(),nullptr,0xffffffffu);
        context->OMSetDepthStencilState(cfcaDepthState.Get(),0);
        context->DrawIndexedInstanced(3,1,0,0,0);
        context->OMSetBlendState(nullptr,nullptr,0xffffffffu);
        context->OMSetDepthStencilState(depthState.Get(),0);
        ID3D11Buffer* unbound=nullptr;context->VSSetConstantBuffers(0,1,&unbound);
        return snapshot(&snap,sizeof(snap))!=0;
    };
    // A world-camera depth writer outside the engine producer's families: the prepass pair with camera 0 and the surface
    // in `instanceData`. Nothing marks it (it has the named world camera), and it writes depth.
    auto worldDepthWriter=[&](ID3D11Buffer* instanceData) {
        ID3D11RenderTargetView* target=poolRtv.Get();context->OMSetRenderTargets(1,&target,dsv.Get());
        ID3D11Buffer* cb=camera[0].Get();context->VSSetConstantBuffers(1,1,&cb);context->PSSetConstantBuffers(1,1,&cb);
        context->IASetInputLayout(prepassLayout.Get());
        vb[0]=instanceData;context->IASetVertexBuffers(0,2,vb,strides,offsets);
        context->IASetIndexBuffer(indexBuffer.Get(),DXGI_FORMAT_R16_UINT,0);
        context->VSSetShader(prepassVs.Get(),nullptr,0);context->PSSetShader(prepassPs.Get(),nullptr,0);
        ID3D11ShaderResourceView* resources[7]{};resources[0]=textureSrv.Get();
        context->PSSetShaderResources(0,7,resources);
        context->DrawIndexedInstanced(3,1,0,0,0);
    };
    // The engine source has a previous-frame scene CB and slot plane by
    // contract. Produce one ordinary world frame before the measured frame.
    Snapshot warmup{};
    for(unsigned frame=0;frame<2;++frame) {
        if(!originalDraw(0,poolRtv.Get(),warmup) || !ok(present.present(),"source warmup Present"))return 2;
        for(unsigned c=0;c<cameraCount;++c) {
            float rows[280][4]{};cameraRows(c,rows);
            ID3D11Buffer* bound=camera[c].Get();context->VSSetConstantBuffers(1,1,&bound);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if(!ok(context->Map(camera[c].Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"next-frame camera upload"))return 2;
            std::memcpy(mapped.pData,rows,sizeof(rows));context->Unmap(camera[c].Get(),0);
        }
        if(settlement)for(unsigned c=0;c<2;++c)if(!uploadClip(c))return 2;
        context->ClearRenderTargetView(poolRtv.Get(),clearPool);
        context->ClearRenderTargetView(hdrRtv.Get(),clearHdr);
        context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,1,0);
    }
    if(!snapshot(&before,sizeof(before)) || !before.live)return 2;
    const auto poolBefore=readPixels(device,context,pool.Get(),4);
    const auto hdrBefore=readPixels(device,context,hdr.Get(),4);
    const auto depthBefore=readPixels(device,context,depth.Get(),8);
    if(prepassCase) {
        // The settlement's order, in a frame after one that named the world: the first-person camera draws first
        // (the family's draw and, in the settlement, the laser pair), then a run of the world camera's depth prepass
        // with color masked off and the laser pair again, and only then the material draw that names the world.
        if(!originalDraw(1,poolRtv.Get(),evidence.firstPerson))return 2;
        if(settlement && !cfcaDraw(1,evidence.cfcaFirstPerson))return 2;
        const auto depthAfterFirstPerson=readPixels(device,context,depth.Get(),8);
        D3D11_BLEND_DESC prepassMask{};   // every render-target write mask zero
        ComPtr<ID3D11BlendState> prepassMaskState;
        if(!ok(device->CreateBlendState(&prepassMask,&prepassMaskState),"prepass color mask"))return 2;
        ID3D11RenderTargetView* prepassTarget=poolRtv.Get();context->OMSetRenderTargets(1,&prepassTarget,dsv.Get());
        context->OMSetBlendState(prepassMaskState.Get(),nullptr,0xffffffffu);
        context->IASetInputLayout(prepassLayout.Get());
        vb[0]=instanceBuffer[0].Get();context->IASetVertexBuffers(0,2,vb,strides,offsets);
        context->IASetIndexBuffer(prepassIndexBuffer.Get(),DXGI_FORMAT_R16_UINT,0);
        context->VSSetShader(prepassVs.Get(),nullptr,0);context->PSSetShader(prepassPs.Get(),nullptr,0);
        ID3D11ShaderResourceView* prepassResources[7]{};prepassResources[0]=textureSrv.Get();
        context->PSSetShaderResources(0,7,prepassResources);
        ID3D11Buffer* prepassCamera=camera[0].Get();   // the world camera, as the settlement's prepass draws it
        context->VSSetConstantBuffers(1,1,&prepassCamera);context->PSSetConstantBuffers(1,1,&prepassCamera);
        for(unsigned i=0;i<prepassDraws;++i)context->DrawIndexedInstanced(3,1,3*i,0,0);
        context->OMSetBlendState(nullptr,nullptr,0xffffffffu);
        if(settlement && !cfcaDraw(0,evidence.cfcaWorld))return 2;
        if(!snapshot(&evidence.prepass,sizeof(evidence.prepass)))return 2;
        evidence.prepassDepthPixels=changedPixels(depthAfterFirstPerson,readPixels(device,context,depth.Get(),8),8);
    }
    // The world draw: family 0 with camera 0. It names the world and is counted unmarked, never planned or captured.
    if(!originalDraw(0,poolRtv.Get(),evidence.world))return 2;
    if(stateDraw) {
        // A masked or blended draw of the first-person pair (family 1, camera 1), then the ordinary one. With a depth
        // write it defines the surface it shows and is admitted; without one it cannot change the surface and is
        // forwarded. Both are first-person draws, so both are planned.
        D3D11_BLEND_DESC drawBlend{};
        const bool blendOn=blendedState||blendedNoDepth;
        drawBlend.RenderTarget[0].RenderTargetWriteMask=partialState?7:15;
        drawBlend.RenderTarget[0].BlendEnable=blendOn?TRUE:FALSE;
        drawBlend.RenderTarget[0].SrcBlend=D3D11_BLEND_ONE;
        drawBlend.RenderTarget[0].DestBlend=blendOn?D3D11_BLEND_ONE:D3D11_BLEND_ZERO;
        drawBlend.RenderTarget[0].BlendOp=D3D11_BLEND_OP_ADD;
        drawBlend.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_ONE;
        drawBlend.RenderTarget[0].DestBlendAlpha=blendOn?D3D11_BLEND_ONE:D3D11_BLEND_ZERO;
        drawBlend.RenderTarget[0].BlendOpAlpha=D3D11_BLEND_OP_ADD;
        ComPtr<ID3D11BlendState> drawBlendState;
        if(!ok(device->CreateBlendState(&drawBlend,&drawBlendState),"state draw blend"))return 2;
        D3D11_DEPTH_STENCIL_DESC keepDepth{};keepDepth.DepthEnable=TRUE;
        keepDepth.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;keepDepth.DepthFunc=D3D11_COMPARISON_ALWAYS;
        ComPtr<ID3D11DepthStencilState> keepDepthState;
        if(!ok(device->CreateDepthStencilState(&keepDepth,&keepDepthState),"state draw read-only depth"))return 2;
        const auto beforeDraw=readPixels(device,context,pool.Get(),4);
        context->OMSetBlendState(drawBlendState.Get(),nullptr,0xffffffffu);
        if(blendedNoDepth)context->OMSetDepthStencilState(keepDepthState.Get(),0);
        if(!originalDraw(1,poolRtv.Get(),evidence.stateDrawn))return 2;
        evidence.stateColorPixels=changedPixels(beforeDraw,readPixels(device,context,pool.Get(),4),4);
        context->OMSetBlendState(nullptr,nullptr,0xffffffffu);
        context->OMSetDepthStencilState(depthState.Get(),0);
        if(!originalDraw(1,poolRtv.Get(),evidence.stateLater))return 2;
        evidence.alternate=evidence.stateLater;
    } else if(!originalDraw(1,poolRtv.Get(),evidence.alternate))return 2;   // the first person's surface: family 1, camera 1 (2 in the weapon-camera cases)
    if(staleMark) {
        // A world surface drawn over the first person's, in front of it (camera 0, record 11). It has the named world camera,
        // so nothing marks it: the owner plane must come out byte-identical, and the first-person marks it covers keep
        // a depth that is no longer the pixel's. The prep trusts a mark only while its depth is still the pixel's.
        const auto planePre=readOwnerPlane(device,context,ownerView);
        const auto depthPre=readPixels(device,context,depth.Get(),8);
        worldDepthWriter(instanceBuffer[2].Get());
        const auto planePost=readOwnerPlane(device,context,ownerView);
        const auto depthPost=readPixels(device,context,depth.Get(),8);
        if(!snapshot(&evidence.worldWriter,sizeof(evidence.worldWriter)))return 2;
        evidence.planeUntouched=!planePre.empty() && planePre==planePost;
        evidence.marksBefore=classifyOwnerMarks(planePre,depthPre);
        evidence.marksAfter=classifyOwnerMarks(planePost,depthPost);
        evidence.worldWriterDepthPixels=changedPixels(depthPre,depthPost,8);
    }
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
        const bool depthWrite=std::strcmp(name,"inert_depth_write")==0 || inertWorld;
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
        // The inert pair's camera is explicit: the first person's (camera 1: a foreign camera, so a first-person depth
        // writer that is not a pool mesh cannot be captured and refuses) in every inert case but the world one, whose
        // draw has the named world camera and is never planned, marked or refused.
        ID3D11Buffer* inertCamera=camera[inertWorld?0:1].Get();
        context->VSSetConstantBuffers(1,1,&inertCamera);context->PSSetConstantBuffers(1,1,&inertCamera);
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
    // The HDR light target's writers. Every case has the world writer (family 0, camera 0: the supported pair is also the
    // engine producer, which keeps its own slots; the domain neither marks nor refuses it). state_blended_hdr_world blends
    // that writer; state_blended_hdr adds a blended first-person one (family 1, camera 1) into the same target.
    D3D11_BLEND_DESC hdrBlend{};
    hdrBlend.RenderTarget[0].RenderTargetWriteMask=15;hdrBlend.RenderTarget[0].BlendEnable=TRUE;
    hdrBlend.RenderTarget[0].SrcBlend=hdrBlend.RenderTarget[0].DestBlend=D3D11_BLEND_ONE;
    hdrBlend.RenderTarget[0].SrcBlendAlpha=hdrBlend.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_ONE;
    hdrBlend.RenderTarget[0].BlendOp=hdrBlend.RenderTarget[0].BlendOpAlpha=D3D11_BLEND_OP_ADD;
    ComPtr<ID3D11BlendState> hdrBlendState;
    if((blendedHdr||blendedHdrWorld) && !ok(device->CreateBlendState(&hdrBlend,&hdrBlendState),"blended HDR writer"))return 2;
    if(blendedHdrWorld)context->OMSetBlendState(hdrBlendState.Get(),nullptr,0xffffffffu);
    if(!originalDraw(0,hdrRtv.Get(),evidence.writer))return 2;
    if(blendedHdrWorld)context->OMSetBlendState(nullptr,nullptr,0xffffffffu);
    if(blendedHdr) {
        context->OMSetBlendState(hdrBlendState.Get(),nullptr,0xffffffffu);
        if(!originalDraw(1,hdrRtv.Get(),evidence.firstPersonHdr))return 2;
        context->OMSetBlendState(nullptr,nullptr,0xffffffffu);
    }
    const auto ownerAtH=readOwnerPixels(device,context,ownerView);
    evidence.gpuWorldPixelsAtH=ownerAtH.world;
    evidence.gpuForeignPixelsAtH=ownerAtH.foreign;
    if(staleMark)evidence.marksAtH=classifyOwnerMarks(readOwnerPlane(device,context,ownerView),readPixels(device,context,depth.Get(),8));
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
    {   // The sizes the case ran at, from the targets: the scene's HDR target, and the swap chain's back buffer the proxy reads as the output.
        D3D11_TEXTURE2D_DESC hdrDesc{},backDesc{};hdr->GetDesc(&hdrDesc);
        ComPtr<ID3D11Texture2D> backBuffer;
        if(SUCCEEDED(present.swapchain()->GetBuffer(0,IID_PPV_ARGS(&backBuffer))) && backBuffer)backBuffer->GetDesc(&backDesc);
        evidence.renderW=hdrDesc.Width;evidence.renderH=hdrDesc.Height;evidence.outputW=backDesc.Width;evidence.outputH=backDesc.Height;
    }
    const auto& after=evidence.consumer;
    const bool taa=std::strcmp(after.mode,"on")==0;
    const bool hostGuard=std::strcmp(name,"unsupported_host")==0;
    const bool depthGuard=std::strcmp(name,"inert_depth_write")==0;
    const bool colorInert=std::strcmp(name,"inert_color_write")==0;
    // A guard expects a refusal that must remain. Every other case expects the production resolve to complete.
    const bool guardCase=hostGuard||depthGuard||blendedHdr||closeScale;
    const bool inertNoWrite=std::strcmp(name,"inert_no_write")==0;
    // The counters are cumulative for the process: what a case claims is what changed over the measured frame.
    const auto since=[](uint64_t now,uint64_t then){return now>=then?now-then:0ull;};
    const uint64_t predictedDelta=since(after.predictedWorld,before.predictedWorld);
    const uint64_t surfaceDelta=since(after.surfacePreserving,before.surfacePreserving);
    const uint64_t surfaceForeignDelta=since(after.surfacePreservingForeign,before.surfacePreservingForeign);
    const uint64_t worldUnmarkedDelta=since(after.worldUnmarked,before.worldUnmarked);
    const uint64_t worldMarkerDelta=since(after.worldMarkers,before.worldMarkers);
    const uint64_t foreignDelta=since(after.foreignSeen,before.foreignSeen);
    const uint64_t capturedDelta=since(after.captured,before.captured);
    const uint64_t attemptedDelta=since(after.hAttempts,before.hAttempts);
    const uint64_t qualifiedDelta=since(after.hQualified,before.hQualified);
    const uint64_t backendDelta=since(after.hdrBackendCompleted,before.hdrBackendCompleted);
    const uint64_t resolverDelta=since(after.resolverCalls,before.resolverCalls);
    const uint64_t spatialDelta=since(after.hdrSpatial,before.hdrSpatial);
    const uint64_t backendFailureDelta=since(after.backendFailures,before.backendFailures);
    const uint64_t prepassPredicted=since(evidence.prepass.predictedWorld,before.predictedWorld);
    const uint64_t prepassForeign=since(evidence.prepass.foreignSeen,before.foreignSeen);
    const uint64_t prepassCaptured=since(evidence.prepass.captured,before.captured);
    const uint64_t prepassWorldMarkers=since(evidence.prepass.worldMarkers,before.worldMarkers);
    const uint64_t prepassUnmarked=since(evidence.prepass.worldUnmarked,before.worldUnmarked);
    // No refusal anywhere on the frame: the snapshot reports the frame's first failure over every candidate.
    const bool frameClean=!after.firstFailureFrame;
    // The domain marks first-person surfaces only (section 104): its world-marker count stays at zero, and every draw
    // with camera 0 (the named world camera) is counted unmarked. worldDraws is how many such draws a case makes:
    // the naming draw and the HDR writer, and the case's own (inert_depth_write_world, stale_foreign_mark). Prepass
    // and laser draws with camera 0 before naming are predicted, not unmarked. Native TAA never plans anything.
    const unsigned worldDraws=2u+(inertWorld?1u:0u)+(staleMark?1u:0u);
    const bool worldsUnmarked=taa || (!worldMarkerDelta && worldUnmarkedDelta==worldDraws);
    const bool rasterReady=evidence.validDepthPixels>0 && evidence.poolPixels>0 && evidence.hdrPixels>0;
    const bool ownershipForeign=evidence.world.namedWorld && evidence.alternate.foreignSeen &&
        evidence.alternate.captured && evidence.alternate.gpuIdentitySubmitted && evidence.gpuForeignPixelsAtH>0;
    // The engine producer's own slots (the supported family-0 pair) mark the world; the shipping host has no producer.
    const bool ownership=ownershipForeign && evidence.gpuWorldPixelsAtH>0;
    const bool route=after.hdrTriggered && after.hdrSelected && after.resolverCalls;
    const bool inertNoWriteMeasured=!inertNoWrite ||
        (evidence.inertStencilPixels>0 && !evidence.inertColorPixels && !evidence.inertDepthPixels &&
         !evidence.inert.firstFailureFrame);
    const bool upstream=rasterReady && evidence.world.namedWorld && route && inertNoWriteMeasured &&
        (taa || (ownership && after.hQualified>0));
    const bool inertWriterRefusal=depthGuard && after.hAttempts>0 && !after.hQualified &&
        evidence.inert.firstFailureVs==0xFC1193AFFC596F74ull &&
        evidence.inert.firstFailurePs==0x258B95AC99520C1Full &&
        std::strcmp(evidence.inert.firstFailureStage,"inert-state")==0 &&
        std::strcmp(evidence.inert.firstFailureReason,"foreground-inert-depth-write")==0;
    // state_blended_hdr: a blended first-person (family 1, camera 1) depth writer into the HDR light target. Measured
    // 2026-10-06 on the section 104 runtime, contradicting the expectation that it is admitted, captured and resolved:
    // the domain does admit it (format 26 takes a mark: foreignSeen and captured rise for it, the refusal inventory
    // stays empty, no first failure), but the prefix model, which decides what H is, vetoes any second camera writing H
    // before an attempt is made (flat_runtime_model.h: hdr-camera-changed, exempting only the laser pair 88DCF116/
    // 494506A6 and overlay-protected draws). The frame is refused as conflicting-hdr-target-or-camera, with no H
    // attempt and no resolve. This pins that verdict; it flips to a resolve if the veto is ever lifted for such draws.
    const bool hdrSecondCameraRefusal=blendedHdr && evidence.hdrPixels>0 &&
        foreignDelta==2 && capturedDelta==2 && !surfaceDelta && frameClean && !after.failureKinds &&
        !attemptedDelta && !qualifiedDelta && !backendDelta && !resolverDelta &&
        std::strcmp(after.hdrVerdict,"conflicting-hdr-target-or-camera")==0;
    // Settlement order: every prepass draw (camera 0) ran before the world was named, was planned as the predicted
    // world and none was captured or marked; only the first-person draws (camera 1, before naming) and the ordinary
    // alternate draw (camera 1) are foreign. In the close-scale case the pre-naming first-person draw has camera 2, with
    // the world's near and a scale 3% off the world's, so it too is taken for the world: it is predicted (never captured)
    // and the one foreign draw is the alternate after naming (camera 2 again). In the mismatch case camera 2's scale is
    // 25% off, the draw is not predicted, and it is foreign and captured like the one after naming (the section 104 fix for
    // aiming down sights). The settlement also draws the laser pair in both cameras before
    // naming: the camera-1 one is a foreign pool draw (captured), the camera-0 one is the predicted world like the
    // prepass run. None of the predicted draws is marked: the domain's world-marker count must not rise over the run.
    const unsigned mistaken=closeScale?1u:0u,laserForeign=settlement?1u:0u,laserWorld=settlement?1u:0u;
    const bool namedAfterPrepass=prepassCase && !evidence.prepass.namedWorld && evidence.world.namedWorld;
    // The prepass draws wrote depth (a run that discarded everything would still plan, but proves less).
    const bool prepassPlanned=namedAfterPrepass && evidence.prepassDepthPixels>0 &&
        prepassPredicted==prepassDraws+mistaken+laserWorld && !prepassWorldMarkers && !prepassUnmarked &&
        prepassForeign==1u-mistaken+laserForeign && prepassCaptured==1u-mistaken+laserForeign &&
        predictedDelta==prepassDraws+mistaken+laserWorld &&
        foreignDelta==2u-mistaken+laserForeign && capturedDelta==2u-mistaken+laserForeign;
    const bool settlementConfirmed=settlement && prepassPlanned && frameClean && qualifiedDelta>0;
    // The weapon camera 25% off the world's scale at the world's near plane: its pre-naming draw is captured as the first-person
    // draw it is (prepassPlanned with nothing mistaken: one foreign draw before naming and one after), no refusal anywhere, H
    // qualified and the backend's frame.
    const bool scaleRecognised=mismatch && prepassPlanned && frameClean && qualifiedDelta>0;
    // The camera that shares the world's near and is within the scale margin of its projection is taken for the world: H
    // must refuse on that witness, and it is the only refusal the frame's inventory holds.
    const bool closeScaleRefusal=closeScale && prepassPlanned && attemptedDelta>0 && !qualifiedDelta && !backendDelta &&
        after.firstFailureFrame==after.frame && after.firstFailureSelectedH &&
        after.failureKinds==1 && !after.failureKindsDropped &&
        std::strcmp(after.firstFailureStage,"H-qualification")==0 &&
        std::strcmp(after.firstFailureReason,"foreground-pending-null-not-selected-world")==0 &&
        std::strcmp(after.hRefusal,"foreground-pending-null-not-selected-world")==0;
    // The inert pair is drawn with camera 1 (first person) in the inert cases below except the world one. A first-person
    // draw that is not a pool mesh cannot be captured; one that cannot write depth is forwarded unchanged (counted),
    // one that writes depth refuses (inert_depth_write above).
    const bool inertNoWriteRule=inertNoWrite && surfaceDelta==1 && surfaceForeignDelta==1 &&
        foreignDelta==1 && capturedDelta==1 && frameClean;
    // Rule C: a draw that cannot write depth is forwarded unchanged: counted, never refused. Rule B: a depth-writing
    // Gbuffer draw is admitted however it blends or masks, so these two draws are planned and captured as foreign.
    const bool forwardedColorOnly=colorInert && evidence.inertColorPixels>0 && !evidence.inertDepthPixels &&
        !evidence.inert.firstFailureFrame && surfaceDelta==1 && surfaceForeignDelta==1 &&
        foreignDelta==1 && capturedDelta==1 && frameClean;
    // Both state draws (camera 1) are planned as foreign pool writers; only the one that can write depth is captured.
    const bool forwardedBlended=blendedNoDepth && evidence.stateColorPixels>0 &&
        surfaceDelta==1 && surfaceForeignDelta==1 && foreignDelta==2 && capturedDelta==1 && frameClean;
    const bool admittedState=(partialState||blendedState) && evidence.stateColorPixels>0 &&
        !surfaceDelta && foreignDelta==2 && capturedDelta==2 && frameClean;
    // A blended depth writer into the HDR light target with camera 0 is a world draw: never planned, marked or refused.
    // Only the alternate (camera 1) is foreign; the blended draw changed H's pixels.
    const bool worldHdrBlended=blendedHdrWorld && evidence.hdrPixels>0 && !surfaceDelta &&
        foreignDelta==1 && capturedDelta==1 && frameClean;
    // The inert depth writer with camera 0 (full-screen, color masked): the same unmarked, unrefused world draw.
    const bool worldInertWriter=inertWorld && evidence.inertDepthPixels>0 && !evidence.inertColorPixels &&
        !evidence.inert.firstFailureFrame && !surfaceDelta && foreignDelta==1 && capturedDelta==1 && frameClean;
    // A camera-0 depth writer outside the producer's families (the prepass pair) over the camera-1 surface, in front of it:
    // it writes depth and no mark (the plane is byte-identical around it), every first-person mark was fresh before
    // it, the ones it covers are stale after it and stay in the plane, and the frame still qualifies and resolves.
    // Marks the producer's later camera-0 HDR writer re-marks as world leave the foreign count by H, so the stale
    // marks counted at H are the ones only the prepass-pair draw overdrew.
    const bool staleMarkRule=staleMark && evidence.planeUntouched && evidence.worldWriterDepthPixels>0 &&
        evidence.marksBefore.foreign>0 && evidence.marksBefore.stale==0 &&
        evidence.marksAfter.foreign==evidence.marksBefore.foreign && evidence.marksAfter.stale>0 &&
        evidence.marksAtH.stale>0 && evidence.marksAtH.fresh>0 &&
        foreignDelta==1 && capturedDelta==1 && !surfaceDelta && frameClean && qualifiedDelta>0;
    // The supersampled on-foot frame (the HDR route's R > D): the plain `scene` draws (a world draw and a first-person one, so a mixed
    // camera) at the render size above the output, 1.5 x per axis by the targets' own sizes. H must qualify at that size and the frame
    // must be the backend's, not the spatial recovery's. The backend half is checked where the verdict is made: a WARP device cannot
    // evaluate NGX, and that case is UNSUPPORTED, not a failed rule.
    const bool supersampledSizes=supersampledAny && evidence.renderW==renderW && evidence.renderH==renderH &&
        evidence.outputW==outputW && evidence.outputH==outputH &&
        uint64_t(evidence.renderW)*2==uint64_t(evidence.outputW)*3 && uint64_t(evidence.renderH)*2==uint64_t(evidence.outputH)*3;
    const bool supersampleRule=supersampledSizes && attemptedDelta>0 && qualifiedDelta>0 && !surfaceDelta &&
        foreignDelta==1 && capturedDelta==1 && frameClean && !after.failureKinds;
    const bool supersampleBackendRan=!supersampledAny || (backendDelta>0 && !spatialDelta && !backendFailureDelta);
    const bool ruleConfirmed=worldsUnmarked && (inertNoWrite?inertNoWriteRule:colorInert?forwardedColorOnly:
        blendedNoDepth?forwardedBlended:(partialState||blendedState)?admittedState:blendedHdrWorld?worldHdrBlended:
        inertWorld?worldInertWriter:staleMark?staleMarkRule:settlement?settlementConfirmed:mismatch?scaleRecognised:
        supersampledAny?supersampleRule:true);
    const bool guardConfirmed=worldsUnmarked && (hostGuard?
        rasterReady && evidence.world.namedWorld && (taa || ownershipForeign) && after.hdrTriggered && !after.hAttempts &&
            std::strcmp(after.hdrVerdict,"engine-source-not-ready")==0:
        depthGuard?evidence.inertDepthPixels>0 && !evidence.inertColorPixels && inertWriterRefusal:
        blendedHdr?hdrSecondCameraRefusal:closeScale?closeScaleRefusal:false);
    const char* verdict="FAIL";
    const char* cause="upstream-qualification-or-raster-refused";
    const char* passCause=colorInert||blendedNoDepth?"depth-preserving-draw-forwarded":
        partialState||blendedState?"depth-writing-gbuffer-draw-admitted":
        blendedHdrWorld?"world-camera-hdr-writer-left-unmarked":inertWorld?"world-camera-depth-writer-left-unmarked":
        staleMark?"stale-first-person-mark-kept-and-frame-resolved":
        mismatch?"weapon-camera-at-world-near-captured-by-projection-scale":
        supersampledAny?"supersampled-hdr-route-qualified-and-resolved-at-render-size":
        settlement?"predicted-world-prepass-planned-without-capture":"production-hdr-resolve-completed";
    if(guardCase) {
        verdict=guardConfirmed?"PASS":"FAIL";
        cause=guardConfirmed?hostGuard?"host-guard-confirmed":depthGuard?"depth-write-refusal-confirmed":
            blendedHdr?"second-camera-hdr-writer-refused-by-selector":"close-scale-camera-taken-for-world-refusal-confirmed":
            "guard-not-confirmed";
    } else if(upstream && ruleConfirmed && after.hdrResolves && (taa || after.hdrBackendCompleted) && supersampleBackendRan) {
        verdict="PASS";cause=passCause;
    } else if(upstream && ruleConfirmed && !taa && driver==D3D_DRIVER_TYPE_WARP &&
              (std::strcmp(after.mode,"dlaa")==0 || std::strcmp(after.mode,"dlss")==0) &&
              after.backendFailures && !after.hdrBackendCompleted) {
        verdict="UNSUPPORTED";cause="configured-backend-unavailable-on-adapter";
    } else if(upstream && !ruleConfirmed)cause="expected-rule-not-observed";
    // A supersampled case that fails names where the frame stopped, in the order it meets the gates: the sizes the case planned, the H
    // attempt, H's qualification (the 64M-pixel extent cap is here: observed.lastH.reason and firstFailure name the refusal), a refusal
    // by the resolver before its prep dispatch (no GPU work was done: a gate of flatMonoResolve itself, not the backend), a backend
    // failure, and last a resolve that never reached the backend.
    if(supersampledAny && std::strcmp(verdict,"FAIL")==0) {
        const uint64_t preppedDelta=since(after.hdrPrepped,before.hdrPrepped);
        cause=!supersampledSizes?"supersampled-sizes-not-as-planned":
            !attemptedDelta?"supersampled-h-never-attempted":
            !qualifiedDelta?"supersampled-h-not-qualified":
            backendFailureDelta?"supersampled-backend-failed":
            (spatialDelta && !preppedDelta)?"supersampled-h-qualified-resolver-refused-before-the-backend":
            spatialDelta?"supersampled-h-qualified-then-spatial-recovery":
            !backendDelta?"supersampled-h-qualified-no-backend-call":
            !ruleConfirmed?"expected-rule-not-observed":cause;
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
         <<",\"selectedH\":"<<s.firstFailureSelectedH
         <<",\"stateValid\":"<<s.firstFailureState.valid
         <<",\"boundColors\":"<<s.firstFailureState.boundColors
         <<",\"component0\":"<<s.firstFailureState.slot[0].componentMask
         <<",\"writeMask0\":"<<s.firstFailureState.slot[0].effectiveWriteMask
         <<",\"blend0\":"<<s.firstFailureState.slot[0].blendEnable
         <<",\"rtvFormat0\":"<<s.firstFailureState.slot[0].viewFormat
         <<",\"budgetValid\":"<<s.firstFailureBudget.valid<<"}";
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
          <<",\"sizes\":{\"renderWidth\":"<<evidence.renderW<<",\"renderHeight\":"<<evidence.renderH
          <<",\"outputWidth\":"<<evidence.outputW<<",\"outputHeight\":"<<evidence.outputH<<"}"
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
          <<",\"stateDrawColorPixels\":"<<evidence.stateColorPixels
          // Counter changes over the measured frame, which is what a rule case claims. The prepass counters are read
          // before the world is named, so the order the settlement draws in is itself recorded.
          <<",\"measuredFrame\":{\"predictedWorld\":"<<predictedDelta
          <<",\"surfacePreserving\":"<<surfaceDelta<<",\"surfacePreservingForeign\":"<<surfaceForeignDelta
          <<",\"worldUnmarked\":"<<worldUnmarkedDelta<<",\"worldMarkers\":"<<worldMarkerDelta
          <<",\"foreignSeen\":"<<foreignDelta<<",\"captured\":"<<capturedDelta
          <<",\"hAttempts\":"<<attemptedDelta<<",\"hQualified\":"<<qualifiedDelta<<",\"backendCalls\":"<<backendDelta
          <<",\"hdrSpatial\":"<<spatialDelta<<",\"backendFailures\":"<<backendFailureDelta
          <<",\"failureKinds\":"<<after.failureKinds<<",\"failureKindsDropped\":"<<after.failureKindsDropped
          <<",\"prepass\":{\"draws\":"<<(prepassCase?prepassDraws:0u)<<",\"mistakenDraws\":"<<mistaken
          <<",\"namedWorldAfterRun\":"<<evidence.prepass.namedWorld<<",\"predictedWorld\":"<<prepassPredicted
          <<",\"foreignSeen\":"<<prepassForeign<<",\"captured\":"<<prepassCaptured
          <<",\"worldMarkers\":"<<prepassWorldMarkers<<",\"worldUnmarked\":"<<prepassUnmarked
          <<",\"depthPixelsChanged\":"<<evidence.prepassDepthPixels<<"}}"
          // The owner plane around a camera-0 depth writer drawn over a camera-1 surface (stale_foreign_mark): a mark is
          // fresh while its depth is still the pixel's raw depth, stale once something else wrote depth there.
          <<",\"staleMark\":{\"planeUntouched\":"<<(evidence.planeUntouched?"true":"false")
          <<",\"worldWriterDepthPixels\":"<<evidence.worldWriterDepthPixels
          <<",\"before\":{\"foreign\":"<<evidence.marksBefore.foreign<<",\"stale\":"<<evidence.marksBefore.stale
          <<",\"fresh\":"<<evidence.marksBefore.fresh<<"},\"after\":{\"foreign\":"<<evidence.marksAfter.foreign
          <<",\"stale\":"<<evidence.marksAfter.stale<<",\"fresh\":"<<evidence.marksAfter.fresh
          <<"},\"atH\":{\"foreign\":"<<evidence.marksAtH.foreign<<",\"stale\":"<<evidence.marksAtH.stale
          <<",\"fresh\":"<<evidence.marksAtH.fresh<<"}}"
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
          <<",\"ruleConfirmed\":"<<(ruleConfirmed?"true":"false")
          <<"},\"limitations\":[\"Controlled geometry, camera and OM state reconstructed; shader bytes captured\"";
    if(!hostGuard)result<<",\"Emit-hook availability reconstructed only in offline test-link proxy\"";
    if(prepassCase)result<<",\"Prepass order, shader pair and run length follow the 2026-10 settlement census; geometry, record contents and the alpha-test inputs are reconstructed\"";
    if(staleMark)result<<",\"The covering world surface is a reconstructed small pool record drawn with the prepass pair; the owner plane is read back through the proxy's test export\"";
    if(supersampledAny)result<<",\"The scene targets are rendered above the swap chain's size (the HDR route's R > D): 1.5 x per axis; the game's own final copy that would downsample the result is not part of the bench\"";
    if(supersampled4k)result<<",\"Hardware adapter only: 5760 x 3240 into 3840 x 2160 is impractical on WARP\"";
    result<<"]}";
    std::puts(result.str().c_str());
    return std::strcmp(verdict,"PASS")==0?0:std::strcmp(verdict,"UNSUPPORTED")==0?2:1;
}
