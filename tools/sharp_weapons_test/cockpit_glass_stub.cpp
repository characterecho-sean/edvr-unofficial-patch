// A stand-in for src\d3d11\cockpit_glass.cpp in the rigs that compile the UI layer whole (ui_layer_world_test, ui_layer_device_test, flat_weapon_admission_test): the layer asks the
// legacy canopy's swap three things and the flat adapter one more, and these rigs have no swap, so the answer is always "not in".
#include "../../src/d3d11/cockpit_glass.h"

namespace edvr {
namespace detail {
std::atomic<bool> g_cockpitGlassLegacy{false};
}  // namespace detail
bool cockpitGlassSwapActive() { return false; }
bool cockpitGlassSetLayerMap(ID3D11DeviceContext*, float, float, float, float) { return false; }
void cockpitGlassClearLayerMap(ID3D11DeviceContext*) {}
}  // namespace edvr
