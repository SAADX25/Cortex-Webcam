#include "cortex/tracking/HandTracker.h"
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <iostream>
#include <cmath>
using Microsoft::WRL::ComPtr;
void check(bool value,const char* error){if(!value)throw std::runtime_error(error);}
int main(int argc,char** argv){CoInitializeEx(nullptr,COINIT_MULTITHREADED);try{
    check(argc>=2,"Expected hand models directory and optional image fixture");cortex::HandTracker tracker(argv[1]);
    std::vector<uint8_t> blank(256*256*4,0);check(tracker.track(blank,256,256).empty(),"Blank image must not invent fingers");
    if(argc>2){ComPtr<IWICImagingFactory> factory;check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory))),"WIC");
        ComPtr<IWICBitmapDecoder> decoder;auto path=std::filesystem::path(argv[2]);check(SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder)),"Read hand fixture");
        ComPtr<IWICBitmapFrameDecode> frame;decoder->GetFrame(0,&frame);UINT w{},h{};frame->GetSize(&w,&h);
        ComPtr<IWICFormatConverter> convert;factory->CreateFormatConverter(&convert);check(SUCCEEDED(convert->Initialize(frame.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)),"RGBA fixture");
        std::vector<uint8_t> rgba(size_t(w)*h*4);check(SUCCEEDED(convert->CopyPixels(nullptr,w*4,static_cast<UINT>(rgba.size()),rgba.data())),"Fixture pixels");
        auto start=std::chrono::steady_clock::now();auto hands=tracker.track(rgba,w,h);
        std::cout<<"Hands="<<hands.size()<<" inference_ms="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<<'\n';
        check(!hands.empty(),"Official hand fixture must produce a measured hand");
        for(auto& hand:hands)for(auto p:hand.world)check(std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&p.confidence>=.8f,"All 21 hand points must be finite and measured");
    }
    std::cout<<"PASS: MediaPipe Hands real ONNX inference\n";CoUninitialize();return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';CoUninitialize();return 1;}}
