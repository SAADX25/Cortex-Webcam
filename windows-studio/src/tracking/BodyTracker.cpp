// MediaPipe/OpenCV Zoo preprocessing and decoding: Apache-2.0.
// See third_party/MediaPipePose/LICENSE and README.md for pinned model provenance.
#include "cortex/tracking/BodyTracker.h"
#include "cortex/tracking/HandVisibilityGate.h"
#include "cortex/tracking/HandTracker.h"
#include "cortex/tracking/HandIdentityTracker.h"
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
    const float upperLift = std::acos(std::clamp(uy / a, -1.f, 1.f)) / std::numbers::pi_v<float>;
    // In a frontal 2D camera view, lifting a hand in front of the torso foreshortens
    // the upper arm while raising the wrist above the elbow. Combine upper-arm
    // angle with wrist elevation so forward gestures also lift the avatar's shoulder.
    const float wristRaise = vy < 0.f ? std::clamp((elbow.y - wrist.y) / (a + b), 0.f, 1.f) : 0.f;
    const float lift = std::clamp(upperLift * 1.35f + wristRaise * 0.55f, 0.f, 1.f);
    const float rawBend =
        std::acos(std::clamp((ux * vx + uy * vy) / (a * b), -1.f, 1.f)) / std::numbers::pi_v<float>;
    const float bend = std::clamp(rawBend * 1.25f, 0.f, 1.f);
    return {lift, bend, std::clamp(confidence, 0.f, 1.f)};
}
void mapHand(ArmPose& arm, LandmarkPoint elbow, LandmarkPoint wrist, LandmarkPoint pinky,
             LandmarkPoint index, LandmarkPoint thumb, float confidence) {
    arm.handConfidence = 0;
    if (arm.confidence <= 0 || !std::isfinite(confidence) || confidence < .5f)
        return;
    const float fx = wrist.x - elbow.x, fy = wrist.y - elbow.y;
    const float forearm = std::hypot(fx, fy);
    const float hx = (pinky.x + index.x) * .5f - wrist.x, hy = (pinky.y + index.y) * .5f - wrist.y;
    const float handLen = std::hypot(hx, hy);
    if (!std::isfinite(forearm) || !std::isfinite(handLen) || forearm < 5 || handLen < 2)
        return;
    // Signed angle forearm -> hand direction, ±90° maps to ±1.
    const float cross = fx * hy - fy * hx, dot = fx * hx + fy * hy;
    arm.wrist = std::clamp(std::atan2(cross, dot) / (std::numbers::pi_v<float> * .5f), -1.f, 1.f);
    // A fist pulls the fingertips back to the wrist and together; an open hand
    // reaches ~0.35-0.45 forearm lengths with a wide index/pinky/thumb spread.
    const float reach = handLen / forearm;
    const float spread = (std::hypot(index.x - pinky.x, index.y - pinky.y) +
                          std::hypot(thumb.x - index.x, thumb.y - index.y)) / forearm;
    const float openReach = std::clamp((reach - .18f) / (.38f - .18f), 0.f, 1.f);
    const float openSpread = std::clamp((spread - .12f) / (.30f - .12f), 0.f, 1.f);
    arm.hand = std::clamp(.6f * openReach + .4f * openSpread, 0.f, 1.f);
    arm.handConfidence = std::clamp(confidence, 0.f, 1.f);
}
void mapHand21(ArmPose& arm, LandmarkPoint elbow, const HandLandmarks& hand) {
    if (arm.confidence <= 0 || !std::isfinite(hand.confidence) || hand.confidence < .6f)
        return;
    const auto dist3 = [](Joint3D a, Joint3D b) {
        return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) +
                         (a.z - b.z) * (a.z - b.z));
    };
    const float palmLen = dist3(hand.world[0], hand.world[9]);
    if (!std::isfinite(palmLen) || palmLen < 1e-3f)
        return;
    // Measure average finger extension across all 5 fingers (tips 4, 8, 12, 16, 20)
    // relative to the palm length (wrist 0 -> middle MCP 9) plus finger splay.
    float extension = 0.f;
    for (int tip : {4, 8, 12, 16, 20})
        extension += dist3(hand.world[0], hand.world[tip]) / palmLen;
    extension *= 0.2f;
    const float splay = (dist3(hand.world[8], hand.world[20]) +
                         dist3(hand.world[4], hand.world[8])) / palmLen;
    const float openExt = std::clamp((extension - 1.05f) / (1.80f - 1.05f), 0.f, 1.f);
    const float openSplay = std::clamp((splay - 0.55f) / (1.35f - 0.55f), 0.f, 1.f);
    arm.hand = std::clamp(0.75f * openExt + 0.25f * openSplay, 0.f, 1.f);
    arm.handConfidence = std::clamp(hand.confidence, 0.f, 1.f);
    // Refine 2D wrist bend using the 21-point wrist (0) -> middle MCP (9) vector.
    const float fx = hand.image[0].x - elbow.x, fy = hand.image[0].y - elbow.y;
    const float forearm = std::hypot(fx, fy);
    const float hx = hand.image[9].x - hand.image[0].x, hy = hand.image[9].y - hand.image[0].y;
    const float handLen = std::hypot(hx, hy);
    if (std::isfinite(forearm) && std::isfinite(handLen) && forearm >= 5.f && handLen >= 2.f) {
        const float cross = fx * hy - fy * hx, dot = fx * hx + fy * hy;
        arm.wrist =
            std::clamp(std::atan2(cross, dot) / (std::numbers::pi_v<float> * .5f), -1.f, 1.f);
    }
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
    HandIdentityTracker handIdentity;
    HandVisibilityGate handVisibility;
    std::unique_ptr<HandTracker> hands;
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "cortex-arms"};
    Ort::Session detector{nullptr}, pose{nullptr};
    std::vector<Vec> anchor = anchors();
    std::vector<float> input;
    Vec smoothHip{};
    Vec smoothUp{0.f, -1.f};
    float smoothRadius{};
    unsigned lastW{}, lastH{};
    bool hasRoi{};
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
    const auto handDirectory = directory.parent_path() / "hands";
    if (std::filesystem::is_regular_file(handDirectory / "handpose_estimation_mediapipe_2023feb.onnx") &&
        std::filesystem::is_regular_file(handDirectory / "palm_detection_mediapipe_2023feb.onnx"))
        impl_->hands = std::make_unique<HandTracker>(handDirectory);
}
BodyTracker::~BodyTracker() = default;
void BodyTracker::reset() noexcept { impl_->hasRoi = false; impl_->handIdentity.reset(); impl_->handVisibility.reset(); }
UpperBodyPose BodyTracker::track(std::span<const uint8_t> image, unsigned w, unsigned h) {
    std::array<Joint3D, 33> imageJoints{};
    auto body = trackBody(image, w, h, imageJoints);
    if (impl_->hands) {
        const auto detections = impl_->hands->track(image, w, h);
        const auto now = std::chrono::steady_clock::now();
        auto hands = impl_->handIdentity.update(detections, imageJoints, w, h, now);
        impl_->handVisibility.update(hands, w, h, now);
        body.visibleHands = (hands[0].confidence >= .8f ? 1 : 0) +
                            (hands[1].confidence >= .8f ? 1 : 0);
        if (hands[0].confidence >= .8f && imageJoints[13].confidence >= .5f)
            mapHand21(body.left, {imageJoints[13].x, imageJoints[13].y}, hands[0]);
        if (hands[1].confidence >= .8f && imageJoints[14].confidence >= .5f)
            mapHand21(body.right, {imageJoints[14].x, imageJoints[14].y}, hands[1]);
    }
    return body;
}
UpperBodyPose BodyTracker::trackBody(std::span<const uint8_t> image, unsigned w, unsigned h,
                                    std::array<Joint3D, 33>& imageJoints) {
    if (!w || !h || image.size() != size_t(w) * h * 4)
        throw std::invalid_argument("Invalid body tracking image");
    auto& s = *impl_;
    if (w != s.lastW || h != s.lastH) {
        s.hasRoi = false;
        s.lastW = w;
        s.lastH = h;
    }
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
    float confidence = .5f;
    for (int i = 0; i < 2254; ++i) {
        const float v = sigmoid(score[i]);
        if (std::isfinite(v) && v > confidence) {
            confidence = v;
            best = i;
        }
    }
    if (best < 0) {
        s.hasRoi = false;
        return {};
    }
    const auto point = [&](int i) {
        return Vec{(box[best * 12 + 4 + 2 * i] / d + s.anchor[best].x) * scale - left / ratio,
                   (box[best * 12 + 5 + 2 * i] / d + s.anchor[best].y) * scale - top / ratio};
    };
    const auto rawHip = point(0), rawFull = point(1);
    const float rawDist = std::hypot(rawFull.x - rawHip.x, rawFull.y - rawHip.y);
    if (!std::isfinite(rawDist) || rawDist < 15 || rawDist > scale * 3) {
        s.hasRoi = false;
        return {};
    }
    //Enlarge the crop slightly so raised elbows and wrists remain inside the 256x256 ROI,
    // and low-pass filter the detector ROI across frames to prevent crop-induced landmark jitter.
    const float rawRadius = rawDist * 1.15f;
    Vec rawUp{(rawFull.x - rawHip.x) / rawDist, (rawFull.y - rawHip.y) / rawDist};
    // In upper-body framing when hips are below the frame, person detection's hip->full-body
    // vector can swing sideways toward a raised arm. Clamp crop roll within ±30° of upright (0, -1).
    if (rawUp.y > -0.866f) {
        rawUp.x = std::clamp(rawUp.x, -0.5f, 0.5f);
        rawUp.y = -std::sqrt(std::max(0.f, 1.f - rawUp.x * rawUp.x));
    }
    if (!s.hasRoi ||
        std::hypot(rawHip.x - s.smoothHip.x, rawHip.y - s.smoothHip.y) > rawRadius * 0.5f ||
        std::abs(rawRadius - s.smoothRadius) > rawRadius * 0.4f) {
        s.smoothHip = rawHip;
        s.smoothRadius = rawRadius;
        s.smoothUp = rawUp;
        s.hasRoi = true;
    } else {
        constexpr float roiAlpha = 0.35f;
        s.smoothHip.x += roiAlpha * (rawHip.x - s.smoothHip.x);
        s.smoothHip.y += roiAlpha * (rawHip.y - s.smoothHip.y);
        s.smoothRadius += roiAlpha * (rawRadius - s.smoothRadius);
        s.smoothUp.x += roiAlpha * (rawUp.x - s.smoothUp.x);
        s.smoothUp.y += roiAlpha * (rawUp.y - s.smoothUp.y);
        const float upLen = std::hypot(s.smoothUp.x, s.smoothUp.y);
        if (upLen > 1e-4f) {
            s.smoothUp.x /= upLen;
            s.smoothUp.y /= upLen;
        } else {
            s.smoothUp = rawUp;
        }
    }
    const Vec hip = s.smoothHip;
    const float radius = s.smoothRadius;
    const Vec up = s.smoothUp, right{-up.y, up.x};
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
    if (!std::isfinite(presence) || presence < .5f) {
        s.hasRoi = false;
        return {};
    }
    std::array<LandmarkPoint, 33> points{};
    std::array<float, 33> visibility{};
    const float marginX = w * 0.25f, marginY = h * 0.35f;
    for (int i = 0; i < 33; ++i) {
        points[i] = camera((landmarks[i * 5] / p - .5f) * 2 * radius,
                           (.5f - landmarks[i * 5 + 1] / p) * 2 * radius);
        const float vis = sigmoid(landmarks[i * 5 + 3]);
        const float pres = sigmoid(landmarks[i * 5 + 4]);
        visibility[i] = (i >= 13 && i <= 22) ? (vis * (0.35f + 0.65f * pres))
                                             : std::min(vis, pres);
        const float maxY = (i >= 23) ? float(h) * 0.98f : float(h) + marginY;
        if (!std::isfinite(points[i].x) || !std::isfinite(points[i].y) ||
            points[i].x < -marginX || points[i].x >= w + marginX ||
            points[i].y < -marginY || points[i].y >= maxY)
            visibility[i] = 0;
        if(i>=15 && i<=22 && (points[i].x<0 || points[i].x>=w || points[i].y<0 || points[i].y>=h))
            visibility[i]=0;
    }
    const float shoulderConfidence = std::min(visibility[11], visibility[12]);
    if (!std::isfinite(shoulderConfidence) || shoulderConfidence < .55f)
        return {};
    const auto arm = [&](int shoulder, int elbow, int wrist) {
        const float fullConf = std::min({visibility[shoulder], visibility[elbow], visibility[wrist]});
        if (fullConf >= .6f)
            return mapArm(points[shoulder], points[elbow], points[wrist], fullConf);
        // In closer upper-body framing, the shoulder and elbow may be clearly
        // visible while the wrist touches the edge of the frame.
        const float upperConf = std::min(visibility[shoulder], visibility[elbow]);
        if (upperConf >= .6f) {
            const float ux = points[elbow].x - points[shoulder].x;
            const float uy = points[elbow].y - points[shoulder].y;
            const float len = std::hypot(ux, uy);
            if (std::isfinite(len) && len >= 3.f) {
                const LandmarkPoint extrapolatedWrist{points[elbow].x + ux, points[elbow].y + uy};
                auto partial = mapArm(points[shoulder], points[elbow], extrapolatedWrist, upperConf);
                if (visibility[wrist] >= .35f) {
                    const auto withWrist =
                        mapArm(points[shoulder], points[elbow], points[wrist], upperConf);
                    partial.bend = withWrist.bend;
                    partial.lift = std::max(partial.lift, withWrist.lift);
                }
                return partial;
            }
        }
        return ArmPose{};
    };
    const bool hasHips = visibility[23] >= 0.4f && visibility[24] >= 0.4f;
    // Normalize Z from 256-pixel pose crop space into [-1, 1] before angle mapping.
    constexpr float invP = 1.f / p;
    const auto angles = mapBodyAngles(points[11], points[12],
                                      landmarks[11 * 5 + 2] * invP, landmarks[12 * 5 + 2] * invP,
                                      landmarks[23 * 5 + 2] * invP, landmarks[24 * 5 + 2] * invP,
                                      hasHips);
    // Pose landmarks: 15/16 wrist, 17/18 pinky, 19/20 index, 21/22 thumb.
    auto leftArm = arm(11, 13, 15), rightArm = arm(12, 14, 16);
    const auto hand = [&](ArmPose& a, int elbow, int wrist, int pinky, int index, int thumb) {
        mapHand(a, points[elbow], points[wrist], points[pinky], points[index], points[thumb],
                std::min({visibility[wrist], visibility[pinky], visibility[index],
                          visibility[thumb]}));
    };
    hand(leftArm, 13, 15, 17, 19, 21);
    hand(rightArm, 14, 16, 18, 20, 22);
    for (size_t i = 0; i < 33; ++i)
        imageJoints[i] = {points[i].x, points[i].y, landmarks[i * 5 + 2] * (2 * radius / p),
                          visibility[i]};
    return {leftArm, rightArm, std::max(.61f, presence * shoulderConfidence),
            angles.angleX, angles.angleY, angles.angleZ};
}
#else
struct BodyTracker::Impl {};
BodyTracker::BodyTracker(const std::filesystem::path&) {
    throw std::runtime_error("Body tracking requires ONNX Runtime");
}
BodyTracker::~BodyTracker() = default;
void BodyTracker::reset() noexcept {}
UpperBodyPose BodyTracker::track(std::span<const uint8_t>, unsigned, unsigned) {
    return {};
}
#endif
}  // namespace cortex
