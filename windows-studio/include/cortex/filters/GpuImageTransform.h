#pragma once
#include "cortex/filters/VideoProcessor.h"
namespace cortex {
// GPU-only RGBA rotation. Encoded NV12 buffers keep their original geometry.
class GpuImageTransform {
public:
    explicit GpuImageTransform(std::shared_ptr<GpuDevice>);
    ~GpuImageTransform();
    GpuImage rotate(const GpuImage&,int clockwiseDegrees);
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
