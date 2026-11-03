#include "cortex/filters/GpuImageTransform.h"
#include "GpuHelpers.h"
namespace cortex {
struct GpuImageTransform::Impl {
    std::shared_ptr<GpuDevice> gpu;GpuImage output;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11VertexShader> vertex;ComPtr<ID3D11PixelShader> pixel;
    ComPtr<ID3D11Buffer> constants;ComPtr<ID3D11SamplerState> sampler;ComPtr<ID3D11RasterizerState> raster;
};
GpuImageTransform::GpuImageTransform(std::shared_ptr<GpuDevice> gpu):impl_(std::make_unique<Impl>()){
    auto& s=*impl_;s.gpu=std::move(gpu);std::lock_guard lock(s.gpu->mutex);
    auto vs=detail::compile("Fullscreen","vs_5_0"),ps=detail::compile("Rotate","ps_5_0");
    detail::hr(s.gpu->device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&s.vertex),"Rotation vertex shader");
    detail::hr(s.gpu->device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&s.pixel),"Rotation pixel shader");
    D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=16;buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    detail::hr(s.gpu->device->CreateBuffer(&buffer,nullptr,&s.constants),"Rotation parameters");
    D3D11_SAMPLER_DESC sample{};sample.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sample.AddressU=sample.AddressV=sample.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sample.MaxLOD=D3D11_FLOAT32_MAX;
    detail::hr(s.gpu->device->CreateSamplerState(&sample,&s.sampler),"Rotation sampler");
    D3D11_RASTERIZER_DESC r{};r.FillMode=D3D11_FILL_SOLID;r.CullMode=D3D11_CULL_NONE;r.DepthClipEnable=TRUE;
    detail::hr(s.gpu->device->CreateRasterizerState(&r,&s.raster),"Rotation rasterizer");
}
GpuImageTransform::~GpuImageTransform()=default;
GpuImage GpuImageTransform::rotate(const GpuImage& input,int rotation){
    if(rotation!=0&&rotation!=90&&rotation!=180&&rotation!=270)throw std::invalid_argument("Rotation must be a clockwise quarter turn");
    if(rotation==0)return input;
    auto& s=*impl_;std::lock_guard lock(s.gpu->mutex);auto* context=s.gpu->context.Get();
    const auto width=rotation%180?input.height:input.width,height=rotation%180?input.width:input.height;
    if(s.output.width!=width||s.output.height!=height){
        s.target.Reset();s.output=detail::image(*s.gpu,width,height);
        detail::hr(s.gpu->device->CreateRenderTargetView(s.output.texture.Get(),nullptr,&s.target),"Rotated frame target");
    }
    auto* target=s.target.Get();context->OMSetRenderTargets(1,&target,nullptr);context->OMSetBlendState(nullptr,nullptr,0xffffffff);context->OMSetDepthStencilState(nullptr,0);context->RSSetState(s.raster.Get());
    D3D11_VIEWPORT viewport{0,0,float(width),float(height),0,1};context->RSSetViewports(1,&viewport);
    context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const float parameters[]={float(rotation/90),0,0,0};context->UpdateSubresource(s.constants.Get(),0,nullptr,parameters,0,0);
    auto* constants=s.constants.Get();auto* sampler=s.sampler.Get();auto* view=input.view.Get();
    context->VSSetShader(s.vertex.Get(),nullptr,0);context->PSSetShader(s.pixel.Get(),nullptr,0);context->PSSetConstantBuffers(0,1,&constants);context->PSSetSamplers(0,1,&sampler);context->PSSetShaderResources(0,1,&view);context->Draw(3,0);
    view=nullptr;context->PSSetShaderResources(0,1,&view);context->OMSetRenderTargets(0,nullptr,nullptr);return s.output;
}
}
