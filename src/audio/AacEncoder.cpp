#include "audio/AacEncoder.h"

#include "core/Log.h"

#include <fdk-aac/aacenc_lib.h>

#include <algorithm>
#include <utility>

namespace rec {

AacEncoder::~AacEncoder() {
    if (enc_) aacEncClose(&enc_);
}

bool AacEncoder::init(uint32_t kbps) {
    static constexpr uint32_t kAllowed[] = {96, 128, 160, 192};
    kbps_ = *std::ranges::min_element(kAllowed, {}, [&](uint32_t a) { return a > kbps ? a - kbps : kbps - a; });

    if (aacEncOpen(&enc_, 0x01 /* AAC core only */, kChannels) != AACENC_OK) {
        enc_ = nullptr;
        log::error("aac: could not open the encoder");
        return false;
    }
    // Constant bitrate, no afterburner: the afterburner costs more CPU than the whole Windows encoder
    // did, which is what this encoder replaced.
    const std::pair<AACENC_PARAM, UINT> params[] = {
        {AACENC_AOT, AOT_AAC_LC},
        {AACENC_SAMPLERATE, kSampleRate},
        {AACENC_CHANNELMODE, MODE_2},
        {AACENC_CHANNELORDER, 1}, // WAV order (L, R)
        {AACENC_BITRATEMODE, 0},
        {AACENC_BITRATE, kbps_ * 1000},
        {AACENC_TRANSMUX, TT_MP4_RAW},
        {AACENC_AFTERBURNER, 0},
    };
    for (const auto& [param, value] : params) {
        if (aacEncoder_SetParam(enc_, param, value) != AACENC_OK) {
            log::error("aac: parameter 0x{:X} = {} rejected", static_cast<uint32_t>(param), value);
            return false;
        }
    }
    AACENC_InfoStruct info{};
    if (aacEncEncode(enc_, nullptr, nullptr, nullptr, nullptr) != AACENC_OK || aacEncInfo(enc_, &info) != AACENC_OK ||
        info.frameLength != kFrameSamples) {
        log::error("aac: encoder initialisation failed");
        return false;
    }
    asc_.assign(info.confBuf, info.confBuf + info.confSize);
    priming_ = info.nDelay;
    // Largest AAC frame: 6144 bits per channel.
    out_.resize(std::max<size_t>(info.maxOutBufBytes, 768 * kChannels));
    return true;
}

bool AacEncoder::encode(std::span<const int16_t> pcm, const FrameCallback& onFrame) {
    if (!enc_) return false;
    // The encoder takes what it needs to fill one frame per call and reports how much it consumed.
    while (!pcm.empty()) {
        void* inPtr = const_cast<int16_t*>(pcm.data());
        INT inId = IN_AUDIO_DATA, inSize = static_cast<INT>(pcm.size_bytes()), inElSize = sizeof(int16_t);
        void* outPtr = out_.data();
        INT outId = OUT_BITSTREAM_DATA, outSize = static_cast<INT>(out_.size()), outElSize = 1;
        AACENC_BufDesc in{1, &inPtr, &inId, &inSize, &inElSize};
        AACENC_BufDesc out{1, &outPtr, &outId, &outSize, &outElSize};
        AACENC_InArgs inArgs{};
        inArgs.numInSamples = static_cast<INT>(pcm.size());
        AACENC_OutArgs outArgs{};
        if (const AACENC_ERROR e = aacEncEncode(enc_, &in, &out, &inArgs, &outArgs); e != AACENC_OK) {
            log::error("aac: encode failed (0x{:04X})", static_cast<uint32_t>(e));
            return false;
        }
        if (outArgs.numOutBytes > 0) onFrame({out_.data(), static_cast<size_t>(outArgs.numOutBytes)}, framesOut_++);
        if (outArgs.numInSamples <= 0 && outArgs.numOutBytes <= 0) break; // shouldn't happen; don't spin
        pcm = pcm.subspan(std::min(pcm.size(), static_cast<size_t>(outArgs.numInSamples)));
    }
    return true;
}

bool AacEncoder::drain(const FrameCallback& onFrame) {
    if (!enc_) return false;
    // numInSamples = -1 flushes the delay line, one frame per call, until the encoder reports EOF.
    for (int guard = 0; guard < 16; ++guard) {
        void* outPtr = out_.data();
        INT outId = OUT_BITSTREAM_DATA, outSize = static_cast<INT>(out_.size()), outElSize = 1;
        AACENC_BufDesc in{};
        AACENC_BufDesc out{1, &outPtr, &outId, &outSize, &outElSize};
        AACENC_InArgs inArgs{};
        inArgs.numInSamples = -1;
        AACENC_OutArgs outArgs{};
        const AACENC_ERROR e = aacEncEncode(enc_, &in, &out, &inArgs, &outArgs);
        if (outArgs.numOutBytes > 0) onFrame({out_.data(), static_cast<size_t>(outArgs.numOutBytes)}, framesOut_++);
        if (e == AACENC_ENCODE_EOF) return true;
        if (e != AACENC_OK) {
            log::error("aac: flush failed (0x{:04X})", static_cast<uint32_t>(e));
            return false;
        }
    }
    return true;
}

} // namespace rec
