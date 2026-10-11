#include "encode/AmfEncoder.h"

#include "core/Log.h"
#include "core/Utf.h"

#include <AMF/components/ColorSpace.h>
#include <AMF/components/VideoEncoderAV1.h>
#include <AMF/components/VideoEncoderHEVC.h>
#include <AMF/components/VideoEncoderVCE.h>
#include <AMF/core/Buffer.h>
#include <AMF/core/Debug.h>
#include <AMF/core/Factory.h>
#include <AMF/core/Trace.h>

#include <algorithm>

namespace rec {
namespace {

struct AmfRuntime {
    HMODULE module = nullptr;
    amf::AMFFactory* factory = nullptr;
    amf_uint64 version = 0;
};

const AmfRuntime& runtime() {
    static const AmfRuntime rt = [] {
        AmfRuntime r;
        r.module = LoadLibraryExW(AMF_DLL_NAME, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!r.module) return r;
        auto query = reinterpret_cast<AMFQueryVersion_Fn>(GetProcAddress(r.module, AMF_QUERY_VERSION_FUNCTION_NAME));
        auto init = reinterpret_cast<AMFInit_Fn>(GetProcAddress(r.module, AMF_INIT_FUNCTION_NAME));
        if (!query || !init || query(&r.version) != AMF_OK) return r;
        // Ask for the older of the runtime and our headers so an older driver still initialises.
        if (init(std::min<amf_uint64>(r.version, AMF_FULL_VERSION), &r.factory) != AMF_OK) {
            r.factory = nullptr;
            return r;
        }
        // AMF's own tracing and debug checks run on our threads for every frame; keep only errors.
        amf::AMFTrace* trace = nullptr;
        if (r.factory->GetTrace(&trace) == AMF_OK && trace) {
            trace->EnableWriter(AMF_TRACE_WRITER_DEBUG_OUTPUT, false);
            trace->EnableWriter(AMF_TRACE_WRITER_CONSOLE, false);
            trace->SetGlobalLevel(AMF_TRACE_ERROR);
        }
        amf::AMFDebug* debug = nullptr;
        if (r.factory->GetDebug(&debug) == AMF_OK && debug) {
            debug->AssertsEnable(false);
            debug->EnablePerformanceMonitor(false);
        }
        log::info("amf: runtime {}.{}.{}", AMF_GET_MAJOR_VERSION(r.version), AMF_GET_MINOR_VERSION(r.version),
                  AMF_GET_SUBMINOR_VERSION(r.version));
        return r;
    }();
    return rt;
}

const char* resultName(AMF_RESULT r) {
    switch (r) {
    case AMF_OK: return "OK";
    case AMF_FAIL: return "FAIL";
    case AMF_INVALID_ARG: return "INVALID_ARG";
    case AMF_NOT_SUPPORTED: return "NOT_SUPPORTED";
    case AMF_NOT_FOUND: return "NOT_FOUND";
    case AMF_INPUT_FULL: return "INPUT_FULL";
    case AMF_REPEAT: return "REPEAT";
    case AMF_EOF: return "EOF";
    default: return "error";
    }
}

} // namespace

bool AmfEncoder::runtimeAvailable() { return runtime().factory != nullptr; }

AmfEncoder::~AmfEncoder() { shutdown(); }

template <class T>
bool AmfEncoder::set(const wchar_t* name, const T& value, bool required) {
    const AMF_RESULT r = encoder_->SetProperty(name, value);
    if (r != AMF_OK) {
        if (required) log::error("amf: setting {} failed ({})", narrow(name), resultName(r));
        else log::info("amf: optional {} not applied ({})", narrow(name), resultName(r));
        return !required;
    }
    return true;
}

bool AmfEncoder::init(const GpuDevice& gpu, const EncoderConfig& config) {
    shutdown();
    const auto& rt = runtime();
    if (!rt.factory) {
        log::error("amf: runtime not available");
        return false;
    }
    if (config.tenBit && config.codec == Codec::H264) {
        log::error("amf: H.264 has no 10-bit profile on AMF");
        return false;
    }
    config_ = config;
    format_ = config.rgbInput ? DXGI_FORMAT_B8G8R8A8_UNORM : config.tenBit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    frameDuration100ns_ = 10'000'000LL * config.fpsDen / config.fpsNum;

    AMF_RESULT r = rt.factory->CreateContext(&context_);
    if (r == AMF_OK) r = context_->InitDX11(gpu.device.get());
    if (r != AMF_OK) {
        log::error("amf: context init failed ({})", resultName(r));
        shutdown();
        return false;
    }
    const wchar_t* id = config.codec == Codec::H264   ? AMFVideoEncoderVCE_AVC
                        : config.codec == Codec::HEVC ? AMFVideoEncoder_HEVC
                                                      : AMFVideoEncoder_AV1;
    r = rt.factory->CreateComponent(context_, id, &encoder_);
    if (r != AMF_OK) {
        log::error("amf: this GPU has no {} encoder ({})", narrow(id), resultName(r));
        shutdown();
        return false;
    }
    if (amf::AMFCapsPtr caps; encoder_->GetCaps(&caps) == AMF_OK) {
        amf_int64 conversion = -2;
        caps->GetProperty(config.codec == Codec::H264   ? AMF_VIDEO_ENCODER_CAP_COLOR_CONVERSION
                          : config.codec == Codec::HEVC ? AMF_VIDEO_ENCODER_HEVC_CAP_COLOR_CONVERSION
                                                        : AMF_VIDEO_ENCODER_AV1_CAP_COLOR_CONVERSION,
                          &conversion);
        amf_int64 alignW = 1, alignH = 1;
        if (config.codec == Codec::AV1) {
            caps->GetProperty(AMF_VIDEO_ENCODER_AV1_CAP_WIDTH_ALIGNMENT_FACTOR, &alignW);
            caps->GetProperty(AMF_VIDEO_ENCODER_AV1_CAP_HEIGHT_ALIGNMENT_FACTOR, &alignH);
        }
        hardwareRgb_ = conversion == amf::AMF_ACCEL_HARDWARE;
        log::info("amf: RGB conversion in {}, alignment {}x{}",
                  hardwareRgb_ ? "fixed-function hardware" : conversion == amf::AMF_ACCEL_GPU ? "GPU shaders" : "software",
                  alignW, alignH);
    }
    if (!configure()) {
        shutdown();
        return false;
    }
    const amf::AMF_SURFACE_FORMAT surfaceFormat = config.rgbInput ? amf::AMF_SURFACE_BGRA
                                                  : config.tenBit ? amf::AMF_SURFACE_P010
                                                                  : amf::AMF_SURFACE_NV12;
    r = encoder_->Init(surfaceFormat, static_cast<amf_int32>(config.width), static_cast<amf_int32>(config.height));
    if (r != AMF_OK) {
        log::error("amf: encoder Init {}x{} failed ({})", config.width, config.height, resultName(r));
        shutdown();
        return false;
    }
    if (config.codec == Codec::AV1) {
        amf_int64 mode = -1;
        encoder_->GetProperty(AMF_VIDEO_ENCODER_AV1_ALIGNMENT_MODE, &mode);
        log::info("amf: AV1 alignment mode after Init: {}", mode);
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
            log::error("amf: input texture allocation failed");
            shutdown();
            return false;
        }
    }
    log::info("amf: {} {}x{} @ {}/{} {}-bit, {} kbps", config.codec == Codec::H264 ? "H.264" : config.codec == Codec::HEVC ? "HEVC" : "AV1",
              config.width, config.height, config.fpsNum, config.fpsDen, config.tenBit ? 10 : 8, config.bitrateKbps);
    return true;
}

