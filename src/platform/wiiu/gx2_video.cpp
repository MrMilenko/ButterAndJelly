// SPDX-License-Identifier: GPL-2.0-or-later
#include "gx2_video.h"

#include "core/log.h"
#include "core/platform.h"

#include <coreinit/cache.h>
#include <gfd.h>
#include <gx2/draw.h>
#include <gx2/enum.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/sampler.h>
#include <gx2/shaders.h>
#include <gx2/state.h>
#include <gx2/texture.h>
#include <gx2/utils.h>
#include <gx2r/buffer.h>

#include <cstdio>
#include <cstring>
#include <malloc.h>
#include <vector>

namespace {

// Four vertices, each two floats of position followed by two of texture
// coordinate.
constexpr uint32_t kVertexStride = 4 * sizeof(float);
constexpr uint32_t kVertexCount  = 4;

// Planes are copied into buffers aligned for the GPU. Two sets in rotation so
// the CPU is never writing the buffer the GPU is currently sampling.
constexpr int kPlaneBufferCount = 2;

void* AllocAligned(size_t alignment, size_t size)
{
    return memalign(alignment, (size + alignment - 1) & ~(alignment - 1));
}

std::vector<uint8_t> ReadFile(const std::string& path)
{
    std::vector<uint8_t> data;
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return data;
    std::fseek(fp, 0, SEEK_END);
    const long size = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (size > 0) {
        data.resize((size_t)size);
        if (std::fread(data.data(), 1, (size_t)size, fp) != (size_t)size) data.clear();
    }
    std::fclose(fp);
    return data;
}

}  // namespace

struct Gx2Video::Impl {
    GX2VertexShader* vertexShader = nullptr;
    GX2PixelShader*  pixelShader  = nullptr;
    GX2FetchShader   fetchShader{};
    void*            fetchProgram = nullptr;

    GX2Sampler sampler{};
    GX2Texture luma{};
    GX2Texture chroma{};

    float* vertices = nullptr;

    // Plane storage, aligned and sized for the current frame.
    uint8_t* lumaBuffer[kPlaneBufferCount]   = { nullptr, nullptr };
    uint8_t* chromaBuffer[kPlaneBufferCount] = { nullptr, nullptr };
    size_t   lumaCapacity   = 0;
    size_t   chromaCapacity = 0;
    int      bufferIndex    = 0;

    int width = 0, height = 0, lumaStride = 0, chromaStride = 0;
};

Gx2Video::~Gx2Video()
{
    shutdown();
}

bool Gx2Video::loadShaders(const std::string& path, std::string& error)
{
    const std::vector<uint8_t> file = ReadFile(path);
    if (file.empty()) { error = "could not read " + path; return false; }

    if (GFDGetVertexShaderCount(file.data()) < 1 ||
        GFDGetPixelShaderCount(file.data()) < 1) {
        error = "shader file is missing a vertex or pixel shader";
        return false;
    }

    // Header and program are allocated separately: the program has to sit on
    // a boundary the shader units require.
    const uint32_t vsHeader  = GFDGetVertexShaderHeaderSize(0, file.data());
    const uint32_t vsProgram = GFDGetVertexShaderProgramSize(0, file.data());
    impl_->vertexShader = (GX2VertexShader*)AllocAligned(64, vsHeader);
    void* vsCode = AllocAligned(GX2_SHADER_PROGRAM_ALIGNMENT, vsProgram);
    if (!impl_->vertexShader || !vsCode) { error = "out of memory for the vertex shader"; return false; }
    if (!GFDGetVertexShader(impl_->vertexShader, vsCode, 0, file.data())) {
        error = "could not read the vertex shader";
        return false;
    }

    const uint32_t psHeader  = GFDGetPixelShaderHeaderSize(0, file.data());
    const uint32_t psProgram = GFDGetPixelShaderProgramSize(0, file.data());
    impl_->pixelShader = (GX2PixelShader*)AllocAligned(64, psHeader);
    void* psCode = AllocAligned(GX2_SHADER_PROGRAM_ALIGNMENT, psProgram);
    if (!impl_->pixelShader || !psCode) { error = "out of memory for the pixel shader"; return false; }
    if (!GFDGetPixelShader(impl_->pixelShader, psCode, 0, file.data())) {
        error = "could not read the pixel shader";
        return false;
    }

    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, vsCode, vsProgram);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, psCode, psProgram);
    return true;
}

