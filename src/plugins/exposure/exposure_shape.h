#pragma once

namespace edvr::plugins::exposure {

// Classify the game's exposure compute pass from its two output bindings.
// The binding shadow and resource resolver are core services.
bool shapeLooksLikeExposure();

}  // namespace edvr::plugins::exposure
