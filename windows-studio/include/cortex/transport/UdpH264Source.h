#pragma once
#include "cortex/sources/IVideoSource.h"
#include <thread>
#include <atomic>
#include <mutex>

namespace cortex {
struct UdpSourceConfig {
    uint16_t port{5004};
    std::string bindAddress{"0.0.0.0"};
    std::string
        senderAddress;  // Optional numeric IPv4 allowlist. No hostname lookup on receive path.
    std::chrono::milliseconds reorderWindow{8};
};
class UdpH264Source final : public IVideoSource {
public:
    UdpH264Source(std::shared_ptr<GpuDevice>, UdpSourceConfig = {});
    ~UdpH264Source() override;
    void start() override;
    void stop() noexcept override;
    FramePtr latestFrame() const override;
    SourceStatus status() const override;
    void setAutoPeer(const std::string& numericIpv4);
    std::shared_ptr<GpuDevice> gpuDevice() const override { return gpu_; }

private:
    void run() noexcept;
    std::shared_ptr<GpuDevice> gpu_;
    UdpSourceConfig config_;
    // Lifecycle methods may be called by controller threads, never by the receive thread.
    std::mutex lifecycleMutex_;
    mutable std::mutex stateMutex_;
    SourceStatus status_;
    FramePtr latest_;
    std::atomic<bool> stopping_{false};
    std::atomic<uint32_t> autoPeer_{0};
    std::thread worker_;
};
}  // namespace cortex
