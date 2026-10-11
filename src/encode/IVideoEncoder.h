#pragma once

#include "core/Settings.h"
#include "gpu/ColorConverter.h"
#include "gpu/D3DDevice.h"

#include <d3d11.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace rec {

struct EncoderConfig {
    Codec codec = Codec::HEVC;
    bool tenBit = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fpsNum = 60;
    uint32_t fpsDen = 1;
    RateControl rateControl = RateControl::VBR;
    uint32_t bitrateKbps = 50'000;
    uint32_t maxBitrateKbps = 75'000;
    uint32_t cqLevel = 23; // H.264/HEVC QP scale (0-51); mapped for AV1
    uint32_t gopFrames = 120;
    QualityPreset preset = QualityPreset::Balanced;
    // Input surfaces are BGRA (full-range RGB) and the encoder converts to YUV itself, e.g. in
    // VCN's fixed-function colour converter. Requires output size == capture size.
    bool rgbInput = false;
};

// An encoder-owned input texture. The pump renders into it, then hands it back via encode().
struct InputSurface {
    ID3D11Texture2D* texture = nullptr;
    UINT arraySlice = 0;
    uint32_t index = 0;
};

struct EncodedPacket {
    std::span<const uint8_t> data; // Annex-B (H.264/HEVC) or low-overhead OBUs (AV1)
    int64_t pts100ns = 0;
    bool keyframe = false;
};

class PacketSink {
public:
    virtual ~PacketSink() = default;
    virtual void onPacket(const EncodedPacket& packet) = 0;
};

enum FrameFlags : uint32_t {
    kFrameNone = 0,
    kFrameForceKeyframe = 1,
    kFrameDuplicate = 2, // same content as the previous frame (constant frame rate padding)
};

class IVideoEncoder {
public:
    virtual ~IVideoEncoder() = default;

    virtual const char* name() const = 0;
    virtual bool init(const GpuDevice& gpu, const EncoderConfig& config) = 0;
    // A free input surface, or nullopt if every surface is still in flight.
    virtual std::optional<InputSurface> acquireSurface() = 0;
    // Submits a surface. For kFrameDuplicate, pass the previous frame's surface again.
    virtual bool encode(const InputSurface& surface, int64_t pts100ns, uint32_t flags) = 0;
    // Zero-copy: encodes a caller's BGRA texture (config.rgbInput) directly. `owner` is held until
    // the encoder has finished reading the texture (e.g. it keeps a capture frame out of its pool).
    virtual bool encodeExternal(ID3D11Texture2D* texture, const winrt::com_ptr<::IUnknown>& owner, int64_t pts100ns,
                                uint32_t flags) = 0;
    // True if the encoder converts RGB to YUV in fixed-function hardware, making encodeExternal the
    // cheapest path (no shader pass, no intermediate copy).
    virtual bool hardwareRgbConversion() const = 0;
    // Non-blocking: delivers every packet that is ready.
    virtual void drain(PacketSink& sink) = 0;
    // End of stream: blocks until all submitted frames are delivered.
    virtual void flush(PacketSink& sink) = 0;

    virtual DXGI_FORMAT inputFormat() const = 0;
};

} // namespace rec
