#include "mux/Bitstream.h"
#include "mux/Mp4Writer.h"

#include <doctest/doctest.h>

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace bs = rec::bitstream;
using rec::Codec;

namespace {

std::vector<uint8_t> readFile(const char* name) {
    std::ifstream in(std::filesystem::path(RECORDION_TEST_DATA_DIR) / name, std::ios::binary);
    REQUIRE_MESSAGE(in, name);
    return {std::istreambuf_iterator<char>(in), {}};
}

struct AccessUnit {
    std::vector<uint8_t> bytes;
    bool keyframe = false;
};

// Splits an elementary stream into access units at AUDs (H.264/HEVC) or temporal delimiters (AV1).
std::vector<AccessUnit> splitAccessUnits(Codec codec, const std::vector<uint8_t>& stream) {
    std::vector<AccessUnit> aus;
    if (codec == Codec::AV1) {
        bs::forEachObu(stream, [&](uint8_t type, std::span<const uint8_t> obu, std::span<const uint8_t>) {
            if (type == 2 || aus.empty()) aus.emplace_back();
            if (type == 1) aus.back().keyframe = true; // libaom repeats the sequence header on key frames
            aus.back().bytes.insert(aus.back().bytes.end(), obu.begin(), obu.end());
        });
        return aus;
    }
    bs::forEachAnnexBNal(stream, [&](std::span<const uint8_t> nal) {
        const int type = codec == Codec::H264 ? (nal[0] & 0x1F) : ((nal[0] >> 1) & 0x3F);
        const bool aud = codec == Codec::H264 ? type == 9 : type == 35;
        const bool key = codec == Codec::H264 ? type == 5 : (type >= 16 && type <= 23);
        if (aud || aus.empty()) aus.emplace_back();
        aus.back().keyframe |= key;
        static constexpr uint8_t startCode[] = {0, 0, 0, 1};
        aus.back().bytes.insert(aus.back().bytes.end(), std::begin(startCode), std::end(startCode));
        aus.back().bytes.insert(aus.back().bytes.end(), nal.begin(), nal.end());
    });
    return aus;
}

struct Adts {
    std::vector<std::vector<uint8_t>> frames;
    std::vector<uint8_t> asc;
};

Adts parseAdts(const std::vector<uint8_t>& data) {
    Adts out;
    size_t p = 0;
    while (p + 7 <= data.size()) {
        REQUIRE(data[p] == 0xFF);
        const bool protectionAbsent = data[p + 1] & 1;
        const int profile = data[p + 2] >> 6;
        const int freqIndex = (data[p + 2] >> 2) & 0xF;
        const int channels = ((data[p + 2] & 1) << 2) | (data[p + 3] >> 6);
        const size_t frameLen = ((data[p + 3] & 3) << 11) | (data[p + 4] << 3) | (data[p + 5] >> 5);
        const size_t header = protectionAbsent ? 7 : 9;
        out.frames.emplace_back(data.begin() + p + header, data.begin() + p + frameLen);
        if (out.asc.empty()) {
            const int objectType = profile + 1;
            out.asc = {static_cast<uint8_t>((objectType << 3) | (freqIndex >> 1)),
                       static_cast<uint8_t>(((freqIndex & 1) << 7) | (channels << 3))};
        }
        p += frameLen;
    }
    return out;
}

std::vector<std::string> topLevelBoxes(const std::vector<uint8_t>& d) {
    std::vector<std::string> out;
    size_t p = 0;
    while (p + 8 <= d.size()) {
        uint64_t size = (uint64_t{d[p]} << 24) | (d[p + 1] << 16) | (d[p + 2] << 8) | d[p + 3];
        out.emplace_back(reinterpret_cast<const char*>(&d[p + 4]), 4);
        if (size == 1)
            for (int i = 0; i < 8; ++i) size = (i == 0 ? 0 : size << 8) | d[p + 8 + i];
        REQUIRE(size >= 8);
        p += size;
    }
    CHECK(p == d.size());
    return out;
}

std::string run(const std::string& cmd) {
    std::string out;
    FILE* f = _popen((cmd + " 2>&1").c_str(), "r");
    if (!f) return "<popen failed>";
    char buf[512];
    while (fgets(buf, sizeof(buf), f)) out += buf;
    _pclose(f);
    return out;
}

} // namespace

