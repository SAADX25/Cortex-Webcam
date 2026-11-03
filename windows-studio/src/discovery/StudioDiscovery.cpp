#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windns.h>
#include "cortex/discovery/StudioDiscovery.h"
#include "cortex/discovery/HandshakeProtocol.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <array>
#include <stdexcept>
#include <sstream>
#include <algorithm>

namespace cortex {
namespace {
// Async DNS callbacks retain their request/state, including after a timed-out
// registration or owner destruction. Late successful registrations are removed.
struct Advertisement:std::enable_shared_from_this<Advertisement> {
    DNS_SERVICE_INSTANCE* instance{};
    DNS_SERVICE_REGISTER_REQUEST request{};
    std::mutex mutex;
    std::condition_variable changed;
    bool pending{},registered{},stopping{},finished{};
    DWORD result{DNS_REQUEST_PENDING};
    struct Callback {std::shared_ptr<Advertisement> state;bool remove;};
    ~Advertisement(){if(instance)DnsServiceFreeInstance(instance);}
    static void WINAPI complete(DWORD status,void* context,DNS_SERVICE_INSTANCE* returned) {
        std::unique_ptr<Callback> callback(static_cast<Callback*>(context));
        auto state=callback->state;
        if(returned)DnsServiceFreeInstance(returned);
        bool remove=false;
        {
            std::lock_guard lock(state->mutex);
            state->pending=false;state->result=status;state->finished=true;
            state->registered=!callback->remove&&status==ERROR_SUCCESS;
            remove=state->stopping&&state->registered;
            if(remove){state->pending=true;state->registered=false;}
        }
        state->changed.notify_all();
        if(remove)state->issue(true);
    }
    void issue(bool remove) {
        auto* callback=new Callback{shared_from_this(),remove};
        {
            std::lock_guard lock(mutex);
            pending=true;finished=false;request.pQueryContext=callback;
        }
        const auto resultCode=remove?DnsServiceDeRegister(&request,nullptr):DnsServiceRegister(&request,nullptr);
        if(resultCode!=DNS_REQUEST_PENDING)complete(resultCode,callback,nullptr);
    }
    void stop() {
        bool remove;
        {std::lock_guard lock(mutex);stopping=true;remove=registered&&!pending;if(remove){pending=true;registered=false;}}
        if(remove)issue(true);
    }
};
std::shared_ptr<Advertisement> advertise(uint16_t controlPort,uint16_t videoPort) {
    wchar_t host[256]{};DWORD length=256;
    if(!GetComputerNameExW(ComputerNameDnsHostname,host,&length))throw std::runtime_error("Cannot determine mDNS hostname");
    std::wstring hostName=std::wstring(host)+L".local";
    // Process suffix avoids collisions between simultaneously running instances.
    std::wstring name=L"Cortex-"+std::wstring(host).substr(0,30)+L"-"+std::to_wstring(GetCurrentProcessId())+L"._cortexcam._udp.local";
    const std::wstring video=std::to_wstring(videoPort);
    const wchar_t* keys[]={L"v",L"codec",L"transport",L"video"};
    const wchar_t* values[]={L"1",L"h264",L"rtp",video.c_str()};
    auto state=std::make_shared<Advertisement>();
    state->instance=DnsServiceConstructInstance(name.c_str(),hostName.c_str(),nullptr,nullptr,controlPort,0,0,4,keys,values);
    if(!state->instance)throw std::runtime_error("Cannot construct mDNS service");
    state->request.Version=DNS_QUERY_REQUEST_VERSION1;
    state->request.pServiceInstance=state->instance;
    state->request.pRegisterCompletionCallback=Advertisement::complete;
    state->request.unicastEnabled=FALSE;
    state->issue(false);
    std::unique_lock lock(state->mutex);
    const bool done=state->changed.wait_for(lock,std::chrono::seconds(5),[&]{return state->finished;});
    if(!done||state->result!=ERROR_SUCCESS) {
        const auto error=state->result;lock.unlock();state->stop();
        throw std::runtime_error("mDNS registration failed/timed out: "+std::to_string(error));
    }
    return state;
}
}
struct StudioDiscovery::Impl {
    SOCKET socket{INVALID_SOCKET};
    bool winsock{};
    std::atomic<bool> stopping{false};
    std::thread worker;
    std::shared_ptr<Advertisement> advertisement;
    mutable std::mutex mutex;
    std::string detail{"Starting discovery"};
    ~Impl(){
        if(advertisement)advertisement->stop();
        stopping=true;
        if(worker.joinable())worker.join();
        if(socket!=INVALID_SOCKET)closesocket(socket);
        if(winsock)WSACleanup();
    }
};
StudioDiscovery::StudioDiscovery(uint16_t videoPort,uint16_t controlPort,std::string allowedPeer,
    std::function<bool()> ready,std::function<void(const std::string&)> leaseChanged,
    std::function<std::pair<uint64_t,uint64_t>()> videoStats,std::function<FrameRateRequest()> frameRateRequest,
    std::function<void(FrameRateReport)> frameRateReported):impl_(std::make_unique<Impl>()) {
    if(!videoPort||!controlPort||videoPort==controlPort||!ready||!leaseChanged)throw std::invalid_argument("Invalid discovery configuration");
    auto& s=*impl_;WSADATA data{};
    if(WSAStartup(MAKEWORD(2,2),&data))throw std::runtime_error("Discovery WSAStartup failed");
    s.winsock=true;
    s.socket=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    BOOL exclusive=TRUE;
    if(s.socket==INVALID_SOCKET||setsockopt(s.socket,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<char*>(&exclusive),sizeof(exclusive)))
        throw std::runtime_error("Cannot create exclusive control socket");
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(controlPort);address.sin_addr.s_addr=INADDR_ANY;
    if(bind(s.socket,reinterpret_cast<sockaddr*>(&address),sizeof(address)))throw std::runtime_error("Cannot bind discovery handshake port");
    // A DNS registration failure must not take down direct/broadcast handshakes.
    try{s.advertisement=advertise(controlPort,videoPort);s.detail="mDNS + LAN auto-discovery; UDP "+std::to_string(controlPort);}
    catch(const std::exception& e){s.detail="LAN auto-discovery active; mDNS unavailable: "+std::string(e.what());}
    s.worker=std::thread([state=&s,videoPort,allowed=std::move(allowedPeer),ready=std::move(ready),leaseChanged=std::move(leaseChanged),videoStats=std::move(videoStats),frameRateRequest=std::move(frameRateRequest),frameRateReported=std::move(frameRateReported)] {
        try {
            HandshakeProtocol protocol(videoPort);std::string lastLease;
            std::array<char,512> buffer{};
            while(!state->stopping) {
                fd_set set;FD_ZERO(&set);FD_SET(state->socket,&set);timeval timeout{0,100000};
                const int selected=select(0,&set,nullptr,nullptr,&timeout);
                if(selected==SOCKET_ERROR)throw std::runtime_error("Discovery socket wait failed");
                if(selected>0) {
                    sockaddr_in peer{};int length=sizeof(peer);
                    const int count=recvfrom(state->socket,buffer.data(),static_cast<int>(buffer.size()),0,reinterpret_cast<sockaddr*>(&peer),&length);
                    if(count==SOCKET_ERROR) {if(WSAGetLastError()==WSAEMSGSIZE)continue;throw std::runtime_error("Discovery receive failed");}
                    char ip[INET_ADDRSTRLEN]{};inet_ntop(AF_INET,&peer.sin_addr,ip,sizeof(ip));
                    if(allowed.empty()||allowed==ip) {
                        auto response=protocol.respond(std::string_view(buffer.data(),count),ip,ready(),std::chrono::steady_clock::now(),frameRateRequest?frameRateRequest():FrameRateRequest{});
                        if(frameRateReported)if(auto report=protocol.frameRateReport())frameRateReported(*report);
                        if(response.empty()&&videoStats&&protocol.leasePeer(std::chrono::steady_clock::now())==ip){
                            std::istringstream request(std::string(buffer.data(),count));std::string version,command,nonce,extra;
                            if(request>>version>>command>>nonce&&!(request>>extra)&&version=="CORTEX/1"&&command=="STATUS"&&nonce.size()==32&&
                                std::all_of(nonce.begin(),nonce.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');})){
                                const auto [frames,packets]=videoStats();response="CORTEX/1 VIDEO "+nonce+" "+std::to_string(frames)+" "+std::to_string(packets)+"\n";
                            }
                        }
                        // Install the RTP peer filter before acknowledging readiness.
                        const auto lease=protocol.leasePeer(std::chrono::steady_clock::now());
                        if(lease!=lastLease){leaseChanged(lease);lastLease=lease;}
                        if(!response.empty())sendto(state->socket,response.data(),static_cast<int>(response.size()),0,reinterpret_cast<sockaddr*>(&peer),length);
                    }
                }
                const auto lease=protocol.leasePeer(std::chrono::steady_clock::now());
                if(lease!=lastLease){leaseChanged(lease);lastLease=lease;}
            }
            leaseChanged("");
        }catch(const std::exception& error){
            std::lock_guard lock(state->mutex);state->detail=std::string("Discovery failed: ")+error.what();
        }
    });
}
StudioDiscovery::~StudioDiscovery()=default;
std::string StudioDiscovery::status()const{std::lock_guard lock(impl_->mutex);return impl_->detail;}
}
