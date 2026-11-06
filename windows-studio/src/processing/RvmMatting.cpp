#include "cortex/processing/RvmMatting.h"
#include "gpu/GpuHelpers.h"
#include <cstring>
#ifdef CORTEX_HAS_DIRECTML
#include <onnxruntime_cxx_api.h>
#include <dml_provider_factory.h>
#include <array>
#include <dxgi1_2.h>
namespace cortex {
struct RvmMatting::Impl {
    static constexpr UINT Width = 512, Height = 288, Count = Width * Height * 3;
    std::shared_ptr<GpuDevice> gpu;
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "cortex-rvm"};
    Ort::Session session{nullptr};
    Ort::MemoryInfo cpu = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::MemoryInfo dml{"DML", OrtDeviceAllocator, 0, OrtMemTypeDefault};
    std::vector<Ort::Value> recurrent;
    ComPtr<ID3D11ComputeShader> preprocess;
    ComPtr<ID3D11Buffer> tensor, staging;
    ComPtr<ID3D11UnorderedAccessView> tensorView;
    ComPtr<ID3D11SamplerState> sampler;
    GpuImage mask;
    std::vector<float> input = std::vector<float>(Count);
    float zero = 0, ratio = 1;
};
RvmMatting::RvmMatting(std::shared_ptr<GpuDevice> gpu, const std::filesystem::path& path)
    : impl_(std::make_unique<Impl>()) {
    auto& s = *impl_;
    s.gpu = std::move(gpu);
    if (!std::filesystem::is_regular_file(path))
        throw std::runtime_error("RVM ONNX model file not found");
    // Match the D3D11 adapter, rather than assuming GPU 0 is the decoding GPU.
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    detail::hr(s.gpu->device.As(&dxgi), "Get GPU adapter");
    detail::hr(dxgi->GetAdapter(&adapter), "Get GPU adapter");
    detail::hr(adapter->GetDesc(&desc), "Get adapter LUID");
    ComPtr<IDXGIFactory1> factory;
    detail::hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
    int index = -1;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> candidate;
        if (factory->EnumAdapters1(i, &candidate) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 d{};
        candidate->GetDesc1(&d);
        if (d.AdapterLuid.HighPart == desc.AdapterLuid.HighPart &&
            d.AdapterLuid.LowPart == desc.AdapterLuid.LowPart) {
            index = static_cast<int>(i);
            break;
        }
    }
    if (index < 0)
        throw std::runtime_error("DML adapter matching failed");
    Ort::SessionOptions options;
    options.DisableMemPattern();
    options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    options.SetIntraOpNumThreads(1);
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    const OrtDmlApi* api{};
    Ort::ThrowOnError(Ort::GetApi().GetExecutionProviderApi("DML", ORT_API_VERSION,
                                                            reinterpret_cast<const void**>(&api)));
    Ort::ThrowOnError(api->SessionOptionsAppendExecutionProvider_DML(options, index));
    // ORT executes dynamic shape/control operators on CPU; matting convolutions
    // use DirectML. DML provider creation is mandatory, with no CPU-only retry.
    wchar_t profile[32768]{};
    if (GetEnvironmentVariableW(L"CORTEX_ORT_PROFILE", profile, 32768) > 0)
        options.EnableProfiling(profile);
    s.session = Ort::Session(s.env, path.c_str(), options);
    if (s.session.GetInputCount() != 6 || s.session.GetOutputCount() != 6)
        throw std::runtime_error("Expected the official six-input/six-output RVM model");
    Ort::AllocatorWithDefaultOptions allocator;
    const char* inputs[] = {"src", "r1i", "r2i", "r3i", "r4i", "downsample_ratio"};
    for (size_t i = 0; i < 6; ++i) {
        auto name = s.session.GetInputNameAllocated(i, allocator);
        if (std::string(name.get()) != inputs[i] ||
            s.session.GetInputTypeInfo(i).GetTensorTypeAndShapeInfo().GetElementType() !=
                ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
            throw std::runtime_error("Use the official FP32 RVM ONNX model");
    }
    std::lock_guard lock(s.gpu->mutex);
    auto code = detail::compile("Preprocess", "cs_5_0");
    detail::hr(s.gpu->device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(),
                                                  nullptr, &s.preprocess),
               "AI preprocess shader");
    D3D11_BUFFER_DESC b{};
    b.ByteWidth = Impl::Count * sizeof(float);
    b.Usage = D3D11_USAGE_DEFAULT;
    b.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    b.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    b.StructureByteStride = 4;
    detail::hr(s.gpu->device->CreateBuffer(&b, nullptr, &s.tensor), "AI tensor buffer");
    detail::hr(s.gpu->device->CreateUnorderedAccessView(s.tensor.Get(), nullptr, &s.tensorView),
               "AI tensor UAV");
    b.Usage = D3D11_USAGE_STAGING;
    b.BindFlags = 0;
    b.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    b.MiscFlags = 0;
    b.StructureByteStride = 0;
    detail::hr(s.gpu->device->CreateBuffer(&b, nullptr, &s.staging), "AI tensor readback");
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    detail::hr(s.gpu->device->CreateSamplerState(&sampler, &s.sampler), "AI sampler");
    s.mask = detail::image(*s.gpu, Impl::Width, Impl::Height, DXGI_FORMAT_R32_FLOAT,
                           D3D11_BIND_SHADER_RESOURCE);
}
RvmMatting::~RvmMatting() = default;
void RvmMatting::reset() {
    impl_->recurrent.clear();
}
ComPtr<ID3D11ShaderResourceView> RvmMatting::infer(const GpuImage& image) {
    auto& s = *impl_;
    auto& input = s.input;
    {
        std::lock_guard lock(s.gpu->mutex);
        auto* srv = image.view.Get();
        auto* uav = s.tensorView.Get();
        auto* sampler = s.sampler.Get();
        s.gpu->context->CSSetShader(s.preprocess.Get(), nullptr, 0);
        s.gpu->context->CSSetShaderResources(0, 1, &srv);
        s.gpu->context->CSSetSamplers(0, 1, &sampler);
        s.gpu->context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        s.gpu->context->Dispatch(32, 18, 1);
        srv = nullptr;
        uav = nullptr;
        s.gpu->context->CSSetShaderResources(0, 1, &srv);
        s.gpu->context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        s.gpu->context->CSSetShader(nullptr, nullptr, 0);
        s.gpu->context->CopyResource(s.staging.Get(), s.tensor.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        detail::hr(s.gpu->context->Map(s.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
                   "AI tensor map");
        std::memcpy(input.data(), mapped.pData, input.size() * sizeof(float));
        s.gpu->context->Unmap(s.staging.Get(), 0);
    }
    const std::array<int64_t, 4> shape{1, 3, Impl::Height, Impl::Width}, one{1, 1, 1, 1};
    const int64_t ratioShape = 1;
    auto src = Ort::Value::CreateTensor<float>(s.cpu, input.data(), input.size(), shape.data(),
                                               shape.size());
    auto ratio = Ort::Value::CreateTensor<float>(s.cpu, &s.ratio, 1, &ratioShape, 1);
    if (s.recurrent.empty())
        for (int i = 0; i < 4; ++i)
            s.recurrent.push_back(
                Ort::Value::CreateTensor<float>(s.cpu, &s.zero, 1, one.data(), one.size()));
    Ort::IoBinding binding(s.session);
    binding.BindInput("src", src);
    binding.BindInput("downsample_ratio", ratio);
    const char* inputs[] = {"r1i", "r2i", "r3i", "r4i"};
    const char* outputs[] = {"fgr", "pha", "r1o", "r2o", "r3o", "r4o"};
    for (int i = 0; i < 4; ++i)
        binding.BindInput(inputs[i], s.recurrent[i]);
    for (int i = 0; i < 6; ++i)
        binding.BindOutput(outputs[i], i == 1 ? s.cpu : s.dml);
    binding.SynchronizeInputs();
    s.session.Run(Ort::RunOptions{nullptr}, binding);
    binding.SynchronizeOutputs();
    auto values = binding.GetOutputValues();
    auto& alpha = values[1];
    if (alpha.GetTensorTypeAndShapeInfo().GetShape() !=
        std::vector<int64_t>{1, 1, Impl::Height, Impl::Width})
        throw std::runtime_error("Unexpected RVM alpha shape");
    {
        std::lock_guard lock(s.gpu->mutex);
        s.gpu->context->UpdateSubresource(s.mask.texture.Get(), 0, nullptr,
                                          alpha.GetTensorData<float>(), Impl::Width * sizeof(float),
                                          0);
    }
    s.recurrent.clear();
    for (size_t i = 2; i < 6; ++i)
        s.recurrent.push_back(std::move(values[i]));
    return s.mask.view;
}
}  // namespace cortex
#else
namespace cortex {
struct RvmMatting::Impl {};
RvmMatting::RvmMatting(std::shared_ptr<GpuDevice>, const std::filesystem::path&) {
    throw std::runtime_error("AI support not built: set ONNXRUNTIME_ROOT and DIRECTML_ROOT");
}
RvmMatting::~RvmMatting() = default;
void RvmMatting::reset() {}
ComPtr<ID3D11ShaderResourceView> RvmMatting::infer(const GpuImage&) {
    throw std::runtime_error("AI unavailable");
}
}  // namespace cortex
#endif
