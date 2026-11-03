#pragma once
#include "cortex/core/VideoFrame.h"
#include <filesystem>
namespace cortex {
// Local-file GPU decode, latest-frame publication, timestamp-paced looping.
class BackgroundVideo {
public:
    BackgroundVideo(std::shared_ptr<GpuDevice>,const std::filesystem::path&);
    ~BackgroundVideo();
    FramePtr latestFrame()const;
    std::string error()const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
