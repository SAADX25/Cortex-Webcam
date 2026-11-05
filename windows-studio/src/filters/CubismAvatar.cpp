#include "cortex/filters/AvatarFilter.h"
#include <stdexcept>
#include <iostream>

#ifdef CORTEX_HAS_CUBISM
#include "GpuHelpers.h"
#include "CubismShaderSource.generated.h"
#include <CubismModelSettingJson.hpp>
#include <ICubismAllocator.hpp>
#include <Utils/CubismJson.hpp>
#include <Id/CubismIdManager.hpp>
#include <Model/CubismUserModel.hpp>
#include <Rendering/D3D11/CubismRenderer_D3D11.hpp>
#include <wincodec.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <vector>

namespace {
// Shader generation is invoked from the SDK. Its compile call is redirected by
// CMake so a cold load can stop between individual shaders instead of compiling
// the entire set before noticing that Studio is closing.
thread_local std::stop_token avatarLoadStop;
thread_local bool avatarShaderCompileStarted{};

void checkAvatarLoadStop() {
    if (avatarLoadStop.stop_requested())
        throw std::runtime_error("Live2D character loading cancelled");
}
}  // namespace

extern "C" HRESULT WINAPI CortexCompileCubismShader(
    LPCVOID source, SIZE_T size, LPCSTR sourceName, const D3D_SHADER_MACRO* defines,
    ID3DInclude* include, LPCSTR entry, LPCSTR target, UINT flags, UINT effectFlags,
    ID3DBlob** code, ID3DBlob** errors) {
    avatarShaderCompileStarted = true;
    // Never throw through the SDK's C compiler callback. Let its renderer finish
    // initialization so every destructor field is valid, then the surrounding
    // C++ load operation reports cancellation before the model can be published.
    if (avatarLoadStop.stop_requested()) {
        if (code)
            *code = nullptr;
        if (errors)
            *errors = nullptr;
        return E_ABORT;
    }
    return D3DCompile(source, size, sourceName, defines, include, entry, target, flags,
                      effectFlags, code, errors);
}

namespace cortex {
namespace {
namespace cubism = Live2D::Cubism::Framework;
using Renderer = cubism::Rendering::CubismRenderer_D3D11;

// The framework has global allocator, IDs, and device caches shared by all instances.
// Immediate-context work acquires the GPU lock before this lock. Loading uses
// only thread-safe ID3D11Device resource creation and must never acquire the GPU
// lock while holding this lock, otherwise rendering and loading can deadlock.
std::recursive_mutex cubismMutex;

class Allocator final : public cubism::ICubismAllocator {
public:
    void* Allocate(cubism::csmSizeType size) override { return std::malloc(size); }
    void Deallocate(void* memory) override { std::free(memory); }
    void* AllocateAligned(cubism::csmSizeType size, cubism::csmUint32 alignment) override {
        return _aligned_malloc(size, alignment);
    }
    void DeallocateAligned(void* memory) override { _aligned_free(memory); }
};

cubism::csmByte* loadShader(const std::string path, cubism::csmSizeInt* size) {
    *size = 0;
    const char* source = nullptr;
    size_t length{};
    if (path == "FrameworkShaders/CubismEffect.fx") {
        source = detail::cubismEffect;
        length = sizeof(detail::cubismEffect) - 1;
    } else if (path == "FrameworkShaders/CubismBlendMode.fx") {
        source = detail::cubismBlend;
        length = sizeof(detail::cubismBlend) - 1;
    }
    if (!source)
        return nullptr;
    auto* bytes = new (std::nothrow) cubism::csmByte[length];
    if (bytes) {
        std::memcpy(bytes, source, length);
        *size = static_cast<cubism::csmSizeInt>(length);
    }
    return bytes;
}

struct Runtime {
    Allocator allocator;
    cubism::CubismFramework::Option options{};
    Runtime() {
        options.LoggingLevel = cubism::CubismFramework::Option::LogLevel_Warning;
        options.LogFunction = [](const char* message) { OutputDebugStringA(message); };
        options.LoadFileFunction = loadShader;
        options.ReleaseBytesFunction = [](cubism::csmByte* data) { delete[] data; };
        if (!cubism::CubismFramework::StartUp(&allocator, &options))
            throw std::runtime_error("Cubism initialization failed");
        cubism::CubismFramework::Initialize();
    }
    ~Runtime() {
        // The D3D11 SDK keeps per-device resources outside StaticRelease().
        // Destroy these while the Cubism allocator is still alive.
        cubism::Rendering::CubismDeviceInfo_D3D11::ReleaseAllDeviceInfo();
        cubism::CubismFramework::Dispose();
        cubism::CubismFramework::CleanUp();
    }
};

std::shared_ptr<Runtime> acquireRuntime() {
    static std::weak_ptr<Runtime> active;
    auto runtime = active.lock();
    if (!runtime) {
        runtime = std::make_shared<Runtime>();
        active = runtime;
    }
    return runtime;
}

std::vector<cubism::csmByte> readAsset(const std::filesystem::path& path) {
    checkAvatarLoadStop();
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    const auto length = stream.tellg();
    if (!stream || length <= 0 || length > 128 * 1024 * 1024)
        throw std::runtime_error("Missing, empty or oversized Live2D asset: " +
                                 path.filename().string());
    std::vector<cubism::csmByte> data(static_cast<size_t>(length));
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char*>(data.data()), length))
        throw std::runtime_error("Could not read Live2D asset");
    checkAvatarLoadStop();
    return data;
}

