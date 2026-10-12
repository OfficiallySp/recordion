#include "encode/NvencEncoder.h"

#include "core/Log.h"

#include <algorithm>

namespace rec {
namespace {

using GetMaxVersionFn = NVENCSTATUS(NVENCAPI*)(uint32_t*);
using CreateInstanceFn = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);

struct NvencRuntime {
    HMODULE module = nullptr;
    CreateInstanceFn createInstance = nullptr;
};

const NvencRuntime& runtime() {
    static const NvencRuntime rt = [] {
        NvencRuntime r;
        r.module = LoadLibraryExW(L"nvEncodeAPI64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!r.module) return r;
        auto maxVersion = reinterpret_cast<GetMaxVersionFn>(GetProcAddress(r.module, "NvEncodeAPIGetMaxSupportedVersion"));
        auto create = reinterpret_cast<CreateInstanceFn>(GetProcAddress(r.module, "NvEncodeAPICreateInstance"));
        uint32_t version = 0;
        if (!maxVersion || !create || maxVersion(&version) != NV_ENC_SUCCESS) return r;
        const uint32_t needed = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
        if (version < needed) {
            log::warn("nvenc: the driver supports API {}.{}, {}.{} is needed; update the NVIDIA driver", version >> 4,
                      version & 0xF, NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);
            return r;
        }
        r.createInstance = create;
        return r;
    }();
    return rt;
}

GUID presetFor(QualityPreset p) {
    switch (p) {
    case QualityPreset::Speed: return NV_ENC_PRESET_P2_GUID;
    case QualityPreset::Balanced: return NV_ENC_PRESET_P4_GUID;
    case QualityPreset::Quality: return NV_ENC_PRESET_P5_GUID;
    case QualityPreset::MaxQuality: return NV_ENC_PRESET_P7_GUID;
    }
    return NV_ENC_PRESET_P4_GUID;
}

template <class Vui>
void setBt709(Vui& vui) {
    vui.videoSignalTypePresentFlag = 1;
    vui.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
    vui.videoFullRangeFlag = 0;
    vui.colourDescriptionPresentFlag = 1;
    vui.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
    vui.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
    vui.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
}

} // namespace

bool NvencEncoder::runtimeAvailable() { return runtime().createInstance != nullptr; }

NvencEncoder::~NvencEncoder() { shutdown(); }

bool NvencEncoder::init(const GpuDevice& gpu, const EncoderConfig& config) {
    shutdown();
    if (!runtimeAvailable()) return false;
    if (config.rgbInput) return false; // see encodeExternal()
    config_ = config;
    format_ = config.tenBit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    context_.copy_from(gpu.context.get());

    api_ = {};
    api_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (runtime().createInstance(&api_) != NV_ENC_SUCCESS) return false;

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};
    open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    open.device = gpu.device.get();
    open.apiVersion = NVENCAPI_VERSION;
    if (api_.nvEncOpenEncodeSessionEx(&open, &encoder_) != NV_ENC_SUCCESS) {
        log::error("nvenc: could not open an encode session (too many sessions, or no NVENC on this GPU)");
        encoder_ = nullptr;
        return false;
    }

    const GUID codec = config.codec == Codec::H264 ? NV_ENC_CODEC_H264_GUID
                       : config.codec == Codec::HEVC ? NV_ENC_CODEC_HEVC_GUID
                                                     : NV_ENC_CODEC_AV1_GUID;
    const GUID preset = presetFor(config.preset);
    const auto tuning = NV_ENC_TUNING_INFO_HIGH_QUALITY;
    NV_ENC_PRESET_CONFIG presetConfig{};
    presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
    presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;
    if (api_.nvEncGetEncodePresetConfigEx(encoder_, codec, preset, tuning, &presetConfig) != NV_ENC_SUCCESS) {
        log::error("nvenc: this GPU can't encode the selected codec");
        shutdown();
        return false;
    }
    NV_ENC_CONFIG cfg = presetConfig.presetCfg;
    cfg.version = NV_ENC_CONFIG_VER;
    cfg.gopLength = config.gopFrames;
    cfg.frameIntervalP = 1; // no B-frames

