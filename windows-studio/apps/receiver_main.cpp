#include "cortex/transport/UdpH264Source.h"
#include "cortex/sources/VideoSourceManager.h"
#include "cortex/discovery/StudioDiscovery.h"
#include "cortex/filters/VideoProcessor.h"
#include "cortex/filters/BackgroundVideo.h"
#include "cortex/output/UnityCameraOutput.h"
#include <windows.h>
#include <conio.h>
#include <atomic>
#include <iostream>
#include <thread>
#include <vector>
namespace {
std::atomic<bool> quitting{false};
BOOL WINAPI consoleHandler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
        quitting = true;
        return TRUE;
    }
    return FALSE;
}
unsigned number(const std::wstring& text, unsigned min, unsigned max) {
    size_t used{};
    const auto value = std::stoul(text, &used);
    if (used != text.size() || value < min || value > max)
        throw std::invalid_argument("Numeric argument out of range");
    return static_cast<unsigned>(value);
}
std::string ascii(const std::wstring& value) {
    if (value.find_first_not_of(L"0123456789.") != std::wstring::npos)
        throw std::invalid_argument("Sender must be numeric IPv4");
    std::string result;
    for (auto c : value)
        result.push_back(static_cast<char>(c));
    return result;
}
}  // namespace
int wmain(int argc, wchar_t** argv) {
    try {
        cortex::UdpSourceConfig config;
        unsigned duration = 0, fps = 30;
        uint16_t controlPort = 5005;
        int device = 0;
        bool outputEnabled = true, preserveAlpha = false;
        cortex::FilterSettings filters;
        std::filesystem::path image, video, model;
        int index = 1;
        std::vector<std::wstring> positional;
        while (index < argc && std::wstring(argv[index]).rfind(L"--", 0) != 0)
            positional.emplace_back(argv[index++]);
        if (positional.size() > 4)
            throw std::invalid_argument(
                "Expected video-port, sender-IP, duration, control-port before options");
        if (!positional.empty())
            config.port = static_cast<uint16_t>(number(positional[0], 1, 65535));
        if (positional.size() > 1)
            config.senderAddress = ascii(positional[1]);
        if (positional.size() > 2)
            duration = number(positional[2], 1, 86400);
        if (positional.size() > 3)
            controlPort = static_cast<uint16_t>(number(positional[3], 1, 65535));
        auto argument = [&]() -> std::wstring {
            if (index >= argc)
                throw std::invalid_argument("Missing option value");
            return argv[index++];
        };
        while (index < argc) {
            const std::wstring option = argv[index++];
            if (option == L"--filter") {
                const auto mode = argument();
                if (mode == L"none")
                    filters.removal = cortex::RemovalMode::None;
                else if (mode == L"chroma")
                    filters.removal = cortex::RemovalMode::ChromaKey;
                else if (mode == L"ai")
                    filters.removal = cortex::RemovalMode::AiMatting;
                else
                    throw std::invalid_argument("Filter must be none, chroma or ai");
            } else if (option == L"--background-image")
                image = argument();
            else if (option == L"--background-video")
                video = argument();
            else if (option == L"--model")
                model = argument();
            else if (option == L"--transparent")
                filters.transparent = true;
            else if (option == L"--virtual-alpha")
                preserveAlpha = true;
            else if (option == L"--no-output")
                outputEnabled = false;
            else if (option == L"--device")
                device = static_cast<int>(number(argument(), 0, 74));
            else if (option == L"--fps") {
                fps = number(argument(), 30, 60);
                if (fps != 30 && fps != 60)
                    throw std::invalid_argument("Output fps must be 30 or 60");
            } else if (option == L"--help") {
                std::cout << "cortex-receive [video-port [phone-IP [seconds [control-port]]]]\n"
                          << "  --filter none|chroma|ai --background-image FILE | "
                             "--background-video FILE\n"
                          << "  --model RVM_FP32.onnx --transparent --virtual-alpha --device 0 "
                             "--fps 30|60 --no-output\n"
                          << "Live keys: C chroma, A toggle AI, O off, T transparency, Q quit.\n";
                return 0;
            } else
                throw std::invalid_argument("Unknown option; use --help");
        }
        if (!image.empty() && !video.empty())
            throw std::invalid_argument("Choose an image or a video background");
        if (filters.removal == cortex::RemovalMode::AiMatting && model.empty())
            throw std::invalid_argument("AI mode requires --model pointing to RVM FP32 ONNX");
        auto gpu = cortex::GpuDevice::create();
        cortex::VideoProcessor processor(gpu);
        processor.setSettings(filters);
        if (!image.empty())
            processor.setBackgroundImage(image);
        if (!model.empty())
            processor.setMattingModel(model);
        std::unique_ptr<cortex::BackgroundVideo> background;
        if (!video.empty())
            background = std::make_unique<cortex::BackgroundVideo>(gpu, video);
        std::unique_ptr<cortex::UnityCameraOutput> output;
        if (outputEnabled)
            output = std::make_unique<cortex::UnityCameraOutput>(gpu, device);
        auto phone = std::make_shared<cortex::UdpH264Source>(gpu, config);
        cortex::VideoSourceManager manager(gpu);
        manager.add(cortex::SourceId::Phone, phone);
        manager.startAll();
        std::unique_ptr<cortex::StudioDiscovery> discovery;
        try {
            discovery = std::make_unique<cortex::StudioDiscovery>(
                config.port, controlPort, config.senderAddress,
                [phone] { return phone->status().state == cortex::SourceStatus::State::Running; },
                [phone](const std::string& peer) { phone->setAutoPeer(peer); });
            std::cout << discovery->status() << "\n";
        } catch (const std::exception& e) {
            std::cerr << "Automatic discovery unavailable; manual RTP still available: " << e.what()
                      << "\n";
        }
        SetConsoleCtrlHandler(consoleHandler, TRUE);
        std::cout << "Listening on UDP " << config.port
                  << ". GPU processing enabled. Keys: C chroma, A AI, O off, T alpha, Q quit.\n";
        if (output)
            std::cout << "UnityCapture device " << device
                      << " output enabled; select Unity Video Capture in the receiving app.\n";
        const auto started = std::chrono::steady_clock::now();
        auto next = started, nextLog = started;
        const auto interval = std::chrono::nanoseconds(1'000'000'000 / fps);
        uint64_t rendered = 0;
        while (!quitting && (!duration || std::chrono::steady_clock::now() - started <
                                              std::chrono::seconds(duration))) {
            while (_kbhit())
                switch (_getch()) {
                case 'c':
                case 'C':
                    filters.removal = cortex::RemovalMode::ChromaKey;
                    processor.setSettings(filters);
                    break;
                case 'o':
                case 'O':
                    filters.removal = cortex::RemovalMode::None;
                    processor.setSettings(filters);
                    break;
                case 'a':
                case 'A':
                    if (model.empty())
                        std::cerr << "Provide --model to enable GPU AI matting\n";
                    else {
                        filters.removal = filters.removal == cortex::RemovalMode::AiMatting
                                              ? cortex::RemovalMode::None
                                              : cortex::RemovalMode::AiMatting;
                        processor.setSettings(filters);
                    }
                    break;
                case 't':
                case 'T':
                    filters.transparent = !filters.transparent;
                    processor.setSettings(filters);
                    break;
                case 'q':
                case 'Q':
                    quitting = true;
                    break;
                }
            const auto status = phone->status();
            if (status.state == cortex::SourceStatus::State::Failed)
                throw std::runtime_error(status.detail);
            if (background && !background->error().empty())
                throw std::runtime_error(background->error());
            const auto scene = manager.snapshot();
            processor.render(scene, background ? background->latestFrame() : cortex::FramePtr{});
            ++rendered;
            if (output)
                output->submit(processor.exportFrame(preserveAlpha, true));
            const auto now = std::chrono::steady_clock::now();
            if (now >= nextLog) {
                const auto stats = output ? output->stats() : cortex::OutputStats{};
                std::cout << "frames=" << status.decodedFrames << " packets=" << status.packets
                          << " gaps=" << status.packetGaps
                          << " decodeErrors=" << status.decodeErrors
                          << " available=" << scene.layers.size() << " rendered=" << rendered
                          << " virtualSent=" << stats.sent << " virtualSkipped=" << stats.skipped
                          << " virtualInactive=" << stats.inactive << "\n";
                nextLog = now + std::chrono::seconds(1);
            }
            next += interval;
            if (next < now)
                next = now + interval;
            std::this_thread::sleep_until(next);
        }
        discovery.reset();
        manager.stopAll();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Receiver error: " << e.what() << "\n";
        return 1;
    }
}
