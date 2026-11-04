#pragma once
#include "cortex/filters/AvatarFilter.h"
#include <array>
namespace cortex {
// Perspective fitting of the stable OpenSeeFace nose/jaw contour (BSD-2-Clause).
std::optional<HeadRotation> estimateHeadRotation(const std::array<LandmarkPoint, 66>&,
                                                 unsigned imageWidth, unsigned imageHeight);
}  // namespace cortex
