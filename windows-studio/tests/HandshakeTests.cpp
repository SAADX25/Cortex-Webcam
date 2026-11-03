#include "cortex/discovery/HandshakeProtocol.h"
#include <stdexcept>
#include <iostream>
using namespace cortex;
void require(bool value){if(!value)throw std::runtime_error("Handshake expectation failed");}
int main(){try{
    HandshakeProtocol p(5004);const auto now=std::chrono::steady_clock::now();
    const std::string nonce="0123456789abcdef0123456789abcdef";
    const std::string hello="CORTEX/1 HELLO "+nonce+" H264 1920 1080 60\n";
    require(p.respond("CORTEX/1 DISCOVER "+nonce,"10.0.0.2",true,now)=="CORTEX/1 OFFER "+nonce+"\n");
    require(p.leasePeer(now).empty());
    require(p.respond("CORTEX/1 DISCOVER "+nonce+" junk","10.0.0.2",true,now).empty());
    require(p.respond("CORTEX/1 DISCOVER "+nonce,"10.0.0.2",false,now).empty());
    require(p.respond(hello,"10.0.0.2",false,now)=="CORTEX/1 WAIT "+nonce+"\n");
    require(p.leasePeer(now).empty());
    require(p.respond(hello,"10.0.0.2",true,now)=="CORTEX/1 READY "+nonce+" 5004 H264 1920 1080 60\n");
    require(p.respond(hello,"10.0.0.3",true,now)=="CORTEX/1 BUSY "+nonce+"\n");
    require(p.respond(hello,"10.0.0.2",true,now+std::chrono::seconds(4)).find("READY")!=std::string::npos);
    require(p.leasePeer(now+std::chrono::seconds(9))=="10.0.0.2");
    require(p.leasePeer(now+std::chrono::seconds(13)).empty());
    require(p.respond(hello,"10.0.0.3",true,now+std::chrono::seconds(13)).find("READY")!=std::string::npos);
    require(p.respond("CORTEX/1 HELLO bad H264 1920 1080 30","x",true,now).empty());
    require(p.respond(hello+"junk","x",true,now).empty());
    require(p.respond("CORTEX/1 HELLO "+nonce+" H264 1920 1080 120","x",true,now).empty());
    HandshakeProtocol sync(5004);
    const auto hello2=[&](int fps,std::string ack){return "CORTEX/2 HELLO "+nonce+" H264 1920 1080 "+std::to_string(fps)+" "+ack;};
    require(sync.respond(hello2(30,"0"),"phone",true,now,{41,60})=="CORTEX/2 READY "+nonce+" 5004 H264 1920 1080 60 41\n");
    require(sync.frameRateReport()->syncSupported&&sync.frameRateReport()->fps==30&&sync.frameRateReport()->acknowledged==0);
    require(sync.respond(hello2(60,"41"),"phone",true,now,{41,60}).find("1080 60 41")!=std::string::npos);
    // Once acknowledged, a phone-side change or hardware fallback must win.
    require(sync.respond(hello2(30,"41"),"phone",true,now,{41,60}).find("1080 30 41")!=std::string::npos);
    require(sync.respond(hello2(30,"41"),"phone",true,now,{42,60}).find("1080 60 42")!=std::string::npos);
    require(sync.respond(hello2(30,"42"),"other",true,now,{42,60})=="CORTEX/2 BUSY "+nonce+"\n");
    require(!sync.frameRateReport());
    require(!sync.respond(hello2(30,"42")+" device=53616d73756e6720533234","phone",true,now).empty());
    require(sync.frameRateReport()->deviceName=="Samsung S24");
    require(sync.respond(hello2(30,"42")+" device=0a","phone",true,now).empty());
    require(sync.respond(hello2(30,"42")+" device=zz","phone",true,now).empty());
    require(sync.respond(hello2(30,"-1"),"phone",true,now).empty());
    require(sync.respond(hello2(30,"9223372036854775808"),"phone",true,now).empty());
    require(sync.respond(hello2(30,"42")+" junk","phone",true,now).empty());
    require(sync.respond(hello,"phone",true,now,{99,30})=="CORTEX/1 READY "+nonce+" 5004 H264 1920 1080 60\n");
    require(!sync.frameRateReport()->syncSupported);
    std::cout<<"Handshake validation, leases, frame-rate requests/acknowledgements, fallback and legacy compatibility passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
