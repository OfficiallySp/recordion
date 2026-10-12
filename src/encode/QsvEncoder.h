#pragma once

#include "encode/IVideoEncoder.h"

#include <vpl/mfx.h>

#include <array>
#include <deque>
#include <vector>

struct mfxMemoryInterface;

namespace rec {

// Intel Quick Sync through oneVPL 2.x. Runs in LowPower (VDENC) mode, the fixed-function encoder,
// unless Max quality is chosen. Our own D3D11 textures are imported as encoder surfaces
// (mfxMemoryInterface::ImportFrameSurface, shared where the runtime allows, copied otherwise).
class QsvEncoder final : public IVideoEncoder {
public:
    ~QsvEncoder() override;

    // A oneVPL hardware implementation (Intel GPU driver) is present.
    static bool runtimeAvailable();

    const char* name() const override { return "Intel Quick Sync"; }
    bool init(const GpuDevice& gpu, const EncoderConfig& config) override;
    std::optional<InputSurface> acquireSurface() override;
    bool encode(const InputSurface& surface, int64_t pts100ns, uint32_t flags) override;
    bool encodeExternal(ID3D11Texture2D*, const winrt::com_ptr<::IUnknown>&, int64_t, uint32_t) override { return false; }
    bool hardwareRgbConversion() const override { return false; }
    void drain(PacketSink& sink) override;
    void flush(PacketSink& sink) override;
    DXGI_FORMAT inputFormat() const override { return format_; }

private:
    static constexpr size_t kPoolSize = 8;

    struct Slot {
        winrt::com_ptr<ID3D11Texture2D> texture;
        mfxFrameSurface1* imported = nullptr; // our reference while the encoder may still read it
    };
    struct Output {
        std::vector<uint8_t> data;
        mfxBitstream bitstream{};
        bool busy = false;
    };
    struct Pending {
        mfxSyncPoint sync;
        uint32_t output;
    };

    bool slotFree(Slot& slot);
    std::optional<uint32_t> freeOutput() const;
    bool submit(mfxFrameSurface1* surface, uint32_t flags);
    bool deliver(PacketSink& sink, uint32_t waitMs);
    void shutdown();

    void* loader_ = nullptr;
    mfxSession session_ = nullptr;
    mfxMemoryInterface* memory_ = nullptr;
    winrt::com_ptr<ID3D11DeviceContext> context_;
    EncoderConfig config_;
    DXGI_FORMAT format_ = DXGI_FORMAT_NV12;
    std::array<Slot, kPoolSize> slots_;
    std::array<Output, kPoolSize> outputs_;
    std::deque<Pending> pending_;
    uint32_t nextSlot_ = 0;
    bool loggedImportFailure_ = false;
};

} // namespace rec