std::filesystem::path assetPath(const std::filesystem::path& directory, const char* name) {
    const auto relative =
        std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(name)));
    const auto full = std::filesystem::weakly_canonical(directory / relative);
    const auto within = full.lexically_relative(directory);
    if (relative.empty() || relative.is_absolute() || within.empty() || *within.begin() == "..")
        throw std::runtime_error("Live2D asset must be inside the model folder");
    return full;
}

std::filesystem::path modelSettingsPath(std::filesystem::path path) {
    if (path.extension() == L".moc3") {
        path.replace_extension(L".model3.json");
        if (!std::filesystem::is_regular_file(path))
            throw std::runtime_error("Select the .model3.json file supplied with the model");
    }
    if (!path.filename().wstring().ends_with(L".model3.json"))
        throw std::runtime_error("Choose a Live2D .model3.json file");
    return std::filesystem::weakly_canonical(path);
}

ComPtr<ID3D11ShaderResourceView> loadTexture(GpuDevice& gpu, const std::filesystem::path& path) {
    checkAvatarLoadStop();
    ComPtr<IWICImagingFactory> factory;
    detail::hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)),
               "Live2D image factory");
    ComPtr<IWICBitmapDecoder> decoder;
    detail::hr(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnLoad, &decoder),
               "Read Live2D texture");
    ComPtr<IWICBitmapFrameDecode> frame;
    detail::hr(decoder->GetFrame(0, &frame), "Live2D texture frame");
    UINT width{}, height{};
    detail::hr(frame->GetSize(&width, &height), "Live2D texture size");
    if (!width || !height || width > 8192 || height > 8192)
        throw std::runtime_error("Live2D texture must be at most 8192 x 8192");
    ComPtr<IWICFormatConverter> converter;
    detail::hr(factory->CreateFormatConverter(&converter), "Live2D texture converter");
    detail::hr(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                     WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom),
               "Live2D RGBA texture");
    std::vector<uint8_t> pixels(size_t(width) * height * 4);
    detail::hr(
        converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()),
        "Decode Live2D texture");
    checkAvatarLoadStop();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{pixels.data(), width * 4, 0};
    // Device resource creation is thread-safe and does not use the immediate context.
    ComPtr<ID3D11Texture2D> texture;
    detail::hr(gpu.device->CreateTexture2D(&desc, &initial, &texture), "Create Live2D texture");
    ComPtr<ID3D11ShaderResourceView> view;
    detail::hr(gpu.device->CreateShaderResourceView(texture.Get(), nullptr, &view),
               "Create Live2D texture view");
    return view;
}

struct MotionDeleter {
    void operator()(cubism::ACubismMotion* motion) const { cubism::ACubismMotion::Delete(motion); }
};
using Motion = std::unique_ptr<cubism::ACubismMotion, MotionDeleter>;

struct MappedParam {
    int index{-1};
    float minVal{-1.0f};
    float maxVal{1.0f};
    float defVal{0.0f};
    bool valid{false};

    void init(cubism::CubismModel* model, const std::vector<const char*>& candidateIds) {
        if (!model)
            return;
        auto* idManager = cubism::CubismFramework::GetIdManager();
        const int count = model->GetParameterCount();
        for (const char* name : candidateIds) {
            auto handle = idManager->GetId(name);
            for (int i = 0; i < count; ++i) {
                if (model->GetParameterId(i) == handle) {
                    index = i;
                    minVal = model->GetParameterMinimumValue(i);
                    maxVal = model->GetParameterMaximumValue(i);
                    defVal = model->GetParameterDefaultValue(i);
                    valid = true;
                    return;
                }
            }
        }
    }

    void setBipolar(cubism::CubismModel* model, float normalized) const {
        if (!valid || !model)
            return;
        const float val = std::clamp(normalized, -1.0f, 1.0f);
        const float actual = val >= 0.0f ? (defVal + val * (maxVal - defVal))
                                         : (defVal + (-val) * (minVal - defVal));
        model->SetParameterValue(index, actual);
    }