bool AmfEncoder::configure() {
    const auto& c = config_;
    const AMFSize size = AMFConstructSize(static_cast<amf_int32>(c.width), static_cast<amf_int32>(c.height));
    const AMFRate rate = AMFConstructRate(c.fpsNum, c.fpsDen);
    const amf_int64 bitrate = amf_int64{c.bitrateKbps} * 1000;
    const amf_int64 peak = amf_int64{std::max(c.maxBitrateKbps, c.bitrateKbps)} * 1000;
    const bool maxQuality = c.preset == QualityPreset::MaxQuality;
    bool ok = true;

    switch (c.codec) {
    case Codec::H264: {
        ok &= set(AMF_VIDEO_ENCODER_USAGE, amf_int64{AMF_VIDEO_ENCODER_USAGE_TRANSCODING});
        ok &= set(AMF_VIDEO_ENCODER_PROFILE, amf_int64{AMF_VIDEO_ENCODER_PROFILE_HIGH});
        const amf_int64 preset = c.preset == QualityPreset::Speed     ? AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED
                                 : c.preset == QualityPreset::Balanced ? AMF_VIDEO_ENCODER_QUALITY_PRESET_BALANCED
                                 : c.preset == QualityPreset::Quality  ? AMF_VIDEO_ENCODER_QUALITY_PRESET_QUALITY
                                                                       : AMF_VIDEO_ENCODER_QUALITY_PRESET_HIGH_QUALITY;
        ok &= set(AMF_VIDEO_ENCODER_QUALITY_PRESET, preset);
        ok &= set(AMF_VIDEO_ENCODER_FRAMESIZE, size);
        ok &= set(AMF_VIDEO_ENCODER_FRAMERATE, rate);
        ok &= set(AMF_VIDEO_ENCODER_B_PIC_PATTERN, amf_int64{0});
        ok &= set(AMF_VIDEO_ENCODER_IDR_PERIOD, amf_int64{c.gopFrames});
        ok &= set(AMF_VIDEO_ENCODER_HEADER_INSERTION_SPACING, amf_int64{c.gopFrames});
        ok &= set(AMF_VIDEO_ENCODER_INSERT_AUD, false);
        set(AMF_VIDEO_ENCODER_PRE_ANALYSIS_ENABLE, maxQuality, false);
        const amf_int64 rc = c.rateControl == RateControl::CBR   ? AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR
                             : c.rateControl == RateControl::CQP ? AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CONSTANT_QP
                                                                 : AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR;
        ok &= set(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD, rc);
        if (c.rateControl == RateControl::CQP) {
            ok &= set(AMF_VIDEO_ENCODER_QP_I, amf_int64{c.cqLevel});
            ok &= set(AMF_VIDEO_ENCODER_QP_P, amf_int64{c.cqLevel});
        } else {
            ok &= set(AMF_VIDEO_ENCODER_TARGET_BITRATE, bitrate);
            ok &= set(AMF_VIDEO_ENCODER_PEAK_BITRATE, c.rateControl == RateControl::CBR ? bitrate : peak);
            set(AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE, peak, false);
        }
        if (c.rgbInput) set(AMF_VIDEO_ENCODER_INPUT_FULL_RANGE_COLOR, true, false);
        else set(AMF_VIDEO_ENCODER_INPUT_COLOR_PROFILE, amf_int64{AMF_VIDEO_CONVERTER_COLOR_PROFILE_709}, false);
        set(AMF_VIDEO_ENCODER_OUTPUT_COLOR_PROFILE, amf_int64{AMF_VIDEO_CONVERTER_COLOR_PROFILE_709}, false);
        set(AMF_VIDEO_ENCODER_OUTPUT_TRANSFER_CHARACTERISTIC, amf_int64{AMF_COLOR_TRANSFER_CHARACTERISTIC_BT709}, false);
        set(AMF_VIDEO_ENCODER_OUTPUT_COLOR_PRIMARIES, amf_int64{AMF_COLOR_PRIMARIES_BT709}, false);
        set(AMF_VIDEO_ENCODER_OUTPUT_FULL_RANGE_COLOR, false, false);
        break;
    }
    case Codec::HEVC: {
        ok &= set(AMF_VIDEO_ENCODER_HEVC_USAGE, amf_int64{AMF_VIDEO_ENCODER_HEVC_USAGE_TRANSCODING});
        ok &= set(AMF_VIDEO_ENCODER_HEVC_PROFILE,
                  amf_int64{c.tenBit ? AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN_10 : AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN});
        ok &= set(AMF_VIDEO_ENCODER_HEVC_COLOR_BIT_DEPTH, amf_int64{c.tenBit ? AMF_COLOR_BIT_DEPTH_10 : AMF_COLOR_BIT_DEPTH_8});
        const amf_int64 preset = c.preset == QualityPreset::Speed     ? AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED
                                 : c.preset == QualityPreset::Balanced ? AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_BALANCED
                                 : c.preset == QualityPreset::Quality  ? AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_QUALITY
                                                                       : AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_HIGH_QUALITY;
        ok &= set(AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET, preset);
        ok &= set(AMF_VIDEO_ENCODER_HEVC_FRAMESIZE, size);
        ok &= set(AMF_VIDEO_ENCODER_HEVC_FRAMERATE, rate);
        ok &= set(AMF_VIDEO_ENCODER_HEVC_GOP_SIZE, amf_int64{c.gopFrames});
        ok &= set(AMF_VIDEO_ENCODER_HEVC_NUM_GOPS_PER_IDR, amf_int64{1});
        ok &= set(AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE, amf_int64{AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE_IDR_ALIGNED});
        ok &= set(AMF_VIDEO_ENCODER_HEVC_INSERT_AUD, false);
        set(AMF_VIDEO_ENCODER_HEVC_PRE_ANALYSIS_ENABLE, maxQuality, false);
        const amf_int64 rc = c.rateControl == RateControl::CBR   ? AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR
                             : c.rateControl == RateControl::CQP ? AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CONSTANT_QP
                                                                 : AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR;
        ok &= set(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD, rc);
        if (c.rateControl == RateControl::CQP) {
            ok &= set(AMF_VIDEO_ENCODER_HEVC_QP_I, amf_int64{c.cqLevel});
            ok &= set(AMF_VIDEO_ENCODER_HEVC_QP_P, amf_int64{c.cqLevel});
        } else {
            ok &= set(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, bitrate);
            ok &= set(AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, c.rateControl == RateControl::CBR ? bitrate : peak);
            set(AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE, peak, false);
        }
        if (c.rgbInput) set(AMF_VIDEO_ENCODER_HEVC_INPUT_FULL_RANGE_COLOR, true, false);
        else set(AMF_VIDEO_ENCODER_HEVC_INPUT_COLOR_PROFILE, amf_int64{AMF_VIDEO_CONVERTER_COLOR_PROFILE_709}, false);
        set(AMF_VIDEO_ENCODER_HEVC_OUTPUT_COLOR_PROFILE, amf_int64{AMF_VIDEO_CONVERTER_COLOR_PROFILE_709}, false);
        set(AMF_VIDEO_ENCODER_HEVC_OUTPUT_TRANSFER_CHARACTERISTIC, amf_int64{AMF_COLOR_TRANSFER_CHARACTERISTIC_BT709}, false);
        set(AMF_VIDEO_ENCODER_HEVC_OUTPUT_COLOR_PRIMARIES, amf_int64{AMF_COLOR_PRIMARIES_BT709}, false);
        set(AMF_VIDEO_ENCODER_HEVC_OUTPUT_FULL_RANGE_COLOR, false, false);
        break;
    }
    case Codec::AV1: {
        ok &= set(AMF_VIDEO_ENCODER_AV1_USAGE, amf_int64{AMF_VIDEO_ENCODER_AV1_USAGE_TRANSCODING});
        ok &= set(AMF_VIDEO_ENCODER_AV1_PROFILE, amf_int64{AMF_VIDEO_ENCODER_AV1_PROFILE_MAIN});
        ok &= set(AMF_VIDEO_ENCODER_AV1_COLOR_BIT_DEPTH, amf_int64{c.tenBit ? AMF_COLOR_BIT_DEPTH_10 : AMF_COLOR_BIT_DEPTH_8});
        const amf_int64 preset = c.preset == QualityPreset::Speed     ? AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_SPEED
                                 : c.preset == QualityPreset::Balanced ? AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_BALANCED
                                 : c.preset == QualityPreset::Quality  ? AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_QUALITY
                                                                       : AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_HIGH_QUALITY;
        ok &= set(AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET, preset);
        // Without this, 1080p is coded as 1088 (64x16 alignment) on RDNA3-class encoders.
        set(AMF_VIDEO_ENCODER_AV1_ALIGNMENT_MODE, amf_int64{AMF_VIDEO_ENCODER_AV1_ALIGNMENT_MODE_NO_RESTRICTIONS}, false);
        ok &= set(AMF_VIDEO_ENCODER_AV1_FRAMESIZE, size);
        ok &= set(AMF_VIDEO_ENCODER_AV1_FRAMERATE, rate);
        // Finish each frame within one frame time at the lowest clocks that achieve it.
        set(AMF_VIDEO_ENCODER_AV1_ENCODING_LATENCY_MODE,
            amf_int64{AMF_VIDEO_ENCODER_AV1_ENCODING_LATENCY_MODE_POWER_SAVING_REAL_TIME}, false);
        set(AMF_VIDEO_ENCODER_AV1_B_PIC_PATTERN, amf_int64{0}, false);
        ok &= set(AMF_VIDEO_ENCODER_AV1_GOP_SIZE, amf_int64{c.gopFrames});
        ok &= set(AMF_VIDEO_ENCODER_AV1_HEADER_INSERTION_MODE,
                  amf_int64{AMF_VIDEO_ENCODER_AV1_HEADER_INSERTION_MODE_KEY_FRAME_ALIGNED});
        set(AMF_VIDEO_ENCODER_AV1_PRE_ANALYSIS_ENABLE, maxQuality, false);
        const amf_int64 rc = c.rateControl == RateControl::CBR   ? AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_CBR
                             : c.rateControl == RateControl::CQP ? AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_CONSTANT_QP
                                                                 : AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR;
        ok &= set(AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD, rc);
        if (c.rateControl == RateControl::CQP) {
            const amf_int64 qindex = std::clamp<amf_int64>(amf_int64{c.cqLevel} * 255 / 51, 1, 255);
            ok &= set(AMF_VIDEO_ENCODER_AV1_Q_INDEX_INTRA, qindex);
            ok &= set(AMF_VIDEO_ENCODER_AV1_Q_INDEX_INTER, qindex);
        } else {
            ok &= set(AMF_VIDEO_ENCODER_AV1_TARGET_BITRATE, bitrate);
            ok &= set(AMF_VIDEO_ENCODER_AV1_PEAK_BITRATE, c.rateControl == RateControl::CBR ? bitrate : peak);
            set(AMF_VIDEO_ENCODER_AV1_VBV_BUFFER_SIZE, peak, false);
        }
        if (c.rgbInput) set(AMF_VIDEO_ENCODER_AV1_INPUT_FULL_RANGE_COLOR, true, false);
        else set(AMF_VIDEO_ENCODER_AV1_INPUT_COLOR_PROFILE, amf_int64{AMF_VIDEO_CONVERTER_COLOR_PROFILE_709}, false);
        set(AMF_VIDEO_ENCODER_AV1_OUTPUT_COLOR_PROFILE, amf_int64{AMF_VIDEO_CONVERTER_COLOR_PROFILE_709}, false);
        set(AMF_VIDEO_ENCODER_AV1_OUTPUT_TRANSFER_CHARACTERISTIC, amf_int64{AMF_COLOR_TRANSFER_CHARACTERISTIC_BT709}, false);
        set(AMF_VIDEO_ENCODER_AV1_OUTPUT_COLOR_PRIMARIES, amf_int64{AMF_COLOR_PRIMARIES_BT709}, false);
        set(AMF_VIDEO_ENCODER_AV1_OUTPUT_FULL_RANGE_COLOR, false, false);
        break;
    }
    }
    return ok;
}

