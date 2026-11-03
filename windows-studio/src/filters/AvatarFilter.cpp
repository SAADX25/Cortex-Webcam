#include "cortex/filters/AvatarFilter.h"
#include "GpuHelpers.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#ifdef CORTEX_HAS_CUBISM
#include <Model/CubismUserModel.hpp>
#include <Rendering/D3D11/CubismRenderer_D3D11.hpp>
#include <Id/CubismIdManager.hpp>
#include <Math/CubismModelMatrix.hpp>
#include <stdexcept>
#endif

namespace cortex {
#ifdef CORTEX_HAS_CUBISM
namespace {
namespace cubism = Live2D::Cubism::Framework;
using Renderer = cubism::Rendering::CubismRenderer_D3D11;

class CubismAvatar final : public AvatarModel {
public:
    explicit CubismAvatar(std::shared_ptr<cubism::CubismUserModel> asset)
        : asset_(std::move(asset)) {
        if (!asset_ || !asset_->GetModel() || !asset_->GetRenderer<Renderer>())
            throw std::invalid_argument("Cubism model and renderer must be initialized");
    }

    void draw(ID3D11DeviceContext* context, const AvatarPose& pose, unsigned width,
              unsigned height) override {
        auto* model = asset_->GetModel();
        const auto set = [&](const char* key, float value) {
            model->SetParameterValue(cubism::CubismFramework::GetIdManager()->GetId(key), value);
        };
        set("ParamAngleX", pose.yaw);
        set("ParamAngleY", pose.pitch);
        set("ParamAngleZ", pose.roll);
        set("ParamEyeLOpen", pose.eyeOpen);
        set("ParamEyeROpen", pose.eyeOpen);
        set("ParamEyeBallX", pose.gazeX);
        set("ParamEyeBallY", pose.gazeY);
        set("ParamMouthOpenY", pose.mouth);
        model->Update();

        D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
        context->RSSetViewports(1, &viewport);
        cubism::CubismMatrix44 projection;
        const float aspect = float(width) / float(height);
        projection.Scale(std::min(1.f, 1.f / aspect), std::min(1.f, aspect));
        projection.MultiplyByMatrix(asset_->GetModelMatrix());
        auto* renderer = asset_->GetRenderer<Renderer>();
        renderer->StartFrame(context);
        renderer->SetMvpMatrix(&projection);
        renderer->DrawModel();
        renderer->EndFrame();
    }

private:
    std::shared_ptr<cubism::CubismUserModel> asset_;
};
}  // namespace
std::unique_ptr<AvatarModel> makeCubismAvatar(std::shared_ptr<cubism::CubismUserModel> model) {
    return std::make_unique<CubismAvatar>(std::move(model));
}
#endif

namespace {
class BuiltinFaceTracker final : public FaceTracker {
public:
    explicit BuiltinFaceTracker(std::shared_ptr<GpuDevice> gpu)
        : gpu_(std::move(gpu)) {}

    std::optional<FaceLandmarks> track(const VideoFrame& frame) override {
        if (!frame.texture || !frame.width || !frame.height)
            return std::nullopt;
        auto gpu = frame.gpu ? frame.gpu : gpu_;
        if (!gpu || !gpu->device || !gpu->context)
            return std::nullopt;

        std::lock_guard lock(gpu->mutex);
        D3D11_TEXTURE2D_DESC desc{};
        frame.texture->GetDesc(&desc);
        if (desc.Format != DXGI_FORMAT_NV12)
            return std::nullopt;

        if (!staging_ || stagingWidth_ != desc.Width || stagingHeight_ != desc.Height) {
            staging_.Reset();
            D3D11_TEXTURE2D_DESC sDesc{};
            sDesc.Width = desc.Width;
            sDesc.Height = desc.Height;
            sDesc.MipLevels = 1;
            sDesc.ArraySize = 1;
            sDesc.Format = DXGI_FORMAT_NV12;
            sDesc.SampleDesc.Count = 1;
            sDesc.Usage = D3D11_USAGE_STAGING;
            sDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(gpu->device->CreateTexture2D(&sDesc, nullptr, &staging_))) {
                return std::nullopt;
            }
            stagingWidth_ = desc.Width;
            stagingHeight_ = desc.Height;
        }

