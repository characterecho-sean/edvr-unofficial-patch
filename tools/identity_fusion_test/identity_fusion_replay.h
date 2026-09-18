#pragma once

#include <filesystem>

namespace identity_fusion_replay {

// Replays a prepared EDVRIFR1 fixture on the default hardware adapter.
// Benchmark and output are reserved for the full producer/consumer gate;
// source-capture equivalence runs for every replay.
bool run(
    const std::filesystem::path& fixture,
    bool benchmark,
    bool costBreakdown,
    const std::filesystem::path* output);
bool selfTest();

}
