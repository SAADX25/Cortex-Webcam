// Copyright (c) 2019 Emiliana. OpenSeeFace decoding adapted under BSD-2-Clause.
// See third_party/OpenSeeFace/LICENSE.
// Model provenance and license are shipped in models/face-tracking.
#include "cortex/filters/AvatarFilter.h"
#include "GpuHelpers.h"
#include "cortex/filters/HeadPose.h"
#include "cortex/filters/BodyTracker.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#ifdef CORTEX_HAS_DIRECTML
#include <onnxruntime_cxx_api.h>
#endif

namespace cortex {
namespace {
using Clock = std::chrono::steady_clock;
class UnavailableTracker final : public FaceTracker {
public:
    std::optional<FaceLandmarks> track(const VideoFrame&) override { return {}; }
    std::string status() const override {
        return "Face tracking unavailable: ONNX Runtime not built";
    }
};
#ifdef CORTEX_HAS_DIRECTML
struct TrackingImage {
    unsigned width{}, height{};
    std::vector<uint8_t> rgba;
    Clock::time_point captured;
};
struct Box {
    float x{}, y{}, width{}, height{};
};
struct Result {
    std::optional<FaceLandmarks> landmarks;
    Clock::time_point captured;
};
std::filesystem::path defaultModels() {
    std::wstring executable(32768, L'\0');
    const auto size =
        GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!size || size >= executable.size())
        throw std::runtime_error("Cannot locate face models");
    executable.resize(size);
    return std::filesystem::path(executable).parent_path() / "models" / "face-tracking";
}
class OnnxFaceTracker final : public FaceTracker {
    std::shared_ptr<GpuDevice> gpu_;
    GpuColorConverter converter_;
    GpuImage small_;
    ComPtr<ID3D11RenderTargetView> target_;
    ComPtr<ID3D11Texture2D> staging_;
    ComPtr<ID3D11VertexShader> vertex_;
    ComPtr<ID3D11PixelShader> pixel_;
    ComPtr<ID3D11RasterizerState> raster_;
    ComPtr<ID3D11SamplerState> sampler_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<TrackingImage> pending_;
    Result result_;
    std::string status_{"Loading face tracking models"};
    bool closing_{};
    std::atomic<bool> failed_{};
    std::atomic<bool> bodyEnabled_{};
    UpperBodyPose body_;
    Clock::time_point bodyTime_{};
    std::string bodyStatus_;
    Clock::time_point lastSubmit_{};
    int64_t lastTimestamp_{INT64_MIN};
    ID3D11Texture2D* lastTexture_{};
    std::thread worker_;

