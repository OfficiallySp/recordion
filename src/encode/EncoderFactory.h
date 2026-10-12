#pragma once

#include "encode/IVideoEncoder.h"
#include "gpu/D3DDevice.h"

#include <memory>
#include <string>
#include <vector>

namespace rec {

// Frames are encoded on the GPU that captured them (no cross-adapter copies), so the backend is
// chosen by the capture adapter's vendor: AMF on AMD, NVENC on NVIDIA, Quick Sync on Intel.
std::unique_ptr<IVideoEncoder> makeEncoder(GpuVendor vendor);
bool encoderAvailable(GpuVendor vendor);
// Display names of the encoders whose runtimes are installed on this PC.
std::vector<std::string> availableEncoderNames();

} // namespace rec
