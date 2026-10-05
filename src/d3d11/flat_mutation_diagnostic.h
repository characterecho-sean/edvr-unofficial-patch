#pragma once
#include <d3d11.h>
#include <cstring>

namespace edvr {
enum class FlatOverlayMutationOp : unsigned char {
    Map, Unmap, ClearRtv, ClearDsv, ClearUav, GenerateMips, CopyResource,
    CopyRegion, CopyStructureCount, UpdateSubresource, Resolve, Written
};
inline const char* flatMutationOpName(FlatOverlayMutationOp op) {
    switch(op) {
    case FlatOverlayMutationOp::Map:return "map";
    case FlatOverlayMutationOp::Unmap:return "unmap";
    case FlatOverlayMutationOp::ClearRtv:return "clear-rtv";
    case FlatOverlayMutationOp::ClearDsv:return "clear-dsv";
    case FlatOverlayMutationOp::ClearUav:return "clear-uav";
    case FlatOverlayMutationOp::GenerateMips:return "generate-mips";
    case FlatOverlayMutationOp::CopyResource:return "copy-resource";
    case FlatOverlayMutationOp::CopyRegion:return "copy-region";
    case FlatOverlayMutationOp::CopyStructureCount:return "copy-structure-count";
    case FlatOverlayMutationOp::UpdateSubresource:return "update-subresource";
    case FlatOverlayMutationOp::Resolve:return "resolve";
    case FlatOverlayMutationOp::Written:return "written";
    }
    return "unknown";
}

// Owned scalar payload only. Object addresses are identities, never dereferenced
// by the retained report. entry must be a static literal supplied by the caller.
struct FlatMutationDetails {
    enum Known : UINT { View=1,Source=2,SourceView=4,Flags=8,Depth=16,Stencil=32,
        MapType=64,SrcSub=128,DstSub=256,DstXYZ=512,Box=1024,RowPitch=2048,
        DepthPitch=4096,Format=8192,Values=16384 };
    FlatOverlayMutationOp op=FlatOverlayMutationOp::Written;
    const char* entry="unspecified-write";
    const void* view=nullptr;
    const void* source=nullptr;
    const void* sourceView=nullptr;
    UINT flags=0,stencil=0,mapType=0,srcSub=0,dstSub=0;
    UINT known=0;
    UINT dstX=0,dstY=0,dstZ=0,rowPitch=0,depthPitch=0,format=0;
    float depth=0;
    UINT values[4]{}; // Exact float/uint clear bits; entry identifies the API.
    bool hasBox=false,hasValues=false;
    D3D11_BOX box{};
    static FlatMutationDetails named(FlatOverlayMutationOp op,const char* entry) {
        FlatMutationDetails d;d.op=op;d.entry=entry;return d;
    }
    static FlatMutationDetails clear(FlatOverlayMutationOp op,const char* entry,
                                     const void* values) {
        auto d=named(op,entry);d.hasValues=values!=nullptr;d.known=Values;
        if(values)std::memcpy(d.values,values,sizeof(d.values));return d;
    }
    static FlatMutationDetails clearDepth(UINT flags,float depth,UINT stencil) {
        auto d=named(FlatOverlayMutationOp::ClearDsv,"ClearDepthStencilView");
        d.flags=flags;d.depth=depth;d.stencil=stencil;d.known=Flags|Depth|Stencil;return d;
    }
    static FlatMutationDetails transfer(FlatOverlayMutationOp op,const char* entry,
        const void* source,UINT srcSub=0,UINT dstSub=0,const D3D11_BOX* box=nullptr,
        UINT x=0,UINT y=0,UINT z=0) {
        auto d=named(op,entry);d.source=source;d.srcSub=srcSub;d.dstSub=dstSub;
        if(op==FlatOverlayMutationOp::CopyResource)d.known=Source;
        if(op==FlatOverlayMutationOp::CopyRegion)d.known=Source|SrcSub|DstSub|DstXYZ|Box;
        if(op==FlatOverlayMutationOp::UpdateSubresource)d.known=DstSub|Box;
        if(op==FlatOverlayMutationOp::Resolve)d.known=Source|SrcSub|DstSub;
        d.hasBox=box!=nullptr;if(box)d.box=*box;d.dstX=x;d.dstY=y;d.dstZ=z;return d;
    }
};
inline bool flatMutationSameSignature(const FlatMutationDetails& a,const FlatMutationDetails& b) {
    // Keep changing scalar payloads distinct; the bounded overflow counter makes
    // omissions explicit rather than presenting an incomplete event list as full.
    return a.op==b.op && a.known==b.known && std::strcmp(a.entry,b.entry)==0 && a.view==b.view &&
        a.source==b.source && a.sourceView==b.sourceView && a.flags==b.flags &&
        a.stencil==b.stencil && a.mapType==b.mapType && a.srcSub==b.srcSub &&
        a.dstSub==b.dstSub && a.dstX==b.dstX && a.dstY==b.dstY && a.dstZ==b.dstZ &&
        a.rowPitch==b.rowPitch && a.depthPitch==b.depthPitch && a.format==b.format &&
        std::memcmp(&a.depth,&b.depth,sizeof(float))==0 && a.hasBox==b.hasBox &&
        (!a.hasBox || std::memcmp(&a.box,&b.box,sizeof(a.box))==0) &&
        a.hasValues==b.hasValues && (!a.hasValues || std::memcmp(a.values,b.values,sizeof(a.values))==0);
}
}
