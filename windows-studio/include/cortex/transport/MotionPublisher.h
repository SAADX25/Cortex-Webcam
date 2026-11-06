#pragma once
#include "cortex/tracking/HumanoidMotion.h"
#include <memory>
namespace cortex {
class MotionPublisher {
public:
    explicit MotionPublisher(uint16_t port);
    ~MotionPublisher();
    bool publish(HumanoidMotion);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
