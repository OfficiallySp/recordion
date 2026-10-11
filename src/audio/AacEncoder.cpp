#include "audio/AacEncoder.h"

#include "core/Log.h"

#include <mfapi.h>
#include <mferror.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <cstring>

namespace rec {
namespace {

LONGLONG samplesTo100ns(uint64_t samples) { return static_cast<LONGLONG>(samples * 10'000'000 / AacEncoder::kSampleRate); }

} // namespace

AacEncoder::~AacEncoder() {
    mft_ = nullptr;
    if (mfStarted_) MFShutdown();
}

bool AacEncoder::init(uint32_t kbps) {
    // The Windows AAC encoder accepts exactly these average bitrates.
    static constexpr uint32_t kAllowed[] = {96, 128, 160, 192};
    kbps_ = *std::ranges::min_element(kAllowed, {}, [&](uint32_t a) { return a > kbps ? a - kbps : kbps - a; });

    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return false;
    mfStarted_ = true;
    if (FAILED(CoCreateInstance(CLSID_AACMFTEncoder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(mft_.put())))) {
        log::error("aac: Windows AAC encoder not available");
        return false;
    }

    winrt::com_ptr<IMFMediaType> in;
    MFCreateMediaType(in.put());
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kSampleRate);
    in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
    in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, kChannels * 2);
    in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kSampleRate * kChannels * 2);
    if (FAILED(mft_->SetInputType(0, in.get(), 0))) {
        log::error("aac: input type rejected");
        return false;
    }

    winrt::com_ptr<IMFMediaType> out;
    MFCreateMediaType(out.put());
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kSampleRate);
    out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
    out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kbps_ * 1000 / 8);
    out->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);                    // raw frames, no ADTS headers
    out->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29); // AAC-LC
    if (FAILED(mft_->SetOutputType(0, out.get(), 0))) {
        log::error("aac: output type rejected");
        return false;
    }

    // MF_MT_USER_DATA is the HEAACWAVEINFO tail (12 bytes) followed by the AudioSpecificConfig.
    winrt::com_ptr<IMFMediaType> current;
    UINT8* blob = nullptr;
    UINT32 blobSize = 0;
    if (SUCCEEDED(mft_->GetOutputCurrentType(0, current.put())) &&
        SUCCEEDED(current->GetAllocatedBlob(MF_MT_USER_DATA, &blob, &blobSize))) {
        if (blobSize > 12) asc_.assign(blob + 12, blob + blobSize);
        CoTaskMemFree(blob);
    }
    if (asc_.empty()) asc_ = {0x11, 0x90}; // AAC-LC, 48 kHz, stereo

    MFT_OUTPUT_STREAM_INFO info{};
    mft_->GetOutputStreamInfo(0, &info);
    outputBufferSize_ = std::max<uint32_t>(info.cbSize, 8192);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

bool AacEncoder::encode(std::span<const int16_t> pcm, const FrameCallback& onFrame) {
    if (!mft_ || pcm.empty()) return mft_ != nullptr;
    const DWORD bytes = static_cast<DWORD>(pcm.size_bytes());
    winrt::com_ptr<IMFMediaBuffer> buffer;
    winrt::com_ptr<IMFSample> sample;
    if (FAILED(MFCreateMemoryBuffer(bytes, buffer.put())) || FAILED(MFCreateSample(sample.put()))) return false;
    BYTE* dst = nullptr;
    buffer->Lock(&dst, nullptr, nullptr);
    std::memcpy(dst, pcm.data(), bytes);
    buffer->Unlock();
    buffer->SetCurrentLength(bytes);
    sample->AddBuffer(buffer.get());
    const uint64_t frames = pcm.size() / kChannels;
    sample->SetSampleTime(samplesTo100ns(inputSamples_));
    sample->SetSampleDuration(samplesTo100ns(inputSamples_ + frames) - samplesTo100ns(inputSamples_));
    inputSamples_ += frames;

    HRESULT hr = mft_->ProcessInput(0, sample.get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        if (!pull(onFrame)) return false;
        hr = mft_->ProcessInput(0, sample.get(), 0);
    }
    if (FAILED(hr)) {
        log::error("aac: ProcessInput failed (0x{:08X})", static_cast<uint32_t>(hr));
        return false;
    }
    return pull(onFrame);
}

bool AacEncoder::pull(const FrameCallback& onFrame) {
    for (;;) {
        winrt::com_ptr<IMFMediaBuffer> buffer;
        winrt::com_ptr<IMFSample> sample;
        MFCreateMemoryBuffer(outputBufferSize_, buffer.put());
        MFCreateSample(sample.put());
        sample->AddBuffer(buffer.get());
        MFT_OUTPUT_DATA_BUFFER out{0, sample.get(), 0, nullptr};
        DWORD status = 0;
        const HRESULT hr = mft_->ProcessOutput(0, 1, &out, &status);
        if (out.pEvents) out.pEvents->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
        if (FAILED(hr)) {
            log::error("aac: ProcessOutput failed (0x{:08X})", static_cast<uint32_t>(hr));
            return false;
        }
        BYTE* data = nullptr;
        DWORD length = 0;
        buffer->Lock(&data, nullptr, &length);
        if (length) onFrame({data, length}, framesOut_++);
        buffer->Unlock();
    }
}

bool AacEncoder::drain(const FrameCallback& onFrame) {
    if (!mft_) return false;
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    mft_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    return pull(onFrame);
}

} // namespace rec
