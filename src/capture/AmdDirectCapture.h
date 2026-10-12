#pragma once

#include "capture/ICaptureSource.h"
#include "gpu/D3DDevice.h"

#include <AMF/components/Component.h>
#include <AMF/core/Context.h>

#include <atomic>

namespace rec {

// AMD Direct Capture: the display driver hands over the frame being scanned out, through AMF's
// DisplayCapture component. Unlike Windows.Graphics.Capture, DWM does no work for it (measured:
// ~0.3% 3D load instead of ~1%), so a fullscreen game keeps its fastest present path. Monitors
// only, and the hardware cursor is not part of the image. AMD GPUs only; experimental.
class AmdDirectCapture final : public ICaptureSource {
public:
    AmdDirectCapture(const GpuDevice& gpu, HMONITOR monitor);
    ~AmdDirectCapture() override;

    const char* name() const override { return "AMD Direct Capture"; }
    bool start() override;
    void stop() override;
    std::optional<CapturedFrame> acquireLatest() override;
    bool targetLost() const override { return false; }
    uint64_t framesArrived() const override { return frames_.load(std::memory_order_relaxed); }

private:
    winrt::com_ptr<ID3D11Device5> device_;
    winrt::com_ptr<IDXGIAdapter4> adapter_;
    HMONITOR monitor_;
    amf::AMFContextPtr context_;
    amf::AMFComponentPtr capture_;
    std::atomic<uint64_t> frames_{0};
};

} // namespace rec
