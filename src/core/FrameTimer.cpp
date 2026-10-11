#include "core/FrameTimer.h"

#include "core/Clock.h"

namespace rec {

FrameTimer::FrameTimer(uint32_t fpsNum, uint32_t fpsDen) : fpsNum_(fpsNum), fpsDen_(fpsDen) {
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    start100ns_ = clock::now100ns();
}

FrameTimer::~FrameTimer() {
    if (timer_) CloseHandle(timer_);
}

int64_t FrameTimer::interval100ns() const { return clock::kTicksPerSecond * fpsDen_ / fpsNum_; }

int64_t FrameTimer::tickTime100ns() const {
    // Computed from the index (not accumulated) so rounding never drifts.
    return start100ns_ + static_cast<int64_t>(index_) * clock::kTicksPerSecond * fpsDen_ / fpsNum_;
}

bool FrameTimer::waitNext(HANDLE cancel) {
    ++index_;
    int64_t now = clock::now100ns();
    if (now - tickTime100ns() > interval100ns()) {
        // Fell behind (e.g. system stall): jump to the most recent tick instead of catching up.
        const auto behind = static_cast<uint64_t>((now - start100ns_) * fpsNum_ / (clock::kTicksPerSecond * fpsDen_));
        skipped_ += behind - index_ + 1;
        index_ = behind + 1;
    }
    const int64_t wait = tickTime100ns() - now;
    if (wait <= 0) return true;

    LARGE_INTEGER due;
    due.QuadPart = -wait; // relative, 100 ns units
    SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE);
    const HANDLE handles[] = {timer_, cancel};
    const DWORD r = WaitForMultipleObjects(cancel ? 2 : 1, handles, FALSE, INFINITE);
    return r == WAIT_OBJECT_0;
}

} // namespace rec
