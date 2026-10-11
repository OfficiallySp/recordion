#pragma once

#include <mftransform.h>

#include <winrt/base.h>

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace rec {

// AAC-LC via the Media Foundation encoder built into Windows: 48 kHz stereo 16-bit PCM in,
// one raw AAC frame (1024 samples) out per callback.
class AacEncoder {
public:
    static constexpr uint32_t kSampleRate = 48000;
    static constexpr uint32_t kChannels = 2;
    static constexpr uint32_t kFrameSamples = 1024;
    // Encoder delay at the start of the stream. The Windows encoder emits frames already aligned to
    // its input (measured: a tone onset decodes at the same sample; see test_audio.cpp).
    static constexpr uint32_t kPrimingSamples = 0;

    ~AacEncoder();
    // kbps: 96, 128, 160 or 192 (what the Windows encoder supports).
    bool init(uint32_t kbps);
    // Interleaved stereo PCM; any length. `onFrame(data, index)` gets each encoded frame; frame
    // `index` decodes to input samples starting at index * 1024 - kPrimingSamples.
    using FrameCallback = std::function<void(std::span<const uint8_t> frame, uint64_t index)>;
    bool encode(std::span<const int16_t> interleaved, const FrameCallback& onFrame);
    bool drain(const FrameCallback& onFrame);

    const std::vector<uint8_t>& audioSpecificConfig() const { return asc_; }
    uint32_t bitrate() const { return kbps_ * 1000; }

private:
    bool pull(const FrameCallback& onFrame);

    winrt::com_ptr<IMFTransform> mft_;
    std::vector<uint8_t> asc_;
    uint32_t kbps_ = 0;
    uint32_t outputBufferSize_ = 0;
    uint64_t inputSamples_ = 0;
    uint64_t framesOut_ = 0;
    bool mfStarted_ = false;
};

} // namespace rec