    auto& rc = cfg.rcParams;
    const bool maxQuality = config.preset == QualityPreset::MaxQuality;
    rc.rateControlMode = config.rateControl == RateControl::CBR   ? NV_ENC_PARAMS_RC_CBR
                         : config.rateControl == RateControl::CQP ? NV_ENC_PARAMS_RC_CONSTQP
                                                                  : NV_ENC_PARAMS_RC_VBR;
    rc.averageBitRate = config.bitrateKbps * 1000;
    rc.maxBitRate = std::max(config.maxBitrateKbps, config.bitrateKbps) * 1000;
    rc.vbvBufferSize = rc.maxBitRate; // one second
    const uint32_t qp = config.codec == Codec::AV1 ? std::min(255u, config.cqLevel * 255 / 51) : config.cqLevel;
    rc.constQP = {qp, qp, qp};
    // Lookahead and temporal AQ run on CUDA cores, i.e. the game's; spatial AQ only for Max quality.
    rc.enableLookahead = 0;
    rc.enableTemporalAQ = 0;
    rc.enableAQ = maxQuality ? 1 : 0;

    switch (config.codec) {
    case Codec::H264: {
        auto& h = cfg.encodeCodecConfig.h264Config;
        cfg.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
        h.idrPeriod = config.gopFrames;
        h.repeatSPSPPS = 1;
        h.outputAUD = 0;
        h.chromaFormatIDC = 1;
        setBt709(h.h264VUIParameters);
        break;
    }
    case Codec::HEVC: {
        auto& h = cfg.encodeCodecConfig.hevcConfig;
        cfg.profileGUID = config.tenBit ? NV_ENC_HEVC_PROFILE_MAIN10_GUID : NV_ENC_HEVC_PROFILE_MAIN_GUID;
        h.idrPeriod = config.gopFrames;
        h.repeatSPSPPS = 1;
        h.outputAUD = 0;
        h.chromaFormatIDC = 1;
        h.inputBitDepth = h.outputBitDepth = config.tenBit ? NV_ENC_BIT_DEPTH_10 : NV_ENC_BIT_DEPTH_8;
        setBt709(h.hevcVUIParameters);
        break;
    }
    case Codec::AV1: {
        auto& a = cfg.encodeCodecConfig.av1Config;
        cfg.profileGUID = NV_ENC_AV1_PROFILE_MAIN_GUID;
        a.idrPeriod = config.gopFrames;
        a.repeatSeqHdr = 1;
        a.outputAnnexBFormat = 0; // low-overhead OBUs, as MP4 wants
        a.chromaFormatIDC = 1;
        a.inputBitDepth = a.outputBitDepth = config.tenBit ? NV_ENC_BIT_DEPTH_10 : NV_ENC_BIT_DEPTH_8;
        a.colorPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
        a.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
        a.matrixCoefficients = NV_ENC_VUI_MATRIX_COEFFS_BT709;
        a.colorRange = 0;
        break;
    }
    }

    NV_ENC_INITIALIZE_PARAMS init{};
    init.version = NV_ENC_INITIALIZE_PARAMS_VER;
    init.encodeGUID = codec;
    init.presetGUID = preset;
    init.tuningInfo = tuning;
    init.encodeWidth = init.maxEncodeWidth = init.darWidth = config.width;
    init.encodeHeight = init.maxEncodeHeight = init.darHeight = config.height;
    init.frameRateNum = config.fpsNum;
    init.frameRateDen = config.fpsDen;
    init.enablePTD = 1;
    init.enableEncodeAsync = 1;
    init.encodeConfig = &cfg;
    if (const NVENCSTATUS s = api_.nvEncInitializeEncoder(encoder_, &init); s != NV_ENC_SUCCESS) {
        log::error("nvenc: initialisation failed ({})", static_cast<int>(s));
        shutdown();
        return false;
    }

