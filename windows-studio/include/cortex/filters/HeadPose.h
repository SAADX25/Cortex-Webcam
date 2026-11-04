#pragma once
#include "cortex/filters/AvatarFilter.h"
#include <array>
namespace cortex {
struct HeadPoseEstimate {
    HeadRotation rotation;
    // Perspective projection of the fitted head origin, relative to image center.
    // X/Y are image fractions. Depth is relative to the reference face, not metres.
    float positionX{}, positionY{}, depth{};
};
std::optional<HeadPoseEstimate> estimateHeadPose(const std::array<LandmarkPoint, 66>&,
                                                 unsigned imageWidth, unsigned imageHeight,
                                                 float sensitivity = 1.0f,
                                                 const std::array<float, 66>* confidence = nullptr);
// Robust perspective fitting of the OpenSeeFace nose/jaw and eye corners
// (BSD-2-Clause). Returns physical angles before neutral calibration; clamp to
// the model's animation range only after subtracting the neutral pose.
std::optional<HeadRotation> estimateHeadRotation(const std::array<LandmarkPoint, 66>&,
                                                 unsigned imageWidth, unsigned imageHeight,
                                                 float sensitivity = 1.0f,
                                                 const std::array<float, 66>* confidence = nullptr);
}  // namespace cortex
