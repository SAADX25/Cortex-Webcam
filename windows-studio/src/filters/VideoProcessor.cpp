#include "cortex/filters/VideoProcessor.h"
#include "cortex/filters/RvmMatting.h"
#include "GpuHelpers.h"
#include <wincodec.h>
#include <cmath>
#include <sstream>
#include <iomanip>
namespace cortex {
struct VideoProcessor::Impl {
    struct Source {
        GpuColorConverter converter;
        FramePtr previous;
        GpuImage rgb;
        std::unique_ptr<RvmMatting> matting;
        ComPtr<ID3D11ShaderResourceView> mask;
        explicit Source(std::shared_ptr<GpuDevice> gpu)
            : converter(std::move(gpu)) {}
    };
    std::shared_ptr<GpuDevice> gpu;
    GpuImage target, background, exported;
    ComPtr<ID3D11RenderTargetView> targetView, exportView;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> composite, backgroundShader, exportShader;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11RasterizerState> rasterizer;
    std::map<SourceId, std::unique_ptr<Source>> sources;
    std::unique_ptr<GpuColorConverter> backgroundConverter;
    std::filesystem::path model;
    FilterSettings settings;
    std::unique_ptr<AvatarFilter> avatar;
    std::unique_ptr<AvatarModel> avatarModel;
    std::unique_ptr<FaceTracker> tracker;
    std::chrono::steady_clock::time_point avatarStart, poseTime;
    AvatarPose smoothedPose, neutralPose;
    uint64_t calibration{};
    std::string trackingStatus{"Open Cortex Sender to track your face"};
    void draw(const GpuImage& image, ID3D11ShaderResourceView* mask, NormalizedRect rect,
              bool isBackground) {
        const float availableW = target.width * rect.width,
                    availableH = target.height * rect.height;
        float width = availableW, height = availableH;
        if (!isBackground) {
            const float scale = std::min(availableW / image.width, availableH / image.height);
            width = image.width * scale;
            height = image.height * scale;
        }
        float uvX = 1, uvY = 1;
        if (isBackground) {
            const float scale =
                (settings.backgroundFit
                     ? std::min(availableW / image.width, availableH / image.height)
                     : std::max(availableW / image.width, availableH / image.height)) *
                settings.backgroundZoom;
            uvX = availableW / (image.width * scale);
            uvY = availableH / (image.height * scale);
        }
        D3D11_VIEWPORT viewport{
            target.width * rect.x +
                (availableW - width) * (isBackground ? settings.backgroundX : .5f),
            target.height * rect.y +
                (availableH - height) * (isBackground ? settings.backgroundY : .5f),
            width,
            height,
            0,
            1};
        auto* context = gpu->context.Get();
        context->RSSetViewports(1, &viewport);
        const float values[] = {settings.similarity,
                                settings.softness,
                                settings.spill,
                                static_cast<float>(settings.removal == RemovalMode::ChromaKey ? 1
                                                   : settings.removal == RemovalMode::AiMatting
                                                       ? 2
                                                       : 0),
                                uvX,
                                uvY,
                                (1 - uvX) * settings.backgroundX,
                                (1 - uvY) * settings.backgroundY};
        context->UpdateSubresource(constants.Get(), 0, nullptr, values, 0, 0);
        ID3D11ShaderResourceView* views[] = {image.view.Get(), mask};
        context->PSSetShaderResources(0, 2, views);
        context->PSSetShader(isBackground ? backgroundShader.Get() : composite.Get(), nullptr, 0);
        context->Draw(3, 0);
        ID3D11ShaderResourceView* clear[2]{};
        context->PSSetShaderResources(0, 2, clear);
    }
};
VideoProcessor::VideoProcessor(std::shared_ptr<GpuDevice> gpu, uint32_t width, uint32_t height)
    : impl_(std::make_unique<Impl>()) {
    if (!gpu || !width || !height || width > 3840 || height > 3840 ||
        uint64_t(width) * height > 3840ULL * 2160)
        throw std::invalid_argument("Invalid output dimensions");
    auto& s = *impl_;
    s.gpu = std::move(gpu);
    s.backgroundConverter = std::make_unique<GpuColorConverter>(s.gpu);
    std::lock_guard lock(s.gpu->mutex);
    s.target = detail::image(*s.gpu, width, height);
    s.exported = detail::image(*s.gpu, width, height);
    detail::hr(
        s.gpu->device->CreateRenderTargetView(s.exported.texture.Get(), nullptr, &s.exportView),
        "Export target");
    detail::hr(
        s.gpu->device->CreateRenderTargetView(s.target.texture.Get(), nullptr, &s.targetView),
        "Composite target");
    auto vs = detail::compile("Fullscreen", "vs_5_0"), ps = detail::compile("Composite", "ps_5_0"),
         bg = detail::compile("Background", "ps_5_0");
    detail::hr(s.gpu->device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(),
                                                 nullptr, &s.vertex),
               "Fullscreen shader");
    detail::hr(s.gpu->device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(),
                                                nullptr, &s.composite),
               "Chroma key shader");
    detail::hr(s.gpu->device->CreatePixelShader(bg->GetBufferPointer(), bg->GetBufferSize(),
                                                nullptr, &s.backgroundShader),
               "Background shader");
    auto exported = detail::compile("Export", "ps_5_0");
    detail::hr(s.gpu->device->CreatePixelShader(exported->GetBufferPointer(),
                                                exported->GetBufferSize(), nullptr,
                                                &s.exportShader),
               "Alpha export shader");
    D3D11_BUFFER_DESC b{};
    b.ByteWidth = 32;
    b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    detail::hr(s.gpu->device->CreateBuffer(&b, nullptr, &s.constants), "Filter parameters");
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    detail::hr(s.gpu->device->CreateSamplerState(&sampler, &s.sampler), "Composite sampler");
    D3D11_BLEND_DESC blend{};
    auto& rt = blend.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    detail::hr(s.gpu->device->CreateBlendState(&blend, &s.blend), "Alpha blending");
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    detail::hr(s.gpu->device->CreateRasterizerState(&raster, &s.rasterizer),
               "Composite rasterizer");
}
VideoProcessor::~VideoProcessor() = default;
void VideoProcessor::setAvatarModel(std::unique_ptr<AvatarModel> model) {
    impl_->avatarModel = std::move(model);
}
void VideoProcessor::setFaceTracker(std::unique_ptr<FaceTracker> tracker) {
    impl_->tracker = std::move(tracker);
}
void VideoProcessor::setOutputSize(uint32_t width, uint32_t height) {
    auto& s = *impl_;
    if (s.target.width == width && s.target.height == height)
        return;
    if (!width || !height || width > 3840 || height > 3840 ||
        uint64_t(width) * height > 3840ULL * 2160)
        throw std::invalid_argument("Invalid output dimensions");
    std::lock_guard lock(s.gpu->mutex);
    s.targetView.Reset();
    s.exportView.Reset();
    s.target = detail::image(*s.gpu, width, height);
    s.exported = detail::image(*s.gpu, width, height);
    detail::hr(
        s.gpu->device->CreateRenderTargetView(s.target.texture.Get(), nullptr, &s.targetView),
        "Composite target resize");
    detail::hr(
        s.gpu->device->CreateRenderTargetView(s.exported.texture.Get(), nullptr, &s.exportView),
        "Export target resize");
}
const FilterSettings& VideoProcessor::settings() const {
    return impl_->settings;
}
std::string VideoProcessor::avatarTrackingStatus() const {
    return impl_->trackingStatus;
}
void VideoProcessor::setSettings(const FilterSettings& settings) {
    if (!std::isfinite(settings.avatarSensitivity) || settings.avatarSensitivity < .5f ||
        settings.avatarSensitivity > 2.5f)
        throw std::invalid_argument("Invalid head movement sensitivity");
    if (!std::isfinite(settings.avatarSmoothing) || settings.avatarSmoothing < 0 ||
        settings.avatarSmoothing > .3f)
        throw std::invalid_argument("Invalid avatar smoothing");
    if (!std::isfinite(settings.similarity) || !std::isfinite(settings.softness) ||
        !std::isfinite(settings.spill) || settings.similarity < 0 || settings.similarity > 1 ||
        settings.softness < 0.001f || settings.softness > 1 || settings.spill < 0 ||
        settings.spill > 1)
        throw std::invalid_argument("Invalid chroma key controls");
    const auto& framing = settings.avatarFraming;
    if (!std::isfinite(framing.scale) || !std::isfinite(framing.x) || !std::isfinite(framing.y) ||
        framing.scale < .25f || framing.scale > 6 || std::abs(framing.x) > 1 ||
        std::abs(framing.y) > 1)
        throw std::invalid_argument("Invalid avatar size or position");
    if (settings.removal != impl_->settings.removal)
        for (auto& [id, source] : impl_->sources) {
            source->previous.reset();
            if (source->matting)
                source->matting->reset();
        }
    if (settings.avatarEnabled && !impl_->settings.avatarEnabled)
        impl_->avatarStart = std::chrono::steady_clock::now();
    impl_->settings = settings;
}
void VideoProcessor::setMattingModel(const std::filesystem::path& model) {
    if (!std::filesystem::is_regular_file(model))
        throw std::runtime_error("Matting model not found");
    impl_->model = model;
    for (auto& [id, source] : impl_->sources) {
        source->matting.reset();
        source->previous.reset();
    }
}
void VideoProcessor::clearBackground() {
    impl_->background = {};
}
void VideoProcessor::setBackgroundImage(const std::filesystem::path& path) {
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct ComScope {
        bool active;
        ~ComScope() {
            if (active)
                CoUninitialize();
        }
    } scope{SUCCEEDED(initialized)};
    ComPtr<IWICImagingFactory> factory;
    detail::hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)),
               "WIC image factory");
    ComPtr<IWICBitmapDecoder> decoder;
    detail::hr(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnLoad, &decoder),
               "Load background image");
    ComPtr<IWICBitmapFrameDecode> frame;
    detail::hr(decoder->GetFrame(0, &frame), "Read background image");
    UINT width{}, height{};
    frame->GetSize(&width, &height);
    if (!width || !height)
        throw std::runtime_error("Background image is empty");
    ComPtr<IWICBitmapScaler> scaler;
    IWICBitmapSource* source = frame.Get();
    if (width > 4096 || height > 4096) {
        const double scale = 4096.0 / std::max(width, height);
        width = std::max(1u, static_cast<UINT>(width * scale));
        height = std::max(1u, static_cast<UINT>(height * scale));
        detail::hr(factory->CreateBitmapScaler(&scaler), "Image scaler");
        detail::hr(scaler->Initialize(frame.Get(), width, height, WICBitmapInterpolationModeFant),
                   "Resize background");
        source = scaler.Get();
    }
    ComPtr<IWICFormatConverter> converter;
    detail::hr(factory->CreateFormatConverter(&converter), "Image RGBA converter");
    detail::hr(converter->Initialize(source, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                     nullptr, 0, WICBitmapPaletteTypeCustom),
               "Decode image RGBA");
    std::vector<uint8_t> pixels(size_t(width) * height * 4);
    detail::hr(
        converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()),
        "Copy background image");
    std::lock_guard lock(impl_->gpu->mutex);
    impl_->background = detail::image(*impl_->gpu, width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
                                      D3D11_BIND_SHADER_RESOURCE);
    impl_->gpu->context->UpdateSubresource(impl_->background.texture.Get(), 0, nullptr,
                                           pixels.data(), width * 4, 0);
}
GpuImage VideoProcessor::render(const SceneSnapshot& scene, FramePtr backgroundVideo) {
    auto& s = *impl_;
    // Convert/infer before binding the composite target. AI has an explicit reduced-size I/O
    // bridge.
    AvatarPose pose;
    if (s.settings.avatarEnabled) {
        if (!s.avatar)
            s.avatar = std::make_unique<AvatarFilter>(s.gpu);
        if (s.settings.avatarTestMotion) {
            const auto elapsed = std::chrono::steady_clock::now() - s.avatarStart;
            pose = AvatarFilter::testPose(std::chrono::duration<double>(elapsed).count());
        } else {
            const auto now = std::chrono::steady_clock::now();
            AvatarPose targetPose;
            targetPose.cameraDriven = true;
            targetPose.armsEnabled = s.settings.avatarArms;
            s.trackingStatus = "Waiting for phone camera - open Cortex Sender";
            if (!scene.layers.empty() && scene.layers.front().frame &&
                now - scene.layers.front().frame->readyAt < std::chrono::milliseconds(500)) {
                if (!s.tracker)
                    s.tracker = makeFaceTracker(s.gpu);
                s.tracker->setBodyTracking(s.settings.avatarArms);
                if (auto landmarks = s.tracker->track(*scene.layers.front().frame)) {
                    if (auto measured = AvatarFilter::map(*landmarks)) {
                        if (s.calibration != s.settings.avatarCalibration) {
                            s.neutralPose = *measured;
                            s.calibration = s.settings.avatarCalibration;
                        }
                        targetPose = *measured;
                        targetPose.yaw = std::clamp((targetPose.yaw - s.neutralPose.yaw) *
                                                        s.settings.avatarSensitivity,
                                                    -30.f, 30.f);
                        targetPose.pitch = std::clamp((targetPose.pitch - s.neutralPose.pitch) *
                                                          s.settings.avatarSensitivity,
                                                      -30.f, 30.f);
                        targetPose.roll = std::clamp((targetPose.roll - s.neutralPose.roll) *
                                                         s.settings.avatarSensitivity,
                                                     -30.f, 30.f);
                    }
                }
                targetPose.armsEnabled = s.settings.avatarArms;
                if (s.settings.avatarArms) {
                    const auto body = s.tracker->upperBody().value_or(UpperBodyPose{});
                    targetPose.arms = body;
                    targetPose.bodyAngleX = body.bodyAngleX;
                    targetPose.bodyAngleY = body.bodyAngleY;
                    targetPose.bodyAngleZ = body.bodyAngleZ;
                }
                if (s.settings.avatarMirror) {
                    targetPose.yaw = -targetPose.yaw;
                    targetPose.roll = -targetPose.roll;
                    targetPose.gazeX = -targetPose.gazeX;
                    targetPose.bodyAngleX = -targetPose.bodyAngleX;
                    targetPose.bodyAngleZ = -targetPose.bodyAngleZ;
                    std::swap(targetPose.arms.left, targetPose.arms.right);
                    std::swap(targetPose.browLY, targetPose.browRY);
                    std::swap(targetPose.leftEyeOpen, targetPose.rightEyeOpen);
                }
                s.trackingStatus = s.tracker->status();
            }
            const float dt =
                s.poseTime.time_since_epoch().count() == 0
                    ? 1.f / 30
                    : std::clamp(std::chrono::duration<float>(now - s.poseTime).count(), 0.f, .1f);
            s.poseTime = now;
            // Faster eyelids preserve natural blinks while head motion stays smooth.
            pose = smoothAvatarPose(s.smoothedPose, targetPose, dt, s.settings.avatarSmoothing);
            const auto fast = smoothAvatarPose(s.smoothedPose, targetPose, dt,
                                               std::min(.020f, s.settings.avatarSmoothing));
            pose.eyeOpen = fast.eyeOpen;
            pose.leftEyeOpen = fast.leftEyeOpen;
            pose.rightEyeOpen = fast.rightEyeOpen;
            s.smoothedPose = pose;
            std::ostringstream values;
            values << " | Head " << std::fixed << std::setprecision(0) << pose.yaw << " / "
                   << pose.pitch << " / " << pose.roll;
            s.trackingStatus += values.str();
        }
    }
    if (!s.settings.avatarEnabled)
        for (const auto& layer : scene.layers) {
            if (!layer.frame)
                continue;
            auto& source = s.sources[layer.source];
            if (!source)
                source = std::make_unique<Impl::Source>(s.gpu);
            if (source->previous != layer.frame) {
                source->rgb = source->converter.convert(*layer.frame);
                if (s.settings.removal == RemovalMode::AiMatting) {
                    if (!source->matting)
                        source->matting = std::make_unique<RvmMatting>(s.gpu, s.model);
                    if (source->previous &&
                        (layer.frame->rotationDegrees != source->previous->rotationDegrees ||
                         layer.frame->sourceTimestamp90k <= source->previous->sourceTimestamp90k ||
                         layer.frame->readyAt - source->previous->readyAt >
                             std::chrono::milliseconds(500)))
                        source->matting->reset();
                    source->mask = source->matting->infer(source->rgb);
                }
                source->previous = layer.frame;
            }
        }
    GpuImage background = s.background;
    if (backgroundVideo)
        background = s.backgroundConverter->convert(*backgroundVideo);
    std::lock_guard lock(s.gpu->mutex);
    auto* context = s.gpu->context.Get();
    auto* target = s.targetView.Get();
    context->OMSetRenderTargets(1, &target, nullptr);
    context->OMSetBlendState(s.blend.Get(), nullptr, 0xffffffff);
    context->OMSetDepthStencilState(nullptr, 0);
    context->RSSetState(s.rasterizer.Get());
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(s.vertex.Get(), nullptr, 0);
    auto* sampler = s.sampler.Get();
    context->PSSetSamplers(0, 1, &sampler);
    auto* buffer = s.constants.Get();
    context->PSSetConstantBuffers(0, 1, &buffer);
    const float clear[] = {s.settings.transparent ? 0 : s.settings.background[0],
                           s.settings.transparent ? 0 : s.settings.background[1],
                           s.settings.transparent ? 0 : s.settings.background[2],
                           s.settings.transparent ? 0.0f : 1.0f};
    context->ClearRenderTargetView(target, clear);
    if (background.view && !s.settings.transparent)
        s.draw(background, nullptr, {}, true);
    if (s.settings.avatarEnabled) {
        if (s.settings.avatarMode == 1 && s.avatarModel) {
            s.avatarModel->setAnimation(s.settings.avatarMotion, s.settings.avatarExpression);
            s.avatarModel->setFraming(s.settings.avatarFraming);
            s.avatarModel->draw(context, pose, s.target.width, s.target.height);
        } else
            s.avatar->draw(pose, s.target.width, s.target.height, s.settings.avatarFraming);
    } else if (s.settings.removal != RemovalMode::BackgroundOnly)
        for (const auto& layer : scene.layers)
            if (layer.frame) {
                auto& source = *s.sources.at(layer.source);
                s.draw(source.rgb, source.mask.Get(), layer.destination, false);
            }
    context->OMSetRenderTargets(0, nullptr, nullptr);
    return s.target;
}
GpuImage VideoProcessor::exportFrame(bool preserveAlpha, bool bottomUp) {
    auto& s = *impl_;
    std::lock_guard lock(s.gpu->mutex);
    auto* context = s.gpu->context.Get();
    auto* target = s.exportView.Get();
    context->OMSetRenderTargets(1, &target, nullptr);
    context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    context->OMSetDepthStencilState(nullptr, 0);
    context->RSSetState(s.rasterizer.Get());
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(s.vertex.Get(), nullptr, 0);
    auto* sampler = s.sampler.Get();
    context->PSSetSamplers(0, 1, &sampler);
    D3D11_VIEWPORT viewport{
        0, 0, static_cast<float>(s.target.width), static_cast<float>(s.target.height), 0, 1};
    context->RSSetViewports(1, &viewport);
    const float values[8] = {bottomUp ? 1.0f : 0.0f, 0, 0, preserveAlpha ? 1.0f : 0.0f};
    context->UpdateSubresource(s.constants.Get(), 0, nullptr, values, 0, 0);
    auto* buffer = s.constants.Get();
    context->PSSetConstantBuffers(0, 1, &buffer);
    auto* view = s.target.view.Get();
    context->PSSetShaderResources(0, 1, &view);
    context->PSSetShader(s.exportShader.Get(), nullptr, 0);
    context->Draw(3, 0);
    view = nullptr;
    context->PSSetShaderResources(0, 1, &view);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    return s.exported;
}
}  // namespace cortex