        auto* context = gpu->context.Get();
        context->CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0, frame.texture.Get(),
                                       frame.arraySlice, nullptr);

        D3D11_MAPPED_SUBRESOURCE map{};
        if (FAILED(context->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &map))) {
            return std::nullopt;
        }

        const uint8_t* yPlane = static_cast<const uint8_t*>(map.pData);
        const UINT pitch = map.RowPitch;
        const uint32_t w = frame.width;
        const uint32_t h = frame.height;

        const uint32_t stepX = std::max(1u, w / 80u);
        const uint32_t stepY = std::max(1u, h / 60u);

        double sumX = 0, sumY = 0, totalW = 0;
        double sumX2 = 0, sumY2 = 0, sumXY = 0;

        for (uint32_t y = 0; y < h; y += stepY) {
            const uint8_t* row = yPlane + y * pitch;
            for (uint32_t x = 0; x < w; x += stepX) {
                uint8_t val = row[x];
                if (val >= 40 && val <= 220) {
                    double weight = double(val);
                    sumX += x * weight;
                    sumY += y * weight;
                    sumX2 += double(x) * x * weight;
                    sumY2 += double(y) * y * weight;
                    sumXY += double(x) * y * weight;
                    totalW += weight;
                }
            }
        }

        context->Unmap(staging_.Get(), 0);

        if (totalW < 1.0) {
            return std::nullopt;
        }

        float cx = static_cast<float>(sumX / totalW);
        float cy = static_cast<float>(sumY / totalW);

        double mu20 = (sumX2 / totalW) - (cx * cx);
        double mu02 = (sumY2 / totalW) - (cy * cy);
        double mu11 = (sumXY / totalW) - (cx * cy);
        float rollAngle = static_cast<float>(0.5 * std::atan2(2.0 * mu11, mu20 - mu02));
        rollAngle = std::clamp(rollAngle, -0.6f, 0.6f);

        float span = std::clamp(static_cast<float>(std::sqrt(std::max(1.0, mu20)) * 1.6),
                                float(w) * 0.15f, float(w) * 0.45f);

        float cosR = std::cos(rollAngle);
        float sinR = std::sin(rollAngle);
        float dx = span * 0.5f * cosR;
        float dy = span * 0.5f * sinR;

        float perpX = -sinR;
        float perpY = cosR;

        FaceLandmarks raw{};
        raw.leftEye = {cx - dx, cy - dy};
        raw.rightEye = {cx + dx, cy + dy};
        raw.nose = {cx + perpX * span * 0.55f, cy + perpY * span * 0.55f};

        float mouthCenterY = cy + perpY * span * 0.90f;
        float mouthCenterX = cx + perpX * span * 0.90f;
        float mouthGap = span * 0.08f;
        raw.upperLip = {mouthCenterX - perpX * mouthGap * 0.5f,
                        mouthCenterY - perpY * mouthGap * 0.5f};
        raw.lowerLip = {mouthCenterX + perpX * mouthGap * 0.5f,
                        mouthCenterY + perpY * mouthGap * 0.5f};

        raw.eyeOpen = 1.0f;
        raw.gazeX = std::clamp((cx - float(w) * 0.5f) / (float(w) * 0.35f), -1.0f, 1.0f);
        raw.gazeY = std::clamp((cy - float(h) * 0.45f) / (float(h) * 0.35f), -1.0f, 1.0f);

        if (!hasSmoothed_) {
            smoothed_ = raw;
            hasSmoothed_ = true;
        } else {
            const float alpha = 0.30f;
            auto smoothPt = [&](LandmarkPoint& curr, const LandmarkPoint& target) {
                curr.x = curr.x + alpha * (target.x - curr.x);
                curr.y = curr.y + alpha * (target.y - curr.y);
            };
            smoothPt(smoothed_.leftEye, raw.leftEye);
            smoothPt(smoothed_.rightEye, raw.rightEye);
            smoothPt(smoothed_.nose, raw.nose);
            smoothPt(smoothed_.upperLip, raw.upperLip);
            smoothPt(smoothed_.lowerLip, raw.lowerLip);
            smoothed_.eyeOpen = smoothed_.eyeOpen + alpha * (raw.eyeOpen - smoothed_.eyeOpen);
            smoothed_.gazeX = smoothed_.gazeX + alpha * (raw.gazeX - smoothed_.gazeX);
            smoothed_.gazeY = smoothed_.gazeY + alpha * (raw.gazeY - smoothed_.gazeY);
        }

