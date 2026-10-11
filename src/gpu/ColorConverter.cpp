#include "gpu/ColorConverter.h"

#include "core/Log.h"

#include <algorithm>
#include <cmath>

namespace rec {
namespace {

constexpr size_t kMaxCachedViews = 12;

template <class Vec>
void trimCache(Vec& cache) {
    if (cache.size() > kMaxCachedViews) cache.erase(cache.begin()); // oldest first
}

} // namespace

Size scaledOutputSize(Size content, uint32_t targetHeight) {
    Size out = content;
    if (targetHeight != 0 && targetHeight < content.height && content.height != 0) {
        out.height = targetHeight;
        out.width = static_cast<uint32_t>(std::lround(static_cast<double>(content.width) * targetHeight / content.height));
    }
    out.width &= ~1u; // 4:2:0 needs even dimensions
    out.height &= ~1u;
    return out;
}

bool VideoProcessorConverter::init(ID3D11Device* device, ID3D11DeviceContext* context, Size input, Size output,
                                   DXGI_FORMAT outputFormat, uint32_t fps) {
    reset();
    if (!device || !context) return false;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(videoDevice_.put()))) ||
        FAILED(context->QueryInterface(IID_PPV_ARGS(videoContext_.put())))) {
        log::error("convert: device has no video support");
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate = {fps, 1};
    cd.InputWidth = input.width;
    cd.InputHeight = input.height;
    cd.OutputFrameRate = {fps, 1};
    cd.OutputWidth = output.width;
    cd.OutputHeight = output.height;
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    if (FAILED(videoDevice_->CreateVideoProcessorEnumerator(&cd, enumerator_.put()))) {
        log::error("convert: CreateVideoProcessorEnumerator failed for {}x{} -> {}x{}", input.width, input.height,
                   output.width, output.height);
        return false;
    }

    UINT support = 0;
    if (FAILED(enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &support)) ||
        !(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) {
        log::error("convert: BGRA input not supported by the video processor");
        return false;
    }
    if (FAILED(enumerator_->CheckVideoProcessorFormat(outputFormat, &support)) ||
        !(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
        log::error("convert: output format {} not supported by the video processor", static_cast<int>(outputFormat));
        return false;
    }
    if (FAILED(videoDevice_->CreateVideoProcessor(enumerator_.get(), 0, processor_.put()))) {
        log::error("convert: CreateVideoProcessor failed");
        return false;
    }

    auto* vp = processor_.get();
    // Without this, driver "video enhancement" settings can alter what we record.
    videoContext_->VideoProcessorSetStreamAutoProcessingMode(vp, 0, FALSE);
    videoContext_->VideoProcessorSetStreamFrameFormat(vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    videoContext_->VideoProcessorSetStreamColorSpace1(vp, 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    videoContext_->VideoProcessorSetOutputColorSpace1(vp, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
    D3D11_VIDEO_COLOR black{};
    black.YCbCr = {0.0625f, 0.5f, 0.5f, 1.0f}; // studio-range black for the letterbox bars
    videoContext_->VideoProcessorSetOutputBackgroundColor(vp, TRUE, &black);

    input_ = input;
    output_ = output;
    outputFormat_ = outputFormat;
    lastContent_ = {};
    return true;
}

void VideoProcessorConverter::reset() {
    inputViews_.clear();
    outputViews_.clear();
    processor_ = nullptr;
    enumerator_ = nullptr;
    videoContext_ = nullptr;
    videoDevice_ = nullptr;
    input_ = output_ = lastContent_ = {};
}

ID3D11VideoProcessorInputView* VideoProcessorConverter::inputView(ID3D11Texture2D* texture) {
    for (auto& [tex, view] : inputViews_)
        if (tex == texture) return view.get();
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc{};
    desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    winrt::com_ptr<ID3D11VideoProcessorInputView> view;
    if (FAILED(videoDevice_->CreateVideoProcessorInputView(texture, enumerator_.get(), &desc, view.put()))) return nullptr;
    inputViews_.emplace_back(texture, view);
    trimCache(inputViews_);
    return view.get();
}

ID3D11VideoProcessorOutputView* VideoProcessorConverter::outputView(ID3D11Texture2D* texture, UINT slice) {
    const OutputKey key{texture, slice};
    for (auto& [k, view] : outputViews_)
        if (k == key) return view.get();
    D3D11_TEXTURE2D_DESC td{};
    texture->GetDesc(&td);
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC desc{};
    if (td.ArraySize > 1) {
        desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
        desc.Texture2DArray = {0, slice, 1};
    } else {
        desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    }
    winrt::com_ptr<ID3D11VideoProcessorOutputView> view;
    if (FAILED(videoDevice_->CreateVideoProcessorOutputView(texture, enumerator_.get(), &desc, view.put()))) return nullptr;
    outputViews_.emplace_back(key, view);
    trimCache(outputViews_);
    return view.get();
}

bool VideoProcessorConverter::convert(ID3D11Texture2D* input, Size content, ID3D11Texture2D* output, UINT outputArraySlice) {
    if (!processor_) return false;
    auto* vp = processor_.get();

    if (content != lastContent_) {
        const RECT src{0, 0, static_cast<LONG>(std::min(content.width, input_.width)),
                       static_cast<LONG>(std::min(content.height, input_.height))};
        videoContext_->VideoProcessorSetStreamSourceRect(vp, 0, TRUE, &src);

        // Fit the content into the output, preserving aspect ratio; the background fills the rest.
        const double scale = std::min(static_cast<double>(output_.width) / src.right,
                                      static_cast<double>(output_.height) / src.bottom);
        const LONG w = static_cast<LONG>(std::lround(src.right * scale)) & ~1;
        const LONG h = static_cast<LONG>(std::lround(src.bottom * scale)) & ~1;
        const LONG x = ((static_cast<LONG>(output_.width) - w) / 2) & ~1;
        const LONG y = ((static_cast<LONG>(output_.height) - h) / 2) & ~1;
        const RECT dst{x, y, x + w, y + h};
        videoContext_->VideoProcessorSetStreamDestRect(vp, 0, TRUE, &dst);
        lastContent_ = content;
    }

    auto* in = inputView(input);
    auto* out = outputView(output, outputArraySlice);
    if (!in || !out) return false;

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = in;
    return SUCCEEDED(videoContext_->VideoProcessorBlt(vp, out, 0, 1, &stream));
}

} // namespace rec
