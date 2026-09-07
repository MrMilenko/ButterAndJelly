# SDL2x360 changes

Four changes to [SDL2x360](https://github.com/Wolf3s/SDL2x360), applied to the
vendored copy in `third_party/SDL2x360`. All are worth having upstream.

## Texture binding

`sdl2x360-settexture-fetchconstant.patch`, against `SDL_render_d3d.c`.

Every draw sampled whichever texture was bound last.
`D3DDevice_SetTexture_Inline` passes a `UINT64` fetch constant mask, and clang
used to split 64 bit values across two argument slots where the XDK's compiler
uses one register, so the mask arrived corrupt and the fetch constant was never
marked dirty. `SetTextureFetchConstant` takes a `UINT` and a pointer instead.

OXDK's `0003-PowerPC-Xenon-64-bit-integers-in-one-register.patch` fixes the
compiler, so this is no longer required. `SetStreamSource`,
`SetVertexShaderConstantFN` and `SetPixelShaderConstantFN` take the same mask
and were wrong the same way; all four are correct now. Kept because it costs
nothing.

## YUV shaders

SDL ships them as PC `ps_2_0` bytecode, which Xenos rejects, so all three fail
to create and the renderer stops advertising YUV. The vendored copy keeps the
HLSL and compiles it on the console with `D3DXCompileShader`.

Without this, colour conversion falls to the CPU: about 78ms a frame at 720p
against a 41.7ms budget.

## YUV plane format

`PixelFormatToD3DFMT` returned `D3DFMT_L8`, which is tiled, while every other
sampled texture uses the `LIN_` variants. Linear rows into a tiled texture come
out as shuffled blocks. Now `D3DFMT_LIN_L8`.

## The YUV capability gate

SDL gates its YUV path on `caps.MaxSimultaneousTextures >= 3`. That describes
the fixed function multitexture pipeline, which Xenos does not have, so it
reports 0 and the path is skipped before a shader is attempted. The vendored
copy tests whether the shaders compile.
