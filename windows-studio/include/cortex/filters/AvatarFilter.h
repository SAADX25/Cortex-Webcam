#pragma once
#include "cortex/core/VideoFrame.h"
#include <optional>
#include <filesystem>
#include <string>

namespace cortex {
// Position offsets are fractions of the output frame; positive Y moves down.
struct AvatarFraming {
    float scale{1}, x{}, y{};
};
struct HeadRotation {
    float yaw{}, pitch{}, roll{};
};
struct ArmPose {
    float lift{}, bend{}, confidence{};
};
struct UpperBodyPose {
    ArmPose left, right;
    float confidence{};
};
struct AvatarPose {
    float yaw{}, pitch{}, roll{}, mouth{}, eyeOpen{1}, gazeX{}, gazeY{};
    float leftEyeOpen{-1}, rightEyeOpen{-1};
    bool cameraDriven{};
    bool armsEnabled{};
    UpperBodyPose arms;
};

struct LandmarkPoint {
    float x{}, y{};
};
// Coordinates in upright display pixels, with left/right ordered from the viewer.
struct FaceLandmarks {
    LandmarkPoint leftEye, rightEye, nose, upperLip, lowerLip;
    float eyeOpen{1}, gazeX{}, gazeY{};
    float leftEyeOpen{-1}, rightEyeOpen{-1};
    std::optional<HeadRotation> headRotation;
    bool needsHeadFit{};
};

class FaceTracker {
public:
    virtual ~FaceTracker() = default;
    virtual std::optional<FaceLandmarks> track(const VideoFrame&) = 0;
    virtual std::string status() const { return "Face tracking"; }
    virtual void setBodyTracking(bool) {}
    virtual std::optional<UpperBodyPose> upperBody() const { return {}; }
};

class AvatarModel {
public:
    virtual ~AvatarModel() = default;
    virtual void draw(ID3D11DeviceContext*, const AvatarPose&, unsigned width, unsigned height) = 0;
    virtual void setAnimation(int motion, int expression) = 0;
    virtual void setFraming(const AvatarFraming&) = 0;
};

bool isCubismSupported();
std::unique_ptr<AvatarModel> loadAvatarModel(std::shared_ptr<GpuDevice> gpu,
                                             const std::filesystem::path& path);
std::unique_ptr<FaceTracker> makeFaceTracker(std::shared_ptr<GpuDevice> gpu = nullptr,
                                             const std::filesystem::path& modelDirectory = {});

// Frame-rate independent low-pass filtering; dt and response are seconds.
AvatarPose smoothAvatarPose(const AvatarPose& previous, const AvatarPose& target, float dt,
                            float response);

class AvatarFilter {
public:
    explicit AvatarFilter(std::shared_ptr<GpuDevice>);
    static std::optional<AvatarPose> map(const FaceLandmarks&);
    static AvatarPose testPose(double seconds);
    // Called on the render thread with the compositing target and fullscreen VS bound.
    void draw(const AvatarPose&, unsigned width, unsigned height,
              const AvatarFraming& framing = {});

private:
    std::shared_ptr<GpuDevice> gpu_;
    ComPtr<ID3D11PixelShader> shader_;
    ComPtr<ID3D11Buffer> constants_;
};
}  // namespace cortex
