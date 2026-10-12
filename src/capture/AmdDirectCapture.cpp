#include "capture/AmdDirectCapture.h"

#include "core/Clock.h"
#include "core/Log.h"
#include "encode/AmfEncoder.h"

#include <AMF/components/DisplayCapture.h>
#include <AMF/core/Surface.h>

namespace rec {
namespace {

// Keeps the AMF surface (and so its texture) alive while the encoder reads it.
class SurfaceOwner final : public ::IUnknown {
public:
    explicit SurfaceOwner(amf::AMFSurfacePtr surface) : surface_(std::move(surface)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid != __uuidof(::IUnknown)) {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        *out = this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }

private:
    std::atomic<ULONG> refs_{1};
    amf::AMFSurfacePtr surface_;
};

// DisplayCapture's monitor index counts the outputs of our adapter.
int outputIndex(IDXGIAdapter4* adapter, HMONITOR monitor) {
    winrt::com_ptr<IDXGIOutput> output;
    for (UINT i = 0; adapter->EnumOutputs(i, output.put()) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_OUTPUT_DESC desc{};
        if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) return static_cast<int>(i);
        output = nullptr;
    }
    return -1;
}

} // namespace

AmdDirectCapture::AmdDirectCapture(const GpuDevice& gpu, HMONITOR monitor)
    : device_(gpu.device), adapter_(gpu.adapter), monitor_(monitor) {}

AmdDirectCapture::~AmdDirectCapture() { stop(); }

bool AmdDirectCapture::start() {
    amf::AMFFactory* factory = AmfEncoder::factory();
    const int index = outputIndex(adapter_.get(), monitor_);
    if (!factory || index < 0) return false;
    // The capture context shares our D3D11 device, so frames can go to the encoder without a copy.
    if (factory->CreateContext(&context_) != AMF_OK || context_->InitDX11(device_.get()) != AMF_OK ||
        factory->CreateComponent(context_, AMFDisplayCapture, &capture_) != AMF_OK) {
        log::warn("amd capture: DisplayCapture unavailable");
        stop();
        return false;
    }
    capture_->SetProperty(AMF_DISPLAYCAPTURE_MONITOR_INDEX, amf_int64{index});
    // Non-blocking: hand back the current frame when asked; our frame timer does the pacing.
    capture_->SetProperty(AMF_DISPLAYCAPTURE_MODE, amf_int64{AMF_DISPLAYCAPTURE_MODE_GET_CURRENT_SURFACE});
    // A copy we can hold while the encoder reads it, rather than the surface being scanned out.
    capture_->SetProperty(AMF_DISPLAYCAPTURE_DUPLICATEOUTPUT, true);
    if (capture_->Init(amf::AMF_SURFACE_UNKNOWN, 0, 0) != AMF_OK) {
        log::warn("amd capture: Init failed (another app may be using it, e.g. Adrenalin's recorder)");
        stop();
        return false;
    }
    amf_int64 format = 0;
    capture_->GetProperty(AMF_DISPLAYCAPTURE_FORMAT, &format);
    if (format != amf::AMF_SURFACE_BGRA) { // HDR displays deliver 10/16-bit formats we don't convert yet
        log::warn("amd capture: display format {} not supported; falling back", format);
        stop();
        return false;
    }
    log::info("amd capture: capturing display {}", index);
    return true;
}

void AmdDirectCapture::stop() {
    if (capture_) {
        capture_->Terminate();
        capture_ = nullptr;
    }
    if (context_) {
        context_->Terminate();
        context_ = nullptr;
    }
}

std::optional<CapturedFrame> AmdDirectCapture::acquireLatest() {
    if (!capture_) return std::nullopt;
    amf::AMFDataPtr data;
    if (capture_->QueryOutput(&data) != AMF_OK || !data) return std::nullopt;
    amf::AMFSurfacePtr surface(data);
    if (!surface || surface->GetPlanesCount() < 1) return std::nullopt;
    amf::AMFPlane* plane = surface->GetPlaneAt(0);

    CapturedFrame f;
    f.texture.copy_from(static_cast<ID3D11Texture2D*>(plane->GetNative()));
    f.width = static_cast<uint32_t>(plane->GetWidth());
    f.height = static_cast<uint32_t>(plane->GetHeight());
    f.time100ns = clock::now100ns();
    f.sequence = frames_.fetch_add(1, std::memory_order_relaxed) + 1;
    f.owner.attach(new SurfaceOwner(surface));
    return f;
}

} // namespace rec
