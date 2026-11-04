#include "cortex/ui/StudioBridge.h"
#include "cortex/transport/UdpH264Source.h"
#include "cortex/discovery/StudioDiscovery.h"
#include "cortex/filters/VideoProcessor.h"
#include "cortex/filters/BackgroundVideo.h"
#include "cortex/output/UnityCameraOutput.h"
#include <dxgi1_3.h>
#include <d3dcompiler.h>
#include <thread>
#include <future>
#include <stop_token>
#include <condition_variable>
#include <atomic>
#include <cmath>
#include <algorithm>
#include <cstring>
namespace {
using namespace cortex;
using Clock = std::chrono::steady_clock;
static_assert(sizeof(CortexStudioState) == 3576, "Managed/native state layout changed");
void check(HRESULT result, const char* what) {
    if (FAILED(result))
        throw std::runtime_error(std::string(what) + " (" +
                                 std::to_string(static_cast<unsigned long>(result)) + ")");
}
std::wstring wide(const std::string& text) {
    if (text.empty())
        return {};
    const int count =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(count, L' ');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), count);
    return out;
}
template <size_t N> void text(wchar_t (&destination)[N], const std::wstring& value) {
    wcsncpy_s(destination, value.c_str(), _TRUNCATE);
}
class Preview {
    std::shared_ptr<GpuDevice> gpu;
    HWND window;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> raster;
    UINT width{}, height{};

public:
    Preview(std::shared_ptr<GpuDevice> device, HWND hwnd)
        : gpu(std::move(device))
        , window(hwnd) {
        const char* source =
            R"(Texture2D<float4> Frame:register(t0);SamplerState Clamp:register(s0);
struct V{float4 p:SV_POSITION;float2 uv:TEXCOORD0;};
V VS(uint id:SV_VertexID){V v;v.uv=float2((id<<1)&2,id&2);v.p=float4(v.uv.x*2-1,1-v.uv.y*2,0,1);return v;}
float4 PS(V v):SV_TARGET {float4 c=Frame.Sample(Clamp,v.uv);uint2 p=uint2(v.p.xy)/16;float bg=((p.x+p.y)&1)?0.105:0.075;return float4(c.rgb+bg*(1-c.a),1);})";
        std::lock_guard lock(gpu->mutex);
        auto compile = [&](const char* entry, const char* version) {
            ComPtr<ID3DBlob> code, error;
            check(D3DCompile(source, strlen(source), nullptr, nullptr, nullptr, entry, version,
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error),
                  "Compile preview shader");
            return code;
        };
        auto vs = compile("VS", "vs_5_0"), ps = compile("PS", "ps_5_0");
        check(gpu->device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr,
                                              &vertex),
              "Preview vertex shader");
        check(gpu->device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr,
                                             &pixel),
              "Preview pixel shader");
        D3D11_SAMPLER_DESC sample{};
        sample.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sample.AddressU = sample.AddressV = sample.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sample.MaxLOD = D3D11_FLOAT32_MAX;
        check(gpu->device->CreateSamplerState(&sample, &sampler), "Preview sampler");
        D3D11_RASTERIZER_DESC r{};
        r.FillMode = D3D11_FILL_SOLID;
        r.CullMode = D3D11_CULL_NONE;
        r.DepthClipEnable = TRUE;
        check(gpu->device->CreateRasterizerState(&r, &raster), "Preview rasterizer");
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        check(gpu->device.As(&dxgi), "DXGI preview device");
        check(dxgi->GetAdapter(&adapter), "Preview adapter");
        check(adapter->GetParent(IID_PPV_ARGS(&factory)), "Preview factory");
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = desc.Height = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.Scaling = DXGI_SCALING_STRETCH;
        check(factory->CreateSwapChainForHwnd(gpu->device.Get(), window, &desc, nullptr, nullptr,
                                              &swap),
              "Create GPU preview");
        factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        ComPtr<IDXGISwapChain2> lowLatency;
        if (SUCCEEDED(swap.As(&lowLatency)))
            lowLatency->SetMaximumFrameLatency(1);
    }
    void present(const GpuImage& image) {
        RECT rect{};
        if (!GetClientRect(window, &rect) || rect.right < 2 || rect.bottom < 2)
            return;
        std::lock_guard lock(gpu->mutex);
        auto* context = gpu->context.Get();
        if (width != static_cast<UINT>(rect.right) || height != static_cast<UINT>(rect.bottom)) {
            context->OMSetRenderTargets(0, nullptr, nullptr);
            target.Reset();
            width = rect.right;
            height = rect.bottom;
            check(swap->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0), "Resize preview");
            ComPtr<ID3D11Texture2D> back;
            check(swap->GetBuffer(0, IID_PPV_ARGS(&back)), "Preview buffer");
            check(gpu->device->CreateRenderTargetView(back.Get(), nullptr, &target),
                  "Preview target");
        }
        auto* output = target.Get();
        context->OMSetRenderTargets(1, &output, nullptr);
        context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        context->OMSetDepthStencilState(nullptr, 0);
        context->RSSetState(raster.Get());
        const float black[] = {0.025f, 0.035f, 0.05f, 1};
        context->ClearRenderTargetView(output, black);
        const float scale = std::min(float(width) / image.width, float(height) / image.height),
                    w = image.width * scale, h = image.height * scale;
        D3D11_VIEWPORT viewport{(width - w) / 2, (height - h) / 2, w, h, 0, 1};
        context->RSSetViewports(1, &viewport);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex.Get(), nullptr, 0);
        context->PSSetShader(pixel.Get(), nullptr, 0);
        auto* view = image.view.Get();
        auto* sample = sampler.Get();
        context->PSSetShaderResources(0, 1, &view);
        context->PSSetSamplers(0, 1, &sample);
        context->Draw(3, 0);
        view = nullptr;
        context->PSSetShaderResources(0, 1, &view);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        const auto result = swap->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
        if (result != DXGI_ERROR_WAS_STILL_DRAWING)
            check(result, "Present preview");
    }
};
struct Controls {
    bool avatarEnabled{}, avatarTestMotion{};
    int avatarMode = 0, avatarMotion = -1, avatarExpression = -1;
    uint64_t avatarRevision{};
    AvatarFraming avatarFraming;
    float avatarSmoothing{.06f}, avatarSensitivity{1.3f};
    bool avatarArms{false}, avatarMirror{false}, avatarFollowPosition{true};
    bool avatarFollowDistance{true};
    uint64_t avatarCalibration{};
    std::filesystem::path avatarModelPath;
    bool output = false;
    int fps = 30, mode = 0, backgroundKind = 0, videoPort = 5004, controlPort = 5005;
    float backgroundZoom = 1, backgroundX = .5f, backgroundY = .5f;
    bool backgroundFit = false;
    float similarity = .13f, softness = .10f, spill = .8f;
    bool transparent = false;
    std::filesystem::path background, model;
    uint64_t revision = 1, reconnect = 0;
    FrameRateRequest frameRate;
    bool frameRateCompleted{}, frameRateFallback{};
};
class Engine {
    HWND hwnd;
    std::mutex mutex;
    std::condition_variable wake;
    bool closing = false;
    Controls controls;
    CortexStudioState state;
    std::stop_source avatarLoadStop;
    std::thread worker;
    void failure(const std::string& message) {
        std::lock_guard lock(mutex);
        text(state.error, wide(message));
    }
    void run() noexcept {
        try {
            auto gpu = GpuDevice::create();
            VideoProcessor processor(gpu);
            Preview preview(gpu, hwnd);
            std::shared_ptr<UdpH264Source> phone;
            std::unique_ptr<VideoSourceManager> manager;
            std::unique_ptr<StudioDiscovery> discovery;
            std::unique_ptr<UnityCameraOutput> output;
            std::unique_ptr<BackgroundVideo> video;
            Controls applied;
            applied.revision = 0;
            uint64_t lastFrames = 0;
            auto metricTime = Clock::now();
            double measuredFps = 0;
            auto nextReceiverAttempt = Clock::time_point{},
                 nextDiscoveryAttempt = Clock::time_point{};
            std::wstring networkError;
            auto networkFailure = [&](const std::string& message) {
                networkError = wide(message);
                failure(message);
            };
            auto resetReceiver = [&] {
                discovery.reset();
                if (manager)
                    manager->stopAll();
                manager.reset();
                phone.reset();
                lastFrames = 0;
                measuredFps = 0;
                metricTime = Clock::now();
                nextDiscoveryAttempt = {};
                std::lock_guard lock(mutex);
                state.peer[0] = 0;
            };
            {
                std::lock_guard lock(mutex);
                state.ready = 1;
                text(state.status, L"Waiting for your phone");
            }

            bool avatarLoaded = false;
            bool avatarFailed = false;
            std::filesystem::path requestedAvatarPath;
            uint64_t requestedAvatarRevision{};
            struct AvatarLoadTask {
                std::future<std::unique_ptr<AvatarModel>> future;
                std::filesystem::path path;
                uint64_t revision{};
                std::jthread worker;

                AvatarLoadTask(std::shared_ptr<GpuDevice> gpu, const Controls& request)
                    : path(request.avatarModelPath), revision(request.avatarRevision) {
                    std::promise<std::unique_ptr<AvatarModel>> completion;
                    future = completion.get_future();
                    worker = std::jthread(
                        [gpu = std::move(gpu), p = path,
                         result = std::move(completion)](std::stop_token stop) mutable {
                            try {
                                result.set_value(loadAvatarModel(gpu, p, stop));
                            } catch (...) {
                                result.set_exception(std::current_exception());
                            }
                        });
                }
            };
            std::unique_ptr<AvatarLoadTask> pendingAvatar;
            for (;;) {
                Controls current;
                {
                    std::lock_guard lock(mutex);
                    if (closing)
                        break;
                    current = controls;
                }
                const auto tick = Clock::now();
                if (current.revision != applied.revision) {
                    if (current.videoPort != applied.videoPort ||
                        current.controlPort != applied.controlPort ||
                        current.reconnect != applied.reconnect) {
                        resetReceiver();
                        nextReceiverAttempt = {};
                    }
                    if (current.output && !output)
                        output = std::make_unique<UnityCameraOutput>(gpu);
                    if (!current.output)
                        output.reset();
                    if (current.avatarModelPath != applied.avatarModelPath ||
                        current.avatarRevision != applied.avatarRevision) {
                        if (pendingAvatar)
                            pendingAvatar->worker.request_stop();
                        avatarFailed = false;
                        if (current.avatarModelPath.empty()) {
                            processor.setAvatarModel(nullptr);
                            avatarLoaded = false;
                            requestedAvatarPath.clear();
                            requestedAvatarRevision = current.avatarRevision;
                            std::lock_guard lock(mutex);
                            state.error[0] = 0;
                        }
                    }
                    if (current.model != applied.model && !current.model.empty()) {
                        try {
                            processor.setMattingModel(current.model);
                        } catch (const std::exception& error) {
                            failure(error.what());
                        }
                    }
                    if (current.background != applied.background ||
                        current.backgroundKind != applied.backgroundKind) {
                        try {
                            video.reset();
                            processor.clearBackground();
                            if (current.backgroundKind == 1 && !current.background.empty())
                                processor.setBackgroundImage(current.background);
                            if (current.backgroundKind == 2 && !current.background.empty())
                                video = std::make_unique<BackgroundVideo>(gpu, current.background);
                        } catch (const std::exception& error) {
                            failure(error.what());
                        }
                    }
                    FilterSettings filter;
                    filter.avatarEnabled = current.avatarEnabled;
                    filter.avatarTestMotion = current.avatarTestMotion;
                    filter.avatarMode = current.avatarMode;
                    filter.avatarMotion = current.avatarMotion;
                    filter.avatarExpression = current.avatarExpression;
                    filter.avatarFraming = current.avatarFraming;
                    filter.avatarSmoothing = current.avatarSmoothing;
                    filter.avatarSensitivity = current.avatarSensitivity;
                    filter.avatarArms = current.avatarArms;
                    filter.avatarMirror = current.avatarMirror;
                    filter.avatarFollowPosition = current.avatarFollowPosition;
                    filter.avatarFollowDistance = current.avatarFollowDistance;
                    filter.avatarCalibration = current.avatarCalibration;
                    filter.backgroundZoom = current.backgroundZoom;
                    filter.backgroundX = current.backgroundX;
                    filter.backgroundY = current.backgroundY;
                    filter.backgroundFit = current.backgroundFit;
                    filter.removal = static_cast<RemovalMode>(current.mode);
                    filter.similarity = current.similarity;
                    filter.softness = current.softness;
                    filter.spill = current.spill;
                    filter.transparent = current.transparent;
                    processor.setSettings(filter);
                    applied = current;
                }
                if (pendingAvatar &&
                    pendingAvatar->future.wait_for(std::chrono::milliseconds(0)) ==
                        std::future_status::ready) {
                    const bool wanted = !pendingAvatar->worker.get_stop_token().stop_requested() &&
                                        pendingAvatar->path == current.avatarModelPath &&
                                        pendingAvatar->revision == current.avatarRevision;
                    try {
                        auto m = pendingAvatar->future.get();
                        if (wanted) {
                            processor.setAvatarModel(std::move(m));
                            avatarLoaded = true;
                            avatarFailed = false;
                            std::lock_guard lock(mutex);
                            state.error[0] = 0;
                        }
                    } catch (const std::exception& error) {
                        if (wanted) {
                            processor.setAvatarModel(nullptr);
                            avatarLoaded = false;
                            avatarFailed = true;
                            failure(error.what());
                        }
                    }
                    pendingAvatar.reset();
                }
                // Keep at most one loader alive. Cancelling/replacing its future must
                // never wait on the render thread; discard its result when it finishes.
                if (!pendingAvatar && !current.avatarModelPath.empty() &&
                    (requestedAvatarPath != current.avatarModelPath ||
                     requestedAvatarRevision != current.avatarRevision)) {
                    std::lock_guard lock(mutex);
                    if (!closing && controls.avatarRevision == current.avatarRevision &&
                        controls.avatarModelPath == current.avatarModelPath) {
                        pendingAvatar = std::make_unique<AvatarLoadTask>(gpu, current);
                        avatarLoadStop = pendingAvatar->worker.get_stop_source();
                        requestedAvatarPath = current.avatarModelPath;
                        requestedAvatarRevision = current.avatarRevision;
                        avatarFailed = false;
                        text(state.status, L"Live2D Avatar - Loading character...");
                        state.error[0] = 0;
                    }
                }
                // Reception is always armed while Studio is open, including when
                // minimized. Transient socket/port failures retry without a Start button.
                if (!phone && Clock::now() >= nextReceiverAttempt) {
                    try {
                        UdpSourceConfig config;
                        config.port = static_cast<uint16_t>(current.videoPort);
                        phone = std::make_shared<UdpH264Source>(gpu, config);
                        manager = std::make_unique<VideoSourceManager>(gpu);
                        manager->add(SourceId::Phone, phone);
                        manager->startAll();
                    } catch (const std::exception& error) {
                        networkFailure(error.what());
                        resetReceiver();
                        nextReceiverAttempt = Clock::now() + std::chrono::seconds(2);
                    }
                }
                SourceStatus source = phone ? phone->status() : SourceStatus{};
                SceneSnapshot scene = manager ? manager->snapshot() : SceneSnapshot{};
                if (source.state == SourceStatus::State::Failed) {
                    networkFailure(source.detail);
                    resetReceiver();
                    nextReceiverAttempt = Clock::now() + std::chrono::seconds(2);
                    source = {};
                    scene = {};
                }
                if (phone && source.state == SourceStatus::State::Running && !discovery &&
                    Clock::now() >= nextDiscoveryAttempt) {
                    try {
                        discovery = std::make_unique<StudioDiscovery>(
                            static_cast<uint16_t>(current.videoPort),
                            static_cast<uint16_t>(current.controlPort), "",
                            [p = phone] {
                                return p->status().state == SourceStatus::State::Running;
                            },
                            [this, p = phone](const std::string& ip) {
                                p->setAutoPeer(ip);
                                std::lock_guard lock(mutex);
                                text(state.peer, wide(ip));
                                if (ip.empty()) {
                                    state.deviceName[0] = 0;
                                    state.phoneFps = state.phoneSyncAvailable = 0;
                                    state.frameRateAckId = 0;
                                }
                            },
                            [p = phone] {
                                auto stats = p->status();
                                return std::pair{stats.decodedFrames, stats.packets};
                            },
                            [this] {
                                std::lock_guard lock(mutex);
                                return controls.frameRateCompleted ? FrameRateRequest{}
                                                                   : controls.frameRate;
                            },
                            [this](FrameRateReport report) {
                                std::lock_guard lock(mutex);
                                if (!controls.frameRateCompleted && controls.frameRate.id &&
                                    report.syncSupported &&
                                    report.acknowledged == controls.frameRate.id) {
                                    controls.frameRateCompleted = true;
                                    controls.frameRateFallback =
                                        report.fps != controls.frameRate.fps;
                                } else if (controls.frameRateCompleted &&
                                           state.phoneFps != report.fps)
                                    controls.frameRateFallback = false;
                                text(state.deviceName, wide(report.deviceName));
                                state.phoneFps = report.fps;
                                state.phoneSyncAvailable = report.syncSupported;
                                state.frameRateAckId = controls.frameRateCompleted
                                                           ? controls.frameRate.id
                                                           : report.acknowledged;
                                if (!controls.frameRate.id || controls.frameRateCompleted) {
                                    if (controls.fps != report.fps) {
                                        controls.fps = report.fps;
                                        ++controls.revision;
                                        wake.notify_all();
                                    }
                                }
                            });
                        std::lock_guard lock(mutex);
                        if (!networkError.empty() && networkError == state.error)
                            state.error[0] = 0;
                        networkError.clear();
                    } catch (const std::exception& error) {
                        networkFailure(std::string("Auto-connect is retrying: ") + error.what());
                        nextDiscoveryAttempt = Clock::now() + std::chrono::seconds(2);
                    }
                }
                if (discovery) {
                    const auto detail = discovery->status();
                    if (detail.starts_with("Discovery failed:")) {
                        networkFailure(detail);
                        discovery.reset();
                        nextDiscoveryAttempt = Clock::now() + std::chrono::seconds(2);
                    }
                    std::lock_guard lock(mutex);
                    text(state.discovery, wide(detail));
                } else {
                    std::lock_guard lock(mutex);
                    text(state.discovery, L"Auto-connect is starting or retrying�");
                }
                if (video && !video->error().empty()) {
                    failure(video->error());
                    video.reset();
                }
                GpuImage rendered;
                if (!scene.layers.empty()) {
                    const bool portrait = scene.layers.front().frame->rotationDegrees % 180 != 0;
                    processor.setOutputSize(portrait ? 1080 : 1920, portrait ? 1920 : 1080);
                } else if (video && video->latestFrame()) {
                    auto vf = video->latestFrame();
                    const bool portrait = vf->height > vf->width;
                    processor.setOutputSize(portrait ? 1080 : 1920, portrait ? 1920 : 1080);
                }
                try {
                    rendered = processor.render(scene, video ? video->latestFrame() : FramePtr{});
                } catch (const std::exception& error) {
                    if (current.avatarEnabled)
                        throw;
                    failure(error.what());
                    FilterSettings original;
                    original.removal = RemovalMode::None;
                    processor.setSettings(original);
                    {
                        std::lock_guard lock(mutex);
                        controls.mode = 0;
                        ++controls.revision;
                    }
                    rendered = processor.render(scene);
                }
                const bool hasMedia =
                    (current.backgroundKind == 1 && !current.background.empty()) ||
                    (current.backgroundKind == 2 && video && bool(video->latestFrame()));
                const bool hasVisual = current.avatarEnabled || !scene.layers.empty() || hasMedia;
                if (hasVisual)
                    preview.present(rendered);
                if (output)
                    output->submit(processor.exportFrame(false, true));
                const auto now = Clock::now();
                if (now - metricTime >= std::chrono::seconds(1)) {
                    measuredFps = double(source.decodedFrames - lastFrames) /
                                  std::chrono::duration<double>(now - metricTime).count();
                    lastFrames = source.decodedFrames;
                    metricTime = now;
                }
                {
                    std::lock_guard lock(mutex);
                    state.running = source.state == SourceStatus::State::Running;
                    state.hasFrame = hasVisual ? 1 : 0;
                    state.virtualEnabled = current.output;
                    state.mode = controls.mode;
                    state.outputFps = current.fps;
                    state.incomingFps = measuredFps;
                    state.frames = source.decodedFrames;
                    state.packets = source.packets;
                    state.gaps = source.packetGaps;
                    state.width = hasVisual ? rendered.width : 0;
                    state.height = hasVisual ? rendered.height : 0;
                    const auto stats = output ? output->stats() : OutputStats{};
                    state.virtualSent = stats.sent;
                    state.virtualSkipped = stats.skipped;
                    state.requestedPhoneFps = controls.frameRate.fps;
                    state.frameRateRequestId = controls.frameRate.id;
                    state.frameRatePending = controls.frameRate.id && !controls.frameRateCompleted;
                    state.phoneRateFallback = controls.frameRateFallback;
                    text(state.diagnostics, wide(source.detail));
                    const bool avatarLoading = !current.avatarModelPath.empty() &&
                                               !avatarFailed && pendingAvatar &&
                                               (pendingAvatar->path != current.avatarModelPath ||
                                                pendingAvatar->revision != current.avatarRevision ||
                                                !pendingAvatar->worker.get_stop_token().stop_requested());
                    const wchar_t* st =
                        avatarLoading
                            ? L"Live2D Avatar - Loading character..."
                            : current.avatarEnabled
                                   ? (current.avatarMode == 1
                                          ? (avatarLoaded
                                                 ? (current.avatarTestMotion ? L"Live2D Avatar - Test motion"
                                                                             : L"Live2D Avatar - Camera tracking")
                                                 : (avatarFailed
                                                        ? L"Live2D Avatar - Failed to load character"
                                                        : L"Live2D Avatar - Choose a character"))
                                          : (current.avatarTestMotion
                                                 ? L"GPU Test Avatar - Test motion"
                                                 : L"GPU Test Avatar - Camera tracking"))
                            : (!scene.layers.empty()
                                   ? (current.mode == 3
                                          ? L"Background media active (Phone camera hidden)"
                                          : L"Phone camera is live")
                                   : (hasMedia
                                          ? (current.backgroundKind == 2
                                                 ? L"Background video playing (Standalone)"
                                                 : L"Background image active (Standalone)")
                                          : (!state.running ? L"Reconnecting automatically..."
                                             : source.packets
                                                 ? L"Receiving video - waiting for a complete frame"
                                                 : L"Ready - add background media or open Cortex "
                                                   L"Sender")));
                    text(state.status,
                         current.avatarEnabled && !current.avatarTestMotion
                             ? std::wstring(st) + L" | " + wide(processor.avatarTrackingStatus())
                             : std::wstring(st));
                }
                std::unique_lock lock(mutex);
                wake.wait_until(lock, tick + std::chrono::nanoseconds(1'000'000'000 / current.fps),
                                [&] { return closing || controls.revision != applied.revision; });
            }
            if (pendingAvatar)
                pendingAvatar->worker.request_stop();
            pendingAvatar.reset();
            discovery.reset();
            if (manager)
                manager->stopAll();
        } catch (const std::exception& error) {
            std::lock_guard lock(mutex);
            state.ready = 0;
            state.running = 0;
            state.hasFrame = 0;
            text(state.status, L"Video engine unavailable");
            text(state.error, wide(error.what()));
        }
    }

public:
    Engine(HWND window, const wchar_t* model)
        : hwnd(window) {
        if (model)
            controls.model = model;
        state.running = 1;
        state.outputFps = 30;
        text(state.status, L"Starting your studio�");
        worker = std::thread([this] { run(); });
    }
    ~Engine() {
        {
            std::lock_guard lock(mutex);
            closing = true;
            avatarLoadStop.request_stop();
        }
        wake.notify_all();
        if (worker.joinable())
            worker.join();
    }
    int snapshot(CortexStudioState* destination, int bytes) {
        if (!destination || bytes != sizeof(CortexStudioState))
            return 0;
        std::lock_guard lock(mutex);
        *destination = state;
        return 1;
    }
    void set(int property, double value) {
        if (!std::isfinite(value))
            return;
        std::lock_guard lock(mutex);
        switch (property) {
        case 1:
        case 13:
            ++controls.reconnect;
            break;
        case 2:
            controls.mode = std::clamp(static_cast<int>(value), 0, 3);
            break;
        case 3:
            controls.output = value != 0;
            break;
        case 4:
            controls.fps = value >= 60 ? 60 : 30;
            break;
        case 5:
            controls.similarity = static_cast<float>(std::clamp(value, 0.0, 1.0));
            break;
        case 6:
            controls.softness = static_cast<float>(std::clamp(value, .001, 1.0));
            break;
        case 7:
            controls.spill = static_cast<float>(std::clamp(value, 0.0, 1.0));
            break;
        case 8:
            controls.transparent = value != 0;
            break;
        case 9:
            controls.videoPort = std::clamp(static_cast<int>(value), 1, 65535);
            break;
        case 10:
            controls.controlPort = std::clamp(static_cast<int>(value), 1, 65535);
            break;
        case 11:
            state.error[0] = 0;
            return;
        case 12: {
            const auto ports = static_cast<uint64_t>(value);
            const int video = static_cast<int>(ports & 65535),
                      control = static_cast<int>((ports >> 16) & 65535);
            if (!video || !control || video == control)
                return;
            controls.videoPort = video;
            controls.controlPort = control;
            break;
        }
        case 15:
            controls.backgroundZoom = std::clamp(static_cast<float>(value), 1.f, 3.f);
            break;
        case 16:
            controls.backgroundX = std::clamp(static_cast<float>(value), 0.f, 1.f);
            break;
        case 17:
            controls.backgroundY = std::clamp(static_cast<float>(value), 0.f, 1.f);
            break;
        case CortexAvatarEnabled:
            controls.avatarEnabled = value != 0;
            break;
        case CortexAvatarTestMotion:
            controls.avatarTestMotion = value != 0;
            break;
        case CortexAvatarMode:
            controls.avatarMode = std::clamp(static_cast<int>(value), 0, 1);
            break;
        case CortexAvatarMotion:
            controls.avatarMotion = std::clamp(static_cast<int>(value), -1, 1023);
            break;
        case CortexAvatarExpression:
            controls.avatarExpression = std::clamp(static_cast<int>(value), -1, 255);
            break;
        case CortexAvatarScale:
            controls.avatarFraming.scale = std::clamp(static_cast<float>(value), .25f, 6.f);
            break;
        case CortexAvatarPositionX:
            controls.avatarFraming.x = std::clamp(static_cast<float>(value), -1.f, 1.f);
            break;
        case CortexAvatarPositionY:
            controls.avatarFraming.y = std::clamp(static_cast<float>(value), -1.f, 1.f);
            break;
        case CortexAvatarSmoothing:
            controls.avatarSmoothing = std::clamp(static_cast<float>(value), 0.f, .3f);
            break;
        case CortexAvatarRecenter:
            ++controls.avatarCalibration;
            break;
        case CortexAvatarSensitivity:
            controls.avatarSensitivity = std::clamp(static_cast<float>(value), .5f, 2.5f);
            break;
        case CortexAvatarArms:
            controls.avatarArms = value != 0;
            break;
        case CortexAvatarMirror:
            controls.avatarMirror = value != 0;
            break;
        case CortexAvatarFollowPosition:
            controls.avatarFollowPosition = value != 0;
            break;
        case CortexAvatarFollowDistance:
            controls.avatarFollowDistance = value != 0;
            break;
        case 18:
            controls.backgroundFit = value != 0;
            break;
        case 14: {
            if (value != 30 && value != 60)
                return;
            controls.frameRate = {
                std::max(controls.frameRate.id + 1,
                         static_cast<uint64_t>(Clock::now().time_since_epoch().count())),
                static_cast<int>(value)};
            controls.frameRateCompleted = controls.frameRateFallback = false;
            break;
        }
        default:
            return;
        }
        ++controls.revision;
        state.error[0] = 0;
        wake.notify_all();
    }
    void path(int kind, const wchar_t* path) {
        std::lock_guard lock(mutex);
        if (kind == 3)
            controls.model = path ? path : L"";
        else if (kind == 4) {
            // Request directly from the controller thread, even if rendering is
            // currently waiting for the model's GPU resources.
            avatarLoadStop.request_stop();
            controls.avatarModelPath = path ? path : L"";
            ++controls.avatarRevision;
        } else {
            controls.background = path ? path : L"";
            controls.backgroundKind = controls.background.empty() ? 0 : kind;
        }
        ++controls.revision;
        state.error[0] = 0;
        wake.notify_all();
    }
};
}  // namespace
void* __cdecl cortex_studio_create(HWND preview, const wchar_t* model) {
    try {
        return new Engine(preview, model);
    } catch (...) {
        return nullptr;
    }
}
void __cdecl cortex_studio_destroy(void* handle) {
    delete static_cast<Engine*>(handle);
}
int __cdecl cortex_studio_get_state(void* handle, CortexStudioState* state, int bytes) {
    return handle ? static_cast<Engine*>(handle)->snapshot(state, bytes) : 0;
}
void __cdecl cortex_studio_set(void* handle, int property, double value) {
    if (handle)
        static_cast<Engine*>(handle)->set(property, value);
}
void __cdecl cortex_studio_path(void* handle, int kind, const wchar_t* path) {
    if (handle)
        static_cast<Engine*>(handle)->path(kind, path);
}
