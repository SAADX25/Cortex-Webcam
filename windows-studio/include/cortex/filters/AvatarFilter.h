#pragma once
#include "cortex/core/VideoFrame.h"
#include <optional>
#include <filesystem>
#include <string>
#include <stop_token>

namespace cortex {
struct HumanoidLandmarks;
// Position offsets are fractions of the output frame; positive Y moves down.
struct AvatarFraming {
    float scale{1}, x{}, y{};
};
struct HeadRotation {
    float yaw{}, pitch{}, roll{};
};
struct ArmPose {
    float lift{}, bend{}, confidence{};
    // Wrist bend relative to the forearm in [-1, 1] (±90°) and hand openness in
    // [0, 1] (0 = fist, 1 = spread). handConfidence 0 means "not measured".
    float wrist{}, hand{.5f}, handConfidence{};
};
struct UpperBodyPose {
    ArmPose left, right;
    float confidence{};
    float bodyAngleX{}, bodyAngleY{}, bodyAngleZ{};
    // Body inference runs independently of face inference; use its own sample clock.
    std::chrono::steady_clock::time_point capturedAt;
    std::shared_ptr<const HumanoidLandmarks> landmarks3D;
};
struct AvatarPose {
    float yaw{}, pitch{}, roll{}, mouth{}, eyeOpen{1}, gazeX{}, gazeY{};
    float leftEyeOpen{-1}, rightEyeOpen{-1};
    float browLY{}, browRY{};
    float mouthForm{};
    float bodyAngleX{}, bodyAngleY{}, bodyAngleZ{};
    bool cameraDriven{};
    bool armsEnabled{};
    UpperBodyPose arms;
    // Fractions of the output frame; positive X is right and positive Y is down.
    float positionX{}, positionY{};
    float depthScale{1}, headDepth{};
};

struct LandmarkPoint {
    float x{}, y{};
};
// Coordinates in upright display pixels, with left/right ordered from the viewer.
struct FaceLandmarks {
    LandmarkPoint leftEye, rightEye, nose, upperLip, lowerLip;
    float eyeOpen{1}, gazeX{}, gazeY{};
    float leftEyeOpen{-1}, rightEyeOpen{-1};
    float browLY{}, browRY{};
    float mouthOpenY{}, mouthForm{};
    std::optional<HeadRotation> headRotation;
    bool needsHeadFit{};
    // Upright image dimensions for normalized face movement. Zero means unavailable.
    unsigned imageWidth{}, imageHeight{};
    float headDepth{};
    bool hasHeadPosition{};
    float headPositionX{}, headPositionY{};
    std::chrono::steady_clock::time_point capturedAt;
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
                                             const std::filesystem::path& path,
                                             std::stop_token stop = {});
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