    D3D11_TEXTURE2D_DESC d{};
    d.Width = config.width;
    d.Height = config.height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = format_;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    for (auto& slot : slots_) {
        if (FAILED(gpu.device->CreateTexture2D(&d, nullptr, slot.texture.put()))) {
            shutdown();
            return false;
        }
        NV_ENC_REGISTER_RESOURCE reg{};
        reg.version = NV_ENC_REGISTER_RESOURCE_VER;
        reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        reg.resourceToRegister = slot.texture.get();
        reg.width = config.width;
        reg.height = config.height;
        reg.bufferFormat = config.tenBit ? NV_ENC_BUFFER_FORMAT_YUV420_10BIT : NV_ENC_BUFFER_FORMAT_NV12;
        reg.bufferUsage = NV_ENC_INPUT_IMAGE;
        if (api_.nvEncRegisterResource(encoder_, &reg) != NV_ENC_SUCCESS) {
            log::error("nvenc: could not register an input texture");
            shutdown();
            return false;
        }
        slot.registered = reg.registeredResource;
    }
    for (auto& out : outputs_) {
        NV_ENC_CREATE_BITSTREAM_BUFFER bb{};
        bb.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        out.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        NV_ENC_EVENT_PARAMS ev{};
        ev.version = NV_ENC_EVENT_PARAMS_VER;
        ev.completionEvent = out.event;
        if (api_.nvEncCreateBitstreamBuffer(encoder_, &bb) != NV_ENC_SUCCESS ||
            api_.nvEncRegisterAsyncEvent(encoder_, &ev) != NV_ENC_SUCCESS) {
            shutdown();
            return false;
        }
        out.bitstream = bb.bitstreamBuffer;
    }
    log::info("nvenc: {} {}x{} @ {}/{} {}-bit, {} kbps", config.codec == Codec::H264 ? "H.264" : config.codec == Codec::HEVC ? "HEVC" : "AV1",
              config.width, config.height, config.fpsNum, config.fpsDen, config.tenBit ? 10 : 8, config.bitrateKbps);
    return true;
}

std::optional<InputSurface> NvencEncoder::acquireSurface() {
    for (uint32_t i = 0; i < kPoolSize; ++i) {
        const uint32_t index = (nextSlot_ + i) % kPoolSize;
        if (slots_[index].inFlight == 0 && pending_.size() < outputs_.size()) {
            nextSlot_ = index + 1;
            return InputSurface{slots_[index].texture.get(), 0, index};
        }
    }
    return std::nullopt;
}

bool NvencEncoder::encode(const InputSurface& in, int64_t pts100ns, uint32_t flags) {
    if (!encoder_ || in.index >= kPoolSize) return false;
    uint32_t index = in.index;
    if ((flags & kFrameDuplicate) && slots_[index].inFlight > 0) {
        // A registered resource can't be mapped twice: repeat the frame from a copy.
        const auto copy = acquireSurface();
        if (!copy) return true; // everything in flight: skip this repeat
        context_->CopyResource(slots_[copy->index].texture.get(), slots_[index].texture.get());
        index = copy->index;
    }
    if (pending_.size() >= outputs_.size()) return false;

    NV_ENC_MAP_INPUT_RESOURCE map{};
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = slots_[index].registered;
    if (api_.nvEncMapInputResource(encoder_, &map) != NV_ENC_SUCCESS) return false;

    const uint32_t out = nextOutput_;
    NV_ENC_PIC_PARAMS pic{};
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.inputBuffer = map.mappedResource;
    pic.bufferFmt = map.mappedBufferFmt;
    pic.inputWidth = config_.width;
    pic.inputHeight = config_.height;
    pic.inputPitch = config_.width;
    pic.outputBitstream = outputs_[out].bitstream;
    pic.completionEvent = outputs_[out].event;
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic.inputTimeStamp = static_cast<uint64_t>(pts100ns);
    if (flags & kFrameForceKeyframe) pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;

    const NVENCSTATUS s = api_.nvEncEncodePicture(encoder_, &pic);
    if (s != NV_ENC_SUCCESS && s != NV_ENC_ERR_NEED_MORE_INPUT) {
        api_.nvEncUnmapInputResource(encoder_, map.mappedResource);
        log::error("nvenc: encode failed ({})", static_cast<int>(s));
        return false;
    }
    ++slots_[index].inFlight;
    pending_.push_back({index, map.mappedResource, out});
    nextOutput_ = (nextOutput_ + 1) % outputs_.size();
    return true;
}

