#include "cortex/filters/HumanoidMotion.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
namespace cortex {
namespace {
struct V { float x{},y{},z{}; };
V v(Joint3D p){return {p.x,p.y,p.z};}
V operator-(V a,V b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
V operator*(V a,float b){return {a.x*b,a.y*b,a.z*b};}
float dot(V a,V b){return a.x*b.x+a.y*b.y+a.z*b.z;}
V cross(V a,V b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
V unit(V a){float n=std::sqrt(dot(a,a));return n>1e-6f?a*(1/n):V{};}
bool valid(Joint3D p){return std::isfinite(p.confidence)&&p.confidence>=.5f&&p.confidence<=1.f&&std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
using Q=BoneRotation;
Q norm(Q q){float n=std::sqrt(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w);if(n<1e-6f)return {};float s=(q.w<0?-1.f:1.f)/n;return {q.x*s,q.y*s,q.z*s,q.w*s,q.confidence};}
float smoothstep(float lo,float hi,float value){float t=std::clamp((value-lo)/(hi-lo),0.f,1.f);return t*t*(3.f-2.f*t);}
Q blend(Q a,Q b,float t){
    float sign=a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w<0?-1.f:1.f;
    return norm({std::lerp(a.x,b.x*sign,t),std::lerp(a.y,b.y*sign,t),std::lerp(a.z,b.z*sign,t),std::lerp(a.w,b.w*sign,t),std::min(a.confidence,b.confidence)});
}
Q clampAngle(Q q,float wMin){q=norm(q);if(q.confidence==0||q.w>=wMin)return q;
    float s=std::sqrt(q.x*q.x+q.y*q.y+q.z*q.z);if(s<1e-6f)return {0,0,0,1,q.confidence};
    float k=std::sqrt(std::max(0.f,1.f-wMin*wMin))/s;return {q.x*k,q.y*k,q.z*k,wMin,q.confidence};}
Q inv(Q q){return {-q.x,-q.y,-q.z,q.w,q.confidence};}
Q mul(Q a,Q b){return norm({a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z, std::min(a.confidence,b.confidence)});}
Q swing(V a,V b,float confidence){a=unit(a);b=unit(b);if(dot(a,a)<.5f||dot(b,b)<.5f)return {};
    float d=std::clamp(dot(a,b),-1.f,1.f);V axis=cross(a,b);
    if(d<-.9999f){axis=unit(cross(a,{0,1,0}));if(dot(axis,axis)<.5f)axis=unit(cross(a,{0,0,1}));return {axis.x,axis.y,axis.z,0,confidence};}
    return norm({axis.x,axis.y,axis.z,1+d,confidence});}
V rotate(Q q,V a){V u{q.x,q.y,q.z};V t=cross(u,a)*2;V c=cross(u,t);return {a.x+q.w*t.x+c.x,a.y+q.w*t.y+c.y,a.z+q.w*t.z+c.z};}
Q frame(V x,V normal,float confidence){
    x=unit(x);V z=unit(normal-x*dot(normal,x));V y=unit(cross(z,x));
    if(dot(x,x)<.5f||dot(y,y)<.5f)return {};
    // Quaternion from a right-handed orthonormal basis (columns X,Y,Z).
    float t=x.x+y.y+z.z;Q q{};
    if(t>0){float s=std::sqrt(1+t)*2;q={ (y.z-z.y)/s,(z.x-x.z)/s,(x.y-y.x)/s,s/4,confidence};}
    else if(x.x>y.y&&x.x>z.z){float s=std::sqrt(1+x.x-y.y-z.z)*2;q={s/4,(y.x+x.y)/s,(z.x+x.z)/s,(y.z-z.y)/s,confidence};}
    else if(y.y>z.z){float s=std::sqrt(1+y.y-x.x-z.z)*2;q={(y.x+x.y)/s,s/4,(z.y+y.z)/s,(z.x-x.z)/s,confidence};}
    else {float s=std::sqrt(1+z.z-x.x-y.y)*2;q={(z.x+x.z)/s,(z.y+y.z)/s,s/4,(x.y-y.x)/s,confidence};}
    return norm(q);
}
void put(std::vector<uint8_t>& out,uint64_t n,size_t bytes){for(size_t i=0;i<bytes;++i)out.push_back(static_cast<uint8_t>(n>>(8*i)));}
uint64_t get(std::span<const uint8_t> data,size_t& offset,size_t bytes){uint64_t n=0;for(size_t i=0;i<bytes;++i)n|=uint64_t(data[offset++])<<(8*i);return n;}
}
ArmIkResult solveTwoBoneIK(Joint3D shoulder,Joint3D target,Joint3D pole,float a,float b){
    if(!valid(shoulder)||!valid(target)||!valid(pole)||!std::isfinite(a)||!std::isfinite(b)||a<1e-5f||b<1e-5f)return {};
    V root=v(shoulder),delta=v(target)-root;float distance=std::sqrt(dot(delta,delta));
    if(distance<1e-5f)return {};
    V direction=unit(delta),side=unit((v(pole)-root)-direction*dot(v(pole)-root,direction));
    if(dot(side,side)<.5f){side=unit(cross(direction,{0,0,1}));if(dot(side,side)<.5f)side=unit(cross(direction,{0,1,0}));}
    float reach=std::clamp(distance,std::abs(a-b)+1e-6f,a+b);
    float along=(a*a+reach*reach-b*b)/(2*reach),height=std::sqrt(std::max(0.f,a*a-along*along));
    V elbow{root.x+direction.x*along+side.x*height,root.y+direction.y*along+side.y*height,root.z+direction.z*along+side.z*height};
    V wrist{root.x+direction.x*reach,root.y+direction.y*reach,root.z+direction.z*reach};
    float c=std::min({shoulder.confidence,target.confidence,pole.confidence});
    return {{elbow.x,elbow.y,elbow.z,c},{wrist.x,wrist.y,wrist.z,c},true};
}
static HumanoidMotion solveHumanoidImpl(const HumanoidLandmarks& input,
    std::array<float,2>* twistAngles=nullptr,
    std::array<std::chrono::steady_clock::time_point,2>* twistTimes=nullptr,
    std::array<bool,2>* twistValid=nullptr){
    HumanoidMotion out;
    out.timestampUs=std::chrono::duration_cast<std::chrono::microseconds>(input.capturedAt.time_since_epoch()).count();
    Q chest{};chest.confidence=1;
    if(valid(input.body[11])&&valid(input.body[12])){
        V shoulder=v(input.body[11])-v(input.body[12]);
        shoulder.z*=0.35f;
        V up{0,1,0};
        if(valid(input.body[23])&&valid(input.body[24])){
            V a=v(input.body[11]),b=v(input.body[12]),c=v(input.body[23]),d=v(input.body[24]);
            V torso={(a.x+b.x-c.x-d.x)/2,(a.y+b.y-c.y-d.y)/2,(a.z+b.z-c.z-d.z)*0.175f};
            if(torso.y>.15f)up=unit({std::clamp(torso.x,-torso.y*.35f,torso.y*.35f),torso.y,std::clamp(torso.z,-torso.y*.35f,torso.y*.35f)});}
        chest=clampAngle(frame(shoulder,cross(shoulder,up),std::min(input.body[11].confidence,input.body[12].confidence)),0.976f);out.bones[0]=chest;
    }
    if(input.hasFace&&std::isfinite(input.headYaw)&&std::isfinite(input.headPitch)&&std::isfinite(input.headRoll)){
        constexpr float deg2rad=0.0174532925f*0.5f;
        float cy=std::cos(input.headYaw*deg2rad),sy=std::sin(input.headYaw*deg2rad);
        float cp=std::cos(input.headPitch*deg2rad),sp=std::sin(input.headPitch*deg2rad);
        float cr=std::cos(-input.headRoll*deg2rad),sr=std::sin(-input.headRoll*deg2rad);
        // OpenSeeFace positive pitch looks up. In Y-up/Z-toward-camera
        // coordinates a negative X rotation raises the forward vector.
        Q qy{0,sy,0,cy,1.f},qp{-sp,0,0,cp,1.f},qr{0,0,sr,cr,1.f};
        Q head=mul(qy,mul(qp,qr));
        if(out.bones[0].confidence==0&&std::isfinite(input.bodySwayX)){
            float sway=std::clamp(-input.bodySwayX*0.18f,-0.08f,0.08f);
            out.bones[0]=norm({0,0,sway,1.f,0.85f});
            chest=out.bones[0];
        }
        out.bones[1]=clampAngle(mul(inv(chest),head),0.88f);
    }else if(!input.hasFace&&valid(input.body[7])&&valid(input.body[8])&&valid(input.body[0])){
        V left=v(input.body[7]),right=v(input.body[8]);
        V center{(left.x+right.x)/2,(left.y+right.y)/2,(left.z+right.z)/2};
        auto head=frame(left-right,v(input.body[0])-center,
                        std::min({input.body[7].confidence,input.body[8].confidence,input.body[0].confidence}));
        out.bones[1]=clampAngle(mul(inv(chest),head),0.906f);
    }
    for(int side=0;side<2;++side){
        const int shoulder=11+side,elbow=13+side,wrist=15+side,base=side?5:2;
        const auto& hand=input.hands[side];
        const bool measuredPalm=std::isfinite(hand.confidence)&&hand.confidence>=.8f&&hand.confidence<=1.f&&
            valid(hand.world[0])&&valid(hand.world[5])&&valid(hand.world[9])&&valid(hand.world[17]);
        // Pose can confidently extrapolate a wrist outside the image. Once
        // Hands is available, missing measured palms invalidate that whole
        // arm chain instead of continuing to animate its guessed elbow/wrist.
        if(input.handsAvailable&&!measuredPalm)continue;
        const float sign=side?-1.f:1.f;V rest{sign,0,0};Q lowerWorld=chest;
        if(valid(input.body[shoulder])&&valid(input.body[elbow])&&valid(input.body[wrist])){
            float c=std::min({input.body[shoulder].confidence,input.body[elbow].confidence,input.body[wrist].confidence});
            V measuredUpper=v(input.body[elbow])-v(input.body[shoulder]),measuredLower=v(input.body[wrist])-v(input.body[elbow]);
            auto ik=solveTwoBoneIK(input.body[shoulder],input.body[wrist],input.body[elbow],std::sqrt(dot(measuredUpper,measuredUpper)),std::sqrt(dot(measuredLower,measuredLower)));
            if(ik.valid){
            Q upper=swing(rest,v(ik.elbow)-v(input.body[shoulder]),c);
            Q lower=swing(rest,v(ik.wrist)-v(ik.elbow),c);
            out.bones[base]=mul(inv(chest),upper);out.bones[base+1]=mul(inv(upper),lower);lowerWorld=lower;
            }
        }else if(valid(input.body[shoulder])&&valid(input.body[elbow])&&
                 std::isfinite(input.body[wrist].x)&&std::isfinite(input.body[wrist].y)&&std::isfinite(input.body[wrist].z)){
            float c=std::min(input.body[shoulder].confidence,input.body[elbow].confidence);
            Q upper=swing(rest,v(input.body[elbow])-v(input.body[shoulder]),c);
            if(upper.confidence>0){
                out.bones[base]=mul(inv(chest),upper);lowerWorld=upper;
                if(input.body[wrist].confidence>=.25f){
                    Q lower=swing(rest,v(input.body[wrist])-v(input.body[elbow]),c);
                    if(lower.confidence>0){out.bones[base+1]=mul(inv(upper),lower);lowerWorld=lower;}
                }
            }
        }
        if(!std::isfinite(hand.confidence)||hand.confidence<.8f||hand.confidence>1.f){
            float hc=std::clamp(input.handOpenConfidence[side],0.f,1.f);
            if(hc>=.35f){
                // Pose's sparse hand landmarks cannot reconstruct a selective
                // finger gesture after Hands loses it. Let the receiver hold/fade
                // the last measured pose rather than replace it with a guessed fist.
                if(input.handsAvailable)hc=std::min(hc,.30f);
                float open=std::clamp(input.handOpen[side],0.f,1.f);
                float curl=1.f-open;
                if(input.wristConfidence[side]>=.35f&&std::isfinite(input.wristBend[side])){
                    float wb=std::clamp(input.wristBend[side],-1.f,1.f)*0.32f;
                    out.bones[base+2]=norm({0.12f,0,-sign*wb,1.f,hc});
                }
                for(int finger=0;finger<5;++finger){
                    float fScale=finger==0?0.55f:(0.82f+0.10f*float(finger-1));
                    for(int joint=0;joint<3;++joint){
                        float jScale=joint==0?0.85f:(joint==1?1.05f:0.70f);
                        float angle=(0.18f+0.95f*curl)*fScale*jScale;
                        float half=angle*0.5f;
                        if(finger==0){
                            out.bones[8+side*15+joint]=norm({0.12f*curl,sign*std::sin(half)*0.65f,-sign*std::sin(half)*0.55f,std::cos(half),hc});
                        }else{
                            float splay=(float(finger)-2.2f)*0.035f*open;
                            out.bones[8+side*15+finger*3+joint]=norm({0,joint==0?sign*splay:0.f,-sign*std::sin(half),std::cos(half),hc});
                        }
                    }
                }
            }
            continue;
        }
        if(!valid(hand.world[0])||!valid(hand.world[5])||!valid(hand.world[9])||!valid(hand.world[17]))continue;
        V forward=v(hand.world[9])-v(hand.world[0]);
        float palmLen=std::sqrt(dot(forward,forward));
        if(palmLen<1e-4f)continue;
        V normal=cross(v(hand.world[5])-v(hand.world[0]),v(hand.world[17])-v(hand.world[0]))*sign;
        Q palm=frame(forward*sign,cross(forward*sign,normal),hand.confidence);if(palm.confidence==0)continue;
        {
            Q rel=mul(inv(lowerWorld),palm);
            V handAxis=rotate(rel,rest);
            Q rawSwing=swing(rest,handAxis,hand.confidence);
            Q wSwing=clampAngle(rawSwing,0.819f);
            Q rawTwist=mul(inv(rawSwing),rel);
            rawTwist=norm({rawTwist.x,0.f,0.f,rawTwist.w,hand.confidence});
            if(out.bones[base+1].confidence>0){
                float twistAngle=2.f*std::atan2(rawTwist.x,rawTwist.w);
                if(twistAngles){
                    const auto gap=input.capturedAt-(*twistTimes)[side];
                    if((*twistValid)[side] && gap>=std::chrono::steady_clock::duration::zero() && gap<=std::chrono::milliseconds(250))
                        twistAngle=(*twistAngles)[side]+std::remainder(twistAngle-(*twistAngles)[side],6.28318530718f);
                    (*twistAngles)[side]=twistAngle;
                    (*twistTimes)[side]=input.capturedAt;
                    (*twistValid)[side]=true;
                }
                Q twistForearm{std::sin(twistAngle*0.20f),0.f,0.f,std::cos(twistAngle*0.20f),hand.confidence};
                out.bones[base+1]=mul(out.bones[base+1],twistForearm);
                out.bones[base+2]=mul(inv(twistForearm),mul(wSwing,rawTwist));
            }else{
                out.bones[base+2]=mul(wSwing,rawTwist);
            }
        }
        V w0=v(hand.world[0]);
        std::array<float,5> fingerCurl{};
        std::array<bool,5> fingerValid{};
        float minCurl=1.f,maxCurl=0.f;
        int validFingers=0;
        for(int finger=1;finger<5;++finger){
            int first=1+finger*4;
            if(!valid(hand.world[first])||!valid(hand.world[first+1])||!valid(hand.world[first+2])||!valid(hand.world[first+3]))continue;
            V p0=v(hand.world[first]),p1=v(hand.world[first+1]),p2=v(hand.world[first+2]),p3=v(hand.world[first+3]);
            float mcpDist=std::max(1e-4f,std::sqrt(dot(p0-w0,p0-w0)));
            float tipWrist=std::sqrt(dot(p3-w0,p3-w0))/mcpDist;
            float segLen=std::sqrt(dot(p1-p0,p1-p0))+std::sqrt(dot(p2-p1,p2-p1))+std::sqrt(dot(p3-p2,p3-p2));
            if(segLen<1e-4f)continue;
            fingerValid[finger]=true;
            float straightRatio=std::sqrt(dot(p3-p0,p3-p0))/std::max(1e-4f,segLen);
            V palmTipDir=unit(rotate(inv(palm),p3-p0));
            float cTip=std::clamp((1.72f-tipWrist)/(1.72f-1.02f),0.f,1.f);
            float cStraight=std::clamp((0.88f-straightRatio)/(0.88f-0.48f),0.f,1.f);
            float cProj=std::clamp((0.78f-palmTipDir.x*sign)/(0.78f+0.15f),0.f,1.f);
            fingerCurl[finger]=std::clamp(0.45f*cTip+0.30f*cStraight+0.25f*cProj,0.f,1.f);
            minCurl=std::min(minCurl,fingerCurl[finger]);
            maxCurl=std::max(maxCurl,fingerCurl[finger]);
            ++validFingers;
        }
        // A whole-hand grip should close together. Couple only similar measured
        // curls; preserve selective gestures whenever one finger remains open.
        float gripTogether=0;
        if(validFingers==4){
            float sum=0;
            for(int finger=1;finger<5;++finger)sum+=fingerCurl[finger];
            float common=sum*.25f;
            float together=(1.f-smoothstep(.12f,.28f,maxCurl-minCurl))*smoothstep(.12f,.40f,minCurl);
            gripTogether=together;
            for(int finger=1;finger<5;++finger)
                fingerCurl[finger]=std::lerp(fingerCurl[finger],common,together);
        }
        for(int finger=1;finger<5;++finger){
            float c=fingerCurl[finger];
            float natural=smoothstep(.12f,.80f,c);
            float spread=maxCurl-minCurl;
            float contrast=validFingers>=2?smoothstep(.18f,.42f,spread):0.f;
            float selective=smoothstep(.20f,.80f,(c-minCurl)/std::max(spread,1e-4f));
            fingerCurl[finger]=std::lerp(natural,selective,contrast);
        }
        // Thumb extension vs tuck detection (for thumbs-up, thumbs-down, fist, middle-finger).
        if(valid(hand.world[1])&&valid(hand.world[2])&&valid(hand.world[3])&&valid(hand.world[4])){
            V t1=v(hand.world[1]),t2=v(hand.world[2]),t3=v(hand.world[3]),t4=v(hand.world[4]);
            float tSeg=std::sqrt(dot(t2-t1,t2-t1))+std::sqrt(dot(t3-t2,t3-t2))+std::sqrt(dot(t4-t3,t4-t3));
            fingerValid[0]=tSeg>=1e-4f;
            float tStraight=std::sqrt(dot(t4-t1,t4-t1))/std::max(1e-4f,tSeg);
            float tipToMidMcp=std::sqrt(dot(t4-v(hand.world[9]),t4-v(hand.world[9])))/palmLen;
            float tipToIdxMcp=std::sqrt(dot(t4-v(hand.world[5]),t4-v(hand.world[5])))/palmLen;
            float cDist=std::clamp((0.88f-std::min(tipToMidMcp,tipToIdxMcp*1.15f))/(0.88f-0.42f),0.f,1.f);
            float cBend=std::clamp((0.90f-tStraight)/(0.90f-0.62f),0.f,1.f);
            fingerCurl[0]=std::clamp(0.65f*cDist+0.35f*cBend,0.f,1.f);
            if(validFingers>0)fingerCurl[0]*=1.f-smoothstep(.55f,.80f,minCurl)*(1.f-smoothstep(.25f,.55f,fingerCurl[0]));
        }
        const V thumbRest=unit({sign*0.72f,-0.12f,0.68f});
        for(int finger=0;finger<5;++finger){
            if(!fingerValid[finger])continue;
            const V fingerRest=finger==0?thumbRest:rest;
            Q chain{};chain.confidence=hand.confidence;
            int first=1+finger*4;
            float curl=fingerCurl[finger];
            V p0=v(hand.world[first]),p1=v(hand.world[first+1]);
            V d0Palm=unit(rotate(inv(palm),p1-p0));
            float splayZ=std::clamp(d0Palm.z,-0.38f,0.38f)*(1.f-0.85f*curl);
            for(int joint=0;joint<3;++joint){
                auto a=hand.world[first+joint],b=hand.world[first+joint+1];if(!valid(a)||!valid(b))continue;
                V palmDir=rotate(inv(palm),v(b)-v(a));
                V localDir=unit(rotate(inv(chain),palmDir));
                if(dot(localDir,localDir)<.5f)continue;
                Q step{};
                if(finger>0){
                    float measuredFlex=std::atan2(std::max(0.f,-localDir.y),std::max(0.05f,localDir.x*sign));
                    float targetFlex=curl*(joint==0?1.40f:(joint==1?1.56f:1.18f));
                    float flex=std::lerp(std::lerp(measuredFlex,targetFlex,std::lerp(.65f,.95f,gripTogether)),std::max(measuredFlex,targetFlex*.95f),smoothstep(.55f,.80f,curl));
                    flex*=std::lerp(.25f,1.f,smoothstep(.08f,.25f,curl));
                    V dir=unit({sign*std::cos(flex),-std::sin(flex),joint==0?splayZ:0.f});
                    step=clampAngle(swing(fingerRest,dir,hand.confidence),0.68f);
                }else{
                    Q extended;
                        // Extended thumb (e.g. Thumbs-Up / Thumbs-Down / Open Hand): follow 3D thumb direction on joint 0, keep distal joints straight.
                        if(joint==0){
                            V thumbDir=unit({localDir.x,std::clamp(localDir.y,-0.35f,0.35f),std::max(0.25f,localDir.z)});
                            extended=clampAngle(swing(thumbRest,thumbDir,hand.confidence),0.82f);
                        }else{
                            extended=clampAngle(swing(thumbRest,localDir,hand.confidence),0.96f);
                        }
                        float tAngle=curl*(joint==0?0.65f:(joint==1?0.85f:0.70f));
                        float half=tAngle*0.5f;
                        Q tucked=norm({0.15f*curl,sign*std::sin(half)*0.72f,-sign*std::sin(half)*0.55f,std::cos(half),hand.confidence});
                        step=blend(extended,tucked,smoothstep(.12f,.35f,curl));
                }
                out.bones[8+side*15+finger*3+joint]=step;
                chain=mul(chain,step);
            }
        }
    }
    if(input.hasFace){
        out.hasExpressions=true;
        out.expressions[0]=std::clamp(1.f-input.leftEyeOpen,0.f,1.f);
        out.expressions[1]=std::clamp(1.f-input.rightEyeOpen,0.f,1.f);
        out.expressions[2]=std::clamp(input.mouthOpen,0.f,1.f);
        out.expressions[3]=std::clamp(input.mouthSmile,-1.f,1.f);
        out.expressions[4]=std::clamp(input.brow,-1.f,1.f);
        out.expressions[5]=std::clamp(input.gazeX,-1.f,1.f);
        out.expressions[6]=std::clamp(input.gazeY,-1.f,1.f);
        out.expressions[7]=std::clamp(input.bodySwayX,-1.f,1.f);
    }
    return out;
}
HumanoidMotion solveHumanoid(const HumanoidLandmarks& input){return solveHumanoidImpl(input);}
HumanoidMotion HumanoidMotionSolver::solve(const HumanoidLandmarks& input){
    auto result=solveHumanoidImpl(input,&twistAngles_,&twistTimes_,&twistValid_);
    if(input.hasFace&&result.bones[1].confidence>=.55f&&
       valid(input.faceEyesImage[0])&&valid(input.faceEyesImage[1])){
        lastFaceHead_=result.bones[1];lastFaceEyes_=input.faceEyesImage;
        lastFaceTime_=input.capturedAt;hasFaceHead_=true;
    }else if(!input.hasFace&&hasFaceHead_&&input.capturedAt>=lastFaceTime_&&
             input.capturedAt-lastFaceTime_<=std::chrono::milliseconds(750)){
        const auto covers=[&](int side,int eye){
            const auto& hand=input.hands[side];if(hand.confidence<.8f)return false;
            float x=0,y=0;int count=0;
            for(int i:{0,5,9,13,17})if(valid(hand.image[i])){x+=hand.image[i].x;y+=hand.image[i].y;++count;}
            if(count<3)return false;x/=count;y/=count;float radius=0;
            for(int i:{0,5,9,13,17})if(valid(hand.image[i]))
                radius=std::max(radius,std::hypot(hand.image[i].x-x,hand.image[i].y-y));
            return radius>3&&std::hypot(lastFaceEyes_[eye].x-x,lastFaceEyes_[eye].y-y)<=radius*1.35f;
        };
        if((covers(0,0)&&covers(1,1))||(covers(0,1)&&covers(1,0)))
            result.bones[1]=lastFaceHead_;
    }
    return stabilize_?filter_.update(result,input.capturedAt):result;
}
void HumanoidMotionSolver::reset(){twistAngles_={};twistTimes_={};twistValid_={};filter_.reset();hasFaceHead_=false;lastFaceEyes_={};lastFaceHead_={};lastFaceTime_={};}
void HumanoidMotionFilter::reset(){rotations_={};lastGood_={};valid_={};lastSample_={};initialized_=false;}
HumanoidMotion HumanoidMotionFilter::update(const HumanoidMotion& input,std::chrono::steady_clock::time_point time){
    auto output=input;
    float dt=initialized_?std::chrono::duration<float>(time-lastSample_).count():0.f;
    if(initialized_&&(dt<0||dt>.35f)){reset();dt=0;}
    for(size_t i=0;i<input.bones.size();++i){
        auto target=input.bones[i];
        float length=target.x*target.x+target.y*target.y+target.z*target.z+target.w*target.w;
        if(!std::isfinite(target.confidence)||target.confidence>1.f||target.confidence<0||
           !std::isfinite(length)||length<1e-8f){output.bones[i]={};continue;}
        if(target.confidence<.35f)continue;
        target=norm(target);
        if(!valid_[i]||time-lastGood_[i]>std::chrono::milliseconds(350)){
            rotations_[i]=target;valid_[i]=true;
        }else if(dt>0){
            auto previous=rotations_[i];
            float d=std::abs(previous.x*target.x+previous.y*target.y+previous.z*target.z+previous.w*target.w);
            float angle=114.591559f*std::acos(std::clamp(d,0.f,1.f));
            const bool finger=i>=8, wrist=i==4||i==7;
            float noise=finger?.75f:(wrist?.65f:(i==1?.25f:.40f));
            if(angle>noise){
                // Soft angular dead zone: tiny tremor stays still, but sustained
                // movement is delayed by at most the small noise radius.
                auto quietTarget=blend(previous,target,(angle-noise)/angle);
                float fast=smoothstep(2.f,finger?18.f:25.f,angle);
                float response=std::lerp(finger?.065f:.10f,finger?.015f:.03f,fast);
                rotations_[i]=blend(previous,quietTarget,1.f-std::exp(-std::min(dt,.1f)/response));
            }
        }
        rotations_[i].confidence=target.confidence;
        output.bones[i]=rotations_[i];
        lastGood_[i]=time;
    }
    lastSample_=time;initialized_=true;
    return output;
}
std::vector<uint8_t> encodeMotion(const HumanoidMotion& pose){
    std::vector<uint8_t> out{'C','X','V','R','M','0','0','1'};put(out,1,4);put(out,HumanoidMotion::boneCount,4);put(out,pose.sequence,8);put(out,pose.timestampUs,8);
    for(auto bone:pose.bones)for(float f:{bone.x,bone.y,bone.z,bone.w,bone.confidence})put(out,std::bit_cast<uint32_t>(f),4);
    if(pose.hasExpressions)for(float f:pose.expressions)put(out,std::bit_cast<uint32_t>(f),4);
    return out;
}
bool decodeMotion(std::span<const uint8_t> data,HumanoidMotion& out){
    constexpr std::array<uint8_t,8> magic{'C','X','V','R','M','0','0','1'};
    constexpr size_t baseBytes=32+HumanoidMotion::boneCount*20;
    constexpr size_t extBytes=baseBytes+HumanoidMotion::expressionCount*4;
    if((data.size()!=baseBytes&&data.size()!=extBytes)||!std::equal(magic.begin(),magic.end(),data.begin()))return false;
    size_t offset=8;if(get(data,offset,4)!=1||get(data,offset,4)!=HumanoidMotion::boneCount)return false;
    HumanoidMotion pose;pose.sequence=get(data,offset,8);pose.timestampUs=get(data,offset,8);
    for(auto& b:pose.bones){float* fields[]{&b.x,&b.y,&b.z,&b.w,&b.confidence};for(float* f:fields){*f=std::bit_cast<float>(static_cast<uint32_t>(get(data,offset,4)));if(!std::isfinite(*f))return false;}
        float length=b.x*b.x+b.y*b.y+b.z*b.z+b.w*b.w;if(length<.99f||length>1.01f||b.confidence<0||b.confidence>1)return false;}
    if(data.size()==extBytes){
        pose.hasExpressions=true;
        for(float& e:pose.expressions){e=std::bit_cast<float>(static_cast<uint32_t>(get(data,offset,4)));if(!std::isfinite(e))return false;}
    }
    out=pose;return true;
}
}
