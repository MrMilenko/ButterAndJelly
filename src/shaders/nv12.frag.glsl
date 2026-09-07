// NV12 to RGB on the GPU.
//
// The hardware decoder writes a luma plane and an interleaved chroma plane.
// Sampling them as two textures and converting here means the CPU never
// touches video pixels at all: no colour conversion, and no upload through
// SDL's texture path, which costs a full GPU sync per frame.
//
// Rec.709 limited range, the same matrix the CPU path used.
#version 420 core

layout(binding = 0) uniform sampler2D tex_luma;     // .r = Y
layout(binding = 1) uniform sampler2D tex_chroma;   // .r = Cb, .g = Cr

in  vec2 v_uv;
out vec4 out_color;

void main()
{
    float luma   = texture(tex_luma, v_uv).r;
    vec2  chroma = texture(tex_chroma, v_uv).rg;

    // Expand from the 16-235 studio range video is coded in.
    float y  = 1.16438356 * (luma - 0.06274510);
    float cb = chroma.r - 0.5;
    float cr = chroma.g - 0.5;

    out_color = clamp(vec4(y + 1.79274107 * cr,
                           y - 0.21324861 * cb - 0.53290933 * cr,
                           y + 2.11240179 * cb,
                           1.0), 0.0, 1.0);
}
