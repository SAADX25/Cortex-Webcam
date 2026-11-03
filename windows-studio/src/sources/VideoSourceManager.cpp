#include "cortex/sources/VideoSourceManager.h"
#include <cmath>
#include <stdexcept>

namespace cortex {
VideoSourceManager::VideoSourceManager(std::shared_ptr<GpuDevice> gpu)
    : gpu_(std::move(gpu)) {
    if (!gpu_)
        throw std::invalid_argument("A shared GPU device is required");
}
VideoSourceManager::~VideoSourceManager() {
    stopAll();
}
void VideoSourceManager::add(SourceId id, std::shared_ptr<IVideoSource> source) {
    if (!source || source->gpuDevice() != gpu_)
        throw std::invalid_argument("Source must use the manager's shared GPU");
    std::lock_guard lock(mutex_);
    if (!sources_.emplace(id, std::move(source)).second)
        throw std::invalid_argument("Source already registered");
}
void VideoSourceManager::startAll() {
    std::vector<std::shared_ptr<IVideoSource>> sources;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [id, source] : sources_)
            sources.push_back(source);
    }
    try {
        for (auto& source : sources)
            source->start();
    } catch (...) {
        for (auto& source : sources)
            source->stop();
        throw;
    }
}
void VideoSourceManager::stopAll() noexcept {
    std::vector<std::shared_ptr<IVideoSource>> sources;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [id, source] : sources_)
            sources.push_back(source);
    }
    for (auto& source : sources)
        source->stop();
}
void VideoSourceManager::select(SourceId main) {
    std::lock_guard lock(mutex_);
    if (!sources_.contains(main))
        throw std::invalid_argument("Source is not registered");
    main_ = main;
}
void VideoSourceManager::setPictureInPicture(std::optional<SourceId> inset, NormalizedRect r) {
    if (!std::isfinite(r.x) || !std::isfinite(r.y) || !std::isfinite(r.width) ||
        !std::isfinite(r.height) || r.x < 0 || r.y < 0 || r.width <= 0 || r.height <= 0 ||
        r.x + r.width > 1 || r.y + r.height > 1)
        throw std::invalid_argument("PiP rectangle must be inside the unit viewport");
    std::lock_guard lock(mutex_);
    if (inset && !sources_.contains(*inset))
        throw std::invalid_argument("PiP source is not registered");
    inset_ = inset;
    insetRect_ = r;
}
SceneSnapshot VideoSourceManager::snapshot(std::chrono::milliseconds maxAge) const {
    std::shared_ptr<IVideoSource> mainSource, insetSource;
    SourceId main;
    std::optional<SourceId> inset;
    NormalizedRect rect;
    {
        std::lock_guard lock(mutex_);
        main = main_;
        inset = inset_;
        rect = insetRect_;
        if (auto i = sources_.find(main); i != sources_.end())
            mainSource = i->second;
        if (inset && *inset != main)
            if (auto i = sources_.find(*inset); i != sources_.end())
                insetSource = i->second;
    }
    SceneSnapshot result;
    const auto now = std::chrono::steady_clock::now();
    auto append = [&](SourceId id, const auto& source, NormalizedRect destination) {
        if (!source)
            return;
        auto frame = source->latestFrame();
        if (frame && now - frame->receivedAt <= maxAge)
            result.layers.push_back({id, std::move(frame), destination});
    };
    append(main, mainSource, {});
    if (inset)
        append(*inset, insetSource, rect);
    return result;
}
}  // namespace cortex
