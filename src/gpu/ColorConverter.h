#pragma once

#include <d3d11_4.h>

#include <winrt/base.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rec {

struct Size {
    uint32_t width = 0;
    uint32_t height = 0;
    bool operator==(const Size&) const = default;
};

// Largest even size that fits `content` into `targetHeight` rows preserving aspect (0 = native).
Size scaledOutputSize(Size content, uint32_t targetHeight);

// BGRA (full-range RGB) -> NV12 / P010 (BT.709 limited range) with scaling and letterboxing, via
// ID3D11VideoProcessor. Input views are cached per source texture, output views per target.
class VideoProcessorConverter {
public:
    bool init(ID3D11Device* device, ID3D11DeviceContext* context, Size input, Size output, DXGI_FORMAT outputFormat,
              uint32_t fps);
    // `content` is the valid region of `input` (WGC textures can be larger than the content).
    bool convert(ID3D11Texture2D* input, Size content, ID3D11Texture2D* output, UINT outputArraySlice = 0);
    void reset();

    Size inputSize() const { return input_; }
    Size outputSize() const { return output_; }
    DXGI_FORMAT outputFormat() const { return outputFormat_; }

private:
    ID3D11VideoProcessorInputView* inputView(ID3D11Texture2D* texture);
    ID3D11VideoProcessorInputView* copiedInputView(ID3D11Texture2D* texture);
    ID3D11VideoProcessorOutputView* outputView(ID3D11Texture2D* texture, UINT slice);

    winrt::com_ptr<ID3D11VideoDevice> videoDevice_;
    winrt::com_ptr<ID3D11DeviceContext> context_;
    winrt::com_ptr<ID3D11VideoContext1> videoContext_;
    winrt::com_ptr<ID3D11VideoProcessorEnumerator> enumerator_;
    winrt::com_ptr<ID3D11VideoProcessor> processor_;
    Size input_{};
    Size output_{};
    Size lastContent_{};
    DXGI_FORMAT outputFormat_ = DXGI_FORMAT_UNKNOWN;

    // Small caches: WGC cycles through a handful of pool surfaces; encoders own ~8 input surfaces.
    std::vector<std::pair<ID3D11Texture2D*, winrt::com_ptr<ID3D11VideoProcessorInputView>>> inputViews_;
    // For sources whose textures the video processor refuses (AMD Direct Capture's): copied here first.
    winrt::com_ptr<ID3D11Texture2D> copy_;
    winrt::com_ptr<ID3D11VideoProcessorInputView> copyView_;
    struct OutputKey {
        ID3D11Texture2D* texture;
        UINT slice;
        bool operator==(const OutputKey&) const = default;
    };
    std::vector<std::pair<OutputKey, winrt::com_ptr<ID3D11VideoProcessorOutputView>>> outputViews_;
};

} // namespace rec
