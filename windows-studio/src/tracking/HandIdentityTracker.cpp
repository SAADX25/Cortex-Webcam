#include "cortex/tracking/HandIdentityTracker.h"
#include <algorithm>
#include <cmath>

namespace cortex {
namespace {
bool visible(const Joint3D& p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.confidence) && p.confidence >= .65f;
}
}
void HandIdentityTracker::reset() noexcept { tracks_ = {}; width_ = height_ = 0; }
std::array<HandLandmarks, 2> HandIdentityTracker::update(
    std::span<const HandLandmarks> detections, const std::array<Joint3D, 33>& body,
    unsigned width, unsigned height, std::chrono::steady_clock::time_point time) {
    std::array<HandLandmarks, 2> result{};
    if (!width || !height) return result;
    if (width != width_ || height != height_) { reset(); width_ = width; height_ = height; }
    std::array<const HandLandmarks*, 2> hands{};
    size_t count = 0;
    for (const auto& hand : detections) {
        if (!std::isfinite(hand.confidence) || hand.confidence < .8f ||
            !std::isfinite(hand.image[0].x) || !std::isfinite(hand.image[0].y)) continue;
        if (count < 2) hands[count++] = &hand;
        else if (hand.confidence > hands[1]->confidence) hands[1] = &hand;
        if (count == 2 && hands[1]->confidence > hands[0]->confidence) std::swap(hands[0], hands[1]);
    }
    const float scale = static_cast<float>(std::min(width, height));
    // Pose often merges the two wrist landmarks during contact. In that case
    // use handedness and predicted motion rather than a noisy nearest wrist.
    const bool poseUseful = visible(body[15]) && visible(body[16]) &&
        std::hypot(body[15].x - body[16].x, body[15].y - body[16].y) > scale * .10f;
    const auto cost = [&](const HandLandmarks& hand, int side) {
        float probability = std::isfinite(hand.rightProbability)
            ? std::clamp(hand.rightProbability, 0.f, 1.f) : .5f;
        float value = (side ? 1.f - probability : probability) * 1.6f;
        if (poseUseful) {
            const auto& wrist = body[15 + side];
            float d = std::hypot(hand.image[0].x - wrist.x, hand.image[0].y - wrist.y) / scale;
            value += std::min(d * d * 30.f, 6.f) * std::min(body[15].confidence, body[16].confidence);
        }
        const auto& track = tracks_[side];
        float gap = std::chrono::duration<float>(time - track.time).count();
        if (track.valid && gap >= 0 && gap <= .25f) {
            float horizon = std::min(gap, .1f);
            float x = track.wrist.x + track.velocityX * horizon;
            float y = track.wrist.y + track.velocityY * horizon;
            float d = std::hypot(hand.image[0].x - x, hand.image[0].y - y) / scale;
            value += std::min(d * d * 12.f, 2.f);
        }
        return value;
    };
    if (count == 1) {
        int side = cost(*hands[0], 0) <= cost(*hands[0], 1) ? 0 : 1;
        result[side] = *hands[0];
    } else if (count == 2) {
        bool swapped = cost(*hands[0], 1) + cost(*hands[1], 0) < cost(*hands[0], 0) + cost(*hands[1], 1);
        result[swapped ? 1 : 0] = *hands[0];
        result[swapped ? 0 : 1] = *hands[1];
    }
    for (int side = 0; side < 2; ++side) {
        if (result[side].confidence < .8f) continue;
        auto& track = tracks_[side];
        float dt = std::chrono::duration<float>(time - track.time).count();
        float vx = 0, vy = 0;
        if (track.valid && dt > .005f && dt <= .25f) {
            vx = std::clamp((result[side].image[0].x - track.wrist.x) / dt, -scale * 3, scale * 3);
            vy = std::clamp((result[side].image[0].y - track.wrist.y) / dt, -scale * 3, scale * 3);
        }
        track = {result[side].image[0], vx, vy, time, true};
    }
    return result;
}
}
