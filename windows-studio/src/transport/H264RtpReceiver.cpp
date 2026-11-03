#include "cortex/transport/H264RtpReceiver.h"
#include <algorithm>
#include <stdexcept>

namespace cortex {
namespace {
constexpr size_t MaxAccessUnit = 4 * 1024 * 1024;
uint16_t u16(std::span<const uint8_t> b, size_t i) {
    return uint16_t((b[i] << 8) | b[i + 1]);
}
uint32_t u32(std::span<const uint8_t> b, size_t i) {
    return (uint32_t(u16(b, i)) << 16) | u16(b, i + 2);
}
int distance(uint16_t a, uint16_t b) {
    const auto d = uint16_t(a - b);
    return d < 32768 ? d : int(d) - 65536;
}
void startCode(std::vector<uint8_t>& dst, std::span<const uint8_t> nal) {
    dst.insert(dst.end(), {0, 0, 0, 1});
    dst.insert(dst.end(), nal.begin(), nal.end());
}
}  // namespace
H264RtpReceiver::H264RtpReceiver(Sink sink, std::chrono::milliseconds reorder)
    : sink_(std::move(sink))
    , reorder_(reorder) {
    if (!sink_ || reorder.count() < 0 || reorder > std::chrono::milliseconds(50))
        throw std::invalid_argument("Invalid RTP receiver configuration");
}
std::optional<H264RtpReceiver::Packet> H264RtpReceiver::parse(std::span<const uint8_t> b,
                                                              Clock::time_point now) {
    if (b.size() < 12 || b.size() > 2048 || b[0] >> 6 != 2 || (b[1] & 127) != 96)
        return {};
    size_t offset = 12 + 4 * (b[0] & 15), end = b.size();
    int rotation = 0;
    if (offset > end)
        return {};
    if (b[0] & 16) {
        if (offset + 4 > end)
            return {};
        const size_t bytes = 4 + 4 * size_t(u16(b, offset + 2));
        if (bytes > end - offset)
            return {};
        if (u16(b, offset) == 0xbede) {
            size_t i = offset + 4;
            const size_t limit = offset + bytes;
            while (i < limit) {
                const auto element = b[i++];
                if (element == 0)
                    continue;
                const auto id = element >> 4;
                if (id == 15)
                    break;
                const size_t length = (element & 15) + 1;
                if (length > limit - i)
                    return {};
                if (id == 1) {
                    if (length != 1 || b[i] > 3)
                        return {};
                    rotation = b[i] * 90;
                }
                i += length;
            }
        }
        offset += bytes;
    }
    if (b[0] & 32) {
        const size_t padding = b.back();
        if (!padding || padding > end - offset)
            return {};
        end -= padding;
    }
    if (offset >= end)
        return {};
    return Packet{
        u16(b, 2), u32(b, 4), u32(b, 8), bool(b[1] & 128), {b.begin() + offset, b.begin() + end},
        now,       rotation};
}
void H264RtpReceiver::push(std::span<const uint8_t> bytes, Clock::time_point now) {
    auto p = parse(bytes, now);
    if (!p) {
        ++stats_.rejected;
        return;
    }
    if (ssrc_ && *ssrc_ != p->ssrc) {
        // A new sender run gets a new SSRC. Avoid bouncing between active senders.
        if (now - lastPacket_ < std::chrono::seconds(2)) {
            ++stats_.rejected;
            return;
        }
        pending_.clear();
        expected_.reset();
        timestamp_.reset();
        lastTimestamp_.reset();
        au_.clear();
        fu_.clear();
        sps_.clear();
        pps_.clear();
        extendedTimestamp_ = 0;
        lose();
    }
    ssrc_ = p->ssrc;
    lastPacket_ = now;
    ++stats_.packets;
    if (!expected_)
        expected_ = p->sequence;
    const int delta = distance(p->sequence, *expected_);
    if (delta < 0) {
        ++stats_.late;
        return;
    }
    if (delta >= 64) {
        stats_.gaps += uint64_t(delta);
        lose();
        pending_.clear();
        expected_ = p->sequence;
    }
    pending_.try_emplace(p->sequence, std::move(*p));
    drain(now);
}
void H264RtpReceiver::tick(Clock::time_point now) {
    drain(now);
}
void H264RtpReceiver::drain(Clock::time_point now) {
    while (!pending_.empty()) {
        auto found = pending_.find(*expected_);
        if (found == pending_.end()) {
            auto oldest = std::min_element(
                pending_.begin(), pending_.end(),
                [](const auto& a, const auto& b) { return a.second.arrival < b.second.arrival; });
            if (now - oldest->second.arrival < reorder_ && pending_.size() < 63)
                break;
            auto next = std::min_element(
                pending_.begin(), pending_.end(), [&](const auto& a, const auto& b) {
                    return distance(a.first, *expected_) < distance(b.first, *expected_);
                });
            stats_.gaps += uint64_t(distance(next->first, *expected_));
            lose();
            expected_ = next->first;
            continue;
        }
        Packet packet = std::move(found->second);
        pending_.erase(found);
        *expected_ = uint16_t(*expected_ + 1);
        consume(packet);
    }
}
void H264RtpReceiver::lose() {
    damaged_ = true;
    waitingIdr_ = true;
    reset_ = true;
    fu_.clear();
}
void H264RtpReceiver::append(std::span<const uint8_t> nal) {
    if (nal.empty() || (nal[0] & 128) || (nal[0] & 31) == 0 || (nal[0] & 31) > 23 ||
        au_.size() + nal.size() + 4 > MaxAccessUnit) {
        lose();
        return;
    }
    const int type = nal[0] & 31;
    ++stats_.nalTypes[type];
    if (type == 7)
        sps_ = {nal.begin(), nal.end()};
    if (type == 8)
        pps_ = {nal.begin(), nal.end()};
    if (type >= 1 && type <= 5)
        hasVcl_ = true;
    if (type == 5)
        idr_ = true;
    startCode(au_, nal);
}
void H264RtpReceiver::consume(const Packet& p) {
    if (!timestamp_ || *timestamp_ != p.timestamp) {
        if (!au_.empty() || !fu_.empty()) {
            lose();
            ++stats_.dropped;
        }
        timestamp_ = p.timestamp;
        au_.clear();
        fu_.clear();
        damaged_ = false;
        idr_ = false;
        hasVcl_ = false;
        rotation_ = p.rotation;
    }
    const auto& b = p.payload;
    const int type = b[0] & 31;
    if (b[0] & 128)
        lose();
    else if (type >= 1 && type <= 23) {
        if (!fu_.empty())
            lose();
        append(b);
    } else if (type == 24) {
        if (!fu_.empty())
            lose();
        size_t i = 1;
        while (i + 2 <= b.size()) {
            const size_t length = u16(b, i);
            i += 2;
            if (!length || length > b.size() - i) {
                lose();
                break;
            }
            append(std::span(b).subspan(i, length));
            i += length;
        }
        if (i != b.size())
            lose();
    } else if (type == 28 && b.size() > 2) {
        const bool start = b[1] & 128, end = b[1] & 64;
        const uint8_t header = uint8_t((b[0] & 0xe0) | (b[1] & 31));
        if ((b[1] & 32) || (start && end) || !(b[1] & 31) || (b[1] & 31) > 23)
            lose();
        else if (start) {
            if (!fu_.empty())
                lose();
            fu_ = {header};
            fu_.insert(fu_.end(), b.begin() + 2, b.end());
        } else if (fu_.empty() || fu_[0] != header || fu_.size() + b.size() - 2 > MaxAccessUnit)
            lose();
        else {
            fu_.insert(fu_.end(), b.begin() + 2, b.end());
            if (end) {
                append(fu_);
                fu_.clear();
            }
        }
    } else
        lose();
    if (p.marker) {
        ++stats_.markers;
        finish(p.arrival);
    }
}
void H264RtpReceiver::finish(Clock::time_point now) {
    if (!fu_.empty())
        lose();
    if (hasVcl_ && !damaged_ && (!waitingIdr_ || (idr_ && !sps_.empty() && !pps_.empty()))) {
        if (!lastTimestamp_)
            extendedTimestamp_ = *timestamp_;
        else {
            const uint32_t delta = *timestamp_ - *lastTimestamp_;
            // Sender has no B pictures. A backward/discontinuous clock requires a new IDR.
            if (delta > 90000 * 10U) {
                waitingIdr_ = true;
                reset_ = true;
                if (!idr_) {
                    au_.clear();
                    ++stats_.dropped;
                    return;
                }
            }
            extendedTimestamp_ += delta;
        }
        lastTimestamp_ = timestamp_;
        std::vector<uint8_t> output;
        if (idr_) {
            startCode(output, sps_);
            startCode(output, pps_);
        }
        output.insert(output.end(), au_.begin(), au_.end());
        waitingIdr_ = false;
        const bool reset = reset_;
        reset_ = false;
        ++stats_.accessUnits;
        sink_(EncodedAccessUnit{std::move(output), extendedTimestamp_, now, reset, {}, rotation_});
    } else if (hasVcl_ || damaged_)
        ++stats_.dropped;
    au_.clear();
    fu_.clear();
    damaged_ = false;
    idr_ = false;
    hasVcl_ = false;
}
}  // namespace cortex
