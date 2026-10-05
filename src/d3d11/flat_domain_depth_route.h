#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace edvr {
// A frame may touch several native-sized depth resources before the scene is
// known. Keep their evidence separate and select only by the H resource.
// Slots retain the preceding frame's resource for motion-history continuity.
template<std::size_t Capacity=4> class FlatDomainDepthRoute {
public:
    struct Observation {int slot=-1;bool created=false;};
    void clear(){slots_={};frame_=~uint64_t{0};overflow_=false;unknownMutation_=false;pinned_=nullptr;}
    void beginFrame(uint64_t frame){if(frame_!=frame){frame_=frame;overflow_=false;unknownMutation_=false;}}
    void pin(const void* depth){pinned_=depth;}
    void noteUnknownMutation(uint64_t frame){beginFrame(frame);unknownMutation_=true;}
    bool unknownMutation(uint64_t frame)const{return frame==frame_ && unknownMutation_;}
    Observation observe(const void* depth,uint64_t frame) {
        beginFrame(frame);
        if(!depth)return {};
        for(std::size_t i=0;i<Capacity;++i)if(slots_[i].depth==depth) {
            slots_[i].lastFrame=frame;return {static_cast<int>(i),false};
        }
        std::size_t choice=Capacity;
        for(std::size_t i=0;i<Capacity;++i)if(!slots_[i].depth){choice=i;break;}
        if(choice==Capacity)for(std::size_t i=0;i<Capacity;++i)
            if(slots_[i].depth!=pinned_ && slots_[i].lastFrame!=frame &&
               (choice==Capacity || slots_[i].lastFrame<slots_[choice].lastFrame))choice=i;
        if(choice==Capacity){overflow_=true;return {};}
        slots_[choice]={depth,frame};return {static_cast<int>(choice),true};
    }
    int selected(const void* depth,uint64_t frame)const {
        if(frame!=frame_ || !depth)return -1;
        for(std::size_t i=0;i<Capacity;++i)
            if(slots_[i].depth==depth && slots_[i].lastFrame==frame)return static_cast<int>(i);
        return -1;
    }
    const void* depth(std::size_t slot)const{return slots_[slot].depth;}
    unsigned count(uint64_t frame)const {
        if(frame!=frame_)return 0;
        unsigned n=0;for(const auto& slot:slots_)n+=slot.depth && slot.lastFrame==frame;return n;
    }
    bool overflowed(uint64_t frame)const{return frame==frame_ && overflow_;}
private:
    struct Slot {const void* depth=nullptr;uint64_t lastFrame=~uint64_t{0};};
    std::array<Slot,Capacity> slots_{};
    uint64_t frame_=~uint64_t{0};bool overflow_=false,unknownMutation_=false;const void* pinned_=nullptr;
};
} // namespace edvr
