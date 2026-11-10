#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>
namespace cortex {
struct Joint3D { float x{}, y{}, z{}, confidence{}; };
struct HandLandmarks {
    // Upright camera pixels (Z relative to wrist), and wrist-relative model meters.
    std::array<Joint3D, 21> image{}, world{};
    float confidence{}, rightProbability{};
};

class HandTracker {
public:
    explicit HandTracker(const std::filesystem::path&);
    ~HandTracker();
    // Unordered detections. Assign identity by pose wrist proximity when available.
    std::vector<HandLandmarks> track(std::span<const uint8_t>, unsigned, unsigned);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
