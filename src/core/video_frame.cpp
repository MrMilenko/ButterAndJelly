// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/video_frame.h"
#include "core/thread.h"

#include <algorithm>
#include <cstring>
#include <vector>

void Nv12Frame::copyFrom(const Nv12View& view)
{
    width        = view.width;
    height       = view.height;
    lumaStride   = view.lumaStride;
    chromaStride = view.chromaStride;
    chromaLayout = view.chromaLayout;
    pts          = view.pts;

    const size_t lumaBytes   = (size_t)view.lumaStride * view.height;
    // Planar frames hold two chroma planes back to back, so twice the rows.
    const size_t chromaRows  = view.chromaLayout == ChromaLayout::Planar
                                   ? (size_t)view.height          // height/2 twice
                                   : (size_t)(view.height / 2);
    const size_t chromaBytes = (size_t)view.chromaStride * chromaRows;

    // resize keeps whatever capacity is already there, so a recycled frame
    // does not go back to the allocator.
    luma.resize(lumaBytes);
    chroma.resize(chromaBytes);
    std::memcpy(luma.data(), view.luma, lumaBytes);
    std::memcpy(chroma.data(), view.chroma, chromaBytes);
}

namespace {

// Fixed point coefficients, scaled by 256. Limited range input (Y 16-235,
// chroma 16-240), which is what H.264 video carries unless it says otherwise.
struct Coefficients {
    int rV, gU, gV, bU;
};

constexpr Coefficients kBt601{ 409, -100, -208, 516 };
constexpr Coefficients kBt709{ 459,  -55, -136, 541 };

// Saturating 0..255 through a table rather than branches. The range covers
// every value the arithmetic below can produce.
constexpr int kClampOffset = 512;
constexpr int kClampSize   = 256 + 2 * kClampOffset;

const uint8_t* ClampTable()
{
    static const std::vector<uint8_t> table = [] {
        std::vector<uint8_t> t((size_t)kClampSize);
        for (int i = 0; i < kClampSize; ++i) {
            const int value = i - kClampOffset;
            t[(size_t)i] = (uint8_t)(value < 0 ? 0 : (value > 255 ? 255 : value));
        }
        return t;
    }();
    return table.data();
}

// Byte positions within a pixel, worked out once rather than branched on per
// pixel: {red, green, blue, alpha}.
struct ByteOrder { int r, g, b, a; };

// A memory probe rather than arithmetic, so it cannot disagree with the
// machine it is running on.
static int ByteOfMask(uint32_t mask)
{
    uint32_t word = mask;
    const uint8_t* bytes = (const uint8_t*)&word;
    for (int i = 0; i < 4; ++i) {
        if (bytes[i] == 0xFF) return i;
    }
    return 0;
}

inline ByteOrder OrderFor(PixelOrder order)
{
    if (order == PixelOrder::Argb) return ByteOrder{ 1, 2, 3, 0 };
    if (order == PixelOrder::Bgra) return ByteOrder{ 2, 1, 0, 3 };
    return ByteOrder{ 0, 1, 2, 3 };
}

void ConvertBand(const Nv12Frame& frame, uint8_t* destination,
                 int destinationStride, const Coefficients& c,
                 int firstRow, int lastRow, ByteOrder o)
{
    const uint8_t* clamp = ClampTable() + kClampOffset;

    // Chroma is shared by a 2x2 block, so rows are handled in pairs and the
    // per-block chroma terms are computed once for four pixels.
    for (int y = firstRow; y < lastRow; ++y) {
        const uint8_t* lumaRow = frame.luma.data() + (size_t)y * frame.lumaStride;
        uint8_t* out = destination + (size_t)y * destinationStride;

        // Where this row's U and V samples live, and how far apart two
        // consecutive ones are: adjacent in a planar frame, every other byte
        // in an interleaved one.
        const bool planar = frame.chromaLayout == ChromaLayout::Planar;
        const size_t rowOffset = (size_t)(y / 2) * frame.chromaStride;
        const uint8_t* uRow = frame.chroma.data() + rowOffset;
        const uint8_t* vRow = planar
                                  ? uRow + frame.vPlaneOffset()
                                  : uRow + 1;
        const int step = planar ? 1 : 2;

        for (int x = 0; x < frame.width; x += 2) {
            const int u = (int)uRow[(x / 2) * step] - 128;
            const int v = (int)vRow[(x / 2) * step] - 128;

            const int rTerm = c.rV * v;
            const int gTerm = c.gU * u + c.gV * v;
            const int bTerm = c.bU * u;

            for (int dx = 0; dx < 2 && x + dx < frame.width; ++dx) {
                const int yScaled = 298 * ((int)lumaRow[x + dx] - 16);
                uint8_t* pixel = out + (size_t)(x + dx) * 4;
                pixel[o.r] = clamp[(yScaled + rTerm) >> 8];
                pixel[o.g] = clamp[(yScaled + gTerm) >> 8];
                pixel[o.b] = clamp[(yScaled + bTerm) >> 8];
                pixel[o.a] = 255;
            }
        }
    }
}

}  // namespace

