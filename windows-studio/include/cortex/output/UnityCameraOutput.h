#pragma once
#include "cortex/filters/VideoProcessor.h"
namespace cortex {
struct OutputStats {
    uint64_t sent{}, skipped{}, inactive{};
};
class UnityCameraOutput {
public:
    UnityCameraOutput(std::shared_ptr<GpuDevice>, int device = 0);
    ~UnityCameraOutput();
    void submit(const GpuImage& rgba);  // Straight RGBA8, one render/controller thread.
    OutputStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace cortex
