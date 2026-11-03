#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "cortex/ui/StudioBridge.h"
#include <chrono>
#include <thread>
#include <iostream>
#include <stdexcept>
#include <functional>
#include <atomic>
#include <string>
using Clock=std::chrono::steady_clock;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void pump(){MSG message;while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}}
bool until(const std::function<bool()>& predicate,int seconds=12){
    const auto end=Clock::now()+std::chrono::seconds(seconds);
    while(Clock::now()<end){pump();if(predicate())return true;std::this_thread::sleep_for(std::chrono::milliseconds(25));}
    return false;
}
struct Socket {
    SOCKET value=INVALID_SOCKET;uint16_t port{};
    Socket(){
        value=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);require(value!=INVALID_SOCKET,"Create test UDP socket");
        BOOL exclusive=TRUE;setsockopt(value,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<char*>(&exclusive),sizeof(exclusive));
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        require(bind(value,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"Reserve test port");
        int bytes=sizeof(address);require(getsockname(value,reinterpret_cast<sockaddr*>(&address),&bytes)==0,"Get test port");port=ntohs(address.sin_port);
    }
    void close(){if(value!=INVALID_SOCKET){closesocket(value);value=INVALID_SOCKET;}}
    ~Socket(){close();}
};
struct Fixture {
    HWND window{};void* engine{};
    Fixture(){
        // A hidden native window is only the GPU component's render target; no
        // user application, focus or desktop input is read or manipulated.
        window=CreateWindowExW(0,L"STATIC",L"Studio native integration fixture",WS_OVERLAPPEDWINDOW,0,0,640,360,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        require(window!=nullptr,"Create fixture window");engine=cortex_studio_create(window,L"");require(engine!=nullptr,"Create engine");
    }
    CortexStudioState snapshot(){CortexStudioState state;require(cortex_studio_get_state(engine,&state,sizeof(state))!=0,"Read native state");return state;}
    void ports(uint16_t video,uint16_t control){cortex_studio_set(engine,12,double(video)+65536.0*control);}
    ~Fixture(){if(engine){
        // Keep processing window messages while the GPU worker tears down.
        bool done=false;std::atomic<bool> finished=false;
        std::thread closer([&]{cortex_studio_destroy(engine);finished=true;});
        while(!done){pump();done=finished;std::this_thread::sleep_for(std::chrono::milliseconds(5));}closer.join();
    }if(window)DestroyWindow(window);}
};
bool ready(uint16_t control,uint16_t video){
    Socket socket;DWORD timeout=80;setsockopt(socket.value,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout));
    sockaddr_in peer{};peer.sin_family=AF_INET;peer.sin_addr.s_addr=htonl(INADDR_LOOPBACK);peer.sin_port=htons(control);
    const std::string nonce="0123456789abcdef0123456789abcdef";
    const std::string request="CORTEX/1 HELLO "+nonce+" H264 1920 1080 30\n";
    sendto(socket.value,request.data(),static_cast<int>(request.size()),0,reinterpret_cast<sockaddr*>(&peer),sizeof(peer));
    char buffer[512]{};const auto bytes=recv(socket.value,buffer,sizeof(buffer),0);
    return bytes>0&&std::string(buffer,bytes)=="CORTEX/1 READY "+nonce+" "+std::to_string(video)+" H264 1920 1080 30\n";
}
std::string synced(uint16_t control,int fps,uint64_t acknowledged){
    Socket socket;DWORD timeout=150;setsockopt(socket.value,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout));
    sockaddr_in peer{};peer.sin_family=AF_INET;peer.sin_addr.s_addr=htonl(INADDR_LOOPBACK);peer.sin_port=htons(control);
    const std::string request="CORTEX/2 HELLO 0123456789abcdef0123456789abcdef H264 1920 1080 "+std::to_string(fps)+" "+std::to_string(acknowledged)+"\n";
    sendto(socket.value,request.data(),static_cast<int>(request.size()),0,reinterpret_cast<sockaddr*>(&peer),sizeof(peer));
    char buffer[512]{};const auto bytes=recv(socket.value,buffer,sizeof(buffer),0);
    return bytes>0?std::string(buffer,bytes):"";
}
int main(){WSADATA data;WSAStartup(MAKEWORD(2,2),&data);try{
    Socket occupiedVideo,availableControl;const auto video=occupiedVideo.port,control=availableControl.port;availableControl.close();
    Fixture studio;studio.ports(video,control);
    require(until([&]{return std::wstring(studio.snapshot().error).find(L"Bind UDP")!=std::wstring::npos;}),"Expected video bind conflict");
    occupiedVideo.close();
    require(until([&]{return ready(control,video);}),"Receiver must recover when the video port becomes free without Start");
    require(studio.snapshot().running!=0,"Receiver is automatically armed");
    cortex_studio_set(studio.engine,1,0); // Legacy Stop must not leave new builds permanently paused.
    require(until([&]{return studio.snapshot().peer[0]==0;}),"Legacy reconnect must release its previous peer");
    require(until([&]{return ready(control,video);}),"Legacy Stop must reconnect automatically");
    Socket occupiedControl;const auto replacement=occupiedControl.port;studio.ports(video,replacement);
    require(until([&]{return std::wstring(studio.snapshot().error).find(L"bind discovery")!=std::wstring::npos;}),"Expected control port conflict");
    occupiedControl.close();
    require(until([&]{return ready(replacement,video);}),"Discovery must recover when the control port becomes free without a button");
    cortex_studio_set(studio.engine,13,1);
    require(until([&]{return studio.snapshot().peer[0]==0;}),"Reconnect must release its previous peer");
    require(until([&]{return ready(replacement,video);}),"Optional reconnect must automatically return to ready");
    cortex_studio_set(studio.engine,14,60);
    require(until([&]{return studio.snapshot().frameRateRequestId!=0;}),"Frame-rate request must enter native state");
    const auto request=studio.snapshot().frameRateRequestId;
    require(studio.snapshot().outputFps==30,"Windows must not claim the phone has switched before acknowledgement");
    require(until([&]{return synced(replacement,30,0).find("1080 60 "+std::to_string(request))!=std::string::npos;}),"Phone must receive Windows' requested rate");
    require(studio.snapshot().frameRatePending!=0,"Rate remains pending before phone acknowledgement");
    require(until([&]{synced(replacement,60,request);auto s=studio.snapshot();return s.phoneSyncAvailable&&s.phoneFps==60&&s.outputFps==60&&!s.frameRatePending;}),"Confirmed phone FPS must synchronize Windows output");
    require(until([&]{synced(replacement,30,request);return studio.snapshot().outputFps==30;}),"A later phone-side change must synchronize back to Windows");
    require(studio.snapshot().phoneRateFallback==0,"A user change on the phone must not be labeled a hardware fallback");
    require(until([&]{return synced(replacement,30,0).find("1080 30 0")!=std::string::npos;}),"Completed commands must be retired so reconnecting cannot reapply an old setting");
    cortex_studio_set(studio.engine,14,60);
    require(until([&]{return studio.snapshot().frameRateRequestId!=request;}),"New command must have a new acknowledgement id");
    const auto unsupported=studio.snapshot().frameRateRequestId;
    require(until([&]{synced(replacement,30,unsupported);auto s=studio.snapshot();return !s.frameRatePending&&s.phoneFps==30&&s.outputFps==30&&s.phoneRateFallback;}),"Phone capability fallback must not loop or pretend 60 fps");
    std::cout<<"PASS: auto-connect recovery and acknowledged two-way frame-rate synchronization, including capability fallback\n";
    WSACleanup();return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<"\n";WSACleanup();return 1;}}
