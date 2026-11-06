#pragma once
#include "cortex/avatar/AvatarFilter.h"
#include "cortex/tracking/HumanoidMotion.h"
#include <span>
namespace cortex {
// Called only on the inference worker. Coordinates are upright camera pixels.
class BodyTracker {
public:
    explicit BodyTracker(const std::filesystem::path& modelDirectory);
    ~BodyTracker();
    void reset() noexcept;
    UpperBodyPose track(std::span<const uint8_t> rgba, unsigned width, unsigned height);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    UpperBodyPose trackBody(std::span<const uint8_t>, unsigned, unsigned);
};
// Stable arm geometry shared by model inference and unit tests.
ArmPose mapArm(LandmarkPoint shoulder, LandmarkPoint elbow, LandmarkPoint wrist, float confidence);

// Wrist bend and hand openness from MediaPipe Pose hand landmarks (pinky,
// index and thumb tips) or 21-point MediaPipe Hands landmarks. Updates
// arm.wrist/hand/handConfidence in place.
void mapHand(ArmPose& arm, LandmarkPoint elbow, LandmarkPoint wrist, LandmarkPoint pinky,
             LandmarkPoint index, LandmarkPoint thumb, float confidence);
void mapHand21(ArmPose& arm, LandmarkPoint elbow, const HandLandmarks& hand);

// Stable body angle geometry mapped to Live2D [-10, 10] range.
struct BodyAngles {
    float angleX{}, angleY{}, angleZ{};
};
BodyAngles mapBodyAngles(LandmarkPoint leftShoulder, LandmarkPoint rightShoulder,
                         float leftShoulderZ, float rightShoulderZ,
                         float leftHipZ = 0.f, float rightHipZ = 0.f,
                         bool hasHips = false);
}  // namespace cortex