std::optional<InputSurface> AmfEncoder::acquireSurface() {
    for (uint32_t i = 0; i < kPoolSize; ++i) {
        const uint32_t index = (nextSlot_ + i) % kPoolSize;
        if (slots_[index].refs.load(std::memory_order_acquire) == 0) {
            nextSlot_ = index + 1;
            return InputSurface{slots_[index].texture.get(), 0, index};
        }
    }
    return std::nullopt;
}

void AMF_STD_CALL AmfEncoder::OnSurfaceDataRelease(amf::AMFSurface* surface) {
    auto* texture = static_cast<ID3D11Texture2D*>(surface->GetPlaneAt(0)->GetNative());
    for (auto& slot : slots_) {
        if (slot.texture.get() == texture) {
            slot.refs.fetch_sub(1, std::memory_order_release);
            return;
        }
    }
    winrt::com_ptr<::IUnknown> owner; // released after the lock
    std::scoped_lock lock(externalMutex_);
    const auto it = std::ranges::find(external_, texture, &External::texture);
    if (it != external_.end()) {
        owner = std::move(it->owner);
        external_.erase(it);
    }
}

bool AmfEncoder::encode(const InputSurface& in, int64_t pts100ns, uint32_t flags) {
    if (!encoder_ || in.index >= kPoolSize) return false;
    Slot& slot = slots_[in.index];
    amf::AMFSurfacePtr surface;
    slot.refs.fetch_add(1, std::memory_order_acq_rel); // released in OnSurfaceDataRelease
    if (context_->CreateSurfaceFromDX11Native(slot.texture.get(), &surface, this) != AMF_OK) {
        slot.refs.fetch_sub(1, std::memory_order_release);
        return false;
    }
    return submit(surface, pts100ns, flags);
}

