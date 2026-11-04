// MediaPipe/OpenCV Zoo preprocessing and decoding: Apache-2.0.
// See third_party/MediaPipePose/LICENSE and README.md for pinned model provenance.
#include "cortex/filters/BodyTracker.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>
#ifdef CORTEX_HAS_DIRECTML
#include <onnxruntime_cxx_api.h>
#endif
namespace cortex {
ArmPose mapArm(LandmarkPoint shoulder, LandmarkPoint elbow, LandmarkPoint wrist, float confidence) {
    const float ux = elbow.x - shoulder.x, uy = elbow.y - shoulder.y;
    const float vx = wrist.x - elbow.x, vy = wrist.y - elbow.y;
    const float a = std::hypot(ux, uy), b = std::hypot(vx, vy);
    if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(confidence) || a < 3 || b < 3 ||
        confidence < .6f)
        return {};
    const float lift = std::acos(std::clamp(uy / a, -1.f, 1.f)) / std::numbers::pi_v<float>;
    const float bend =
        std::acos(std::clamp((ux * vx + uy * vy) / (a * b), -1.f, 1.f)) / std::numbers::pi_v<float>;
    return {lift, bend, std::clamp(confidence, 0.f, 1.f)};
}
BodyAngles mapBodyAngles(LandmarkPoint leftShoulder, LandmarkPoint rightShoulder,
                         float leftShoulderZ, float rightShoulderZ,
                         float leftHipZ, float rightHipZ,
                         bool hasHips) {
    const float sdx = leftShoulder.x - rightShoulder.x;
    const float sdy = leftShoulder.y - rightShoulder.y;
    const float sDist = std::hypot(sdx, sdy);
    float bodyAngleZ = 0.f;
    if (sDist > 5.f && std::isfinite(sdx) && std::isfinite(sdy)) {
        const float tiltRad = std::atan2(sdy, sdx);
        const float tiltDeg = tiltRad * (180.f / std::numbers::pi_v<float>);
        constexpr float zDeadzone = 0.5f;
        if (std::abs(tiltDeg) > zDeadzone) {
            bodyAngleZ = std::clamp((tiltDeg - std::copysign(zDeadzone, tiltDeg)) * 1.2f, -10.f, 10.f);
        }
    }
    const float sdz = leftShoulderZ - rightShoulderZ;
    float bodyAngleX = 0.f;
    if (std::isfinite(sdz)) {
        constexpr float xDeadzone = 0.015f;
        if (std::abs(sdz) > xDeadzone) {
            bodyAngleX = std::clamp((sdz - std::copysign(xDeadzone, sdz)) * 25.f, -10.f, 10.f);
        }
    }
    float bodyAngleY = 0.f;
    if (hasHips && std::isfinite(leftHipZ) && std::isfinite(rightHipZ)) {
        const float shoulderZ = (leftShoulderZ + rightShoulderZ) * 0.5f;
        const float hipZ = (leftHipZ + rightHipZ) * 0.5f;
        const float pitchDiff = shoulderZ - hipZ;
        constexpr float yDeadzone = 0.02f;
        if (std::abs(pitchDiff) > yDeadzone) {
            bodyAngleY = std::clamp((pitchDiff - std::copysign(yDeadzone, pitchDiff)) * 20.f, -10.f, 10.f);
        }
    }
    return {bodyAngleX, bodyAngleY, bodyAngleZ};
}
#ifdef CORTEX_HAS_DIRECTML
namespace {
float sigmoid(float x) {
    return 1 / (1 + std::exp(-std::clamp(x, -80.f, 80.f)));
}
struct Vec {
    float x{}, y{};
};
std::vector<Vec> anchors() {
    std::vector<Vec> a;
    for (const auto [grid, repeats] : {std::pair{28, 2}, std::pair{14, 2}, std::pair{7, 6}})
        for (int y = 0; y < grid; ++y)
            for (int x = 0; x < grid; ++x)
                for (int n = 0; n < repeats; ++n)
                    a.push_back({(x + .5f) / grid, (y + .5f) / grid});
    return a;
}
std::array<float, 3> sample(std::span<const uint8_t> image, unsigned w, unsigned h, float x,
                            float y) {
    if (x < 0 || y < 0 || x > float(w - 1) || y > float(h - 1))
        return {};
    const auto x0 = static_cast<unsigned>(x), y0 = static_cast<unsigned>(y);
    const auto x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    std::array<float, 3> p{};
    for (size_t c = 0; c < 3; ++c) {
        const auto at = [&](unsigned px, unsigned py) {
            return float(image[(size_t(py) * w + px) * 4 + c]) / 255;
        };
        p[c] = std::lerp(std::lerp(at(x0, y0), at(x1, y0), x - x0),
                         std::lerp(at(x0, y1), at(x1, y1), x - x0), y - y0);
    }
    return p;
}
std::vector<Ort::Value> run(Ort::Session& s, const std::vector<float>& input,
                            std::span<const int64_t> shape) {
    auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    auto tensor = Ort::Value::CreateTensor<float>(memory, const_cast<float*>(input.data()),
                                                  input.size(), shape.data(), shape.size());
    Ort::AllocatorWithDefaultOptions allocator;
    auto name = s.GetInputNameAllocated(0, allocator);
    const char* inputName = name.get();
    std::vector<Ort::AllocatedStringPtr> names;
    std::vector<const char*> outputs;
    for (size_t i = 0; i < s.GetOutputCount(); ++i) {
        names.push_back(s.GetOutputNameAllocated(i, allocator));
        outputs.push_back(names.back().get());
    }
    return s.Run(Ort::RunOptions{nullptr}, &inputName, &tensor, 1, outputs.data(), outputs.size());
}
}  // namespace
struct BodyTracker::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "cortex-arms"};
    Ort::Session detector{nullptr}, pose{nullptr};
    std::vector<Vec> anchor = anchors();
    std::vector<float> input;
};
BodyTracker::BodyTracker(const std::filesystem::path& directory)
    : impl_(std::make_unique<Impl>()) {
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(2);
    options.SetInterOpNumThreads(1);
    options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    impl_->detector = Ort::Session(
        impl_->env, (directory / "person_detection_mediapipe_2023mar.onnx").c_str(), options);
    impl_->pose = Ort::Session(
        impl_->env, (directory / "pose_estimation_mediapipe_2023mar.onnx").c_str(), options);
}
BodyTracker::~BodyTracker() = default;
UpperBodyPose BodyTracker::track(std::span<const uint8_t> image, unsigned w, unsigned h) {
    if (!w || !h || image.size() != size_t(w) * h * 4)
        throw std::invalid_argument("Invalid body tracking image");
    auto& s = *impl_;
    constexpr int d = 224, p = 256;
    const float scale = static_cast<float>(std::max(w, h));
    const float ratio = d / scale;
    const int rw = static_cast<int>(w * ratio), rh = static_cast<int>(h * ratio),
              left = (d - rw) / 2, top = (d - rh) / 2;
    s.input.assign(3 * d * d, 0);
    for (int y = 0; y < rh; ++y)
        for (int x = 0; x < rw; ++x) {
            const auto rgb = sample(image, w, h, (x + .5f) / ratio - .5f, (y + .5f) / ratio - .5f);
            for (int c = 0; c < 3; ++c)
                s.input[c * d * d + (y + top) * d + x + left] = rgb[c] * 2 - 1;
        }
    const std::array<int64_t, 4> detectShape{1, 3, d, d}, poseShape{1, p, p, 3};
    auto detections = run(s.detector, s.input, detectShape);
    const float* score = nullptr;
    const float* box = nullptr;
    for (auto& out : detections) {
        const auto shape = out.GetTensorTypeAndShapeInfo().GetShape();
        if (shape == std::vector<int64_t>{1, 2254, 1})
            score = out.GetTensorData<float>();
        if (shape == std::vector<int64_t>{1, 2254, 12})
            box = out.GetTensorData<float>();
    }
    if (!score || !box)
        throw std::runtime_error("Unexpected MediaPipe person output");
    int best = -1;
    float confidence = .6f;
    for (int i = 0; i < 2254; ++i) {
        const float v = sigmoid(score[i]);
        if (std::isfinite(v) && v > confidence) {
            confidence = v;
            best = i;
        }
    }
    if (best < 0)
        return {};
    const auto point = [&](int i) {
        return Vec{(box[best * 12 + 4 + 2 * i] / d + s.anchor[best].x) * scale - left / ratio,
                   (box[best * 12 + 5 + 2 * i] / d + s.anchor[best].y) * scale - top / ratio};
    };
    const auto hip = point(0), full = point(1);
    const float radius = std::hypot(full.x - hip.x, full.y - hip.y);
    if (!std::isfinite(radius) || radius < 15 || radius > scale * 3)
        return {};
    const Vec up{(full.x - hip.x) / radius, (full.y - hip.y) / radius}, right{-up.y, up.x};
    const auto camera = [&](float x, float y) {
        return LandmarkPoint{hip.x + right.x * x + up.x * y, hip.y + right.y * x + up.y * y};
    };
    s.input.resize(p * p * 3);
    for (int y = 0; y < p; ++y)
        for (int x = 0; x < p; ++x) {
            const auto source =
                camera(((x + .5f) / p - .5f) * 2 * radius, (.5f - (y + .5f) / p) * 2 * radius);
            const auto rgb = sample(image, w, h, source.x, source.y);
            for (int c = 0; c < 3; ++c)
                s.input[(y * p + x) * 3 + c] = rgb[c];
        }
    auto pose = run(s.pose, s.input, poseShape);
    const float* landmarks = nullptr;
    float presence = 0;
    for (auto& out : pose) {
        const auto shape = out.GetTensorTypeAndShapeInfo().GetShape();
        if (shape == std::vector<int64_t>{1, 195})
            landmarks = out.GetTensorData<float>();
        if (shape == std::vector<int64_t>{1, 1})
            presence = out.GetTensorData<float>()[0];
    }
    if (!landmarks)
        throw std::runtime_error("Unexpected MediaPipe pose output");
    if (!std::isfinite(presence) || presence < .6f)
        return {};
    std::array<LandmarkPoint, 33> points{};
    std::array<float, 33> visibility{};
    for (int i = 0; i < 33; ++i) {
        points[i] = camera((landmarks[i * 5] / p - .5f) * 2 * radius,
                           (.5f - landmarks[i * 5 + 1] / p) * 2 * radius);
        visibility[i] = std::min(sigmoid(landmarks[i * 5 + 3]), sigmoid(landmarks[i * 5 + 4]));
        if (!std::isfinite(points[i].x) || !std::isfinite(points[i].y) || points[i].x < 0 ||
            points[i].x >= w || points[i].y < 0 || points[i].y >= h)
            visibility[i] = 0;
    }
    const float shoulderConfidence = std::min(visibility[11], visibility[12]);
    if (!std::isfinite(shoulderConfidence) || shoulderConfidence < .6f)
        return {};
    const auto arm = [&](int shoulder, int elbow, int wrist) {
        return mapArm(points[shoulder], points[elbow], points[wrist],
                      std::min({visibility[shoulder], visibility[elbow], visibility[wrist]}));
    };
    const bool hasHips = visibility[23] >= 0.4f && visibility[24] >= 0.4f;
    const auto angles = mapBodyAngles(points[11], points[12],
                                      landmarks[11 * 5 + 2], landmarks[12 * 5 + 2],
                                      landmarks[23 * 5 + 2], landmarks[24 * 5 + 2],
                                      hasHips);
    return {arm(11, 13, 15), arm(12, 14, 16), presence * shoulderConfidence,
            angles.angleX, angles.angleY, angles.angleZ};
}
#else
struct BodyTracker::Impl {};
BodyTracker::BodyTracker(const std::filesystem::path&) {
    throw std::runtime_error("Body tracking requires ONNX Runtime");
}
BodyTracker::~BodyTracker() = default;
UpperBodyPose BodyTracker::track(std::span<const uint8_t>, unsigned, unsigned) {
    return {};
}
#endif
}  // namespace cortex
