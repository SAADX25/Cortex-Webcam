#include "cortex/filters/VideoProcessor.h"
#include "cortex/filters/RvmMatting.h"
#include "GpuHelpers.h"
#include <wincodec.h>
#include <cmath>
namespace cortex {
struct VideoProcessor::Impl {
    struct Source {
        GpuColorConverter converter;
        FramePtr previous;
        GpuImage rgb;
        std::unique_ptr<RvmMatting> matting;
        ComPtr<ID3D11ShaderResourceView> mask;
        explicit Source(std::shared_ptr<GpuDevice> gpu):converter(std::move(gpu)){}
    };
    std::shared_ptr<GpuDevice> gpu;
    GpuImage target,background,exported;
    ComPtr<ID3D11RenderTargetView> targetView,exportView;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> composite,backgroundShader,exportShader;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11RasterizerState> rasterizer;
    std::map<SourceId,std::unique_ptr<Source>> sources;
    std::unique_ptr<GpuColorConverter> backgroundConverter;
    std::filesystem::path model;
    FilterSettings settings;
    void draw(const GpuImage& image,ID3D11ShaderResourceView* mask,NormalizedRect rect,bool isBackground){
        const float availableW=target.width*rect.width,availableH=target.height*rect.height;
        float width=availableW,height=availableH;
        if(!isBackground){const float scale=std::min(availableW/image.width,availableH/image.height);width=image.width*scale;height=image.height*scale;}
        float uvX=1,uvY=1;
        if(isBackground){const float scale=(settings.backgroundFit?std::min(availableW/image.width,availableH/image.height):std::max(availableW/image.width,availableH/image.height))*settings.backgroundZoom;uvX=availableW/(image.width*scale);uvY=availableH/(image.height*scale);}
        D3D11_VIEWPORT viewport{target.width*rect.x+(availableW-width)*(isBackground?settings.backgroundX:.5f),target.height*rect.y+(availableH-height)*(isBackground?settings.backgroundY:.5f),width,height,0,1};
        auto* context=gpu->context.Get();context->RSSetViewports(1,&viewport);
        const float values[]={settings.similarity,settings.softness,settings.spill,static_cast<float>(settings.removal==RemovalMode::ChromaKey?1:settings.removal==RemovalMode::AiMatting?2:0),uvX,uvY,(1-uvX)*settings.backgroundX,(1-uvY)*settings.backgroundY};
        context->UpdateSubresource(constants.Get(),0,nullptr,values,0,0);
        ID3D11ShaderResourceView* views[]={image.view.Get(),mask};context->PSSetShaderResources(0,2,views);
        context->PSSetShader(isBackground?backgroundShader.Get():composite.Get(),nullptr,0);
        context->Draw(3,0);
        ID3D11ShaderResourceView* clear[2]{};context->PSSetShaderResources(0,2,clear);
    }
};
VideoProcessor::VideoProcessor(std::shared_ptr<GpuDevice> gpu,uint32_t width,uint32_t height):impl_(std::make_unique<Impl>()){
    if(!gpu||!width||!height||width>3840||height>3840||uint64_t(width)*height>3840ULL*2160)throw std::invalid_argument("Invalid output dimensions");
    auto& s=*impl_;s.gpu=std::move(gpu);s.backgroundConverter=std::make_unique<GpuColorConverter>(s.gpu);
    std::lock_guard lock(s.gpu->mutex);s.target=detail::image(*s.gpu,width,height);
    s.exported=detail::image(*s.gpu,width,height);
    detail::hr(s.gpu->device->CreateRenderTargetView(s.exported.texture.Get(),nullptr,&s.exportView),"Export target");
    detail::hr(s.gpu->device->CreateRenderTargetView(s.target.texture.Get(),nullptr,&s.targetView),"Composite target");
    auto vs=detail::compile("Fullscreen","vs_5_0"),ps=detail::compile("Composite","ps_5_0"),bg=detail::compile("Background","ps_5_0");
    detail::hr(s.gpu->device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&s.vertex),"Fullscreen shader");
    detail::hr(s.gpu->device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&s.composite),"Chroma key shader");
    detail::hr(s.gpu->device->CreatePixelShader(bg->GetBufferPointer(),bg->GetBufferSize(),nullptr,&s.backgroundShader),"Background shader");
    auto exported=detail::compile("Export","ps_5_0");detail::hr(s.gpu->device->CreatePixelShader(exported->GetBufferPointer(),exported->GetBufferSize(),nullptr,&s.exportShader),"Alpha export shader");
    D3D11_BUFFER_DESC b{};b.ByteWidth=32;b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    detail::hr(s.gpu->device->CreateBuffer(&b,nullptr,&s.constants),"Filter parameters");
    D3D11_SAMPLER_DESC sampler{};sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sampler.MaxLOD=D3D11_FLOAT32_MAX;
    detail::hr(s.gpu->device->CreateSamplerState(&sampler,&s.sampler),"Composite sampler");
    D3D11_BLEND_DESC blend{};auto& rt=blend.RenderTarget[0];rt.BlendEnable=TRUE;rt.SrcBlend=rt.SrcBlendAlpha=D3D11_BLEND_ONE;rt.DestBlend=rt.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;rt.BlendOp=rt.BlendOpAlpha=D3D11_BLEND_OP_ADD;rt.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    detail::hr(s.gpu->device->CreateBlendState(&blend,&s.blend),"Alpha blending");
    D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;
    detail::hr(s.gpu->device->CreateRasterizerState(&raster,&s.rasterizer),"Composite rasterizer");
}
VideoProcessor::~VideoProcessor()=default;
void VideoProcessor::setOutputSize(uint32_t width,uint32_t height){
    auto& s=*impl_;if(s.target.width==width&&s.target.height==height)return;
    if(!width||!height||width>3840||height>3840||uint64_t(width)*height>3840ULL*2160)throw std::invalid_argument("Invalid output dimensions");
    std::lock_guard lock(s.gpu->mutex);s.targetView.Reset();s.exportView.Reset();
    s.target=detail::image(*s.gpu,width,height);s.exported=detail::image(*s.gpu,width,height);
    detail::hr(s.gpu->device->CreateRenderTargetView(s.target.texture.Get(),nullptr,&s.targetView),"Composite target resize");
    detail::hr(s.gpu->device->CreateRenderTargetView(s.exported.texture.Get(),nullptr,&s.exportView),"Export target resize");
}
const FilterSettings& VideoProcessor::settings()const{return impl_->settings;}
void VideoProcessor::setSettings(const FilterSettings& settings){
    if(!std::isfinite(settings.similarity)||!std::isfinite(settings.softness)||!std::isfinite(settings.spill)||settings.similarity<0||settings.similarity>1||settings.softness<0.001f||settings.softness>1||settings.spill<0||settings.spill>1)throw std::invalid_argument("Invalid chroma key controls");
    if(settings.removal!=impl_->settings.removal)for(auto& [id,source]:impl_->sources){source->previous.reset();if(source->matting)source->matting->reset();}
    impl_->settings=settings;
}
void VideoProcessor::setMattingModel(const std::filesystem::path& model){
    if(!std::filesystem::is_regular_file(model))throw std::runtime_error("Matting model not found");
    impl_->model=model;
    for(auto& [id,source]:impl_->sources){source->matting.reset();source->previous.reset();}
}
void VideoProcessor::clearBackground(){impl_->background={};}
void VideoProcessor::setBackgroundImage(const std::filesystem::path& path){
    const auto initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    struct ComScope{bool active;~ComScope(){if(active)CoUninitialize();}} scope{SUCCEEDED(initialized)};
    ComPtr<IWICImagingFactory> factory;detail::hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)),"WIC image factory");
    ComPtr<IWICBitmapDecoder> decoder;detail::hr(factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder),"Load background image");
    ComPtr<IWICBitmapFrameDecode> frame;detail::hr(decoder->GetFrame(0,&frame),"Read background image");
    UINT width{},height{};frame->GetSize(&width,&height);if(!width||!height)throw std::runtime_error("Background image is empty");
    ComPtr<IWICBitmapScaler> scaler; IWICBitmapSource* source=frame.Get();
    if(width>4096||height>4096){const double scale=4096.0/std::max(width,height);width=std::max(1u,static_cast<UINT>(width*scale));height=std::max(1u,static_cast<UINT>(height*scale));detail::hr(factory->CreateBitmapScaler(&scaler),"Image scaler");detail::hr(scaler->Initialize(frame.Get(),width,height,WICBitmapInterpolationModeFant),"Resize background");source=scaler.Get();}
    ComPtr<IWICFormatConverter> converter;detail::hr(factory->CreateFormatConverter(&converter),"Image RGBA converter");
    detail::hr(converter->Initialize(source,GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom),"Decode image RGBA");
    std::vector<uint8_t> pixels(size_t(width)*height*4);detail::hr(converter->CopyPixels(nullptr,width*4,static_cast<UINT>(pixels.size()),pixels.data()),"Copy background image");
    std::lock_guard lock(impl_->gpu->mutex);impl_->background=detail::image(*impl_->gpu,width,height,DXGI_FORMAT_R8G8B8A8_UNORM,D3D11_BIND_SHADER_RESOURCE);
    impl_->gpu->context->UpdateSubresource(impl_->background.texture.Get(),0,nullptr,pixels.data(),width*4,0);
}
GpuImage VideoProcessor::render(const SceneSnapshot& scene,FramePtr backgroundVideo){
    auto& s=*impl_;
    // Convert/infer before binding the composite target. AI has an explicit reduced-size I/O bridge.
    for(const auto& layer:scene.layers){
        if(!layer.frame)continue;
        auto& source=s.sources[layer.source];if(!source)source=std::make_unique<Impl::Source>(s.gpu);
        if(source->previous!=layer.frame){
            source->rgb=source->converter.convert(*layer.frame);
            if(s.settings.removal==RemovalMode::AiMatting){
                if(!source->matting)source->matting=std::make_unique<RvmMatting>(s.gpu,s.model);
                if(source->previous&&(layer.frame->rotationDegrees!=source->previous->rotationDegrees||layer.frame->sourceTimestamp90k<=source->previous->sourceTimestamp90k||layer.frame->readyAt-source->previous->readyAt>std::chrono::milliseconds(500)))source->matting->reset();
                source->mask=source->matting->infer(source->rgb);
            }
            source->previous=layer.frame;
        }
    }
    GpuImage background=s.background;
    if(backgroundVideo)background=s.backgroundConverter->convert(*backgroundVideo);
    std::lock_guard lock(s.gpu->mutex);auto* context=s.gpu->context.Get();
    auto* target=s.targetView.Get();context->OMSetRenderTargets(1,&target,nullptr);
    context->OMSetBlendState(s.blend.Get(),nullptr,0xffffffff);context->OMSetDepthStencilState(nullptr,0);context->RSSetState(s.rasterizer.Get());
    context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(s.vertex.Get(),nullptr,0);auto* sampler=s.sampler.Get();context->PSSetSamplers(0,1,&sampler);auto* buffer=s.constants.Get();context->PSSetConstantBuffers(0,1,&buffer);
    const float clear[]={s.settings.transparent?0:s.settings.background[0],s.settings.transparent?0:s.settings.background[1],s.settings.transparent?0:s.settings.background[2],s.settings.transparent?0.0f:1.0f};
    context->ClearRenderTargetView(target,clear);
    if(background.view&&!s.settings.transparent)s.draw(background,nullptr,{},true);
    for(const auto& layer:scene.layers)if(layer.frame){auto& source=*s.sources.at(layer.source);s.draw(source.rgb,source.mask.Get(),layer.destination,false);}
    context->OMSetRenderTargets(0,nullptr,nullptr);
    return s.target;
}
GpuImage VideoProcessor::exportFrame(bool preserveAlpha,bool bottomUp){
    auto& s=*impl_;std::lock_guard lock(s.gpu->mutex);auto* context=s.gpu->context.Get();
    auto* target=s.exportView.Get();context->OMSetRenderTargets(1,&target,nullptr);context->OMSetBlendState(nullptr,nullptr,0xffffffff);
    D3D11_VIEWPORT viewport{0,0,static_cast<float>(s.target.width),static_cast<float>(s.target.height),0,1};context->RSSetViewports(1,&viewport);
    const float values[]={bottomUp?1.0f:0.0f,0,0,preserveAlpha?1.0f:0.0f};context->UpdateSubresource(s.constants.Get(),0,nullptr,values,0,0);
    auto* view=s.target.view.Get();context->PSSetShaderResources(0,1,&view);context->PSSetShader(s.exportShader.Get(),nullptr,0);context->Draw(3,0);
    view=nullptr;context->PSSetShaderResources(0,1,&view);context->OMSetRenderTargets(0,nullptr,nullptr);return s.exported;
}
}