bool AmfEncoder::encodeExternal(ID3D11Texture2D* texture, const winrt::com_ptr<::IUnknown>& owner, int64_t pts100ns,
                                uint32_t flags) {
    if (!encoder_ || !config_.rgbInput) return false;
    {
        std::scoped_lock lock(externalMutex_);
        external_.push_back({texture, owner});
    }
    amf::AMFSurfacePtr surface;
    if (context_->CreateSurfaceFromDX11Native(texture, &surface, this) != AMF_OK) {
        std::scoped_lock lock(externalMutex_);
        external_.pop_back();
        return false;
    }
    return submit(surface, pts100ns, flags);
}

bool AmfEncoder::submit(amf::AMFSurface* surface, int64_t pts100ns, uint32_t flags) {
    surface->SetPts(pts100ns);
    surface->SetDuration(frameDuration100ns_);

    if (flags & kFrameForceKeyframe) {
        switch (config_.codec) {
        case Codec::H264:
            surface->SetProperty(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, amf_int64{AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR});
            surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_SPS, true);
            surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_PPS, true);
            break;
        case Codec::HEVC:
            surface->SetProperty(AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE, amf_int64{AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_IDR});
            surface->SetProperty(AMF_VIDEO_ENCODER_HEVC_INSERT_HEADER, true);
            break;
        case Codec::AV1:
            surface->SetProperty(AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE, amf_int64{AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE_KEY});
            surface->SetProperty(AMF_VIDEO_ENCODER_AV1_FORCE_INSERT_SEQUENCE_HEADER, true);
            break;
        }
    } else if (flags & kFrameDuplicate) {
        // A skip picture costs the encoder almost nothing and only a few bytes of bitstream.
        if (config_.codec == Codec::H264)
            surface->SetProperty(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, amf_int64{AMF_VIDEO_ENCODER_PICTURE_TYPE_SKIP});
        else if (config_.codec == Codec::HEVC)
            surface->SetProperty(AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE, amf_int64{AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_SKIP});
    }

    for (int attempt = 0;; ++attempt) {
        const AMF_RESULT r = encoder_->SubmitInput(surface);
        if (r == AMF_OK || r == AMF_NEED_MORE_INPUT) return true;
        if (r != AMF_INPUT_FULL || attempt >= 200) {
            log::error("amf: SubmitInput failed ({})", resultName(r));
            return false;
        }
        // Queue full: pull finished packets out to make room, keep them for the next drain().
        amf::AMFDataPtr data;
        if (encoder_->QueryOutput(&data) == AMF_OK && data) pending_.push_back(data);
        else Sleep(1);
    }
}

