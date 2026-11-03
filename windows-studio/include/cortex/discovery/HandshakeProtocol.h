#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <optional>
namespace cortex {
struct FrameRateRequest { uint64_t id{}; int fps{30}; };
struct FrameRateReport { int fps{}; bool syncSupported{}; uint64_t acknowledged{}; std::string deviceName; };
// Small, versioned control protocol; video remains RTP/UDP on a separate port.
class HandshakeProtocol {
public:
    explicit HandshakeProtocol(uint16_t videoPort):videoPort_(videoPort){}
    std::string respond(std::string_view message,std::string_view peer,bool ready,
        std::chrono::steady_clock::time_point now,FrameRateRequest request = {});
    std::string leasePeer(std::chrono::steady_clock::time_point now);
    std::optional<FrameRateReport> frameRateReport() const { return report_; }
private:
    uint16_t videoPort_;
    std::string peer_;
    std::chrono::steady_clock::time_point expires_{};
    std::optional<FrameRateReport> report_;
};
}
