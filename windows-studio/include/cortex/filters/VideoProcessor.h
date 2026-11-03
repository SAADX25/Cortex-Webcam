#pragma once
#include "cortex/sources/VideoSourceManager.h"
#include <filesystem>
#include "cortex/filters/AvatarFilter.h"
namespace cortex {
struct GpuImage {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    uint32_t width{}, height{};
};
enum class RemovalMode { None, ChromaKey, AiMatting, BackgroundOnly };
struct FilterSettings {
    RemovalMode removal{RemovalMode::ChromaKey};
    float similarity{0.13f}, softness{0.10f}, spill{0.8f};
    bool transparent{false};
    bool avatarEnabled{}, avatarTestMotion{};
    int avatarMode{0};
    float backgroundZoom{1}, backgroundX{.5f}, backgroundY{.5f};
    bool backgroundFit{false};
    float background[3]{0.04f, 0.04f, 0.04f};
};
class RvmMatting;
class VideoProcessor {
public:
    VideoProcessor(std::shared_ptr<GpuDevice>, uint32_t width = 1920, uint32_t height = 1080);
    ~VideoProcessor();
    void setSettings(const FilterSettings&);
    // Configure from the processing thread before rendering the next frame.
    void setAvatarModel(std::unique_ptr<AvatarModel>);
    void setFaceTracker(std::unique_ptr<FaceTracker>);
    void setOutputSize(uint32_t width, uint32_t height);
    void setBackgroundImage(const std::filesystem::path&);
    void clearBackground();
    void
    setMattingModel(const std::filesystem::path&);  // One independent recurrent state per source.
    GpuImage render(const SceneSnapshot&, FramePtr backgroundVideo = {});
    // UnityCapture consumes bottom-up RGBA; preview and other exports stay top-down.
    GpuImage exportFrame(bool preserveAlpha = false,
                         bool bottomUp = false);  // Straight RGBA or flattened opaque black.
    const FilterSettings& settings() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Converts decoder-pool NV12 textures/array slices to renderable RGB entirely on GPU.
class GpuColorConverter {
public:
    explicit GpuColorConverter(std::shared_ptr<GpuDevice>);
    ~GpuColorConverter();
    GpuImage convert(const VideoFrame&);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace cortex
