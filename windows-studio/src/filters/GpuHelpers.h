#pragma once
#include "cortex/filters/VideoProcessor.h"
#include "ShaderSource.generated.h"
#include <d3dcompiler.h>
#include <stdexcept>
namespace cortex::detail {
inline void hr(HRESULT result, const char* text) {
    if (FAILED(result))
        throw std::runtime_error(std::string(text) +
                                 " HRESULT=" + std::to_string(static_cast<unsigned long>(result)));
}
inline ComPtr<ID3DBlob> compile(const char* entry, const char* target) {
    ComPtr<ID3DBlob> code, error;
    const auto result =
        D3DCompile(ShaderSource, sizeof(ShaderSource) - 1, nullptr, nullptr, nullptr, entry, target,
                   D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error);
    if (FAILED(result))
        throw std::runtime_error(error ? static_cast<const char*>(error->GetBufferPointer())
                                       : "Shader compilation failed");
    return code;
}
inline GpuImage image(GpuDevice& gpu, UINT width, UINT height,
                      DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM,
                      UINT binds = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE) {
    GpuImage out;
    out.width = width;
    out.height = height;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = binds;
    hr(gpu.device->CreateTexture2D(&desc, nullptr, &out.texture), "Create GPU image");
    if (binds & D3D11_BIND_SHADER_RESOURCE)
        hr(gpu.device->CreateShaderResourceView(out.texture.Get(), nullptr, &out.view),
           "Create image SRV");
    return out;
}
}  // namespace cortex::detail
