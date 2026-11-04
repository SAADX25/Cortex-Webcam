#include "cortex/filters/AvatarFilter.h"
#include "GpuHelpers.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace cortex {
std::optional<AvatarPose> AvatarFilter::map(const FaceLandmarks& f) {
    const float dx = f.rightEye.x - f.leftEye.x, dy = f.rightEye.y - f.leftEye.y;
    const float span = std::hypot(dx, dy);
    if (!std::isfinite(span) || span < 1)
        return {};
    const float x = f.nose.x - (f.leftEye.x + dx * .5f);
    const float y = f.nose.y - (f.leftEye.y + dy * .5f);
    const float nx = (x * dx + y * dy) / (span * span), ny = (y * dx - x * dy) / (span * span);
    const float mouth = std::hypot(f.upperLip.x - f.lowerLip.x, f.upperLip.y - f.lowerLip.y) / span;
    for (float v : {nx, ny, mouth, f.eyeOpen, f.gazeX, f.gazeY, f.leftEyeOpen, f.rightEyeOpen})
        if (!std::isfinite(v))
            return {};
    const HeadRotation rotation =
        f.needsHeadFit && !f.headRotation
            ? HeadRotation{}
            : f.headRotation.value_or(HeadRotation{
                  std::clamp(nx * 80, -45.f, 45.f), std::clamp((ny - .55f) * 80, -35.f, 35.f),
                  std::atan2(dy, dx) * 180 / std::numbers::pi_v<float>});
    if (!std::isfinite(rotation.yaw) || !std::isfinite(rotation.pitch) ||
        !std::isfinite(rotation.roll))
        return {};
    return AvatarPose{rotation.yaw,
                      rotation.pitch,
                      rotation.roll,
                      std::clamp(mouth * 4, 0.f, 1.f),
                      std::clamp(f.eyeOpen, 0.f, 1.f),
                      std::clamp(f.gazeX, -1.f, 1.f),
                      std::clamp(f.gazeY, -1.f, 1.f),
                      f.leftEyeOpen < 0 ? -1.f : std::clamp(f.leftEyeOpen, 0.f, 1.f),
                      f.rightEyeOpen < 0 ? -1.f : std::clamp(f.rightEyeOpen, 0.f, 1.f),
                      true};
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
    AvatarPose result{
        blend(previous.yaw, target.yaw),
        blend(previous.pitch, target.pitch),
        blend(previous.roll, target.roll),
        blend(previous.mouth, target.mouth),
        blend(previous.eyeOpen, target.eyeOpen),
        blend(previous.gazeX, target.gazeX),
        blend(previous.gazeY, target.gazeY),
        eye(previous.leftEyeOpen, target.leftEyeOpen, previous.eyeOpen, target.eyeOpen),
        eye(previous.rightEyeOpen, target.rightEyeOpen, previous.eyeOpen, target.eyeOpen),
        target.cameraDriven};
    result.armsEnabled = target.armsEnabled;
    const auto arm = [&](const ArmPose& a, const ArmPose& b) {
        return ArmPose{blend(a.lift, b.lift), blend(a.bend, b.bend), b.confidence};
    };
    result.arms = {arm(previous.arms.left, target.arms.left),
                   arm(previous.arms.right, target.arms.right), target.arms.confidence};
    return result;
}

AvatarPose AvatarFilter::testPose(double seconds) {
    const float t = static_cast<float>(std::fmod(seconds, 120.0));
    return {18 * std::sin(t),
            9 * std::sin(2 * t),
            6 * std::sin(t * .5f),
            .5f + .5f * std::sin(3 * t),
            std::min(1.f, std::abs(std::fmod(t, 4.f) - 2) * 12),
            std::sin(t),
            .5f * std::cos(t)};
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
    const float scaledWidth = width * framing.scale, scaledHeight = height * framing.scale;
    const float top = std::min(1.f, float(width) / float(height));
    D3D11_VIEWPORT viewport{(width - scaledWidth) * .5f + framing.x * width,
                            (1 - framing.scale) * (1 - top) * height * .5f + framing.y * height,
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
