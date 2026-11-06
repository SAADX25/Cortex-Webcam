#pragma once
#include "cortex/filters/HumanoidMotion.h"
#include <filesystem>
#include <memory>
namespace cortex {
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
