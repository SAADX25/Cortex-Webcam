#pragma once
#include "cortex/filters/AvatarFilter.h"
#include <array>
namespace cortex {
// Perspective fitting of the stable OpenSeeFace nose/jaw contour (BSD-2-Clause).
// Live2D ranges are [-30, 30] for head angles.
std::optional<HeadRotation> estimateHeadRotation(const std::array<LandmarkPoint, 66>&,
                                                 unsigned imageWidth, unsigned imageHeight,
                                                 float sensitivity = 1.0f);
}  // namespace cortex
