#pragma once
#include <memory>
#include <functional>
#include <string>
#include <cstdint>
#include "cortex/discovery/HandshakeProtocol.h"
namespace cortex {
class StudioDiscovery {
public:
    StudioDiscovery(uint16_t videoPort, uint16_t controlPort, std::string allowedPeer,
                    std::function<bool()> ready,
                    std::function<void(const std::string&)> leaseChanged,
                    std::function<std::pair<uint64_t, uint64_t>()> videoStats = {},
                    std::function<FrameRateRequest()> frameRateRequest = {},
                    std::function<void(FrameRateReport)> frameRateReported = {});
    ~StudioDiscovery();
    StudioDiscovery(const StudioDiscovery&) = delete;
    StudioDiscovery& operator=(const StudioDiscovery&) = delete;
    std::string status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace cortex
