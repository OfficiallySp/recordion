#pragma once

#include "encode/IVideoEncoder.h"

#include <nvEncodeAPI.h>

#include <array>
#include <deque>

namespace rec {

// NVIDIA NVENC (Video Codec SDK 13.1 API, driver R570+). Asynchronous: each frame has a completion
// event, so drain() never blocks. Input textures live on our D3D11 device and are registered with
// NVENC once. Lookahead and temporal AQ (which run on CUDA cores) stay off unless Max quality.
class NvencEncoder final : public IVideoEncoder {
public:
    ~NvencEncoder() override;

    // Loads nvEncodeAPI64.dll and checks the driver supports our API version.
    static bool runtimeAvailable();

    const char* name() const override { return "NVIDIA NVENC"; }
    bool init(const GpuDevice& gpu, const EncoderConfig& config) override;
    std::optional<InputSurface> acquireSurface() override;
    bool encode(const InputSurface& surface, int64_t pts100ns, uint32_t flags) override;
    // NVENC converts RGB with CUDA (shader cores), so the video processor path is cheaper for games.
    bool encodeExternal(ID3D11Texture2D*, const winrt::com_ptr<::IUnknown>&, int64_t, uint32_t) override { return false; }
    bool hardwareRgbConversion() const override { return false; }
    void drain(PacketSink& sink) override;
    void flush(PacketSink& sink) override;
    DXGI_FORMAT inputFormat() const override { return format_; }

private:
    static constexpr size_t kPoolSize = 8;

    struct Slot {
        winrt::com_ptr<ID3D11Texture2D> texture;
        NV_ENC_REGISTERED_PTR registered = nullptr;
        int inFlight = 0;
    };
    struct Output {
        NV_ENC_OUTPUT_PTR bitstream = nullptr;
        HANDLE event = nullptr;
    };
    struct Pending {
        uint32_t slot;
        NV_ENC_INPUT_PTR mapped;
        uint32_t output;
    };

    bool deliver(PacketSink& sink, DWORD waitMs);
    void shutdown();

    NV_ENCODE_API_FUNCTION_LIST api_{};
    void* encoder_ = nullptr;
    winrt::com_ptr<ID3D11DeviceContext> context_;
    EncoderConfig config_;
    DXGI_FORMAT format_ = DXGI_FORMAT_NV12;
    std::array<Slot, kPoolSize> slots_;
    std::array<Output, kPoolSize> outputs_;
    std::deque<Pending> pending_;
    uint32_t nextSlot_ = 0;
    uint32_t nextOutput_ = 0;
};

} // namespace rec
