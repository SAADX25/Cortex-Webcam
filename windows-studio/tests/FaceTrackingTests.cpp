#include "cortex/processing/VideoProcessor.h"
#include <wincodec.h>
#include "cortex/tracking/BodyTracker.h"
#include "cortex/tracking/HeadPose.h"
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
    UpperBodyPose body{{.5f, .7f, .9f}, {.2f, .3f, .9f}, .9f};
    bool faceVisible{true};
    std::optional<FaceLandmarks> track(const VideoFrame&) override {
        return faceVisible ? std::optional(face) : std::nullopt;
    }
    std::optional<UpperBodyPose> upperBody() const override { return body; }
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
        {
            // Forearm points down 30 px; open hand reaches 12 px with wide spread,
            // fist keeps fingertips 3 px from the wrist and together.
            auto open = mapArm({10, 10}, {10, 40}, {10, 70}, .9f);
            mapHand(open, {10, 40}, {10, 70}, {6, 82}, {14, 82}, {20, 76}, .9f);
            auto fist = mapArm({10, 10}, {10, 40}, {10, 70}, .9f);
            mapHand(fist, {10, 40}, {10, 70}, {9.5f, 73}, {10.5f, 73}, {11, 72}, .9f);
            check(open.handConfidence > 0 && fist.handConfidence > 0 && open.hand > .8f &&
                      fist.hand < .2f && std::abs(open.wrist) < .1f,
                  "Hand geometry must distinguish an open hand from a fist");
            auto bent = mapArm({10, 10}, {10, 40}, {10, 70}, .9f);
            mapHand(bent, {10, 40}, {10, 70}, {22, 66}, {22, 74}, {20, 78}, .9f);
            check(std::abs(bent.wrist) > .8f, "A wrist bent 90 degrees must reach the limit");
            auto hidden = mapArm({10, 10}, {10, 40}, {10, 70}, .9f);
            mapHand(hidden, {10, 40}, {10, 70}, {6, 82}, {14, 82}, {20, 76}, .2f);
            check(hidden.handConfidence == 0, "Occluded hand must not drive the fingers");
        }
        const auto bodyTilt = mapBodyAngles({20, 25}, {10, 20}, 0.5f, -0.5f, 0.2f, -0.2f, true);
        check(bodyTilt.angleZ > 0 && bodyTilt.angleX > 0,
              "Body angle mapping must recover tilt and rotation");
        auto tracker = makeFaceTracker(gpu, std::filesystem::path(argv[1]));
        std::optional<FaceLandmarks> face;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        const auto start = std::chrono::steady_clock::now();
        while (!face && std::chrono::steady_clock::now() < deadline) {
            frame->readyAt = std::chrono::steady_clock::now();
            frame->sourceTimestamp90k += 3000;
            face = tracker->track(*frame);
            std::this_thread::sleep_for(std::chrono::milliseconds(35));
        }
        std::cout << tracker->status() << '\n';
        check(face.has_value(), "ONNX must detect the actual astronaut face in NV12 array slice 1");
        check(face->imageWidth == 512 && face->imageHeight == 512,
              "Tracked face must retain upright image dimensions for position following");
        check(std::isfinite(face->gazeX) && std::isfinite(face->gazeY) &&
                  std::abs(face->gazeX) <= 1 && std::abs(face->gazeY) <= 1 &&
                  tracker->status().find("Quality tracking") != std::string::npos &&
                  tracker->status().find("Pupil gaze") != std::string::npos &&
                  face->headDepth > 0 && face->hasHeadPosition &&
                  face->capturedAt.time_since_epoch().count() != 0,
              "The quality model, pupil inference and fitted spatial head pose must run on a real image");
        std::cout << "Pupil gaze=" << face->gazeX << ',' << face->gazeY << '\n';
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
        frame = imageFrame(gpu, std::filesystem::path(argv[2]), true);
        check(!tracker->track(*frame),
              "Changing rotation on the same tracker must invalidate the old face immediately");
        face.reset();
        const auto rotateDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!face && std::chrono::steady_clock::now() < rotateDeadline) {
            frame->readyAt = std::chrono::steady_clock::now();
            frame->sourceTimestamp90k += 3000;
            face = tracker->track(*frame);
            std::this_thread::sleep_for(std::chrono::milliseconds(35));
        }
        check(face && face->leftEye.x > 175 && face->leftEye.y < 150 && face->leftEye.y > 65,
              "Rotation metadata must restore upright facial landmarks before inference");
        frame->readyAt = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        check(!tracker->track(*frame),
              "A stale camera texture must not refresh or replay a previously tracked face");
        // A blank image must clear the last face instead of synthesizing landmarks.
        std::vector<uint8_t> blank(512 * 512 * 3 / 2, 128);
        {
            std::lock_guard lock(gpu->mutex);
            gpu->context->UpdateSubresource(frame->texture.Get(), 1, nullptr, blank.data(), 512, 0);
        }
        bool cleared = false;
        const auto noFaceDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!cleared && std::chrono::steady_clock::now() < noFaceDeadline) {
            frame->readyAt = std::chrono::steady_clock::now();
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
        auto fixtureTracker = std::make_unique<FixtureTracker>();
        auto* controlled = fixtureTracker.get();
        controlled->face.imageWidth = controlled->face.imageHeight = 512;
        controlled->face.headRotation = HeadRotation{0, 45, 0};
        controlled->face.needsHeadFit = true;
        processor.setFaceTracker(std::move(fixtureTracker));
        FilterSettings settings;
        settings.avatarEnabled = true;
        settings.avatarSmoothing = 0;
        settings.avatarArms = true;
        settings.avatarSensitivity = 1;
        settings.avatarMirror = false;
        controlled->face.headDepth = 5;
        processor.setSettings(settings);
        frame->readyAt = std::chrono::steady_clock::now();
        SceneSnapshot scene{{{SourceId::Phone, frame, {}}}};
        processor.render(scene);
        check(std::abs(recorded->pose.pitch) < .001f &&
                  std::abs(recorded->pose.positionX) < .001f,
              "The first physical head pose must automatically establish neutral framing");
        controlled->face.headRotation = HeadRotation{20, 35, 0};
        controlled->face.headDepth = 3.5f;
        controlled->face.gazeX = .35f;
        for (auto* point : {&controlled->face.leftEye, &controlled->face.rightEye,
                            &controlled->face.nose, &controlled->face.upperLip,
                            &controlled->face.lowerLip}) {
            point->x += 48;
            point->y += 32;
        }
        frame->readyAt = std::chrono::steady_clock::now();
        processor.render(scene);
        check(recorded->pose.yaw > 19.9f && std::abs(recorded->pose.pitch + 10) < .001f &&
                  recorded->pose.depthScale > 1.42f && recorded->pose.gazeX == .35f &&
                  recorded->pose.positionX > .09f && recorded->pose.positionY > .06f &&
                  recorded->pose.mouth > .9f &&
                  recorded->pose.leftEyeOpen == 0 && recorded->pose.armsEnabled &&
                  recorded->pose.arms.left.bend > .69f,
              "Physical head deltas beyond the model range, position, mouth and blink must reach the avatar");

        // Test mirrored camera orientation
        settings.avatarMirror = true;
        processor.setSettings(settings);
        processor.render(scene);
        check(recorded->pose.yaw < -19.9f && recorded->pose.positionX < -.09f &&
                  recorded->pose.gazeX == -.35f &&
                  recorded->pose.positionY > .06f && recorded->pose.mouth > .9f &&
                  recorded->pose.rightEyeOpen == 0 && recorded->pose.armsEnabled &&
                  recorded->pose.arms.right.bend > .69f,
              "Mirrored camera pose must invert yaw and swap eyes/arms");

        settings.avatarFollowPosition = false;
        processor.setSettings(settings);
        processor.render(scene);
        check(std::abs(recorded->pose.positionX) < .001f &&
                  std::abs(recorded->pose.positionY) < .001f && recorded->pose.yaw < -19.9f,
              "Disabling position following must keep head rotation active");
        settings.avatarFollowDistance = false;
        processor.setSettings(settings);
        processor.render(scene);
        check(recorded->pose.depthScale == 1 && recorded->pose.yaw < -19.9f,
              "Distance following must be independently switchable without losing head rotation");
        settings.avatarFollowDistance = true;
        settings.avatarFollowPosition = true;

        settings.avatarCalibration = 1;
        processor.setSettings(settings);
        processor.render(scene);
        check(std::abs(recorded->pose.yaw) < .001f && std::abs(recorded->pose.pitch) < .001f &&
                  std::abs(recorded->pose.positionX) < .001f &&
                  std::abs(recorded->pose.positionY) < .001f && recorded->pose.mouth > .9f,
              "Recenter must zero head angles and position, preserving mouth and blink");
        check(recorded->pose.depthScale == 1, "Recenter must establish the reference camera distance");
        settings.avatarMirror = false;
        settings.avatarSmoothing = .06f;
        processor.setSettings(settings);
        controlled->face.capturedAt = std::chrono::steady_clock::now();
        controlled->body.capturedAt = controlled->face.capturedAt;
        controlled->face.headRotation = HeadRotation{5, 35, 0};
        frame->readyAt = controlled->face.capturedAt;
        processor.render(scene);
        const auto once = recorded->pose;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        frame->readyAt = std::chrono::steady_clock::now();
        processor.render(scene);
        check(recorded->pose.yaw == once.yaw && recorded->pose.depthScale == once.depthScale,
              "Rendering the same inference twice must not manufacture new motion samples");
        controlled->body.left.lift = .95f;
        controlled->body.bodyAngleZ = 12.f;
        controlled->body.capturedAt = std::chrono::steady_clock::now();
        processor.render(scene);
        check(recorded->pose.arms.left.lift > once.arms.left.lift + .001f &&
                  recorded->pose.bodyAngleZ > once.bodyAngleZ + .001f &&
                  recorded->pose.yaw == once.yaw,
              "A new body sample must move arms and torso while the face sample is unchanged");
        const auto bodyOnce = recorded->pose;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        frame->readyAt = std::chrono::steady_clock::now();
        processor.render(scene);
        check(recorded->pose.arms.left.lift == bodyOnce.arms.left.lift &&
                  recorded->pose.bodyAngleZ == bodyOnce.bodyAngleZ &&
                  recorded->pose.yaw == bodyOnce.yaw,
              "A fresh render frame must not replay either cached inference sample");
        settings.avatarSmoothing = 0;
        processor.setSettings(settings);
        controlled->faceVisible = false;
        controlled->body.left.lift = .15f;
        controlled->body.bodyAngleZ = -7.f;
        controlled->body.capturedAt = std::chrono::steady_clock::now();
        processor.render(scene);
        check(std::abs(recorded->pose.arms.left.lift - .15f) < .001f &&
                  std::abs(recorded->pose.bodyAngleZ + 7.f) < .001f,
              "Face loss must not overwrite a fresh independent body measurement");
        settings.avatarSmoothing = .06f;
        processor.setSettings(settings);
        processor.render(SceneSnapshot{});
        const auto firstLoss = recorded->pose;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        processor.render(SceneSnapshot{});
        check(recorded->pose.arms.left.confidence == 0 &&
                  std::abs(recorded->pose.bodyAngleZ) < std::abs(firstLoss.bodyAngleZ),
              "Losing the phone input must keep decaying the body on render ticks");
        settings.avatarSmoothing = 0;
        processor.setSettings(settings);
        controlled->faceVisible = true;
        frame->readyAt = std::chrono::steady_clock::now();
        processor.render(scene);
        settings.avatarArms = false;
        processor.setSettings(settings);
        processor.render(scene);
        check(!recorded->pose.armsEnabled && recorded->pose.arms.left.confidence == 0 &&
                  recorded->pose.arms.right.confidence == 0 && recorded->pose.bodyAngleZ == 0,
              "Disabling arms must take effect even with a cached face sample");
        settings.avatarSmoothing = .06f;
        ++settings.avatarCalibration;
        processor.setSettings(settings);
        controlled->face.hasHeadPosition = true;
        controlled->face.headPositionX = 0;
        controlled->face.headPositionY = 0;
        controlled->face.headDepth = 5;
        controlled->face.headRotation = HeadRotation{};
        controlled->face.capturedAt = std::chrono::steady_clock::now();
        frame->readyAt = controlled->face.capturedAt;
        processor.render(scene);
        for (float noise : {.8f, -.8f, .9f, -.9f}) {
            controlled->face.headRotation = HeadRotation{noise, noise, noise};
            controlled->face.headPositionX = noise * .004f;
            controlled->face.headPositionY = noise * .004f;
            controlled->face.headDepth = 5 + noise * .05f;
            controlled->face.capturedAt += std::chrono::milliseconds(33);
            processor.render(scene);
            check(recorded->pose.yaw == 0 && recorded->pose.pitch == 0 &&
                      recorded->pose.roll == 0 && recorded->pose.positionX == 0 &&
                      recorded->pose.positionY == 0 && recorded->pose.depthScale == 1,
                  "Stationary camera noise must not rotate, translate or resize the avatar");
        }
        controlled->face.headRotation = HeadRotation{15, 0, 0};
        controlled->face.headPositionX = .08f;
        controlled->face.headDepth = 4;
        controlled->face.capturedAt += std::chrono::milliseconds(33);
        processor.render(scene);
        check(recorded->pose.yaw > 1 && recorded->pose.positionX > .01f &&
                  recorded->pose.depthScale > 1.03f,
              "Deliberate motion must still reach the avatar with smoothing enabled");
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
