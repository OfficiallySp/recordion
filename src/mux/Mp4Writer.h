#pragma once

#include "core/Settings.h"
#include "mux/ByteSink.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace rec {

struct Mp4Sample {
    std::span<const uint8_t> data; // already in sample format (see bitstream::appendSample)
    int64_t time100ns = 0;         // presentation time on the shared QPC timeline
    bool keyframe = false;
    uint32_t size = 0;             // byte count when `data` is empty (sample tables only)
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
    // When the last frame stops showing (variable frame rate: it holds until the next change or the
    // end of the clip). 0 or not after the last sample: one nominal frame duration.
    int64_t videoEnd100ns = 0;
    std::vector<Mp4AudioInput> audio;
    std::string title;          // shown by players / Explorer
    int64_t creationUnixTime = 0;
};

// Writes a complete, faststart MP4 (moov before mdat) in one sequential pass. Samples of all
// tracks are interleaved in ~0.5 s chunks so players and editors read the file linearly.
bool writeMp4Clip(const Mp4ClipInput& input, ByteSink& sink);

// "Hybrid" MP4 for recordings: written as fragments (moof + mdat per GOP) so the file stays
// playable up to the last fragment if the app or the PC dies, then turned into an ordinary MP4 by
// finish(): a complete moov is appended and the fragment headers become 'free' boxes, which
// editors that dislike fragmented files (Premiere, Resolve) never see.
class Mp4FragmentedWriter {
public:
    // Writes ftyp and the fragmented moov. Audio may start before the video (pre-roll); the edit
    // lists hide it.
    bool begin(ByteSink& sink, const Mp4VideoTrack& video, const std::vector<Mp4AudioTrack>& audio,
               int64_t videoStart100ns, const std::vector<int64_t>& audioStart100ns, std::string title,
               int64_t creationUnixTime);
    // One fragment; the video should start on a keyframe. Empty spans are fine. `videoEnd100ns` is
    // when the fragment's last frame stops showing (usually the next fragment's first frame); 0 =
    // one nominal frame duration.
    bool writeFragment(ByteSink& sink, std::span<const Mp4Sample> video, const std::vector<std::span<const Mp4Sample>>& audio,
                       int64_t videoEnd100ns = 0);
    // After the sink's file is closed: appends the complete moov and retires the fragment boxes.
    bool finish(const std::filesystem::path& file);

    double durationSeconds() const;
    size_t videoSamples() const { return tables_.videoSamples.size(); }

private:
    struct Chunks {
        std::vector<uint32_t> firstSample;
        std::vector<uint64_t> offsets;
    };

    Mp4ClipInput tables_;         // track descriptions and size-only samples for the final moov
    std::vector<Chunks> chunks_;  // [0] video, [1..] audio
    std::vector<uint64_t> audioDecodeTime_;
    std::vector<uint64_t> retire_; // file offsets of the fragmented moov and every moof
    int64_t videoStart_ = 0;
    uint32_t sequence_ = 0;
};

} // namespace rec