TEST_CASE("clip writer produces valid MP4s for every codec, with AAC audio") {
    const auto adts = parseAdts(readFile("aac_48k_stereo.adts"));
    REQUIRE(adts.frames.size() >= 4);

    struct Case {
        Codec codec;
        const char* file;
        const char* codecName;
    };
    for (const Case c : {Case{Codec::H264, "h264_202x114.h264", "h264"}, Case{Codec::HEVC, "hevc10_202x114.hevc", "hevc"},
                         Case{Codec::AV1, "av1_202x114.obu", "av1"}}) {
        CAPTURE(c.codecName);
        const auto aus = splitAccessUnits(c.codec, readFile(c.file));
        REQUIRE(aus.size() == 4);
        REQUIRE(aus[0].keyframe);
        const auto config = bs::parseVideoConfig(c.codec, aus[0].bytes);
        REQUIRE(config);

        const int64_t base = 123'456'789; // arbitrary QPC-timeline start
        std::vector<std::vector<uint8_t>> sampleData(aus.size());
        rec::Mp4ClipInput in;
        in.video = {c.codec, 202, 114, 60, 1, config->configRecord};
        for (size_t i = 0; i < aus.size(); ++i) {
            bs::appendSample(c.codec, aus[i].bytes, sampleData[i]);
            in.videoSamples.push_back({sampleData[i], base + static_cast<int64_t>(i) * 10'000'000 / 60, aus[i].keyframe});
        }
        rec::Mp4AudioInput audio;
        audio.track = {.name = "Game+Mic", .sampleRate = 48000, .channels = 2, .bitrate = 128000,
                       .primingSamples = 1024, .audioSpecificConfig = adts.asc};
        const int64_t audioStart = base - 50'000; // 5 ms before the first video frame
        for (size_t j = 0; j < adts.frames.size(); ++j)
            audio.samples.push_back({adts.frames[j], audioStart + static_cast<int64_t>(j) * 1024 * 10'000'000 / 48000, true});
        in.audio.push_back(std::move(audio));
        in.title = "Test Game";
        in.creationUnixTime = 1'791'763'200;

        rec::VectorSink sink;
        REQUIRE(rec::writeMp4Clip(in, sink));
        CHECK(topLevelBoxes(sink.data) == std::vector<std::string>{"ftyp", "moov", "mdat"});

        const auto path = std::filesystem::temp_directory_path() / (std::string("recordion_test_") + c.codecName + ".mp4");
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(sink.data.data()),
                                                     static_cast<std::streamsize>(sink.data.size()));
#if defined(RECORDION_FFPROBE) && defined(RECORDION_FFMPEG)
        const std::string probe = run(std::string("\"\"") + RECORDION_FFPROBE +
                                      "\" -v error -show_entries stream=codec_name,codec_type,width,height,nb_frames"
                                      " -show_entries format_tags=title -of compact \"" + path.string() + "\"\"");
        INFO(probe);
        CHECK(probe.find(std::string("codec_name=") + c.codecName) != std::string::npos);
        CHECK(probe.find("width=202|height=114") != std::string::npos);
        CHECK(probe.find("nb_frames=4") != std::string::npos);
        CHECK(probe.find("codec_name=aac") != std::string::npos);
        CHECK(probe.find("tag:title=Test Game") != std::string::npos);
        const std::string decode = run(std::string("\"\"") + RECORDION_FFMPEG + "\" -v error -i \"" + path.string() +
                                       "\" -f null -\"");
        CHECK_MESSAGE(decode.empty(), decode);
#endif
        std::filesystem::remove(path);
    }
}

