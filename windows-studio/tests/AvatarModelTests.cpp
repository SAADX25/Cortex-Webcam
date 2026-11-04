#include "cortex/filters/VideoProcessor.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>

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
        auto model = loadAvatarModel(gpu, path);
        check(bool(model), "Haru model must load");
        processor.setAvatarModel(std::move(model));
        FilterSettings settings;
        settings.avatarEnabled = true;
        settings.avatarMode = 1;
        settings.transparent = true;
        processor.setSettings(settings);
        auto first = read(gpu, processor.render({}));
        check(visible(first) > 25000, "Haru must produce textured visible pixels");
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
        closeupPath.replace_filename("haru-avatar-closeup.bmp");
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
        controls->face.headRotation = HeadRotation{25, -15, 12};
        frame->readyAt = std::chrono::steady_clock::now();
        const auto turned = read(gpu, processor.render(live));
        check(changedPixels(neutralPose, turned) > 1000,
              "Measured head angles must visibly deform the actual Haru model");
        auto neutralPath = std::filesystem::path(argv[2]);
        neutralPath.replace_filename("haru-camera-neutral.bmp");
        saveBmp(neutralPath, neutralPose, 960, 720);
        controls->arms = {{.8f, .6f, .9f}, {.5f, .8f, .9f}, .9f};
        frame->readyAt = std::chrono::steady_clock::now();
        const auto raised = read(gpu, processor.render(live));
        check(changedPixels(turned, raised) > 1000 &&
                  armCoverage(raised) > armCoverage(turned) + 2000,
              "Tracked arms must occupy new arm pixels outside the torso, not just move the hair");
        auto raisedPath = std::filesystem::path(argv[2]);
        raisedPath.replace_filename("haru-camera-arms.bmp");
        saveBmp(raisedPath, raised, 960, 720);
        settings.avatarFraming = {};
        processor.setSettings(settings);
        // Each bundled motion and expression must be safe to select on the rendering thread.
        std::cout << "Rendered Haru; testing animations" << std::endl;
        settings.avatarTestMotion = true;
        for (int motion = 0; motion < 6; ++motion) {
            settings.avatarMotion = motion;
            for (int expression = -1; expression < 8; ++expression) {
                settings.avatarExpression = expression;
                processor.setSettings(settings);
                processor.render({});
            }
        }
        auto animated = read(gpu, processor.exportFrame(true));
        check(animated != exported, "Motions or expressions must change the model output");
        std::cout << "Switching to GPU avatar" << std::endl;
        settings.avatarMode = 0;
        processor.setSettings(settings);
        auto fallback = read(gpu, processor.render({}));
        check(visible(fallback) > 10000, "Switching a loaded model to GPU test avatar must work");
        std::cout << "Unloading model" << std::endl;
        processor.setAvatarModel(nullptr);
        std::cout << "Reloading moc3" << std::endl;
        auto mocPath = path;
        mocPath.replace_filename("Haru.moc3");
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
        std::cout << "Haru loading, D3D11 pixels, 6 motions, 8 expressions, alpha, switching and "
                     "export passed"
                  << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
