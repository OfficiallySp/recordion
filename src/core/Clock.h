#pragma once

#include <cstdint>

namespace rec::clock {

// Every timestamp in Recordion lives on the QPC timeline, expressed in 100 ns units.
// WGC's SystemRelativeTime and WASAPI's u64QPCPosition already use this unit.
inline constexpr int64_t kTicksPerSecond = 10'000'000;

int64_t qpcFrequency();
int64_t qpcRaw();
int64_t now100ns();

constexpr int64_t secondsTo100ns(double s) { return static_cast<int64_t>(s * kTicksPerSecond); }
constexpr double toSeconds(int64_t t100ns) { return static_cast<double>(t100ns) / kTicksPerSecond; }

} // namespace rec::clock
