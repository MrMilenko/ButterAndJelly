#include "gl_shaders360.h"
#include "shaders/VsMain.h"
#include "shaders/PsReplace.h"
#include "shaders/PsModulate.h"
#include "shaders/PsColorOnly.h"

static D3DVertexShader *s_vs;
static D3DPixelShader  *s_ps[GLPS_COUNT];
static int              s_selected = GLPS_REPLACE;

/* One declaration per vertex layout the shim emits. The FVF is built at
   fakeglx.cpp:704-713 as position, optional diffuse, then one or two texcoord sets, so
   these two cover everything it produces. The shaders read TEXCOORD1 only in the
   multitexture variants, which Quake does not reach, so the single-texcoord layout is
   safe to use with them. */
static D3DVertexDeclaration *s_decl1;   /* pos, colour, uv0      */
static D3DVertexDeclaration *s_decl2;   /* pos, colour, uv0, uv1 */

static const D3DVERTEXELEMENT9 s_elems1[] = {
    { 0,  0, D3DDECLTYPE_FLOAT3,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
    { 0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0 },
    { 0, 16, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
    D3DDECL_END()
};
static const D3DVERTEXELEMENT9 s_elems2[] = {
    { 0,  0, D3DDECLTYPE_FLOAT3,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
    { 0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0 },
    { 0, 16, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
    { 0, 24, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
    D3DDECL_END()
};

HRESULT GLShaders_Init (D3DDevice *dev)
{
    HRESULT hr;

    if (FAILED(hr = dev->CreateVertexShader((const DWORD *)g_VsMain, &s_vs)))            return hr;
    if (FAILED(hr = dev->CreatePixelShader((const DWORD *)g_PsReplace,   &s_ps[GLPS_REPLACE])))    return hr;
    if (FAILED(hr = dev->CreatePixelShader((const DWORD *)g_PsModulate,  &s_ps[GLPS_MODULATE])))   return hr;
    if (FAILED(hr = dev->CreatePixelShader((const DWORD *)g_PsColorOnly, &s_ps[GLPS_COLOR_ONLY]))) return hr;
    if (FAILED(hr = dev->CreateVertexDeclaration(s_elems1, &s_decl1)))                    return hr;
    if (FAILED(hr = dev->CreateVertexDeclaration(s_elems2, &s_decl2)))                    return hr;

    return S_OK;
}

void GLShaders_Select (int psId)
{
    if (psId >= 0 && psId < GLPS_COUNT)
        s_selected = psId;
}

static D3DXMATRIX s_mvp;

void GLShaders_SetTransform (const D3DXMATRIX *modelview, const D3DXMATRIX *projection)
{
    D3DXMatrixMultiply (&s_mvp, modelview, projection);
}

HRESULT GLShaders_Apply (D3DDevice *dev, DWORD fvf, const D3DXMATRIX *mvp)
{
    int texcoords = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;

    dev->SetVertexDeclaration (texcoords >= 2 ? s_decl2 : s_decl1);
    dev->SetVertexShader (s_vs);
    dev->SetPixelShader  (s_ps[s_selected]);

    /* Compiled /Zpr, so the constants are the matrix ROWS and a D3DXMATRIX uploads as it
       sits in memory. Transposing here is the classic way to get a black screen. */
    dev->SetVertexShaderConstantF (0, (const float *)(mvp ? mvp : &s_mvp), 4);
    return S_OK;
}

void GLShaders_Shutdown (void)
{
    int i;
    if (s_vs)    { s_vs->Release();    s_vs = 0; }
    for (i = 0; i < GLPS_COUNT; i++)
        if (s_ps[i]) { s_ps[i]->Release(); s_ps[i] = 0; }
    if (s_decl1) { s_decl1->Release(); s_decl1 = 0; }
    if (s_decl2) { s_decl2->Release(); s_decl2 = 0; }
}