bool Gx2Video::init(const std::string& shaderPath, std::string& error)
{
    shutdown();
    impl_ = new Impl();

    if (!loadShaders(shaderPath, error)) { shutdown(); return false; }

    // Two attributes, both two floats: position then texture coordinate.
    GX2AttribStream streams[2]{};
    streams[0].location    = 0;
    streams[0].buffer      = 0;
    streams[0].offset      = 0;
    streams[0].format      = GX2_ATTRIB_FORMAT_FLOAT_32_32;
    streams[0].type        = GX2_ATTRIB_INDEX_PER_VERTEX;
    streams[0].aluDivisor  = 0;
    streams[0].mask        = GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y,
                                          GX2_SQ_SEL_0, GX2_SQ_SEL_1);
    streams[0].endianSwap  = GX2_ENDIAN_SWAP_DEFAULT;

    streams[1] = streams[0];
    streams[1].location = 1;
    streams[1].offset   = 2 * sizeof(float);

    const uint32_t fetchSize = GX2CalcFetchShaderSizeEx(
        2, GX2_FETCH_SHADER_TESSELLATION_NONE, GX2_TESSELLATION_MODE_DISCRETE);
    impl_->fetchProgram = AllocAligned(GX2_SHADER_PROGRAM_ALIGNMENT, fetchSize);
    if (!impl_->fetchProgram) { error = "out of memory for the fetch shader"; shutdown(); return false; }

    GX2InitFetchShaderEx(&impl_->fetchShader, (uint8_t*)impl_->fetchProgram, 2, streams,
                         GX2_FETCH_SHADER_TESSELLATION_NONE,
                         GX2_TESSELLATION_MODE_DISCRETE);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, impl_->fetchProgram, fetchSize);

    impl_->vertices = (float*)AllocAligned(GX2_VERTEX_BUFFER_ALIGNMENT,
                                           kVertexCount * kVertexStride);
    if (!impl_->vertices) { error = "out of memory for the vertex buffer"; shutdown(); return false; }

    // Linear filtering, clamped at the edges so the last row of chroma does
    // not wrap around to the first.
    GX2InitSampler(&impl_->sampler, GX2_TEX_CLAMP_MODE_CLAMP,
                   GX2_TEX_XY_FILTER_MODE_LINEAR);

    ready_ = true;
    LOGF("[gx2] video renderer ready");
    return true;
}

void Gx2Video::shutdown()
{
    ready_ = false;
    if (!impl_) return;

    for (int i = 0; i < kPlaneBufferCount; ++i) {
        if (impl_->lumaBuffer[i])   free(impl_->lumaBuffer[i]);
        if (impl_->chromaBuffer[i]) free(impl_->chromaBuffer[i]);
    }
    if (impl_->vertices)     free(impl_->vertices);
    if (impl_->fetchProgram) free(impl_->fetchProgram);
    if (impl_->vertexShader) free(impl_->vertexShader);
    if (impl_->pixelShader)  free(impl_->pixelShader);

    delete impl_;
    impl_ = nullptr;
}

