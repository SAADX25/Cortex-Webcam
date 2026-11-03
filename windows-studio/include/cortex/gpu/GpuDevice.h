#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <memory>
#include <mutex>

namespace cortex {
using Microsoft::WRL::ComPtr;
class GpuDevice {
public:
    static std::shared_ptr<GpuDevice> create();
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    // Every module issuing immediate-context work must use this recursive lock.
    std::recursive_mutex mutex;
};
}  // namespace cortex
