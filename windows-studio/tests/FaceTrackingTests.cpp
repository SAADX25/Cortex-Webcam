#include "cortex/filters/VideoProcessor.h"
#include <wincodec.h>
#include "cortex/filters/BodyTracker.h"
#include "cortex/filters/HeadPose.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <thread>
using namespace cortex;
namespace {
void check(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
std::shared_ptr<VideoFrame> imageFrame(const std::shared_ptr<GpuDevice>& gpu,
                                       const std::filesystem::path& path, bool rotated = false,
                                       std::vector<uint8_t>* rgba = nullptr) {
    ComPtr<IWICImagingFactory> factory;
    check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                     IID_PPV_ARGS(&factory))),
          "WIC factory");
    ComPtr<IWICBitmapDecoder> decoder;
    check(SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                       WICDecodeMetadataCacheOnLoad, &decoder)),
          "Read face fixture");
    ComPtr<IWICBitmapFrameDecode> source;
    decoder->GetFrame(0, &source);
    UINT w{}, h{};
    source->GetSize(&w, &h);
    check(w == 512 && h == 512, "Expected NASA 512x512 fixture");
    ComPtr<IWICFormatConverter> rgb;
    factory->CreateFormatConverter(&rgb);
    check(
        SUCCEEDED(rgb->Initialize(source.Get(), GUID_WICPixelFormat32bppRGBA,
                                  WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)),
        "Decode face fixture");
    std::vector<uint8_t> pixels(size_t(w) * h * 4), bytes(size_t(w) * h * 3 / 2);
    check(
        SUCCEEDED(rgb->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size()), pixels.data())),
        "Face pixels");
    if (rotated) {
        const auto upright = pixels;
        // Store a 270-degree image; 90-degree frame metadata must restore it.
        for (unsigned y = 0; y < h; ++y)
            for (unsigned x = 0; x < w; ++x)
                for (unsigned c = 0; c < 4; ++c)
                    pixels[(size_t(y) * w + x) * 4 + c] =
                        upright[(size_t(x) * w + w - 1 - y) * 4 + c];
    }
    if (rgba)
        *rgba = pixels;
    const auto limit = [](float value) {
        return static_cast<uint8_t>(std::clamp(std::lround(value), 0l, 255l));
    };
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
            const auto* p = &pixels[(size_t(y) * w + x) * 4];
            bytes[size_t(y) * w + x] = limit(16 + .257f * p[0] + .504f * p[1] + .098f * p[2]);
        }
    for (unsigned y = 0; y < h; y += 2)
        for (unsigned x = 0; x < w; x += 2) {
            float r = 0, g = 0, b = 0;
            for (unsigned dy = 0; dy < 2; ++dy)
                for (unsigned dx = 0; dx < 2; ++dx) {
                    const auto* p = &pixels[(size_t(y + dy) * w + x + dx) * 4];
                    r += p[0] * .25f;
                    g += p[1] * .25f;
                    b += p[2] * .25f;
                }
            const auto index = size_t(w) * h + size_t(y / 2) * w + x;
            bytes[index] = limit(128 - .148f * r - .291f * g + .439f * b);
            bytes[index + 1] = limit(128 + .439f * r - .368f * g - .071f * b);
        }
    auto frame = std::make_shared<VideoFrame>();
    frame->gpu = gpu;
    frame->width = w;
    frame->height = h;
    frame->matrix = VideoFrame::Matrix::Bt601;
    frame->range = VideoFrame::Range::Limited;
    frame->readyAt = frame->receivedAt = std::chrono::steady_clock::now();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 2;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_DECODER;
    std::vector<uint8_t> blank(bytes.size(), 128);
    const D3D11_SUBRESOURCE_DATA initial[]{{blank.data(), w, 0}, {bytes.data(), w, 0}};
    check(SUCCEEDED(gpu->device->CreateTexture2D(&desc, initial, &frame->texture)),
          "Face NV12 fixture");
    frame->arraySlice = 1;
    frame->rotationDegrees = rotated ? 90 : 0;
    return frame;
}
class FixtureTracker final : public FaceTracker {
public:
    FaceLandmarks face{{10, 10}, {30, 10}, {24, 23}, {20, 26}, {20, 31}, .5f, 0, 0, 0, 1};
    std::optional<FaceLandmarks> track(const VideoFrame&) override { return face; }
    std::optional<UpperBodyPose> upperBody() const override {
        return UpperBodyPose{{.5f, .7f, .9f}, {.2f, .3f, .9f}, .9f};
    }
};
class RecordingAvatar final : public AvatarModel {
public:
    AvatarPose pose;
    void draw(ID3D11DeviceContext*, const AvatarPose& p, unsigned, unsigned) override { pose = p; }
    void setAnimation(int, int) override {}
    void setFraming(const AvatarFraming&) override {}
};
}  // namespace
int main(int argc, char** argv) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        check(argc == 3, "Expected model directory and face fixture");
        auto gpu = GpuDevice::create();
        std::vector<uint8_t> rgb;
        auto frame = imageFrame(gpu, std::filesystem::path(argv[2]), false, &rgb);
        BodyTracker body(std::filesystem::path(argv[1]).parent_path() / "upper-body");
        const auto upper = body.track(rgb, 512, 512);
        std::cout << "Body confidence=" << upper.confidence << " L=" << upper.left.lift << ","
                  << upper.left.bend << "," << upper.left.confidence << " R=" << upper.right.lift
                  << "," << upper.right.bend << "," << upper.right.confidence << '\n';
        check(upper.confidence > .6f, "Actual upper body must be detected in the NASA fixture");
        std::fill(rgb.begin(), rgb.end(), uint8_t{128});
        check(body.track(rgb, 512, 512).confidence == 0, "Blank frame must not invent arm joints");
        const auto straight = mapArm({10, 10}, {10, 40}, {10, 70}, .9f);
        const auto raised = mapArm({10, 10}, {40, 10}, {40, -20}, .9f);
        check(straight.lift < .001f && straight.bend < .001f && raised.lift > .49f &&
                  raised.bend > .49f,
              "Arm geometry must distinguish a lowered straight arm from a raised bent arm");
        check(mapArm({10, 10}, {10, 40}, {10, 70}, .2f).confidence == 0,
              "Occluded wrist must not drive a pose");
        auto tracker = makeFaceTracker(gpu, std::filesystem::path(argv[1]));
        std::optional<FaceLandmarks> face;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        const auto start = std::chrono::steady_clock::now();
        while (!face && std::chrono::steady_clock::now() < deadline) {
            frame->sourceTimestamp90k += 3000;
            face = tracker->track(*frame);
            std::this_thread::sleep_for(std::chrono::milliseconds(35));
        }
        std::cout << tracker->status() << '\n';
        check(face.has_value(), "ONNX must detect the actual astronaut face in NV12 array slice 1");
        std::cout << "Initial detection ms="
                  << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                               start)
                         .count()
                  << '\n';
        std::cout << "Eyes=" << face->leftEye.x << ',' << face->leftEye.y << ' ' << face->rightEye.x
                  << ',' << face->rightEye.y << " mouth=" << face->upperLip.x << ','
                  << face->upperLip.y << ' ' << face->lowerLip.x << ',' << face->lowerLip.y << '\n';
        check(face->leftEye.x > 175 && face->rightEye.x < 285 && face->leftEye.y < 150 &&
                  face->leftEye.y > 65,
              "Decoded landmarks must be on the actual face, not the image center");
        const auto pose = AvatarFilter::map(*face);
        check(face->headRotation.has_value(), "Real 3D head fit must succeed");
        std::cout << "Head " << pose->yaw << "," << pose->pitch << "," << pose->roll << '\n';
        check(pose && std::isfinite(pose->yaw) && pose->leftEyeOpen >= 0 && pose->rightEyeOpen <= 1,
              "Detected facial features must produce a finite live pose");
        tracker.reset();
        frame = imageFrame(gpu, std::filesystem::path(argv[2]), true);
        tracker = makeFaceTracker(gpu, std::filesystem::path(argv[1]));
        face.reset();
        const auto rotateDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!face && std::chrono::steady_clock::now() < rotateDeadline) {
            frame->sourceTimestamp90k += 3000;
            face = tracker->track(*frame);
            std::this_thread::sleep_for(std::chrono::milliseconds(35));
        }
        check(face && face->leftEye.x > 175 && face->leftEye.y < 150 && face->leftEye.y > 65,
              "Rotation metadata must restore upright facial landmarks before inference");
        // A blank image must clear the last face instead of synthesizing landmarks.
        std::vector<uint8_t> blank(512 * 512 * 3 / 2, 128);
        {
            std::lock_guard lock(gpu->mutex);
            gpu->context->UpdateSubresource(frame->texture.Get(), 1, nullptr, blank.data(), 512, 0);
        }
        bool cleared = false;
        const auto noFaceDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!cleared && std::chrono::steady_clock::now() < noFaceDeadline) {
            frame->sourceTimestamp90k += 3000;
            const auto value = tracker->track(*frame);
            cleared = !value && tracker->status().starts_with("Face not found");
            std::this_thread::sleep_for(std::chrono::milliseconds(35));
        }
        check(cleared, "Blank frame must clear the face and report tracking loss");
        tracker.reset();  // Must join an idle or busy inference worker cleanly.
        auto missing = makeFaceTracker(gpu, L"missing-model-directory");
        const auto missingDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!missing->status().starts_with("Face tracking unavailable") &&
               std::chrono::steady_clock::now() < missingDeadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        check(!missing->track(*frame) && missing->status().starts_with("Face tracking unavailable"),
              "Missing assets must be explicit and nonfatal");
        VideoProcessor processor(gpu, 128, 128);
        auto recording = std::make_unique<RecordingAvatar>();
        auto* recorded = recording.get();
        processor.setAvatarModel(std::move(recording));
        processor.setFaceTracker(std::make_unique<FixtureTracker>());
        FilterSettings settings;
        settings.avatarEnabled = true;
        settings.avatarMode = 1;
        settings.avatarSmoothing = 0;
        settings.avatarSensitivity = 1;
        processor.setSettings(settings);
        frame->readyAt = std::chrono::steady_clock::now();
        SceneSnapshot scene{{{SourceId::Phone, frame, {}}}};
        processor.render(scene);
        check(recorded->pose.yaw > 10 && recorded->pose.mouth > .9f &&
                  recorded->pose.leftEyeOpen == 0 && recorded->pose.armsEnabled &&
                  recorded->pose.arms.left.bend > .69f,
              "Camera pose, mouth and blink must reach the rendered avatar");
        settings.avatarCalibration = 1;
        processor.setSettings(settings);
        processor.render(scene);
        check(std::abs(recorded->pose.yaw) < .001f && std::abs(recorded->pose.pitch) < .001f &&
                  recorded->pose.mouth > .9f,
              "Recenter must zero only head angles, preserving mouth and blink");
        settings.avatarTestMotion = true;
        processor.setSettings(settings);
        processor.render(scene);
        check(recorded->pose.leftEyeOpen < 0,
              "Demo must use procedural pose, not stale live landmarks");
        std::cout << "PASS: real face inference, tracking loss, missing assets, pipeline mapping "
                     "and recenter.\n";
        if (SUCCEEDED(com))
            CoUninitialize();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        if (SUCCEEDED(com))
            CoUninitialize();
        return 1;
    }
}