    void setUnipolar(cubism::CubismModel* model, float normalized) const {
        if (!valid || !model)
            return;
        const float val = std::clamp(normalized, 0.0f, 1.0f);
        const float actual = minVal + val * (maxVal - minVal);
        model->SetParameterValue(index, actual);
    }

    void setEye(cubism::CubismModel* model, float openAmount) const {
        if (!valid || !model)
            return;
        float actual = defVal;
        if (openAmount <= 1.0f) {
            const float clamped = std::clamp(openAmount, 0.0f, 1.0f);
            actual = minVal + clamped * (defVal - minVal);
        } else {
            const float clamped = std::clamp(openAmount, 1.0f, 2.0f);
            actual = defVal + (clamped - 1.0f) * (maxVal - defVal);
        }
        model->SetParameterValue(index, actual);
    }

    void setAngle(cubism::CubismModel* model, float degrees, float maxExpectedDeg = 30.0f) const {
        if (!valid || !model)
            return;
        const float norm = degrees / (maxExpectedDeg > 0.0f ? maxExpectedDeg : 30.0f);
        setBipolar(model, norm);
    }
};

class Model final : public cubism::CubismUserModel {
public:
    std::vector<Motion> motions, expressions;
    std::vector<size_t> idle;
    int selectedMotion{-2}, selectedExpression{-2};
    size_t nextIdle{};
    bool wasCameraDriven{};
    bool physicsInitialized{};

    MappedParam angleX, angleY, angleZ;
    MappedParam eyeLOpen, eyeROpen;
    MappedParam eyeBallX, eyeBallY;
    MappedParam browLY, browRY;
    MappedParam mouthOpenY, mouthForm;
    MappedParam bodyAngleX, bodyAngleY, bodyAngleZ;
    MappedParam armLA, armLB, armRA, armRB;
    MappedParam armL, armR;
    MappedParam breath, eyeLSmile, eyeRSmile, browLAngle, browRAngle;
    MappedParam shoulderL, shoulderR, elbowL, elbowR, wristL, wristR;
    MappedParam handL, handR;
    MappedParam leftShoulderUp, rightShoulderUp, handLDrawOrder, handRDrawOrder;
    MappedParam bodyAngleX2, bodyAngleY2, bodyCenter2, leftShoulderUp2, rightShoulderUp2;
    std::array<float, 6> physInput_{};
    bool physInputValid_{};

    ~Model() override {
        _motionManager->StopAllMotions();
        _expressionManager->StopAllMotions();
    }

    void initParameters() {
        if (!_model)
            return;
        angleX.init(_model, {"ParamAngleX", "PARAM_ANGLE_X", "paramAngleX"});
        angleY.init(_model, {"ParamAngleY", "PARAM_ANGLE_Y", "paramAngleY"});
        angleZ.init(_model, {"ParamAngleZ", "PARAM_ANGLE_Z", "paramAngleZ"});

        eyeLOpen.init(_model, {"ParamEyeLOpen", "PARAM_EYE_L_OPEN", "paramEyeLOpen"});
        eyeROpen.init(_model, {"ParamEyeROpen", "PARAM_EYE_R_OPEN", "paramEyeROpen"});

        eyeBallX.init(_model, {"ParamEyeBallX", "PARAM_EYE_BALL_X", "paramEyeBallX"});
        eyeBallY.init(_model, {"ParamEyeBallY", "PARAM_EYE_BALL_Y", "paramEyeBallY"});

        browLY.init(_model, {"ParamBrowLY", "PARAM_BROW_L_Y", "paramBrowLY"});
        browRY.init(_model, {"ParamBrowRY", "PARAM_BROW_R_Y", "paramBrowRY"});

        mouthOpenY.init(_model, {"ParamMouthOpenY", "PARAM_MOUTH_OPEN_Y", "ParamMouthOpen",
                                "paramMouthOpenY", "PARAM_MOUTH_OPEN"});
        mouthForm.init(_model, {"ParamMouthForm", "PARAM_MOUTH_FORM", "paramMouthForm"});

        bodyAngleX.init(_model, {"ParamBodyAngleX", "PARAM_BODY_ANGLE_X", "paramBodyAngleX"});
        bodyAngleY.init(_model, {"ParamBodyAngleY", "PARAM_BODY_ANGLE_Y", "paramBodyAngleY"});
        bodyAngleZ.init(_model, {"ParamBodyAngleZ", "PARAM_BODY_ANGLE_Z", "paramBodyAngleZ"});

        armLA.init(_model, {"ParamArmLA", "paramArmLA"});
        armLB.init(_model, {"ParamArmLB", "paramArmLB"});
        armRA.init(_model, {"ParamArmRA", "paramArmRA"});
        armRB.init(_model, {"ParamArmRB", "paramArmRB"});

        armL.init(_model, {"ParamArmL", "PARAM_ARM_L", "paramArmL"});
        armR.init(_model, {"ParamArmR", "PARAM_ARM_R", "paramArmR"});
        breath.init(_model, {"ParamBreath", "PARAM_BREATH"});
        eyeLSmile.init(_model, {"ParamEyeLSmile"});
        eyeRSmile.init(_model, {"ParamEyeRSmile"});
        browLAngle.init(_model, {"ParamBrowLAngle"});
        browRAngle.init(_model, {"ParamBrowRAngle"});
        shoulderL.init(_model, {"ParamArmL01"});
        shoulderR.init(_model, {"ParamArmR01"});
        elbowL.init(_model, {"ParamArmL02"});
        elbowR.init(_model, {"ParamArmR02"});
        wristL.init(_model, {"ParamArmL03"});
        wristR.init(_model, {"ParamArmR03"});
        handL.init(_model, {"ParamHandL"});
        handR.init(_model, {"ParamHandR"});
        leftShoulderUp.init(_model, {"ParamLeftShoulderUp"});
        rightShoulderUp.init(_model, {"ParamRightShoulderUp"});
        handLDrawOrder.init(_model, {"ParamHandLDrawOrder"});
        handRDrawOrder.init(_model, {"ParamHandRDrawOrder"});
        bodyAngleX2.init(_model, {"ParamBodyAngleX2"});
        bodyAngleY2.init(_model, {"ParamBodyAngleY2"});
        bodyCenter2.init(_model, {"ParamBodyCenter2"});
        leftShoulderUp2.init(_model, {"ParamLeftShoulderUp2"});
        rightShoulderUp2.init(_model, {"ParamRightShoulderUp2"});
    }

