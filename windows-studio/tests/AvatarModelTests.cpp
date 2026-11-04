#include "cortex/filters/VideoProcessor.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>

using namespace cortex;
namespace {
class ControlledTracking final : public FaceTracker {
public:
    FaceLandmarks face{{10, 10}, {30, 10}, {20, 21}, {20, 26}, {20, 26}};
    UpperBodyPose arms;
    std::optional<FaceLandmarks> track(const VideoFrame&) override { return face; }
    std::optional<UpperBodyPose> upperBody() const override { return arms; }
};
size_t armCoverage(const std::vector<uint8_t>& pixels) {
    size_t count = 0;
    for (size_t y = 275; y < 500; ++y)
        for (size_t x = 0; x < 960; ++x)
            if ((x < 400 || x > 575) && pixels[(y * 960 + x) * 4 + 3] > 100)
                ++count;
    return count;
}
size_t changedPixels(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    size_t count = 0;
    for (size_t i = 0; i < a.size(); i += 4)
        if (std::abs(int(a[i]) - b[i]) + std::abs(int(a[i + 1]) - b[i + 1]) +
                std::abs(int(a[i + 2]) - b[i + 2]) >
            30)
            ++count;
    return count;
}
size_t changedHairPixels(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    size_t count{};
    // Ren's head at the 1.8x framing below; exclude the eyes, mouth and torso.
    for (size_t y = 35; y < 135; ++y)
        for (size_t x = 370; x < 595; ++x) {
            const auto i = (y * 960 + x) * 4;
            if (std::abs(int(a[i]) - b[i]) + std::abs(int(a[i + 1]) - b[i + 1]) +
                    std::abs(int(a[i + 2]) - b[i + 2]) > 30)
                ++count;
        }
    return count;
}
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
std::vector<uint8_t> read(const std::shared_ptr<GpuDevice>& gpu, const GpuImage& image) {
    std::lock_guard lock(gpu->mutex);
    D3D11_TEXTURE2D_DESC desc{};
    image.texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    check(SUCCEEDED(gpu->device->CreateTexture2D(&desc, nullptr, &staging)), "Readback texture");
    gpu->context->CopyResource(staging.Get(), image.texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(SUCCEEDED(gpu->context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)),
          "Readback map");
    std::vector<uint8_t> pixels(size_t(image.width) * image.height * 4);
    for (unsigned row = 0; row < image.height; ++row)
        std::memcpy(pixels.data() + size_t(row) * image.width * 4,
                    static_cast<const uint8_t*>(mapped.pData) + size_t(row) * mapped.RowPitch,
                    image.width * 4);
    gpu->context->Unmap(staging.Get(), 0);
    return pixels;
}
size_t visible(const std::vector<uint8_t>& pixels) {
    size_t count{};
    for (size_t i = 3; i < pixels.size(); i += 4)
        if (pixels[i] > 100)
            ++count;
    return count;
}
double centerX(const std::vector<uint8_t>& pixels, unsigned width) {
    double sum = 0, count = 0;
    for (size_t i = 0; i < pixels.size(); i += 4)
        if (pixels[i + 3] > 100) {
            sum += (i / 4) % width;
            ++count;
        }
    check(count > 0, "Avatar must remain visible");
    return sum / count;
}
double centerY(const std::vector<uint8_t>& pixels, unsigned width) {
    double sum = 0, count = 0;
    for (size_t i = 0; i < pixels.size(); i += 4)
        if (pixels[i + 3] > 100) {
            sum += (i / 4) / width;
            ++count;
        }
    check(count > 0, "Avatar must remain visible");
    return sum / count;
}
void saveBmp(const std::filesystem::path& file, std::vector<uint8_t> rgba, int width, int height) {
    for (size_t i = 0; i < rgba.size(); i += 4)
        std::swap(rgba[i], rgba[i + 2]);
    const uint32_t header[] = {uint32_t(rgba.size() + 54),
                               0,
                               54,
                               40,
                               uint32_t(width),
                               uint32_t(-height),
                               32u << 16 | 1u,
                               0,
                               uint32_t(rgba.size()),
                               0,
                               0,
                               0,
                               0};
    std::ofstream out(file, std::ios::binary);
    out.write("BM", 2);
    out.write(reinterpret_cast<const char*>(header), sizeof(header));
    out.write(reinterpret_cast<const char*>(rgba.data()), std::streamsize(rgba.size()));
    check(bool(out), "Save rendered Haru snapshot");
}
}  // namespace
int main(int argc, char** argv) {
    try {
        check(argc == 3, "Expected model and snapshot paths");
        check(isCubismSupported(), "Cubism must be enabled in this build");
        auto gpu = GpuDevice::create();
        const std::filesystem::path path(argv[1]);
        bool rejected = false;
        try {
            loadAvatarModel(gpu, path.parent_path() / "missing.model3.json");
        } catch (const std::exception&) {
            rejected = true;
        }
        check(rejected, "Missing model must report an error");
        VideoProcessor processor(gpu, 960, 720);
        std::stop_source cancelled;
        cancelled.request_stop();
        rejected = false;
        try {
            loadAvatarModel(gpu, path, cancelled.get_token());
        } catch (const std::exception& error) {
            rejected = std::string(error.what()).find("cancelled") != std::string::npos;
        }
        check(rejected, "An already cancelled load must report cancellation");

        // A first load compiles the SDK shaders. Stop while that work is active
        // and then load again to verify that partial shaders do not poison retry.
        std::cout << "Testing cancellation during first Live2D load" << std::endl;
        std::stop_source coldStop;
        auto coldLoad = std::async(std::launch::async, [gpu, path, token = coldStop.get_token()] {
            return loadAvatarModel(gpu, path, token);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto cancelAt = std::chrono::steady_clock::now();
        coldStop.request_stop();
        check(coldLoad.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
              "Cancelling first shader compilation must finish within five seconds");
        rejected = false;
        try {
            coldLoad.get();
        } catch (const std::exception& error) {
            rejected = std::string(error.what()).find("cancelled") != std::string::npos;
        }
        check(rejected, "A running cold load must observe cancellation");
        std::cout << "Cold load cancellation completed in "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - cancelAt).count()
                  << " ms; testing retry" << std::endl;
        auto model = loadAvatarModel(gpu, path);
        check(bool(model), "Ren model must load");
        processor.setAvatarModel(std::move(model));
        std::future<std::unique_ptr<AvatarModel>> reloading;
        {
            // Rendering owns this lock before it reaches Cubism. Reloading must
            // not wait for it while holding the Cubism runtime lock.
            std::lock_guard gpuLock(gpu->mutex);
            reloading = std::async(std::launch::async, [gpu, path] {
                return loadAvatarModel(gpu, path);
            });
            check(reloading.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
                  "Reload must finish while the render thread owns the GPU lock");
        }
        processor.setAvatarModel(reloading.get());
        FilterSettings settings;
        settings.avatarEnabled = true;
        settings.avatarMode = 1;
        settings.avatarArms = true;
        settings.transparent = true;
        processor.setSettings(settings);
        auto first = read(gpu, processor.render({}));
        check(visible(first) > 25000, "Ren must produce textured visible pixels");
        auto exported = read(gpu, processor.exportFrame(true));
        check(visible(exported) > 25000 && exported[3] == 0, "Avatar export must preserve alpha");
        saveBmp(argv[2], read(gpu, processor.exportFrame(false)), 960, 720);
        settings.avatarFraming = {3.2f, 0, -.05f};
        processor.setSettings(settings);
        const auto closeup = read(gpu, processor.render({}));
        check(visible(closeup) > visible(first) * 1.5,
              "Face close-up must increase model coverage");
        check(visible(read(gpu, processor.exportFrame(true))) == visible(closeup),
              "Camera export must use the same avatar framing");
        auto closeupPath = std::filesystem::path(argv[2]);
        closeupPath.replace_filename("ren-avatar-closeup.bmp");
        saveBmp(closeupPath, read(gpu, processor.exportFrame(false)), 960, 720);
        settings.avatarFraming = {1, .25f, 0};
        processor.setSettings(settings);
        const auto moved = read(gpu, processor.render({}));
        check(centerX(moved, 960) - centerX(first, 960) > 170,
              "Horizontal position must move the actual textured model");
        settings.avatarFraming = {};
        processor.setSettings(settings);
        auto tracking = std::make_unique<ControlledTracking>();
        auto* controls = tracking.get();
        controls->face.headRotation = HeadRotation{};
        processor.setFaceTracker(std::move(tracking));
        auto frame = std::make_shared<VideoFrame>();
        frame->readyAt = std::chrono::steady_clock::now();
        SceneSnapshot live{{{SourceId::Phone, frame, {}}}};
        settings.avatarSmoothing = 0;
        settings.avatarSensitivity = 1;
        settings.avatarFraming = {1.8f, 0, 0};
        processor.setSettings(settings);
        const auto neutralPose = read(gpu, processor.render(live));
        for (int i = 0; i < 12; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            frame->readyAt = std::chrono::steady_clock::now();
            processor.render(live);
        }
        const auto steadyPose = read(gpu, processor.render(live));
        check(changedPixels(neutralPose, steadyPose) < 100,
              "A still camera pose must not be moved by prerecorded idle animations");
        const HeadRotation axes[]{{25, 0, 0}, {-25, 0, 0}, {0, 20, 0},
                                  {0, -20, 0}, {0, 0, 20}, {0, 0, -20}};
        const char* axisNames[]{"yaw-positive", "yaw-negative", "pitch-positive", "pitch-negative",
                                "roll-positive", "roll-negative"};
        for (size_t i = 0; i < std::size(axes); ++i) {
            controls->face.headRotation = axes[i];
            frame->readyAt = std::chrono::steady_clock::now();
            const auto rotated = read(gpu, processor.render(live));
            check(changedPixels(steadyPose, rotated) > 500,
                  "Each head-rotation axis must independently deform the textured model");
            auto axisPath = std::filesystem::path(argv[2]);
            axisPath.replace_filename(std::string("ren-camera-") + axisNames[i] + ".bmp");
            saveBmp(axisPath, rotated, 960, 720);
        }
        controls->face.headRotation = HeadRotation{25, -15, 12};
        frame->readyAt = std::chrono::steady_clock::now();
        const auto turned = read(gpu, processor.render(live));
        check(changedPixels(neutralPose, turned) > 1000,
              "Measured head angles must visibly deform the actual Ren model");
        for (int i = 0; i < 20; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            frame->readyAt = std::chrono::steady_clock::now();
            processor.render(live);
        }
        const auto hairFollowing = read(gpu, processor.render(live));
        check(changedHairPixels(turned, hairFollowing) > 100,
              "Authored hair physics must continue responding after the tracked head stops");
        auto neutralPath = std::filesystem::path(argv[2]);
        neutralPath.replace_filename("ren-camera-neutral.bmp");
        saveBmp(neutralPath, neutralPose, 960, 720);
        settings.avatarArms = true;
        processor.setSettings(settings);
        controls->arms = {{.8f, .6f, .9f}, {.5f, .8f, .9f}, .9f};
        frame->readyAt = std::chrono::steady_clock::now();
        const auto raised = read(gpu, processor.render(live));
        check(changedPixels(turned, raised) > 1000 &&
                  armCoverage(raised) > armCoverage(turned) + 2000,
              "Tracked arms must occupy new arm pixels outside the torso, not just move the hair");
        controls->arms.left = {.8f, 0.f, .9f};
        frame->readyAt = std::chrono::steady_clock::now();
        const auto straightElbow = read(gpu, processor.render(live));
        controls->arms.left.bend = .8f;
        frame->readyAt = std::chrono::steady_clock::now();
        const auto bentElbow = read(gpu, processor.render(live));
        check(changedPixels(straightElbow, bentElbow) > 250,
              "Ren elbow bend must deform the arm independently of an unchanged shoulder lift");
        controls->arms.bodyAngleX = 8.f;
        controls->arms.bodyAngleZ = -6.f;
        frame->readyAt = std::chrono::steady_clock::now();
        const auto bodyTurned = read(gpu, processor.render(live));
        check(changedPixels(raised, bodyTurned) > 500,
              "Tracked body angles must deform the Live2D body parameters");
        auto raisedPath = std::filesystem::path(argv[2]);
        raisedPath.replace_filename("ren-camera-arms.bmp");
        saveBmp(raisedPath, raised, 960, 720);
        const auto previousFace = controls->face;
        controls->face = FaceLandmarks{{40, 50}, {60, 50}, {50, 61}, {50, 66}, {50, 66}};
        controls->face.headRotation = HeadRotation{};
        controls->face.imageWidth = 100;
        controls->face.imageHeight = 100;
        controls->face.headDepth = 5;
        settings.avatarArms = false;
        settings.avatarMirror = false;
        settings.avatarFraming = {1.4f, 0, 0};
        ++settings.avatarCalibration;
        processor.setSettings(settings);
        frame->readyAt = std::chrono::steady_clock::now();
        const auto centered = read(gpu, processor.render(live));
        controls->face.headDepth = 3.5f;
        frame->readyAt = std::chrono::steady_clock::now();
        const auto closer = read(gpu, processor.render(live));
        check(visible(closer) > visible(centered) * 1.15,
              "Moving closer must enlarge the actual model using fitted depth");
        settings.avatarFollowDistance = false;
        processor.setSettings(settings);
        frame->readyAt = std::chrono::steady_clock::now();
        const auto distanceDisabled = read(gpu, processor.render(live));
        check(std::abs(double(visible(distanceDisabled)) - visible(centered)) <
                  visible(centered) * .05,
              "Disabling camera distance following must preserve the manual framing");
        controls->face.headDepth = 5;
        settings.avatarFollowDistance = true;
        processor.setSettings(settings);
        for (auto* point : {&controls->face.leftEye, &controls->face.rightEye,
                           &controls->face.nose, &controls->face.upperLip,
                           &controls->face.lowerLip}) {
            point->x += 15.f;
            point->y += 10.f;
        }
        frame->readyAt = std::chrono::steady_clock::now();
        const auto translated = read(gpu, processor.render(live));
        check(centerX(translated, 960) - centerX(centered, 960) > 80,
              "Moving sideways in camera must translate the actual Live2D character");
        check(centerY(translated, 960) - centerY(centered, 960) > 15,
              "Moving down in camera must translate the actual Live2D character down");
        settings.avatarMirror = true;
        processor.setSettings(settings);
        frame->readyAt = std::chrono::steady_clock::now();
        const auto mirroredTranslation = read(gpu, processor.render(live));
        check(centerX(centered, 960) - centerX(mirroredTranslation, 960) > 80,
              "Mirror must reverse camera translation as well as head rotation");
        auto translationPath = std::filesystem::path(argv[2]);
        translationPath.replace_filename("ren-camera-translation.bmp");
        saveBmp(translationPath, translated, 960, 720);
        controls->face = previousFace;
        settings.avatarMirror = false;
        settings.avatarFraming = {};
        settings.avatarArms = false;
        processor.setSettings(settings);
        // Each bundled motion and expression must be safe to select on the rendering thread.
        std::cout << "Rendered Ren; testing animations" << std::endl;
        settings.avatarTestMotion = true;
        for (int motion = 0; motion < 3; ++motion) {
            settings.avatarMotion = motion;
            for (int expression = -1; expression < 5; ++expression) {
                settings.avatarExpression = expression;
                processor.setSettings(settings);
                processor.render({});
            }
        }
        auto animated = read(gpu, processor.exportFrame(true));
        check(animated != exported, "Motions or expressions must change the model output");
        const auto absPath = std::filesystem::absolute(path);
        const auto rootDir = absPath.parent_path().parent_path().parent_path().parent_path();

        const auto markCandidate = rootDir / "windows-studio" / ".deps" / "cubism-sdk" / "Samples" / "Resources" / "Mark" / "Mark.model3.json";
        auto customPath = std::filesystem::exists(markCandidate) ? markCandidate : std::filesystem::path{};
        if (!customPath.empty()) {
            std::cout << "Testing model swap to " << customPath.filename().string() << std::endl;
            auto maleModel = loadAvatarModel(gpu, customPath);
            check(bool(maleModel), "Custom male model must load");
            processor.setAvatarModel(std::move(maleModel));
            settings.avatarMode = 1;
            settings.avatarEnabled = true;
            settings.transparent = true;
            settings.avatarTestMotion = false;
            settings.avatarFraming = {};
            processor.setSettings(settings);
            controls->face.headRotation = HeadRotation{};
            frame->readyAt = std::chrono::steady_clock::now();
            auto maleNeutral = read(gpu, processor.render(live));
            check(visible(maleNeutral) > 10000, "Male model must produce textured visible pixels");

            controls->face.headRotation = HeadRotation{20, -10, 10};
            frame->readyAt = std::chrono::steady_clock::now();
            auto maleTurned = read(gpu, processor.render(live));
            check(changedPixels(maleNeutral, maleTurned) > 500,
                  "Head rotation must deform custom male model");

            controls->face.eyeOpen = 0.0f;
            controls->face.leftEyeOpen = 0.0f;
            controls->face.rightEyeOpen = 0.0f;
            controls->face.mouthOpenY = 0.8f;
            frame->readyAt = std::chrono::steady_clock::now();
            auto maleExpression = read(gpu, processor.render(live));
            check(changedPixels(maleTurned, maleExpression) > 500,
                  "Face blink and mouth tracking must deform custom male model");
            std::cout << "Model swap to " << customPath.filename().string() << " and tracking passed" << std::endl;
        }
        std::cout << "Switching to GPU avatar" << std::endl;
        settings.avatarMode = 0;
        processor.setSettings(settings);
        auto fallback = read(gpu, processor.render({}));
        check(visible(fallback) > 10000, "Switching a loaded model to GPU test avatar must work");
        std::cout << "Unloading model" << std::endl;
        processor.setAvatarModel(nullptr);
        std::cout << "Reloading moc3" << std::endl;
        auto mocPath = path;
        mocPath.replace_filename("Ren.moc3");
        processor.setAvatarModel(loadAvatarModel(gpu, mocPath));
        settings.avatarMode = 1;
        processor.setSettings(settings);
        check(visible(read(gpu, processor.render({}))) > 25000,
              "moc3 selection must resolve companion settings");
        std::cout << "Disabling avatar" << std::endl;
        settings.avatarEnabled = false;
        settings.background[0] = 1;
        settings.transparent = false;
        processor.setSettings(settings);
        std::cout << "Rendering normal background" << std::endl;
        auto background = read(gpu, processor.render({}));
        std::cout << "Exporting normal background" << std::endl;
        auto camera = read(gpu, processor.exportFrame());
        std::cout << "Checking normal background" << std::endl;
        check(background[0] > 250 && camera[0] > 250 && camera[3] == 255,
              "Normal rendering and camera export must survive Cubism state changes");
        std::cout << "Final model unload" << std::endl;
        processor.setAvatarModel(nullptr);
        std::cout << "Ren loading, D3D11 pixels, motions, expressions, alpha, switching and "
                     "export passed"
                  << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
