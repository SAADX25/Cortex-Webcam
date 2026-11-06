#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <vector>

namespace cortex {
struct Joint3D { float x{}, y{}, z{}, confidence{}; };
struct HandLandmarks {
    // Upright camera pixels (Z relative to wrist), and wrist-relative model meters.
    std::array<Joint3D, 21> image{}, world{};
    float confidence{}, rightProbability{};
};
struct HumanoidLandmarks {
    // Unity axes: X right, Y up, Z toward the camera. Model-inferred, not depth sensor data.
    std::array<Joint3D, 33> body{};
    std::array<Joint3D, 33> bodyImage{}; // Upright pixels for associating hand wrists.
    std::array<HandLandmarks, 2> hands{}; // Anatomical left/right, never display-mirrored.
    std::array<float, 2> handOpen{1.f, 1.f}, handOpenConfidence{};
    std::array<float, 2> wristBend{}, wristConfidence{};
    bool hasFace{};
    std::array<Joint3D, 2> faceEyesImage{}; // Upright pixels, used only for brief face occlusion bridging.
    float headYaw{}, headPitch{}, headRoll{};
    float leftEyeOpen{1.f}, rightEyeOpen{1.f};
    float mouthOpen{}, mouthSmile{}, brow{};
    float gazeX{}, gazeY{}, bodySwayX{};
    std::chrono::steady_clock::time_point capturedAt{};
    bool handsAvailable{};
};
struct BoneRotation { float x{}, y{}, z{}, w{1}, confidence{}; };
struct ArmIkResult { Joint3D elbow, wrist; bool valid{}; };
ArmIkResult solveTwoBoneIK(Joint3D shoulder, Joint3D target, Joint3D elbowPole,
                          float upperLength, float lowerLength);
// Stable wire IDs: chest/head, left upper/lower/hand, right upper/lower/hand,
// then thumb/index/middle/ring/little, proximal/intermediate/distal, left then right.
struct HumanoidMotion {
    static constexpr size_t boneCount = 38;
    static constexpr size_t expressionCount = 8;
    std::array<BoneRotation, boneCount> bones{};
    // [0]=blinkL, [1]=blinkR, [2]=mouthOpen, [3]=mouthSmile, [4]=brow, [5]=gazeX, [6]=gazeY, [7]=bodySwayX
    std::array<float, expressionCount> expressions{};
    bool hasExpressions{};
    uint64_t sequence{}, timestampUs{};
};
HumanoidMotion solveHumanoid(const HumanoidLandmarks&);
class HumanoidMotionFilter {
public:
    HumanoidMotion update(const HumanoidMotion&, std::chrono::steady_clock::time_point);
    void reset();
private:
    std::array<BoneRotation, HumanoidMotion::boneCount> rotations_{};
    std::array<std::chrono::steady_clock::time_point, HumanoidMotion::boneCount> lastGood_{};
    std::array<bool, HumanoidMotion::boneCount> valid_{};
    std::chrono::steady_clock::time_point lastSample_{};
    bool initialized_{};
};
// One instance per stream, serialized by its owner. Unwraps wrist twist across
// +/-180 degrees so distributing it along the forearm cannot reverse suddenly.
class HumanoidMotionSolver {
public:
    explicit HumanoidMotionSolver(bool stabilize = true) : stabilize_(stabilize) {}
    HumanoidMotion solve(const HumanoidLandmarks&);
    void reset();
private:
    std::array<float, 2> twistAngles_{};
    std::array<std::chrono::steady_clock::time_point, 2> twistTimes_{};
    std::array<bool, 2> twistValid_{};
    HumanoidMotionFilter filter_;
    bool stabilize_{true};
    BoneRotation lastFaceHead_{};
    std::array<Joint3D, 2> lastFaceEyes_{};
    std::chrono::steady_clock::time_point lastFaceTime_{};
    bool hasFaceHead_{};
};
std::vector<uint8_t> encodeMotion(const HumanoidMotion&);
bool decodeMotion(std::span<const uint8_t>, HumanoidMotion&);
} // namespace cortex
