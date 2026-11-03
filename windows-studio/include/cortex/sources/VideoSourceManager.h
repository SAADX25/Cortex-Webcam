#pragma once
#include "cortex/sources/IVideoSource.h"
#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace cortex {
enum class SourceId { Phone, UsbWebcam };
struct NormalizedRect { float x{},y{},width{1},height{1}; };
struct SceneLayer { SourceId source; FramePtr frame; NormalizedRect destination; };
struct SceneSnapshot { std::vector<SceneLayer> layers; };

// Render thread pulls a snapshot; source threads never call the UI/compositor.
class VideoSourceManager {
public:
    explicit VideoSourceManager(std::shared_ptr<GpuDevice> gpu);
    ~VideoSourceManager();
    void add(SourceId id,std::shared_ptr<IVideoSource> source);
    void startAll();
    void stopAll() noexcept;
    void select(SourceId main);
    void setPictureInPicture(std::optional<SourceId> inset,NormalizedRect rect={0.68f,0.68f,0.30f,0.30f});
    SceneSnapshot snapshot(std::chrono::milliseconds maxAge=std::chrono::milliseconds(250)) const;
private:
    std::shared_ptr<GpuDevice> gpu_;
    mutable std::mutex mutex_;
    std::map<SourceId,std::shared_ptr<IVideoSource>> sources_;
    SourceId main_{SourceId::Phone};
    std::optional<SourceId> inset_;
    NormalizedRect insetRect_;
};
}
