#include "capture/WgcCapture.h"

#include "core/Clock.h"
#include "core/Log.h"

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>

namespace rec {

namespace wgc = winrt::Windows::Graphics::Capture;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;

namespace {

constexpr auto kPixelFormat = DirectXPixelFormat::B8G8R8A8UIntNormalized;

// Asked once per process. Allowed without a prompt for desktop apps; lets us hide the yellow border.
bool borderlessAllowed() {
    static const bool allowed = [] {
        try {
            const auto status = wgc::GraphicsCaptureAccess::RequestAccessAsync(wgc::GraphicsCaptureAccessKind::Borderless).get();
            return status == winrt::Windows::Security::Authorization::AppCapabilityAccess::AppCapabilityAccessStatus::Allowed;
        } catch (const winrt::hresult_error& e) {
            log::warn("wgc: borderless access request failed (0x{:08X})", static_cast<uint32_t>(e.code()));
            return false;
        }
    }();
    return allowed;
}

} // namespace

WgcCapture::WgcCapture(const GpuDevice& gpu, CaptureTarget target, CaptureOptions options)
    : device_(gpu.winrtDevice), target_(target), options_(options) {}

WgcCapture::~WgcCapture() { stop(); }

bool WgcCapture::start() {
    try {
        if (!wgc::GraphicsCaptureSession::IsSupported()) {
            log::error("wgc: Windows.Graphics.Capture is not supported on this system");
            return false;
        }
        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        if (target_.window) {
            winrt::check_hresult(interop->CreateForWindow(target_.window, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                                          winrt::put_abi(item_)));
        } else {
            winrt::check_hresult(interop->CreateForMonitor(target_.monitor, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                                           winrt::put_abi(item_)));
        }
        itemClosed_ = item_.Closed(winrt::auto_revoke, [this](auto&&, auto&&) {
            targetLost_.store(true, std::memory_order_relaxed);
            log::info("wgc: capture target closed");
        });

        poolSize_ = item_.Size();
        pool_ = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(device_, kPixelFormat,
                                                                    static_cast<int32_t>(options_.poolBuffers), poolSize_);
        frameArrived_ = pool_.FrameArrived(winrt::auto_revoke, {this, &WgcCapture::onFrameArrived});

        session_ = pool_.CreateCaptureSession(item_);
        session_.IsCursorCaptureEnabled(options_.cursor);
        if (borderlessAllowed()) session_.IsBorderRequired(false);
        // Throttle at the source a little above the target rate, so it never beats against the pump.
        const int64_t interval = clock::kTicksPerSecond * 9 / (10 * static_cast<int64_t>(options_.fps));
        session_.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan{interval});
        if (target_.window) session_.IncludeSecondaryWindows(true);

        session_.StartCapture();
        log::info("wgc: capturing {} at {}x{}", target_.window ? "window" : "monitor", poolSize_.Width, poolSize_.Height);
        return true;
    } catch (const winrt::hresult_error& e) {
        log::error("wgc: start failed (0x{:08X}): {}", static_cast<uint32_t>(e.code()), winrt::to_string(e.message()));
        stop();
        return false;
    }
}

void WgcCapture::stop() {
    frameArrived_.revoke();
    itemClosed_.revoke();
    if (session_) {
        session_.Close();
        session_ = nullptr;
    }
    {
        std::scoped_lock lock(mutex_);
        latest_ = nullptr;
    }
    if (pool_) {
        pool_.Close();
        pool_ = nullptr;
    }
    item_ = nullptr;
}

void WgcCapture::onFrameArrived(const wgc::Direct3D11CaptureFramePool& pool, const winrt::Windows::Foundation::IInspectable&) {
    auto frame = pool.TryGetNextFrame();
    if (!frame) return;
    framesArrived_.fetch_add(1, std::memory_order_relaxed);

    const auto size = frame.ContentSize();
    if (size.Width != poolSize_.Width || size.Height != poolSize_.Height) {
        // Resolution changed: resize the pool. This frame's surface still has the old size, so skip it.
        poolSize_ = size;
        pool.Recreate(device_, kPixelFormat, static_cast<int32_t>(options_.poolBuffers), size);
        log::info("wgc: content resized to {}x{}", size.Width, size.Height);
        return;
    }

    std::scoped_lock lock(mutex_);
    latest_ = std::move(frame); // releases any unconsumed older frame back to the pool
    ++latestSequence_;
}

std::optional<CapturedFrame> WgcCapture::acquireLatest() {
    wgc::Direct3D11CaptureFrame frame{nullptr};
    uint64_t sequence = 0;
    {
        std::scoped_lock lock(mutex_);
        if (!latest_ || latestSequence_ == returnedSequence_) return std::nullopt;
        frame = std::exchange(latest_, nullptr);
        sequence = returnedSequence_ = latestSequence_;
    }

    CapturedFrame out;
    auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    if (FAILED(access->GetInterface(IID_PPV_ARGS(out.texture.put())))) return std::nullopt;
    const auto size = frame.ContentSize();
    out.width = static_cast<uint32_t>(size.Width);
    out.height = static_cast<uint32_t>(size.Height);
    out.time100ns = frame.SystemRelativeTime().count();
    out.sequence = sequence;
    out.owner = frame.as<::IUnknown>();
    return out;
}

} // namespace rec
