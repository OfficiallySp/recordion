#pragma once

#include <windows.h>

#include <cstdint>

namespace rec {

// Drift-free fixed-rate ticker on a high-resolution waitable timer (no global timeBeginPeriod).
class FrameTimer {
public:
    FrameTimer(uint32_t fpsNum, uint32_t fpsDen = 1);
    ~FrameTimer();
    FrameTimer(const FrameTimer&) = delete;
    FrameTimer& operator=(const FrameTimer&) = delete;

    // Sleeps until the next tick. Returns false if `cancel` was signalled instead. If we fell
    // behind by more than a tick, the schedule skips ahead rather than bursting.
    bool waitNext(HANDLE cancel = nullptr);

    int64_t tickTime100ns() const; // scheduled time of the current tick
    uint64_t tickIndex() const { return index_; }
    uint64_t skippedTicks() const { return skipped_; }
    int64_t interval100ns() const;

private:
    HANDLE timer_ = nullptr;
    uint32_t fpsNum_;
    uint32_t fpsDen_;
    int64_t start100ns_ = 0;
    uint64_t index_ = 0;
    uint64_t skipped_ = 0;
};

} // namespace rec
