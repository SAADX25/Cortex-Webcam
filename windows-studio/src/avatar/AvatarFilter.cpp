#include "cortex/avatar/AvatarFilter.h"
#include "gpu/GpuHelpers.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace cortex {
std::optional<AvatarPose> AvatarFilter::map(const FaceLandmarks& f) {
    if (f.needsHeadFit && !f.headRotation)
        return {};
    const float dx = f.rightEye.x - f.leftEye.x, dy = f.rightEye.y - f.leftEye.y;
    const float span = std::hypot(dx, dy);
    if (!std::isfinite(span) || span < 1)
        return {};
    const float x = f.nose.x - (f.leftEye.x + dx * .5f);
    const float y = f.nose.y - (f.leftEye.y + dy * .5f);
    const float nx = (x * dx + y * dy) / (span * span), ny = (y * dx - x * dy) / (span * span);
    const float rawMouth = std::hypot(f.upperLip.x - f.lowerLip.x, f.upperLip.y - f.lowerLip.y) / span;
    for (float v : {nx, ny, rawMouth, f.eyeOpen, f.gazeX, f.gazeY, f.leftEyeOpen, f.rightEyeOpen,
                    f.browLY, f.browRY, f.mouthOpenY, f.mouthForm, f.headDepth,
                    f.headPositionX, f.headPositionY})
        if (!std::isfinite(v))
            return {};
    const HeadRotation rotation =
        f.headRotation.value_or(HeadRotation{
                  std::clamp(nx * 80, -30.f, 30.f), std::clamp((ny - .55f) * 80, -30.f, 30.f),
                  std::clamp(std::atan2(dy, dx) * 180 / std::numbers::pi_v<float>, -30.f, 30.f)});
    if (!std::isfinite(rotation.yaw) || !std::isfinite(rotation.pitch) ||
        !std::isfinite(rotation.roll))
        return {};
    constexpr float mouthBaseline = 0.03f;
    const float mouthVal = f.mouthOpenY > 0 ? f.mouthOpenY : (rawMouth <= mouthBaseline ? 0.f : std::clamp((rawMouth - mouthBaseline) * 4.5f, 0.f, 1.f));
    AvatarPose pose{};
    // Keep physical angles until neutral calibration. Clipping a tilted camera's
    // baseline here loses movement before the renderer ever sees it.
    pose.yaw = rotation.yaw;
    pose.pitch = rotation.pitch;
    pose.roll = rotation.roll;
    pose.headDepth = f.headDepth;
    if (f.hasHeadPosition) {
        pose.positionX = std::clamp(f.headPositionX, -.5f, .5f);
        pose.positionY = std::clamp(f.headPositionY, -.5f, .5f);
    } else if (f.imageWidth && f.imageHeight) {
        pose.positionX = std::clamp((f.leftEye.x + f.rightEye.x) * .5f / f.imageWidth - .5f,
                                    -.5f, .5f);
        pose.positionY = std::clamp((f.leftEye.y + f.rightEye.y) * .5f / f.imageHeight - .5f,
                                    -.5f, .5f);
    }
    pose.mouth = std::clamp(mouthVal, 0.f, 1.f);
    pose.eyeOpen = std::clamp(f.eyeOpen, 0.f, 1.f);
    pose.gazeX = std::clamp(f.gazeX, -1.f, 1.f);
    pose.gazeY = std::clamp(f.gazeY, -1.f, 1.f);
    pose.leftEyeOpen = f.leftEyeOpen < 0 ? -1.f : std::clamp(f.leftEyeOpen, 0.f, 1.f);
    pose.rightEyeOpen = f.rightEyeOpen < 0 ? -1.f : std::clamp(f.rightEyeOpen, 0.f, 1.f);
    pose.browLY = std::clamp(f.browLY, -1.f, 1.f);
    pose.browRY = std::clamp(f.browRY, -1.f, 1.f);
    pose.mouthForm = std::clamp(f.mouthForm, -1.f, 1.f);
    pose.cameraDriven = true;
    return pose;
}

