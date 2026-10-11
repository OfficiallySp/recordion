#pragma once

#include <cstdint>

namespace rec {

// Maps between the shared QPC timeline (100 ns) and 48 kHz sample positions counted from `origin`.
struct AudioTimeline {
    static constexpr int64_t kRate = 48000;
    int64_t origin100ns = 0;

    // Rounded to the nearest sample.
    int64_t toSample(int64_t t100ns) const {
        const int64_t d = t100ns - origin100ns;
        return d >= 0 ? (d * kRate + 5'000'000) / 10'000'000 : -((-d * kRate + 5'000'000) / 10'000'000);
    }
    int64_t toTime(int64_t sample) const { return origin100ns + sample * 10'000'000 / kRate; }
};

} // namespace rec
