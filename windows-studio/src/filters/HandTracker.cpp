// Independent implementation of OpenCV Zoo / MediaPipe Hands preprocessing.
// Model/source provenance: third_party/MediaPipeHands/README.md (Apache-2.0).
#include "cortex/filters/HandTracker.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#ifdef CORTEX_HAS_DIRECTML
#include <onnxruntime_cxx_api.h>
namespace cortex {
namespace {
struct P { float x{}, y{}; };
struct Palm { std::array<P,7> points; float score{}, x{}, y{}, w{}, h{}; };
float sigmoid(float x) { return 1.f/(1.f+std::exp(-std::clamp(x,-80.f,80.f))); }
std::array<float,3> sample(std::span<const uint8_t> rgba, unsigned w,unsigned h,float x,float y) {
    if (!std::isfinite(x)||!std::isfinite(y)||x<0||y<0||x>w-1.f||y>h-1.f) return {};
    unsigned ix=static_cast<unsigned>(x), iy=static_cast<unsigned>(y);
    unsigned jx=std::min(ix+1,w-1), jy=std::min(iy+1,h-1);
    std::array<float,3> p{};
    for (size_t c=0;c<3;++c) {
        auto at=[&](unsigned a,unsigned b){return rgba[(size_t(b)*w+a)*4+c]/255.f;};
        p[c]=std::lerp(std::lerp(at(ix,iy),at(jx,iy),x-ix),std::lerp(at(ix,jy),at(jx,jy),x-ix),y-iy);
    }
    return p;
}
std::vector<Ort::Value> run(Ort::Session& session,std::vector<float>& data,int size) {
    std::array<int64_t,4> shape{1,size,size,3};
    auto memory=Ort::MemoryInfo::CreateCpu(OrtArenaAllocator,OrtMemTypeDefault);
    auto tensor=Ort::Value::CreateTensor<float>(memory,data.data(),data.size(),shape.data(),4);
    Ort::AllocatorWithDefaultOptions allocator;
    auto input=session.GetInputNameAllocated(0,allocator); const char* name=input.get();
    std::vector<Ort::AllocatedStringPtr> owners; std::vector<const char*> names;
    for (size_t i=0;i<session.GetOutputCount();++i) { owners.push_back(session.GetOutputNameAllocated(i,allocator)); names.push_back(owners.back().get()); }
    return session.Run(Ort::RunOptions{nullptr},&name,&tensor,1,names.data(),names.size());
}
}
struct HandTracker::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING,"cortex-hands"};
    Ort::Session palm{nullptr}, hand{nullptr};
    std::vector<P> anchors;
    std::vector<float> input;
};
HandTracker::HandTracker(const std::filesystem::path& path):impl_(std::make_unique<Impl>()) {
    auto& s=*impl_; Ort::SessionOptions options;
    options.SetIntraOpNumThreads(2); options.SetInterOpNumThreads(1);
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    s.palm=Ort::Session(s.env,(path/"palm_detection_mediapipe_2023feb.onnx").c_str(),options);
    s.hand=Ort::Session(s.env,(path/"handpose_estimation_mediapipe_2023feb.onnx").c_str(),options);
    for (auto [grid,repeats]:{std::pair{24,2},std::pair{12,6}})
        for(int y=0;y<grid;++y) for(int x=0;x<grid;++x) for(int i=0;i<repeats;++i)
            s.anchors.push_back({(x+.5f)/grid,(y+.5f)/grid});
}
HandTracker::~HandTracker()=default;
std::vector<HandLandmarks> HandTracker::track(std::span<const uint8_t> rgba,unsigned w,unsigned h) {
    if(!w||!h||rgba.size()!=size_t(w)*h*4) throw std::invalid_argument("Invalid hand frame");
    auto& s=*impl_; constexpr int d=192, p=224;
    const float scale=static_cast<float>(std::max(w,h)), ratio=d/scale;
    const int rw=static_cast<int>(w*ratio),rh=static_cast<int>(h*ratio),left=(d-rw)/2,top=(d-rh)/2;
    s.input.assign(d*d*3,0.f);
    for(int y=0;y<rh;++y)for(int x=0;x<rw;++x){
        auto rgb=sample(rgba,w,h,(x+.5f)/ratio-.5f,(y+.5f)/ratio-.5f);
        for(int c=0;c<3;++c)s.input[((y+top)*d+x+left)*3+c]=rgb[c];
    }
    auto output=run(s.palm,s.input,d); const float* scores=nullptr; const float* boxes=nullptr;
    for(auto& out:output){auto shape=out.GetTensorTypeAndShapeInfo().GetShape();
        if(shape==std::vector<int64_t>{1,2016,1})scores=out.GetTensorData<float>();
        if(shape==std::vector<int64_t>{1,2016,18})boxes=out.GetTensorData<float>();}
    if(!scores||!boxes)throw std::runtime_error("Unexpected palm model outputs");
    std::vector<Palm> candidates, selected;
    for(size_t i=0;i<s.anchors.size();++i){
        const float score=sigmoid(scores[i]); if(!std::isfinite(score)||score<.65f)continue;
        const float* b=boxes+i*18; Palm palm{}; palm.score=score;
        const float cx=(b[0]/d+s.anchors[i].x)*scale-left/ratio,cy=(b[1]/d+s.anchors[i].y)*scale-top/ratio;
        palm.w=b[2]/d*scale;palm.h=b[3]/d*scale;palm.x=cx-palm.w/2;palm.y=cy-palm.h/2;
        if(!std::isfinite(cx)||!std::isfinite(cy)||palm.w<4||palm.h<4)continue;
        for(int j=0;j<7;++j)palm.points[j]={(b[4+j*2]/d+s.anchors[i].x)*scale-left/ratio,(b[5+j*2]/d+s.anchors[i].y)*scale-top/ratio};
        candidates.push_back(palm);
    }
    std::sort(candidates.begin(),candidates.end(),[](auto& a,auto& b){return a.score>b.score;});
    for(auto& a:candidates){bool duplicate=false;for(auto& b:selected){
        float iw=std::max(0.f,std::min(a.x+a.w,b.x+b.w)-std::max(a.x,b.x));
        float ih=std::max(0.f,std::min(a.y+a.h,b.y+b.h)-std::max(a.y,b.y));
        if(iw*ih/(a.w*a.h+b.w*b.h-iw*ih)>.3f)duplicate=true;
    }if(!duplicate)selected.push_back(a);if(selected.size()==2)break;}
    std::vector<HandLandmarks> result;
    for(const auto& palm:selected){
        // Rotate the palm landmarks into an upright local frame. Equivalent to
        // Zoo's rotation+crop, sampled directly from the original RGB image.
        P up{palm.points[2].x-palm.points[0].x,palm.points[2].y-palm.points[0].y};
        float length=std::hypot(up.x,up.y); if(!std::isfinite(length)||length<3)continue;
        up.x/=length;up.y/=length; P right{-up.y,up.x};
        P origin{palm.x+palm.w/2,palm.y+palm.h/2};
        float minX=1e9f,minY=1e9f,maxX=-1e9f,maxY=-1e9f;
        for(auto pt:palm.points){float dx=pt.x-origin.x,dy=pt.y-origin.y;
            float x=dx*right.x+dy*right.y,y=-(dx*up.x+dy*up.y);
            minX=std::min(minX,x);maxX=std::max(maxX,x);minY=std::min(minY,y);maxY=std::max(maxY,y);}
        float cx=(minX+maxX)/2,cy=(minY+maxY)/2-.4f*(maxY-minY);
        float side=3*std::max(maxX-minX,maxY-minY); if(!std::isfinite(side)||side<8||side>scale*4)continue;
        P center{origin.x+right.x*cx-up.x*cy,origin.y+right.y*cx-up.y*cy};
        s.input.resize(p*p*3);
        for(int y=0;y<p;++y)for(int x=0;x<p;++x){
            float a=((x+.5f)/p-.5f)*side,b=((y+.5f)/p-.5f)*side;
            auto rgb=sample(rgba,w,h,center.x+right.x*a-up.x*b,center.y+right.y*a-up.y*b);
            for(int c=0;c<3;++c)s.input[(y*p+x)*3+c]=rgb[c];}
        auto handOutput=run(s.hand,s.input,p);
        // The pinned ONNX export orders: screen landmarks, confidence,
        // handedness, metric world landmarks. Verify shapes before reading.
        if(handOutput.size()!=4)throw std::runtime_error("Unexpected hand output count");
        if(handOutput[0].GetTensorTypeAndShapeInfo().GetElementCount()!=63||
           handOutput[1].GetTensorTypeAndShapeInfo().GetElementCount()!=1||
           handOutput[2].GetTensorTypeAndShapeInfo().GetElementCount()!=1||
           handOutput[3].GetTensorTypeAndShapeInfo().GetElementCount()!=63)
            throw std::runtime_error("Unexpected hand output shapes");
        HandLandmarks hand;hand.confidence=handOutput[1].GetTensorData<float>()[0];
        hand.rightProbability=handOutput[2].GetTensorData<float>()[0];
        if(!std::isfinite(hand.confidence)||hand.confidence<.8f)continue;
        auto screen=handOutput[0].GetTensorData<float>(); auto world=handOutput[3].GetTensorData<float>();
        bool valid=true;
        for(int i=0;i<21;++i){float x=(screen[i*3]/p-.5f)*side,y=(screen[i*3+1]/p-.5f)*side;
            hand.image[i]={center.x+right.x*x-up.x*y,center.y+right.y*x-up.y*y,screen[i*3+2]*side/p,hand.confidence};
            // Unity Y up and positive Z toward the camera; world output X/Y are crop-relative.
            hand.world[i]={right.x*world[i*3]-up.x*world[i*3+1],-(right.y*world[i*3]-up.y*world[i*3+1]),-world[i*3+2],hand.confidence};
            const auto& a=hand.world[i];valid=valid&&std::isfinite(a.x)&&std::isfinite(a.y)&&std::isfinite(a.z);}
        if(valid)result.push_back(hand);
    }
    return result;
}
}
#else
namespace cortex {
struct HandTracker::Impl {};
HandTracker::HandTracker(const std::filesystem::path&){throw std::runtime_error("Hands require ONNX Runtime");}
HandTracker::~HandTracker()=default;
std::vector<HandLandmarks> HandTracker::track(std::span<const uint8_t>,unsigned,unsigned){return {};}
}
#endif
