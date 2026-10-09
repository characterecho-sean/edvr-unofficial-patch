#pragma once

// F2: the GPU half of the identity join (skin_join.h says what it decides, skin_join_shader.h is the HLSL). This is the object
// engine_velocity.cpp owns: it runs JoinCS once per present frame over the game's palette-chain dispatches (one or two a frame), builds the pose table at the frame boundary from the
// first eye's pool copy and the draws' instance-stream references, and hands the draws the three views the cloned vertex shaders read
// (t108 previous palette, t109 join, t110 previous pose).
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

// The instance-stream entries the frame's skinned draws read, as the CPU saw the draws (engine_velocity.cpp collects them at the draw hook; skin_join.h,
// "the pose table's CPU reference", says why this and nothing else decides which record of a base is the live one).
struct SkinRefs {
    const uint32_t* ranges = nullptr;   // (StartInstanceLocation, instances) per skinned draw, two words each
    uint32_t pairs = 0;
    ID3D11Buffer* stream = nullptr;     // the instance stream the draws read (vertex buffer, stride 8)
    uint32_t streamOffset = 0;          // its input-assembler byte offset
    bool complete = false;              // every skinned draw of the frame is in `ranges`, all reading `stream`
    const char* why = "";               // when not complete: what the draw hook met (a draw it could not list)
};

struct SkinWindow {
    bool valid = false;
    uint32_t gpu[skinjoin::kStatWords] = {};           // the GPU counters' growth since the last window
    skinjoin::WindowCpu cpu;                           // ...and the CPU counters' growth over THE SAME chain frames (snapshotted with the GPU copy)
    uint32_t chainRefused = 0, overJobs = 0, createFailed = 0;
    uint64_t frames = 0;                               // the chain frames the window covers (equal to gpu[kStatFrames] by construction)
};

class SkinJoinGpu {
public:
    SkinJoinGpu();
    ~SkinJoinGpu();
    SkinJoinGpu(const SkinJoinGpu&) = delete;
    SkinJoinGpu& operator=(const SkinJoinGpu&) = delete;

    // The owner thread, from the chain dispatch hook, BEFORE the game's dispatch: note one dispatch of the frame's palette chain (`groups` is its x). The
    // game dispatches the chain once or twice a present frame; the join is ONE per present frame over the dispatches' job tables concatenated in dispatch
    // order, run by flush() at the first skinned draw that needs it, or by flushPending() at the frame boundary when none did. Returns whether the
    // dispatch was taken (a job table with jobs, the bindings the ledger measured).
    bool noteChain(ID3D11DeviceContext* ctx, uint32_t present, uint32_t groups);
    // Run the join of `present`'s dispatches now if they are waiting (the draws are about to bind its views); a no-op otherwise.
    void flush(ID3D11DeviceContext* ctx, uint32_t present);
    // The frame boundary: whatever is still waiting runs, whatever its present.
    void flushPending(ID3D11DeviceContext* ctx);
    // The last join that ran was live for its frame (history certified, a previous palette): what the entry fade waits for when the frame had skinned jobs.
    bool lastJoinLive() const;
    // The owner thread, at the frame boundary (every draw of `present` has been issued): the pose table of `present` from the private copy of
    // the pool, the live record of each base chosen by `refs` (skin_join_shader.h). It is next frame's "previous pose".
    void buildPose(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* poolSrv, uint32_t records, uint32_t present, const SkinRefs& refs);
    // The views the draws of `present` bind.
    SkinViews views(uint32_t present) const;
    // The window since the previous call (cumulative GPU counters read back without waiting); valid=false when nothing new.
    SkinWindow takeWindow(ID3D11DeviceContext* ctx);
    // The last hook state line for the periodic log.
    bool liveNow(uint32_t present) const { return views(present).live; }
    void release();

    // Counters for the log line that do not need the GPU.
    uint64_t chainFrames() const { return chainFrames_; }   // the joins that ran (one a present frame with a chain)
    uint64_t poseScatters() const { return poseScatters_; }
    // Why the last pose table had no exact reference list ("" when it had one): the CPU's verdict, for the periodic line.
    const char* lastInexactWhy() const { return inexactWhy_; }

    struct Impl;   // public so the translation unit's helpers can take it; its definition is private to that unit

private:
    void runJoin(ID3D11DeviceContext* ctx);
    std::unique_ptr<Impl> impl_;
    uint64_t chainFrames_ = 0, poseScatters_ = 0;
    const char* inexactWhy_ = "";
};

}  // namespace edvr