    TrackingImage capture(const VideoFrame& frame) {
        const auto rgb =
            converter_.convert(frame);  // Handles NV12 array slices and phone rotation.
        const float ratio = std::min(1.f, 512.f / std::max(rgb.width, rgb.height));
        const auto width = std::max(1u, static_cast<unsigned>(rgb.width * ratio));
        const auto height = std::max(1u, static_cast<unsigned>(rgb.height * ratio));
        std::lock_guard lock(gpu_->mutex);
        auto* context = gpu_->context.Get();
        if (!vertex_) {
            auto vs = detail::compile("Fullscreen", "vs_5_0");
            auto ps = detail::compile("TrackingDownsample", "ps_5_0");
            detail::hr(gpu_->device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(),
                                                        nullptr, &vertex_),
                       "Tracking vertex shader");
            detail::hr(gpu_->device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(),
                                                       nullptr, &pixel_),
                       "Tracking resize shader");
            D3D11_SAMPLER_DESC sample{};
            sample.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sample.AddressU = sample.AddressV = sample.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sample.MaxLOD = D3D11_FLOAT32_MAX;
            detail::hr(gpu_->device->CreateSamplerState(&sample, &sampler_), "Tracking sampler");
            D3D11_RASTERIZER_DESC raster{};
            raster.FillMode = D3D11_FILL_SOLID;
            raster.CullMode = D3D11_CULL_NONE;
            raster.DepthClipEnable = TRUE;
            detail::hr(gpu_->device->CreateRasterizerState(&raster, &raster_),
                       "Tracking rasterizer");
        }
        if (small_.width != width || small_.height != height) {
            target_.Reset();
            staging_.Reset();
            small_ = detail::image(*gpu_, width, height);
            detail::hr(
                gpu_->device->CreateRenderTargetView(small_.texture.Get(), nullptr, &target_),
                "Tracking target");
            D3D11_TEXTURE2D_DESC desc{};
            small_.texture->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            detail::hr(gpu_->device->CreateTexture2D(&desc, nullptr, &staging_),
                       "Tracking readback");
        }
        auto* target = target_.Get();
        auto* view = rgb.view.Get();
        auto* sample = sampler_.Get();
        D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
        context->OMSetRenderTargets(1, &target, nullptr);
        context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        context->OMSetDepthStencilState(nullptr, 0);
        context->RSSetState(raster_.Get());
        context->RSSetViewports(1, &viewport);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex_.Get(), nullptr, 0);
        context->PSSetShader(pixel_.Get(), nullptr, 0);
        context->PSSetSamplers(0, 1, &sample);
        context->PSSetShaderResources(0, 1, &view);
        context->Draw(3, 0);
        view = nullptr;
        context->PSSetShaderResources(0, 1, &view);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->CopyResource(staging_.Get(), small_.texture.Get());
        TrackingImage image{width, height, std::vector<uint8_t>(size_t(width) * height * 4),
                            Clock::now()};
        D3D11_MAPPED_SUBRESOURCE map{};
        detail::hr(context->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &map), "Tracking map");
        for (unsigned y = 0; y < height; ++y)
            std::memcpy(image.rgba.data() + size_t(y) * width * 4,
                        static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch,
                        width * 4);
        context->Unmap(staging_.Get(), 0);
        return image;
    }

    // Bilinear RGB/ImageNet normalization for both official 224x224 models.
    static void input(const TrackingImage& image, Box box, std::vector<float>& tensor) {
        constexpr unsigned size = 224, plane = size * size;
        constexpr float mean[]{.485f, .456f, .406f}, sd[]{.229f, .224f, .225f};
        tensor.resize(3 * plane);
        for (unsigned y = 0; y < size; ++y) {
            const float sy = std::clamp(box.y + (y + .5f) * box.height / size - .5f, 0.f,
                                        float(image.height - 1));
            const auto y0 = static_cast<unsigned>(sy), y1 = std::min(y0 + 1, image.height - 1);
            const float fy = sy - y0;
            for (unsigned x = 0; x < size; ++x) {
                const float sx = std::clamp(box.x + (x + .5f) * box.width / size - .5f, 0.f,
                                            float(image.width - 1));
                const auto x0 = static_cast<unsigned>(sx), x1 = std::min(x0 + 1, image.width - 1);
                const float fx = sx - x0;
                for (unsigned c = 0; c < 3; ++c) {
                    const auto at = [&](unsigned px, unsigned py) {
                        return float(image.rgba[(size_t(py) * image.width + px) * 4 + c]);
                    };
                    const float upper = std::lerp(at(x0, y0), at(x1, y0), fx);
                    const float lower = std::lerp(at(x0, y1), at(x1, y1), fx);
                    tensor[c * plane + y * size + x] =
                        (std::lerp(upper, lower, fy) / 255 - mean[c]) / sd[c];
                }
            }
        }
    }
    static std::vector<Ort::Value> run(Ort::Session& session, const char* name,
                                       std::vector<float>& data) {
        const std::array<int64_t, 4> shape{1, 3, 224, 224};
        const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        auto tensor = Ort::Value::CreateTensor<float>(memory, data.data(), data.size(),
                                                      shape.data(), shape.size());
        Ort::AllocatorWithDefaultOptions allocator;
        std::vector<Ort::AllocatedStringPtr> names;
        std::vector<const char*> pointers;
        for (size_t i = 0; i < session.GetOutputCount(); ++i) {
            names.push_back(session.GetOutputNameAllocated(i, allocator));
            pointers.push_back(names.back().get());
        }
        return session.Run(Ort::RunOptions{nullptr}, &name, &tensor, 1, pointers.data(),
                           pointers.size());
    }
    static Box clampBox(Box b, const TrackingImage& image) {
        const float right = std::clamp(b.x + b.width, 0.f, float(image.width));
        const float bottom = std::clamp(b.y + b.height, 0.f, float(image.height));
        b.x = std::clamp(b.x, 0.f, float(image.width - 1));
        b.y = std::clamp(b.y, 0.f, float(image.height - 1));
        b.width = std::max(0.f, right - b.x);
        b.height = std::max(0.f, bottom - b.y);
        return b;
    }
    void work(const std::filesystem::path& directory) noexcept {
        try {
            Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "cortex-face");
            Ort::SessionOptions options;
            options.SetIntraOpNumThreads(2);
            options.SetInterOpNumThreads(1);
            options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
            options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            const auto detectorPath = directory / "mnv3_detection_opt.onnx";
            const auto landmarksPath = directory / "lm_model1_opt.onnx";
            if (!std::filesystem::is_regular_file(detectorPath) ||
                !std::filesystem::is_regular_file(landmarksPath))
                throw std::runtime_error(
                    "Face models missing: models/face-tracking (rebuild the package)");
            Ort::Session detector(env, detectorPath.c_str(), options),
                landmarks(env, landmarksPath.c_str(), options);
            Ort::AllocatorWithDefaultOptions allocator;
            const auto detectName = detector.GetInputNameAllocated(0, allocator);
            const auto landmarkName = landmarks.GetInputNameAllocated(0, allocator);
            Box face{};
            unsigned count = 0, previousWidth = 0, previousHeight = 0;
            std::vector<float> tensor;
            std::unique_ptr<BodyTracker> bodyTracker;
            Clock::time_point nextBody{}, bodyRetry{};
            {
                std::lock_guard lock(mutex_);
                status_ = "Looking for your face";
            }
            for (;;) {
                TrackingImage image;
                {
                    std::unique_lock lock(mutex_);
                    wake_.wait(lock, [&] { return closing_ || pending_.has_value(); });
                    if (closing_)
                        return;
                    image = std::move(*pending_);
                    pending_.reset();
                }
                if (image.width != previousWidth || image.height != previousHeight) {
                    face = {};
                    previousWidth = image.width;
                    previousHeight = image.height;
                }
                if (face.width < 4 || ++count % 45 == 0) {
                    input(image, {0, 0, float(image.width), float(image.height)}, tensor);
                    auto outputs = run(detector, detectName.get(), tensor);
                    const auto shape = outputs.at(0).GetTensorTypeAndShapeInfo().GetShape();
                    if (shape != std::vector<int64_t>{1, 2, 56, 56})
                        throw std::runtime_error("Unexpected OpenSeeFace detector output");
                    const float* data = outputs[0].GetTensorData<float>();
                    const auto best = std::max_element(data, data + 56 * 56);
                    face = {};
                    if (std::isfinite(*best) && *best >= .60f) {
                        const auto index = best - data;
                        const float radius = data[56 * 56 + index] * 112;
                        const float cx = float(index % 56) * 4, cy = float(index / 56) * 4;
                        if (std::isfinite(radius) && radius > 2)
                            face = clampBox({(cx - radius) * image.width / 224,
                                             (cy - radius) * image.height / 224,
                                             2 * radius * image.width / 224,
                                             2 * radius * image.height / 224},
                                            image);
                    }
                }
                std::optional<FaceLandmarks> found;
                if (face.width >= 4 && face.height >= 4) {
                    const auto crop =
                        clampBox({face.x - face.width * .10f, face.y - face.height * .125f,
                                  face.width * 1.20f, face.height * 1.25f},
                                 image);
                    input(image, crop, tensor);
                    auto outputs = run(landmarks, landmarkName.get(), tensor);
                    if (outputs.at(0).GetTensorTypeAndShapeInfo().GetShape() !=
                        std::vector<int64_t>{1, 198, 28, 28})
                        throw std::runtime_error("Unexpected OpenSeeFace landmark output");
                    const float* data = outputs[0].GetTensorData<float>();
                    std::array<LandmarkPoint, 66> points{};
                    float confidence = 0;
                    bool valid = true;
                    constexpr int cells = 28 * 28;
                    for (int i = 0; i < 66; ++i) {
                        const float* map = data + i * cells;
                        const auto peak = std::max_element(map, map + cells);
                        const auto index = peak - map;
                        const auto logit = [](float p) {
                            p = std::clamp(p, .0000001f, .9999999f);
                            return std::log(p / (1 - p)) / 16;
                        };
                        // OpenSeeFace offset planes are row (Y), then column (X).
                        const float y =
                            float(index / 28) / 27 + logit(data[(66 + i) * cells + index]);
                        const float x =
                            float(index % 28) / 27 + logit(data[(132 + i) * cells + index]);
                        points[i] = {crop.x + crop.width * (223.f / 224) * x,
                                     crop.y + crop.height * (223.f / 224) * y};
                        confidence += *peak;
                        valid =
                            valid && std::isfinite(x) && std::isfinite(y) && std::isfinite(*peak);
                    }
                    if (valid && confidence / 66 >= .60f) {
                        const auto avg = [&](int a, int b) {
                            return LandmarkPoint{(points[a].x + points[b].x) * .5f,
                                                 (points[a].y + points[b].y) * .5f};
                        };
                        const auto distance = [&](int a, int b) {
                            return std::hypot(points[a].x - points[b].x, points[a].y - points[b].y);
                        };
                        const auto eye = [&](int a) {
                            const float ratio = (distance(a + 1, a + 5) + distance(a + 2, a + 4)) /
                                                std::max(1.f, 2 * distance(a, a + 3));
                            return std::clamp((ratio - .10f) / .16f, 0.f, 1.f);
                        };
                        FaceLandmarks f;
                        f.leftEye = avg(36, 39);
                        f.rightEye = avg(42, 45);
                        f.nose = points[30];
                        f.upperLip = points[60];
                        f.lowerLip = points[64];
                        f.leftEyeOpen = eye(36);
                        f.rightEyeOpen = eye(42);
                        f.eyeOpen = (f.leftEyeOpen + f.rightEyeOpen) * .5f;
                        f.headRotation = estimateHeadRotation(points, image.width, image.height);
                        f.needsHeadFit = true;
                        // Gaze stays neutral: this model measures eyelids, not pupils.
                        const float span =
                            std::hypot(f.rightEye.x - f.leftEye.x, f.rightEye.y - f.leftEye.y);
                        if (span > 4)
                            found = f;
                        float x1 = float(image.width), y1 = float(image.height), x2 = 0, y2 = 0;
                        for (const auto& p : points) {
                            x1 = std::min(x1, p.x);
                            x2 = std::max(x2, p.x);
                            y1 = std::min(y1, p.y);
                            y2 = std::max(y2, p.y);
                        }
                        face = clampBox({x1, y1, x2 - x1, y2 - y1}, image);
                    } else
                        face = {};
                }
                if (bodyEnabled_ && Clock::now() >= nextBody) {
                    nextBody = Clock::now() + std::chrono::milliseconds(80);
                    try {
                        if (!bodyTracker && Clock::now() >= bodyRetry)
                            bodyTracker = std::make_unique<BodyTracker>(directory.parent_path() /
                                                                        "upper-body");
                        if (bodyTracker) {
                            const auto body =
                                bodyTracker->track(image.rgba, image.width, image.height);
                            std::lock_guard lock(mutex_);
                            body_ = body;
                            bodyTime_ = image.captured;
                            const int arms = (body.left.confidence >= .6f ? 1 : 0) +
                                             (body.right.confidence >= .6f ? 1 : 0);
                            bodyStatus_ = "Arms " + std::to_string(arms) +
                                          (arms == 2 ? "/2 visible" : "/2 - show elbows/wrists");
                        }
                    } catch (const std::exception&) {
                        std::lock_guard lock(mutex_);
                        body_ = {};
                        bodyStatus_ = "Arm tracking unavailable - check upper-body models";
                        bodyRetry = Clock::now() + std::chrono::seconds(5);
                    }
                }
                {
                    std::lock_guard lock(mutex_);
                    result_ = {found, image.captured};
                    status_ =
                        found ? (found->headRotation ? "3D head, blink and mouth tracked"
                                                     : "Face found - move closer for head rotation")
                              : "Face not found - look toward the camera";
                }
            }
        } catch (const std::exception& error) {
            std::lock_guard lock(mutex_);
            result_ = {};
            pending_.reset();
            status_ = std::string("Face tracking unavailable: ") + error.what();
            failed_ = true;
        }
    }

