#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace rec {

// Stereo float samples addressed by absolute timeline position. Positions never written (a silent
// loopback stream, a gap) read back as silence.
class AudioRing {
public:
    static constexpr int kChannels = 2;

    explicit AudioRing(size_t capacityFrames = 48000 * 2) : data_(capacityFrames * kChannels, 0.0f), capacity_(capacityFrames) {}

    // Writes `frames` interleaved stereo frames (nullptr = silence) at absolute position `pos`.
    void write(int64_t pos, const float* samples, size_t frames) {
        if (frames == 0) return;
        if (pos > end_) zero(end_, pos); // gap since the last write: silence
        for (size_t i = 0; i < frames; ++i) {
            float* dst = slot(pos + static_cast<int64_t>(i));
            dst[0] = samples ? samples[2 * i] : 0.0f;
            dst[1] = samples ? samples[2 * i + 1] : 0.0f;
        }
        end_ = std::max(end_, pos + static_cast<int64_t>(frames));
        if (start_ < 0 || pos < start_) start_ = pos;
        start_ = std::max(start_, end_ - static_cast<int64_t>(capacity_));
    }

    // Adds `gain` * samples in [pos, pos + frames) into `out` (interleaved stereo).
    void mixInto(int64_t pos, size_t frames, float gain, float* out) const {
        for (size_t i = 0; i < frames; ++i) {
            const int64_t p = pos + static_cast<int64_t>(i);
            if (start_ < 0 || p < start_ || p >= end_) continue;
            const float* src = slot(p);
            out[2 * i] += gain * src[0];
            out[2 * i + 1] += gain * src[1];
        }
    }

    int64_t end() const { return end_; } // one past the last written position
    bool empty() const { return start_ < 0; }

private:
    float* slot(int64_t pos) { return &data_[static_cast<size_t>(pos % static_cast<int64_t>(capacity_)) * kChannels]; }
    const float* slot(int64_t pos) const {
        return &data_[static_cast<size_t>(pos % static_cast<int64_t>(capacity_)) * kChannels];
    }
    void zero(int64_t from, int64_t to) {
        from = std::max(from, to - static_cast<int64_t>(capacity_));
        for (int64_t p = std::max<int64_t>(from, 0); p < to; ++p) {
            float* dst = slot(p);
            dst[0] = dst[1] = 0.0f;
        }
    }

    std::vector<float> data_;
    size_t capacity_;
    int64_t start_ = -1; // first valid position, -1 = nothing written
    int64_t end_ = 0;
};

} // namespace rec
