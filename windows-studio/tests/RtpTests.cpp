#include "cortex/transport/H264RtpReceiver.h"
#include <iostream>
#include <stdexcept>
#include <string>

using namespace cortex;
namespace {
void require(bool value, const char* description) {
    if (!value)
        throw std::runtime_error(description);
}
std::vector<uint8_t> packet(uint16_t seq, uint32_t ts, std::vector<uint8_t> payload,
                            bool marker = true, uint32_t ssrc = 7) {
    std::vector<uint8_t> b{0x80,
                           uint8_t(96 | (marker ? 128 : 0)),
                           uint8_t(seq >> 8),
                           uint8_t(seq),
                           uint8_t(ts >> 24),
                           uint8_t(ts >> 16),
                           uint8_t(ts >> 8),
                           uint8_t(ts),
                           uint8_t(ssrc >> 24),
                           uint8_t(ssrc >> 16),
                           uint8_t(ssrc >> 8),
                           uint8_t(ssrc)};
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}
void seed(H264RtpReceiver& r, Clock::time_point now, uint16_t seq = 0, uint32_t ts = 1000) {
    r.push(packet(seq, ts, {0x67, 0x42, 0x80}, false), now);
    r.push(packet(uint16_t(seq + 1), ts, {0x68, 0x80}, false), now);
    r.push(packet(uint16_t(seq + 2), ts, {0x65, 0x80}), now);
}
void reorderAndFu() {
    std::vector<EncodedAccessUnit> out;
    H264RtpReceiver r([&](auto au) { out.push_back(std::move(au)); });
    const auto now = Clock::now();
    seed(r, now);
    r.push(packet(4, 2000, {0x7c, 0x45, 3, 4}), now);
    require(out.size() == 1, "future packet must wait for missing fragment");
    r.push(packet(3, 2000, {0x7c, 0x85, 1, 2}, false), now);
    require(out.size() == 2, "reordered FU-A must produce an access unit");
    const std::vector<uint8_t> suffix{0, 0, 0, 1, 0x65, 1, 2, 3, 4};
    require(std::equal(suffix.rbegin(), suffix.rend(), out.back().annexB.rbegin()),
            "FU-A must reconstruct original NAL");
    require(out.front().resetDecoder && !out.back().resetDecoder,
            "only discontinuities reset decoder");
}
void lossAndRecovery() {
    std::vector<EncodedAccessUnit> out;
    H264RtpReceiver r([&](auto au) { out.push_back(std::move(au)); });
    const auto now = Clock::now();
    seed(r, now);
    r.push(packet(4, 2000, {0x7c, 0x45, 3, 4}), now);  // Missing start fragment, sequence 3.
    r.tick(now + std::chrono::milliseconds(9));
    r.push(packet(5, 3000, {0x41, 0x80}), now + std::chrono::milliseconds(10));
    require(out.size() == 1, "loss must suppress incomplete and dependent pictures");
    r.push(packet(6, 4000, {0x65, 0x80}), now + std::chrono::milliseconds(11));
    require(out.size() == 2 && out.back().resetDecoder,
            "IDR with cached SPS/PPS must recover and reset decoder");
    require(r.stats().gaps == 1, "missing RTP packet counted");
}
void wrapAndDuplicates() {
    std::vector<EncodedAccessUnit> out;
    H264RtpReceiver r([&](auto au) { out.push_back(std::move(au)); });
    const auto now = Clock::now();
    seed(r, now, 65534, 0xfffffff0U);
    r.push(packet(1, 0x10, {0x41, 0x80}), now);
    r.push(packet(1, 0x10, {0x41, 0x80}), now);
    require(out.size() == 2, "sequence wrap and duplicate handling");
    require(out[1].timestamp90k - out[0].timestamp90k == 32, "RTP timestamps must unwrap");
    require(r.stats().late == 1, "duplicate must be dropped");
}
void malformedAndMissingMarker() {
    std::vector<EncodedAccessUnit> out;
    H264RtpReceiver r([&](auto au) { out.push_back(std::move(au)); });
    const auto now = Clock::now();
    r.push(std::vector<uint8_t>{0x80}, now);
    auto bad = packet(0, 0, {0x65});
    bad[0] |= 16;
    r.push(bad, now);
    seed(r, now);
    r.push(packet(3, 2000, {0x41, 0x80}, false), now);
    r.push(packet(4, 3000, {0x41, 0x80}), now);
    require(out.size() == 1, "missing marker must invalidate references until IDR");
    r.push(packet(5, 4000, {0x65, 0x80}), now);
    require(out.size() == 2 && out.back().resetDecoder, "recover from missing marker");
    require(r.stats().rejected == 2, "malformed headers rejected");
}
void aggregationAndSenderRestart() {
    std::vector<EncodedAccessUnit> out;
    H264RtpReceiver r([&](auto au) { out.push_back(std::move(au)); });
    const auto now = Clock::now();
    r.push(packet(0, 1000, {24, 0, 3, 0x67, 0x42, 0x80, 0, 2, 0x68, 0x80, 0, 2, 0x65, 0x80}), now);
    require(out.size() == 1, "STAP-A configuration and IDR");
    r.push(packet(0, 0, {0x65, 0x80}, true, 8), now + std::chrono::milliseconds(1));
    require(out.size() == 1, "active SSRC may not be hijacked");
    r.push(packet(0, 0, {0x65, 0x80}, true, 8), now + std::chrono::seconds(3));
    require(out.size() == 1, "new sender must provide new codec configuration");
    const std::vector<uint8_t> stap{24, 0, 3, 0x67, 0x42, 0x80, 0, 2, 0x68, 0x80, 0, 2, 0x65, 0x80};
    r.push(packet(1, 1000, stap, true, 8), now + std::chrono::seconds(3));
    require(out.size() == 2 && out.back().resetDecoder, "sender restart recovery");
}
void rotationHeaderExtension() {
    std::vector<EncodedAccessUnit> out;
    H264RtpReceiver receiver([&](auto au) { out.push_back(std::move(au)); });
    auto now = Clock::now();
    auto rotate = [](std::vector<uint8_t> data, uint8_t turns) {
        data[0] |= 16;
        data.insert(data.begin() + 12, {0xbe, 0xde, 0, 1, 0x10, turns, 0, 0});
        return data;
    };
    receiver.push(rotate(packet(0, 1000, {0x67, 0x42, 0x80}, false), 1), now);
    receiver.push(rotate(packet(1, 1000, {0x68, 0x80}, false), 1), now);
    receiver.push(rotate(packet(2, 1000, {0x65, 0x80}), 1), now);
    require(out.size() == 1 && out.front().rotationDegrees == 90,
            "RTP orientation must survive reassembly");
    receiver.push(rotate(packet(3, 4000, {0x41, 0x80}), 3), now);
    require(out.size() == 2 && out.back().rotationDegrees == 270,
            "Rotation can change without resetting the codec");
    auto invalid = rotate(packet(4, 7000, {0x41, 0x80}), 4);
    receiver.push(invalid, now);
    require(receiver.stats().rejected == 1, "Invalid orientation metadata must be rejected");
}
}  // namespace
int main() {
    try {
        reorderAndFu();
        lossAndRecovery();
        wrapAndDuplicates();
        malformedAndMissingMarker();
        aggregationAndSenderRestart();
        rotationHeaderExtension();
        std::cout << "6 RTP test groups passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
