#include "cortex/tracking/HumanoidMotion.h"
#include "cortex/tracking/HandIdentityTracker.h"
#include "cortex/tracking/HandVisibilityGate.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <fstream>
#include <algorithm>
using namespace cortex;
void check(bool value,const char* text){if(!value)throw std::runtime_error(text);}
namespace {
HandLandmarks testHand(float curl,int side=0){
    HandLandmarks hand;hand.confidence=1;
    hand.world[0]={0,0,0,1};
    for(int f=1;f<5;++f){
        int first=1+f*4;float x=f==2?.09f:.08f,z=(2-f)*.02f;
        hand.world[first]={x,0,z,1};
        for(int j=0;j<3;++j){
            float length=j==0?.03f:(j==1?.02f:.015f);
            float angle=-curl*(j==0?1.3f:(j==1?2.6f:3.5f));
            auto previous=hand.world[first+j];
            hand.world[first+j+1]={previous.x+length*std::cos(angle),previous.y+length*std::sin(angle),z,1};
        }
    }
    for(int j=0;j<4;++j)hand.world[1+j]={.025f+.02f*j,-.003f*j,.035f+.018f*j,1};
    if(side)for(auto& point:hand.world)point.x=-point.x;
    return hand;
}
float angularChange(BoneRotation a,BoneRotation b){
    float dot=a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w;
    return 114.59156f*std::acos(std::clamp(std::abs(dot),0.f,1.f));
}
BoneRotation product(BoneRotation a,BoneRotation b){
    return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
        a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z,1};
}
}
int main(int argc,char** argv){try{
    HumanoidLandmarks input;
    input.body[11]={.2f,1.4f,0,1};input.body[12]={-.2f,1.4f,0,1};
    input.body[13]={.5f,1.4f,0,1};input.body[15]={.8f,1.4f,0,1};
    input.body[14]={-.5f,1.4f,0,1};input.body[16]={-.8f,1.4f,0,1};
    input.body[7]={.1f,1.7f,0,1};input.body[8]={-.1f,1.7f,0,1};input.body[0]={0,1.7f,.1f,1};
    auto neutral=solveHumanoid(input);
    auto unreachable=solveTwoBoneIK({0,0,0,1},{10,0,0,1},{0,1,0,1},.3f,.4f);
    check(unreachable.valid&&std::abs(unreachable.wrist.x-.7f)<1e-5f,"IK must clamp unreachable targets to fixed limb length");
    auto pole=solveTwoBoneIK({0,0,0,1},{.4f,0,0,1},{0,1,0,1},.3f,.3f);
    check(pole.valid&&pole.elbow.y>.2f,"IK must preserve the elbow bend plane");
    check(neutral.bones[2].confidence==1&&std::abs(neutral.bones[2].w-1)<1e-5,"T-pose must be identity");
    check(neutral.bones[1].confidence==1&&std::abs(neutral.bones[1].w-1)<1e-5,"Forward head must be identity");
    {
        auto unfitted=input;unfitted.hasFace=true;unfitted.headPitch=std::numeric_limits<float>::quiet_NaN();
        auto unavailable=solveHumanoid(unfitted);
        check(unavailable.bones[1].confidence==0&&unavailable.hasExpressions,
              "A face without a valid head fit must retain expressions without claiming a neutral/Pose head measurement");
    }
    {
        HandVisibilityGate gate;auto start=std::chrono::steady_clock::now();
        std::array<HandLandmarks,2> measured{testHand(0),testHand(0,1)};
        for(int side=0;side<2;++side)for(int joint:{0,5,9,17})measured[side].image[joint]={200.f+side*200,200.f+joint,0,1};
        auto sample=measured;gate.update(sample,640,480,start);
        check(sample[0].confidence==0&&sample[1].confidence==0,"One detected hand frame must not wake an arm");
        sample=measured;gate.update(sample,640,480,start+std::chrono::milliseconds(33));
        check(sample[0].confidence==0,"Reacquisition requires multiple fresh inference samples");
        sample=measured;gate.update(sample,640,480,start+std::chrono::milliseconds(66));
        check(sample[0].confidence>=.8f&&sample[1].confidence>=.8f,"Stable visible palms must resume tracking");
        sample=measured;sample[0].image[0].x=-10;gate.update(sample,640,480,start+std::chrono::milliseconds(100));
        check(sample[0].confidence==0&&sample[1].confidence>=.8f,"Out-of-frame wrist must stop only its own hand");
        sample=measured;gate.update(sample,640,480,start+std::chrono::milliseconds(133));
        check(sample[0].confidence==0,"A single false return must not reactivate an occluded hand");
        gate.reset();sample=measured;gate.update(sample,640,480,start+std::chrono::milliseconds(166));
        check(sample[1].confidence==0,"Stream resets must require a fresh hand acquisition");
    }
    {
        auto seen=input;seen.handsAvailable=true;seen.hands[0]=testHand(0);seen.hasFace=true;
        auto present=solveHumanoid(seen);
        check(present.bones[2].confidence>.5f&&present.bones[5].confidence==0,"Arm chains must follow their own measured palms");
        seen.hands[0]={};
        for(int step=0;step<20;++step){
            seen.body[15].x=.2f+.02f*step;seen.body[15].y=1.6f-.01f*step;
            auto missing=solveHumanoid(seen);
            for(int bone=2;bone<38;++bone)check(missing.bones[bone].confidence<.35f,"Missing Hands must suppress confident Pose arm guesses and fingers");
            check(missing.bones[0].confidence>0&&missing.bones[1].confidence>0&&missing.hasExpressions,
                "Losing hands must not disable body/head/face tracking");
        }
    }
    for(float pitch:{-25.f,25.f}){
        auto face=input;face.hasFace=true;face.headPitch=pitch;
        auto motion=solveHumanoid(face);auto q=product(motion.bones[0],motion.bones[1]);
        float facingY=2*(q.y*q.z-q.w*q.x);
        check(facingY*pitch>0,"Physical look-up/look-down must raise/lower the avatar forward vector without reversing pitch");
    }
    {
        HumanoidLandmarks face;face.hasFace=true;face.headPitch=15;face.bodySwayX=.5f;
        auto motion=solveHumanoid(face);auto world=product(motion.bones[0],motion.bones[1]);
        check(2*(world.y*world.z-world.w*world.x)>.2f&&std::abs(world.z)<.001f,
              "Head world orientation must remain independent of the synthetic chest sway fallback");
    }
    {
        HandIdentityTracker identities;
        auto time=std::chrono::steady_clock::now();
        std::array<Joint3D,33> body{};
        std::array<HandLandmarks,2> detections{testHand(0,0),testHand(0,1)};
        for(int step=0;step<=30;++step){
            detections[0].rightProbability=.05f;detections[1].rightProbability=.95f;
            detections[0].image[0]={200.f+step*7,300,0,1};
            detections[1].image[0]={440.f-step*7,300,0,1};
            auto unordered=detections;if(step%2)std::swap(unordered[0],unordered[1]);
            auto assigned=identities.update(unordered,body,640,480,time+std::chrono::milliseconds(step*16));
            check(assigned[0].image[0].x==detections[0].image[0].x&&assigned[1].image[0].x==detections[1].image[0].x,
                  "Crossing hands and detector order changes must preserve anatomical identity");
        }
        auto single=identities.update(std::span(&detections[1],1),body,640,480,time+std::chrono::milliseconds(500));
        check(single[0].confidence==0&&single[1].confidence>=.8f,"One hidden hand must not duplicate the remaining detection");
        identities.reset();body[15]={400,300,0,1};body[16]={200,300,0,1};
        detections[0].image[0]={400,300,0,1};detections[1].image[0]={200,300,0,1};
        detections[0].rightProbability=detections[1].rightProbability=.1f;
        auto paired=identities.update(detections,body,640,480,time);
        check(paired[0].confidence>0&&paired[1].confidence>0&&paired[0].image[0].x==400&&paired[1].image[0].x==200,
              "Joint assignment must not collapse two hands onto one identity when labels are ambiguous");
    }
    {
        HumanoidMotionSolver occlusion(false);
        auto face=input;auto time=std::chrono::steady_clock::now();
        face.hasFace=true;face.headYaw=20;face.capturedAt=time;
        face.faceEyesImage={Joint3D{280,180,0,1},Joint3D{360,180,0,1}};
        auto visible=occlusion.solve(face);
        face.hasFace=false;face.body[0].confidence=face.body[7].confidence=face.body[8].confidence=0;
        for(int side=0;side<2;++side){
            face.hands[side]=testHand(0,side);int point=0;
            for(int joint:{0,5,9,13,17}){
                float angle=point++*1.256637061f;
                face.hands[side].image[joint]={280.f+80.f*side+25.f*std::cos(angle),180.f+25.f*std::sin(angle),0,1};
            }
        }
        face.capturedAt=time+std::chrono::milliseconds(300);auto covered=occlusion.solve(face);
        check(covered.bones[1].confidence>0&&angularChange(visible.bones[1],covered.bones[1])<.1f,
              "Hands covering the eyes must bridge a short face dropout without snapping the head");
        face.capturedAt=time+std::chrono::milliseconds(900);auto expired=occlusion.solve(face);
        check(expired.bones[1].confidence==0,"Occluded face hold must expire rather than freeze indefinitely");
        occlusion.reset();face.capturedAt=time+std::chrono::milliseconds(1000);
        check(occlusion.solve(face).bones[1].confidence==0,"Stream reset must discard the occluded face pose");
    }
    for(int side=0;side<2;++side){
        auto gesture=input;gesture.hands[side]=testHand(0,side);
        auto previous=solveHumanoid(gesture);
        for(int step=1;step<=1000;++step){
            auto moving=testHand(step/1000.f,side);
            // Keep middle finger open while the other fingers close gradually.
            auto open=testHand(0,side);
            for(int point=9;point<=12;++point)moving.world[point]=open.world[point];
            gesture.hands[side]=moving;auto next=solveHumanoid(gesture);
            for(int bone=8+side*15;bone<23+side*15;++bone)
                check(angularChange(previous.bones[bone],next.bones[bone])<3.5f,
                      "Continuous finger input must not snap at gesture contrast thresholds");
            previous=next;
        }
        auto missing=gesture;missing.hands[side].world[6].confidence=0;
        auto lostFinger=solveHumanoid(missing);
        check(lostFinger.bones[11+side*15].confidence==0&&lostFinger.bones[12+side*15].confidence==0&&
              lostFinger.bones[14+side*15].confidence>0,
              "One missing index joint must invalidate that finger while preserving other fingers");
        auto grip=input;grip.hands[side]=testHand(.72f,side);
        auto gripPose=solveHumanoid(grip);
        for(int joint=0;joint<3;++joint){
            float minAngle=180,maxAngle=0;
            for(int finger=1;finger<5;++finger){
                float angle=angularChange(gripPose.bones[8+side*15+finger*3+joint],BoneRotation{});
                minAngle=std::min(minAngle,angle);maxAngle=std::max(maxAngle,angle);
            }
            check(minAngle>35&&maxAngle-minAngle<12,
                  "A whole-hand grip must close all four fingers together with bounded joint differences");
        }
        HumanoidMotionSolver solver(false);HumanoidMotion last;
        auto start=std::chrono::steady_clock::now();
        for(int degrees=160;degrees<=200;++degrees){
            gesture.hands[side]=testHand(.3f,side);
            float angle=degrees*.01745329252f;
            for(auto& p:gesture.hands[side].world){float y=p.y,z=p.z;p.y=std::cos(angle)*y-std::sin(angle)*z;p.z=std::sin(angle)*y+std::cos(angle)*z;}
            gesture.capturedAt=start+std::chrono::milliseconds((degrees-160)*16);
            auto next=solver.solve(gesture);int lower=side?6:3,hand=side?7:4;
            if(degrees>160)check(angularChange(last.bones[lower],next.bones[lower])<2,
                "Forearm twist must remain continuous when wrist crosses 180 degrees");
            BoneRotation expected{std::sin(angle/2),0,0,std::cos(angle/2),1};
            check(angularChange(product(next.bones[lower],next.bones[hand]),expected)<.1f,
                "Distributing twist must preserve total measured palm orientation");
            last=next;
        }
        solver.reset();
        auto reset=solver.solve(gesture),fresh=HumanoidMotionSolver{}.solve(gesture);
        check(angularChange(reset.bones[side?6:3],fresh.bones[side?6:3])<.1f,"Reset must discard twist history");
    }
    for(int fps:{30,60,120}){
        HumanoidMotionFilter filter;
        auto start=std::chrono::steady_clock::now();
        HumanoidMotion pose;pose.bones[0].confidence=pose.bones[11].confidence=1;
        filter.update(pose,start);
        for(int frame=1;frame<=fps;++frame){
            float half=.3f*std::sin(frame*1.7f)*.00872664626f;
            pose.bones[0]={0,0,std::sin(half),std::cos(half),1};
            pose.bones[11]=pose.bones[0];
            auto steady=filter.update(pose,start+std::chrono::microseconds(frame*1000000/fps));
            check(angularChange(steady.bones[0],BoneRotation{})<.1f&&angularChange(steady.bones[11],BoneRotation{})<.1f,
                  "Small stationary tremor must not shake torso or fingers at 30/60/120fps");
        }
        pose.bones[0]=pose.bones[11]={0,0,.70710678f,.70710678f,1};
        HumanoidMotion moving;
        for(int frame=1;frame<=static_cast<int>(fps*.15f)+1;++frame)
            moving=filter.update(pose,start+std::chrono::microseconds(1000000+frame*1000000/fps));
        check(angularChange(moving.bones[0],pose.bones[0])<8&&angularChange(moving.bones[11],pose.bones[11])<4,
              "Damping must still catch up to a deliberate 90-degree motion within 150ms");
        auto repeated=filter.update(pose,start+std::chrono::microseconds(1000000+(static_cast<int>(fps*.15f)+1)*1000000/fps));
        check(angularChange(repeated.bones[0],moving.bones[0])<.1f,"Duplicate timestamp must not advance stabilization");
        filter.reset();auto reset=filter.update(pose,start);
        check(angularChange(reset.bones[0],pose.bones[0])<.1f,"Reset must clear damping history");
    }
    {
        auto withHands = input;
        withHands.hands[0].confidence = 1.f;
        withHands.hands[0].world[0] = {0, 0, 0, 1};
        withHands.hands[0].world[5] = {.08f, 0, .03f, 1};
        withHands.hands[0].world[9] = {.09f, 0, 0, 1};
        withHands.hands[0].world[17] = {.08f, 0, -.03f, 1};
        // Index finger curled into a fist: MCP -70 deg, PIP -150 deg, DIP -210 deg in palm XY plane.
        withHands.hands[0].world[6] = {.08f + .03f * std::cos(-1.22f), .03f * std::sin(-1.22f), .03f, 1};
        withHands.hands[0].world[7] = {withHands.hands[0].world[6].x + .02f * std::cos(-2.62f),
                                       withHands.hands[0].world[6].y + .02f * std::sin(-2.62f), .03f, 1};
        withHands.hands[0].world[8] = {withHands.hands[0].world[7].x + .015f * std::cos(-3.66f),
                                       withHands.hands[0].world[7].y + .015f * std::sin(-3.66f), .03f, 1};
        auto handPose = solveHumanoid(withHands);
        check(handPose.bones[4].confidence == 1 && std::abs(handPose.bones[4].w - 1) < 1e-4f,
              "Palm-down T-pose wrist must be identity");
        check(handPose.bones[11].z < -.4f && handPose.bones[12].z < -.4f && handPose.bones[13].z < -.3f,
              "Curled index finger joints must flex monotonically without 180-degree sign flip");
        auto fallbackHand = input;
        fallbackHand.handOpen[0] = 0.1f;
        fallbackHand.handOpenConfidence[0] = 0.85f;
        fallbackHand.hasFace = true;
        fallbackHand.leftEyeOpen = 0.1f;
        fallbackHand.mouthOpen = 0.75f;
        auto solvedFallback = solveHumanoid(fallbackHand);
        check(solvedFallback.bones[11].confidence > 0.8f && solvedFallback.bones[11].z < -0.25f &&
                  solvedFallback.hasExpressions && solvedFallback.expressions[0] > 0.85f &&
                  solvedFallback.expressions[2] > 0.7f,
              "Fallback fist curl and live facial expressions must populate");
        auto extBytes = encodeMotion(solvedFallback);
        fallbackHand.handsAvailable=true;
        auto uncertain=solveHumanoid(fallbackHand);
        check(uncertain.bones[11].confidence<.35f&&uncertain.hasExpressions,
              "Occluded Hands must not be replaced by a confident guessed finger gesture");
        HumanoidMotion extDecoded;
        check(extBytes.size() == 824 && decodeMotion(extBytes, extDecoded) && extDecoded.hasExpressions &&
                  std::abs(extDecoded.expressions[2] - 0.75f) < 1e-5f,
              "Extended 824-byte expression datagram must round trip");
        // Test selective finger gesture (e.g., middle finger extended while index/ring/little are curled).
        for (int f : {1, 3, 4}) {
            int base = 1 + f * 4;
            float zOff = (2 - f) * .015f;
            withHands.hands[0].world[base] = {.08f, 0, zOff, 1};
            withHands.hands[0].world[base + 1] = {.08f + .03f * std::cos(-1.22f), .03f * std::sin(-1.22f), zOff, 1};
            withHands.hands[0].world[base + 2] = {withHands.hands[0].world[base + 1].x + .02f * std::cos(-2.62f),
                                                  withHands.hands[0].world[base + 1].y + .02f * std::sin(-2.62f), zOff, 1};
            withHands.hands[0].world[base + 3] = {withHands.hands[0].world[base + 2].x + .015f * std::cos(-3.66f),
                                                  withHands.hands[0].world[base + 2].y + .015f * std::sin(-3.66f), zOff, 1};
        }
        withHands.hands[0].world[9] = {.09f, 0, 0, 1};
        withHands.hands[0].world[10] = {.12f, 0, 0, 1};
        withHands.hands[0].world[11] = {.145f, 0, 0, 1};
        withHands.hands[0].world[12] = {.165f, 0, 0, 1};
        auto gesturePose = solveHumanoid(withHands);
        check(gesturePose.bones[11].z < -.5f && gesturePose.bones[17].z < -.5f && gesturePose.bones[20].z < -.5f &&
                  std::abs(gesturePose.bones[14].z) < .05f && std::abs(gesturePose.bones[15].z) < .05f,
              "Selective single-finger gesture must curl folded fingers tightly while keeping extended finger straight");
    }
    input.body[15]={.5f,1.7f,0,1};auto bend=solveHumanoid(input);
    check(std::abs(bend.bones[3].z)>.6f&&std::abs(bend.bones[3].w)>.6f,"90 degree elbow must be a normalized local rotation");
    input.body[15]={.2f,1.4f,0,1};auto reverse=solveHumanoid(input);
    check(std::isfinite(reverse.bones[3].w),"Antiparallel joints must not produce NaN");
    input.body[15]=input.body[13];auto degenerate=solveHumanoid(input);
    check(degenerate.bones[3].confidence==0,"Zero length joints must not invent rotation");
    input.body[15]={std::numeric_limits<float>::quiet_NaN(),0,0,1};
    auto invalid=solveHumanoid(input);check(invalid.bones[2].confidence==0,"Invalid landmarks must be rejected");
    bend.sequence=17;bend.timestampUs=123456789;auto bytes=encodeMotion(bend);HumanoidMotion decoded;
    check(bytes.size()==792&&decodeMotion(bytes,decoded),"One 792-byte datagram must round trip");
    check(decoded.sequence==17&&decoded.timestampUs==123456789,"Wire timestamps and sequence must survive");
    check(std::abs(decoded.bones[3].z-bend.bones[3].z)<1e-6f,"Quaternion must survive wire format");
    if(argc>1){std::ofstream file(argv[1],std::ios::binary);file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());check(bool(file),"Write cross-language fixture");}
    bytes.pop_back();check(!decodeMotion(bytes,decoded),"Truncated motion must be rejected");
    bytes=encodeMotion(bend);bytes[8]=2;check(!decodeMotion(bytes,decoded),"Unknown protocol version must be rejected");
    bend.bones[0].x=std::numeric_limits<float>::infinity();check(!decodeMotion(encodeMotion(bend),decoded),"Nonfinite wire rotations must be rejected");
    std::cout<<"PASS: humanoid T-pose, elbow, singularities, loss and bounded motion protocol\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
