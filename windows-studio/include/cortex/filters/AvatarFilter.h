#pragma once
#include "cortex/core/VideoFrame.h"
#include <optional>
#include <filesystem>
#ifdef CORTEX_HAS_CUBISM
namespace Live2D::Cubism::Framework {
class CubismUserModel;
}
#endif

namespace cortex {
struct AvatarPose {
    float yaw{}, pitch{}, roll{}, mouth{}, eyeOpen{1}, gazeX{}, gazeY{};
};

struct LandmarkPoint {
    float x{}, y{};
};
// Coordinates in upright display pixels, with left/right ordered from the viewer.
struct FaceLandmarks {
    LandmarkPoint leftEye, rightEye, nose, upperLip, lowerLip;
    float eyeOpen{1}, gazeX{}, gazeY{};
};

class FaceTracker {
public:
    virtual ~FaceTracker() = default;
    virtual std::optional<FaceLandmarks> track(const VideoFrame&) = 0;
};

class AvatarModel {
public:
    virtual ~AvatarModel() = default;
    virtual void draw(ID3D11DeviceContext*, const AvatarPose&, unsigned width, unsigned height) = 0;
};

#ifdef CORTEX_HAS_CUBISM
// The asset owner initializes Cubism and supplies a textured renderer on the processor device.
std::unique_ptr<AvatarModel>
makeCubismAvatar(std::shared_ptr<Live2D::Cubism::Framework::CubismUserModel> model);
#endif

bool isCubismSupported();
std::unique_ptr<AvatarModel> loadAvatarModel(std::shared_ptr<GpuDevice> gpu,
                                             const std::filesystem::path& path);
std::unique_ptr<FaceTracker> makeFaceTracker(std::shared_ptr<GpuDevice> gpu = nullptr);

class AvatarFilter {
public:
    explicit AvatarFilter(std::shared_ptr<GpuDevice>);
    static std::optional<AvatarPose> map(const FaceLandmarks&);
    static AvatarPose testPose(double seconds);
    // Called on the render thread with the compositing target and fullscreen VS bound.
    void draw(const AvatarPose&, unsigned width, unsigned height);

private:
    std::shared_ptr<GpuDevice> gpu_;
    ComPtr<ID3D11PixelShader> shader_;
    ComPtr<ID3D11Buffer> constants_;
};
}  // namespace cortex
