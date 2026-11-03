#include "cortex/filters/VideoProcessor.h"
#include "cortex/output/UnityCameraOutput.h"
#include "cortex/filters/BackgroundVideo.h"
#include "cortex/filters/GpuImageTransform.h"
#include <iostream>
#include <fstream>
#include <atomic>
#include <thread>
#include <cstring>
#include "shared.inl"
using namespace cortex;
namespace {
void check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
FramePtr nv12(std::shared_ptr<GpuDevice> gpu,uint8_t y,uint8_t u,uint8_t v){
    static std::atomic<int64_t> timestamp{0};
    auto frame=std::make_shared<VideoFrame>();frame->gpu=gpu;frame->width=frame->height=64;frame->matrix=VideoFrame::Matrix::Bt709;frame->range=VideoFrame::Range::Limited;frame->readyAt=frame->receivedAt=std::chrono::steady_clock::now();frame->sourceTimestamp90k=timestamp.fetch_add(3000);
    std::vector<uint8_t> plane(64*64*3/2,y);for(size_t i=64*64;i<plane.size();i+=2){plane[i]=u;plane[i+1]=v;}
    D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=64;desc.MipLevels=1;desc.ArraySize=2;desc.Format=DXGI_FORMAT_NV12;desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_DECODER;
    std::vector<uint8_t> first(plane.size(),128);D3D11_SUBRESOURCE_DATA data[]={{first.data(),64,0},{plane.data(),64,0}};
    check(SUCCEEDED(gpu->device->CreateTexture2D(&desc,data,&frame->texture)),"Create NV12 fixture");frame->arraySlice=1;return frame;
}
std::vector<uint8_t> read(std::shared_ptr<GpuDevice> gpu,const GpuImage& image){
    std::lock_guard lock(gpu->mutex);D3D11_TEXTURE2D_DESC desc{};image.texture->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;check(SUCCEEDED(gpu->device->CreateTexture2D(&desc,nullptr,&staging)),"Readback fixture");gpu->context->CopyResource(staging.Get(),image.texture.Get());D3D11_MAPPED_SUBRESOURCE map{};check(SUCCEEDED(gpu->context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map)),"Map fixture");
    std::vector<uint8_t> bytes(size_t(image.width)*image.height*4);for(UINT y=0;y<image.height;++y)std::memcpy(bytes.data()+size_t(y)*image.width*4,static_cast<const uint8_t*>(map.pData)+size_t(y)*map.RowPitch,image.width*4);gpu->context->Unmap(staging.Get(),0);return bytes;
}
SceneSnapshot scene(FramePtr frame){return {{{SourceId::Phone,std::move(frame),{}}}};}
}
int main(int argc,char** argv){try{
    auto gpu=GpuDevice::create();VideoProcessor processor(gpu,64,64);FilterSettings settings;settings.transparent=true;processor.setSettings(settings);
    {
        const uint8_t pixels[]={255,0,0,255,255,0,0,255,0,255,0,255,0,255,0,255,
            0,0,255,255,0,0,255,255,255,255,0,255,255,255,0,255};
        GpuImage quadrants;quadrants.width=4;quadrants.height=2;
        D3D11_TEXTURE2D_DESC desc{};desc.Width=4;desc.Height=2;desc.MipLevels=1;desc.ArraySize=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial{pixels,16,0};check(SUCCEEDED(gpu->device->CreateTexture2D(&desc,&initial,&quadrants.texture)),"Create rotation fixture");
        check(SUCCEEDED(gpu->device->CreateShaderResourceView(quadrants.texture.Get(),nullptr,&quadrants.view)),"Rotation fixture view");
        GpuImageTransform rotate(gpu);
        auto clockwise=rotate.rotate(quadrants,90);auto bytes=read(gpu,clockwise);
        check(clockwise.width==2&&clockwise.height==4&&bytes[2]>240&&bytes[0]<10,"90 degree rotation must move blue to top-left and swap dimensions");
        check(bytes[4]>240&&bytes[5]<10&&bytes[6]<10,"90 degree rotation must move red to top-right");
        auto flipped=read(gpu,rotate.rotate(quadrants,180));check(flipped[0]>240&&flipped[1]>240&&flipped[2]<10,"180 degree rotation moves yellow to top-left");
        auto counter=read(gpu,rotate.rotate(quadrants,270));check(counter[1]>240&&counter[0]<10,"270 degree rotation moves green to top-left");
    }
    auto green=nv12(gpu,173,42,26),red=nv12(gpu,63,102,240),blue=nv12(gpu,32,240,118);
    auto keyed=read(gpu,processor.render(scene(green)));check(keyed[3]<10,"Green must become transparent");
    auto foreground=read(gpu,processor.render(scene(red)));check(foreground[0]>220&&foreground[1]<30&&foreground[3]>240,"Red foreground and array slice must survive");
    processor.render(scene(green));auto opaque=read(gpu,processor.exportFrame(false));check(opaque[3]==255&&opaque[0]<10,"Virtual camera default must flatten alpha");
    auto alpha=read(gpu,processor.exportFrame(true));check(alpha[3]<10,"Optional alpha export must preserve transparency");
    auto edge=nv12(gpu,144,81,45);auto premultiplied=read(gpu,processor.render(scene(edge)));auto straight=read(gpu,processor.exportFrame(true));
    check(premultiplied[3]>10&&premultiplied[3]<245&&straight[2]>premultiplied[2]+5,"Partial-alpha edges must be exported as straight RGBA");
    settings.transparent=false;settings.background[2]=1;processor.setSettings(settings);auto replaced=read(gpu,processor.render(scene(green)));check(replaced[2]>240&&replaced[3]==255,"Custom background color must replace green");
    // A tiny blue BMP exercises WIC loading and GPU image compositing.
    const auto bmp=std::filesystem::temp_directory_path()/"cortex-processing-test-background.bmp";
    const uint8_t bitmap[]={0x42,0x4d,58,0,0,0,0,0,0,0,54,0,0,0,40,0,0,0,1,0,0,0,1,0,0,0,1,0,24,0,0,0,0,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,0,0,0};
    {std::ofstream file(bmp,std::ios::binary);file.write(reinterpret_cast<const char*>(bitmap),sizeof(bitmap));}
    processor.setBackgroundImage(bmp);auto imageBackground=read(gpu,processor.render(scene(green)));check(imageBackground[2]>240&&imageBackground[0]<10,"Custom image must replace green");processor.clearBackground();std::filesystem::remove(bmp);
    {
        auto large=std::filesystem::temp_directory_path()/"cortex-large-background.bmp";
        BITMAPFILEHEADER fileHeader{}; fileHeader.bfType=0x4d42; fileHeader.bfOffBits=sizeof(BITMAPFILEHEADER)+sizeof(BITMAPINFOHEADER); fileHeader.bfSize=fileHeader.bfOffBits+5000*4;
        BITMAPINFOHEADER info{}; info.biSize=sizeof(info); info.biWidth=5000; info.biHeight=1; info.biPlanes=1; info.biBitCount=32;
        std::vector<uint8_t> row(5000*4); for(size_t i=0;i<row.size();i+=4){row[i]=255;row[i+3]=255;}
        {std::ofstream file(large,std::ios::binary);file.write(reinterpret_cast<char*>(&fileHeader),sizeof(fileHeader));file.write(reinterpret_cast<char*>(&info),sizeof(info));file.write(reinterpret_cast<char*>(row.data()),row.size());}
        processor.setBackgroundImage(large); auto resized=read(gpu,processor.render(scene(green)));
        check(resized[2]>240,"Oversized backgrounds must be resized and composited");processor.clearBackground();std::filesystem::remove(large);
    }
    settings.removal=RemovalMode::None;processor.setSettings(settings);
    SceneSnapshot pip{{{SourceId::Phone,red,{}},{SourceId::UsbWebcam,blue,{0.5f,0.5f,0.5f,0.5f}}}};
    auto composed=read(gpu,processor.render(pip));check(composed[0]>220&&composed[(48*64+48)*4+2]>220,"GPU PiP must draw inset over main source");
    SceneSnapshot rows{{{SourceId::Phone,red,{0,0,1,0.5f}},{SourceId::UsbWebcam,blue,{0,0.5f,1,0.5f}}}};
    auto upright=read(gpu,processor.render(rows));auto bottomUp=read(gpu,processor.exportFrame(false,true));
    const auto top=(16*64+32)*4,bottom=(48*64+32)*4;
    check(upright[top]>220&&upright[bottom+2]>220,"Preview must keep the red top and blue bottom");
    check(bottomUp[top+2]>220&&bottomUp[bottom]>220,"UnityCapture export must reverse rows for bottom-up DirectShow delivery");
    auto topDown=read(gpu,processor.exportFrame());
    check(topDown[top]>220&&topDown[bottom+2]>220,"Default exports must retain top-down orientation");
    UnityCameraOutput output(gpu,40);std::atomic<bool> received=false;
    // Webcam clients negotiate their own capture resolution. Require the resize
    // contract as well as valid pixels; otherwise Discord displays a test pattern
    // when it requests 720p while Studio sends 1080p.
    std::jthread consumer([&](std::stop_token stop){SharedImageMemory receiver(40);while(!stop.stop_requested()){receiver.Receive([](int w,int h,int stride,SharedImageMemory::EFormat f,SharedImageMemory::EResizeMode resize,SharedImageMemory::EMirrorMode,int,uint8_t* pixels,void* context){if(w==64&&h==64&&stride>=64&&f==SharedImageMemory::FORMAT_UINT8&&resize==SharedImageMemory::RESIZEMODE_LINEAR&&pixels[0]>220)static_cast<std::atomic<bool>*>(context)->store(true);},&received);std::this_thread::sleep_for(std::chrono::milliseconds(2));}});
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!received&&std::chrono::steady_clock::now()<deadline){processor.render(scene(red));output.submit(processor.exportFrame());std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    consumer.request_stop();consumer.join();check(received,"UnityCapture receiver must receive RGBA pixels with client-resolution scaling enabled");
    processor.setOutputSize(64,96);auto portrait=processor.render(scene(red));auto portraitPixels=read(gpu,portrait);
    check(portrait.width==64&&portrait.height==96&&portraitPixels[(48*64+32)*4]>220,"Portrait output must preserve the foreground without stretching");
    processor.setOutputSize(64,64);
    if(argc>1&&std::string(argv[1])!="-"){processor.setMattingModel(std::filesystem::path(argv[1]));settings.removal=RemovalMode::AiMatting;settings.transparent=true;processor.setSettings(settings);for(int i=0;i<5;++i){auto result=read(gpu,processor.render(scene(nv12(gpu,63,102,240))));check(result.size()==64*64*4,"RVM output image");}std::cout<<"RVM DirectML recurrent inference passed"<<std::endl;}
    if(argc>2){BackgroundVideo video(gpu,std::filesystem::path(argv[2]));const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(5);FramePtr frame;
        while(!frame&&video.error().empty()&&std::chrono::steady_clock::now()<until){frame=video.latestFrame();std::this_thread::sleep_for(std::chrono::milliseconds(10));}
        if(!video.error().empty())throw std::runtime_error(video.error());check(bool(frame),"GPU background video must produce a frame");settings.removal=RemovalMode::None;settings.transparent=false;processor.setSettings(settings);auto pixels=read(gpu,processor.render({},frame));check(pixels[2]>150,"Decoded background video must be composited");
        bool looped=false;int64_t previous=frame->sourceTimestamp90k;const auto loopDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while(std::chrono::steady_clock::now()<loopDeadline&&!looped){auto next=video.latestFrame();if(next){looped=next->sourceTimestamp90k<previous;previous=next->sourceTimestamp90k;}if(!video.error().empty())throw std::runtime_error(video.error());std::this_thread::sleep_for(std::chrono::milliseconds(10));}
        check(looped,"Background playback must loop and reset its decode state");std::cout<<"FFmpeg D3D11VA video background and looping passed\n";
    }
    std::cout<<"GPU NV12 conversion, chroma/alpha/background, PiP, and UnityCapture IPC passed\n";return 0;
}catch(const std::exception& e){std::cout<<"Processing test failed: "<<e.what()<<std::endl;return 1;}}