    void update(float elapsed, const AvatarPose& pose, int motion, int expression) {
        // 1. Load saved parameters
        _model->LoadParameters();

        const bool modeChanged = pose.cameraDriven != wasCameraDriven;
        if (modeChanged) {
            _motionManager->StopAllMotions();
            for (int i = 0; i < _model->GetParameterCount(); ++i)
                _model->SetParameterValue(i, _model->GetParameterDefaultValue(i));
            _model->SaveParameters();
            selectedMotion = -2;
            wasCameraDriven = pose.cameraDriven;
        }

        // Camera tracking owns the pose. Prerecorded idle motions also animate
        // hair and body channels, so even a reduced weight introduces movement
        // unrelated to the person in front of the camera.
        if (!pose.cameraDriven) {
            if (motion != selectedMotion) {
                _motionManager->StopAllMotions();
                selectedMotion = motion;
                if (motion >= 0 && size_t(motion) < motions.size()) {
                    motions[motion]->SetWeight(1.0f);
                    _motionManager->StartMotionPriority(motions[motion].get(), false, 3);
                }
            }
            if (_motionManager->IsFinished() && !idle.empty()) {
                auto* idleMotion = motions[idle[nextIdle++ % idle.size()]].get();
                idleMotion->SetWeight(1.0f);
                _motionManager->StartMotionPriority(idleMotion, false, 1);
            }
            _motionManager->UpdateMotion(_model, elapsed);
        }

        if (expression != selectedExpression) {
            _expressionManager->StopAllMotions();
            selectedExpression = expression;
            if (expression >= 0 && size_t(expression) < expressions.size())
                _expressionManager->StartMotion(expressions[expression].get(), false);
        }
        _expressionManager->UpdateMotion(_model, elapsed);

        // 3. Apply face, head, and body tracking parameters
        angleX.setAngle(_model, pose.yaw, 30.0f);
        angleY.setAngle(_model, pose.pitch, 30.0f);
        angleZ.setAngle(_model, pose.roll, 30.0f);

        const float lEye = pose.leftEyeOpen < 0 ? pose.eyeOpen : pose.leftEyeOpen;
        const float rEye = pose.rightEyeOpen < 0 ? pose.eyeOpen : pose.rightEyeOpen;
        eyeLOpen.setEye(_model, lEye);
        eyeROpen.setEye(_model, rEye);

        eyeBallX.setBipolar(_model, pose.gazeX);
        eyeBallY.setBipolar(_model, pose.gazeY);

        browLY.setBipolar(_model, pose.browLY);
        browRY.setBipolar(_model, pose.browRY);

        mouthOpenY.setUnipolar(_model, pose.mouth);
        mouthForm.setBipolar(_model, pose.mouthForm);

        bodyAngleX.setAngle(_model, pose.bodyAngleX, 10.0f);
        bodyAngleY.setAngle(_model, pose.bodyAngleY, 10.0f);
        bodyAngleZ.setAngle(_model, pose.bodyAngleZ, 10.0f);

        if (pose.cameraDriven) {
            if (pose.armsEnabled) {
                if (armLA.valid && armLB.valid) {
                    const auto armHaru = [&](const ArmPose& a, const MappedParam& upper,
                                             const MappedParam& lower, const char* partA,
                                             const char* partB) {
                        const float lift = std::clamp(a.lift, 0.f, 1.f);
                        const float bend = std::clamp(a.bend, 0.f, 1.f);
                        _model->SetParameterValue(upper.index, -1.0f + 2.0f * lift);
                        _model->SetParameterValue(lower.index, 5.0f * bend);
                        const bool raised = lift > .25f || bend > .25f;
                        const auto show = [&](const char* id, float opacity) {
                            const auto handle = cubism::CubismFramework::GetIdManager()->GetId(id);
                            const int index = _model->GetPartIndex(handle);
                            if (index >= 0 && index < _model->GetPartCount())
                                _model->SetPartOpacity(index, opacity);
                            const int pIndex = _model->GetParameterIndex(handle);
                            if (pIndex >= 0 && pIndex < _model->GetParameterCount())
                                _model->SetParameterValue(pIndex, opacity);
                        };
                        show(partA, raised ? 0.f : 1.f);
                        show(partB, raised ? 1.f : 0.f);
                    };
                    armHaru(pose.arms.left, armLA, armLB, "Part01ArmLA001", "Part01ArmLB001");
                    armHaru(pose.arms.right, armRA, armRB, "Part01ArmRA001", "Part01ArmRB001");
                } else {
                    const auto armRig = [&](const ArmPose& a, const MappedParam& arm,
                                             const MappedParam& shoulder, const MappedParam& elbow,
                                             const MappedParam& wrist, const MappedParam& hand,
                                             const MappedParam& shoulderUp,
                                             const MappedParam& handDrawOrder) {
                        const bool measured =
                            a.confidence > 0.f || a.lift > .005f || a.bend > .005f;
                        const float lift = measured ? std::clamp(a.lift, 0.f, 1.f) : 0.f;
                        const float bend = measured ? std::clamp(a.bend, 0.f, 1.f) : 0.f;
                        if (shoulder.valid && elbow.valid) {
                            // Ren authors separate shoulder and elbow rotations.
                            // Its neutral is 0, shoulder lifts toward +40, and a
                            // flexed elbow bends toward -30. Keep each measured
                            // joint independent instead of feeding max(lift,bend)
                            // into the shoulder alone.
                            shoulder.setBipolar(_model, lift);
                            elbow.setBipolar(_model, -bend);
                        } else {
                            arm.setBipolar(_model, lift);
                        }
                        if (shoulderUp.valid)
                            shoulderUp.setBipolar(_model, lift * .5f);
                        if (handDrawOrder.valid)
                            handDrawOrder.setUnipolar(
                                _model, (lift > .22f || bend > .30f) ? 1.f : 0.f);
                        // Wrist bend and hand open/close come from the pose
                        // model's pinky/index/thumb landmarks. Fall back to the
                        // authored rest pose when the hand is not visible.
                        const bool handMeasured = measured && a.handConfidence >= .35f;
                        if (wrist.valid) {
                            if (handMeasured)
                                wrist.setBipolar(_model, a.wrist);
                            else
                                _model->SetParameterValue(wrist.index, wrist.defVal);
                        }
                        if (hand.valid) {
                            if (handMeasured)
                                hand.setBipolar(_model, a.hand * 2.f - 1.f);
                            else
                                _model->SetParameterValue(hand.index, hand.defVal);
                        }
                    };
                    armRig(pose.arms.left, armL, shoulderL, elbowL, wristL, handL,
                           leftShoulderUp, handLDrawOrder);
                    armRig(pose.arms.right, armR, shoulderR, elbowR, wristR, handR,
                           rightShoulderUp, handRDrawOrder);
                }
            } else {
                // Arms disabled: keep natural resting arm pose
                const auto showPart = [&](const char* id, float opacity) {
                    const auto handle = cubism::CubismFramework::GetIdManager()->GetId(id);
                    const int index = _model->GetPartIndex(handle);
                    if (index >= 0 && index < _model->GetPartCount())
                        _model->SetPartOpacity(index, opacity);
                    const int pIndex = _model->GetParameterIndex(handle);
                    if (pIndex >= 0 && pIndex < _model->GetParameterCount())
                        _model->SetParameterValue(pIndex, opacity);
                };
                if (armLA.valid && armLB.valid) {
                    showPart("Part01ArmLA001", 1.0f);
                    showPart("Part01ArmLB001", 0.0f);
                    showPart("Part01ArmRA001", 1.0f);
                    showPart("Part01ArmRB001", 0.0f);
                    _model->SetParameterValue(armLA.index, -1.0f);
                    _model->SetParameterValue(armLB.index, 0.0f);
                    _model->SetParameterValue(armRA.index, -1.0f);
                    _model->SetParameterValue(armRB.index, 0.0f);
                } else {
                    for (const auto* parameter :
                         {&armL, &armR, &shoulderL, &shoulderR, &elbowL, &elbowR, &wristL, &wristR,
                          &handL, &handR, &leftShoulderUp, &rightShoulderUp,
                          &handLDrawOrder, &handRDrawOrder})
                        if (parameter->valid)
                            _model->SetParameterValue(parameter->index, parameter->defVal);
                }
            }
        }

        // Expression coupling: couple wide smiles to eye smile and brow height to brow angle
        // without injecting artificial sine-wave oscillation when the user is still.
        if (pose.cameraDriven) {
            if (breath.valid)
                _model->SetParameterValue(breath.index, breath.defVal);
            const float smile = std::clamp((pose.mouthForm - .35f) / .65f, 0.f, 1.f);
            const float lOpen = pose.leftEyeOpen < 0 ? pose.eyeOpen : pose.leftEyeOpen;
            const float rOpen = pose.rightEyeOpen < 0 ? pose.eyeOpen : pose.rightEyeOpen;
            eyeLSmile.setUnipolar(_model, smile * .8f * std::clamp(lOpen, 0.f, 1.f));
            eyeRSmile.setUnipolar(_model, smile * .8f * std::clamp(rOpen, 0.f, 1.f));
            browLAngle.setBipolar(_model, pose.browLY * .6f);
            browRAngle.setBipolar(_model, pose.browRY * .6f);
            for (const auto* sec : {&bodyAngleX2, &bodyAngleY2, &bodyCenter2,
                                    &leftShoulderUp2, &rightShoulderUp2})
                if (sec->valid)
                    _model->SetParameterValue(sec->index, sec->defVal);
        }

        // 4. Save parameters
        _model->SaveParameters();

        // Keep measured channels authoritative if a custom physics rig also
        // writes a tracked parameter. Hair and clothing keep their own outputs.
        const std::array<const MappedParam*, 32> tracked{
            &angleX, &angleY, &angleZ, &eyeLOpen, &eyeROpen, &eyeBallX, &eyeBallY,
            &browLY, &browRY, &mouthOpenY, &mouthForm, &bodyAngleX, &bodyAngleY,
            &bodyAngleZ, &armLA, &armLB, &armRA, &armRB, &armL, &armR,
            &shoulderL, &shoulderR, &elbowL, &elbowR, &wristL, &wristR, &handL, &handR,
            &leftShoulderUp, &rightShoulderUp, &handLDrawOrder, &handRDrawOrder};
        std::array<float, tracked.size()> measured{};
        if (pose.cameraDriven)
            for (size_t i = 0; i < tracked.size(); ++i)
                if (tracked[i]->valid)
                    measured[i] = _model->GetParameterValue(tracked[i]->index);

        // 5. Apply pose and the model's authored hair/clothing physics. Start
        // from equilibrium after loading or a camera/demo switch so stale demo
        // motion does not leave the tracked character swaying.
        if (_pose)
            _pose->UpdateParameters(_model, elapsed);
        if (_physics) {
            if (!physicsInitialized || modeChanged) {
                _physics->Stabilization(_model);
                physicsInitialized = true;
                physInputValid_ = false;
            }
            if (pose.cameraDriven) {
                const std::array<const MappedParam*, 6> physDrivers{
                    &angleX, &angleY, &angleZ, &bodyAngleX, &bodyAngleY, &bodyAngleZ};
                const float dtClamped = std::clamp(elapsed, 0.f, .1f);
                const float alpha = dtClamped > 0.f ? (1.f - std::exp(-dtClamped / .035f)) : 1.f;
                for (size_t i = 0; i < physDrivers.size(); ++i) {
                    if (!physDrivers[i]->valid)
                        continue;
                    const float targetVal = _model->GetParameterValue(physDrivers[i]->index);
                    if (!physInputValid_)
                        physInput_[i] = targetVal;
                    else
                        physInput_[i] += alpha * (targetVal - physInput_[i]);
                    _model->SetParameterValue(physDrivers[i]->index, physInput_[i]);
                }
                physInputValid_ = true;
            }
            _physics->Evaluate(_model, elapsed);
        }
        if (pose.cameraDriven) {
            for (size_t i = 0; i < tracked.size(); ++i)
                if (tracked[i]->valid)
                    _model->SetParameterValue(tracked[i]->index, measured[i]);
            // Dampen secondary whole-body physics pendulums in camera-driven mode
            // so head/gaze micro-movements do not make the entire torso/shoulders wobble.
            for (const auto* sec : {&bodyAngleX2, &bodyAngleY2, &bodyCenter2,
                                    &leftShoulderUp2, &rightShoulderUp2}) {
                if (sec->valid) {
                    const float v = _model->GetParameterValue(sec->index);
                    _model->SetParameterValue(sec->index, sec->defVal + (v - sec->defVal) * .20f);
                }
            }
        }

        // 6. Update model state
        _model->Update();
    }
};

