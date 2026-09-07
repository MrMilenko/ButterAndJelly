/* The shader layer that stands in for the fixed function pipeline.
 *
 * The Xbox 360 has no fixed function: SetTextureStageState does not exist on it, so every
 * texture environment the GL shim used to express with D3DTSS_COLOROP has to be a pixel
 * shader here. Quake reaches three of them.
 */
#ifndef FAKEGLX_SHADERS360_H
#define FAKEGLX_SHADERS360_H

#include <xtl.h>

#ifdef __cplusplus
extern "C" {
#endif

enum GLPixelShaderId {
    GLPS_REPLACE = 0,   /* GL_REPLACE: sample the texture                    */
    GLPS_MODULATE,      /* GL_MODULATE: texture * vertex colour              */
    GLPS_COLOR_ONLY,    /* texture disabled: vertex colour alone             */
    GLPS_COUNT
};

/* Creates the shaders. Call once, after the device exists. */
HRESULT GLShaders_Init (D3DDevice *dev);

/* Which pixel shader the next draw uses. Chosen from the whole state -- whether a texture
   is bound as well as the texenv mode -- because GL selects on both, and keying on texenv
   alone is what leaves the shim drawing with a stale shader. */
void GLShaders_Select (int psId);

/* The current transform. The matrix stacks live in a different class from the draw call,
   so the shader layer holds the combined matrix rather than reaching across. */
void GLShaders_SetTransform (const D3DXMATRIX *modelview, const D3DXMATRIX *projection);

/* Binds shaders, the vertex declaration for this FVF, and the transform, before a draw. */
HRESULT GLShaders_Apply (D3DDevice *dev, DWORD fvf, const D3DXMATRIX *mvp);

void GLShaders_Shutdown (void);

#ifdef __cplusplus
}
#endif

#endif
