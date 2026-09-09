// SPDX-License-Identifier: GPL-2.0-or-later

// Decoded frames, and the colour conversion for platforms that need one.
//
// Frames carry a luma plane and two chroma planes at half resolution. The
// Wii U converts on the CPU, because its SDL renderer takes RGB only; the
// Xbox 360 hands the planes to the GPU and never calls the converter.

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

// How the two chroma planes sit in the chroma buffer.
//
// Interleaved is NV12, U and V alternating, which the Wii U's decoder
// produces. Planar is I420, the U plane then the V plane, which libavcodec
// produces and SDL's YUV upload takes.
enum class ChromaLayout { Interleaved, Planar };

// A borrowed view of a decoded frame, valid only for the duration of the
// callback that delivers it. Lets the hardware decoder hand over its own
// output buffer without copying it first.
struct Nv12View {
    const uint8_t* luma   = nullptr;
    const uint8_t* chroma = nullptr;
    int width  = 0;
    int height = 0;
    int lumaStride   = 0;
    int chromaStride = 0;
    int64_t pts = -1;
    ChromaLayout chromaLayout = ChromaLayout::Interleaved;

    bool valid() const { return luma && chroma && width > 0 && height > 0; }
};

struct Nv12Frame {
    std::vector<uint8_t> luma;      // width x height, lumaStride per row
    std::vector<uint8_t> chroma;    // width x height/2, interleaved U,V
    int width  = 0;
    int height = 0;
    int lumaStride   = 0;
    int chromaStride = 0;
    int64_t pts = -1;               // 90kHz units
    ChromaLayout chromaLayout = ChromaLayout::Interleaved;

    // Where the V plane starts, for Planar frames. Meaningless otherwise.
    size_t vPlaneOffset() const
    {
        return (size_t)chromaStride * (height / 2);
    }

    bool valid() const { return width > 0 && height > 0 && !luma.empty(); }

    // Copies a view into this frame, reusing the existing allocations when
    // they are already big enough. Recycled frames therefore cost a memcpy
    // and no allocation, which matters at 24 frames a second.
    void copyFrom(const Nv12View& view);
};

// Which matrix to use converting to RGB. Rec.601 for standard definition,
// Rec.709 for high definition; picking the wrong one shifts every colour.
enum class ColorSpace { Bt601, Bt709 };

// Byte order within a pixel in memory, not the SDL format name: on a little
// endian machine SDL_PIXELFORMAT_ARGB8888 is B,G,R,A. Prefer PixelBytes,
// which asks SDL; this is for callers with no renderer to ask.
enum class PixelOrder { Rgba, Argb, Bgra };

// Where each channel's byte sits within a 32-bit pixel.
struct PixelBytes {
    int r = 0, g = 1, b = 2, a = 3;
};

// Turns a channel mask into the index of the byte it occupies: 0x00FF0000
// is byte 2 on a little endian machine and byte 1 on a big endian one.
PixelBytes PixelBytesFromMasks(uint32_t rMask, uint32_t gMask,
                               uint32_t bMask, uint32_t aMask);

// Picks the matrix the way every other player does: by frame height.
inline ColorSpace ColorSpaceForHeight(int height)
{
    return height >= 576 ? ColorSpace::Bt709 : ColorSpace::Bt601;
}

// Converts NV12 to packed 32-bit RGBA (byte order R,G,B,A) on the calling
// thread. Fine for one-off conversions; too slow alone for live playback,
// where measurements on the console put a 720p frame at 42ms against a 41.7ms
// budget.
void ConvertNv12ToRgba(const Nv12Frame& frame,
                       uint8_t* destination,
                       int destinationStride,
                       ColorSpace space,
                       PixelOrder order = PixelOrder::Rgba);

// Packs a decoded frame into YUY2 for a texture unit that converts while
// sampling. `destination` needs width * 2 bytes a row.
void PackNv12ToYuy2(const Nv12Frame& frame,
                    uint8_t* destination,
                    int destinationStride);

// The same conversion spread across persistent worker threads.
//
// Splitting the image three ways brings a 720p frame from 42ms down to 14ms
// on the console. The workers are kept alive between frames deliberately:
// creating them per frame would mean 72 thread creations a second, which is
// its own cost on a machine this size.
class Nv12Converter {
public:
    // `threads` counts the calling thread, which takes a band itself rather
    // than idling. Three is right for the console's three cores.
    explicit Nv12Converter(int threads);
    ~Nv12Converter();

    Nv12Converter(const Nv12Converter&) = delete;
    Nv12Converter& operator=(const Nv12Converter&) = delete;

    // Explicit byte offsets, from the renderer's own masks.
    void convert(const Nv12Frame& frame, uint8_t* destination,
                 int destinationStride, ColorSpace space, PixelBytes bytes);

    void convert(const Nv12Frame& frame, uint8_t* destination,
                 int destinationStride, ColorSpace space,
                 PixelOrder order = PixelOrder::Rgba);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
