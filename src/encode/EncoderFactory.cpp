#include "encode/EncoderFactory.h"

#include "encode/AmfEncoder.h"
#include "encode/NvencEncoder.h"
#include "encode/QsvEncoder.h"

namespace rec {

std::unique_ptr<IVideoEncoder> makeEncoder(GpuVendor vendor) {
    switch (vendor) {
    case GpuVendor::AMD: return std::make_unique<AmfEncoder>();
    case GpuVendor::NVIDIA: return std::make_unique<NvencEncoder>();
    case GpuVendor::Intel: return std::make_unique<QsvEncoder>();
    case GpuVendor::Unknown: break;
    }
    return nullptr;
}

bool encoderAvailable(GpuVendor vendor) {
    switch (vendor) {
    case GpuVendor::AMD: return AmfEncoder::runtimeAvailable();
    case GpuVendor::NVIDIA: return NvencEncoder::runtimeAvailable();
    case GpuVendor::Intel: return QsvEncoder::runtimeAvailable();
    case GpuVendor::Unknown: break;
    }
    return false;
}

std::vector<std::string> availableEncoderNames() {
    std::vector<std::string> names;
    if (AmfEncoder::runtimeAvailable()) names.emplace_back("AMD (AMF)");
    if (NvencEncoder::runtimeAvailable()) names.emplace_back("NVIDIA (NVENC)");
    if (QsvEncoder::runtimeAvailable()) names.emplace_back("Intel (Quick Sync)");
    return names;
}

} // namespace rec
