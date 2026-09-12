// Optional local-only replay of original VS bytecode, packed mesh windows
// and three draw-time scene/pool states from an explicit eye capture.
// The fixture contains game assets and is never committed or distributed.
void replayMeshes(ID3D11Device* dev,ID3D11DeviceContext* ctx,const char* path,bool benchmark){
    using namespace edvr::mesh_motion_detail;
    std::ifstream in(path,std::ios::binary);auto take=[&](void* p,size_t bytes){in.read(static_cast<char*>(p),std::streamsize(bytes));check(bool(in),"complete replay input");};
    char magic[8];UINT cases=0;take(magic,8);take(&cases,4);check(!std::memcmp(magic,"EDVRMRP1",8) && cases<=64,"mesh replay header");
    auto buffer=[&](UINT bytes,UINT bind,const void* data=nullptr,UINT step=0){D3D11_BUFFER_DESC b{};b.ByteWidth=bytes;b.BindFlags=bind;b.StructureByteStride=step;b.MiscFlags=step?D3D11_RESOURCE_MISC_BUFFER_STRUCTURED:0;D3D11_SUBRESOURCE_DATA s{};s.pSysMem=data;ComPtr<ID3D11Buffer> r;hr(dev->CreateBuffer(&b,data?&s:nullptr,&r));return r;};
    unsigned checked=0;double maximum=0;
    for(UINT sample=0;sample<cases;++sample){
        ctx->ClearState();meshMotionShutdown();uint64_t hash=0;UINT size[3]{};take(&hash,8);take(size,sizeof(size));
        check(size[0]>0 && size[0]<65536 && size[1]<=262144 && size[2]<=262144 && size[2]%12==0,"bounded replay assets");
        std::vector<char> code(size[0]),vertices(size[1]),indices(size[2]);take(code.data(),code.size());take(vertices.data(),vertices.size());take(indices.data(),indices.size());
        auto vb=buffer(size[1],D3D11_BIND_VERTEX_BUFFER,vertices.data()),ib=buffer(size[2],D3D11_BIND_INDEX_BUFFER,indices.data());UINT ids[2]{};
        auto instanceBuffer=buffer(8,D3D11_BIND_VERTEX_BUFFER,ids),pool=buffer(336,D3D11_BIND_SHADER_RESOURCE,nullptr,336),sceneCb=buffer(276*16,D3D11_BIND_CONSTANT_BUFFER);
        ComPtr<ID3D11ShaderResourceView> poolView;hr(dev->CreateShaderResourceView(pool.Get(),nullptr,&poolView));
        ComPtr<ID3D11VertexShader> vs;hr(dev->CreateVertexShader(code.data(),code.size(),nullptr,&vs));
        D3D11_INPUT_ELEMENT_DESC el[4]={{"INSTANCEANDMODELDATAINDEX",0,DXGI_FORMAT_R32G32_UINT,0,0,D3D11_INPUT_PER_INSTANCE_DATA,1},{"PACKEDVERTEXDATAA",0,DXGI_FORMAT_R32G32B32A32_UINT,1,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"PACKEDVERTEXDATAB",0,DXGI_FORMAT_R32G32B32A32_UINT,1,16,D3D11_INPUT_PER_VERTEX_DATA,0},{"PACKEDVERTEXDATAC",0,DXGI_FORMAT_R32G32_UINT,1,32,D3D11_INPUT_PER_VERTEX_DATA,0}};
        ComPtr<ID3D11InputLayout> layout;hr(dev->CreateInputLayout(el,4,code.data(),code.size(),&layout));
        D3D11_SO_DECLARATION_ENTRY entry{0,"SV_POSITION",0,0,4,0};UINT soStride=16;ComPtr<ID3D11GeometryShader> so;
        hr(dev->CreateGeometryShaderWithStreamOutput(code.data(),code.size(),&entry,1,&soStride,1,D3D11_SO_NO_RASTERIZED_STREAM,nullptr,&so));
        UINT count=size[2]/4;auto positions=buffer(count*16,D3D11_BIND_STREAM_OUTPUT);
        constexpr UINT W=2774,H=2740;D3D11_TEXTURE2D_DESC td{};td.Width=W;td.Height=H;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R32_TYPELESS;td.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        ComPtr<ID3D11Texture2D> scene;ComPtr<ID3D11DepthStencilView> dsv;hr(dev->CreateTexture2D(&td,nullptr,&scene));D3D11_DEPTH_STENCIL_VIEW_DESC dd{};dd.Format=DXGI_FORMAT_D32_FLOAT;dd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;hr(dev->CreateDepthStencilView(scene.Get(),&dd,&dsv));
        D3D11_DEPTH_STENCIL_DESC depth{};depth.DepthEnable=TRUE;depth.DepthFunc=D3D11_COMPARISON_GREATER_EQUAL;depth.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;ComPtr<ID3D11DepthStencilState> ds;hr(dev->CreateDepthStencilState(&depth,&ds));
        D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;ComPtr<ID3D11RasterizerState> raster;hr(dev->CreateRasterizerState(&rd,&raster));
        std::vector<float> previous;
        for(unsigned frame=0;frame<3;++frame){
            if(frame)meshMotionFrameBoundary(ctx);float sc[276][4];UINT p[84];take(sc,sizeof(sc));take(p,sizeof(p));check(p[0]==0,"replay is a rigid mesh");
            testEye=0;testScene=scene.Get();testDepth=dsv.Get();ctx->OMSetRenderTargets(0,nullptr,dsv.Get());ctx->OMSetDepthStencilState(ds.Get(),0);ctx->OMSetBlendState(nullptr,nullptr,~0u);ctx->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,0,0);
            ctx->RSSetState(raster.Get());D3D11_VIEWPORT vp{0,0,float(W),float(H),0,1};ctx->RSSetViewports(1,&vp);ctx->VSSetShader(vs.Get(),nullptr,0);ctx->PSSetShader(nullptr,nullptr,0);ctx->IASetInputLayout(layout.Get());
            ID3D11Buffer* vb2[2]={instanceBuffer.Get(),vb.Get()};UINT strides[2]={8,40},offsets[2]{};ctx->IASetVertexBuffers(0,2,vb2,strides,offsets);ctx->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R32_UINT,0);
            ctx->UpdateSubresource(pool.Get(),0,nullptr,p,0,0);ctx->UpdateSubresource(sceneCb.Get(),0,nullptr,sc,0,0);ctx->VSSetShaderResources(33,1,poolView.GetAddressOf());ctx->VSSetConstantBuffers(1,1,sceneCb.GetAddressOf());
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);ctx->GSSetShader(so.Get(),nullptr,0);UINT zero=0;ctx->SOSetTargets(1,positions.GetAddressOf(),&zero);issue(ctx,count,1,0,0,0);ctx->SOSetTargets(0,nullptr,nullptr);ctx->GSSetShader(nullptr,nullptr,0);
            auto current=readBuffer(dev,ctx,positions.Get());
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);issue(ctx,count,1,0,0,0);meshMotionDraw(ctx,issue,count,1,0,0,0,hash);
            ID3D11ShaderResourceView* views[2]{};meshMotionViews(ctx,scene.Get(),views);check(views[1]!=nullptr,"original shader captured");auto record=readBuffer(dev,ctx,eyes[0].history[eyes[0].write].buffer.Get());
            // Transform-only replay missed material families whose visible
            // surfaces had no coverage. Compare every original depth sample
            // with the reissue, including the actual VS-to-PS register link.
            auto depthPixels=readTexture(dev,ctx,scene.Get()),coverage=readTexture(dev,ctx,eyes[0].coverage.Get());
            unsigned visible=0,covered=0;
            for(unsigned i=0;i<W*H;++i)if(depthPixels[i]>0){++visible;covered+=coverage[i*2]==1 && coverage[i*2+1]==depthPixels[i];}
            std::printf("original raster %u frame %u: %u/%u depth samples covered\n",sample,frame,covered,visible);
            check(covered==visible,"coverage matches every original raster depth sample");
            if(frame){
                check(record[59]==1,"original draw transforms matched");double largest=0;unsigned tested=0;
                for(unsigned i=0;i<count;++i){auto* q=current.data()+i*4;auto* old=previous.data()+i*4;if(q[3]<=.025f || old[3]<=.025f || std::fabs(q[0])>q[3] || std::fabs(q[1])>q[3])continue;
                    double v[3]={q[0],q[1],q[3]},before[3]{};for(int r=0;r<3;++r){before[r]=record[47+r*4];for(int j=0;j<3;++j)before[r]+=record[44+r*4+j]*v[j];}
                    double dx=(before[0]/before[2]-old[0]/old[3])*W/2,dy=(before[1]/before[2]-old[1]/old[3])*H/2;largest=std::max(largest,std::sqrt(dx*dx+dy*dy));++tested;
                }
                std::printf("original replay %u frame %u: %u vertices, max error %.6f px\n",sample,frame,tested,largest);
                if(tested){check(largest<.003,"GPU history matches original VS within 0.003 input pixel");maximum=std::max(maximum,largest);checked+=tested;}
            }
            previous=std::move(current);
        }
        if(benchmark && sample==4){
            // Deliberately repeated hull geometry measures the added pass,
            // not the game's shading. No claim that this is flight timing.
            LARGE_INTEGER freq{},begin{},finish{};QueryPerformanceFrequency(&freq);double cpuMs=0;
            for(unsigned frame=0;frame<100;++frame){
                meshMotionFrameBoundary(ctx);ctx->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,0,0);issue(ctx,count,1,0,0,0);
                QueryPerformanceCounter(&begin);
                for(unsigned i=0;i<32;++i)meshMotionDraw(ctx,issue,count,1,0,0,0,hash);
                ID3D11ShaderResourceView* views[2]{};meshMotionViews(ctx,scene.Get(),views);
                QueryPerformanceCounter(&finish);cpuMs+=double(finish.QuadPart-begin.QuadPart)*1000/freq.QuadPart;
            }
            // Finish solely in this offline benchmark, then collect queued
            // timestamps; the production path never performs this readback.
            readBuffer(dev,ctx,positions.Get());for(int i=0;i<8;++i)meshMotionFrameBoundary(ctx);
            auto d=drawGpu.totals,m=matchGpu.totals;
            std::printf("32-draw hull stress: CPU %.3f ms/eye, sampled capture+reissue %.3f us/draw (%u samples), match %.3f us/eye (%u samples)\n",cpuMs/100,d.samples?d.ms*1000/d.samples:0,d.samples,m.samples?m.ms*1000/m.samples:0,m.samples);
            check(d.samples>0 && m.samples>0,"hardware benchmark timestamps available");
        }
    }
    check(checked>0,"visible original vertices tested");std::printf("original mesh replay: %u vertices, max %.6f input pixel\n",checked,maximum);
}