TEST_CASE("hybrid MP4: playable while fragmented, an ordinary MP4 after finish()") {
    const auto adts = parseAdts(readFile("aac_48k_stereo.adts"));
    const auto aus = splitAccessUnits(Codec::H264, readFile("h264_202x114.h264"));
    REQUIRE(aus.size() == 4);
    const auto config = bs::parseVideoConfig(Codec::H264, aus[0].bytes);
    REQUIRE(config);

    const int64_t base = 987'654'321;
    std::vector<std::vector<uint8_t>> videoData(aus.size());
    std::vector<rec::Mp4Sample> video;
    for (size_t i = 0; i < aus.size(); ++i) {
        bs::appendSample(Codec::H264, aus[i].bytes, videoData[i]);
        video.push_back({videoData[i], base + static_cast<int64_t>(i) * 10'000'000 / 60, aus[i].keyframe});
    }
    std::vector<rec::Mp4Sample> audio;
    const int64_t audioStart = base - 2 * 1024 * 10'000'000LL / 48000; // two frames of pre-roll
    for (size_t j = 0; j < adts.frames.size(); ++j)
        audio.push_back({adts.frames[j], audioStart + static_cast<int64_t>(j) * 1024 * 10'000'000 / 48000, true});
    const rec::Mp4AudioTrack track{.name = "Game", .sampleRate = 48000, .channels = 2, .bitrate = 128000,
                                   .audioSpecificConfig = adts.asc};

    rec::VectorSink sink;
    rec::Mp4FragmentedWriter w;
    REQUIRE(w.begin(sink, {Codec::H264, 202, 114, 60, 1, config->configRecord}, {track}, base, {audioStart},
                    "Test Game", 1'791'763'200));
    // Two fragments, each starting on a keyframe; audio split roughly at the same point.
    const size_t audioSplit = audio.size() / 2;
    REQUIRE(w.writeFragment(sink, std::span(video).subspan(0, 2), {std::span(audio).subspan(0, audioSplit)}));
    REQUIRE(w.writeFragment(sink, std::span(video).subspan(2), {std::span(audio).subspan(audioSplit)}));
    CHECK(topLevelBoxes(sink.data) == std::vector<std::string>{"ftyp", "moov", "moof", "mdat", "moof", "mdat"});

    const auto path = std::filesystem::temp_directory_path() / "recordion_hybrid_test.mp4";
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(sink.data.data()),
                                                 static_cast<std::streamsize>(sink.data.size()));
#if defined(RECORDION_FFPROBE) && defined(RECORDION_FFMPEG)
    auto probe = [&] {
        return run(std::string("\"\"") + RECORDION_FFPROBE +
                   "\" -v error -count_frames -show_entries stream=codec_name,nb_read_frames -of compact \"" +
                   path.string() + "\"\"");
    };
    auto decodeErrors = [&] {
        return run(std::string("\"\"") + RECORDION_FFMPEG + "\" -v error -i \"" + path.string() + "\" -f null -\"");
    };
    // As if the recorder had been killed here: still a playable (fragmented) file.
    std::string p = probe();
    INFO(p);
    CHECK(p.find("codec_name=h264|nb_read_frames=4") != std::string::npos);
    CHECK(p.find("codec_name=aac") != std::string::npos);
    CHECK(decodeErrors().empty());
#endif

    REQUIRE(w.finish(path));
    std::ifstream in(path, std::ios::binary);
    const std::vector<uint8_t> finished{std::istreambuf_iterator<char>(in), {}};
    in.close();
    CHECK(topLevelBoxes(finished) == std::vector<std::string>{"ftyp", "free", "free", "mdat", "free", "mdat", "moov"});
    CHECK(w.durationSeconds() == doctest::Approx(4.0 / 60).epsilon(0.01));
#if defined(RECORDION_FFPROBE) && defined(RECORDION_FFMPEG)
    p = probe();
    INFO(p);
    CHECK(p.find("codec_name=h264|nb_read_frames=4") != std::string::npos);
    CHECK(p.find("codec_name=aac") != std::string::npos);
    const std::string errors = decodeErrors();
    CHECK_MESSAGE(errors.empty(), errors);
    const std::string format = run(std::string("\"\"") + RECORDION_FFPROBE +
                                   "\" -v error -show_entries format=duration:format_tags=title -of compact \"" +
                                   path.string() + "\"\"");
    INFO(format);
    CHECK(format.find("tag:title=Test Game") != std::string::npos);
#endif
    std::filesystem::remove(path);
}

TEST_CASE("clip writer rejects input that doesn't start with a keyframe") {
    const std::vector<uint8_t> data{0, 0, 0, 1, 0x41};
    rec::Mp4ClipInput in;
    in.video = {Codec::H264, 16, 16, 60, 1, {}};
    in.videoSamples.push_back({data, 0, false});
    rec::VectorSink sink;
    CHECK_FALSE(rec::writeMp4Clip(in, sink));
}