bool NvencEncoder::deliver(PacketSink& sink, DWORD waitMs) {
    if (pending_.empty()) return false;
    const Pending p = pending_.front();
    if (WaitForSingleObject(outputs_[p.output].event, waitMs) != WAIT_OBJECT_0) return false;
    NV_ENC_LOCK_BITSTREAM lock{};
    lock.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock.outputBitstream = outputs_[p.output].bitstream;
    if (api_.nvEncLockBitstream(encoder_, &lock) == NV_ENC_SUCCESS) {
        const bool key = lock.pictureType == NV_ENC_PIC_TYPE_IDR || lock.pictureType == NV_ENC_PIC_TYPE_I;
        sink.onPacket({{static_cast<const uint8_t*>(lock.bitstreamBufferPtr), lock.bitstreamSizeInBytes},
                       static_cast<int64_t>(lock.outputTimeStamp), key});
        api_.nvEncUnlockBitstream(encoder_, outputs_[p.output].bitstream);
    }
    api_.nvEncUnmapInputResource(encoder_, p.mapped);
    --slots_[p.slot].inFlight;
    pending_.pop_front();
    return true;
}

void NvencEncoder::drain(PacketSink& sink) {
    while (deliver(sink, 0)) {
    }
}

void NvencEncoder::flush(PacketSink& sink) {
    if (!encoder_) return;
    NV_ENC_PIC_PARAMS eos{};
    eos.version = NV_ENC_PIC_PARAMS_VER;
    eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
    HANDLE eosEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    NV_ENC_EVENT_PARAMS ev{};
    ev.version = NV_ENC_EVENT_PARAMS_VER;
    ev.completionEvent = eosEvent;
    api_.nvEncRegisterAsyncEvent(encoder_, &ev);
    eos.completionEvent = eosEvent;
    api_.nvEncEncodePicture(encoder_, &eos);
    while (deliver(sink, 2000)) {
    }
    WaitForSingleObject(eosEvent, 2000);
    api_.nvEncUnregisterAsyncEvent(encoder_, &ev);
    CloseHandle(eosEvent);
}

void NvencEncoder::shutdown() {
    if (encoder_) {
        for (const auto& p : pending_) {
            WaitForSingleObject(outputs_[p.output].event, 2000);
            api_.nvEncUnmapInputResource(encoder_, p.mapped);
        }
        pending_.clear();
        for (auto& slot : slots_)
            if (slot.registered) api_.nvEncUnregisterResource(encoder_, slot.registered);
        for (auto& out : outputs_) {
            if (out.event) {
                NV_ENC_EVENT_PARAMS ev{};
                ev.version = NV_ENC_EVENT_PARAMS_VER;
                ev.completionEvent = out.event;
                api_.nvEncUnregisterAsyncEvent(encoder_, &ev);
            }
            if (out.bitstream) api_.nvEncDestroyBitstreamBuffer(encoder_, out.bitstream);
        }
        api_.nvEncDestroyEncoder(encoder_);
        encoder_ = nullptr;
    }
    for (auto& slot : slots_) slot = {};
    for (auto& out : outputs_) {
        if (out.event) CloseHandle(out.event);
        out = {};
    }
    nextSlot_ = nextOutput_ = 0;
}

} // namespace rec