AvatarPose smoothAvatarPose(const AvatarPose& previous, const AvatarPose& target, float dt,
                            float response) {
    if (!std::isfinite(dt) || !std::isfinite(response) || dt < 0 || response < 0)
        throw std::invalid_argument("Invalid avatar smoothing time");
    const float alpha = response == 0 ? 1.f : -std::expm1(-dt / response);
    const auto blend = [alpha](float a, float b) { return a + alpha * (b - a); };
    const auto eye = [&](float a, float b, float fallbackA, float fallbackB) {
        return blend(a < 0 ? fallbackA : a, b < 0 ? fallbackB : b);
    };
    AvatarPose result{};
    result.yaw = blend(previous.yaw, target.yaw);
    result.pitch = blend(previous.pitch, target.pitch);
    result.roll = blend(previous.roll, target.roll);
    result.mouth = blend(previous.mouth, target.mouth);
    result.eyeOpen = blend(previous.eyeOpen, target.eyeOpen);
    result.gazeX = blend(previous.gazeX, target.gazeX);
    result.gazeY = blend(previous.gazeY, target.gazeY);
    result.leftEyeOpen = eye(previous.leftEyeOpen, target.leftEyeOpen, previous.eyeOpen, target.eyeOpen);
    result.rightEyeOpen = eye(previous.rightEyeOpen, target.rightEyeOpen, previous.eyeOpen, target.eyeOpen);
    result.browLY = blend(previous.browLY, target.browLY);
    result.browRY = blend(previous.browRY, target.browRY);
    result.mouthForm = blend(previous.mouthForm, target.mouthForm);
    result.bodyAngleX = blend(previous.bodyAngleX, target.bodyAngleX);
    result.bodyAngleY = blend(previous.bodyAngleY, target.bodyAngleY);
    result.bodyAngleZ = blend(previous.bodyAngleZ, target.bodyAngleZ);
    result.positionX = blend(previous.positionX, target.positionX);
    result.positionY = blend(previous.positionY, target.positionY);
    result.depthScale = blend(previous.depthScale, target.depthScale);
    result.headDepth = target.headDepth;
    result.cameraDriven = target.cameraDriven;
    result.armsEnabled = target.armsEnabled;
    const auto arm = [&](const ArmPose& a, const ArmPose& b) {
        return ArmPose{blend(a.lift, b.lift), blend(a.bend, b.bend), b.confidence,
                       blend(a.wrist, b.wrist), blend(a.hand, b.hand), b.handConfidence};
    };
    result.arms = {arm(previous.arms.left, target.arms.left),
                   arm(previous.arms.right, target.arms.right), target.arms.confidence,
                   result.bodyAngleX, result.bodyAngleY, result.bodyAngleZ};
    return result;
}

AvatarPose AvatarFilter::testPose(double seconds) {
    const float t = static_cast<float>(std::fmod(seconds, 120.0));
    AvatarPose p{};
    p.yaw = 18 * std::sin(t);
    p.pitch = 9 * std::sin(2 * t);
    p.roll = 6 * std::sin(t * .5f);
    p.mouth = .5f + .5f * std::sin(3 * t);
    p.eyeOpen = std::min(1.f, std::abs(std::fmod(t, 4.f) - 2) * 12);
    p.gazeX = std::sin(t);
    p.gazeY = .5f * std::cos(t);
    p.browLY = .3f * std::sin(t);
    p.browRY = .3f * std::sin(t);
    p.mouthForm = .5f * std::sin(t);
    p.bodyAngleX = 4 * std::sin(t);
    p.bodyAngleY = 2 * std::sin(2 * t);
    p.bodyAngleZ = 3 * std::sin(t * .5f);
    p.cameraDriven = false;
    return p;
}

AvatarFilter::AvatarFilter(std::shared_ptr<GpuDevice> gpu)
    : gpu_(std::move(gpu)) {
    const auto code = detail::compile("AvatarPreview", "ps_5_0");
    detail::hr(gpu_->device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(),
                                               nullptr, &shader_),
               "Avatar preview shader");
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = 32;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    detail::hr(gpu_->device->CreateBuffer(&desc, nullptr, &constants_), "Avatar pose buffer");
}

void AvatarFilter::draw(const AvatarPose& pose, unsigned width, unsigned height,
                        const AvatarFraming& framing) {
    const float depthScale = pose.cameraDriven ? pose.depthScale : 1;
    const float scale = framing.scale * depthScale;
    const float scaledWidth = width * scale, scaledHeight = height * scale;
    const float top = std::min(1.f, float(width) / float(height));
    const float moveX = pose.cameraDriven ? pose.positionX : 0;
    const float moveY = pose.cameraDriven ? pose.positionY : 0;
    D3D11_VIEWPORT viewport{(width - scaledWidth) * .5f + (framing.x + moveX) * width,
                            (1 - scale) * (1 - top) * height * .5f +
                                (framing.y + moveY) * height,
                            scaledWidth,
                            scaledHeight,
                            0,
                            1};
    const float params[] = {pose.yaw,     pose.pitch, pose.roll,  pose.mouth,
                            pose.eyeOpen, pose.gazeX, pose.gazeY, float(width) / float(height)};
    auto* context = gpu_->context.Get();
    context->RSSetViewports(1, &viewport);
    context->UpdateSubresource(constants_.Get(), 0, nullptr, params, 0, 0);
    auto* buffer = constants_.Get();
    context->PSSetConstantBuffers(0, 1, &buffer);
    context->PSSetShader(shader_.Get(), nullptr, 0);
    context->Draw(3, 0);
}
}  // namespace cortex
