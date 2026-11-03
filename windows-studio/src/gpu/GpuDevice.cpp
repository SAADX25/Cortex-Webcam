#include "cortex/gpu/GpuDevice.h"
#include <d3d11_4.h>
#include <stdexcept>

namespace cortex {
std::shared_ptr<GpuDevice> GpuDevice::create() {
    auto gpu = std::make_shared<GpuDevice>();
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL selected{};
    HRESULT hr =
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                          D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                          levels, 2, D3D11_SDK_VERSION, &gpu->device, &selected, &gpu->context);
    if (FAILED(hr))
        throw std::runtime_error("Cannot create hardware D3D11 video device");
    ComPtr<ID3D11Multithread> multithread;
    if (FAILED(gpu->context.As(&multithread)))
        throw std::runtime_error("D3D11 multithread protection unavailable");
    multithread->SetMultithreadProtected(TRUE);
    return gpu;
}
}  // namespace cortex
