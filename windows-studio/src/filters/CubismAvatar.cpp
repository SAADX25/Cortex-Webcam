#include "cortex/filters/AvatarFilter.h"
#include <stdexcept>

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
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <vector>

namespace cortex {
namespace {
namespace cubism = Live2D::Cubism::Framework;
using Renderer = cubism::Rendering::CubismRenderer_D3D11;

// The framework has global allocator, IDs, and device caches shared by all instances.
// Always acquire the GPU lock before this lock.
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
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    const auto length = stream.tellg();
    if (!stream || length <= 0 || length > 128 * 1024 * 1024)
        throw std::runtime_error("Missing, empty or oversized Live2D asset: " +
                                 path.filename().string());
    std::vector<cubism::csmByte> data(static_cast<size_t>(length));
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char*>(data.data()), length))
        throw std::runtime_error("Could not read Live2D asset");
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
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{pixels.data(), width * 4, 0};
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

class Model final : public cubism::CubismUserModel {
public:
    std::vector<Motion> motions, expressions;
    std::vector<size_t> idle;
    int selectedMotion{-2}, selectedExpression{-2};
    size_t nextIdle{};
    bool wasCameraDriven{};

    ~Model() override {
        _motionManager->StopAllMotions();
        _expressionManager->StopAllMotions();
    }

