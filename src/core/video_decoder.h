// SPDX-License-Identifier: GPL-2.0-or-later

// video_decoder.h: H.264 decoding, behind one interface.
//
// One implementation per platform: hardware on the Wii U, libavcodec on both
// Xboxes and the desktop. All take Annex-B access units and emit NV12, so
// everything downstream is shared and testable on a laptop.

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

    // Builds whichever implementation this platform has. The original Xbox
    // builds libavcodec cdecl and everything else stdcall, and this is the
    // only symbol crossing that line.
#if defined(_XBOX) && !defined(_XENON)
  #define BJ_DECODER_CALL __cdecl
#else
  #define BJ_DECODER_CALL
#endif
    static std::unique_ptr<VideoDecoder> BJ_DECODER_CALL Create();
};
