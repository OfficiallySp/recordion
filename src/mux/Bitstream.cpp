#include "mux/Bitstream.h"

#include "mux/BitReader.h"

namespace rec::bitstream {
namespace {

// --- H.264 -------------------------------------------------------------------------------------

constexpr uint8_t kH264Sps = 7, kH264Pps = 8, kH264Aud = 9;

void skipScalingList(BitReader& br, int size) {
    int last = 8, next = 8;
    for (int j = 0; j < size; ++j) {
        if (next != 0) next = (last + br.se() + 256) % 256;
        last = next == 0 ? last : next;
    }
}

bool highProfile(uint32_t profile) {
    switch (profile) {
    case 100: case 110: case 122: case 244: case 44: case 83: case 86: case 118: case 128: case 138: case 139:
    case 134: case 135:
        return true;
    default:
        return false;
    }
}

struct H264Sps {
    uint32_t profile = 0, compat = 0, level = 0;
    uint32_t chromaFormat = 1, bitDepthLuma = 8, bitDepthChroma = 8;
    uint32_t width = 0, height = 0;
};

std::optional<H264Sps> parseH264Sps(std::span<const uint8_t> nal) {
    const auto rbsp = unescapeRbsp(nal.subspan(1)); // skip NAL header
    BitReader br(rbsp);
    H264Sps s;
    s.profile = br.bits(8);
    s.compat = br.bits(8);
    s.level = br.bits(8);
    br.ue(); // seq_parameter_set_id
    bool separateColourPlane = false;
    if (highProfile(s.profile)) {
        s.chromaFormat = br.ue();
        if (s.chromaFormat == 3) separateColourPlane = br.flag();
        s.bitDepthLuma = br.ue() + 8;
        s.bitDepthChroma = br.ue() + 8;
        br.flag(); // qpprime_y_zero_transform_bypass_flag
        if (br.flag()) { // seq_scaling_matrix_present_flag
            const int lists = s.chromaFormat != 3 ? 8 : 12;
            for (int i = 0; i < lists; ++i)
                if (br.flag()) skipScalingList(br, i < 6 ? 16 : 64);
        }
    }
    br.ue(); // log2_max_frame_num_minus4
    const uint32_t pocType = br.ue();
    if (pocType == 0) {
        br.ue();
    } else if (pocType == 1) {
        br.flag();
        br.se();
        br.se();
        const uint32_t cycle = br.ue();
        for (uint32_t i = 0; i < cycle && !br.overrun(); ++i) br.se();
    }
    br.ue();   // max_num_ref_frames
    br.flag(); // gaps_in_frame_num_value_allowed_flag
    const uint32_t widthMbs = br.ue() + 1;
    const uint32_t heightMapUnits = br.ue() + 1;
    const bool frameMbsOnly = br.flag();
    if (!frameMbsOnly) br.flag(); // mb_adaptive_frame_field_flag
    br.flag();                    // direct_8x8_inference_flag
    uint32_t cl = 0, cr = 0, ct = 0, cb = 0;
    if (br.flag()) {
        cl = br.ue();
        cr = br.ue();
        ct = br.ue();
        cb = br.ue();
    }
    if (br.overrun()) return std::nullopt;

    const uint32_t chroma = separateColourPlane ? 0 : s.chromaFormat;
    const uint32_t subW = (chroma == 1 || chroma == 2) ? 2 : 1;
    const uint32_t subH = chroma == 1 ? 2 : 1;
    const uint32_t cropX = chroma == 0 ? 1 : subW;
    const uint32_t cropY = (chroma == 0 ? 1 : subH) * (frameMbsOnly ? 1 : 2);
    s.width = widthMbs * 16 - cropX * (cl + cr);
    s.height = (frameMbsOnly ? 1 : 2) * heightMapUnits * 16 - cropY * (ct + cb);
    return s;
}

std::optional<VideoStreamInfo> parseH264Config(std::span<const uint8_t> au) {
    std::span<const uint8_t> sps, pps;
    forEachAnnexBNal(au, [&](std::span<const uint8_t> nal) {
        const uint8_t type = nal[0] & 0x1F;
        if (type == kH264Sps && sps.empty()) sps = nal;
        else if (type == kH264Pps && pps.empty()) pps = nal;
    });
    if (sps.size() < 4 || pps.empty()) return std::nullopt;
    const auto parsed = parseH264Sps(sps);
    if (!parsed) return std::nullopt;

    VideoStreamInfo info;
    info.codec = Codec::H264;
    info.width = parsed->width;
    info.height = parsed->height;
    info.bitDepth = parsed->bitDepthLuma;
    info.profile = parsed->profile;
    info.level = parsed->level;
    auto& r = info.configRecord;
    r = {1, static_cast<uint8_t>(parsed->profile), static_cast<uint8_t>(parsed->compat),
         static_cast<uint8_t>(parsed->level), 0xFF /* 4-byte lengths */, 0xE1 /* 1 SPS */};
    r.push_back(static_cast<uint8_t>(sps.size() >> 8));
    r.push_back(static_cast<uint8_t>(sps.size()));
    r.insert(r.end(), sps.begin(), sps.end());
    r.push_back(1); // 1 PPS
    r.push_back(static_cast<uint8_t>(pps.size() >> 8));
    r.push_back(static_cast<uint8_t>(pps.size()));
    r.insert(r.end(), pps.begin(), pps.end());
    if (highProfile(parsed->profile)) {
        r.push_back(static_cast<uint8_t>(0xFC | parsed->chromaFormat));
        r.push_back(static_cast<uint8_t>(0xF8 | (parsed->bitDepthLuma - 8)));
        r.push_back(static_cast<uint8_t>(0xF8 | (parsed->bitDepthChroma - 8)));
        r.push_back(0); // no SPS extensions
    }
    return info;
}

// --- HEVC --------------------------------------------------------------------------------------

constexpr uint8_t kHevcVps = 32, kHevcSps = 33, kHevcPps = 34, kHevcAud = 35;

uint8_t hevcType(std::span<const uint8_t> nal) { return (nal[0] >> 1) & 0x3F; }

std::optional<VideoStreamInfo> parseHevcConfig(std::span<const uint8_t> au) {
    std::span<const uint8_t> vps, sps, pps;
    forEachAnnexBNal(au, [&](std::span<const uint8_t> nal) {
        if (nal.size() < 3) return;
        switch (hevcType(nal)) {
        case kHevcVps: if (vps.empty()) vps = nal; break;
        case kHevcSps: if (sps.empty()) sps = nal; break;
        case kHevcPps: if (pps.empty()) pps = nal; break;
        default: break;
        }
    });
    if (vps.empty() || sps.empty() || pps.empty()) return std::nullopt;

    const auto rbsp = unescapeRbsp(sps.subspan(2)); // skip 2-byte NAL header
    if (rbsp.size() < 13) return std::nullopt;
    BitReader br(rbsp);
    br.bits(4); // sps_video_parameter_set_id
    const uint32_t maxSubLayersMinus1 = br.bits(3);
    const bool temporalIdNesting = br.flag();
    // profile_tier_level(1, maxSubLayersMinus1): the 12-byte general part is copied verbatim below.
    br.bits(3); // general_profile_space, general_tier_flag
    const uint32_t profileIdc = br.bits(5);
    br.skip(32 + 48);
    const uint32_t levelIdc = br.bits(8);
    std::vector<std::pair<bool, bool>> subLayer(maxSubLayersMinus1);
    for (auto& [profilePresent, levelPresent] : subLayer) {
        profilePresent = br.flag();
        levelPresent = br.flag();
    }
    if (maxSubLayersMinus1 > 0)
        for (uint32_t i = maxSubLayersMinus1; i < 8; ++i) br.skip(2);
    for (const auto& [profilePresent, levelPresent] : subLayer) {
        if (profilePresent) br.skip(88);
        if (levelPresent) br.skip(8);
    }
    br.ue(); // sps_seq_parameter_set_id
    const uint32_t chromaFormat = br.ue();
    if (chromaFormat == 3) br.flag(); // separate_colour_plane_flag
    const uint32_t picWidth = br.ue();
    const uint32_t picHeight = br.ue();
    uint32_t cl = 0, cr = 0, ct = 0, cb = 0;
    if (br.flag()) { // conformance_window_flag
        cl = br.ue();
        cr = br.ue();
        ct = br.ue();
        cb = br.ue();
    }
    const uint32_t bitDepthLuma = br.ue() + 8;
    const uint32_t bitDepthChroma = br.ue() + 8;
    if (br.overrun()) return std::nullopt;

    const uint32_t subW = (chromaFormat == 1 || chromaFormat == 2) ? 2 : 1;
    const uint32_t subH = chromaFormat == 1 ? 2 : 1;

    VideoStreamInfo info;
    info.codec = Codec::HEVC;
    info.width = picWidth - subW * (cl + cr);
    info.height = picHeight - subH * (ct + cb);
    info.bitDepth = bitDepthLuma;
    info.profile = profileIdc;
    info.level = levelIdc;

    // HEVCDecoderConfigurationRecord (ISO/IEC 14496-15 8.3.3.1).
    auto& r = info.configRecord;
    r.push_back(1);
    r.insert(r.end(), rbsp.begin() + 1, rbsp.begin() + 13); // profile/tier/compat/constraints/level
    r.push_back(0xF0); // reserved + min_spatial_segmentation_idc = 0
    r.push_back(0x00);
    r.push_back(0xFC); // reserved + parallelismType = 0
    r.push_back(static_cast<uint8_t>(0xFC | chromaFormat));
    r.push_back(static_cast<uint8_t>(0xF8 | (bitDepthLuma - 8)));
    r.push_back(static_cast<uint8_t>(0xF8 | (bitDepthChroma - 8)));
    r.push_back(0); // avgFrameRate
    r.push_back(0);
    r.push_back(static_cast<uint8_t>(((maxSubLayersMinus1 + 1) << 3) | (temporalIdNesting ? 0x04 : 0) | 0x03));
    r.push_back(3); // numOfArrays
    for (auto nal : {vps, sps, pps}) {
        r.push_back(static_cast<uint8_t>(0x80 | hevcType(nal))); // array_completeness = 1 (hvc1)
        r.push_back(0);
        r.push_back(1); // numNalus
        r.push_back(static_cast<uint8_t>(nal.size() >> 8));
        r.push_back(static_cast<uint8_t>(nal.size()));
        r.insert(r.end(), nal.begin(), nal.end());
    }
    return info;
}

// --- AV1 ---------------------------------------------------------------------------------------

constexpr uint8_t kObuSequenceHeader = 1, kObuTemporalDelimiter = 2, kObuPadding = 15;

void appendNormalizedObu(std::span<const uint8_t> obu, std::span<const uint8_t> payload, std::vector<uint8_t>& out) {
    const uint8_t header = obu[0];
    out.push_back(static_cast<uint8_t>(header | 0x02)); // obu_has_size_field = 1
    if (header & 0x04) out.push_back(obu[1]);           // extension header
    uint8_t leb[8];
    const size_t n = writeLeb128(payload.size(), leb);
    out.insert(out.end(), leb, leb + n);
    out.insert(out.end(), payload.begin(), payload.end());
}

std::optional<VideoStreamInfo> parseAv1Config(std::span<const uint8_t> au) {
    std::span<const uint8_t> seqObu, seqPayload;
    if (!forEachObu(au, [&](uint8_t type, std::span<const uint8_t> obu, std::span<const uint8_t> payload) {
            if (type == kObuSequenceHeader && seqObu.empty()) {
                seqObu = obu;
                seqPayload = payload;
            }
        }) ||
        seqObu.empty())
        return std::nullopt;

    BitReader br(seqPayload);
    const uint32_t seqProfile = br.bits(3);
    br.flag(); // still_picture
    const bool reducedStill = br.flag();
    uint32_t seqLevel0 = 0, seqTier0 = 0;
    if (reducedStill) {
        seqLevel0 = br.bits(5);
    } else {
        bool decoderModelInfo = false;
        uint32_t bufferDelayLength = 0;
        if (br.flag()) { // timing_info_present_flag
            br.bits(32); // num_units_in_display_tick
            br.bits(32); // time_scale
            if (br.flag()) br.uvlc(); // equal_picture_interval -> num_ticks_per_picture_minus_1
            decoderModelInfo = br.flag();
            if (decoderModelInfo) {
                bufferDelayLength = br.bits(5) + 1;
                br.bits(32); // num_units_in_decoding_tick
                br.bits(5);  // buffer_removal_time_length_minus_1
                br.bits(5);  // frame_presentation_time_length_minus_1
            }
        }
        const bool initialDisplayDelay = br.flag();
        const uint32_t opCount = br.bits(5) + 1;
        for (uint32_t i = 0; i < opCount; ++i) {
            br.bits(12); // operating_point_idc
            const uint32_t level = br.bits(5);
            const uint32_t tierBit = level > 7 ? br.bits(1) : 0;
            if (i == 0) {
                seqLevel0 = level;
                seqTier0 = tierBit;
            }
            if (decoderModelInfo && br.flag()) {
                br.skip(bufferDelayLength * 2); // decoder/encoder_buffer_delay
                br.flag();                      // low_delay_mode_flag
            }
            if (initialDisplayDelay && br.flag()) br.bits(4);
        }
    }
    const int widthBits = static_cast<int>(br.bits(4)) + 1;
    const int heightBits = static_cast<int>(br.bits(4)) + 1;
    const uint32_t maxWidth = br.bits(widthBits) + 1;
    const uint32_t maxHeight = br.bits(heightBits) + 1;
    if (!reducedStill && br.flag()) { // frame_id_numbers_present_flag
        br.bits(4);
        br.bits(3);
    }
    br.flag(); // use_128x128_superblock
    br.flag(); // enable_filter_intra
    br.flag(); // enable_intra_edge_filter
    if (!reducedStill) {
        br.flag(); // enable_interintra_compound
        br.flag(); // enable_masked_compound
        br.flag(); // enable_warped_motion
        br.flag(); // enable_dual_filter
        const bool orderHint = br.flag();
        if (orderHint) {
            br.flag(); // enable_jnt_comp
            br.flag(); // enable_ref_frame_mvs
        }
        const uint32_t forceScreenContentTools = br.flag() ? 2 : br.bits(1);
        if (forceScreenContentTools > 0 && !br.flag()) br.bits(1); // seq_force_integer_mv
        if (orderHint) br.bits(3);
    }
    br.flag(); // enable_superres
    br.flag(); // enable_cdef
    br.flag(); // enable_restoration
    // color_config()
    const bool highBitdepth = br.flag();
    bool twelveBit = false;
    if (seqProfile == 2 && highBitdepth) twelveBit = br.flag();
    const bool mono = seqProfile == 1 ? false : br.flag();
    uint32_t cp = 2, tc = 2, mc = 2;
    if (br.flag()) { // color_description_present_flag
        cp = br.bits(8);
        tc = br.bits(8);
        mc = br.bits(8);
    }
    uint32_t subX = 1, subY = 1, chromaPos = 0;
    if (mono) {
        br.flag(); // color_range
    } else if (cp == 1 && tc == 13 && mc == 0) { // sRGB: 4:4:4
        subX = subY = 0;
    } else {
        br.flag(); // color_range
        if (seqProfile == 0) {
            subX = subY = 1;
        } else if (seqProfile == 1) {
            subX = subY = 0;
        } else if (twelveBit) {
            subX = br.bits(1);
            subY = subX ? br.bits(1) : 0;
        } else {
            subX = 1;
            subY = 0;
        }
        if (subX && subY) chromaPos = br.bits(2);
    }
    if (br.overrun()) return std::nullopt;

    VideoStreamInfo info;
    info.codec = Codec::AV1;
    info.width = maxWidth; // the real display size comes from the encoder config
    info.height = maxHeight;
    info.bitDepth = twelveBit ? 12 : highBitdepth ? 10 : 8;
    info.profile = seqProfile;
    info.level = seqLevel0;

    // AV1CodecConfigurationRecord (AV1-ISOBMFF 2.3.3).
    auto& r = info.configRecord;
    r.push_back(0x81); // marker + version 1
    r.push_back(static_cast<uint8_t>((seqProfile << 5) | seqLevel0));
    r.push_back(static_cast<uint8_t>((seqTier0 << 7) | (highBitdepth << 6) | (twelveBit << 5) | (mono << 4) |
                                     (subX << 3) | (subY << 2) | chromaPos));
    r.push_back(0); // no initial_presentation_delay
    appendNormalizedObu(seqObu, seqPayload, r);
    return info;
}

} // namespace

std::vector<uint8_t> unescapeRbsp(std::span<const uint8_t> nal) {
    std::vector<uint8_t> out;
    out.reserve(nal.size());
    int zeros = 0;
    for (uint8_t b : nal) {
        if (zeros >= 2 && b == 3) {
            zeros = 0;
            continue;
        }
        zeros = b == 0 ? zeros + 1 : 0;
        out.push_back(b);
    }
    return out;
}

std::optional<VideoStreamInfo> parseVideoConfig(Codec codec, std::span<const uint8_t> keyframe) {
    switch (codec) {
    case Codec::H264: return parseH264Config(keyframe);
    case Codec::HEVC: return parseHevcConfig(keyframe);
    case Codec::AV1: return parseAv1Config(keyframe);
    }
    return std::nullopt;
}

void appendSample(Codec codec, std::span<const uint8_t> accessUnit, std::vector<uint8_t>& out) {
    if (codec == Codec::AV1) {
        forEachObu(accessUnit, [&](uint8_t type, std::span<const uint8_t> obu, std::span<const uint8_t> payload) {
            if (type == kObuTemporalDelimiter || type == kObuPadding) return;
            appendNormalizedObu(obu, payload, out);
        });
        return;
    }
    forEachAnnexBNal(accessUnit, [&](std::span<const uint8_t> nal) {
        if (codec == Codec::H264) {
            const uint8_t type = nal[0] & 0x1F;
            if (type == kH264Sps || type == kH264Pps || type == kH264Aud) return;
        } else {
            if (nal.size() < 2) return;
            const uint8_t type = hevcType(nal);
            if (type == kHevcVps || type == kHevcSps || type == kHevcPps || type == kHevcAud) return;
        }
        const auto n = static_cast<uint32_t>(nal.size());
        out.push_back(static_cast<uint8_t>(n >> 24));
        out.push_back(static_cast<uint8_t>(n >> 16));
        out.push_back(static_cast<uint8_t>(n >> 8));
        out.push_back(static_cast<uint8_t>(n));
        out.insert(out.end(), nal.begin(), nal.end());
    });
}

size_t writeLeb128(uint64_t value, uint8_t* out) {
    size_t n = 0;
    do {
        uint8_t byte = value & 0x7F;
        value >>= 7;
        if (value) byte |= 0x80;
        out[n++] = byte;
    } while (value && n < 8);
    return n;
}

bool readLeb128(std::span<const uint8_t> data, uint64_t& value, size_t& length) {
    value = 0;
    for (size_t i = 0; i < 8 && i < data.size(); ++i) {
        value |= static_cast<uint64_t>(data[i] & 0x7F) << (7 * i);
        if (!(data[i] & 0x80)) {
            length = i + 1;
            return true;
        }
    }
    return false;
}

} // namespace rec::bitstream
