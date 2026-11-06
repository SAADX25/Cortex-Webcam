#include <winsock2.h>
#include "cortex/transport/MotionPublisher.h"
#include <iostream>
#include <stdexcept>
#include <thread>
int main(){WSADATA startup;WSAStartup(MAKEWORD(2,2),&startup);SOCKET receiver=INVALID_SOCKET;try{
    receiver=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(bind(receiver,reinterpret_cast<sockaddr*>(&address),sizeof(address)))throw std::runtime_error("Bind loopback test");
    int length=sizeof(address);getsockname(receiver,reinterpret_cast<sockaddr*>(&address),&length);DWORD timeout=1000;
    setsockopt(receiver,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout));
    cortex::MotionPublisher publisher(ntohs(address.sin_port));
    for(uint64_t i=1;i<=180;++i){cortex::HumanoidMotion motion;motion.timestampUs=i*16667;
        if(!publisher.publish(motion))throw std::runtime_error("Send loopback packet");
        std::array<uint8_t,1200> bytes{};int count=recv(receiver,reinterpret_cast<char*>(bytes.data()),static_cast<int>(bytes.size()),0);cortex::HumanoidMotion decoded;
        if(count!=792||!cortex::decodeMotion(std::span(bytes.data(),count),decoded)||decoded.sequence!=i)throw std::runtime_error("Lost/reordered/malformed motion packet");
        std::this_thread::sleep_for(std::chrono::milliseconds(16));}
    closesocket(receiver);WSACleanup();std::cout<<"PASS: 180 loopback frames, 792 bytes/frame, zero observed sequence gaps\n";return 0;
}catch(const std::exception& e){if(receiver!=INVALID_SOCKET)closesocket(receiver);WSACleanup();std::cerr<<e.what()<<'\n';return 1;}}