void AmfEncoder::deliver(amf::AMFData* data, PacketSink& sink) {
    amf::AMFBufferPtr buffer(data);
    if (!buffer) return;
    amf_int64 type = -1;
    bool keyframe = false;
    switch (config_.codec) {
    case Codec::H264:
        buffer->GetProperty(AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, &type);
        keyframe = type == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR;
        break;
    case Codec::HEVC:
        buffer->GetProperty(AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE, &type);
        keyframe = type == AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_IDR;
        break;
    case Codec::AV1:
        buffer->GetProperty(AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE, &type);
        keyframe = type == AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE_KEY;
        break;
    }
    sink.onPacket({{static_cast<const uint8_t*>(buffer->GetNative()), buffer->GetSize()}, data->GetPts(), keyframe});
}

void AmfEncoder::drain(PacketSink& sink) {
    if (!encoder_) return;
    while (!pending_.empty()) {
        deliver(pending_.front(), sink);
        pending_.pop_front();
    }
    for (;;) {
        amf::AMFDataPtr data;
        const AMF_RESULT r = encoder_->QueryOutput(&data);
        if (r != AMF_OK || !data) break;
        deliver(data, sink);
    }
}

void AmfEncoder::flush(PacketSink& sink) {
    if (!encoder_) return;
    drain(sink);
    encoder_->Drain();
    for (int idle = 0; idle < 2000;) {
        amf::AMFDataPtr data;
        const AMF_RESULT r = encoder_->QueryOutput(&data);
        if (data) {
            deliver(data, sink);
            idle = 0;
        } else if (r == AMF_EOF) {
            break;
        } else {
            Sleep(1);
            ++idle;
        }
    }
}

void AmfEncoder::shutdown() {
    pending_.clear();
    if (encoder_) {
        encoder_->Terminate();
        encoder_ = nullptr;
    }
    if (context_) {
        context_->Terminate();
        context_ = nullptr;
    }
    for (auto& slot : slots_) {
        slot.texture = nullptr;
        slot.refs.store(0);
    }
    std::scoped_lock lock(externalMutex_);
    external_.clear();
}

} // namespace rec
