#include "gpu/D3DDevice.h"

#include "core/Log.h"
#include "core/Utf.h"

#include <windows.graphics.directx.direct3d11.interop.h>

#include <format>

namespace rec {
namespace {

GpuVendor vendorFromId(UINT id) {
    switch (id) {
    case 0x1002: return GpuVendor::AMD;
    case 0x10DE: return GpuVendor::NVIDIA;
    case 0x8086: return GpuVendor::Intel;
    default: return GpuVendor::Unknown;
    }
}

winrt::com_ptr<IDXGIAdapter4> adapterForMonitor(IDXGIFactory6* factory, HMONITOR monitor) {
    if (monitor) {
        winrt::com_ptr<IDXGIAdapter1> adapter;
        for (UINT a = 0; factory->EnumAdapters1(a, adapter.put()) != DXGI_ERROR_NOT_FOUND; ++a) {
            winrt::com_ptr<IDXGIOutput> output;
            for (UINT o = 0; adapter->EnumOutputs(o, output.put()) != DXGI_ERROR_NOT_FOUND; ++o) {
                DXGI_OUTPUT_DESC desc{};
                if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) return adapter.as<IDXGIAdapter4>();
                output = nullptr;
            }
            adapter = nullptr;
        }
        log::warn("gpu: no adapter owns the target monitor; using the high-performance GPU");
    }
    winrt::com_ptr<IDXGIAdapter4> adapter;
    factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(adapter.put()));
    return adapter;
}

} // namespace

const char* gpuVendorName(GpuVendor vendor) {
    switch (vendor) {
    case GpuVendor::AMD: return "AMD";
    case GpuVendor::NVIDIA: return "NVIDIA";
    case GpuVendor::Intel: return "Intel";
    case GpuVendor::Unknown: break;
    }
    return "Unknown";
}

std::expected<GpuDevice, std::string> createGpuDevice(const GpuDeviceOptions& options) {
    winrt::com_ptr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.put())))) return std::unexpected("CreateDXGIFactory2 failed");

    GpuDevice gpu;
    gpu.adapter = adapterForMonitor(factory.get(), options.monitor);
    if (!gpu.adapter) return std::unexpected("no DXGI adapter found");

    DXGI_ADAPTER_DESC3 desc{};
    gpu.adapter->GetDesc3(&desc);
    gpu.vendor = vendorFromId(desc.VendorId);
    gpu.adapterName = narrow(desc.Description);
    gpu.adapterLuid = desc.AdapterLuid;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    if (options.debugLayer) flags |= D3D11_CREATE_DEVICE_DEBUG;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1};

    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    HRESULT hr = D3D11CreateDevice(gpu.adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
                                   static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, device.put(), nullptr,
                                   context.put());
    if (FAILED(hr) && options.debugLayer) { // debug layer not installed
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(gpu.adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
                               static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, device.put(), nullptr,
                               context.put());
    }
    if (FAILED(hr)) return std::unexpected(std::format("D3D11CreateDevice failed (0x{:08X})", static_cast<uint32_t>(hr)));

    gpu.device = device.as<ID3D11Device5>();
    gpu.context = context.as<ID3D11DeviceContext4>();

    // The encoder runtimes (and oneVPL in particular) submit from their own threads.
    gpu.device.as<ID3D11Multithread>()->SetMultithreadProtected(TRUE);

    auto dxgiDevice = gpu.device.as<IDXGIDevice>();
    if (options.highGpuPriority) {
        // Our per-frame GPU work is tiny; running it ahead of the game's avoids dropped recorded frames
        // in GPU-bound games. Positive values need SeIncreaseBasePriorityPrivilege on some systems.
        if (FAILED(dxgiDevice->SetGPUThreadPriority(7))) log::info("gpu: elevated GPU thread priority not permitted");
    }

    winrt::com_ptr<::IInspectable> inspectable;
    if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put())))
        return std::unexpected("CreateDirect3D11DeviceFromDXGIDevice failed");
    gpu.winrtDevice = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

    log::info("gpu: {} ({}), feature level 0x{:X}", gpu.adapterName, gpuVendorName(gpu.vendor),
              static_cast<int>(gpu.device->GetFeatureLevel()));
    return gpu;
}

} // namespace rec
