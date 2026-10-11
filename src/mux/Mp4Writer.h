#pragma once

#include "core/Settings.h"
#include "mux/ByteSink.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace rec {

struct Mp4Sample {
    std::span<const uint8_t> data; // already in sample format (see bitstream::appendSample)
    int64_t time100ns = 0;         // presentation time on the shared QPC timeline
    bool keyframe = false;
};

struct Mp4VideoTrack {
    Codec codec = Codec::HEVC;
    uint32_t width = 0;  // display size
    uint32_t height = 0;
    uint32_t fpsNum = 60;
    uint32_t fpsDen = 1;
    std::vector<uint8_t> configRecord; // avcC / hvcC / av1C payload
};

struct Mp4AudioTrack {
    std::string name;
    bool enabled = true; // only one audio track should be enabled (the one players pick)
    uint32_t sampleRate = 48000;
    uint32_t channels = 2;
    uint32_t bitrate = 192000;
    uint32_t primingSamples = 0;               // encoder delay, hidden with an edit list
    std::vector<uint8_t> audioSpecificConfig;  // AAC AudioSpecificConfig
};

struct Mp4AudioInput {
    Mp4AudioTrack track;
    std::vector<Mp4Sample> samples; // 1024-sample AAC frames
};

struct Mp4ClipInput {
    Mp4VideoTrack video;
    std::vector<Mp4Sample> videoSamples; // must start with a keyframe
    std::vector<Mp4AudioInput> audio;
    std::string title;          // shown by players / Explorer
    int64_t creationUnixTime = 0;
};

// Writes a complete, faststart MP4 (moov before mdat) in one sequential pass. Samples of all
// tracks are interleaved in ~0.5 s chunks so players and editors read the file linearly.
bool writeMp4Clip(const Mp4ClipInput& input, ByteSink& sink);

} // namespace rec