bool Gx2Video::ensurePlaneBuffers(const Nv12Frame& frame)
{
    if (frame.width == impl_->width && frame.height == impl_->height &&
        impl_->lumaBuffer[0]) {
        return true;
    }

    // Describe both planes and let GX2 work out pitch, size and alignment.
    // Its pitch is rounded up for the texture units and will not generally
    // match the decoder's stride, so rows are copied individually below.
    std::memset(&impl_->luma, 0, sizeof(impl_->luma));
    impl_->luma.surface.dim       = GX2_SURFACE_DIM_TEXTURE_2D;
    impl_->luma.surface.width     = (uint32_t)frame.width;
    impl_->luma.surface.height    = (uint32_t)frame.height;
    impl_->luma.surface.depth     = 1;
    impl_->luma.surface.mipLevels = 1;
    impl_->luma.surface.format    = GX2_SURFACE_FORMAT_UNORM_R8;
    impl_->luma.surface.aa        = GX2_AA_MODE1X;
    impl_->luma.surface.use       = GX2_SURFACE_USE_TEXTURE;
    impl_->luma.surface.tileMode  = GX2_TILE_MODE_LINEAR_ALIGNED;
    impl_->luma.surface.swizzle   = 0;
    impl_->luma.viewNumMips        = 1;
    impl_->luma.viewNumSlices      = 1;
    // Single channel, delivered to the shader in .x.
    impl_->luma.compMap            = 0x00040405;
    GX2CalcSurfaceSizeAndAlignment(&impl_->luma.surface);

    std::memset(&impl_->chroma, 0, sizeof(impl_->chroma));
    impl_->chroma.surface = impl_->luma.surface;
    impl_->chroma.surface.width  = (uint32_t)(frame.width / 2);
    impl_->chroma.surface.height = (uint32_t)(frame.height / 2);
    impl_->chroma.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8;
    impl_->chroma.viewNumMips     = 1;
    impl_->chroma.viewNumSlices   = 1;
    // Two channels: Cb in .x, Cr in .y.
    impl_->chroma.compMap         = 0x00010405;
    GX2CalcSurfaceSizeAndAlignment(&impl_->chroma.surface);

    for (int i = 0; i < kPlaneBufferCount; ++i) {
        if (impl_->lumaBuffer[i])   { free(impl_->lumaBuffer[i]);   impl_->lumaBuffer[i] = nullptr; }
        if (impl_->chromaBuffer[i]) { free(impl_->chromaBuffer[i]); impl_->chromaBuffer[i] = nullptr; }
    }
    for (int i = 0; i < kPlaneBufferCount; ++i) {
        impl_->lumaBuffer[i] = (uint8_t*)AllocAligned(impl_->luma.surface.alignment,
                                                      impl_->luma.surface.imageSize);
        impl_->chromaBuffer[i] = (uint8_t*)AllocAligned(impl_->chroma.surface.alignment,
                                                        impl_->chroma.surface.imageSize);
        if (!impl_->lumaBuffer[i] || !impl_->chromaBuffer[i]) return false;

        // GX2 rounds the pitch up past the frame width, so each row has
        // padding the sampler can reach at the edges. Black once, rather
        // than whatever the allocator handed over.
        std::memset(impl_->lumaBuffer[i], 16, impl_->luma.surface.imageSize);
        std::memset(impl_->chromaBuffer[i], 128, impl_->chroma.surface.imageSize);
    }

    impl_->width        = frame.width;
    impl_->height       = frame.height;
    impl_->lumaStride   = frame.lumaStride;
    impl_->chromaStride = frame.chromaStride;

    LOGF("[gx2] %dx%d planes: luma pitch %u size %u, chroma pitch %u size %u",
         frame.width, frame.height,
         impl_->luma.surface.pitch, impl_->luma.surface.imageSize,
         impl_->chroma.surface.pitch, impl_->chroma.surface.imageSize);
    return true;
}

