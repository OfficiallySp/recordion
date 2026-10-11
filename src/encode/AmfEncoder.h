#pragma once

#include "encode/IVideoEncoder.h"

#include <AMF/components/Component.h>
#include <AMF/core/Context.h>
#include <AMF/core/Surface.h>

#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <vector>

namespace rec {

// AMD AMF hardware encoder (VCN). Input textures live on our D3D11 device and are wrapped with
// CreateSurfaceFromDX11Native, so frames reach the encoder without a copy.
class AmfEncoder final : public IVideoEncoder, private amf::AMFSurfaceObserver {
public:
    ~AmfEncoder() override;

    // Loads amfrt64.dll from the driver; false if this machine has no AMF runtime.
    static bool runtimeAvailable();

    const char* name() const override { return "AMD AMF"; }
    bool init(const GpuDevice& gpu, const EncoderConfig& config) override;
    std::optional<InputSurface> acquireSurface() override;
    bool encode(const InputSurface& surface, int64_t pts100ns, uint32_t flags) override;
    bool encodeExternal(ID3D11Texture2D* texture, const winrt::com_ptr<::IUnknown>& owner, int64_t pts100ns,
                        uint32_t flags) override;
    bool hardwareRgbConversion() const override { return hardwareRgb_; }
    void drain(PacketSink& sink) override;
    void flush(PacketSink& sink) override;
    DXGI_FORMAT inputFormat() const override { return format_; }

private:
    static constexpr size_t kPoolSize = 8;

    void AMF_STD_CALL OnSurfaceDataRelease(amf::AMFSurface* surface) override;
    bool configure();
    template <class T>
    bool set(const wchar_t* name, const T& value, bool required = true);
    bool submit(amf::AMFSurface* surface, int64_t pts100ns, uint32_t flags);
    void deliver(amf::AMFData* data, PacketSink& sink);
    void shutdown();

    struct Slot {
        winrt::com_ptr<ID3D11Texture2D> texture;
        std::atomic<int> refs{0}; // AMF surfaces currently wrapping this texture
    };

    EncoderConfig config_;
    DXGI_FORMAT format_ = DXGI_FORMAT_NV12;
    amf::AMFContextPtr context_;
    amf::AMFComponentPtr encoder_;
    std::array<Slot, kPoolSize> slots_;
    uint32_t nextSlot_ = 0;
    std::deque<amf::AMFDataPtr> pending_; // packets pulled out while the input queue was full
    bool hardwareRgb_ = false;

    // Caller textures in flight (encodeExternal), released from AMF's thread.
    struct External {
        ID3D11Texture2D* texture;
        winrt::com_ptr<::IUnknown> owner;
    };
    std::mutex externalMutex_;
    std::vector<External> external_;
    int64_t frameDuration100ns_ = 0;
};

} // namespace rec
