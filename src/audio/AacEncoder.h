#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

struct AACENCODER;

namespace rec {

// AAC-LC via fdk-aac (the stripped AAC-LC-only build): 48 kHz stereo 16-bit PCM in, one raw AAC
// frame (1024 samples) out per callback. About half the CPU of the encoder built into Windows.
class AacEncoder {
public:
    static constexpr uint32_t kSampleRate = 48000;
    static constexpr uint32_t kChannels = 2;
    static constexpr uint32_t kFrameSamples = 1024;

    AacEncoder() = default;
    AacEncoder(const AacEncoder&) = delete;
    AacEncoder& operator=(const AacEncoder&) = delete;
    ~AacEncoder();
    // kbps: 96, 128, 160 or 192 (what the settings offer; others snap to the nearest).
    bool init(uint32_t kbps);
    // Interleaved stereo PCM; any length. `onFrame(data, index)` gets each encoded frame; frame
    // `index` decodes to input samples starting at index * 1024 - primingSamples().
    using FrameCallback = std::function<void(std::span<const uint8_t> frame, uint64_t index)>;
    bool encode(std::span<const int16_t> interleaved, const FrameCallback& onFrame);
    bool drain(const FrameCallback& onFrame);

    const std::vector<uint8_t>& audioSpecificConfig() const { return asc_; }
    uint32_t bitrate() const { return kbps_ * 1000; }
    // Encoder delay at the start of the stream: the decoded output runs this many samples behind
    // the input (1600 for fdk-aac AAC-LC with only the core module open; see test_audio.cpp).
    uint32_t primingSamples() const { return priming_; }

private:
    AACENCODER* enc_ = nullptr;
    std::vector<uint8_t> out_;
    std::vector<uint8_t> asc_;
    uint32_t kbps_ = 0;
    uint32_t priming_ = 0;
    uint64_t framesOut_ = 0;
};

} // namespace rec
