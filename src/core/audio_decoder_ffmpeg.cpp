// SPDX-License-Identifier: GPL-2.0-or-later

// audio_decoder_ffmpeg.cpp: AAC and MP3 through libavcodec, where it is
// already linked for video.

#include "core/audio_decoder.h"

#include "core/log.h"
#include "core/ts_demux.h"

#if BJ_AUDIO_AAC

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/samplefmt.h>
}

#include <cstring>

namespace {

struct Context {
    AVCodecContext*       codec  = nullptr;
    AVCodecParserContext* parser = nullptr;
    AVPacket*             packet = nullptr;
    AVFrame*              frame  = nullptr;
};

AVCodecID CodecFor(int streamType)
{
    switch (streamType) {
        case kStreamTypeAac:        return AV_CODEC_ID_AAC;
        case kStreamTypeMpeg1Audio:
        case kStreamTypeMpeg2Audio: return AV_CODEC_ID_MP3;
        default:                    return AV_CODEC_ID_AAC;
    }
}

// One decoded frame to interleaved signed 16-bit, which is what the audio
// device takes. AAC decodes planar float, MP3 planar 16-bit, so both need
// walking rather than a memcpy.
void ToS16(const AVFrame* f, std::vector<uint8_t>& out)
{
    const int channels = f->ch_layout.nb_channels;
    const int samples  = f->nb_samples;
    out.resize((size_t)samples * channels * 2);
    int16_t* dst = reinterpret_cast<int16_t*>(out.data());

    const AVSampleFormat fmt = (AVSampleFormat)f->format;
    const bool planar = av_sample_fmt_is_planar(fmt) != 0;

    for (int s = 0; s < samples; ++s) {
        for (int c = 0; c < channels; ++c) {
            const uint8_t* plane = planar ? f->data[c] : f->data[0];
            const int index = planar ? s : (s * channels + c);
            int32_t value = 0;
            switch (fmt) {
                case AV_SAMPLE_FMT_FLT:
                case AV_SAMPLE_FMT_FLTP: {
                    float v = reinterpret_cast<const float*>(plane)[index];
                    if (v >  1.0f) v =  1.0f;
                    if (v < -1.0f) v = -1.0f;
                    value = (int32_t)(v * 32767.0f);
                    break;
                }
                case AV_SAMPLE_FMT_S16:
                case AV_SAMPLE_FMT_S16P:
                    value = reinterpret_cast<const int16_t*>(plane)[index];
                    break;
                case AV_SAMPLE_FMT_S32:
                case AV_SAMPLE_FMT_S32P:
                    value = reinterpret_cast<const int32_t*>(plane)[index] >> 16;
                    break;
                default:
                    value = 0;
                    break;
            }
            *dst++ = (int16_t)value;
        }
    }
}

}  // namespace

Mp3Decoder::~Mp3Decoder() { close(); }

bool Mp3Decoder::open(int streamType, std::string& error)
{
    close();
    streamType_ = streamType;

    const AVCodecID id = CodecFor(streamType);
    const AVCodec* codec = avcodec_find_decoder(id);
    if (!codec) { error = "No decoder for this audio codec"; return false; }

    Context* ctx = new Context();
    ctx->codec = avcodec_alloc_context3(codec);
    ctx->packet = av_packet_alloc();
    ctx->frame  = av_frame_alloc();
    // Segments start mid-stream, so the parser finds frame boundaries rather
    // than trusting the PES payload to begin on one.
    ctx->parser = av_parser_init(id);

    if (!ctx->codec || !ctx->packet || !ctx->frame || !ctx->parser ||
        avcodec_open2(ctx->codec, codec, nullptr) < 0) {
        if (ctx->parser) av_parser_close(ctx->parser);
        if (ctx->frame)  av_frame_free(&ctx->frame);
        if (ctx->packet) av_packet_free(&ctx->packet);
        if (ctx->codec)  avcodec_free_context(&ctx->codec);
        delete ctx;
        error = "Could not start the audio decoder";
        return false;
    }

    handle_ = ctx;
    LOGF("[audio] libavcodec decoder for %s", codec->name);
    return true;
}

void Mp3Decoder::close()
{
    Context* ctx = static_cast<Context*>(handle_);
    if (!ctx) return;
    if (ctx->parser) av_parser_close(ctx->parser);
    if (ctx->frame)  av_frame_free(&ctx->frame);
    if (ctx->packet) av_packet_free(&ctx->packet);
    if (ctx->codec)  avcodec_free_context(&ctx->codec);
    delete ctx;
    handle_ = nullptr;
    sampleRate_ = 0;
    channels_   = 0;
}

bool Mp3Decoder::decode(const uint8_t* data, size_t length, const PcmFn& onPcm)
{
    Context* ctx = static_cast<Context*>(handle_);
    if (!ctx || !data || length == 0) return false;

    while (length > 0) {
        uint8_t* frameData = nullptr;
        int frameSize = 0;
        const int used = av_parser_parse2(ctx->parser, ctx->codec,
                                          &frameData, &frameSize,
                                          data, (int)length,
                                          AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (used < 0) return false;
        data   += used;
        length -= (size_t)used;
        if (frameSize == 0) continue;

        ctx->packet->data = frameData;
        ctx->packet->size = frameSize;
        if (avcodec_send_packet(ctx->codec, ctx->packet) < 0) continue;

        while (avcodec_receive_frame(ctx->codec, ctx->frame) == 0) {
            sampleRate_ = ctx->frame->sample_rate;
            channels_   = ctx->frame->ch_layout.nb_channels;
            ToS16(ctx->frame, scratch_);
            if (!scratch_.empty()) onPcm(scratch_.data(), scratch_.size());
        }
    }
    return true;
}

#endif  // BJ_AUDIO_AAC
