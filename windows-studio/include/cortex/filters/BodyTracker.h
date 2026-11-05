#pragma once
#include "cortex/filters/AvatarFilter.h"
#include <span>
namespace cortex {
// Called only on the inference worker. Coordinates are upright camera pixels.
class BodyTracker {
public:
    explicit BodyTracker(const std::filesystem::path& modelDirectory);
    ~BodyTracker();
    UpperBodyPose track(std::span<const uint8_t> rgba, unsigned width, unsigned height);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Stable arm geometry shared by model inference and unit tests.
ArmPose mapArm(LandmarkPoint shoulder, LandmarkPoint elbow, LandmarkPoint wrist, float confidence);

// Wrist bend and hand openness from MediaPipe Pose hand landmarks (pinky,
// index and thumb tips). Updates arm.wrist/hand/handConfidence in place.
void mapHand(ArmPose& arm, LandmarkPoint elbow, LandmarkPoint wrist, LandmarkPoint pinky,
             LandmarkPoint index, LandmarkPoint thumb, float confidence);

// Stable body angle geometry mapped to Live2D [-10, 10] range.
struct BodyAngles {
    float angleX{}, angleY{}, angleZ{};
};
BodyAngles mapBodyAngles(LandmarkPoint leftShoulder, LandmarkPoint rightShoulder,
                         float leftShoulderZ, float rightShoulderZ,
                         float leftHipZ = 0.f, float rightHipZ = 0.f,
                         bool hasHips = false);
}  // namespace cortex
