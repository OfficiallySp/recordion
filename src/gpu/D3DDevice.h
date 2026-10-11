#pragma once

#include <d3d11_4.h>
#include <dxgi1_6.h>

#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <expected>
#include <string>

namespace rec {

enum class GpuVendor { Unknown, AMD, NVIDIA, Intel };

const char* gpuVendorName(GpuVendor vendor);

// The single D3D11 device shared by capture, colour conversion and the hardware encoder. Keeping all
// of them on one device means GPU work is ordered by submission and no cross-device sync is needed.
struct GpuDevice {
    winrt::com_ptr<IDXGIAdapter4> adapter;
    winrt::com_ptr<ID3D11Device5> device;
    winrt::com_ptr<ID3D11DeviceContext4> context;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrtDevice{nullptr}; // for WGC
    GpuVendor vendor = GpuVendor::Unknown;
    std::string adapterName;
    LUID adapterLuid{};
};

struct GpuDeviceOptions {
    HMONITOR monitor = nullptr; // pick the adapter driving this monitor; null = high-performance GPU
    bool highGpuPriority = true;
    bool debugLayer = false;
};

std::expected<GpuDevice, std::string> createGpuDevice(const GpuDeviceOptions& options);

} // namespace rec