public:
    OnnxFaceTracker(std::shared_ptr<GpuDevice> gpu, const std::filesystem::path& models)
        : gpu_(std::move(gpu))
        , converter_(gpu_) {
        const auto directory = models.empty() ? defaultModels() : models;
        worker_ = std::thread([this, directory] { work(directory); });
    }
    ~OnnxFaceTracker() override {
        {
            std::lock_guard lock(mutex_);
            closing_ = true;
        }
        wake_.notify_one();
        if (worker_.joinable())
            worker_.join();
    }
    std::optional<FaceLandmarks> track(const VideoFrame& frame) override {
        const auto now = Clock::now();
        if (!failed_ && frame.texture && frame.gpu == gpu_ && frame.width && frame.height &&
            now - lastSubmit_ >= std::chrono::milliseconds(33) &&
            (lastTexture_ != frame.texture.Get() || lastTimestamp_ != frame.sourceTimestamp90k)) {
            lastSubmit_ = now;
            lastTexture_ = frame.texture.Get();
            lastTimestamp_ = frame.sourceTimestamp90k;
            try {
                auto image = capture(frame);
                {
                    std::lock_guard lock(mutex_);
                    pending_ = std::move(image);
                }
                wake_
                    .notify_one();  // One pending image: inference never builds an unbounded queue.
            } catch (const std::exception& error) {
                std::lock_guard lock(mutex_);
                result_ = {};
                status_ = std::string("Face tracking input error: ") + error.what();
            }
        }
        std::lock_guard lock(mutex_);
        return now - result_.captured <= std::chrono::milliseconds(500) ? result_.landmarks
                                                                        : std::nullopt;
    }
    void setBodyTracking(bool enabled) override { bodyEnabled_ = enabled; }
    std::optional<UpperBodyPose> upperBody() const override {
        std::lock_guard lock(mutex_);
        if (!bodyEnabled_ || Clock::now() - bodyTime_ > std::chrono::milliseconds(350))
            return {};
        return body_;
    }
    std::string status() const override {
        std::lock_guard lock(mutex_);
        return status_ + (bodyEnabled_
                              ? " | " + (bodyStatus_.empty() ? "Looking for arms" : bodyStatus_)
                              : "");
    }
};
#endif
}  // namespace
std::unique_ptr<FaceTracker> makeFaceTracker(std::shared_ptr<GpuDevice> gpu,
                                             const std::filesystem::path& modelDirectory) {
#ifdef CORTEX_HAS_DIRECTML
    if (!gpu)
        throw std::invalid_argument("Face tracking requires a GPU device");
    return std::make_unique<OnnxFaceTracker>(std::move(gpu), modelDirectory);
#else
    (void)gpu;
    (void)modelDirectory;
    return std::make_unique<UnavailableTracker>();
#endif
}
}  // namespace cortex
