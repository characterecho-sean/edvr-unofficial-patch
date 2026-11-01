// Why the depth probe watches (depth_probe.h). Pure, so the rig (tools/depth_scene_pick_test) runs the decision depthProbeConfigure makes.
//
// The probe's scene pair (the two busiest depth targets of the eye's size, ordered by first bind: which is the left eye, which the right) is what every
// consumer of an eye's identity on a draw asks: the temporal pass, the interface depth, the engine-record velocity, and native on-foot VR's F113 row sources
// and, through them, the ADS sight lock's rendered eyes. Three things want it:
//   - fix.temporal_aa not off: the temporal pass asks for the pair by the eye's size every frame;
//   - advanced.eye_depth_capture: the flight rig runs with fix.temporal_aa off and needs the pair for its verdict;
//   - native on-foot VR (F196): the VR profile with fix.native_on_foot on. With Anti-aliasing off (the shipped default) nothing else asks, so the pair
//     never formed, no eye ever had rows and the ADS sight lock placed the aiming eye by F110's model every frame. This arm has the probe count the draws
//     and form the pair itself at each frame boundary, and answer who is which eye. It takes no depth readbacks: the diagnostics the other two arms run
//     (the unbind-and-sample, the frame-boundary read, the layout census) stay off for it (depthProbeDiagnosticsFor). From F199 depthProbeSceneDepth
//     hands it out the scene depth textures of the pair the boundary formed, for the one size it asks (F85c's reticle march and F119's head-containment grid, with
//     F123's wall contact behind it, read those on the GPU and never had one with Anti-aliasing off); it is never re-picked by an asker's size.
#pragma once

#include <cstring>
#include <string>

namespace edvr {

enum class DepthProbeWhy { kOff, kTemporal, kCapture, kNativeOnFoot };

// fix.temporal_aa as depthProbeConfigure has always read it: any value but off arms the probe, a value the temporal pass does not recognise included.
inline bool depthProbeTemporalValue(const std::string& temporalMode) {
    return !temporalMode.empty() && _stricmp(temporalMode.c_str(), "off") != 0;
}

// Native on-foot VR can run: the VR profile, and fix.native_on_foot on (native_on_foot.cpp readKey's own test, the exact word).
inline bool depthProbeNativeOnFootVr(bool vrProfile, const std::string& nativeOnFootKey) {
    return vrProfile && nativeOnFootKey == "on";
}

// The temporal arm outranks the capture, the capture outranks native on foot: a session that has the diagnostics keeps them whatever else arms it.
inline DepthProbeWhy depthProbeWhyFor(const std::string& temporalMode, bool eyeDepthCapture, bool nativeOnFootVr) {
    if (depthProbeTemporalValue(temporalMode)) return DepthProbeWhy::kTemporal;
    if (eyeDepthCapture) return DepthProbeWhy::kCapture;
    if (nativeOnFootVr) return DepthProbeWhy::kNativeOnFoot;
    return DepthProbeWhy::kOff;
}

inline bool depthProbeWantedFor(const std::string& temporalMode, bool eyeDepthCapture, bool nativeOnFootVr) {
    return depthProbeWhyFor(temporalMode, eyeDepthCapture, nativeOnFootVr) != DepthProbeWhy::kOff;
}

// The readbacks and the census: what the probe did before F196, for the two arms that existed then. The native on-foot arm runs the scene pick alone.
inline bool depthProbeDiagnosticsFor(DepthProbeWhy why) {
    return why == DepthProbeWhy::kTemporal || why == DepthProbeWhy::kCapture;
}

}  // namespace edvr
