// SPDX-License-Identifier: GPL-2.0-or-later

// video_decoder_ffmpeg.cpp: host H.264 decoding through libavcodec.
//
// Exists so the pipeline can be exercised on a laptop. libavcodec hands back
// planar YUV420, which is repacked into NV12 so that host and console
// produce identical input for everything downstream.

#include "core/video_decoder.h"

#include "core/log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

#include <cstring>

namespace {

class FfmpegDecoder final : public VideoDecoder {
public:
    ~FfmpegDecoder() override { close(); }

    bool open(int maxWidth, int maxHeight, std::string& error) override
    {
        (void)maxWidth;
        (void)maxHeight;

        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) { error = "libavcodec has no H.264 decoder"; return false; }

        context_ = avcodec_alloc_context3(codec);
        if (!context_) { error = "could not allocate a decoder context"; return false; }

        // Access units arrive whole from the demuxer, so no parser is needed.
        if (avcodec_open2(context_, codec, nullptr) < 0) {
            error = "could not open the H.264 decoder";
            close();
            return false;
        }

        packet_ = av_packet_alloc();
        frame_  = av_frame_alloc();
        if (!packet_ || !frame_) { error = "out of memory"; close(); return false; }
        return true;
    }

    void close() override
    {
        if (packet_) { av_packet_free(&packet_); packet_ = nullptr; }
        if (frame_)  { av_frame_free(&frame_);   frame_  = nullptr; }
        if (context_) { avcodec_free_context(&context_); context_ = nullptr; }
    }

    bool decode(const uint8_t* annexB, size_t length, int64_t pts,
                const FrameFn& onFrame) override
    {
        if (!context_ || !annexB || length == 0) return false;

        packet_->data = const_cast<uint8_t*>(annexB);
        packet_->size = (int)length;
        packet_->pts  = pts;

        const int sent = avcodec_send_packet(context_, packet_);
        if (sent < 0 && sent != AVERROR(EAGAIN)) return false;

        drain(onFrame);
        return true;
    }

    void flush(const FrameFn& onFrame) override
    {
        if (!context_) return;
        avcodec_send_packet(context_, nullptr);   // signals end of stream
        drain(onFrame);
    }

    const char* name() const override { return "libavcodec"; }

private:
    void drain(const FrameFn& onFrame)
    {
        while (avcodec_receive_frame(context_, frame_) == 0) {
            if (!toNv12(*frame_, scratch_)) continue;

            // Same contract as the console: a borrowed view over a buffer
            // this decoder owns and will overwrite next time.
            Nv12View view;
            view.luma         = scratch_.luma.data();
            view.chroma       = scratch_.chroma.data();
            view.width        = scratch_.width;
            view.height       = scratch_.height;
            view.lumaStride   = scratch_.lumaStride;
            view.chromaStride = scratch_.chromaStride;
            view.pts          = scratch_.pts;
            onFrame(view);
        }
    }

    // libavcodec gives planar YUV420; the console gives NV12. Repack so the
    // rest of the pipeline only ever deals with one layout.
    static bool toNv12(const AVFrame& src, Nv12Frame& out)
    {
        if (src.width <= 0 || src.height <= 0) return false;
        if (src.format != AV_PIX_FMT_YUV420P && src.format != AV_PIX_FMT_YUVJ420P) {
            return false;
        }

        out.width  = src.width;
        out.height = src.height;
        out.lumaStride   = src.width;
        out.chromaStride = src.width;
        out.pts = src.pts;

        out.luma.resize((size_t)src.width * src.height);
        for (int y = 0; y < src.height; ++y) {
            std::memcpy(out.luma.data() + (size_t)y * out.lumaStride,
                        src.data[0] + (size_t)y * src.linesize[0],
                        (size_t)src.width);
        }

        const int chromaWidth  = src.width / 2;
        const int chromaHeight = src.height / 2;
        out.chroma.resize((size_t)out.chromaStride * chromaHeight);
        for (int y = 0; y < chromaHeight; ++y) {
            const uint8_t* u = src.data[1] + (size_t)y * src.linesize[1];
            const uint8_t* v = src.data[2] + (size_t)y * src.linesize[2];
            uint8_t* dst = out.chroma.data() + (size_t)y * out.chromaStride;
            for (int x = 0; x < chromaWidth; ++x) {
                dst[x * 2]     = u[x];
                dst[x * 2 + 1] = v[x];
            }
        }
        return true;
    }

    Nv12Frame       scratch_;   // reused, so repacking costs no allocation
    AVCodecContext* context_ = nullptr;
    AVPacket*       packet_  = nullptr;
    AVFrame*        frame_   = nullptr;
};

}  // namespace

std::unique_ptr<VideoDecoder> VideoDecoder::Create()
{
    return std::unique_ptr<VideoDecoder>(new FfmpegDecoder());
}
