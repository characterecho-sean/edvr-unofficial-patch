#include "exposure_shape.h"

#include "../../d3d11/binding_shadow.h"

namespace edvr::plugins::exposure {
namespace {

edvr::BindSlot uavSlot(uint32_t i) {
    return static_cast<edvr::BindSlot>(
        static_cast<uint32_t>(edvr::BindSlot::CsUav0) + i);
}

}  // namespace

bool shapeLooksLikeExposure() {
    // Slot 0 is a small structured buffer holding the luminance range; slot 1
    // is the tiny one-row texture holding the tonemap parameters. Both are
    // distinctive and independent of the shader bytecode hash.
    edvr::ResourceInfo buf;
    if (!edvr::bindingResolve(edvr::bindingGet(uavSlot(0)), &buf) || !buf.isBuffer) {
        return false;
    }
    if (buf.a == 0 || buf.a > 256) return false;

    edvr::ResourceInfo strip;
    if (!edvr::bindingResolve(edvr::bindingGet(uavSlot(1)), &strip) ||
        !strip.isTexture2D) {
        return false;
    }
    return strip.b == 1 && strip.a != 0 && strip.a <= 64;
}

}  // namespace edvr::plugins::exposure
