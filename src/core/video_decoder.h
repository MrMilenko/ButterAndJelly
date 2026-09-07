// SPDX-License-Identifier: GPL-2.0-or-later

// video_decoder.h: H.264 decoding, behind one interface.
//
// The console has a hardware decoder and the host does not, so there are two
// implementations. Both take Annex-B access units and emit NV12, which means
// everything downstream, colour conversion, timing and presentation, is
// shared and can be tested on a laptop.

#pragma once

#include <functional>
#include <memory>
#include <string>

#include "core/video_frame.h"

class VideoDecoder {
public:
    // Receives a borrowed view. Copy it if you need it after returning.
    using FrameFn = std::function<void(const Nv12View&)>;

    virtual ~VideoDecoder() = default;

    // maxWidth/maxHeight size the decoder's working memory up front. The
    // console reserves it once and cannot grow it later.
    virtual bool open(int maxWidth, int maxHeight, std::string& error) = 0;
    virtual void close() = 0;

    // Feeds one access unit. Frames come out through the callback, and there
    // may be none for a given unit: H.264 reorders, so output lags input.
    virtual bool decode(const uint8_t* annexB, size_t length, int64_t pts,
                        const FrameFn& onFrame) = 0;

    // Drains frames still held for reordering. Call at end of stream.
    virtual void flush(const FrameFn& onFrame) = 0;

    virtual const char* name() const = 0;

    // Builds whichever implementation this platform has.
    static std::unique_ptr<VideoDecoder> Create();
};
