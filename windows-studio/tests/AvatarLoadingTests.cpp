#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "cortex/ui/StudioBridge.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void pump() {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

template <class Predicate> bool until(Predicate predicate, std::chrono::seconds timeout) {
    const auto deadline = Clock::now() + timeout;
    do {
        pump();
        if (predicate())
            return true;
        std::this_thread::sleep_for(10ms);
    } while (Clock::now() < deadline);
    return false;
}

class ReservedPort {
    SOCKET socket_ = INVALID_SOCKET;

public:
    uint16_t port{};
    ReservedPort() {
        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        require(socket_ != INVALID_SOCKET, "Create test UDP socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
                "Reserve test UDP port");
        int bytes = sizeof(address);
        require(getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &bytes) == 0,
                "Read test UDP port");
        port = ntohs(address.sin_port);
    }
    void release() {
        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }
    }
    ~ReservedPort() { release(); }
};

bool loading(const CortexStudioState& state) {
    return std::wstring(state.status).find(L"Loading character") != std::wstring::npos;
}

class Fixture {
    HWND window_{};

public:
    void* engine{};
    Fixture() {
        ReservedPort video, control;
        // This hidden test window only hosts the native preview swap chain.
        window_ = CreateWindowExW(0, L"STATIC", L"Avatar loading integration fixture",
                                  WS_OVERLAPPEDWINDOW, 0, 0, 640, 360, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
        require(window_ != nullptr, "Create native preview fixture");
        engine = cortex_studio_create(window_, L"");
        require(engine != nullptr, "Create native studio engine");
        video.release();
        control.release();
        cortex_studio_set(engine, 12, double(video.port) + 65536.0 * control.port);
    }
    CortexStudioState snapshot() const {
        CortexStudioState state;
        require(cortex_studio_get_state(engine, &state, sizeof(state)) != 0,
                "Read native studio state");
        return state;
    }
    template <class Predicate>
    void expect(Predicate predicate, std::chrono::seconds timeout, const char* message) {
        if (!until([&] { return predicate(snapshot()); }, timeout)) {
            const auto state = snapshot();
            std::wcerr << L"Native status: " << state.status << L"\nNative error: "
                       << state.error << L"\n";
            throw std::runtime_error(message);
        }
    }
    void prepare() {
        expect([](const auto& state) { return state.ready && state.running; }, 12s,
               "Studio must start its receiver automatically");
        cortex_studio_set(engine, CortexAvatarTestMotion, 0);
        cortex_studio_set(engine, CortexAvatarMode, 1);
        cortex_studio_set(engine, CortexAvatarEnabled, 1);
    }
    void begin(const std::filesystem::path& model) {
        cortex_studio_path(engine, 4, model.c_str());
        expect([](const auto& state) { return loading(state); }, 5s,
               "A model request must enter the loading state");
    }
    void loaded() {
        expect(
            [](const auto& state) {
                const std::wstring status(state.status);
                return state.running && state.hasFrame && !state.error[0] && !loading(state) &&
                       status.find(L"Live2D Avatar") != std::wstring::npos &&
                       status.find(L"Camera tracking") != std::wstring::npos;
            },
            75s, "Live model loading must finish while the preview is rendering");
    }
    void clear() {
        cortex_studio_path(engine, 4, L"");
        expect([](const auto& state) { return state.running && !loading(state) && !state.error[0]; },
               15s, "Cancelling a model request must clear loading and keep reception active");
    }
    void close() noexcept {
        if (!engine)
            return;
        auto* handle = engine;
        engine = nullptr;
        std::atomic<bool> finished{false};
        std::thread closer([&] {
            cortex_studio_destroy(handle);
            finished.store(true);
        });
        if (!until([&] { return finished.load(); }, 15s)) {
            // A stuck native worker cannot safely outlive this fixture. Fail the
            // isolated test process instead of hanging its thread destructor.
            std::cerr << "FAIL: Studio shutdown exceeded 15 seconds during avatar loading\n"
                      << std::flush;
            std::_Exit(EXIT_FAILURE);
        }
        closer.join();
    }
    ~Fixture() {
        close();
        if (window_)
            DestroyWindow(window_);
    }
};
}  // namespace

int wmain(int argc, wchar_t** argv) {
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0)
        return 1;
    int result = 0;
    try {
        require(argc == 2 || argc == 3, "Expected model path and optional --shutdown");
        const std::filesystem::path model = argv[1];
        require(std::filesystem::is_regular_file(model), "The Ren model fixture must exist");
        Fixture studio;
        studio.prepare();
        studio.begin(model);
        if (argc == 3) {
            require(std::wstring(argv[2]) == L"--shutdown", "Unknown loading test option");
            // Run in a separate process so cancellation covers cold shader setup.
            studio.close();
            std::cout << "PASS: shutdown during cold avatar loading stays bounded\n";
        } else {
            studio.clear();
            studio.begin(model);
            studio.loaded();
            std::cout << "Loaded Ren in the live engine; reloading while rendering\n" << std::flush;
            studio.begin(model);
            studio.loaded();
            const auto missing = model.parent_path() / L"missing-avatar.model3.json";
            // Replace a live request before it completes. Its canceled result
            // must never overwrite the newer missing-model failure.
            studio.begin(model);
            cortex_studio_path(studio.engine, 4, missing.c_str());
            studio.expect(
                [](const auto& state) {
                    return state.error[0] && !loading(state) &&
                           std::wstring(state.status).find(L"Failed to load character") !=
                               std::wstring::npos;
                },
                15s, "The newest failed request must replace a canceled load and leave loading");
            studio.clear();
            studio.close();
            std::cout << "PASS: live avatar load, reload, cancellation and failure recovery\n";
        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        result = 1;
    }
    WSACleanup();
    return result;
}
