#pragma once

#include <cstdint>
#include <cstdio>

namespace edvr {

// One failed XeSS feature key is remembered at a time. A new size or HDR route
// gets a fresh attempt; a mode reset explicitly clears the remembered key.
struct XessFeatureKey {
    uint32_t inW, inH, outW, outH;
    bool hdr;

    bool operator==(const XessFeatureKey& other) const {
        return inW == other.inW && inH == other.inH &&
               outW == other.outW && outH == other.outH && hdr == other.hdr;
    }
};

class XessFeatureFailureLatch {
public:
    bool matches(const XessFeatureKey& key) const { return failed_ && key_ == key; }
    const char* reason() const { return reason_; }
    void clear() { failed_ = false; reason_[0] = '\0'; }
    void clearIfDifferent(const XessFeatureKey& key) {
        if (failed_ && !(key_ == key)) clear();
    }
    void record(const XessFeatureKey& key, const char* reason) {
        key_ = key;
        std::snprintf(reason_, sizeof(reason_), "%s", reason ? reason : "no reason given");
        failed_ = true;
    }

private:
    XessFeatureKey key_{};
    char reason_[320]{};
    bool failed_ = false;
};

}  // namespace edvr
