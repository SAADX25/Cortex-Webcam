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
}  // namespace cortex
