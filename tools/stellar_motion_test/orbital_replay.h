#pragma once
// Optional comparison against the game's bytecode; game assets stay local.
void verifyOrbitalVertices(ID3D11Device* dev,ID3D11DeviceContext* ctx,const char* path){
    std::ifstream in(path,std::ios::binary);UINT frames,bytes;in.read(reinterpret_cast<char*>(&frames),4);in.read(reinterpret_cast<char*>(&bytes),4);
    check(frames>0&&frames<=64&&bytes<65536,"orbital vertex fixture header");std::vector<char> original(bytes);in.read(original.data(),bytes);
    auto replacement=compile(edvr::kOrbitalCoverageVs,"vs_5_0");
    ComPtr<ID3D11VertexShader> vs[2];ComPtr<ID3D11GeometryShader> so[2];ComPtr<ID3D11InputLayout> layout;
    D3D11_INPUT_ELEMENT_DESC elements[]={{"POSTANGENT",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"OSTOWST",0,DXGI_FORMAT_R32G32B32A32_FLOAT,1,0,D3D11_INPUT_PER_INSTANCE_DATA,1},
        {"OSTOWSR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,1,16,D3D11_INPUT_PER_INSTANCE_DATA,1},
        {"OSTOWSS",0,DXGI_FORMAT_R32G32B32_FLOAT,1,32,D3D11_INPUT_PER_INSTANCE_DATA,1},
        {"COLOUR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,1,44,D3D11_INPUT_PER_INSTANCE_DATA,1}};
    D3D11_SO_DECLARATION_ENTRY declaration[]={{0,"SV_POSITION",0,0,4,0},{0,"__USER_STELLARVERTEX_STABLESIGNEDUNITDISTANCEPERSPECTIVE",0,0,1,0}};UINT stride=20;
    for(int i=0;i<2;++i){const void* p=i?replacement->GetBufferPointer():original.data();SIZE_T n=i?replacement->GetBufferSize():original.size();
        hr(dev->CreateVertexShader(p,n,nullptr,&vs[i]));hr(dev->CreateGeometryShaderWithStreamOutput(p,n,declaration,2,&stride,1,D3D11_SO_NO_RASTERIZED_STREAM,nullptr,&so[i]));}
    hr(dev->CreateInputLayout(elements,5,original.data(),original.size(),&layout));
    auto buffer=[&](UINT size,UINT bind,const void* data,D3D11_USAGE use=D3D11_USAGE_DEFAULT){
        D3D11_BUFFER_DESC d{};d.ByteWidth=size;d.BindFlags=bind;d.Usage=use;d.CPUAccessFlags=use==D3D11_USAGE_STAGING?D3D11_CPU_ACCESS_READ:0;
        D3D11_SUBRESOURCE_DATA init{data,0,0};ComPtr<ID3D11Buffer> b;hr(dev->CreateBuffer(&d,data?&init:nullptr,&b));return b;};
    float maximum=0;unsigned visible=0;
    for(UINT f=0;f<frames;++f){
        UINT count;in.read(reinterpret_cast<char*>(&count),4);check(count>0&&count<=64,"orbital vertex count");
        std::vector<float> scene(333*4),vertices(8194*4),instances(count*15);in.read(reinterpret_cast<char*>(scene.data()),scene.size()*4);in.read(reinterpret_cast<char*>(vertices.data()),vertices.size()*4);in.read(reinterpret_cast<char*>(instances.data()),instances.size()*4);check(bool(in),"complete orbital vertex fixture");
        auto cb=buffer(UINT(scene.size()*4),D3D11_BIND_CONSTANT_BUFFER,scene.data()),vb0=buffer(UINT(vertices.size()*4),D3D11_BIND_VERTEX_BUFFER,vertices.data()),vb1=buffer(UINT(instances.size()*4),D3D11_BIND_VERTEX_BUFFER,instances.data());
        auto output=buffer(8194*count*20,D3D11_BIND_STREAM_OUTPUT,nullptr),stage=buffer(8194*count*20,0,nullptr,D3D11_USAGE_STAGING);
        UINT info[4]={0,0,2,count};auto motionInfo=buffer(16,D3D11_BIND_CONSTANT_BUFFER,info);std::vector<float> results[2];
        for(int i=0;i<2;++i){
            ctx->ClearState();ctx->IASetInputLayout(layout.Get());ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
            ID3D11Buffer* vb[]={vb0.Get(),vb1.Get()};UINT strides[]={16,60},offsets[]={0,0};ctx->IASetVertexBuffers(0,2,vb,strides,offsets);
            ctx->VSSetShader(vs[i].Get(),nullptr,0);ctx->VSSetConstantBuffers(1,1,cb.GetAddressOf());ctx->VSSetConstantBuffers(12,1,motionInfo.GetAddressOf());ctx->GSSetShader(so[i].Get(),nullptr,0);
            UINT zero=0;ctx->SOSetTargets(1,output.GetAddressOf(),&zero);ctx->DrawInstanced(8194,count,0,0);ctx->ClearState();ctx->CopyResource(stage.Get(),output.Get());
            D3D11_MAPPED_SUBRESOURCE map{};hr(ctx->Map(stage.Get(),0,D3D11_MAP_READ,0,&map));results[i].resize(8194*count*5);std::memcpy(results[i].data(),map.pData,results[i].size()*4);ctx->Unmap(stage.Get(),0);
        }
        for(size_t n=0;n<results[0].size();n+=5){const float* a=results[0].data()+n;const float* b=results[1].data()+n;
            if(a[3]<=0||std::fabs(a[0])>a[3]*1.2f||std::fabs(a[1])>a[3]*1.2f)continue;++visible;
            for(int k=0;k<2;++k){float error=std::fabs(a[k]/a[3]-b[k]/b[3])*1134;if(error>maximum)maximum=error;if(error>=.03f)std::printf("orbital vertex %zu error %.6f\n",n/5,error);check(error<.03f,"replacement orbital vertex agrees with game");}
            check(std::fabs(a[2]-b[2])<1e-9f&&std::fabs(a[3]-b[3])<std::fabs(a[3])*1e-5f,"orbital depth preserved");
        }
    }
    check(visible>0,"replay included visible orbital vertices");std::printf("orbital VS: %u visible vertices, maximum %.6f input pixels against original game shader\n",visible,maximum);
}
