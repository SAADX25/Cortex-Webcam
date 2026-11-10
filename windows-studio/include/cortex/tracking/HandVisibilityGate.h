#pragma once
#include "cortex/tracking/HandTracker.h"
#include <chrono>
#include <cmath>

namespace cortex {
// Runs once per Hands inference. A lone false detection must not wake an occluded arm.
class HandVisibilityGate {
    struct State { unsigned count{}; std::chrono::steady_clock::time_point start{},last{}; };
    std::array<State,2> states_{};
    unsigned width_{},height_{};
public:
    void reset() noexcept { states_={};width_=height_=0; }
    void update(std::array<HandLandmarks,2>& hands,unsigned width,unsigned height,
                std::chrono::steady_clock::time_point now) {
        if(width!=width_ || height!=height_){reset();width_=width;height_=height;}
        for(int side=0;side<2;++side){
            auto& hand=hands[side];auto& state=states_[side];
            unsigned visible=0;
            for(int joint:{0,5,9,17}){
                const auto& p=hand.image[joint];
                if(std::isfinite(p.x)&&std::isfinite(p.y)&&p.confidence>.1f&&p.x>=0&&p.x<width&&p.y>=0&&p.y<height)++visible;
            }
            const auto& wrist=hand.image[0];
            bool valid=std::isfinite(hand.confidence)&&hand.confidence>=.8f&&hand.confidence<=1&&visible>=3&&
                wrist.x>=0&&wrist.x<width&&wrist.y>=0&&wrist.y<height;
            if(!valid){state={};hand.confidence=0;continue;}
            if(!state.count || now<state.last || now-state.last>std::chrono::milliseconds(500))
                state={1,now,now};
            else if(now>state.last){++state.count;state.last=now;}
            if(state.count<3 || now-state.start<std::chrono::milliseconds(60))hand.confidence=0;
        }
    }
};
}
