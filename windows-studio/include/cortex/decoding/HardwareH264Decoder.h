#pragma once
#include "cortex/core/VideoFrame.h"
#include "cortex/core/EncodedVideo.h"
#include <functional>
#include <stdexcept>

namespace cortex {
class CorruptBitstream:public std::runtime_error { public:using std::runtime_error::runtime_error; };
// FFmpeg types and ownership are hidden from source, manager and UI contracts.
class HardwareH264Decoder {
public:
    HardwareH264Decoder(std::shared_ptr<GpuDevice>,std::function<void(FramePtr)> sink,bool lowDelay=true);
    ~HardwareH264Decoder();
    HardwareH264Decoder(const HardwareH264Decoder&)=delete;
    HardwareH264Decoder& operator=(const HardwareH264Decoder&)=delete;
    void decode(const EncodedAccessUnit&);
    void finish();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
