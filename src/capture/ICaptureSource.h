#pragma once

#include <windows.h>
#include <d3d11.h>

#include <winrt/base.h>

#include <cstdint>
#include <optional>

namespace rec {

struct CaptureTarget {
    HMONITOR monitor = nullptr; // capture this display...
    HWND window = nullptr;      // ...or this window (takes precedence when set)
};

struct CaptureOptions {
    uint32_t fps = 60;       // capture is throttled at the source to about this rate
    bool cursor = true;
    uint32_t poolBuffers = 3; // deeper when frames are handed straight to the encoder
};

// A captured BGRA frame. Holding it keeps the source's surface out of the pool, so release it as
// soon as the GPU work that reads it has been submitted.
struct CapturedFrame {
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::com_ptr<::IUnknown> owner; // source-specific lifetime handle (e.g. the WGC frame)
    uint32_t width = 0;               // content size; may be smaller than the texture
    uint32_t height = 0;
    int64_t time100ns = 0;            // QPC timeline
    uint64_t sequence = 0;
};

class ICaptureSource {
public:
    virtual ~ICaptureSource() = default;

    virtual const char* name() const = 0;
    // Must be called from an MTA thread.
    virtual bool start() = 0;
    virtual void stop() = 0;
    // The newest frame not returned before, or nullopt if nothing new arrived.
    virtual std::optional<CapturedFrame> acquireLatest() = 0;
    // True once the target is gone (window closed, display removed).
    virtual bool targetLost() const = 0;
    virtual uint64_t framesArrived() const = 0;
};

} // namespace rec
