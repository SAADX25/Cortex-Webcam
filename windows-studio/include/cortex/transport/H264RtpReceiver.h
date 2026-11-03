#pragma once
#include "cortex/core/EncodedVideo.h"
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <vector>
#include <array>

namespace cortex {
struct RtpStats {
    uint64_t packets{}, rejected{}, late{}, gaps{}, accessUnits{}, dropped{},markers{};
    std::array<uint64_t,24> nalTypes{};
};

// One instance per source, single-thread owned. RFC 6184 mode 1.
class H264RtpReceiver {
public:
    using Sink = std::function<void(EncodedAccessUnit)>;
    explicit H264RtpReceiver(Sink sink, std::chrono::milliseconds reorder = std::chrono::milliseconds(8));
    void push(std::span<const uint8_t> datagram, Clock::time_point now);
    void tick(Clock::time_point now);
    void discardUntilIdr() { lose(); }
    RtpStats stats() const { return stats_; }
private:
    struct Packet { uint16_t sequence; uint32_t timestamp, ssrc; bool marker; std::vector<uint8_t> payload; Clock::time_point arrival; int rotation{}; };
    static std::optional<Packet> parse(std::span<const uint8_t>, Clock::time_point);
    void drain(Clock::time_point now);
    void consume(const Packet&);
    void lose();
    void append(std::span<const uint8_t> nal);
    void finish(Clock::time_point now);
    Sink sink_;
    std::chrono::milliseconds reorder_;
    std::map<uint16_t, Packet> pending_;
    std::optional<uint16_t> expected_;
    std::optional<uint32_t> ssrc_, timestamp_, lastTimestamp_;
    Clock::time_point lastPacket_{};
    int64_t extendedTimestamp_{};
    int rotation_{};
    std::vector<uint8_t> au_, fu_, sps_, pps_;
    bool damaged_{}, waitingIdr_{true}, idr_{}, hasVcl_{}, reset_{true};
    RtpStats stats_;
};
}