void PackNv12ToYuy2(const Nv12Frame& frame, uint8_t* destination,
                    int destinationStride)
{
    if (!frame.valid() || !destination) return;

    const bool planar = frame.chromaLayout == ChromaLayout::Planar;
    const int step = planar ? 1 : 2;

    for (int y = 0; y < frame.height; ++y) {
        const uint8_t* lumaRow = frame.luma.data() + (size_t)y * frame.lumaStride;
        const size_t rowOffset = (size_t)(y / 2) * frame.chromaStride;
        const uint8_t* uRow = frame.chroma.data() + rowOffset;
        const uint8_t* vRow = planar ? uRow + frame.vPlaneOffset() : uRow + 1;

        uint8_t* out = destination + (size_t)y * destinationStride;

        for (int x = 0; x + 1 < frame.width; x += 2) {
            const int c = (x / 2) * step;
            out[0] = lumaRow[x];
            out[1] = uRow[c];
            out[2] = lumaRow[x + 1];
            out[3] = vRow[c];
            out += 4;
        }
    }
}

PixelBytes PixelBytesFromMasks(uint32_t rMask, uint32_t gMask,
                               uint32_t bMask, uint32_t aMask)
{
    PixelBytes bytes;
    bytes.r = ByteOfMask(rMask);
    bytes.g = ByteOfMask(gMask);
    bytes.b = ByteOfMask(bMask);
    // A fully opaque pixel still needs somewhere to put the alpha: whichever
    // byte the other three left free.
    bytes.a = aMask ? ByteOfMask(aMask) : (6 - bytes.r - bytes.g - bytes.b);
    return bytes;
}

void ConvertNv12ToRgba(const Nv12Frame& frame, uint8_t* destination,
                       int destinationStride, ColorSpace space,
                       PixelOrder order)
{
    if (!frame.valid() || !destination) return;
    const Coefficients& c = (space == ColorSpace::Bt709) ? kBt709 : kBt601;
    ConvertBand(frame, destination, destinationStride, c, 0, frame.height,
                OrderFor(order));
}

// ------------------------------------------------------------ Nv12Converter

struct Nv12Converter::Impl {
    // What the workers are converting right now. Valid only between the
    // generation bump and every worker reporting done.
    const Nv12Frame*    frame = nullptr;
    uint8_t*            destination = nullptr;
    int                 destinationStride = 0;
    const Coefficients* coefficients = nullptr;
    int                 bandRows = 0;
    ByteOrder           order{ 0, 1, 2, 3 };

    std::vector<bj::Thread> workers;
    bj::Mutex              mutex;
    bj::CondVar startCv;
    bj::CondVar doneCv;
    uint64_t generation = 0;      // bumped once per frame to release workers
    int      outstanding = 0;
    bool     stopping = false;
};

Nv12Converter::Nv12Converter(int threads) : impl_(new Impl())
{
    if (threads < 1) threads = 1;

    // The caller takes band 0, so only the extra bands need a worker.
    for (int i = 1; i < threads; ++i) {
        impl_->workers.emplace_back([this, i] {
            Impl& impl = *impl_;
            uint64_t seen = 0;
            for (;;) {
                bj::Lock lock(impl.mutex);
                impl.startCv.wait(lock, [&] {
                    return impl.stopping || impl.generation != seen;
                });
                if (impl.stopping) return;
                seen = impl.generation;

                const Nv12Frame* frame = impl.frame;
                uint8_t* destination   = impl.destination;
                const int stride       = impl.destinationStride;
                const Coefficients* c  = impl.coefficients;
                const int first = i * impl.bandRows;
                const int last  = std::min(frame->height, first + impl.bandRows);
                const ByteOrder order = impl.order;
                lock.unlock();

                if (first < last) {
                    ConvertBand(*frame, destination, stride, *c, first, last, order);
                }

                lock.lock();
                if (--impl.outstanding == 0) impl.doneCv.notifyOne();
            }
        });
    }
}

Nv12Converter::~Nv12Converter()
{
    {
        bj::ScopedLock lock(impl_->mutex);
        impl_->stopping = true;
    }
    impl_->startCv.notifyAll();
    for (bj::Thread& worker : impl_->workers) {
        if (worker.joinable()) worker.join();
    }
}

void Nv12Converter::convert(const Nv12Frame& frame, uint8_t* destination,
                            int destinationStride, ColorSpace space,
                            PixelOrder order)
{
    const ByteOrder o = OrderFor(order);
    PixelBytes bytes;
    bytes.r = o.r; bytes.g = o.g; bytes.b = o.b; bytes.a = o.a;
    convert(frame, destination, destinationStride, space, bytes);
}

void Nv12Converter::convert(const Nv12Frame& frame, uint8_t* destination,
                            int destinationStride, ColorSpace space,
                            PixelBytes bytes)
{
    if (!frame.valid() || !destination) return;

    const Coefficients& c = (space == ColorSpace::Bt709) ? kBt709 : kBt601;
    const ByteOrder o = ByteOrder{ bytes.r, bytes.g, bytes.b, bytes.a };
    const int threads = (int)impl_->workers.size() + 1;

    if (threads == 1) {
        ConvertBand(frame, destination, destinationStride, c, 0, frame.height, o);
        return;
    }

    // Bands start on even rows so no two threads share a chroma row.
    const int bandRows = ((frame.height / threads) + 1) & ~1;

    {
        bj::ScopedLock lock(impl_->mutex);
        impl_->frame             = &frame;
        impl_->destination       = destination;
        impl_->destinationStride = destinationStride;
        impl_->coefficients      = &c;
        impl_->bandRows          = bandRows;
        impl_->order             = o;
        impl_->outstanding       = threads - 1;
        ++impl_->generation;
    }
    impl_->startCv.notifyAll();

    // The calling thread does the first band rather than blocking on workers.
    ConvertBand(frame, destination, destinationStride, c,
                0, std::min(frame.height, bandRows), o);

    bj::Lock lock(impl_->mutex);
    impl_->doneCv.wait(lock, [&] { return impl_->outstanding == 0; });
}
