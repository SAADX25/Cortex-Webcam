#include "gpu/GpuHelpers.h"
#include <d3d11_1.h>
#include "cortex/processing/GpuImageTransform.h"
namespace cortex {
struct GpuColorConverter::Impl {
    std::shared_ptr<GpuDevice> gpu;
    ComPtr<ID3D11VideoDevice> video;
    ComPtr<ID3D11VideoContext> context;
    ComPtr<ID3D11VideoContext1> context1;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    ComPtr<ID3D11VideoProcessor> processor;
    ComPtr<ID3D11VideoProcessorOutputView> output;
    GpuImage rgb;
    std::unique_ptr<GpuImageTransform> transform;
};
GpuColorConverter::GpuColorConverter(std::shared_ptr<GpuDevice> gpu)
    : impl_(std::make_unique<Impl>()) {
    auto& s = *impl_;
    s.gpu = std::move(gpu);
    s.transform = std::make_unique<GpuImageTransform>(s.gpu);
    detail::hr(s.gpu->device.As(&s.video), "D3D11 video device");
    detail::hr(s.gpu->context.As(&s.context), "D3D11 video context");
    detail::hr(s.context.As(&s.context1), "D3D11 video color space support");
}
GpuColorConverter::~GpuColorConverter() = default;
GpuImage GpuColorConverter::convert(const VideoFrame& frame) {
    auto& s = *impl_;
    if (frame.gpu != s.gpu || !frame.texture || !frame.width || !frame.height)
        throw std::invalid_argument("Invalid GPU frame");
    std::lock_guard lock(s.gpu->mutex);
    D3D11_TEXTURE2D_DESC inputDesc{};
    frame.texture->GetDesc(&inputDesc);
    if (inputDesc.Format != DXGI_FORMAT_NV12 || frame.arraySlice >= inputDesc.ArraySize ||
        frame.width > inputDesc.Width || frame.height > inputDesc.Height)
        throw std::invalid_argument("NV12 frame contract violated");
    if (s.rgb.width != frame.width || s.rgb.height != frame.height) {
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputWidth = frame.width;
        content.InputHeight = frame.height;
        content.OutputWidth = frame.width;
        content.OutputHeight = frame.height;
        content.InputFrameRate = {60, 1};
        content.OutputFrameRate = {60, 1};
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        s.enumerator.Reset();
        s.processor.Reset();
        s.output.Reset();
        detail::hr(s.video->CreateVideoProcessorEnumerator(&content, &s.enumerator),
                   "Video processor enumerator");
        detail::hr(s.video->CreateVideoProcessor(s.enumerator.Get(), 0, &s.processor),
                   "GPU color converter");
        s.rgb = detail::image(*s.gpu, frame.width, frame.height);
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view{};
        view.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        detail::hr(s.video->CreateVideoProcessorOutputView(s.rgb.texture.Get(), s.enumerator.Get(),
                                                           &view, &s.output),
                   "Video output view");
    }
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc{};
    desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    desc.Texture2D.ArraySlice = frame.arraySlice;
    ComPtr<ID3D11VideoProcessorInputView> input;
    detail::hr(s.video->CreateVideoProcessorInputView(frame.texture.Get(), s.enumerator.Get(),
                                                      &desc, &input),
               "Video input view");
    RECT rect{0, 0, static_cast<LONG>(frame.width), static_cast<LONG>(frame.height)};
    s.context->VideoProcessorSetStreamFrameFormat(s.processor.Get(), 0,
                                                  D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    s.context->VideoProcessorSetStreamSourceRect(s.processor.Get(), 0, TRUE, &rect);
    s.context->VideoProcessorSetStreamDestRect(s.processor.Get(), 0, TRUE, &rect);
    s.context->VideoProcessorSetOutputTargetRect(s.processor.Get(), TRUE, &rect);
    const bool full = frame.range == VideoFrame::Range::Full;
    auto matrix = frame.matrix;
    if (matrix == VideoFrame::Matrix::Unspecified)
        matrix = frame.height >= 720 ? VideoFrame::Matrix::Bt709 : VideoFrame::Matrix::Bt601;
    auto color = matrix == VideoFrame::Matrix::Bt601
                     ? (full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601
                             : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601)
                 : matrix == VideoFrame::Matrix::Bt2020
                     ? (full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020
                             : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020)
                     : (full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709
                             : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
    s.context1->VideoProcessorSetStreamColorSpace1(s.processor.Get(), 0, color);
    s.context1->VideoProcessorSetOutputColorSpace1(s.processor.Get(),
                                                   DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    s.context->VideoProcessorSetStreamAutoProcessingMode(s.processor.Get(), 0, FALSE);
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = input.Get();
    detail::hr(s.context->VideoProcessorBlt(s.processor.Get(), s.output.Get(), 0, 1, &stream),
               "NV12 GPU conversion");
    return s.transform->rotate(s.rgb, frame.rotationDegrees);
}
}  // namespace cortex
