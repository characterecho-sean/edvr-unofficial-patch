#pragma once
#include <cstdint>

namespace edvr {
// The camera producer probe (docs/design-flat-camera-integration.md, C1->C2):
// bounded, diagnostic-only. Step 1 hooks the Ghidra-validated single-site
// upload helper (EliteDangerous64.exe+0x51B740) to learn the 5376-byte scene
// CB's staging pool block. Step 2 arms one hardware write breakpoint on the
// block's camera-row span (rows 270..271) on the uploading thread and names
// the writer's RIP. Gated by advanced.flat_camera_producer_probe (default
// off, hot-reloaded); every budget says its name when reached; a refusal
// stands down, never guesses. Called once per Present from flatRuntimePresent.
void flatCameraProducerProbeFrame(uint64_t frame);
} // namespace edvr
