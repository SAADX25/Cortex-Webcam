#include "cortex/output/UnityCameraOutput.h"
#include "../filters/GpuHelpers.h"
#include <array>
#include <cstring>
#include "shared.inl"
namespace cortex {
struct UnityCameraOutput::Impl {
    struct Slot{ComPtr<ID3D11Texture2D> staging;ComPtr<ID3D11Query> ready;bool pending{};uint64_t serial{};};
    std::shared_ptr<GpuDevice> gpu;SharedImageMemory shared;
    std::array<Slot,3> slots;
    uint32_t width{},height{};uint64_t serial{};OutputStats counters;
    Impl(std::shared_ptr<GpuDevice> device,int number):gpu(std::move(device)),shared(number){}
};
UnityCameraOutput::UnityCameraOutput(std::shared_ptr<GpuDevice> gpu,int device):impl_(std::make_unique<Impl>(std::move(gpu),device)){
    if(!impl_->gpu||device<0||device>SharedImageMemory::MAX_CAPNUM)throw std::invalid_argument("Invalid UnityCapture device or GPU");
}
UnityCameraOutput::~UnityCameraOutput()=default;
OutputStats UnityCameraOutput::stats()const{return impl_->counters;}
void UnityCameraOutput::submit(const GpuImage& image){
    auto& s=*impl_;
    if(!image.texture)throw std::invalid_argument("No virtual camera texture");
    ComPtr<ID3D11Device> owner;image.texture->GetDevice(&owner);if(owner.Get()!=s.gpu->device.Get())throw std::invalid_argument("Virtual camera texture uses a different GPU device");
    if(!s.shared.SendIsReady()){++s.counters.inactive;return;}
    D3D11_TEXTURE2D_DESC desc{};image.texture->GetDesc(&desc);
    if(desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM||!desc.Width||!desc.Height||desc.Width>3840||desc.Height>3840||uint64_t(desc.Width)*desc.Height>3840ULL*2160)throw std::invalid_argument("UnityCapture requires RGBA8 up to 4K area");
    Impl::Slot* available=nullptr;Impl::Slot* newest=nullptr;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    {
        std::lock_guard lock(s.gpu->mutex);
        if(s.width!=desc.Width||s.height!=desc.Height){
            s.width=desc.Width;s.height=desc.Height;
            for(auto& slot:s.slots){
                slot={};auto staging=desc;staging.BindFlags=staging.MiscFlags=0;staging.Usage=D3D11_USAGE_STAGING;staging.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                detail::hr(s.gpu->device->CreateTexture2D(&staging,nullptr,&slot.staging),"Virtual camera readback texture");
                D3D11_QUERY_DESC query{D3D11_QUERY_EVENT,0};detail::hr(s.gpu->device->CreateQuery(&query,&slot.ready),"Virtual camera fence");
            }
        }
        for(auto& slot:s.slots){
            if(slot.pending){
                const auto result=s.gpu->context->GetData(slot.ready.Get(),nullptr,0,D3D11_ASYNC_GETDATA_DONOTFLUSH);
                detail::hr(result,"Virtual camera fence query");
                if(result==S_OK){slot.pending=false;if(newest)++s.counters.skipped;if(!newest||slot.serial>newest->serial)newest=&slot;}
            }
        }
        if(newest){
            const auto result=s.gpu->context->Map(newest->staging.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
            if(result==DXGI_ERROR_WAS_STILL_DRAWING){newest->pending=true;newest=nullptr;++s.counters.skipped;}
            else detail::hr(result,"Virtual camera staging map");
        }
        for(auto& slot:s.slots)if(!slot.pending && &slot!=newest){available=&slot;break;}
    }
    if(newest){
        // The receiving app chooses its capture size independently of Studio.
        // Let UnityCapture resample to that size instead of its mismatch pattern.
        const auto result=s.shared.Send(static_cast<int>(s.width),static_cast<int>(s.height),static_cast<int>(mapped.RowPitch/4),mapped.RowPitch*s.height,
            SharedImageMemory::FORMAT_UINT8,SharedImageMemory::RESIZEMODE_LINEAR,SharedImageMemory::MIRRORMODE_DISABLED,1000,static_cast<const uint8_t*>(mapped.pData));
        {std::lock_guard lock(s.gpu->mutex);s.gpu->context->Unmap(newest->staging.Get(),0);}
        if(result==SharedImageMemory::SENDRES_TOOLARGE)throw std::runtime_error("UnityCapture shared frame exceeds receiver buffer");
        if(result==SharedImageMemory::SENDRES_OK)++s.counters.sent;else ++s.counters.skipped;
    }
    if(!available){++s.counters.skipped;return;}
    std::lock_guard lock(s.gpu->mutex);
    s.gpu->context->CopyResource(available->staging.Get(),image.texture.Get());s.gpu->context->End(available->ready.Get());s.gpu->context->Flush();
    available->pending=true;available->serial=++s.serial;
}
}
