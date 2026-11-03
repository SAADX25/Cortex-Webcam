#pragma once
#include "cortex/core/VideoFrame.h"
#include <string>

namespace cortex {
struct SourceStatus {
    enum class State { Stopped, Starting, Running, Failed } state{State::Stopped};
    std::string detail;
    uint64_t decodedFrames{},packets{},packetGaps{},droppedAccessUnits{},decodeErrors{};
};
class IVideoSource {
public:
    virtual ~IVideoSource()=default;
    virtual void start()=0;
    virtual void stop() noexcept=0;
    virtual FramePtr latestFrame() const=0;
    virtual SourceStatus status() const=0;
    virtual std::shared_ptr<GpuDevice> gpuDevice() const=0;
};
}
