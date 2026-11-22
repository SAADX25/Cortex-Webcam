#include "cortex/gpu/GpuDevice.h"
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <stdexcept>

namespace cortex {
std::shared_ptr<GpuDevice> GpuDevice::create() {
    auto gpu = std::make_shared<GpuDevice>();
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL selected{};
    HRESULT hr = E_FAIL;
    ComPtr<IDXGIFactory6> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                    IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                continue;
            hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
                                   2, D3D11_SDK_VERSION, &gpu->device, &selected, &gpu->context);
            if (SUCCEEDED(hr))
                break;
        }
    }
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2,
                               D3D11_SDK_VERSION, &gpu->device, &selected, &gpu->context);
    if (FAILED(hr))
        throw std::runtime_error("Cannot create hardware D3D11 video device");
    ComPtr<ID3D11Multithread> multithread;
    if (FAILED(gpu->context.As(&multithread)))
        throw std::runtime_error("D3D11 multithread protection unavailable");
    multithread->SetMultithreadProtected(TRUE);
    return gpu;
}
}  // namespace cortex
