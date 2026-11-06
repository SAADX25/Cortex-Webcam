#pragma once
#include "cortex/tracking/HumanoidMotion.h"

namespace cortex {
// Assign both detections jointly; image X order is not anatomical hand identity.
// This state belongs to one body worker and resets when camera framing changes.
class HandIdentityTracker {
public:
    std::array<HandLandmarks, 2> update(std::span<const HandLandmarks>,
        const std::array<Joint3D, 33>& bodyImage, unsigned width, unsigned height,
        std::chrono::steady_clock::time_point time);
    void reset() noexcept;
private:
    struct Track {
        Joint3D wrist;
        float velocityX{}, velocityY{};
        std::chrono::steady_clock::time_point time;
        bool valid{};
    };
    std::array<Track, 2> tracks_{};
    unsigned width_{}, height_{};
};
}
