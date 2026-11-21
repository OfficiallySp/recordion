#include "audio/AacEncoder.h"
#include "audio/AudioRing.h"
#include "mux/Bitstream.h"
#include "mux/Mp4Writer.h"

#include <doctest/doctest.h>

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <random>
#include <vector>

namespace {

constexpr uint32_t kRate = rec::AacEncoder::kSampleRate;
constexpr size_t kOnset = 12'000; // sample where the tone burst starts

std::vector<uint8_t> readFile(const char* name) {
    std::ifstream in(std::filesystem::path(RECORDION_TEST_DATA_DIR) / name, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

// One second of silence with a loud 1 kHz burst starting exactly at kOnset.
std::vector<int16_t> toneBurst() {
    std::vector<int16_t> pcm(kRate * 2, 0);
    for (size_t i = kOnset; i < kOnset + 4800; ++i) {
        const auto v = static_cast<int16_t>(16000 * std::sin(2 * std::numbers::pi * 1000 * (i - kOnset) / kRate));
        pcm[2 * i] = pcm[2 * i + 1] = v;
    }
    return pcm;
}

#if defined(RECORDION_FFMPEG)
// Muxes the AAC frames (with a short H.264 video track) and returns the first sample index whose
// decoded amplitude crosses the threshold, or -1.
long decodedOnset(const std::vector<std::vector<uint8_t>>& frames, const std::vector<uint8_t>& asc, uint32_t priming) {
    // Video: the 4-frame fixture, starting at the same instant as the audio.
    static const auto h264 = readFile("h264_202x114.h264");
    static std::vector<std::vector<uint8_t>> videoData;
    rec::Mp4ClipInput in;
    std::vector<std::vector<uint8_t>> aus;
    rec::bitstream::forEachAnnexBNal(h264, [&](std::span<const uint8_t> nal) {
        if ((nal[0] & 0x1F) == 9 || aus.empty()) aus.emplace_back();
        static constexpr uint8_t sc[] = {0, 0, 0, 1};
        aus.back().insert(aus.back().end(), std::begin(sc), std::end(sc));
        aus.back().insert(aus.back().end(), nal.begin(), nal.end());
    });
    in.video = {rec::Codec::H264, 202, 114, 60, 1, rec::bitstream::parseVideoConfig(rec::Codec::H264, aus[0])->configRecord};
    videoData.assign(aus.size(), {});
    for (size_t i = 0; i < aus.size(); ++i) {
        rec::bitstream::appendSample(rec::Codec::H264, aus[i], videoData[i]);
        in.videoSamples.push_back({videoData[i], static_cast<int64_t>(i) * 166'667, i % 2 == 0});
    }
    rec::Mp4AudioInput audio;
    audio.track = {.name = "Test", .sampleRate = kRate, .channels = 2, .bitrate = 192000, .primingSamples = priming,
                   .audioSpecificConfig = asc};
    for (size_t k = 0; k < frames.size(); ++k)
        audio.samples.push_back({frames[k], static_cast<int64_t>(k) * 1024 * 10'000'000 / kRate, true});
    in.audio.push_back(std::move(audio));

    rec::VectorSink sink;
    REQUIRE(rec::writeMp4Clip(in, sink));
    const auto path = std::filesystem::temp_directory_path() / "recordion_aac_test.mp4";
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(sink.data.data()), sink.data.size());

    const std::string cmd = std::string("\"\"") + RECORDION_FFMPEG + "\" -v error -i \"" + path.string() +
                            "\" -map 0:a -f s16le -ac 1 -\"";
    FILE* f = _popen(cmd.c_str(), "rb");
    REQUIRE(f);
    std::vector<int16_t> decoded;
    int16_t buf[4096];
    size_t n;
    while ((n = fread(buf, sizeof(int16_t), std::size(buf), f)) > 0) decoded.insert(decoded.end(), buf, buf + n);
    _pclose(f);
    std::filesystem::remove(path);
    for (size_t i = 0; i < decoded.size(); ++i)
        if (std::abs(decoded[i]) > 8000) return static_cast<long>(i);
    return -1;
}
#endif

} // namespace

TEST_CASE("AAC encoder produces 1024-sample frames and an AudioSpecificConfig") {
    rec::AacEncoder enc;
    REQUIRE(enc.init(192));
    CHECK(enc.bitrate() == 192000);
    REQUIRE(enc.audioSpecificConfig().size() >= 2);
    CHECK((enc.audioSpecificConfig()[0] >> 3) == 2); // AAC-LC

    std::vector<std::vector<uint8_t>> frames;
    std::vector<uint64_t> indices;
    const auto pcm = toneBurst();
    // Feed uneven chunks, as the mixer would.
    static constexpr size_t kChunkFrames[] = {1000, 2048, 31, 7000};
    for (size_t pos = 0, i = 0; pos < pcm.size(); ++i) {
        const size_t n = std::min(kChunkFrames[i % std::size(kChunkFrames)] * 2, pcm.size() - pos);
        REQUIRE(enc.encode({pcm.data() + pos, n}, [&](std::span<const uint8_t> f, uint64_t index) {
            frames.emplace_back(f.begin(), f.end());
            indices.push_back(index);
        }));
        pos += n;
    }
    REQUIRE(enc.drain([&](std::span<const uint8_t> f, uint64_t index) {
        frames.emplace_back(f.begin(), f.end());
        indices.push_back(index);
    }));
    // 48000 samples -> 47 whole frames, plus the frames that flush the encoder delay.
    CHECK(frames.size() >= 47);
    for (size_t i = 0; i < indices.size(); ++i) CHECK(indices[i] == i);

#if defined(RECORDION_FFMPEG)
    // Without an edit list, the onset appears late by exactly the encoder delay...
    const long raw = decodedOnset(frames, enc.audioSpecificConfig(), 0);
    MESSAGE("decoded onset without edit list: ", raw, " (input ", kOnset, ", priming ", enc.primingSamples(), ")");
    CHECK(std::abs(raw - static_cast<long>(kOnset + enc.primingSamples())) <= 16);
    // ...and with the priming in the edit list, it lands on the input sample.
    const long corrected = decodedOnset(frames, enc.audioSpecificConfig(), enc.primingSamples());
    CHECK(std::abs(corrected - static_cast<long>(kOnset)) <= 16);
#endif
}

namespace {

// The ring's original per-sample implementation, as the reference for the run-based one.
class ReferenceRing {
public:
    explicit ReferenceRing(size_t capacity) : data_(capacity * 2, 0.0f), capacity_(capacity) {}
    void write(int64_t pos, const float* samples, size_t frames) {
        if (frames == 0) return;
        if (pos > end_) {
            for (int64_t p = std::max({end_, pos - static_cast<int64_t>(capacity_), int64_t{0}}); p < pos; ++p)
                slot(p)[0] = slot(p)[1] = 0.0f;
        }
        for (size_t i = 0; i < frames; ++i) {
            float* dst = slot(pos + static_cast<int64_t>(i));
            dst[0] = samples ? samples[2 * i] : 0.0f;
            dst[1] = samples ? samples[2 * i + 1] : 0.0f;
        }
        end_ = std::max(end_, pos + static_cast<int64_t>(frames));
        if (start_ < 0 || pos < start_) start_ = pos;
        start_ = std::max(start_, end_ - static_cast<int64_t>(capacity_));
    }
    int64_t end() const { return end_; }
    void mixInto(int64_t pos, size_t frames, float gain, float* out) const {
        for (size_t i = 0; i < frames; ++i) {
            const int64_t p = pos + static_cast<int64_t>(i);
            if (start_ < 0 || p < start_ || p >= end_) continue;
            out[2 * i] += gain * slot(p)[0];
            out[2 * i + 1] += gain * slot(p)[1];
        }
    }

private:
    float* slot(int64_t p) { return &data_[static_cast<size_t>(p % static_cast<int64_t>(capacity_)) * 2]; }
    const float* slot(int64_t p) const { return &data_[static_cast<size_t>(p % static_cast<int64_t>(capacity_)) * 2]; }
    std::vector<float> data_;
    size_t capacity_;
    int64_t start_ = -1;
    int64_t end_ = 0;
};

} // namespace

TEST_CASE("AudioRing matches the per-sample reference across wraps, gaps and overwrites") {
    constexpr size_t kCapacity = 1000;
    rec::AudioRing ring(kCapacity);
    ReferenceRing ref(kCapacity);
    std::mt19937 rng(1234);
    std::vector<float> input(2 * 2500), a, b;
    for (auto& v : input) v = std::uniform_real_distribution<float>(-1, 1)(rng);

    int64_t pos = 0;
    for (int step = 0; step < 3000; ++step) {
        const int kind = static_cast<int>(rng() % 10);
        size_t frames = rng() % 400;
        if (kind == 0) frames = kCapacity + rng() % 1500; // larger than the ring
        if (kind == 1) pos += rng() % 2500;                // a gap: silence
        if (kind == 2 && pos > 300) pos -= rng() % 300;    // overlap (a resync backwards)
        const float* samples = kind == 3 ? nullptr : input.data() + 2 * (rng() % 50);
        ring.write(pos, samples, frames);
        ref.write(pos, samples, frames);
        pos += static_cast<int64_t>(frames);

        const int64_t from = std::max<int64_t>(0, pos - static_cast<int64_t>(rng() % 1600));
        const size_t n = rng() % 1200;
        a.assign(n * 2, 0.25f);
        b.assign(n * 2, 0.25f);
        ring.mixInto(from, n, 0.5f, a.data());
        ref.mixInto(from, n, 0.5f, b.data());
        REQUIRE(a == b);
        REQUIRE(ring.end() == ref.end());
    }
}
