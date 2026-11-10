#include "cortex/tracking/HandTracker.h"
#include "cortex/tracking/HandIdentityTracker.h"
#include "cortex/tracking/HandVisibilityGate.h"
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <iostream>
#include <cmath>
using Microsoft::WRL::ComPtr;
void check(bool value,const char* error){if(!value)throw std::runtime_error(error);}

static void checkHandAssociation() {
    using namespace cortex;
    using namespace std::chrono;
    HandIdentityTracker identities;
    HandVisibilityGate visibility;
    const auto start = steady_clock::now();
    std::array<Joint3D, 33> body{};
    std::array<HandLandmarks, 2> detections{};
    detections[0].confidence = detections[1].confidence = 1;
    detections[0].rightProbability = .05f;
    detections[1].rightProbability = .95f;
    for (int step = 0; step <= 30; ++step) {
        detections[0].image[0] = {200.f + step * 7, 300, 0, 1};
        detections[1].image[0] = {440.f - step * 7, 300, 0, 1};
        for (auto& hand : detections)
            for (int joint : {5, 9, 17})
                hand.image[joint] = {hand.image[0].x + joint, 280, 0, 1};
        auto unordered = detections;
        if (step % 2)
            std::swap(unordered[0], unordered[1]);
        const auto now = start + milliseconds(step * 16);
        auto assigned = identities.update(unordered, body, 640, 480, now);
        check(assigned[0].image[0].x == detections[0].image[0].x &&
              assigned[1].image[0].x == detections[1].image[0].x,
              "Crossing hands and changes in detector order must preserve identity");
        visibility.update(assigned, 640, 480, now);
        if (step == 0)
            check(assigned[0].confidence == 0 && assigned[1].confidence == 0,
                  "A single detection must not activate a hand");
        if (step == 30)
            check(assigned[0].confidence >= .8f && assigned[1].confidence >= .8f,
                  "Sustained detections must activate both hands");
    }
    const auto single = identities.update(std::span(&detections[1], 1), body, 640, 480,
                                           start + milliseconds(500));
    check(single[0].confidence == 0 && single[1].confidence >= .8f,
          "One hidden hand must not duplicate the remaining detection");
}

int main(int argc,char** argv){CoInitializeEx(nullptr,COINIT_MULTITHREADED);try{
    checkHandAssociation();
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
