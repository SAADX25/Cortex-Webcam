#include "cortex/filters/PoseSmoother.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace cortex {
namespace {
constexpr std::array<float AvatarPose::*, 6> channels{
    &AvatarPose::yaw, &AvatarPose::pitch, &AvatarPose::roll,
    &AvatarPose::positionX, &AvatarPose::positionY, &AvatarPose::depthScale};
// The speed coefficient depends on signal units: degrees for angles, frame
// fractions for position, and a neutral-relative ratio for distance.
constexpr std::array<double, 6> speedCoefficient{.035, .035, .035, 14, 14, 14};

void validate(const AvatarPose& pose) {
    for (float value : {pose.yaw, pose.pitch, pose.roll, pose.mouth, pose.eyeOpen,
                       pose.gazeX, pose.gazeY, pose.leftEyeOpen, pose.rightEyeOpen,
                       pose.browLY, pose.browRY, pose.mouthForm,
                       pose.bodyAngleX, pose.bodyAngleY, pose.bodyAngleZ,
                       pose.positionX, pose.positionY, pose.depthScale,
                       pose.arms.left.lift, pose.arms.left.bend, pose.arms.left.confidence,
                       pose.arms.right.lift, pose.arms.right.bend, pose.arms.right.confidence,
                       pose.arms.confidence, pose.arms.bodyAngleX,
                       pose.arms.bodyAngleY, pose.arms.bodyAngleZ})
        if (!std::isfinite(value))
            throw std::invalid_argument("Invalid avatar pose for smoothing");
    if (pose.depthScale <= 0)
        throw std::invalid_argument("Invalid avatar distance scale");
}
}  // namespace

void PoseSmoother::reset() {
    output_ = {};
    derivative_.fill(0);
    initialized_ = false;
}

void PoseSmoother::reset(const AvatarPose& pose) {
    validate(pose);
    output_ = pose;
    derivative_.fill(0);
    initialized_ = true;
}

AvatarPose PoseSmoother::update(const AvatarPose& target, float dt, float response) {
    if (!std::isfinite(dt) || !std::isfinite(response) || dt < 0 || response < 0)
        throw std::invalid_argument("Invalid adaptive smoothing time");
    validate(target);
    // A long interruption starts a new trajectory instead of carrying velocity
    // across a camera reconnect or a paused tracking worker.
    if (!initialized_ || response == 0 || dt > .5f) {
        reset(target);
        return output_;
    }
    if (dt == 0)
        return output_;

    auto next = smoothAvatarPose(output_, target, dt, response);
    constexpr double twoPi = 2 * std::numbers::pi;
    constexpr double derivativeCutoffHz = 1;
    const double derivativeAlpha = -std::expm1(-twoPi * derivativeCutoffHz * dt);
    const double minimumCutoff = 1 / (twoPi * response);
    for (size_t index = 0; index < channels.size(); ++index) {
        const auto member = channels[index];
        const double previous = output_.*member;
        const double value = target.*member;
        // Match the reference algorithm's filtered-position derivative, so a
        // fast movement opens the filter until the displayed pose catches up.
        const double speed = (value - previous) / dt;
        derivative_[index] += derivativeAlpha * (speed - derivative_[index]);
        const double cutoff = minimumCutoff + speedCoefficient[index] *
                                                 std::abs(derivative_[index]);
        const double alpha = -std::expm1(-twoPi * cutoff * dt);
        next.*member = static_cast<float>(previous + alpha * (value - previous));
    }

    const auto eyes = smoothAvatarPose(output_, target, dt, std::min(response, .020f));
    next.eyeOpen = eyes.eyeOpen;
    next.leftEyeOpen = eyes.leftEyeOpen;
    next.rightEyeOpen = eyes.rightEyeOpen;
    const auto mouth = smoothAvatarPose(output_, target, dt, std::min(response, .030f));
    next.mouth = mouth.mouth;
    next.mouthForm = mouth.mouthForm;
    output_ = next;
    return output_;
}
}  // namespace cortex
