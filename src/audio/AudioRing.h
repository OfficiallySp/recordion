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

    // Writes `frames` interleaved stereo frames (nullptr = silence) at absolute position `pos` (>= 0).
    void write(int64_t pos, const float* samples, size_t frames) {
        if (frames == 0) return;
        if (pos > end_) zero(end_, pos); // gap since the last write: silence
        const int64_t endPos = pos + static_cast<int64_t>(frames);
        if (frames > capacity_) { // only the newest capacity_ frames survive
            const size_t skip = frames - capacity_;
            if (samples) samples += skip * kChannels;
            pos += static_cast<int64_t>(skip);
            frames = capacity_;
        }
        // Contiguous runs (at most two, around the wrap) instead of a modulo per sample.
        forEachRun(pos, frames, [&](size_t index, size_t offset, size_t n) {
            float* dst = &data_[index * kChannels];
            if (samples) std::copy_n(samples + offset * kChannels, n * kChannels, dst);
            else std::fill_n(dst, n * kChannels, 0.0f);
        });
        end_ = std::max(end_, endPos);
        if (start_ < 0 || pos < start_) start_ = pos;
        start_ = std::max(start_, end_ - static_cast<int64_t>(capacity_));
    }

    // Adds `gain` * samples in [pos, pos + frames) into `out` (interleaved stereo).
    void mixInto(int64_t pos, size_t frames, float gain, float* out) const {
        if (start_ < 0) return;
        const int64_t from = std::max(pos, start_);
        const int64_t to = std::min(pos + static_cast<int64_t>(frames), end_);
        if (from >= to) return;
        float* base = out + static_cast<size_t>(from - pos) * kChannels;
        forEachRun(from, static_cast<size_t>(to - from), [&](size_t index, size_t offset, size_t n) {
            const float* src = &data_[index * kChannels];
            float* dst = base + offset * kChannels;
            for (size_t i = 0; i < n * kChannels; ++i) dst[i] += gain * src[i];
        });
    }

    int64_t end() const { return end_; } // one past the last written position
    bool empty() const { return start_ < 0; }

private:
    // Calls fn(ringIndex, offsetFromPos, count) for each contiguous piece of [pos, pos + frames),
    // frames <= capacity_.
    template <class Fn>
    void forEachRun(int64_t pos, size_t frames, Fn&& fn) const {
        size_t index = static_cast<size_t>(pos % static_cast<int64_t>(capacity_));
        for (size_t offset = 0; offset < frames;) {
            const size_t n = std::min(frames - offset, capacity_ - index);
            fn(index, offset, n);
            offset += n;
            index = 0;
        }
    }
    void zero(int64_t from, int64_t to) {
        from = std::max({from, to - static_cast<int64_t>(capacity_), int64_t{0}});
        if (from >= to) return;
        forEachRun(from, static_cast<size_t>(to - from),
                   [&](size_t index, size_t, size_t n) { std::fill_n(&data_[index * kChannels], n * kChannels, 0.0f); });
    }

    std::vector<float> data_;
    size_t capacity_;
    int64_t start_ = -1; // first valid position, -1 = nothing written
    int64_t end_ = 0;
};

} // namespace rec
