#pragma once

#include <windows.h>

#include <cstdint>

namespace rec::wallclock {

// Converts a QPC-timeline time to wall-clock time, given one instant known on both clocks.
inline FILETIME fromQpc(int64_t time100ns, int64_t anchorQpc100ns, FILETIME anchorWall) {
    ULARGE_INTEGER u{.LowPart = anchorWall.dwLowDateTime, .HighPart = anchorWall.dwHighDateTime};
    u.QuadPart += time100ns - anchorQpc100ns;
    return {u.LowPart, u.HighPart};
}

inline SYSTEMTIME toLocal(FILETIME utc) {
    SYSTEMTIME st{}, local{};
    FileTimeToSystemTime(&utc, &st);
    SystemTimeToTzSpecificLocalTime(nullptr, &st, &local);
    return local;
}

inline int64_t toUnixSeconds(FILETIME t) {
    const ULARGE_INTEGER u{.LowPart = t.dwLowDateTime, .HighPart = t.dwHighDateTime};
    return static_cast<int64_t>(u.QuadPart / 10'000'000) - 11'644'473'600LL;
}

} // namespace rec::wallclock
