#pragma once

// F2: the GPU half of the identity join (skin_join.h says what it decides, skin_join_shader.h is the HLSL). This is the object
// engine_velocity.cpp owns: it runs JoinCS at the game's palette-chain dispatch, builds the pose table from the first eye's pool
// copy, and hands the draws the three views the cloned vertex shaders read (t108 previous palette, t109 join, t110 previous pose).
// Everything is VR-only and every failure means "no history this frame": the join table is then the empty one and every record's
// valid flag is 0, never a stale answer.

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <map>
#include <memory>

#include "skin_join.h"

namespace edvr {

struct SkinViews {
    ID3D11ShaderResourceView* prevPalette = nullptr;   // t108 (null: reads as zero)
    ID3D11ShaderResourceView* join = nullptr;          // t109 (never null once created: the empty table when not live)
    ID3D11ShaderResourceView* prevPose = nullptr;      // t110
    bool live = false;                                 // a join ran this present frame with its history certified
};

struct SkinWindow {
    bool valid = false;
    uint32_t gpu[skinjoin::kStatWords] = {};           // the GPU counters' growth since the last window
    skinjoin::WindowCpu cpu;
    uint32_t chainRefused = 0, overJobs = 0, createFailed = 0;
    uint64_t frames = 0;
};

class SkinJoinGpu {
public:
    SkinJoinGpu();
    ~SkinJoinGpu();
    SkinJoinGpu(const SkinJoinGpu&) = delete;
    SkinJoinGpu& operator=(const SkinJoinGpu&) = delete;

    // The owner thread, from the chain dispatch hook, BEFORE the game's dispatch: the join. `groups` is the dispatch's x.
    void onChain(ID3D11DeviceContext* ctx, uint32_t present, uint32_t groups);
    // The owner thread, at the first pool snapshot of a present frame (and again when that snapshot is refreshed): this frame's
    // pose table from the private copy of the pool.
    void scatterPose(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* poolSrv, uint32_t records, uint32_t present);
    // The views the draws of `present` bind.
    SkinViews views(uint32_t present) const;
    // The window since the previous call (cumulative GPU counters read back without waiting); valid=false when nothing new.
    SkinWindow takeWindow(ID3D11DeviceContext* ctx);
    // The last hook state line for the periodic log.
    bool liveNow(uint32_t present) const { return views(present).live; }
    void release();

    // Counters for the log line that do not need the GPU.
    uint64_t chainFrames() const { return chainFrames_; }
    uint64_t poseScatters() const { return poseScatters_; }

    struct Impl;   // public so the translation unit's helpers can take it; its definition is private to that unit

private:
    std::unique_ptr<Impl> impl_;
    uint64_t chainFrames_ = 0, poseScatters_ = 0;
};

}  // namespace edvr
