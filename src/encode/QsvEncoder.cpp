#include "encode/QsvEncoder.h"

#include "core/Log.h"

#include <vpl/mfx.h>
#include <vpl/mfxmemory.h>

#include <algorithm>

namespace rec {
namespace {

mfxLoader createLoader() {
    mfxLoader loader = MFXLoad();
    if (!loader) return nullptr;
    auto filter = [&](const char* property, mfxU32 value) {
        mfxConfig cfg = MFXCreateConfig(loader);
        mfxVariant v{};
        v.Version.Version = MFX_VARIANT_VERSION;
        v.Type = MFX_VARIANT_TYPE_U32;
        v.Data.U32 = value;
        MFXSetConfigFilterProperty(cfg, reinterpret_cast<const mfxU8*>(property), v);
    };
    filter("mfxImplDescription.Impl", MFX_IMPL_TYPE_HARDWARE);
    filter("mfxImplDescription.AccelerationMode", MFX_ACCEL_MODE_VIA_D3D11);
    return loader;
}

int64_t toMfxTime(int64_t t100ns) { return t100ns * 9 / 1000; } // 90 kHz
int64_t fromMfxTime(mfxU64 t) { return static_cast<int64_t>(t) * 1000 / 9; }

} // namespace

bool QsvEncoder::runtimeAvailable() {
    static const bool available = [] {
        mfxLoader loader = createLoader();
        if (!loader) return false;
        mfxHDL desc = nullptr;
        const bool found = MFXEnumImplementations(loader, 0, MFX_IMPLCAPS_IMPLDESCSTRUCTURE, &desc) == MFX_ERR_NONE;
        if (desc) MFXDispReleaseImplDescription(loader, desc);
        MFXUnload(loader);
        return found;
    }();
    return available;
}

QsvEncoder::~QsvEncoder() { shutdown(); }

bool QsvEncoder::init(const GpuDevice& gpu, const EncoderConfig& config) {
    shutdown();
    if (config.rgbInput) return false; // see encodeExternal()
    config_ = config;
    format_ = config.tenBit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    context_.copy_from(gpu.context.get());

    loader_ = createLoader();
    if (!loader_ || MFXCreateSession(static_cast<mfxLoader>(loader_), 0, &session_) != MFX_ERR_NONE) {
        log::error("qsv: no Intel hardware encoder available");
        shutdown();
        return false;
    }
    // Share our device: frames never leave the GPU, and the runtime orders its work after ours.
    if (MFXVideoCORE_SetHandle(session_, MFX_HANDLE_D3D11_DEVICE, gpu.device.get()) != MFX_ERR_NONE) {
        log::error("qsv: could not use our D3D11 device");
        shutdown();
        return false;
    }

    const bool maxQuality = config.preset == QualityPreset::MaxQuality;
    mfxVideoParam par{};
    auto& m = par.mfx;
    m.CodecId = config.codec == Codec::H264 ? MFX_CODEC_AVC : config.codec == Codec::HEVC ? MFX_CODEC_HEVC : MFX_CODEC_AV1;
    m.CodecProfile = static_cast<mfxU16>(config.codec == Codec::H264   ? MFX_PROFILE_AVC_HIGH
                                         : config.codec == Codec::HEVC ? (config.tenBit ? MFX_PROFILE_HEVC_MAIN10 : MFX_PROFILE_HEVC_MAIN)
                                                                       : MFX_PROFILE_AV1_MAIN);
    m.TargetUsage = static_cast<mfxU16>(config.preset == QualityPreset::Speed      ? MFX_TARGETUSAGE_BEST_SPEED
                                        : config.preset == QualityPreset::Balanced ? MFX_TARGETUSAGE_BALANCED
                                                                                   : MFX_TARGETUSAGE_BEST_QUALITY);
    // VDENC: the fixed-function encoder. The alternative runs on the GPU's EUs, which the game uses.
    m.LowPower = static_cast<mfxU16>(maxQuality ? MFX_CODINGOPTION_OFF : MFX_CODINGOPTION_ON);

    const uint32_t peak = std::max(config.maxBitrateKbps, config.bitrateKbps);
    const auto multiplier = static_cast<mfxU16>(std::max<uint32_t>(1, (peak + 65534) / 65535)); // kbps fields are 16-bit
    if (config.rateControl == RateControl::CQP) {
        m.RateControlMethod = MFX_RATECONTROL_CQP;
        const auto qp = static_cast<mfxU16>(config.codec == Codec::AV1 ? std::min(255u, config.cqLevel * 255 / 51) : config.cqLevel);
        m.QPI = m.QPP = m.QPB = qp;
    } else {
        m.RateControlMethod = static_cast<mfxU16>(config.rateControl == RateControl::CBR ? MFX_RATECONTROL_CBR : MFX_RATECONTROL_VBR);
        m.BRCParamMultiplier = multiplier;
        m.TargetKbps = static_cast<mfxU16>(config.bitrateKbps / multiplier);
        m.MaxKbps = static_cast<mfxU16>((config.rateControl == RateControl::CBR ? config.bitrateKbps : peak) / multiplier);
    }
    m.GopPicSize = static_cast<mfxU16>(config.gopFrames);
    m.GopRefDist = 1; // no B-frames
    m.GopOptFlag = MFX_GOP_CLOSED;
    m.IdrInterval = config.codec == Codec::H264 ? 0 : 1; // every I frame is an IDR / key frame

    auto& f = m.FrameInfo;
    f.FourCC = config.tenBit ? MFX_FOURCC_P010 : MFX_FOURCC_NV12;
    f.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    f.Width = static_cast<mfxU16>((config.width + 15) & ~15u);
    f.Height = static_cast<mfxU16>((config.height + 15) & ~15u);
    f.CropW = static_cast<mfxU16>(config.width);
    f.CropH = static_cast<mfxU16>(config.height);
    f.FrameRateExtN = config.fpsNum;
    f.FrameRateExtD = config.fpsDen;
    f.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    if (config.tenBit) {
        f.BitDepthLuma = f.BitDepthChroma = 10;
        f.Shift = 1;
    }
    par.IOPattern = MFX_IOPATTERN_IN_VIDEO_MEMORY;
    par.AsyncDepth = 4;

    mfxExtVideoSignalInfo signal{};
    signal.Header = {MFX_EXTBUFF_VIDEO_SIGNAL_INFO, sizeof(signal)};
    signal.VideoFormat = 5; // unspecified
    signal.VideoFullRange = 0;
    signal.ColourDescriptionPresent = 1;
    signal.ColourPrimaries = signal.TransferCharacteristics = signal.MatrixCoefficients = 1; // BT.709
    mfxExtCodingOption option{};
    option.Header = {MFX_EXTBUFF_CODING_OPTION, sizeof(option)};
    option.AUDelimiter = MFX_CODINGOPTION_OFF;
    mfxExtCodingOption2 option2{};
    option2.Header = {MFX_EXTBUFF_CODING_OPTION2, sizeof(option2)};
    option2.RepeatPPS = MFX_CODINGOPTION_ON;
    mfxExtAV1BitstreamParam av1{};
    av1.Header = {MFX_EXTBUFF_AV1_BITSTREAM_PARAM, sizeof(av1)};
    av1.WriteIVFHeaders = MFX_CODINGOPTION_OFF; // raw OBUs; the default would add an IVF container
    std::vector<mfxExtBuffer*> ext{&signal.Header};
    if (config.codec == Codec::AV1) ext.push_back(&av1.Header);
    else {
        ext.push_back(&option.Header);
        ext.push_back(&option2.Header);
    }
    par.ExtParam = ext.data();
    par.NumExtParam = static_cast<mfxU16>(ext.size());

    if (const mfxStatus s = MFXVideoENCODE_Init(session_, &par); s < MFX_ERR_NONE) {
        log::error("qsv: encoder initialisation failed ({})", static_cast<int>(s));
        shutdown();
        return false;
    }
    mfxVideoParam actual{};
    MFXVideoENCODE_GetVideoParam(session_, &actual);
    const size_t bufferBytes = std::max<size_t>(size_t{actual.mfx.BufferSizeInKB} * std::max<mfxU16>(1, actual.mfx.BRCParamMultiplier) * 1000,
                                                config.width * config.height * 3 / 2);
    for (auto& out : outputs_) {
        out.data.resize(bufferBytes);
        out.bitstream.Data = out.data.data();
        out.bitstream.MaxLength = static_cast<mfxU32>(out.data.size());
    }
    if (MFXGetMemoryInterface(session_, &memory_) != MFX_ERR_NONE || !memory_) {
        log::error("qsv: the runtime has no memory interface (oneVPL 2.10+ driver needed)");
        shutdown();
        return false;
    }

    D3D11_TEXTURE2D_DESC d{};
    d.Width = f.Width; // the encoder reads whole 16-aligned surfaces and crops
    d.Height = f.Height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = format_;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (auto& slot : slots_) {
        if (FAILED(gpu.device->CreateTexture2D(&d, nullptr, slot.texture.put()))) {
            shutdown();
            return false;
        }
    }
    log::info("qsv: {} {}x{} @ {}/{} {}-bit, {} kbps{}", config.codec == Codec::H264 ? "H.264" : config.codec == Codec::HEVC ? "HEVC" : "AV1",
              config.width, config.height, config.fpsNum, config.fpsDen, config.tenBit ? 10 : 8, config.bitrateKbps,
              maxQuality ? "" : ", low-power");
    return true;
}

bool QsvEncoder::slotFree(Slot& slot) {
    if (!slot.imported) return true;
    mfxU32 refs = 0;
    slot.imported->FrameInterface->GetRefCounter(slot.imported, &refs);
    if (refs > 1) return false; // the encoder still holds it
    slot.imported->FrameInterface->Release(slot.imported);
    slot.imported = nullptr;
    return true;
}

std::optional<uint32_t> QsvEncoder::freeOutput() const {
    for (uint32_t i = 0; i < outputs_.size(); ++i)
        if (!outputs_[i].busy) return i;
    return std::nullopt;
}

std::optional<InputSurface> QsvEncoder::acquireSurface() {
    if (!freeOutput()) return std::nullopt;
    for (uint32_t i = 0; i < kPoolSize; ++i) {
        const uint32_t index = (nextSlot_ + i) % kPoolSize;
        if (slotFree(slots_[index])) {
            nextSlot_ = index + 1;
            return InputSurface{slots_[index].texture.get(), 0, index};
        }
    }
    return std::nullopt;
}

bool QsvEncoder::encode(const InputSurface& in, int64_t pts100ns, uint32_t flags) {
    if (!session_ || in.index >= kPoolSize) return false;
    uint32_t index = in.index;
    if ((flags & kFrameDuplicate) && !slotFree(slots_[index])) {
        const auto copy = acquireSurface(); // repeat the frame from a copy
        if (!copy) return true;
        context_->CopyResource(slots_[copy->index].texture.get(), slots_[index].texture.get());
        index = copy->index;
    }
    Slot& slot = slots_[index];
    if (!slotFree(slot)) return false;

    mfxSurfaceD3D11Tex2D external{};
    external.SurfaceInterface.Header.SurfaceType = MFX_SURFACE_TYPE_D3D11_TEX2D;
    external.SurfaceInterface.Header.SurfaceFlags = MFX_SURFACE_FLAG_IMPORT_SHARED | MFX_SURFACE_FLAG_IMPORT_COPY;
    external.SurfaceInterface.Header.StructSize = sizeof(external);
    external.texture2D = slot.texture.get();
    mfxFrameSurface1* surface = nullptr;
    const mfxStatus s = memory_->ImportFrameSurface(memory_, MFX_SURFACE_COMPONENT_ENCODE,
                                                    &external.SurfaceInterface.Header, &surface);
    if (s != MFX_ERR_NONE || !surface) {
        if (!loggedImportFailure_) log::error("qsv: importing the frame failed ({})", static_cast<int>(s));
        loggedImportFailure_ = true;
        return false;
    }
    surface->Data.TimeStamp = static_cast<mfxU64>(toMfxTime(pts100ns));
    slot.imported = surface; // released once the encoder lets go of it (slotFree)
    return submit(surface, flags);
}

bool QsvEncoder::submit(mfxFrameSurface1* surface, uint32_t flags) {
    const auto out = freeOutput();
    if (!out) return false;
    Output& o = outputs_[*out];
    o.bitstream.DataLength = o.bitstream.DataOffset = 0;
    mfxEncodeCtrl ctrl{};
    ctrl.FrameType = MFX_FRAMETYPE_I | MFX_FRAMETYPE_IDR | MFX_FRAMETYPE_REF;
    mfxSyncPoint sync = nullptr;
    mfxStatus s = MFX_ERR_NONE;
    for (int attempt = 0; attempt < 100; ++attempt) {
        s = MFXVideoENCODE_EncodeFrameAsync(session_, (flags & kFrameForceKeyframe) && surface ? &ctrl : nullptr, surface,
                                            &o.bitstream, &sync);
        if (s != MFX_WRN_DEVICE_BUSY) break;
        Sleep(1);
    }
    if (s == MFX_ERR_MORE_DATA) return true; // buffered inside the encoder; output comes later
    if (s < MFX_ERR_NONE || !sync) {
        log::error("qsv: encode failed ({})", static_cast<int>(s));
        return false;
    }
    o.busy = true;
    pending_.push_back({sync, *out});
    return true;
}

bool QsvEncoder::deliver(PacketSink& sink, uint32_t waitMs) {
    if (pending_.empty()) return false;
    const Pending p = pending_.front();
    const mfxStatus s = MFXVideoCORE_SyncOperation(session_, p.sync, waitMs);
    if (s == MFX_WRN_IN_EXECUTION) return false;
    Output& o = outputs_[p.output];
    if (s == MFX_ERR_NONE && o.bitstream.DataLength) {
        const bool key = (o.bitstream.FrameType & MFX_FRAMETYPE_IDR) ||
                         (config_.codec == Codec::AV1 && (o.bitstream.FrameType & MFX_FRAMETYPE_I));
        sink.onPacket({{o.bitstream.Data + o.bitstream.DataOffset, o.bitstream.DataLength},
                       fromMfxTime(o.bitstream.TimeStamp), key});
    }
    o.bitstream.DataLength = o.bitstream.DataOffset = 0;
    o.busy = false;
    pending_.pop_front();
    return true;
}

void QsvEncoder::drain(PacketSink& sink) {
    while (deliver(sink, 0)) {
    }
}

void QsvEncoder::flush(PacketSink& sink) {
    if (!session_) return;
    // Pull out everything the encoder still holds.
    for (int i = 0; i < 64; ++i) {
        while (!freeOutput() && deliver(sink, 2000)) {
        }
        const size_t before = pending_.size();
        if (!submit(nullptr, 0) || pending_.size() == before) break; // MFX_ERR_MORE_DATA: empty
    }
    while (deliver(sink, 2000)) {
    }
}

void QsvEncoder::shutdown() {
    if (session_) {
        for (const auto& p : pending_) MFXVideoCORE_SyncOperation(session_, p.sync, 2000);
        pending_.clear();
        MFXVideoENCODE_Close(session_);
    }
    for (auto& slot : slots_) {
        if (slot.imported) slot.imported->FrameInterface->Release(slot.imported);
        slot = {};
    }
    for (auto& out : outputs_) out = {};
    if (session_) MFXClose(session_);
    session_ = nullptr;
    memory_ = nullptr;
    if (loader_) MFXUnload(static_cast<mfxLoader>(loader_));
    loader_ = nullptr;
    nextSlot_ = 0;
}

} // namespace rec