class LoadedAvatar final : public AvatarModel {
public:
    LoadedAvatar(std::shared_ptr<GpuDevice> gpu, const std::filesystem::path& path)
        : gpu_(std::move(gpu))
        , runtime_(acquireRuntime())
        , asset_(std::make_unique<Model>()) {
        const auto settingsPath = modelSettingsPath(path);
        const auto directory = settingsPath.parent_path();
        const auto bytes = readAsset(settingsPath);
        std::unique_ptr<cubism::Utils::CubismJson, decltype(&cubism::Utils::CubismJson::Delete)>
            json(cubism::Utils::CubismJson::Create(bytes.data(), static_cast<int>(bytes.size())),
                 cubism::Utils::CubismJson::Delete);
        if (!json || !json->GetRoot().IsMap() || !json->GetRoot()["FileReferences"].IsMap() ||
            !json->GetRoot()["FileReferences"]["Moc"].IsString())
            throw std::runtime_error("Invalid Live2D model settings JSON");
        cubism::CubismModelSettingJson settings(bytes.data(), static_cast<int>(bytes.size()));
        const auto moc = readAsset(assetPath(directory, settings.GetModelFileName()));
        asset_->LoadModel(moc.data(), static_cast<int>(moc.size()), true);
        checkAvatarLoadStop();
        if (!asset_->GetModel())
            throw std::runtime_error("Invalid or unsupported Live2D moc3 model");
        asset_->initParameters();
        if (settings.GetTextureCount() < 1 || settings.GetTextureCount() > 32)
            throw std::runtime_error("Live2D model must contain 1 to 32 textures");
        if (std::strlen(settings.GetPhysicsFileName())) {
            const auto data = readAsset(assetPath(directory, settings.GetPhysicsFileName()));
            asset_->LoadPhysics(data.data(), static_cast<int>(data.size()));
        }
        if (std::strlen(settings.GetPoseFileName())) {
            const auto data = readAsset(assetPath(directory, settings.GetPoseFileName()));
            asset_->LoadPose(data.data(), static_cast<int>(data.size()));
        }
        for (int groupIndex = 0; groupIndex < settings.GetMotionGroupCount(); ++groupIndex) {
            const auto* group = settings.GetMotionGroupName(groupIndex);
            for (int i = 0; i < settings.GetMotionCount(group); ++i) {
                if (asset_->motions.size() >= 1024)
                    throw std::runtime_error("Too many Live2D motions");
                const auto data =
                    readAsset(assetPath(directory, settings.GetMotionFileName(group, i)));
                Motion motion(asset_->LoadMotion(data.data(), static_cast<int>(data.size()), group,
                                                 nullptr, nullptr, &settings, group, i));
                if (!motion)
                    throw std::runtime_error("Invalid Live2D motion");
                checkAvatarLoadStop();
                if (_stricmp(group, "idle") == 0)
                    asset_->idle.push_back(asset_->motions.size());
                asset_->motions.push_back(std::move(motion));
            }
        }
        if (asset_->idle.empty() && !asset_->motions.empty())
            asset_->idle.push_back(0);
        for (int i = 0; i < settings.GetExpressionCount(); ++i) {
            if (i >= 256)
                throw std::runtime_error("Too many Live2D expressions");
            const auto data = readAsset(assetPath(directory, settings.GetExpressionFileName(i)));
            Motion expression(asset_->LoadExpression(data.data(), static_cast<int>(data.size()),
                                                     settings.GetExpressionName(i)));
            if (!expression)
                throw std::runtime_error("Invalid Live2D expression");
            checkAvatarLoadStop();
            asset_->expressions.push_back(std::move(expression));
        }
        cubism::csmMap<cubism::csmString, float> layout;
        settings.GetLayoutMap(layout);
        asset_->GetModelMatrix()->SetupFromLayout(layout);
        checkAvatarLoadStop();
        try {
            Renderer::SetConstantSettings(1, gpu_->device.Get());
            asset_->CreateRenderer(1920, 1080);
            checkAvatarLoadStop();
        } catch (...) {
            // An interrupted first compile leaves an incomplete per-device
            // shader cache. Remove it before retrying while the allocator lives.
            // Compilation only occurs when this device has no existing cache.
            if (avatarShaderCompileStarted && avatarLoadStop.stop_requested())
                cubism::Rendering::CubismDeviceInfo_D3D11::ReleaseDeviceInfo(gpu_->device.Get());
            throw;
        }
        auto* renderer = asset_->GetRenderer<Renderer>();
        if (!renderer)
            throw std::runtime_error("Live2D D3D11 renderer initialization failed");
        for (int i = 0; i < settings.GetTextureCount(); ++i) {
            textures_.push_back(
                loadTexture(*gpu_, assetPath(directory, settings.GetTextureFileName(i))));
            renderer->BindTexture(i, textures_.back().Get());
        }
        checkAvatarLoadStop();
        renderer->IsPremultipliedAlpha(false);
    }

