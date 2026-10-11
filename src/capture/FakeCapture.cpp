#include "capture/FakeCapture.h"

#include "core/Clock.h"
#include "core/Log.h"

#include "FakeCapturePS.h"
#include "FakeCaptureVS.h"

namespace rec {

FakeCapture::FakeCapture(const GpuDevice& gpu, uint32_t width, uint32_t height)
    : device_(gpu.device), context_(gpu.context), width_(width), height_(height) {}

bool FakeCapture::start() {
    if (FAILED(device_->CreateVertexShader(g_fakeCaptureVS, sizeof(g_fakeCaptureVS), nullptr, vs_.put())) ||
        FAILED(device_->CreatePixelShader(g_fakeCapturePS, sizeof(g_fakeCapturePS), nullptr, ps_.put())))
        return false;

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(Params);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device_->CreateBuffer(&bd, nullptr, params_.put()))) return false;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width_;
    td.Height = height_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // what WGC delivers
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (size_t i = 0; i < textures_.size(); ++i) {
        if (FAILED(device_->CreateTexture2D(&td, nullptr, textures_[i].put())) ||
            FAILED(device_->CreateRenderTargetView(textures_[i].get(), nullptr, rtvs_[i].put())))
            return false;
    }
    log::info("synthetic source {}x{}", width_, height_);
    return true;
}

std::optional<CapturedFrame> FakeCapture::acquireLatest() {
    const size_t i = frame_ % textures_.size();
    const Params p{frame_, width_, height_, 0};
    context_->UpdateSubresource(params_.get(), 0, nullptr, &p, 0, 0);

    ID3D11RenderTargetView* rtv = rtvs_[i].get();
    const D3D11_VIEWPORT vp{0, 0, static_cast<float>(width_), static_cast<float>(height_), 0, 1};
    context_->OMSetRenderTargets(1, &rtv, nullptr);
    context_->RSSetViewports(1, &vp);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->IASetInputLayout(nullptr);
    context_->VSSetShader(vs_.get(), nullptr, 0);
    ID3D11Buffer* cb = params_.get();
    context_->PSSetConstantBuffers(0, 1, &cb);
    context_->PSSetShader(ps_.get(), nullptr, 0);
    context_->Draw(3, 0);
    context_->OMSetRenderTargets(0, nullptr, nullptr);

    CapturedFrame f;
    f.texture = textures_[i];
    f.width = width_;
    f.height = height_;
    f.time100ns = clock::now100ns();
    f.sequence = ++frame_;
    return f;
}

} // namespace rec
