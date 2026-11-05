#include "color_converter.h"

#include "win_error.h"

using Microsoft::WRL::ComPtr;

namespace aum {

ColorConverter::ColorConverter(GpuDevice& gpu, std::uint32_t width, std::uint32_t height) : gpu_(gpu) {
    ThrowIfFailed(gpu_.device.As(&videoDevice_), "ID3D11VideoDevice query");
    ThrowIfFailed(gpu_.context.As(&videoContext_), "ID3D11VideoContext1 query");

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputWidth = width;
    content.InputHeight = height;
    content.OutputWidth = width;
    content.OutputHeight = height;
    content.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    ThrowIfFailed(videoDevice_->CreateVideoProcessorEnumerator(&content, &enumerator_), "CreateVideoProcessorEnumerator");
    ThrowIfFailed(videoDevice_->CreateVideoProcessor(enumerator_.Get(), 0, &processor_), "CreateVideoProcessor");

    videoContext_->VideoProcessorSetStreamColorSpace1(processor_.Get(), 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    videoContext_->VideoProcessorSetOutputColorSpace1(processor_.Get(), DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
    videoContext_->VideoProcessorSetStreamAutoProcessingMode(processor_.Get(), 0, FALSE);
    videoContext_->VideoProcessorSetStreamFrameFormat(processor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
}

void ColorConverter::Convert(ID3D11Texture2D* bgra, ID3D11Texture2D* nv12) {
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{};
    inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11VideoProcessorInputView> input;
    ThrowIfFailed(videoDevice_->CreateVideoProcessorInputView(bgra, enumerator_.Get(), &inputDesc, &input), "CreateVideoProcessorInputView");

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc{};
    outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11VideoProcessorOutputView> output;
    ThrowIfFailed(videoDevice_->CreateVideoProcessorOutputView(nv12, enumerator_.Get(), &outputDesc, &output), "CreateVideoProcessorOutputView");

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = input.Get();
    ThrowIfFailed(videoContext_->VideoProcessorBlt(processor_.Get(), output.Get(), 0, 1, &stream), "VideoProcessorBlt");
}

}  // namespace aum
