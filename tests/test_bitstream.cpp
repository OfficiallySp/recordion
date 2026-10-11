#include "mux/Bitstream.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>

namespace bs = rec::bitstream;
using rec::Codec;

namespace {

std::vector<uint8_t> readFile(const char* name) {
    std::ifstream in(std::filesystem::path(RECORDION_TEST_DATA_DIR) / name, std::ios::binary);
    REQUIRE_MESSAGE(in, name);
    return {std::istreambuf_iterator<char>(in), {}};
}

// HEVC NAL arrays from an hvcC payload, keyed by NAL type.
std::map<int, std::vector<uint8_t>> hvccArrays(const std::vector<uint8_t>& r) {
    std::map<int, std::vector<uint8_t>> out;
    size_t p = 22;
    const int arrays = r[p++];
    for (int a = 0; a < arrays; ++a) {
        const int type = r[p] & 0x3F;
        const int count = (r[p + 1] << 8) | r[p + 2];
        p += 3;
        for (int n = 0; n < count; ++n) {
            const size_t len = (r[p] << 8) | r[p + 1];
            p += 2;
            if (n == 0) out[type].assign(r.begin() + p, r.begin() + p + len);
            p += len;
        }
    }
    return out;
}

} // namespace

TEST_CASE("H.264: avcC matches ffmpeg, cropped size and profile parsed") {
    const auto stream = readFile("h264_202x114.h264");
    const auto info = bs::parseVideoConfig(Codec::H264, stream);
    REQUIRE(info);
    CHECK(info->width == 202);
    CHECK(info->height == 114);
    CHECK(info->profile == 100); // High
    CHECK(info->level == 13);
    CHECK(info->bitDepth == 8);
    CHECK(info->configRecord == readFile("h264_202x114.avcC"));
}

TEST_CASE("HEVC Main10: hvcC header and parameter sets match ffmpeg") {
    const auto stream = readFile("hevc10_202x114.hevc");
    const auto info = bs::parseVideoConfig(Codec::HEVC, stream);
    REQUIRE(info);
    CHECK(info->width == 202);
    CHECK(info->height == 114);
    CHECK(info->profile == 2); // Main 10
    CHECK(info->bitDepth == 10);

    const auto ref = readFile("hevc10_202x114.hvcC");
    const auto& ours = info->configRecord;
    REQUIRE(ours.size() > 23);
    // version + general profile/tier/compatibility/constraints/level
    CHECK(std::equal(ours.begin(), ours.begin() + 13, ref.begin()));
    // chroma format, luma and chroma bit depth
    CHECK(std::equal(ours.begin() + 16, ours.begin() + 19, ref.begin() + 16));
    CHECK((ours[21] & 0x03) == 3); // 4-byte NAL lengths

    const auto ourArrays = hvccArrays(ours);
    const auto refArrays = hvccArrays(ref);
    for (int type : {32, 33, 34}) {
        REQUIRE(ourArrays.contains(type));
        REQUIRE(refArrays.contains(type));
        CHECK(ourArrays.at(type) == refArrays.at(type));
    }
}

TEST_CASE("AV1: av1C matches ffmpeg") {
    const auto stream = readFile("av1_202x114.obu");
    const auto info = bs::parseVideoConfig(Codec::AV1, stream);
    REQUIRE(info);
    CHECK(info->width == 202);
    CHECK(info->height == 114);
    CHECK(info->profile == 0);
    CHECK(info->bitDepth == 8);
    CHECK(info->configRecord == readFile("av1_202x114.av1C"));
}

TEST_CASE("H.264/HEVC samples are length-prefixed without parameter sets or AUDs") {
    for (auto [codec, file] : {std::pair{Codec::H264, "h264_202x114.h264"}, std::pair{Codec::HEVC, "hevc10_202x114.hevc"}}) {
        const auto stream = readFile(file);
        std::vector<uint8_t> sample;
        bs::appendSample(codec, stream, sample);
        REQUIRE_FALSE(sample.empty());

        size_t p = 0, nals = 0;
        while (p + 4 <= sample.size()) {
            const size_t len = (size_t{sample[p]} << 24) | (sample[p + 1] << 16) | (sample[p + 2] << 8) | sample[p + 3];
            REQUIRE(len > 0);
            REQUIRE(p + 4 + len <= sample.size());
            const uint8_t h = sample[p + 4];
            if (codec == Codec::H264) {
                const int type = h & 0x1F;
                CHECK((type != 7 && type != 8 && type != 9));
            } else {
                const int type = (h >> 1) & 0x3F;
                CHECK((type < 32 || type > 35));
            }
            p += 4 + len;
            ++nals;
        }
        CHECK(p == sample.size());
        CHECK(nals >= 4); // at least one slice per frame
    }
}

TEST_CASE("AV1 samples drop temporal delimiters and always carry obu_size") {
    const auto stream = readFile("av1_202x114.obu");
    std::vector<uint8_t> sample;
    bs::appendSample(Codec::AV1, stream, sample);
    REQUIRE_FALSE(sample.empty());
    int obus = 0;
    const bool ok = bs::forEachObu(sample, [&](uint8_t type, std::span<const uint8_t> obu, std::span<const uint8_t>) {
        CHECK(type != 2);
        CHECK(type != 15);
        CHECK((obu[0] & 0x02) != 0);
        ++obus;
    });
    CHECK(ok);
    CHECK(obus > 0);
}

TEST_CASE("LEB128 round-trips") {
    for (uint64_t v : {0ull, 1ull, 127ull, 128ull, 300ull, 16383ull, 16384ull, 0xFFFFFFFFull}) {
        uint8_t buf[8];
        const size_t n = bs::writeLeb128(v, buf);
        uint64_t back = 0;
        size_t len = 0;
        REQUIRE(bs::readLeb128({buf, n}, back, len));
        CHECK(back == v);
        CHECK(len == n);
    }
}

TEST_CASE("Annex-B splitting handles 3- and 4-byte start codes") {
    const std::vector<uint8_t> data{0, 0, 0, 1, 0x67, 0xAA, 0, 0, 1, 0x68, 0xBB, 0xCC, 0, 0, 0, 1, 0x65, 0x01};
    std::vector<std::vector<uint8_t>> nals;
    bs::forEachAnnexBNal(data, [&](std::span<const uint8_t> n) { nals.emplace_back(n.begin(), n.end()); });
    REQUIRE(nals.size() == 3);
    CHECK(nals[0] == std::vector<uint8_t>{0x67, 0xAA});
    CHECK(nals[1] == std::vector<uint8_t>{0x68, 0xBB, 0xCC});
    CHECK(nals[2] == std::vector<uint8_t>{0x65, 0x01});
}

TEST_CASE("emulation prevention bytes are removed") {
    const std::vector<uint8_t> nal{0x00, 0x00, 0x03, 0x01, 0x00, 0x00, 0x03, 0x00, 0x03};
    CHECK(bs::unescapeRbsp(nal) == std::vector<uint8_t>{0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03});
}
