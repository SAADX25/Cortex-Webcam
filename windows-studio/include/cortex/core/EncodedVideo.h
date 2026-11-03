#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>
namespace cortex {
using Clock=std::chrono::steady_clock;
struct EncodedAccessUnit {
    std::vector<uint8_t> annexB;
    int64_t timestamp90k{};
    Clock::time_point receivedAt;
    bool resetDecoder{};
    std::optional<int64_t> decodeTimestamp90k; // Local-file B-frame decode order; RTP sender has no B pictures.
    int rotationDegrees{}; // Display orientation, carried separately from encoded pixels.
};
}
