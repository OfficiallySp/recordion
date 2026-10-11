#pragma once

#include "core/Settings.h"

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace rec::bitstream {

// Calls fn(nal) for each NAL unit in an Annex-B buffer (start codes stripped).
template <class F>
void forEachAnnexBNal(std::span<const uint8_t> data, F&& fn) {
    const uint8_t* const first = data.data();
    const uint8_t* const last = first + data.size();
    // Pointer just past the next 00 00 01 at or after p, or `last`. Scans for the 01 byte with the
    // vectorised memchr instead of testing every byte (this runs on every encoded frame).
    auto nextStart = [last](const uint8_t* p) -> const uint8_t* {
        while (last - p > 2) {
            const auto* one = static_cast<const uint8_t*>(std::memchr(p + 2, 1, static_cast<size_t>(last - (p + 2))));
            if (!one) return last;
            if (one[-1] == 0 && one[-2] == 0) return one + 1;
            p = one - 1; // resume the search just after this 01
        }
        return last;
    };
    const uint8_t* nal = nextStart(first);
    while (nal < last) {
        const uint8_t* next = nextStart(nal);
        const uint8_t* end = next == last ? last : next - 3;
        while (end > nal && end[-1] == 0) --end; // trailing zero of a 4-byte start code
        if (end > nal) fn(std::span<const uint8_t>(nal, static_cast<size_t>(end - nal)));
        nal = next;
    }
}

// Removes emulation-prevention bytes (00 00 03 -> 00 00).
std::vector<uint8_t> unescapeRbsp(std::span<const uint8_t> nal);

// Calls fn(obuType, obuWithHeader, payload) for each OBU in a low-overhead AV1 buffer.
// Returns false on malformed input.
template <class F>
bool forEachObu(std::span<const uint8_t> data, F&& fn);

struct VideoStreamInfo {
    Codec codec = Codec::H264;
    std::vector<uint8_t> configRecord; // avcC / hvcC / av1C box payload
    uint32_t width = 0;                // display size (after cropping)
    uint32_t height = 0;
    uint32_t bitDepth = 8;
    uint32_t profile = 0;              // codec-specific profile_idc / seq_profile
    uint32_t level = 0;                // codec-specific level_idc / seq_level_idx
};

// Builds the container's decoder configuration record from the parameter sets in a keyframe
// (encoders are configured to repeat them on every IDR / key frame).
std::optional<VideoStreamInfo> parseVideoConfig(Codec codec, std::span<const uint8_t> keyframe);

// Appends one encoded access unit to `out` in ISO-BMFF sample format:
//  H.264/HEVC: 4-byte length prefixes; AUD and parameter sets dropped (they live in avcC/hvcC).
//  AV1: temporal delimiters and padding dropped; every OBU gets an explicit obu_size.
void appendSample(Codec codec, std::span<const uint8_t> accessUnit, std::vector<uint8_t>& out);

// LEB128 helpers (AV1).
size_t writeLeb128(uint64_t value, uint8_t* out); // returns bytes written (max 8)
bool readLeb128(std::span<const uint8_t> data, uint64_t& value, size_t& length);

} // namespace rec::bitstream

#include "mux/Bitstream.inl"
