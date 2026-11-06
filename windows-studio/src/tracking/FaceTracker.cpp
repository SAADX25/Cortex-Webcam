// Copyright (c) 2019 Emiliana. OpenSeeFace decoding adapted under BSD-2-Clause.
// See third_party/OpenSeeFace/LICENSE.
// Model provenance and license are shipped in models/face-tracking.
#include "cortex/avatar/AvatarFilter.h"
#include "gpu/GpuHelpers.h"
#include "cortex/tracking/HeadPose.h"
#include "cortex/tracking/BodyTracker.h"
#include "cortex/transport/MotionPublisher.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <limits>
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
    uint64_t generation{}, bodyGeneration{};
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
    std::condition_variable wake_, bodyWake_;
    std::optional<TrackingImage> pending_, pendingBody_;
    Result result_;
    std::string status_{"Loading face tracking models"};
    bool closing_{};
    uint64_t inputGeneration_{}, bodyGeneration_{};
    std::atomic<bool> failed_{};
    std::atomic<bool> bodyEnabled_{};
    UpperBodyPose body_;
    Clock::time_point bodyTime_{};
    std::string bodyStatus_;
    std::unique_ptr<MotionPublisher> motionPublisher_;
    HumanoidMotionSolver motionSolver_; // Accessed only while mutex_ is held.
    Clock::time_point lastMotionPublish_{};
    Clock::time_point lastSubmit_{};
    Clock::time_point nextBodySubmit_{};
    int64_t lastTimestamp_{INT64_MIN};
    ID3D11Texture2D* lastTexture_{};
    unsigned lastWidth_{}, lastHeight_{};
    int lastRotation_{-1};
    std::thread worker_, bodyWorker_;

    TrackingImage capture(const VideoFrame& frame) {
        const auto rgb =
            converter_.convert(frame);  // Handles NV12 array slices and phone rotation.
        const float ratio = std::min(1.f, 768.f / std::max(rgb.width, rgb.height));
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
                            frame.readyAt};
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
                                       std::vector<float>& data,
                                       const std::array<int64_t, 4>& shape = {1, 3, 224, 224}) {
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
    struct EyeCrop {
        LandmarkPoint corner, axis;
        float span{};
        bool flipped{};
    };
    static std::optional<std::pair<float, float>> gaze(
        Ort::Session& session, const char* name, const TrackingImage& image,
        const std::array<LandmarkPoint, 66>& points, float leftOpen, float rightOpen,
        std::vector<float>& tensor) {
        constexpr unsigned size = 32, plane = size * size;
        constexpr float mean[]{.485f, .456f, .406f}, sd[]{.229f, .224f, .225f};
        std::array<EyeCrop, 2> eyes;
        tensor.resize(2 * 3 * plane);
        for (int eye = 0; eye < 2; ++eye) {
            const auto first = points[eye == 0 ? 36 : 42];
            const auto last = points[eye == 0 ? 39 : 45];
            const float span = std::hypot(last.x - first.x, last.y - first.y);
            if (!std::isfinite(span) || span < 5)
                return {};
            const LandmarkPoint axis{(last.x - first.x) / span, (last.y - first.y) / span};
            eyes[eye] = {first, axis, span, eye == 1};
            for (unsigned y = 0; y < size; ++y)
                for (unsigned x = 0; x < size; ++x) {
                    // Affine equivalent of upstream's eye rotation/crop and
                    // second-eye horizontal flip, without a full-frame copy.
                    float ex = -.2f * span + (x + .5f) * (1.4f * span / size) - .5f;
                    if (eye == 1)
                        ex = 1.2f * span - (x + .5f) * (1.4f * span / size) - .5f;
                    const float ey = -.6f * span + (y + .5f) * (1.2f * span / size) - .5f;
                    const float sx = std::clamp(first.x + axis.x * ex - axis.y * ey,
                                                0.f, float(image.width - 1));
                    const float sy = std::clamp(first.y + axis.y * ex + axis.x * ey,
                                                0.f, float(image.height - 1));
                    const auto x0 = static_cast<unsigned>(sx), y0 = static_cast<unsigned>(sy);
                    const auto x1 = std::min(x0 + 1, image.width - 1),
                               y1 = std::min(y0 + 1, image.height - 1);
                    for (unsigned c = 0; c < 3; ++c) {
                        const auto at = [&](unsigned px, unsigned py) {
                            return float(image.rgba[(size_t(py) * image.width + px) * 4 + c]);
                        };
                        const float value = std::lerp(std::lerp(at(x0, y0), at(x1, y0), sx - x0),
                                                       std::lerp(at(x0, y1), at(x1, y1), sx - x0),
                                                       sy - y0);
                        // The gaze network uses N,C,X,Y, unlike the face model.
                        tensor[eye * 3 * plane + c * plane + x * size + y] =
                            (value / 255 - mean[c]) / sd[c];
                    }
                }
        }
        const auto outputs = run(session, name, tensor, {2, 3, 32, 32});
        if (outputs.empty() || outputs[0].GetTensorTypeAndShapeInfo().GetShape() !=
                                   std::vector<int64_t>{2, 3, 8, 8})
            throw std::runtime_error("Unexpected OpenSeeFace gaze output");
        const float* data = outputs[0].GetTensorData<float>();
        float gazeX = 0, gazeY = 0, total = 0;
        for (int eye = 0; eye < 2; ++eye) {
            const float open = eye == 0 ? leftOpen : rightOpen;
            const float* map = data + eye * 3 * 64;
            const auto peak = std::max_element(map, map + 64);
            const auto index = peak - map;
            if (!std::isfinite(*peak) || *peak < .4f || open < .15f)
                continue;
            const auto offset = [](float value) {
                value = std::clamp(value, .0000001f, .9999999f);
                return 4.f * std::log(value / (1 - value));
            };
            float x = 4.f * float(index / 8) + offset(map[64 + index]);
            const float y = 4.f * float(index % 8) + offset(map[128 + index]);
            if (!std::isfinite(x) || !std::isfinite(y) || x < -2 || x > 34 || y < -2 || y > 34)
                continue;
            const auto& crop = eyes[eye];
            if (crop.flipped)
                x = 32 - x;
            const float ex = -.2f * crop.span + x * 1.4f * crop.span / 32;
            const float ey = -.6f * crop.span + y * 1.2f * crop.span / 32;
            const LandmarkPoint pupil{crop.corner.x + crop.axis.x * ex - crop.axis.y * ey,
                                       crop.corner.y + crop.axis.y * ex + crop.axis.x * ey};
            LandmarkPoint center{};
            const int start = eye == 0 ? 36 : 42;
            for (int lid : {1, 2, 4, 5}) {
                center.x += points[start + lid].x * .25f;
                center.y += points[start + lid].y * .25f;
            }
            const float px = pupil.x - center.x, py = pupil.y - center.y;
            const float weight = *peak * std::clamp(open, 0.f, 1.f);
            gazeX += weight * std::clamp((px * crop.axis.x + py * crop.axis.y) /
                                           crop.span * 4.f,
                                       -1.f, 1.f);
            gazeY += weight * std::clamp((px * crop.axis.y - py * crop.axis.x) /
                                           crop.span * 4.f,
                                       -1.f, 1.f);
            total += weight;
        }
        return total > 0 ? std::optional(std::pair{gazeX / total, gazeY / total}) : std::nullopt;
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
    void applyFaceToLandmarks(HumanoidLandmarks& raw) {
        if (!result_.landmarks || Clock::now() - result_.captured > std::chrono::milliseconds(350))
            return;
        const auto& f = *result_.landmarks;
        raw.hasFace = true;
        // Face expressions may be ready before a valid perspective head fit.
        // Do not publish an invented zero-angle head as a neutral observation.
        raw.headYaw=raw.headPitch=raw.headRoll=std::numeric_limits<float>::quiet_NaN();
        if (f.headRotation) {
            raw.headYaw = std::clamp(f.headRotation->yaw, -45.f, 45.f);
            // Send the physical angle. The VRM receiver establishes a stable
            // neutral pose; a single startup frame is not a reliable baseline.
            raw.headPitch = std::clamp(f.headRotation->pitch, -60.f, 60.f);
            raw.headRoll = std::clamp(f.headRotation->roll, -35.f, 35.f);
        }
        raw.leftEyeOpen = f.leftEyeOpen < 0 ? f.eyeOpen : f.leftEyeOpen;
        raw.faceEyesImage = {Joint3D{f.leftEye.x, f.leftEye.y, 0, 1}, Joint3D{f.rightEye.x, f.rightEye.y, 0, 1}};
        raw.rightEyeOpen = f.rightEyeOpen < 0 ? f.eyeOpen : f.rightEyeOpen;
        raw.mouthOpen = f.mouthOpenY;
        raw.mouthSmile = f.mouthForm;
        raw.brow = (f.browLY + f.browRY) * 0.5f;
        raw.gazeX = f.gazeX;
        raw.gazeY = f.gazeY;
        raw.bodySwayX = f.hasHeadPosition ? f.headPositionX : 0.f;
    }
    void workBody(const std::filesystem::path& directory) noexcept {
        std::unique_ptr<BodyTracker> tracker;
        wchar_t portText[16]{};
        uint16_t motionPort{};
        if (GetEnvironmentVariableW(L"CORTEX_VRM_PORT", portText, 16)) {
            wchar_t* end{}; auto port = std::wcstoul(portText, &end, 10);
            if (end != portText && *end == 0 && port > 0 && port <= 65535) {
                motionPort = static_cast<uint16_t>(port);
                std::lock_guard lock(mutex_);
                if (!motionPublisher_)
                    motionPublisher_ = std::make_unique<MotionPublisher>(motionPort);
            }
        }
        wchar_t diagnosticsText[8]{};
        const bool diagnostics = GetEnvironmentVariableW(L"CORTEX_HAND_DIAGNOSTICS", diagnosticsText, 8) && diagnosticsText[0] == L'1';
        Clock::time_point lastDiagnostics{};
        Clock::time_point retry{};
        ArmPose prevLeft{}, prevRight{};
        Clock::time_point prevLeftTime{}, prevRightTime{};
        uint64_t prevGen = UINT64_MAX, prevBodyGen = UINT64_MAX;
        for (;;) {
            TrackingImage image;
            {
                std::unique_lock lock(mutex_);
                bodyWake_.wait(lock, [&] { return closing_ || pendingBody_.has_value(); });
                if (closing_)
                    return;
                image = std::move(*pendingBody_);
                pendingBody_.reset();
                if (!bodyEnabled_ || image.generation != inputGeneration_ ||
                    image.bodyGeneration != bodyGeneration_)
                    continue;
            }
            if (!bodyEnabled_ || Clock::now() - image.captured > std::chrono::milliseconds(250))
                continue;
            if (image.generation != prevGen || image.bodyGeneration != prevBodyGen) {
                if (tracker) tracker->reset();
                prevLeft = prevRight = {};
                prevLeftTime = prevRightTime = {};
                prevGen = image.generation;
                prevBodyGen = image.bodyGeneration;
            }
            try {
                if (!tracker && Clock::now() >= retry)
                    tracker = std::make_unique<BodyTracker>(directory.parent_path() / "upper-body");
                if (!tracker)
                    continue;
                auto body = tracker->track(image.rgba, image.width, image.height);
                if (body.landmarks3D) {
                    auto raw = std::make_shared<HumanoidLandmarks>(*body.landmarks3D);
                    raw->capturedAt = image.captured;
                    body.landmarks3D = std::move(raw);
                }
                // Hold the last valid arm pose briefly across 1-2 frame dropouts during fast motion
                if (body.left.confidence >= .6f) {
                    prevLeft = body.left;
                    prevLeftTime = image.captured;
                } else if (prevLeft.confidence >= .6f &&
                           image.captured - prevLeftTime <= std::chrono::milliseconds(180)) {
                    body.left = prevLeft;
                }
                if (body.right.confidence >= .6f) {
                    prevRight = body.right;
                    prevRightTime = image.captured;
                } else if (prevRight.confidence >= .6f &&
                           image.captured - prevRightTime <= std::chrono::milliseconds(180)) {
                    body.right = prevRight;
                }
                body.confidence =
                    std::max({body.confidence, body.left.confidence, body.right.confidence});
                body.capturedAt = image.captured;
                std::lock_guard lock(mutex_);
                if (bodyEnabled_ && image.generation == inputGeneration_ &&
                    image.bodyGeneration == bodyGeneration_) {
                    body_ = body;
                    bodyTime_ = image.captured;
                    const int arms = (body.left.confidence >= .6f ? 1 : 0) +
                                     (body.right.confidence >= .6f ? 1 : 0);
                    bodyStatus_ = "Arms " + std::to_string(arms) +
                                  (arms == 2 ? "/2 visible" : "/2 - show elbows/wrists");
                    if (body.landmarks3D) {
                        const auto& raw = *body.landmarks3D;
                        const int hands = (raw.hands[0].confidence >= .8f ? 1 : 0) +
                                          (raw.hands[1].confidence >= .8f ? 1 : 0);
                        bodyStatus_ += raw.handsAvailable ? " | Hands " + std::to_string(hands) + "/2 (21 joints)"
                                                         : " | Hands models not installed";
                        if (motionPort) {
                            if (!motionPublisher_) motionPublisher_ = std::make_unique<MotionPublisher>(motionPort);
                            HumanoidLandmarks merged = raw;
                            applyFaceToLandmarks(merged);
                            lastMotionPublish_ = Clock::now();
                            if (!motionPublisher_->publish(motionSolver_.solve(merged)))
                                bodyStatus_ += " | Motion UDP send failed";
                        }
                        if (diagnostics && (raw.hands[0].confidence>=.8f || raw.hands[1].confidence>=.8f) &&
                            Clock::now() - lastDiagnostics >= std::chrono::milliseconds(100)) {
                            lastDiagnostics = Clock::now();
                            std::ostringstream line;
                            line << "HAND_SAMPLE capture_us=" << std::chrono::duration_cast<std::chrono::microseconds>(image.captured.time_since_epoch()).count();
                            for (int side = 0; side < 2; ++side)
                                if (raw.hands[side].confidence >= .8f)
                                    for (size_t joint = 0; joint < 21; ++joint) {
                                        const auto& p = raw.hands[side].world[joint];
                                        line << ' ' << (side ? 'R' : 'L') << joint << '=' << p.x << ',' << p.y << ',' << p.z;
                                    }
                            std::cout << line.str() << std::endl;
                            OutputDebugStringA((line.str() + "\n").c_str());
                        }
                    }
                }
            } catch (const std::exception&) {
                std::lock_guard lock(mutex_);
                if (image.generation == inputGeneration_ && image.bodyGeneration == bodyGeneration_) {
                    body_ = {};
                    bodyStatus_ = "Arm tracking unavailable - check upper-body models";
                }
                retry = Clock::now() + std::chrono::seconds(5);
            }
        }
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
            const bool qualityModel = std::filesystem::is_regular_file(directory / "lm_model3_opt.onnx");
            const auto landmarksPath = directory / (qualityModel ? "lm_model3_opt.onnx" : "lm_model1_opt.onnx");
            if (!std::filesystem::is_regular_file(detectorPath) ||
                !std::filesystem::is_regular_file(landmarksPath))
                throw std::runtime_error(
                    "Face models missing: models/face-tracking (rebuild the package)");
            Ort::Session detector(env, detectorPath.c_str(), options),
                landmarks(env, landmarksPath.c_str(), options);
            std::unique_ptr<Ort::Session> gazeModel;
            if (std::filesystem::is_regular_file(directory / "mnv3_gaze32_split_opt.onnx"))
                try {
                    gazeModel = std::make_unique<Ort::Session>(
                        env, (directory / "mnv3_gaze32_split_opt.onnx").c_str(), options);
                } catch (const Ort::Exception&) {
                    // Face tracking remains usable when the optional eye model cannot load.
                }
            Ort::AllocatorWithDefaultOptions allocator;
            const auto detectName = detector.GetInputNameAllocated(0, allocator);
            const auto landmarkName = landmarks.GetInputNameAllocated(0, allocator);
            const std::string gazeName = gazeModel
                                             ? gazeModel->GetInputNameAllocated(0, allocator).get()
                                             : "";
            Box face{};
            unsigned previousWidth = 0, previousHeight = 0;
            uint64_t previousGeneration = UINT64_MAX;
            std::vector<float> tensor, gazeTensor;
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
                    if (image.generation != inputGeneration_)
                        continue;
                }
                if (Clock::now() - image.captured > std::chrono::milliseconds(250))
                    continue;
                if (image.generation != previousGeneration || image.width != previousWidth ||
                    image.height != previousHeight) {
                    face = {};
                    previousGeneration = image.generation;
                    previousWidth = image.width;
                    previousHeight = image.height;
                }
                if (face.width < 4) {
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
                    std::array<float, 66> pointConfidence{};
                    float confidence = 0;
                    bool valid = true;
                    constexpr int cells = 28 * 28;
                    for (int i = 0; i < 66; ++i) {
                        const float* map = data + i * cells;
                        const auto peak = std::max_element(map, map + cells);
                        const int peakIdx = static_cast<int>(peak - map);
                        const int pr = peakIdx / 28, pc = peakIdx % 28;
                        const auto logit = [](float p) {
                            p = std::clamp(p, .0000001f, .9999999f);
                            return std::log(p / (1 - p)) / 16;
                        };
                        // Soft-argmax across the 3x3 neighborhood when adjacent heatmap cells
                        // are close to the peak so a landmark does not hop discretely between cells.
                        float sumW = 0.f, sumX = 0.f, sumY = 0.f;
                        const float minNeighbor = std::max(0.1f, *peak * 0.75f);
                        for (int dr = -1; dr <= 1; ++dr) {
                            const int r = pr + dr;
                            if (r < 0 || r >= 28)
                                continue;
                            for (int dc = -1; dc <= 1; ++dc) {
                                const int c = pc + dc;
                                if (c < 0 || c >= 28)
                                    continue;
                                const int idx = r * 28 + c;
                                const float w = map[idx];
                                if (std::isfinite(w) && w >= minNeighbor) {
                                    const float weight = w * w * w * w;
                                    const float cellY =
                                        float(r) / 27 + logit(data[(66 + i) * cells + idx]);
                                    const float cellX =
                                        float(c) / 27 + logit(data[(132 + i) * cells + idx]);
                                    if (std::isfinite(cellX) && std::isfinite(cellY)) {
                                        sumW += weight;
                                        sumX += weight * cellX;
                                        sumY += weight * cellY;
                                    }
                                }
                            }
                        }
                        // OpenSeeFace offset planes are row (Y), then column (X).
                        const float y = sumW > 0.f
                                            ? sumY / sumW
                                            : float(pr) / 27 + logit(data[(66 + i) * cells + peakIdx]);
                        const float x = sumW > 0.f
                                            ? sumX / sumW
                                            : float(pc) / 27 + logit(data[(132 + i) * cells + peakIdx]);
                        points[i] = {crop.x + crop.width * (223.f / 224) * x,
                                     crop.y + crop.height * (223.f / 224) * y};
                        pointConfidence[i] = *peak;
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
                            const float h1 = distance(a + 1, a + 5);
                            const float h2 = distance(a + 2, a + 4);
                            const float w = distance(a, a + 3);
                            const float ratio = (h1 + h2) / std::max(1.f, 2.0f * w);
                            // OpenSeeFace EAR calibration with baseline offset and deadzones
                            constexpr float closedThreshold = 0.13f;
                            constexpr float openThreshold = 0.27f;
                            constexpr float deadzone = 0.04f;
                            float val = (ratio - closedThreshold) / (openThreshold - closedThreshold);
                            if (val <= deadzone)
                                return 0.f;
                            if (val >= 1.f - deadzone)
                                return 1.f;
                            return std::clamp((val - deadzone) / (1.f - 2.f * deadzone), 0.f, 1.f);
                        };

                        // Eyebrow vertical movement: distance between eyebrow and eye relative to eye span
                        const auto browCalc = [&]() -> std::pair<float, float> {
                            const float eye_y_l = (points[36].y + points[39].y) * 0.5f;
                            const float brow_y_l = (points[18].y + points[19].y + points[20].y) / 3.0f;
                            const float span_l = std::max(1.f, distance(36, 39));
                            const float ratio_l = (eye_y_l - brow_y_l) / span_l;

                            const float eye_y_r = (points[42].y + points[45].y) * 0.5f;
                            const float brow_y_r = (points[23].y + points[24].y + points[25].y) / 3.0f;
                            const float span_r = std::max(1.f, distance(42, 45));
                            const float ratio_r = (eye_y_r - brow_y_r) / span_r;

                            constexpr float neutralRatio = 0.90f;
                            constexpr float deadzone = 0.03f;
                            constexpr float gain = 3.5f;

                            const float diff_l = ratio_l - neutralRatio;
                            const float diff_r = ratio_r - neutralRatio;

                            float browL = 0.f;
                            if (std::abs(diff_l) > deadzone)
                                browL = std::clamp((diff_l - std::copysign(deadzone, diff_l)) * gain, -1.f, 1.f);
                            float browR = 0.f;
                            if (std::abs(diff_r) > deadzone)
                                browR = std::clamp((diff_r - std::copysign(deadzone, diff_r)) * gain, -1.f, 1.f);
                            return {browL, browR};
                        };

                        FaceLandmarks f;
                        f.imageWidth = image.width;
                        f.imageHeight = image.height;
                        f.capturedAt = image.captured;
                        f.leftEye = avg(36, 39);
                        f.rightEye = avg(42, 45);
                        f.nose = points[30];
                        f.upperLip = points[60];
                        f.lowerLip = points[64];
                        f.leftEyeOpen = eye(36);
                        f.rightEyeOpen = eye(42);
                        // Wink support: landmark models tend to couple both eyelids, so when
                        // one eye is clearly open and the other clearly more closed, separate
                        // them further so a one-eye close shows on the avatar.
                        {
                            float& openEye = f.leftEyeOpen >= f.rightEyeOpen ? f.leftEyeOpen
                                                                             : f.rightEyeOpen;
                            float& closedEye = f.leftEyeOpen >= f.rightEyeOpen ? f.rightEyeOpen
                                                                               : f.leftEyeOpen;
                            if (openEye > 0.55f && openEye - closedEye > 0.30f) {
                                openEye = std::min(1.f, openEye + (1.f - openEye) * 0.6f);
                                closedEye *= 0.4f;
                            }
                        }
                        f.eyeOpen = (f.leftEyeOpen + f.rightEyeOpen) * .5f;
                        f.gazeX = f.gazeY = 0;
                        if (gazeModel)
                            try {
                                if (const auto measured = gaze(*gazeModel, gazeName.c_str(), image,
                                                               points, f.leftEyeOpen,
                                                               f.rightEyeOpen, gazeTensor)) {
                                    f.gazeX = measured->first;
                                    f.gazeY = measured->second;
                                }
                            } catch (const std::exception&) {
                                gazeModel.reset();
                            }
                        const auto [bL, bR] = browCalc();
                        f.browLY = bL;
                        f.browRY = bR;
                        const float span =
                            std::hypot(f.rightEye.x - f.leftEye.x, f.rightEye.y - f.leftEye.y);

                        // Mouth form (smile/frown) and open ratio with baseline offset and deadzone
                        const float innerDist = distance(60, 64);
                        const float mouthRatio = innerDist / std::max(1.f, span);
                        constexpr float mouthClosedBaseline = 0.032f;
                        constexpr float mouthMaxAperture = 0.25f;
                        constexpr float mouthDeadzone = 0.015f;
                        float openY = (mouthRatio - mouthClosedBaseline) / (mouthMaxAperture - mouthClosedBaseline);
                        if (openY <= mouthDeadzone)
                            openY = 0.f;
                        else
                            openY = std::clamp((openY - mouthDeadzone) / (1.f - mouthDeadzone), 0.f, 1.f);
                        f.mouthOpenY = openY;

                        // OpenSeeFace's 66-point mouth uses corners 58/62 and
                        // outer center points 50/55, unlike the 68-point layout.
                        const float cornerY = (points[58].y + points[62].y) * 0.5f;
                        const float lipMidY = (points[50].y + points[55].y) * 0.5f;
                        const float cornerElev = (lipMidY - cornerY) / std::max(1.f, span);
                        const float mouthWidth = distance(58, 62) / std::max(1.f, span);
                        constexpr float neutralWidth = 0.81f;
                        constexpr float neutralCornerElevation = 0.07f;
                        const float formRaw = (cornerElev - neutralCornerElevation) * 4.5f +
                                              (mouthWidth - neutralWidth) * 2.0f;
                        constexpr float formDeadzone = 0.04f;
                        float form = 0.f;
                        if (std::abs(formRaw) > formDeadzone)
                            form = std::clamp((formRaw - std::copysign(formDeadzone, formRaw)) * 2.5f, -1.f, 1.f);
                        f.mouthForm = form;

                        if (const auto head = estimateHeadPose(points, image.width, image.height,
                                                               1.f, &pointConfidence)) {
                            f.headRotation = head->rotation;
                            f.headDepth = head->depth;
                            f.hasHeadPosition = true;
                            f.headPositionX = head->positionX;
                            f.headPositionY = head->positionY;
                        }
                        f.needsHeadFit = true;
                        if (span > 4)
                            found = f;
                        float x1 = float(image.width), y1 = float(image.height), x2 = 0, y2 = 0;
                        for (const auto& p : points) {
                            x1 = std::min(x1, p.x);
                            x2 = std::max(x2, p.x);
                            y1 = std::min(y1, p.y);
                            y2 = std::max(y2, p.y);
                        }
                        const auto nextFace = clampBox({x1, y1, x2 - x1, y2 - y1}, image);
                        if (face.width >= 4 && face.height >= 4) {
                            face = clampBox({face.x * .60f + nextFace.x * .40f,
                                             face.y * .60f + nextFace.y * .40f,
                                             face.width * .60f + nextFace.width * .40f,
                                             face.height * .60f + nextFace.height * .40f},
                                            image);
                        } else {
                            face = nextFace;
                        }
                    } else
                        face = {};
                }
                {
                    std::lock_guard lock(mutex_);
                    if (image.generation == inputGeneration_) {
                        result_ = {found, image.captured};
                        status_ =
                            found ? (found->headRotation ? "Head, blink and mouth tracked"
                                                         : "Face found - move closer for head rotation")
                                  : "Face not found - look toward the camera";
                        if (found)
                            status_ += std::string(qualityModel ? " | Quality tracking" : " | Fast tracking") +
                                       (gazeModel ? " | Pupil gaze" : " | Gaze unavailable");
                        if (found && bodyEnabled_ && motionPublisher_ &&
                            Clock::now() - lastMotionPublish_ >= std::chrono::milliseconds(22)) {
                            HumanoidLandmarks merged =
                                (body_.landmarks3D && Clock::now() - bodyTime_ <= std::chrono::milliseconds(350))
                                    ? *body_.landmarks3D
                                    : HumanoidLandmarks{};
                            merged.capturedAt = image.captured;
                            applyFaceToLandmarks(merged);
                            lastMotionPublish_ = Clock::now();
                            motionPublisher_->publish(motionSolver_.solve(merged));
                        }
                    }
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
        bodyWorker_ = std::thread([this, directory] { workBody(directory); });
    }
    ~OnnxFaceTracker() override {
        {
            std::lock_guard lock(mutex_);
            closing_ = true;
        }
        wake_.notify_one();
        bodyWake_.notify_one();
        if (worker_.joinable())
            worker_.join();
        if (bodyWorker_.joinable())
            bodyWorker_.join();
    }
    std::optional<FaceLandmarks> track(const VideoFrame& frame) override {
        const auto now = Clock::now();
        if (frame.width != lastWidth_ || frame.height != lastHeight_ ||
            frame.rotationDegrees != lastRotation_ ||
            frame.sourceTimestamp90k < lastTimestamp_) {
            std::lock_guard lock(mutex_);
            ++inputGeneration_;
            motionSolver_.reset();
            result_ = {};
            pending_.reset();
            pendingBody_.reset();
            body_ = {};
            bodyTime_ = {};
            bodyStatus_.clear();
            lastWidth_ = frame.width;
            lastHeight_ = frame.height;
            lastRotation_ = frame.rotationDegrees;
            lastTexture_ = nullptr;
            lastTimestamp_ = INT64_MIN;
            lastSubmit_ = {};
            nextBodySubmit_ = {};
        }
        if (!failed_ && frame.texture && frame.gpu == gpu_ && frame.width && frame.height &&
            now - frame.readyAt <= std::chrono::milliseconds(250) &&
            now - lastSubmit_ >= std::chrono::milliseconds(15) &&
            (lastTexture_ != frame.texture.Get() || lastTimestamp_ != frame.sourceTimestamp90k)) {
            lastSubmit_ = now;
            lastTexture_ = frame.texture.Get();
            lastTimestamp_ = frame.sourceTimestamp90k;
            try {
                auto image = capture(frame);
                {
                    std::lock_guard lock(mutex_);
                    image.generation = inputGeneration_;
                    image.bodyGeneration = bodyGeneration_;
                    pending_ = std::move(image);
                    if (bodyEnabled_ && now >= nextBodySubmit_) {
                        pendingBody_ = *pending_;
                        nextBodySubmit_ = now + std::chrono::milliseconds(25);
                    }
                }
                wake_
                    .notify_one();  // One pending image: inference never builds an unbounded queue.
                bodyWake_.notify_one();
            } catch (const std::exception& error) {
                std::lock_guard lock(mutex_);
                result_ = {};
                status_ = std::string("Face tracking input error: ") + error.what();
            }
        }
        std::lock_guard lock(mutex_);
        return Clock::now() - result_.captured <= std::chrono::milliseconds(250) ? result_.landmarks
                                                                          : std::nullopt;
    }
    void setBodyTracking(bool enabled) override {
        if (bodyEnabled_.exchange(enabled) == enabled)
            return;
        std::lock_guard lock(mutex_);
        ++bodyGeneration_;
        motionSolver_.reset();
        pendingBody_.reset();
        body_ = {};
        bodyTime_ = {};
        bodyStatus_.clear();
        nextBodySubmit_ = {};
    }
    std::optional<UpperBodyPose> upperBody() const override {
        std::lock_guard lock(mutex_);
        if (!bodyEnabled_ || Clock::now() - bodyTime_ > std::chrono::milliseconds(500))
            return {};
        return body_;
    }
    std::string status() const override {
        std::lock_guard lock(mutex_);
        if (result_.landmarks && Clock::now() - result_.captured > std::chrono::milliseconds(250))
            return "Waiting for a fresh camera frame";
        return bodyEnabled_ && !bodyStatus_.empty() ? status_ + " | " + bodyStatus_ : status_;
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
