#include "cortex/decoding/HardwareH264Decoder.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/log.h>
}
#include <cstring>
#include <stdexcept>
#include <string>
#include <map>
#include <mutex>

namespace cortex {
namespace {
thread_local std::string decoderMessages;
std::once_flag decoderLogHook;
void decoderLog(void* context,int level,const char* format,va_list args) {
    if(level>AV_LOG_DEBUG)return;
    char message[2048]{};int prefix=0;
    av_log_format_line2(context,level,format,args,message,sizeof(message),&prefix);
    if(decoderMessages.size()<24000)decoderMessages+=message;
}
void check(int code,const char* operation) {
    if (code>=0) return;
    char message[AV_ERROR_MAX_STRING_SIZE]{}; av_strerror(code,message,sizeof(message));
    if (code==AVERROR_INVALIDDATA) throw CorruptBitstream(std::string(operation)+": "+message+" | "+decoderMessages);
    throw std::runtime_error(std::string(operation)+": "+message);
}
AVPixelFormat hardwareFormat(AVCodecContext* context,const AVPixelFormat* formats) {
    // FFmpeg's DXVA H.264 table lists Constrained Baseline, Main and High,
    // excluding plain Baseline. Android AVC encoders may label a regular
    // 8-bit 4:2:0 no-B stream as Baseline without the constraint-set bit.
    // Permit that specific controlled sender profile through hardware selection.
    // This does not add support for FMO/ASO or select a software decoder.
    // Baseline syntax is 8-bit 4:2:0; sw_pix_fmt is not populated yet on
    // every get_format call, so it cannot be used as a negotiation guard.
    if(context->profile==AV_PROFILE_H264_BASELINE)
        context->hwaccel_flags|=AV_HWACCEL_FLAG_ALLOW_PROFILE_MISMATCH;
    else context->hwaccel_flags&=~AV_HWACCEL_FLAG_ALLOW_PROFILE_MISMATCH;
    av_log(context,AV_LOG_DEBUG,"Cortex H.264 profile=%d level=%d; Baseline hardware compatibility=%d\n",
        context->profile,context->level,(context->hwaccel_flags&AV_HWACCEL_FLAG_ALLOW_PROFILE_MISMATCH)!=0);
    for (auto p=formats;*p!=AV_PIX_FMT_NONE;++p) if (*p==AV_PIX_FMT_D3D11) return *p;
    return AV_PIX_FMT_NONE; // Never fall back to CPU decoding.
}
}
struct HardwareH264Decoder::Impl {
    std::shared_ptr<GpuDevice> gpu;
    std::function<void(FramePtr)> sink;
    AVBufferRef* hardware{};
    AVCodecContext* codec{};
    AVFrame* frame{};
    AVPacket* packet{};
    std::map<int64_t,Clock::time_point> arrivals;
    std::map<int64_t,int> rotations;
    ~Impl() { av_packet_free(&packet); av_frame_free(&frame); avcodec_free_context(&codec); av_buffer_unref(&hardware); }
};
HardwareH264Decoder::HardwareH264Decoder(std::shared_ptr<GpuDevice> gpu,std::function<void(FramePtr)> sink,bool lowDelay)
    :impl_(std::make_unique<Impl>()) {
    auto& s=*impl_; s.gpu=std::move(gpu); s.sink=std::move(sink);
    std::call_once(decoderLogHook,[]{av_log_set_level(AV_LOG_WARNING);av_log_set_callback(decoderLog);});
    if (!s.gpu || !s.sink) throw std::invalid_argument("Decoder requires GPU and frame sink");
    const auto* decoder=avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!decoder) throw std::runtime_error("FFmpeg H.264 decoder unavailable");
    bool compatible=false;
    for (int i=0;const auto* c=avcodec_get_hw_config(decoder,i);++i)
        if (c->device_type==AV_HWDEVICE_TYPE_D3D11VA && c->pix_fmt==AV_PIX_FMT_D3D11 &&
            (c->methods&AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) compatible=true;
    if (!compatible) throw std::runtime_error("FFmpeg lacks H.264 D3D11VA support");
    s.hardware=av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!s.hardware) throw std::bad_alloc();
    auto* device=reinterpret_cast<AVD3D11VADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(s.hardware->data)->hwctx);
    device->device=s.gpu->device.Get(); device->device->AddRef();
    device->lock_ctx=&s.gpu->mutex;
    device->lock=[](void* mutex){static_cast<std::recursive_mutex*>(mutex)->lock();};
    device->unlock=[](void* mutex){static_cast<std::recursive_mutex*>(mutex)->unlock();};
    check(av_hwdevice_ctx_init(s.hardware),"Initialize shared D3D11VA device");
    s.codec=avcodec_alloc_context3(decoder);
    if (!s.codec) throw std::bad_alloc();
    s.codec->hw_device_ctx=av_buffer_ref(s.hardware);
    if (!s.codec->hw_device_ctx) throw std::bad_alloc();
    s.codec->get_format=hardwareFormat;
    s.codec->pkt_timebase={1,90000};
    s.codec->thread_count=1;
    if(lowDelay)s.codec->flags|=AV_CODEC_FLAG_LOW_DELAY;
    s.codec->extra_hw_frames=8; // Leave room for latest-frame references and PiP snapshots.
    check(avcodec_open2(s.codec,decoder,nullptr),"Open hardware H.264 decoder");
    s.frame=av_frame_alloc(); s.packet=av_packet_alloc();
    if (!s.frame||!s.packet) throw std::bad_alloc();
}
HardwareH264Decoder::~HardwareH264Decoder()=default;
void HardwareH264Decoder::decode(const EncodedAccessUnit& au) {
    auto& s=*impl_;
    decoderMessages.clear();
    if (au.resetDecoder) { avcodec_flush_buffers(s.codec); s.arrivals.clear(); s.rotations.clear(); }
    if(!au.annexB.empty())s.arrivals[au.timestamp90k]=au.receivedAt;
    if(!au.annexB.empty())s.rotations[au.timestamp90k]=au.rotationDegrees;
    while (s.arrivals.size()>64) s.arrivals.erase(s.arrivals.begin());
    while (s.rotations.size()>64) s.rotations.erase(s.rotations.begin());
    av_packet_unref(s.packet);
    if(!au.annexB.empty()){
        check(av_new_packet(s.packet,static_cast<int>(au.annexB.size())),"Allocate compressed packet");
        std::memcpy(s.packet->data,au.annexB.data(),au.annexB.size());
        s.packet->pts=au.timestamp90k;s.packet->dts=au.decodeTimestamp90k.value_or(au.timestamp90k);
    }
    auto receive=[&] {
        for (;;) {
            const int result=avcodec_receive_frame(s.codec,s.frame);
            if (result==AVERROR(EAGAIN)||result==AVERROR_EOF) break;
            check(result,"Receive hardware frame");
            if ((s.frame->flags&AV_FRAME_FLAG_CORRUPT)||s.frame->decode_error_flags) {
                av_frame_unref(s.frame);
                throw CorruptBitstream("Discarding a damaged decoded picture");
            }
            if (s.frame->format!=AV_PIX_FMT_D3D11) throw std::runtime_error("CPU frame rejected");
            auto* texture=reinterpret_cast<ID3D11Texture2D*>(s.frame->data[0]);
            if (!texture) throw std::runtime_error("Decoder returned no GPU texture");
            D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
            if (desc.Format!=DXGI_FORMAT_NV12) throw std::runtime_error("Only 8-bit NV12 H.264 is supported in Module 1");
            AVFrame* retained=av_frame_clone(s.frame);
            if (!retained) throw std::bad_alloc();
            auto owner=std::shared_ptr<void>(retained,[gpu=s.gpu](void* p){auto* f=static_cast<AVFrame*>(p);av_frame_free(&f);});
            auto output=std::make_shared<VideoFrame>();
            output->backingOwner=std::move(owner);
            output->texture=texture;
            output->arraySlice=static_cast<UINT>(reinterpret_cast<intptr_t>(s.frame->data[1]));
            output->width=s.frame->width; output->height=s.frame->height;
            output->sourceTimestamp90k=s.frame->best_effort_timestamp;
            if(const auto rotation=s.rotations.find(output->sourceTimestamp90k);rotation!=s.rotations.end()){
                output->rotationDegrees=rotation->second;s.rotations.erase(rotation);
            }
            const auto arrival=s.arrivals.find(output->sourceTimestamp90k);
            output->receivedAt=arrival!=s.arrivals.end()?arrival->second:au.receivedAt;
            if (arrival!=s.arrivals.end()) s.arrivals.erase(arrival);
            output->readyAt=Clock::now(); output->gpu=s.gpu;
            output->range=s.frame->color_range==AVCOL_RANGE_JPEG?VideoFrame::Range::Full:
                s.frame->color_range==AVCOL_RANGE_MPEG?VideoFrame::Range::Limited:VideoFrame::Range::Unspecified;
            switch (s.frame->colorspace) {
            case AVCOL_SPC_BT709:output->matrix=VideoFrame::Matrix::Bt709;break;
            case AVCOL_SPC_SMPTE170M:case AVCOL_SPC_BT470BG:output->matrix=VideoFrame::Matrix::Bt601;break;
            case AVCOL_SPC_BT2020_NCL:case AVCOL_SPC_BT2020_CL:output->matrix=VideoFrame::Matrix::Bt2020;break;
            default:break;
            }
            av_frame_unref(s.frame);
            s.sink(std::move(output));
        }
    };
    auto* packet=au.annexB.empty()?nullptr:s.packet;
    int result=avcodec_send_packet(s.codec,packet);
    if (result==AVERROR(EAGAIN)) { receive(); result=avcodec_send_packet(s.codec,packet); }
    check(result,"Submit H.264 access unit");
    receive();
}
void HardwareH264Decoder::finish(){decode(EncodedAccessUnit{{},0,Clock::now(),false,{}});}
}
