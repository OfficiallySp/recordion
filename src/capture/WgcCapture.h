#pragma once

#include "capture/ICaptureSource.h"
#include "gpu/D3DDevice.h"

#include <winrt/Windows.Graphics.Capture.h>

#include <atomic>
#include <mutex>

namespace rec {

// Windows.Graphics.Capture source. The FrameArrived callback only swaps in the newest frame; all GPU
// work happens on the consumer thread, so frames that would be dropped are never touched.
class WgcCapture final : public ICaptureSource {
public:
    WgcCapture(const GpuDevice& gpu, CaptureTarget target, CaptureOptions options);
    ~WgcCapture() override;

    const char* name() const override { return "WGC"; }
    bool start() override;
    void stop() override;
    std::optional<CapturedFrame> acquireLatest() override;
    bool targetLost() const override { return targetLost_.load(std::memory_order_relaxed); }
    uint64_t framesArrived() const override { return framesArrived_.load(std::memory_order_relaxed); }

private:
    void onFrameArrived(const winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool& pool,
                        const winrt::Windows::Foundation::IInspectable&);

    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice device_;
    CaptureTarget target_;
    CaptureOptions options_;

    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item_{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool pool_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::FrameArrived_revoker frameArrived_;
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem::Closed_revoker itemClosed_;
    winrt::Windows::Graphics::SizeInt32 poolSize_{};

    std::mutex mutex_;
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame latest_{nullptr};
    uint64_t latestSequence_ = 0;
    uint64_t returnedSequence_ = 0;

    std::atomic<uint64_t> framesArrived_{0};
    std::atomic<bool> targetLost_{false};
};

} // namespace rec