    void update(float elapsed, const AvatarPose& pose, int motion, int expression) {
        // 1. Load saved parameters
        _model->LoadParameters();

        if (pose.cameraDriven != wasCameraDriven) {
            _motionManager->StopAllMotions();
            for (int i = 0; i < _model->GetParameterCount(); ++i)
                _model->SetParameterValue(i, _model->GetParameterDefaultValue(i));
            _model->SaveParameters();
            selectedMotion = -2;
            wasCameraDriven = pose.cameraDriven;
        }

        // 2. Update background idle motion at reduced weight so it never overwrites active tracking
        if (pose.cameraDriven) {
            if (!idle.empty()) {
                if (_motionManager->IsFinished()) {
                    auto* idleMotion = motions[idle[0]].get();
                    idleMotion->SetWeight(0.20f);
                    idleMotion->SetLoop(true);
                    _motionManager->StartMotionPriority(idleMotion, false, 1);
                }
            }
        } else {
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
        }
        _motionManager->UpdateMotion(_model, elapsed);

        if (expression != selectedExpression) {
            _expressionManager->StopAllMotions();
            selectedExpression = expression;
            if (expression >= 0 && size_t(expression) < expressions.size())
                _expressionManager->StartMotion(expressions[expression].get(), false);
        }
        _expressionManager->UpdateMotion(_model, elapsed);

        // 3. Apply face, head, and body tracking parameters
        const auto set = [&](const char* id, float value) {
            _model->SetParameterValue(cubism::CubismFramework::GetIdManager()->GetId(id), value);
        };
        set("ParamAngleX", pose.yaw);
        set("ParamAngleY", pose.pitch);
        set("ParamAngleZ", pose.roll);
        set("ParamEyeLOpen", pose.leftEyeOpen < 0 ? pose.eyeOpen : pose.leftEyeOpen);
        set("ParamEyeROpen", pose.rightEyeOpen < 0 ? pose.eyeOpen : pose.rightEyeOpen);
        set("ParamEyeBallX", pose.gazeX);
        set("ParamEyeBallY", pose.gazeY);
        set("ParamBrowLY", pose.browLY);
        set("ParamBrowRY", pose.browRY);
        set("ParamMouthOpenY", pose.mouth);
        set("ParamMouthForm", pose.mouthForm);
        set("ParamBodyAngleX", pose.bodyAngleX);
        set("ParamBodyAngleY", pose.bodyAngleY);
        set("ParamBodyAngleZ", pose.bodyAngleZ);

        if (pose.cameraDriven) {
            // A is Haru's relaxed/crossed variant; B supplies the raised/bent variant.
            const auto arm = [&](const ArmPose& a, const char* upper, const char* lower,
                                 const char* partA, const char* partB) {
                const float lift = pose.armsEnabled ? std::clamp(a.lift, 0.f, 1.f) : 0;
                const float bend = pose.armsEnabled ? std::clamp(a.bend, 0.f, 1.f) : 0;
                set(upper, -1 + 2 * lift);
                set(lower, 5 * bend);
                const bool raised = lift > .25f || bend > .25f;
                const auto show = [&](const char* id, float opacity) {
                    set(id, opacity);
                    const auto handle = cubism::CubismFramework::GetIdManager()->GetId(id);
                    const int index = _model->GetPartIndex(handle);
                    if (index >= 0 && index < _model->GetPartCount())
                        _model->SetPartOpacity(index, opacity);
                };
                show(partA, raised ? 0.f : 1.f);
                show(partB, raised ? 1.f : 0.f);
            };
            arm(pose.arms.left, "ParamArmLA", "ParamArmLB", "Part01ArmLA001", "Part01ArmLB001");
            arm(pose.arms.right, "ParamArmRA", "ParamArmRB", "Part01ArmRA001", "Part01ArmRB001");
        }

        // 4. Save parameters
        _model->SaveParameters();

        // 5. Apply eye blink/pose and physics
        if (_pose)
            _pose->UpdateParameters(_model, elapsed);
        if (_physics)
            _physics->Evaluate(_model, elapsed);

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
        if (!asset_->GetModel())
            throw std::runtime_error("Invalid or unsupported Live2D moc3 model");
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
                if (std::strcmp(group, "Idle") == 0)
                    asset_->idle.push_back(asset_->motions.size());
                asset_->motions.push_back(std::move(motion));
            }
        }
        for (int i = 0; i < settings.GetExpressionCount(); ++i) {
            if (i >= 256)
                throw std::runtime_error("Too many Live2D expressions");
            const auto data = readAsset(assetPath(directory, settings.GetExpressionFileName(i)));
            Motion expression(asset_->LoadExpression(data.data(), static_cast<int>(data.size()),
                                                     settings.GetExpressionName(i)));
            if (!expression)
                throw std::runtime_error("Invalid Live2D expression");
            asset_->expressions.push_back(std::move(expression));
        }
        cubism::csmMap<cubism::csmString, float> layout;
        settings.GetLayoutMap(layout);
        asset_->GetModelMatrix()->SetupFromLayout(layout);
        Renderer::SetConstantSettings(1, gpu_->device.Get());
        asset_->CreateRenderer(1920, 1080);
        auto* renderer = asset_->GetRenderer<Renderer>();
        if (!renderer)
            throw std::runtime_error("Live2D D3D11 renderer initialization failed");
        for (int i = 0; i < settings.GetTextureCount(); ++i) {
            textures_.push_back(
                loadTexture(*gpu_, assetPath(directory, settings.GetTextureFileName(i))));
            renderer->BindTexture(i, textures_.back().Get());
        }
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
        projection.Scale(std::min(1.f, 1.f / aspect) * framing_.scale, top * framing_.scale);
        // Zoom toward the top of the fitted model to keep the head in view.
        projection.Translate(2 * framing_.x, (1 - framing_.scale) * top - 2 * framing_.y);
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
                                             const std::filesystem::path& path) {
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
    std::lock_guard gpuLock(gpu->mutex);
    std::lock_guard runtimeLock(cubismMutex);
    return std::make_unique<LoadedAvatar>(std::move(gpu), path);
}
}  // namespace cortex
#else
namespace cortex {
bool isCubismSupported() {
    return false;
}

std::unique_ptr<AvatarModel> loadAvatarModel(std::shared_ptr<GpuDevice>,
                                             const std::filesystem::path&) {
    throw std::runtime_error(
        "Live2D support is unavailable in this build. Install Cubism Native SDK and rebuild.");
}
}  // namespace cortex
#endif
