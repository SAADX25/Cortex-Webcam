#include "cortex/processing/BackgroundVideo.h"
#include "cortex/decoding/HardwareH264Decoder.h"
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/bsf.h>
}
#include <thread>
#include <condition_variable>
#include <atomic>
namespace cortex {
namespace {
void check(int code, const char* operation) {
    if (code < 0) {
        char message[AV_ERROR_MAX_STRING_SIZE]{};
        av_strerror(code, message, sizeof(message));
        throw std::runtime_error(std::string(operation) + ": " + message);
    }
}
struct Demux {
    AVFormatContext* format{};
    AVBSFContext* filter{};
    AVPacket* packet = av_packet_alloc();
    AVPacket* filtered = av_packet_alloc();
    ~Demux() {
        av_packet_free(&packet);
        av_packet_free(&filtered);
        av_bsf_free(&filter);
        avformat_close_input(&format);
    }
};
}  // namespace
struct BackgroundVideo::Impl {
    std::shared_ptr<GpuDevice> gpu;
    std::thread worker;
    std::atomic<bool> stopping{false};
    mutable std::mutex mutex;
    std::condition_variable wake;
    FramePtr latest;
    std::string failure;
    ~Impl() {
        stopping = true;
        wake.notify_all();
        if (worker.joinable())
            worker.join();
    }
};
BackgroundVideo::BackgroundVideo(std::shared_ptr<GpuDevice> gpu, const std::filesystem::path& path)
    : impl_(std::make_unique<Impl>()) {
    if (!std::filesystem::is_regular_file(path))
        throw std::runtime_error("Background video must be a local file");
    impl_->gpu = std::move(gpu);
    impl_->worker = std::thread([s = impl_.get(), path] {
        try {
            Demux demux;
            if (!demux.packet || !demux.filtered)
                throw std::bad_alloc();
            demux.format = avformat_alloc_context();
            if (!demux.format)
                throw std::bad_alloc();
            demux.format->interrupt_callback = {
                [](void* context) { return static_cast<Impl*>(context)->stopping.load() ? 1 : 0; },
                s};
            const auto utf8 = path.u8string();
            check(avformat_open_input(&demux.format, reinterpret_cast<const char*>(utf8.c_str()),
                                      nullptr, nullptr),
                  "Open background video");
            check(avformat_find_stream_info(demux.format, nullptr),
                  "Read background stream metadata");
            const int stream =
                av_find_best_stream(demux.format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            check(stream, "Find background video stream");
            auto* video = demux.format->streams[stream];
            if (video->codecpar->codec_id != AV_CODEC_ID_H264)
                throw std::runtime_error("Background video currently requires 8-bit H.264 "
                                         "(MP4/MKV/MOV); convert other formats before use");
            const auto* filter = av_bsf_get_by_name("h264_mp4toannexb");
            if (!filter)
                throw std::runtime_error("FFmpeg H.264 bitstream filter unavailable");
            check(av_bsf_alloc(filter, &demux.filter), "Allocate background bitstream adapter");
            check(avcodec_parameters_copy(demux.filter->par_in, video->codecpar),
                  "Configure background bitstream adapter");
            demux.filter->time_base_in = video->time_base;
            check(av_bsf_init(demux.filter), "Initialize background bitstream adapter");
            std::optional<int64_t> first;
            auto epoch = std::chrono::steady_clock::now();
            HardwareH264Decoder decoder(
                s->gpu,
                [&](FramePtr frame) {
                    if (s->stopping)
                        return;
                    if (!first) {
                        first = frame->sourceTimestamp90k;
                        epoch = std::chrono::steady_clock::now();
                    }
                    const auto due =
                        epoch + std::chrono::microseconds((frame->sourceTimestamp90k - *first) *
                                                          1'000'000 / 90000);
                    std::unique_lock lock(s->mutex);
                    if (s->wake.wait_until(lock, due, [&] { return s->stopping.load(); }))
                        return;
                    s->latest = std::move(frame);
                },
                false);
            bool reset = true;
            int64_t synthetic = 0;
            auto consume = [&] {
                for (;;) {
                    const int result = av_bsf_receive_packet(demux.filter, demux.filtered);
                    if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
                        break;
                    check(result, "Read background access unit");
                    auto* p = demux.filtered;
                    const auto pts = p->pts == AV_NOPTS_VALUE
                                         ? synthetic
                                         : av_rescale_q(p->pts, demux.filter->time_base_out,
                                                        AVRational{1, 90000});
                    synthetic = pts + 3000;
                    const auto dts = p->dts == AV_NOPTS_VALUE
                                         ? pts
                                         : av_rescale_q(p->dts, demux.filter->time_base_out,
                                                        AVRational{1, 90000});
                    EncodedAccessUnit au{{p->data, p->data + p->size},
                                         pts,
                                         std::chrono::steady_clock::now(),
                                         reset,
                                         dts};
                    reset = false;
                    decoder.decode(au);
                    av_packet_unref(p);
                    if (s->stopping)
                        break;
                }
            };
            while (!s->stopping) {
                const int result = av_read_frame(demux.format, demux.packet);
                if (result == AVERROR_EOF) {
                    check(av_bsf_send_packet(demux.filter, nullptr),
                          "Drain background bitstream adapter");
                    consume();
                    decoder.finish();
                    if (!first)
                        throw std::runtime_error(
                            "Background file contains no decodable video frames");
                    check(av_seek_frame(demux.format, stream,
                                        video->start_time == AV_NOPTS_VALUE ? 0 : video->start_time,
                                        AVSEEK_FLAG_BACKWARD),
                          "Loop background video");
                    av_bsf_flush(demux.filter);
                    reset = true;
                    first.reset();
                    synthetic = 0;
                    continue;
                }
                if (s->stopping)
                    break;
                check(result, "Read background packet");
                if (demux.packet->stream_index == stream) {
                    check(av_bsf_send_packet(demux.filter, demux.packet),
                          "Submit background packet");
                    consume();
                }
                av_packet_unref(demux.packet);
            }
        } catch (const std::exception& error) {
            if (!s->stopping) {
                std::lock_guard lock(s->mutex);
                s->failure = error.what();
                s->latest.reset();
            }
        }
    });
}
BackgroundVideo::~BackgroundVideo() = default;
FramePtr BackgroundVideo::latestFrame() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->latest;
}
std::string BackgroundVideo::error() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->failure;
}
}  // namespace cortex
