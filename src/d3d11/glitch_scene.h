#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace edvr {

// A bounded, read-only sample of the pool used by the recognised eye draw.
// Its decision is used only after a fresh recognised eye draw, before Submit.
struct GlitchSceneGeometry {
    uint32_t matched=0, predicted=0;
    float cameraStep=0, poolStep=0, relativeMedian=0, relativeP90=0, predictionP90=0;
};

enum class GlitchSceneDecision { Unknown, Coherent, CameraReset };

inline GlitchSceneDecision glitchSceneDecision(const GlitchSceneGeometry& g,const float* camera) {
    // A population, not one possibly recycled object. Three fresh frames are
    // required. The 17:34 flight has 128 matches in external view, 59/65 at
    // the two high-wake resets. Missing evidence leaves the legacy path alone.
    if(g.matched<32 || g.predicted<32 || !camera)return GlitchSceneDecision::Unknown;
    if(!std::isfinite(g.relativeP90) || !std::isfinite(g.predictionP90) ||
       !std::isfinite(g.relativeMedian) || !std::isfinite(g.cameraStep))return GlitchSceneDecision::Unknown;
    double radius2=0;
    for(unsigned a=0;a<3;++a){if(!std::isfinite(camera[a]))return GlitchSceneDecision::Unknown;radius2+=double(camera[a])*camera[a];}
    // Shared translations cancel to within floating-point noise (<=1 mm in
    // the measured 5 km rebases). A centimetre allows quantisation, not motion.
    if(g.relativeP90<=.01f && g.predictionP90<=.01f)return GlitchSceneDecision::Coherent;
    // The eye camera collapses into head space while its objects do not.
    // This catches the 13.505 m reset that the auxiliary-camera 250 m floor
    // missed. Ordinary origin rebases took the coherent branch above.
    // Large object motion elsewhere is insufficient: the camera must reset
    // into the one-metre head volume and a majority must disagree by >1 m.
    if(radius2<1 && g.cameraStep>1 && g.relativeMedian>1 && g.predictionP90>1)
        return GlitchSceneDecision::CameraReset;
    return GlitchSceneDecision::Unknown;
}

namespace glitch_scene_detail {
constexpr unsigned kSamples=128, kPoolStride=336;
struct Point {
    uint32_t key[3]{};
    float pos[3]{};
    bool valid=false;
};
struct Sample {
    Point points[kSamples]{};
    float camera[3]{};
    uint32_t frame=~0u;
    bool valid=false;
};
inline void readPool(Sample& out,const void* data,uint32_t bytes,uint32_t frame) {
    out=Sample{};
    if(!data || bytes%kPoolStride)return;
    out.frame=frame;out.valid=true;
    unsigned count=bytes/kPoolStride;if(count>kSamples)count=kSamples;
    for(unsigned i=0;i<count;++i){
        const auto* record=static_cast<const unsigned char*>(data)+i*kPoolStride;
        uint32_t head[8]{},tag=0;
        // Two short reads per record, not a copy of the entire dynamic pool.
        std::memcpy(head,record,sizeof(head));std::memcpy(&tag,record+320,4);
        float scale=0;std::memcpy(&scale,head+1,4);
        auto& p=out.points[i];std::memcpy(p.pos,head+4,sizeof(p.pos));
        p.key[0]=head[7];p.key[1]=tag;p.key[2]=head[1];
        double norm=0;
        for(unsigned a=0;a<4;++a){double q=((head[2+a/2]>>((a%2)*16))&65535)/32767.0-1;norm+=q*q;}
        p.valid=head[0]==0 && (head[7]||tag) && std::isfinite(scale) && std::fabs(scale)>1e-8 &&
            std::fabs(norm-1)<.002 && std::isfinite(p.pos[0]) && std::isfinite(p.pos[1]) && std::isfinite(p.pos[2]);
    }
}
inline bool same(const Point& a,const Point& b){
    return a.valid && b.valid && std::memcmp(a.key,b.key,sizeof(a.key))==0;
}
inline GlitchSceneGeometry compare(const Sample& now,const Sample& prev,const Sample& older){
    GlitchSceneGeometry result{};
    if(!now.valid || !prev.valid || prev.frame+1!=now.frame)return result;
    double camera2=0;for(unsigned a=0;a<3;++a){double d=double(now.camera[a])-prev.camera[a];camera2+=d*d;}
    result.cameraStep=float(std::sqrt(camera2));
    float relative[kSamples]{},prediction[kSamples]{},pool[kSamples]{};
    for(unsigned i=0;i<kSamples;++i){
        const auto& n=now.points[i];const auto& p=prev.points[i];const auto& o=older.points[i];
        if(!same(n,p))continue;
        const bool predict=older.valid && older.frame+1==prev.frame && same(p,o);
        double relative2=0,pool2=0,prediction2=0;
        for(unsigned a=0;a<3;++a){
            double step=double(n.pos[a])-p.pos[a];pool2+=step*step;
            double delta=step-(double(now.camera[a])-prev.camera[a]);relative2+=delta*delta;
            if(predict){double before=(double(p.pos[a])-o.pos[a])-(double(prev.camera[a])-older.camera[a]);
                double error=delta-before;prediction2+=error*error;}
        }
        pool[result.matched]=float(std::sqrt(pool2));
        relative[result.matched++]=float(std::sqrt(relative2));
        if(predict)prediction[result.predicted++]=float(std::sqrt(prediction2));
    }
    if(result.matched){
        std::sort(pool,pool+result.matched);std::sort(relative,relative+result.matched);
        result.poolStep=pool[result.matched/2];result.relativeMedian=relative[result.matched/2];
        result.relativeP90=relative[(result.matched-1)*9/10];
    }
    if(result.predicted){std::sort(prediction,prediction+result.predicted);result.predictionP90=prediction[(result.predicted-1)*9/10];}
    return result;
}
} // namespace glitch_scene_detail
} // namespace edvr
