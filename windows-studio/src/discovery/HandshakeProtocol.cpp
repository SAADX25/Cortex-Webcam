#include "cortex/discovery/HandshakeProtocol.h"
#include <sstream>
#include <algorithm>
#include <charconv>
namespace cortex {
std::string HandshakeProtocol::leasePeer(std::chrono::steady_clock::time_point now) {
    if (now >= expires_)
        peer_.clear();
    return peer_;
}
std::string HandshakeProtocol::respond(std::string_view message, std::string_view peer, bool ready,
                                       std::chrono::steady_clock::time_point now,
                                       FrameRateRequest request) {
    report_.reset();
    if (message.size() > 256)
        return {};
    std::istringstream input{std::string(message)};
    std::string version, command, nonce, codec, extra;
    int width{}, height{}, fps{};
    if (!(input >> version >> command >> nonce) ||
        (version != "CORTEX/1" && version != "CORTEX/2") || nonce.size() != 32 ||
        !std::all_of(nonce.begin(), nonce.end(),
                     [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        return {};
    // LAN broadcast discovery never acquires a camera lease.
    if (command == "DISCOVER")
        return version == "CORTEX/1" && !(input >> extra) && ready
                   ? "CORTEX/1 OFFER " + nonce + "\n"
                   : std::string{};
    if (command != "HELLO" || !(input >> codec >> width >> height >> fps))
        return {};
    uint64_t acknowledged{};
    if (version == "CORTEX/2") {
        std::string value;
        if (!(input >> value) || value.empty() || value.size() > 19 ||
            !std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return {};
        auto parsed = std::from_chars(value.data(), value.data() + value.size(), acknowledged);
        if (parsed.ec != std::errc{} || acknowledged > INT64_MAX)
            return {};
    }
    std::string deviceName;
    if (input >> extra) {
        if (version != "CORTEX/2" || !extra.starts_with("device=") || extra.size() > 135 ||
            (extra.size() - 7) % 2)
            return {};
        const auto hex = extra.substr(7);
        for (size_t i = 0; i < hex.size(); i += 2) {
            unsigned value{};
            auto result = std::from_chars(hex.data() + i, hex.data() + i + 2, value, 16);
            if (result.ec != std::errc{} || result.ptr != hex.data() + i + 2 || value < 32 ||
                value > 126)
                return {};
            deviceName += static_cast<char>(value);
        }
        if (input >> extra)
            return {};
    }
    if (codec != "H264" || width != 1920 || height != 1080 || (fps != 30 && fps != 60))
        return {};
    leasePeer(now);
    if (!ready)
        return version + " WAIT " + nonce + "\n";
    if (!peer_.empty() && peer_ != peer)
        return version + " BUSY " + nonce + "\n";
    peer_ = peer;
    expires_ = now + std::chrono::seconds(8);
    const bool sync = version == "CORTEX/2";
    report_ = FrameRateReport{fps, sync, acknowledged, deviceName};
    const int selected =
        sync && request.id && request.id != acknowledged && (request.fps == 30 || request.fps == 60)
            ? request.fps
            : fps;
    return version + " READY " + nonce + " " + std::to_string(videoPort_) + " H264 1920 1080 " +
           std::to_string(selected) + (sync ? " " + std::to_string(request.id) : "") + "\n";
}
}  // namespace cortex
