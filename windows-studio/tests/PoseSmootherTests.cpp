#include "cortex/tracking/PoseSmoother.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>

using namespace cortex;
namespace {
int checks{};
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
    ++checks;
}
bool isClose(float a, float b, float tolerance = 1e-5f) {
    return std::abs(a - b) <= tolerance;
}
template <typename Work> void rejects(Work work, const char* message) {
    bool rejected = false;
    try {
        work();
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, message);
}
}  // namespace

int main() {
    try {
        for (float fps : {30.f, 60.f, 120.f}) {
            const float dt = 1 / fps;
            PoseSmoother filter;
            filter.reset(AvatarPose{});
            double rawEnergy = 0, filteredEnergy = 0;
            const int samples = static_cast<int>(fps * 4);
            for (int sample = 0; sample < samples; ++sample) {
                AvatarPose noise;
                // A reproducible 10Hz tremor, safely below Nyquist at all rates.
                noise.yaw = .3f * std::sin(2 * std::numbers::pi_v<float> *
                                          10 * sample * dt);
                noise.positionX = noise.yaw * .002f;
                const auto pose = filter.update(noise, dt, .060f);
                if (sample > fps) {
                    rawEnergy += noise.yaw * noise.yaw;
                    filteredEnergy += pose.yaw * pose.yaw;
                }
            }
            check(filteredEnergy < rawEnergy * .25,
                  "Adaptive head filtering must suppress stationary high-frequency jitter");

            filter.reset(AvatarPose{});
            AvatarPose target;
            target.yaw = 30;
            target.positionX = .2f;
            target.depthScale = 1.2f;
            target.mouth = 1;
            target.eyeOpen = target.leftEyeOpen = target.rightEyeOpen = 0;
            auto fixed = AvatarPose{};
            AvatarPose adaptive;
            const int firstInterval = static_cast<int>(std::round(fps * .1f));
            for (int sample = 0; sample < firstInterval; ++sample) {
                adaptive = filter.update(target, dt, .060f);
                fixed = smoothAvatarPose(fixed, target, dt, .060f);
                check(adaptive.yaw >= 0 && adaptive.yaw <= target.yaw &&
                          adaptive.positionX >= 0 && adaptive.positionX <= target.positionX &&
                          adaptive.depthScale >= 1 && adaptive.depthScale <= target.depthScale,
                      "Adaptive movement must not overshoot an angular, lateral or depth step");
                if (sample == 0)
                    check(adaptive.mouth > fixed.mouth && adaptive.eyeOpen < fixed.eyeOpen,
                          "Mouth and eyelids must retain their faster expression responses");
            }
            check(adaptive.yaw > fixed.yaw + 2 &&
                      adaptive.positionX > fixed.positionX + .01f,
                  "Fast angular and lateral movement must catch up sooner than fixed 60ms smoothing");

            filter.reset(AvatarPose{});
            fixed = {};
            double adaptiveLag = 0, fixedLag = 0;
            for (int sample = 1; sample <= static_cast<int>(fps); ++sample) {
                target.yaw = 30 * sample * dt;
                target.positionX = .2f * sample * dt;
                target.depthScale = 1 + .2f * sample * dt;
                adaptive = filter.update(target, dt, .060f);
                fixed = smoothAvatarPose(fixed, target, dt, .060f);
                if (sample > fps * .25f) {
                    adaptiveLag += target.yaw - adaptive.yaw;
                    fixedLag += target.yaw - fixed.yaw;
                }
            }
            check(adaptiveLag < fixedLag * .85,
                  "Sustained head movement must have less lag than fixed smoothing");
            check(std::abs(adaptive.yaw - 30) < 2 &&
                      std::abs(adaptive.positionX - .2f) < .012f,
                  "30/60/120fps trajectories must stay within the same movement error bounds");

            filter.reset(AvatarPose{});
            AvatarPose upperBody;
            upperBody.cameraDriven = upperBody.armsEnabled = true;
            upperBody.bodyAngleX = 8;
            upperBody.bodyAngleY = -6;
            upperBody.bodyAngleZ = 5;
            upperBody.arms = {{1, .8f, .9f, -.7f, 1, .8f},
                              {.2f, .3f, .7f, .4f, 0, .6f}, .9f, 8, -6, 5};
            fixed = {};
            for (int sample = 0; sample < firstInterval; ++sample) {
                adaptive = filter.update(upperBody, dt, .060f);
                fixed = smoothAvatarPose(fixed, upperBody, dt, .060f);
                check(adaptive.bodyAngleX >= 0 && adaptive.bodyAngleX <= 8 &&
                          adaptive.bodyAngleY <= 0 && adaptive.bodyAngleY >= -6 &&
                          adaptive.bodyAngleZ >= 0 && adaptive.bodyAngleZ <= 5 &&
                          adaptive.arms.left.lift >= 0 && adaptive.arms.left.lift <= 1 &&
                          adaptive.arms.left.wrist <= 0 && adaptive.arms.left.wrist >= -.7f &&
                          adaptive.arms.right.wrist >= 0 && adaptive.arms.right.wrist <= .4f,
                      "Body and independent arm/wrist channels must approach their targets without overshoot");
                check(isClose(adaptive.arms.bodyAngleX, adaptive.bodyAngleX) &&
                          isClose(adaptive.arms.bodyAngleY, adaptive.bodyAngleY) &&
                          isClose(adaptive.arms.bodyAngleZ, adaptive.bodyAngleZ) &&
                          adaptive.arms.left.confidence == .9f &&
                          adaptive.arms.right.confidence == .7f &&
                          adaptive.arms.left.handConfidence == .8f &&
                          adaptive.arms.right.handConfidence == .6f,
                      "Filtering must keep torso channels in sync and preserve measured arm confidence");
            }
            check(adaptive.bodyAngleX > fixed.bodyAngleX &&
                      adaptive.bodyAngleY < fixed.bodyAngleY &&
                      adaptive.bodyAngleZ > fixed.bodyAngleZ &&
                      adaptive.arms.left.lift > fixed.arms.left.lift &&
                      adaptive.arms.left.bend > fixed.arms.left.bend &&
                      adaptive.arms.left.wrist < fixed.arms.left.wrist &&
                      adaptive.arms.left.hand > fixed.arms.left.hand &&
                      adaptive.arms.right.wrist > fixed.arms.right.wrist &&
                      adaptive.arms.right.hand < fixed.arms.right.hand,
                  "Fast torso, elbow, wrist and hand movement must respond faster than fixed smoothing");

            // Reset after a gesture must remove velocity from every arm/body
            // channel, including the final derivative slot used by right hand.
            filter.reset(AvatarPose{});
            const auto resetGesture = filter.update(upperBody, dt, .060f);
            PoseSmoother fresh;
            fresh.reset(AvatarPose{});
            const auto freshGesture = fresh.update(upperBody, dt, .060f);
            check(isClose(resetGesture.bodyAngleY, freshGesture.bodyAngleY) &&
                      isClose(resetGesture.arms.left.lift, freshGesture.arms.left.lift) &&
                      isClose(resetGesture.arms.left.wrist, freshGesture.arms.left.wrist) &&
                      isClose(resetGesture.arms.right.hand, freshGesture.arms.right.hand),
                  "Reset must clear adaptive velocity for torso and both arms through the last channel");
        }

        PoseSmoother filter;
        AvatarPose target;
        target.yaw = -22;
        target.positionY = -.1f;
        target.depthScale = .9f;
        auto pose = filter.update(target, 1.f / 60, .060f);
        check(isClose(pose.yaw, target.yaw) && isClose(pose.depthScale, .9f),
              "The first tracking sample must initialize without drifting from a synthetic neutral pose");
        target.yaw = 25;
        pose = filter.update(target, 0, .060f);
        check(isClose(pose.yaw, -22), "Zero elapsed time must leave the filtered pose unchanged");
        pose = filter.update(target, 1.f / 60, 0);
        check(isClose(pose.yaw, target.yaw) && isClose(pose.positionY, target.positionY),
              "Zero smoothing must preserve the exact measured movement");
        filter.reset(AvatarPose{});
        pose = filter.update(AvatarPose{}, 1.f / 60, .060f);
        check(isClose(pose.yaw, 0) && isClose(pose.depthScale, 1),
              "Explicit neutral calibration must clear movement and derivative history");
        filter.reset();
        pose = filter.update(target, 1.f / 60, .060f);
        check(isClose(pose.yaw, target.yaw), "Reset must reinitialize on the next sample");
        target.yaw = -10;
        pose = filter.update(target, 1, .060f);
        check(isClose(pose.yaw, -10), "A long camera gap must discard stale velocity");
        rejects([&] { filter.update(target, -.01f, .06f); }, "Negative sample time must be rejected");
        rejects([&] { filter.update(target, .01f, -.06f); }, "Negative response must be rejected");
        rejects([&] { filter.update(target, std::numeric_limits<float>::quiet_NaN(), .06f); },
                "Non-finite sample time must be rejected");
        rejects([&] { filter.update(target, .01f, std::numeric_limits<float>::infinity()); },
                "Non-finite response must be rejected");
        target.yaw = std::numeric_limits<float>::quiet_NaN();
        rejects([&] { filter.update(target, .01f, .06f); }, "Non-finite pose must be rejected");
        target.yaw = 0;
        target.depthScale = 0;
        rejects([&] { filter.reset(target); }, "A non-positive depth scale must be rejected");
        std::cout << "PASS: " << checks
                  << " adaptive smoothing checks: jitter, motion lag, rates, expressions and resets.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