void Gx2Video::draw(const Nv12Frame& frame, float left, float top,
                    float right, float bottom)
{
    if (!ready_ || !impl_ || !frame.valid()) return;
    if (!ensurePlaneBuffers(frame)) return;

    // Alternate buffers so the CPU never writes what the GPU is reading.
    impl_->bufferIndex = (impl_->bufferIndex + 1) % kPlaneBufferCount;
    uint8_t* lumaDst   = impl_->lumaBuffer[impl_->bufferIndex];
    uint8_t* chromaDst = impl_->chromaBuffer[impl_->bufferIndex];

    const uint64_t copyStart = Platform::NowMs();
    const uint32_t lumaPitch   = impl_->luma.surface.pitch;
    const uint32_t chromaPitch = impl_->chroma.surface.pitch;   // in texels

    for (int y = 0; y < frame.height; ++y) {
        std::memcpy(lumaDst + (size_t)y * lumaPitch,
                    frame.luma.data() + (size_t)y * frame.lumaStride,
                    (size_t)frame.width);
    }
    for (int y = 0; y < frame.height / 2; ++y) {
        std::memcpy(chromaDst + (size_t)y * chromaPitch * 2,
                    frame.chroma.data() + (size_t)y * frame.chromaStride,
                    (size_t)frame.width);
    }

    const uint64_t flushStart = Platform::NowMs();
    copyMs_ = (double)(flushStart - copyStart);

    // The GPU reads main memory directly, so the copies have to leave the
    // CPU's cache before it looks.
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, lumaDst,
                  impl_->luma.surface.imageSize);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, chromaDst,
                  impl_->chroma.surface.imageSize);
    const uint64_t drawStart = Platform::NowMs();
    flushMs_ = (double)(drawStart - flushStart);

    impl_->luma.surface.image   = lumaDst;
    impl_->chroma.surface.image = chromaDst;
    GX2InitTextureRegs(&impl_->luma);
    GX2InitTextureRegs(&impl_->chroma);

    // Triangle strip: bottom left, bottom right, top left, top right. Texture
    // coordinates run top down, which is the opposite of clip space.
    float* v = impl_->vertices;
    v[0]  = left;   v[1]  = bottom; v[2]  = 0.0f; v[3]  = 1.0f;
    v[4]  = right;  v[5]  = bottom; v[6]  = 1.0f; v[7]  = 1.0f;
    v[8]  = left;   v[9]  = top;    v[10] = 0.0f; v[11] = 0.0f;
    v[12] = right;  v[13] = top;    v[14] = 1.0f; v[15] = 0.0f;
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, v,
                  kVertexCount * kVertexStride);

    // Opaque video. Blend state is set explicitly rather than inherited:
    // SDL leaves alpha blending configured, and the previous code passed a
    // blend mask of zero and never touched GX2SetBlendControl at all, so the
    // draw ran with whatever happened to be current.
    GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS);
    GX2SetColorControl(GX2_LOGIC_OP_COPY, 0xFF, FALSE, TRUE);
    GX2SetBlendControl(GX2_RENDER_TARGET_0,
                       GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_ZERO,
                       GX2_BLEND_COMBINE_MODE_ADD, FALSE,
                       GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_ZERO,
                       GX2_BLEND_COMBINE_MODE_ADD);
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);

    // Our shaders declare UniformRegister mode. SDL's do too, so setting it
    // here does not disturb SDL, but leaving it unset means the shader units
    // are configured for whatever was last used.
    GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_REGISTER);

    GX2SetFetchShader(&impl_->fetchShader);
    GX2SetVertexShader(impl_->vertexShader);
    GX2SetPixelShader(impl_->pixelShader);

    // Rec.709 limited range, matching the CPU path. Passed as uniforms
    // rather than baked into the shader as literals.
    //   A = luma scale, luma offset, cr->r, cb->g
    //   B = cr->g, cb->b, chroma offset, alpha
    static const float kCoefA[4] = { 1.16438356f, -0.06274510f,
                                     1.79274107f, -0.21324861f };
    static const float kCoefB[4] = { -0.53290933f, 2.11240179f,
                                     -0.5f, 1.0f };
    GX2SetPixelUniformReg(0, 4, kCoefA);
    GX2SetPixelUniformReg(4, 4, kCoefB);

    GX2SetPixelTexture(&impl_->luma, 0);
    GX2SetPixelSampler(&impl_->sampler, 0);
    GX2SetPixelTexture(&impl_->chroma, 1);
    GX2SetPixelSampler(&impl_->sampler, 1);

    GX2SetAttribBuffer(0, kVertexCount * kVertexStride, kVertexStride,
                       impl_->vertices);

    GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLE_STRIP, kVertexCount, 0, 1);
    drawMs_ = (double)(Platform::NowMs() - drawStart);
}