        return smoothed_;
    }

private:
    std::shared_ptr<GpuDevice> gpu_;
    ComPtr<ID3D11Texture2D> staging_;
    UINT stagingWidth_{};
    UINT stagingHeight_{};
    FaceLandmarks smoothed_{};
    bool hasSmoothed_{false};
};
}  // namespace

bool isCubismSupported() {
#ifdef CORTEX_HAS_CUBISM
    return true;
#else
    return false;
#endif
}

std::unique_ptr<AvatarModel> loadAvatarModel(std::shared_ptr<GpuDevice> gpu,
                                             const std::filesystem::path& path) {
#ifdef CORTEX_HAS_CUBISM
    (void)gpu;
    (void)path;
    return nullptr;
#else
    (void)gpu;
    (void)path;
    return nullptr;
#endif
}

std::unique_ptr<FaceTracker> makeFaceTracker(std::shared_ptr<GpuDevice> gpu) {
    return std::make_unique<BuiltinFaceTracker>(std::move(gpu));
}

std::optional<AvatarPose> AvatarFilter::map(const FaceLandmarks& f) {
    const float dx = f.rightEye.x - f.leftEye.x, dy = f.rightEye.y - f.leftEye.y;
    const float span = std::hypot(dx, dy);
    if (!std::isfinite(span) || span < 1)
        return {};
    const float x = f.nose.x - (f.leftEye.x + dx * .5f);
    const float y = f.nose.y - (f.leftEye.y + dy * .5f);
    const float nx = (x * dx + y * dy) / (span * span), ny = (y * dx - x * dy) / (span * span);
    const float mouth = std::hypot(f.upperLip.x - f.lowerLip.x, f.upperLip.y - f.lowerLip.y) / span;
    for (float v : {nx, ny, mouth, f.eyeOpen, f.gazeX, f.gazeY})
        if (!std::isfinite(v))
            return {};
    return AvatarPose{std::clamp(nx * 80, -45.f, 45.f),
                      std::clamp((ny - .55f) * 80, -35.f, 35.f),
                      std::atan2(dy, dx) * 180 / std::numbers::pi_v<float>,
                      std::clamp(mouth * 4, 0.f, 1.f),
                      std::clamp(f.eyeOpen, 0.f, 1.f),
                      std::clamp(f.gazeX, -1.f, 1.f),
                      std::clamp(f.gazeY, -1.f, 1.f)};
}

AvatarPose AvatarFilter::testPose(double seconds) {
    const float t = static_cast<float>(std::fmod(seconds, 120.0));
    return {18 * std::sin(t),
            9 * std::sin(2 * t),
            6 * std::sin(t * .5f),
            .5f + .5f * std::sin(3 * t),
            std::min(1.f, std::abs(std::fmod(t, 4.f) - 2) * 12),
            std::sin(t),
            .5f * std::cos(t)};
}

AvatarFilter::AvatarFilter(std::shared_ptr<GpuDevice> gpu)
    : gpu_(std::move(gpu)) {
    const auto code = detail::compile("AvatarPreview", "ps_5_0");
    detail::hr(gpu_->device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(),
                                               nullptr, &shader_),
               "Avatar preview shader");
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = 32;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    detail::hr(gpu_->device->CreateBuffer(&desc, nullptr, &constants_), "Avatar pose buffer");
}

void AvatarFilter::draw(const AvatarPose& pose, unsigned width, unsigned height) {
    D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
    const float params[] = {pose.yaw,     pose.pitch, pose.roll,  pose.mouth,
                            pose.eyeOpen, pose.gazeX, pose.gazeY, float(width) / float(height)};
    auto* context = gpu_->context.Get();
    context->RSSetViewports(1, &viewport);
    context->UpdateSubresource(constants_.Get(), 0, nullptr, params, 0, 0);
    auto* buffer = constants_.Get();
    context->PSSetConstantBuffers(0, 1, &buffer);
    context->PSSetShader(shader_.Get(), nullptr, 0);
    context->Draw(3, 0);
}
}  // namespace cortex
