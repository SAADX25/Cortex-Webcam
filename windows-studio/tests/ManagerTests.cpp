#include "cortex/sources/VideoSourceManager.h"
#include <iostream>
#include <stdexcept>

using namespace cortex;
namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct FakeSource:IVideoSource {
    std::shared_ptr<GpuDevice> gpu;
    FramePtr frame;
    bool started{};
    explicit FakeSource(std::shared_ptr<GpuDevice> device):gpu(std::move(device)) {
        auto f=std::make_shared<VideoFrame>();f->receivedAt=std::chrono::steady_clock::now();frame=f;
    }
    void start() override {started=true;}
    void stop() noexcept override {started=false;}
    FramePtr latestFrame() const override {return frame;}
    SourceStatus status() const override {return {};}
    std::shared_ptr<GpuDevice> gpuDevice() const override {return gpu;}
};
}
int main(){try{
    // No physical GPU is used: manager tests exercise routing and lifetime contracts.
    auto gpu=std::make_shared<GpuDevice>();
    auto phone=std::make_shared<FakeSource>(gpu),usb=std::make_shared<FakeSource>(gpu);
    VideoSourceManager manager(gpu);manager.add(SourceId::Phone,phone);manager.add(SourceId::UsbWebcam,usb);manager.startAll();
    require(phone->started&&usb->started,"both sources must remain warm");
    require(manager.snapshot().layers[0].source==SourceId::Phone,"phone default");
    manager.select(SourceId::UsbWebcam);manager.setPictureInPicture(SourceId::Phone);
    auto scene=manager.snapshot();require(scene.layers.size()==2,"two PiP layers");
    require(scene.layers[0].frame==usb->frame&&scene.layers[1].frame==phone->frame,"switch sources without restarting capture");
    manager.setPictureInPicture(SourceId::UsbWebcam);require(manager.snapshot().layers.size()==1,"no duplicate self PiP");
    auto old=std::make_shared<VideoFrame>();old->receivedAt=std::chrono::steady_clock::now()-std::chrono::seconds(1);usb->frame=old;
    require(manager.snapshot().layers.empty(),"stale sources must not silently freeze forever");
    bool rejected=false;try{manager.setPictureInPicture(SourceId::Phone,{-1,0,1,1});}catch(const std::invalid_argument&){rejected=true;}
    require(rejected,"invalid PiP rectangle");
    rejected=false;try{manager.add(SourceId::Phone,std::make_shared<FakeSource>(std::make_shared<GpuDevice>()));}catch(const std::invalid_argument&){rejected=true;}
    require(rejected,"sources must share GPU");manager.stopAll();require(!phone->started&&!usb->started,"stop lifecycle");
    std::cout<<"Source manager routing tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
