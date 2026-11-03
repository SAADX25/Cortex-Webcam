#pragma once
#include "cortex/filters/VideoProcessor.h"
namespace cortex {
class RvmMatting {
public:
    RvmMatting(std::shared_ptr<GpuDevice>, const std::filesystem::path& model);
    ~RvmMatting();
    ComPtr<ID3D11ShaderResourceView> infer(const GpuImage&);
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace cortex