    ~LoadedAvatar() override {
        std::lock_guard gpuLock(gpu_->mutex);
        std::lock_guard runtimeLock(cubismMutex);
        asset_.reset();
        textures_.clear();
        runtime_.reset();
    }

    void setAnimation(int motion, int expression) override {
        motion_ = motion;
        expression_ = expression;
    }

    void setFraming(const AvatarFraming& framing) override { framing_ = framing; }

    void draw(ID3D11DeviceContext* context, const AvatarPose& pose, unsigned width,
              unsigned height) override {
        if (!width || !height)
            return;
        std::lock_guard gpuLock(gpu_->mutex);
        std::lock_guard runtimeLock(cubismMutex);
        const auto now = std::chrono::steady_clock::now();
        const float elapsed =
            std::clamp(std::chrono::duration<float>(now - previous_).count(), 1.f / 240.f, .1f);
        previous_ = now;
        asset_->update(elapsed, pose, motion_, expression_);
        asset_->SetRenderTargetSize(width, height);
        auto* renderer = asset_->GetRenderer<Renderer>();
        D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
        context->RSSetViewports(1, &viewport);
        cubism::CubismMatrix44 projection;
        const float aspect = float(width) / float(height);
        const float top = std::min(1.f, aspect);
        const float scale = framing_.scale * (pose.cameraDriven ? pose.depthScale : 1.f);
        projection.Scale(std::min(1.f, 1.f / aspect) * scale, top * scale);
        // Zoom toward the top of the fitted model to keep the head in view.
        const float positionX = framing_.x + (pose.cameraDriven ? pose.positionX : 0.f);
        const float positionY = framing_.y + (pose.cameraDriven ? pose.positionY : 0.f);
        projection.Translate(2 * positionX, (1 - scale) * top - 2 * positionY);
        projection.MultiplyByMatrix(asset_->GetModelMatrix());
        renderer->StartFrame(context);
        renderer->SetMvpMatrix(&projection);
        renderer->DrawModel();
        renderer->EndFrame();
    }

private:
    std::shared_ptr<GpuDevice> gpu_;
    std::shared_ptr<Runtime> runtime_;
    std::unique_ptr<Model> asset_;
    std::vector<ComPtr<ID3D11ShaderResourceView>> textures_;
    std::chrono::steady_clock::time_point previous_ = std::chrono::steady_clock::now();
    int motion_{-1}, expression_{-1};
    AvatarFraming framing_;
};
}  // namespace

