#pragma once
#include "cortex/avatar/AvatarFilter.h"
#include <array>

namespace cortex {
// Speed-adaptive low-pass filtering based on Casiez, Roussel and Vogel's
// 1 Euro Filter (CHI 2012): https://gery.casiez.net/1euro/ . Independently
// implemented with exact exponential time steps to retain the response slider's
// meaning. Call update once per new tracking result, using its sample interval.
class PoseSmoother {
public:
    void reset();
    void reset(const AvatarPose&);
    AvatarPose update(const AvatarPose& target, float sampleDtSeconds,
                      float responseSeconds);

private:
    AvatarPose output_;
    std::array<double, 21> derivative_{};
    bool initialized_{};
};
}  // namespace cortex
