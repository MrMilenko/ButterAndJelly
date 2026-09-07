// SPDX-License-Identifier: GPL-2.0-or-later

// H.264 on the Xbox 360, in software, through libavcodec 54.
//
// The XDK's own decoder is XMV, which is WMV9 and VC-1, and no server can
// transcode into that, so the console decodes in software.
//
// Separate from the desktop decoder rather than sharing it behind version
// guards: this API predates avcodec_send_packet, av_frame_alloc and
// avcodec_free_context, so the two have almost nothing in common.

#include "core/video_decoder.h"

#include "core/log.h"

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/imgutils.h"
}

#include <cstring>

namespace {

class Ffmpeg1Decoder final : public VideoDecoder {
public:
    ~Ffmpeg1Decoder() override { close(); }

    bool open(int maxWidth, int maxHeight, std::string& error) override
    {
        (void)maxWidth;
        (void)maxHeight;

        avcodec_register_all();

        // Not const in this version.
        AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) { error = "libavcodec has no H.264 decoder"; return false; }

        context_ = avcodec_alloc_context3(codec);
        if (!context_) { error = "could not allocate a decoder context"; return false; }

        // One thread per hardware thread of cores 1 and 2, which is where
        // OXDK's pinning puts them. Core 0 is left to the main thread.
        //
        // Frame threading rather than slice: it scales, and the latency it
        // adds does not matter behind a frame queue.
        context_->thread_count = 4;
        context_->thread_type  = FF_THREAD_FRAME;

        // Access units arrive whole from the demuxer, so no parser is needed.
        if (avcodec_open2(context_, codec, nullptr) < 0) {
            error = "could not open the H.264 decoder";
            close();
            return false;
        }

        frame_ = avcodec_alloc_frame();
        if (!frame_) { error = "out of memory"; close(); return false; }

        LOGF("[video] %s, software H.264", name());
        return true;
    }

    void close() override
    {
        if (frame_) { avcodec_free_frame(&frame_); frame_ = nullptr; }
        if (context_) {
            avcodec_close(context_);
            av_free(context_);
            context_ = nullptr;
        }
    }

    bool decode(const uint8_t* annexB, size_t length, int64_t pts,
                const FrameFn& onFrame) override
    {
        if (!context_ || !annexB || length == 0) return false;

        AVPacket packet;
        av_init_packet(&packet);
        packet.data = const_cast<uint8_t*>(annexB);
        packet.size = (int)length;
        packet.pts  = pts;

        int got = 0;
        const int used = avcodec_decode_video2(context_, frame_, &got, &packet);
        if (used < 0) return false;
        if (got) emit(onFrame);
        return true;
    }

    void flush(const FrameFn& onFrame) override
    {
        if (!context_) return;

        // An empty packet asks for whatever is still held for reordering, and
        // keeps asking until nothing comes back.
        for (;;) {
            AVPacket packet;
            av_init_packet(&packet);
            packet.data = nullptr;
            packet.size = 0;

            int got = 0;
            if (avcodec_decode_video2(context_, frame_, &got, &packet) < 0) break;
            if (!got) break;
            emit(onFrame);
        }
    }

    const char* name() const override { return "libavcodec 54"; }

private:
    void emit(const FrameFn& onFrame)
    {
        if (!toNv12(*frame_, scratch_)) return;

        // Same contract as the console decoders: a borrowed view over a buffer
        // this decoder owns and will overwrite next time.
        Nv12View view;
        view.luma         = scratch_.luma.data();
        view.chroma       = scratch_.chroma.data();
        view.width        = scratch_.width;
        view.height       = scratch_.height;
        view.lumaStride   = scratch_.lumaStride;
        view.chromaStride = scratch_.chromaStride;
        view.pts          = scratch_.pts;
        view.chromaLayout = scratch_.chromaLayout;
        onFrame(view);
    }

    // libavcodec gives planar YUV420, and that is what goes downstream: SDL
    // uploads the three planes as three textures and the GPU does the colour
    // conversion, so keeping them apart means chroma is never touched between
    // the decoder and the screen. The Wii U interleaves to NV12 instead
    // because its hardware decoder produces that natively.
    static bool toNv12(const AVFrame& src, Nv12Frame& out)
    {
        if (src.width <= 0 || src.height <= 0) return false;
        if (src.format != AV_PIX_FMT_YUV420P && src.format != AV_PIX_FMT_YUVJ420P) {
            return false;
        }

        out.width  = src.width;
        out.height = src.height;
        out.lumaStride   = src.width;
        out.chromaStride = src.width / 2;
        out.chromaLayout = ChromaLayout::Planar;
        // pkt_pts, not pts: this version puts the packet's timestamp there.
        out.pts = src.pkt_pts;

        out.luma.resize((size_t)src.width * src.height);
        for (int y = 0; y < src.height; ++y) {
            std::memcpy(out.luma.data() + (size_t)y * out.lumaStride,
                        src.data[0] + (size_t)y * src.linesize[0],
                        (size_t)src.width);
        }

        // U then V, one after the other, each row tightly packed. Two runs of
        // memcpy rather than a per-sample interleave.
        const int chromaWidth  = src.width / 2;
        const int chromaHeight = src.height / 2;
        out.chroma.resize((size_t)out.chromaStride * chromaHeight * 2);
        uint8_t* uPlane = out.chroma.data();
        uint8_t* vPlane = uPlane + out.vPlaneOffset();
        for (int y = 0; y < chromaHeight; ++y) {
            std::memcpy(uPlane + (size_t)y * out.chromaStride,
                        src.data[1] + (size_t)y * src.linesize[1],
                        (size_t)chromaWidth);
            std::memcpy(vPlane + (size_t)y * out.chromaStride,
                        src.data[2] + (size_t)y * src.linesize[2],
                        (size_t)chromaWidth);
        }
        return true;
    }

    Nv12Frame       scratch_;   // reused, so repacking costs no allocation
    AVCodecContext* context_ = nullptr;
    AVFrame*        frame_   = nullptr;
};

}  // namespace

std::unique_ptr<VideoDecoder> VideoDecoder::Create()
{
    return std::unique_ptr<VideoDecoder>(new Ffmpeg1Decoder());
}
