// Discovery records use logical game bindings unless an explicit capture needs
// physical context state. Manual General deliberately retains eager behavior.
#pragma once

namespace edvr {
inline bool flatDiscoveryNeedsOriginalState(bool observing, bool manualWindow,
                                            bool computeCandidate) noexcept {
    return observing && (manualWindow || computeCandidate);
}
}  // namespace edvr
