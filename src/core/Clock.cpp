#include "core/Clock.h"

#include <windows.h>

namespace rec::clock {

int64_t qpcFrequency() {
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    return freq;
}

int64_t qpcRaw() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

int64_t now100ns() {
    const int64_t raw = qpcRaw();
    const int64_t freq = qpcFrequency();
    // Split to avoid overflow on long uptimes.
    return (raw / freq) * kTicksPerSecond + (raw % freq) * kTicksPerSecond / freq;
}

} // namespace rec::clock
