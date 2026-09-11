// SPDX-License-Identifier: GPL-2.0-or-later

// video_decoder_h264dec.cpp: H.264 through the Wii U's hardware decoder.
//
// The decode block reads main memory directly, so buffers must be aligned and
// flushed before it runs and invalidated before the CPU reads them back.
// Getting that wrong gives torn frames rather than an error.

#include "core/video_decoder.h"

#include "core/log.h"

#include <coreinit/cache.h>
#include <h264/decode.h>

#include <cstdlib>
#include <cstring>
#include <malloc.h>

namespace {

// The decoder requires 256 byte alignment for both its working memory and
// the buffers passed to it.
constexpr size_t kAlignment = 0x100;

size_t AlignUp(size_t value, size_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

class H264HardwareDecoder final : public VideoDecoder {
public:
    ~H264HardwareDecoder() override { close(); }

    bool open(int maxWidth, int maxHeight, std::string& error) override
    {
        // High profile at level 4.1 covers everything Jellyfin will be asked
        // to produce, and the probe on real hardware accepted it up to 1080p.
        constexpr int32_t kProfile = 100;   // High
        constexpr int32_t kLevel   = 41;

        uint32_t required = 0;
        H264Error err = H264DECMemoryRequirement(kProfile, kLevel,
                                                 maxWidth, maxHeight, &required);
        if (err != H264_ERROR_OK || required == 0) {
            error = "the decoder rejected this stream size";
            return false;
        }

        memorySize_ = required;
        memory_ = memalign(kAlignment, memorySize_);
        if (!memory_) { error = "not enough memory for the decoder"; return false; }

        err = H264DECInitParam((int32_t)memorySize_, memory_);
        if (err != H264_ERROR_OK) { error = "H264DECInitParam failed"; close(); return false; }

        // Deliver each frame as soon as it is ready rather than letting the
        // decoder pool them, so playback latency stays predictable.
        H264DECSetParam_OUTPUT_PER_FRAME(memory_, 1);
        H264DECSetParam_FPTR_OUTPUT(memory_, &H264HardwareDecoder::OnFrameOutput);

        void* self = this;
        H264DECSetParam_USER_MEMORY(memory_, &self);

        err = H264DECCheckMemSegmentation(memory_, memorySize_);
        if (err != H264_ERROR_OK) {
            error = "the decoder's memory failed its own segmentation check";
            close();
            return false;
        }

        if (H264DECOpen(memory_) != H264_ERROR_OK) {
            error = "H264DECOpen failed"; close(); return false;
        }
        if (H264DECBegin(memory_) != H264_ERROR_OK) {
            error = "H264DECBegin failed"; close(); return false;
        }

        // NV12 is one byte per pixel of luma plus half that of chroma. The
        // decoder writes with its own stride, so leave generous headroom.
        // The frame buffer wants 1024 byte alignment, unlike the decoder's
        // working memory which wants 256.
        frameBufferSize_ = AlignUp((size_t)maxWidth * maxHeight * 3 / 2 + 4096, 1024);
        frameBuffer_ = memalign(1024, frameBufferSize_);
        if (!frameBuffer_) { error = "not enough memory for a frame buffer"; close(); return false; }

        maxWidth_  = maxWidth;
        maxHeight_ = maxHeight;
        opened_    = true;

        LOGF("[video] hardware decoder ready: %dx%d, %u KB working, %lu KB frame",
             maxWidth, maxHeight, required / 1024,
             (unsigned long)(frameBufferSize_ / 1024));
        return true;
    }

    void close() override
    {
        if (opened_) {
            H264DECEnd(memory_);
            H264DECClose(memory_);
            opened_ = false;
        }
        if (frameBuffer_) { free(frameBuffer_); frameBuffer_ = nullptr; }
        if (memory_)      { free(memory_);      memory_ = nullptr; }
    }

    bool decode(const uint8_t* annexB, size_t length, int64_t pts,
                const FrameFn& onFrame) override
    {
        if (!opened_ || !annexB || length == 0) return false;

        // The bitstream has to live in aligned memory the decode block can
        // read, so it is copied rather than passed straight through.
        if (bitstreamCapacity_ < length) {
            if (bitstream_) free(bitstream_);
            bitstreamCapacity_ = AlignUp(length * 2, kAlignment);
            bitstream_ = (uint8_t*)memalign(kAlignment, bitstreamCapacity_);
            if (!bitstream_) { bitstreamCapacity_ = 0; return false; }
        }
        std::memcpy(bitstream_, annexB, length);
        DCFlushRange(bitstream_, (uint32_t)length);

        // The callback runs synchronously inside Execute, so stashing the
        // caller's handler here is safe for the duration of the call.
        currentCallback_ = &onFrame;

        // Timestamps are carried through the decoder as doubles and come back
        // on the output, which is how a reordered frame keeps its PTS.
        H264Error err = H264DECSetBitstream(memory_, bitstream_,
                                            (uint32_t)length, (double)pts);
        if (err != H264_ERROR_OK) { currentCallback_ = nullptr; return false; }

        err = H264DECExecute(memory_, frameBuffer_);
        currentCallback_ = nullptr;

        // Execute reports success as 0xE4 rather than zero. Checking it
        // against H264_ERROR_OK made every successful decode look like a
        // failure. Taken from GaryOderNichts' FFmpeg decoder, which is the
        // reference for this library.
        return err == (H264Error)0xE4 || err == H264_ERROR_OK;
    }

    void flush(const FrameFn& onFrame) override
    {
        if (!opened_) return;
        currentCallback_ = &onFrame;
        H264DECFlush(memory_);
        currentCallback_ = nullptr;
    }

    const char* name() const override { return "Wii U H264DEC"; }

private:
    // Called by the decoder, on its own thread, once per emitted frame.
    static void OnFrameOutput(H264DecodeOutput* output)
    {
        if (!output || output->frameCount <= 0 || !output->decodeResults) return;

        auto* self = static_cast<H264HardwareDecoder*>(output->userMemory);
        if (!self || !self->currentCallback_) return;

        for (int i = 0; i < output->frameCount; ++i) {
            const H264DecodeResult* result = output->decodeResults[i];
            if (!result || !result->framebuffer) continue;
            self->deliver(*result);
        }
    }

    void deliver(const H264DecodeResult& result)
    {
        const int codedWidth  = result.width;
        const int codedHeight = result.height;
        const int stride      = result.nextLine;
        if (codedWidth <= 0 || codedHeight <= 0 || stride < codedWidth) return;

        // The decode block wrote straight to memory behind the CPU's back.
        const size_t total = (size_t)stride * codedHeight * 3 / 2;
        DCInvalidateRange(result.framebuffer, (uint32_t)total);

        // H.264 codes in whole 16 pixel macroblocks, so a 694 line source is
        // decoded as 704 and carries a crop telling us where the picture
        // really ends. Showing the padding would put a band of garbage along
        // the bottom edge.
        int visibleWidth  = codedWidth;
        int visibleHeight = codedHeight;
        if (result.cropEnableFlag) {
            const int byOffset = codedHeight - result.cropTop - result.cropBottom;
            const int byExtent = result.cropBottom - result.cropTop;
            // The two readings of these fields disagree; take whichever
            // lands inside the coded frame.
            if (byOffset > 0 && byOffset <= codedHeight)      visibleHeight = byOffset;
            else if (byExtent > 0 && byExtent <= codedHeight) visibleHeight = byExtent;

            const int wOffset = codedWidth - result.cropLeft - result.cropRight;
            const int wExtent = result.cropRight - result.cropLeft;
            if (wOffset > 0 && wOffset <= codedWidth)      visibleWidth = wOffset;
            else if (wExtent > 0 && wExtent <= codedWidth) visibleWidth = wExtent;

        }

        if (!loggedCrop_) {
            loggedCrop_ = true;
            // Logged for every stream, not only cropped ones. A green band
            // across the top of one film and not another points at the
            // chroma plane starting somewhere other than where we think.
            LOGF("[video] coded %dx%d stride %d, crop %s t%d b%d l%d r%d -> %dx%d",
                 codedWidth, codedHeight, stride,
                 result.cropEnableFlag ? "on" : "off",
                 result.cropTop, result.cropBottom,
                 result.cropLeft, result.cropRight,
                 visibleWidth, visibleHeight);
            // Gary's decoder derives the pitch as the width rounded up to
            // 256 and puts chroma at visibleHeight * pitch. If that differs
            // from the stride the decoder reports, one of the two is wrong.
            const int garyPitch = (codedWidth + 255) & ~255;
            LOGF("[video] stride %d vs width rounded to 256 = %d%s",
                 stride, garyPitch,
                 stride == garyPitch ? " (agree)" : " (DISAGREE)");
        }

        // Chroma is subsampled, so an odd visible height would read half a
        // chroma row; round down to keep the two planes in step.
        visibleHeight &= ~1;

        const uint8_t* base = static_cast<const uint8_t*>(result.framebuffer);

        // Borrowed: the decoder reuses this buffer on the next Execute.
        //
        // Chroma starts after the coded luma rows, not the visible ones, or
        // any stream whose height is not a multiple of 16 gets a band of
        // luma across the top.
        Nv12View view;
        view.luma   = base + (size_t)stride * result.cropTop;
        view.chroma = base + (size_t)stride * codedHeight
                           + (size_t)stride * (result.cropTop / 2);
        view.width        = visibleWidth;
        view.height       = visibleHeight;
        view.lumaStride   = stride;
        view.chromaStride = stride;
        view.pts          = (int64_t)result.timestamp;

        (*currentCallback_)(view);
    }

    void*  memory_          = nullptr;
    size_t memorySize_      = 0;
    void*  frameBuffer_     = nullptr;
    size_t frameBufferSize_ = 0;
    uint8_t* bitstream_     = nullptr;
    size_t bitstreamCapacity_ = 0;

    int  maxWidth_  = 0;
    int  maxHeight_ = 0;
    bool opened_    = false;

    const FrameFn* currentCallback_ = nullptr;
    bool           loggedCrop_      = false;
};

}  // namespace

std::unique_ptr<VideoDecoder> VideoDecoder::Create()
{
    return std::unique_ptr<VideoDecoder>(new H264HardwareDecoder());
}
