#pragma once

#include "capture/ICaptureSource.h"
#include "gpu/D3DDevice.h"

#include <array>

namespace rec {

// Renders synthetic frames on the GPU on demand: every acquireLatest() returns a new frame.
// Gives benchmarks and tests deterministic content that is as hard to encode as a game.
class FakeCapture final : public ICaptureSource {
public:
    FakeCapture(const GpuDevice& gpu, uint32_t width, uint32_t height);

    const char* name() const override { return "synthetic"; }
    bool start() override;
    void stop() override {}
    std::optional<CapturedFrame> acquireLatest() override;
    bool targetLost() const override { return false; }
    uint64_t framesArrived() const override { return frame_; }

private:
    struct Params {
        uint32_t frame, width, height, pad;
    };

    winrt::com_ptr<ID3D11Device5> device_;
    winrt::com_ptr<ID3D11DeviceContext4> context_;
    uint32_t width_;
    uint32_t height_;
    uint32_t frame_ = 0;
    winrt::com_ptr<ID3D11VertexShader> vs_;
    winrt::com_ptr<ID3D11PixelShader> ps_;
    winrt::com_ptr<ID3D11Buffer> params_;
    // Deep enough that a frame is never re-rendered while an encoder still reads it (zero-copy).
    std::array<winrt::com_ptr<ID3D11Texture2D>, 8> textures_;
    std::array<winrt::com_ptr<ID3D11RenderTargetView>, 8> rtvs_;
};

} // namespace rec
