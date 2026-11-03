#include <winsock2.h>
#include <ws2tcpip.h>
#include "cortex/transport/UdpH264Source.h"
#include "cortex/transport/H264RtpReceiver.h"
#include "cortex/decoding/HardwareH264Decoder.h"
#include <array>
#include <stdexcept>
#include <sstream>
#include <fstream>
#include <filesystem>

namespace cortex {
namespace {
struct Winsock {
    Winsock() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data))
            throw std::runtime_error("WSAStartup failed");
    }
    ~Winsock() { WSACleanup(); }
};
struct Socket {
    SOCKET value{INVALID_SOCKET};
    ~Socket() {
        if (value != INVALID_SOCKET)
            closesocket(value);
    }
};
void networkCheck(bool success, const char* operation) {
    if (!success)
        throw std::runtime_error(std::string(operation) + " (Winsock " +
                                 std::to_string(WSAGetLastError()) + ")");
}
}  // namespace
UdpH264Source::UdpH264Source(std::shared_ptr<GpuDevice> gpu, UdpSourceConfig config)
    : gpu_(std::move(gpu))
    , config_(std::move(config)) {
    if (!gpu_ || !config_.port || config_.reorderWindow.count() < 0 ||
        config_.reorderWindow > std::chrono::milliseconds(50))
        throw std::invalid_argument("Invalid UDP source configuration");
}
UdpH264Source::~UdpH264Source() {
    stop();
}
void UdpH264Source::start() {
    std::lock_guard lifecycle(lifecycleMutex_);
    if (worker_.joinable()) {
        {
            std::lock_guard state(stateMutex_);
            if (status_.state != SourceStatus::State::Failed)
                return;
        }
        worker_.join();
    }
    stopping_ = false;
    {
        std::lock_guard state(stateMutex_);
        latest_.reset();
        status_ = {SourceStatus::State::Starting,
                   "Binding RTP/UDP and initializing hardware decoder"};
    }
    try {
        worker_ = std::thread(&UdpH264Source::run, this);
    } catch (...) {
        std::lock_guard state(stateMutex_);
        status_.state = SourceStatus::State::Failed;
        status_.detail = "Cannot start receiver thread";
        throw;
    }
}
void UdpH264Source::stop() noexcept {
    std::lock_guard lifecycle(lifecycleMutex_);
    stopping_ = true;
    if (worker_.joinable())
        worker_.join();  // Socket select is bounded to 5 ms; no cross-thread socket reuse race.
    std::lock_guard state(stateMutex_);
    latest_.reset();
    status_.state = SourceStatus::State::Stopped;
    status_.detail = "Stopped";
}
FramePtr UdpH264Source::latestFrame() const {
    std::lock_guard lock(stateMutex_);
    return latest_;
}
SourceStatus UdpH264Source::status() const {
    std::lock_guard lock(stateMutex_);
    return status_;
}
void UdpH264Source::setAutoPeer(const std::string& numericIpv4) {
    in_addr address{};
    if (!numericIpv4.empty() && inet_pton(AF_INET, numericIpv4.c_str(), &address) != 1)
        throw std::invalid_argument("Invalid automatic peer IPv4");
    autoPeer_ = address.s_addr;
}
void UdpH264Source::run() noexcept {
    try {
        Winsock winsock;
        Socket socket;
        socket.value = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        networkCheck(socket.value != INVALID_SOCKET, "Create socket");
        BOOL exclusive = TRUE;
        networkCheck(setsockopt(socket.value, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                                reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == 0,
                     "Exclusive bind");
        // Absorb a burst of fragmented IDR packets while the GPU submits work.
        // This is socket capacity, not an intentional frame/jitter queue.
        int receiveBytes = 1024 * 1024;
        networkCheck(setsockopt(socket.value, SOL_SOCKET, SO_RCVBUF,
                                reinterpret_cast<const char*>(&receiveBytes),
                                sizeof(receiveBytes)) == 0,
                     "Receive buffer");
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons(config_.port);
        if (inet_pton(AF_INET, config_.bindAddress.c_str(), &local.sin_addr) != 1)
            throw std::invalid_argument("Bind address must be numeric IPv4");
        networkCheck(bind(socket.value, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0,
                     "Bind UDP port");
        std::optional<in_addr> allowed;
        if (!config_.senderAddress.empty()) {
            in_addr address{};
            if (inet_pton(AF_INET, config_.senderAddress.c_str(), &address) != 1)
                throw std::invalid_argument("Sender address must be numeric IPv4");
            allowed = address;
        }
        HardwareH264Decoder decoder(gpu_, [&](FramePtr frame) {
            std::lock_guard lock(stateMutex_);
            latest_ = std::move(frame);
            ++status_.decodedFrames;
        });
        H264RtpReceiver* recovery = nullptr;
        std::string lastDecodeError;
        H264RtpReceiver rtp(
            [&](EncodedAccessUnit au) {
                try {
                    decoder.decode(au);
                } catch (const CorruptBitstream& error) {
                    lastDecodeError = error.what();
                    recovery->discardUntilIdr();
                    std::lock_guard lock(stateMutex_);
                    ++status_.decodeErrors;
                }
            },
            config_.reorderWindow);
        recovery = &rtp;
        {
            std::lock_guard lock(stateMutex_);
            status_.state = SourceStatus::State::Running;
            status_.detail = "Listening for H.264 RTP; hardware-only D3D11VA";
        }
        std::array<uint8_t, 2048> datagram{};
        auto nextDiagnostic = Clock::now();
        std::filesystem::path diagnosticPath;
        wchar_t module[32768]{};
        if (GetModuleFileNameW(nullptr, module, 32768)) {
            diagnosticPath =
                std::filesystem::path(module).parent_path() / L"logs" / L"receiver-status.log";
            std::error_code ignored;
            std::filesystem::create_directories(diagnosticPath.parent_path(), ignored);
        }
        while (!stopping_) {
            fd_set ready;
            FD_ZERO(&ready);
            FD_SET(socket.value, &ready);
            timeval timeout{0, 5000};
            const int selected = select(0, &ready, nullptr, nullptr, &timeout);
            networkCheck(selected != SOCKET_ERROR, "Wait for UDP");
            if (selected > 0) {
                sockaddr_in peer{};
                int size = sizeof(peer);
                const int count = recvfrom(socket.value, reinterpret_cast<char*>(datagram.data()),
                                           static_cast<int>(datagram.size()), 0,
                                           reinterpret_cast<sockaddr*>(&peer), &size);
                if (count == SOCKET_ERROR && WSAGetLastError() == WSAEMSGSIZE)
                    continue;
                networkCheck(count != SOCKET_ERROR, "Receive UDP");
                const auto automaticPeer = autoPeer_.load();
                if ((!allowed || peer.sin_addr.s_addr == allowed->s_addr) &&
                    (!automaticPeer || peer.sin_addr.s_addr == automaticPeer))
                    rtp.push(std::span(datagram.data(), static_cast<size_t>(count)), Clock::now());
            }
            rtp.tick(Clock::now());
            const auto stats = rtp.stats();
            if (Clock::now() >= nextDiagnostic) {
                std::ostringstream detail;
                detail << "packets=" << stats.packets << " accessUnits=" << stats.accessUnits
                       << " markers=" << stats.markers << " SPS=" << stats.nalTypes[7]
                       << " PPS=" << stats.nalTypes[8] << " IDR=" << stats.nalTypes[5]
                       << " dropped=" << stats.dropped << " gaps=" << stats.gaps;
                {
                    std::lock_guard stateLock(stateMutex_);
                    detail << " decoded=" << status_.decodedFrames
                           << " decodeErrors=" << status_.decodeErrors;
                }
                if (!lastDecodeError.empty())
                    detail << " | " << lastDecodeError;
                std::lock_guard stateLock(stateMutex_);
                status_.detail = detail.str();
                if (!diagnosticPath.empty()) {
                    std::ofstream log(diagnosticPath, std::ios::trunc);
                    if (log)
                        log << status_.detail << "\n";
                }
                nextDiagnostic = Clock::now() + std::chrono::seconds(1);
            }
            std::lock_guard lock(stateMutex_);
            status_.packets = stats.packets;
            status_.packetGaps = stats.gaps;
            status_.droppedAccessUnits = stats.dropped;
        }
    } catch (const std::exception& e) {
        std::lock_guard lock(stateMutex_);
        latest_.reset();
        status_.state = SourceStatus::State::Failed;
        status_.detail = e.what();
    } catch (...) {
        std::lock_guard lock(stateMutex_);
        latest_.reset();
        status_.state = SourceStatus::State::Failed;
        status_.detail = "Unexpected receiver failure";
    }
}
}  // namespace cortex
