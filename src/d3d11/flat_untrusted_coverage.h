#pragma once

// Conservative coverage of raster fragments from a camera other than H's
// world camera. The original PS writes MRT7 after its own discard and the
// game's depth/stencil tests. The mask is cleared once per frame and ORed by
// subsequent draws; a later world overdraw deliberately does not erase it.
#include "flat_overlay_layer.h"
#include "flat_mono_frame.h"
#include "flat_camera_phase.h"
#include <array>
#include <cstring>
#include <string>

namespace edvr {
class FlatUntrustedCoverage {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    struct Draw {
        const void* color = nullptr;
        const void* depth = nullptr;
        const void* dsv = nullptr;
        uint64_t vs = 0, ps = 0;
        uint32_t sequence = 0;
        unsigned char camera[kFlatCameraBytes]{};
        bool completed = false;
    };
    static constexpr uint32_t kMaxDraws = 128;
    FlatOverlayLayer layer_;
    std::array<Draw,kMaxDraws> draws_{};
    Ptr<ID3D11Texture2D> color_, depth_;
    Ptr<ID3D11DepthStencilView> dsv_;
    uint64_t frame_ = 0;
    uint32_t count_ = 0, open_ = kMaxDraws;
    uint32_t width_ = 0, height_ = 0;
    bool consumerSeen_ = false;
    std::string failure_;
public:
    void reset() {
        layer_.reset(); color_.Reset(); depth_.Reset(); dsv_.Reset();
        frame_=0; count_=0; open_=kMaxDraws; width_=height_=0; consumerSeen_=false; failure_.clear();
    }
    void beginFrame(uint64_t frame) {
        if(frame_==frame)return;
        layer_.beginFrame(frame); color_.Reset(); depth_.Reset(); dsv_.Reset();
        frame_=frame; count_=0; open_=kMaxDraws; width_=height_=0; consumerSeen_=false; failure_.clear();
    }
    void invalidate(const char* reason) {
        if(consumerSeen_)return;
        if(failure_.empty())failure_=reason?reason:"untrusted-coverage-invalid";
    }
    void noteMutation(ID3D11Resource* resource) {
        if(!consumerSeen_ && count_ && (!resource || resource==depth_.Get() || resource==color_.Get()))
            invalidate("untrusted-source-explicit-mutation");
    }
    void consumer() { consumerSeen_=true; }
    bool active() const { return count_!=0; }
    bool finished() const { return consumerSeen_; }
    bool plan(uint64_t frame,uint32_t sequence,ID3D11Texture2D* color,
              ID3D11Texture2D* depth,ID3D11DepthStencilView* dsv,
              uint64_t vs,uint64_t ps,const unsigned char* camera) {
        if(frame!=frame_ || !color || !depth || !dsv || !camera || !sequence || !vs || !ps) {
            invalidate("untrusted-source-identity");return false;
        }
        if(!count_) {
            D3D11_TEXTURE2D_DESC cd{},dd{};color->GetDesc(&cd);depth->GetDesc(&dd);
            if(!cd.Width || !cd.Height || cd.Width!=dd.Width || cd.Height!=dd.Height ||
               cd.SampleDesc.Count!=1 || dd.SampleDesc.Count!=1 ||
               uint64_t(cd.Width)*cd.Height>128ull*1024*1024) {
                invalidate("untrusted-source-shape");return false;
            }
            color_=color;depth_=depth;dsv_=dsv;width_=cd.Width;height_=cd.Height;
        }
        if(color_.Get()!=color || depth_.Get()!=depth || dsv_.Get()!=dsv ||
           count_>=kMaxDraws || !failure_.empty()) {
            invalidate("untrusted-source-resource-or-cap");return false;
        }
        Draw& draw=draws_[count_++];draw={};
        draw.color=color;draw.depth=depth;draw.dsv=dsv;draw.vs=vs;draw.ps=ps;
        draw.sequence=sequence;std::memcpy(draw.camera,camera,sizeof(draw.camera));
        return true;
    }
    bool beginDraw(ID3D11DeviceContext* ctx,uint64_t frame) {
        if(!ctx || frame!=frame_ || !count_ || open_!=kMaxDraws || !failure_.empty())return false;
        const char* reason=nullptr;
        if(!layer_.beginDraw(ctx,frame,color_.Get(),dsv_.Get(),&reason,true,true,true)) {
            invalidate(reason?reason:"untrusted-private-MRT-refused");return false;
        }
        open_=count_-1;
        return true;
    }
    void endDraw(ID3D11DeviceContext* ctx,bool completed=true) {
        if(open_==kMaxDraws)return;
        layer_.endDraw(ctx);
        if(completed && ctx && !layer_.refusal())draws_[open_].completed=true;
        else invalidate("untrusted-draw-incomplete");
        open_=kMaxDraws;
    }
    bool qualifies(const FlatContractRecord& record,const unsigned char* worldBytes,
                   float phaseX,float phaseY) const {
        const auto& k=record.key;
        if(!frame_ || !count_ || !failure_.empty() || open_!=kMaxDraws ||
           k.depth!=depth_.Get() || k.dsv!=dsv_.Get() ||
           k.width!=width_ || k.height!=height_ || !k.camera || !worldBytes ||
           !layer_.coverageReady(frame_,color_.Get()))return false;
        float world[6][4]{},alternate[6][4]{};
        if(!flat_mono_detail::cameraShape(worldBytes,world) ||
           !flat_mono_detail::cameraShape(k.camera,alternate) ||
           !flatCameraCenteredPairAtPhase(world,alternate,phaseX,phaseY,width_,height_))return false;
        uint32_t matching=0;
        for(uint32_t i=0;i<count_;++i) {
            const Draw& draw=draws_[i];
            if(draw.sequence<record.first || draw.sequence>record.last ||
               draw.color!=k.color || draw.depth!=k.depth || draw.dsv!=k.dsv ||
               draw.vs!=k.vs || draw.ps!=k.ps ||
               std::memcmp(draw.camera,k.camera,sizeof(draw.camera))!=0)continue;
            if(!draw.completed)return false;
            ++matching;
        }
        return matching==record.draws;
    }
    ID3D11ShaderResourceView* view() const {
        return failure_.empty() && open_==kMaxDraws &&
            layer_.coverageReady(frame_,color_.Get())?layer_.coverageView():nullptr;
    }
    const char* failure() const { return failure_.empty()?nullptr:failure_.c_str(); }
    uint32_t drawCount() const { return count_; }
    ID3D11Texture2D* depth() const { return depth_.Get(); }
};
} // namespace edvr
