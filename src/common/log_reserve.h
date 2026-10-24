// The log's post-cap reserve (Log::noteReserved in log.h): the accounting, apart from the file, so a rig can hold it.
//
// WHY. The log stops at log.max_mb (16 MB by default), and a session that runs for hours reaches it. A line that is the
// only evidence of a slow event (the memory watch's mem: lines, src\common\mem_watch.h) would then stop exactly when the
// event is long enough to matter. A line written with noteReserved still goes out after the cap, from a reserve that is
// a fixed size per session: a constant, not a setting, so a run-away writer cannot fill a disk through it.
//
// WHAT. A reserved line is admitted while the bytes already written from the reserve plus its own fit kLogReserveBytes.
// The first line that does not fit is refused with Spent, once, so the log can say the reserve is gone; every line
// after that is Skip. The reserve is only used after the cap: before it, a reserved line is an ordinary line.
#pragma once

#include <cstddef>
#include <cstdint>

namespace edvr {

constexpr uint64_t kLogReserveBytes = 1ull << 20;

// The room one reserved line is formatted in, its stamp and line end included. An ordinary line has 1200 bytes.
constexpr size_t kLogReservedLineBytes = 2800;

enum class ReserveVerdict {
    Write,   // the line is within the reserve
    Spent,   // the reserve cannot take this line: say so, once
    Skip,    // the reserve was spent before: nothing is written
};

class LogReserve {
public:
    ReserveVerdict admit(uint64_t lineBytes) {
        if (spent_) return ReserveVerdict::Skip;
        if (lineBytes > kLogReserveBytes - used_) {
            spent_ = true;
            return ReserveVerdict::Spent;
        }
        used_ += lineBytes;
        return ReserveVerdict::Write;
    }

    void reset() {
        used_ = 0;
        spent_ = false;
    }

    uint64_t used() const { return used_; }
    bool spent() const { return spent_; }

private:
    uint64_t used_ = 0;
    bool spent_ = false;
};

}  // namespace edvr