bool isCubismSupported() {
    return true;
}

std::unique_ptr<AvatarModel> loadAvatarModel(std::shared_ptr<GpuDevice> gpu,
                                             const std::filesystem::path& path,
                                             std::stop_token stop) {
    struct LoadScope {
        std::stop_token previousStop{avatarLoadStop};
        bool previousCompile{avatarShaderCompileStarted};
        explicit LoadScope(std::stop_token token) {
            avatarLoadStop = token;
            avatarShaderCompileStarted = false;
        }
        ~LoadScope() {
            avatarLoadStop = previousStop;
            avatarShaderCompileStarted = previousCompile;
        }
    } scope(stop);
    checkAvatarLoadStop();
    if (!gpu || !gpu->device || !gpu->context)
        throw std::invalid_argument("Live2D requires a D3D11 device");
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE)
        throw std::runtime_error("Could not initialize image loading");
    struct Apartment {
        bool initialized;
        ~Apartment() {
            if (initialized)
                CoUninitialize();
        }
    } apartment{SUCCEEDED(comResult)};
    std::lock_guard runtimeLock(cubismMutex);
    checkAvatarLoadStop();
    return std::make_unique<LoadedAvatar>(std::move(gpu), path);
}
}  // namespace cortex
#else
namespace cortex {
bool isCubismSupported() {
    return false;
}

std::unique_ptr<AvatarModel> loadAvatarModel(std::shared_ptr<GpuDevice>,
                                             const std::filesystem::path&,
                                             std::stop_token) {
    throw std::runtime_error(
        "Live2D support is unavailable in this build. Install Cubism Native SDK and rebuild.");
}
}  // namespace cortex
#endif
