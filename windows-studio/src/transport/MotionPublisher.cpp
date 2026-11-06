#include <winsock2.h>
#include <ws2tcpip.h>
#include "cortex/transport/MotionPublisher.h"
#include <stdexcept>
namespace cortex {
struct MotionPublisher::Impl { SOCKET socket{INVALID_SOCKET};sockaddr_in target{};uint64_t sequence{};bool initialized{};
    ~Impl(){if(socket!=INVALID_SOCKET)closesocket(socket);if(initialized)WSACleanup();}};
MotionPublisher::MotionPublisher(uint16_t port):impl_(std::make_unique<Impl>()){
    if(!port)throw std::invalid_argument("Invalid motion port");
    WSADATA data;if(WSAStartup(MAKEWORD(2,2),&data))throw std::runtime_error("Motion Winsock startup failed");
    auto& s=*impl_;s.initialized=true;s.socket=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(s.socket==INVALID_SOCKET)throw std::runtime_error("Motion socket failed");
    u_long nonblocking=1;ioctlsocket(s.socket,FIONBIO,&nonblocking);
    s.target.sin_family=AF_INET;s.target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);s.target.sin_port=htons(port);
}
MotionPublisher::~MotionPublisher()=default;
bool MotionPublisher::publish(HumanoidMotion pose){auto& s=*impl_;pose.sequence=++s.sequence;auto bytes=encodeMotion(pose);
    return sendto(s.socket,reinterpret_cast<const char*>(bytes.data()),static_cast<int>(bytes.size()),0,reinterpret_cast<sockaddr*>(&s.target),sizeof(s.target))==static_cast<int>(bytes.size());}
}
