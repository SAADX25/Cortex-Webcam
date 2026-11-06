#include "cortex/transport/UdpH264Source.h"
#include "cortex/discovery/StudioDiscovery.h"
#include "cortex/avatar/AvatarFilter.h"
#include <windows.h>
#include <atomic>
#include <iostream>
#include <thread>
namespace {std::atomic<bool> quit{};BOOL WINAPI stop(DWORD code){if(code==CTRL_C_EVENT||code==CTRL_BREAK_EVENT){quit=true;return TRUE;}return FALSE;}}
int wmain(int argc,wchar_t** argv){try{
    unsigned seconds{};std::wstring port=L"39539";bool diagnostics=false;
    for(int i=1;i<argc;++i){std::wstring arg=argv[i];
        if(arg==L"--diagnostics")diagnostics=true;
        else if(arg==L"--seconds"&&i+1<argc)seconds=std::stoul(argv[++i]);
        else if(arg==L"--motion-port"&&i+1<argc){port=argv[++i];size_t used{};auto value=std::stoul(port,&used);if(used!=port.size()||value==0||value>65535)throw std::invalid_argument("Invalid motion port");}
        else if(arg==L"--help"){std::cout<<"cortex-motion [--diagnostics] [--seconds N] [--motion-port 39539]\nClose Cortex Studio first (both use phone ports 5004/5005). Ctrl+C stops.\n";return 0;}
        else throw std::invalid_argument("Unknown motion option");}
    SetEnvironmentVariableW(L"CORTEX_VRM_PORT",port.c_str());
    SetEnvironmentVariableW(L"CORTEX_HAND_DIAGNOSTICS",diagnostics?L"1":L"0");
    auto gpu=cortex::GpuDevice::create();
    auto tracker=cortex::makeFaceTracker(gpu);tracker->setBodyTracking(true);
    auto phone=std::make_shared<cortex::UdpH264Source>(gpu);phone->start();
    cortex::StudioDiscovery discovery(5004,5005,"",[phone]{return phone->status().state==cortex::SourceStatus::State::Running;},[phone](const std::string& peer){phone->setAutoPeer(peer);});
    SetConsoleCtrlHandler(stop,TRUE);auto start=std::chrono::steady_clock::now(),log=start;
    std::cout<<"Cortex motion: phone UDP 5004/5005 -> 127.0.0.1:"<<std::stoul(port)<<". No images recorded.\n";
    while(!quit&&(!seconds||std::chrono::steady_clock::now()-start<std::chrono::seconds(seconds))){
        if(phone->status().state==cortex::SourceStatus::State::Failed)throw std::runtime_error("Phone receiver failed; close Cortex Studio and retry");
        if(auto frame=phone->latestFrame())tracker->track(*frame);
        if(std::chrono::steady_clock::now()>=log){std::cout<<tracker->status()<<'\n';log=std::chrono::steady_clock::now()+std::chrono::seconds(1);}
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    phone->stop();return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
