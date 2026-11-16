/* Direct3D 9 renderer for the Windows player. */

#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>

#include "tools/bink_d3d_renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct BinkD3DRenderer {
    IDirect3D9 *d3d;
    IDirect3DDevice9 *device;
    IDirect3DPixelShader9 *ps_present;
    IDirect3DPixelShader9 *ps_interp;
    IDirect3DTexture9 *planes[2][3];
    IDirect3DTexture9 *motion;
    unsigned long w;
    unsigned long h;
};

typedef struct {
    float x;
    float y;
    float z;
    float rhw;
    float u;
    float v;
} ScreenVertex;

#define BINK_D3D9_FVF (D3DFVF_XYZRHW | D3DFVF_TEX1)

/* ps_3_0: seven fetches with computed coordinates need more than ps_2_0. */
static const char *d3d9_common =
    "float4 row0; float4 row1; float4 row2;\n"
    "float4 u_offset; float4 u_luma; float4 u_blocks;\n"
    "float4 to_rgb(float3 yuv) {\n"
    "    float3 rgb = float3(dot(row0.xyz, yuv), dot(row1.xyz, yuv),\n"
    "                        dot(row2.xyz, yuv)) + u_offset.xyz;\n"
    "    return float4(saturate(rgb), 1.0);\n"
    "}\n";

static const char *d3d9_ps_present =
    "sampler2D y_tex : register(s0);\n"
    "sampler2D u_tex : register(s1);\n"
    "sampler2D v_tex : register(s2);\n"
    "float4 PSMain(float2 uv : TEXCOORD0) : COLOR {\n"
    "    float3 yuv = float3(tex2D(y_tex, uv).r, tex2D(u_tex, uv).r,\n"
    "                        tex2D(v_tex, uv).r);\n"
    "    return to_rgb(yuv);\n"
    "}\n";

static const char *d3d9_ps_interp =
    "sampler2D y0 : register(s0); sampler2D u0 : register(s1);\n"
    "sampler2D v0 : register(s2); sampler2D y1 : register(s3);\n"
    "sampler2D u1 : register(s4); sampler2D v1 : register(s5);\n"
    "sampler2D mv : register(s6);\n"
    "float4 PSMain(float2 uv : TEXCOORD0) : COLOR {\n"
    "    float t = u_luma.z;\n"
    "    float2 block = floor(uv * u_luma.xy / 8.0);\n"
    "    float2 mvuv = (block + 0.5) / u_blocks.xy;\n"
    "    float2 raw = tex2D(mv, mvuv).ra * 255.0 - 128.0;\n"
    "    float2 d = raw / u_luma.xy;\n"
    "    float2 uv0 = uv + t * d;\n"
    "    float2 uv1 = uv - (1.0 - t) * d;\n"
    "    float3 a = float3(tex2D(y0, uv0).r, tex2D(u0, uv0).r,\n"
    "                      tex2D(v0, uv0).r);\n"
    "    float3 b = float3(tex2D(y1, uv1).r, tex2D(u1, uv1).r,\n"
    "                      tex2D(v1, uv1).r);\n"
    "    return to_rgb(lerp(a, b, t));\n"
    "}\n";

static int compile(const char *source, const char *entry, const char *target,
                   const char *extra, ID3DBlob **out)
{
    char text[4096];
    ID3DBlob *errors = NULL;
    HRESULT hr;

    snprintf(text, sizeof(text), "%s%s", extra != NULL ? extra : "", source);
    hr = D3DCompile(text, strlen(text), NULL, NULL, NULL, entry, target, 0, 0,
                    out, &errors);
    if (FAILED(hr)) {
        fprintf(stderr, "%s %s failed to compile:%s%s\n", target, entry,
                errors != NULL ? "\n" : "",
                errors != NULL
                    ? (const char *)errors->lpVtbl->GetBufferPointer(errors)
                    : "");
        if (errors != NULL)
            errors->lpVtbl->Release(errors);
        return -1;
    }
    if (errors != NULL)
        errors->lpVtbl->Release(errors);
    return 0;
}

static IDirect3DTexture9 *make_texture(BinkD3DRenderer *r, D3DFORMAT format,
                                       unsigned long w, unsigned long h)
{
    IDirect3DTexture9 *texture = NULL;

    if (FAILED(r->device->lpVtbl->CreateTexture(
            r->device, (UINT)w, (UINT)h, 1, 0, format, D3DPOOL_MANAGED,
            &texture, NULL)))
        return NULL;
    return texture;
}

/* Copy h rows of w texels, bytes_per_texel wide, into the texture. The
 * source advances by `stride` and the destination by the driver's pitch,
 * which need not equal either the stride or the row size. */
static int upload_rows(IDirect3DTexture9 *texture, const unsigned char *data,
                       unsigned long w, unsigned long h, int stride,
                       unsigned bytes_per_texel)
{
    D3DLOCKED_RECT locked;
    size_t row_bytes = (size_t)w * bytes_per_texel;
    unsigned long y;

    if (texture == NULL || data == NULL)
        return -1;
    if (FAILED(texture->lpVtbl->LockRect(texture, 0, &locked, NULL, 0)))
        return -1;
    if (locked.Pitch < 0 || (size_t)locked.Pitch < row_bytes) {
        texture->lpVtbl->UnlockRect(texture, 0);
        return -1;
    }
    for (y = 0; y < h; y++)
        memcpy((unsigned char *)locked.pBits + (size_t)y * (size_t)locked.Pitch,
               data + (size_t)y * (size_t)stride, row_bytes);
    texture->lpVtbl->UnlockRect(texture, 0);
    return 0;
}

BinkD3DRenderer *bink_d3d9_renderer_create(void *hwnd, unsigned long w,
                                           unsigned long h)
{
    BinkD3DRenderer *r;
    D3DPRESENT_PARAMETERS present;
    D3DDISPLAYMODE mode;
    ID3DBlob *blob = NULL;
    DWORD behaviour = D3DCREATE_HARDWARE_VERTEXPROCESSING |
                      D3DCREATE_FPU_PRESERVE;
    HRESULT hr;
    int slot;

    r = (BinkD3DRenderer *)calloc(1, sizeof(*r));
    if (r == NULL)
        return NULL;
    r->w = w;
    r->h = h;

    r->d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (r->d3d == NULL) {
        fprintf(stderr, "Direct3DCreate9 failed.\n");
        goto fail;
    }
    if (FAILED(r->d3d->lpVtbl->GetAdapterDisplayMode(r->d3d, D3DADAPTER_DEFAULT,
                                                     &mode)))
        memset(&mode, 0, sizeof(mode));

    memset(&present, 0, sizeof(present));
    present.Windowed = TRUE;
    present.SwapEffect = D3DSWAPEFFECT_DISCARD;
    present.BackBufferFormat =
        mode.Format != D3DFMT_UNKNOWN ? mode.Format : D3DFMT_X8R8G8B8;
    present.BackBufferWidth = (UINT)w;
    present.BackBufferHeight = (UINT)h;
    present.hDeviceWindow = (HWND)hwnd;
    present.PresentationInterval = D3DPRESENT_INTERVAL_ONE;

    hr = r->d3d->lpVtbl->CreateDevice(r->d3d, D3DADAPTER_DEFAULT,
                                      D3DDEVTYPE_HAL, (HWND)hwnd, behaviour,
                                      &present, &r->device);
    if (FAILED(hr)) {
        behaviour = D3DCREATE_SOFTWARE_VERTEXPROCESSING |
                    D3DCREATE_FPU_PRESERVE;
        hr = r->d3d->lpVtbl->CreateDevice(r->d3d, D3DADAPTER_DEFAULT,
                                          D3DDEVTYPE_HAL, (HWND)hwnd,
                                          behaviour, &present, &r->device);
    }
    if (FAILED(hr)) {
        fprintf(stderr, "D3D9 CreateDevice failed: 0x%08lx\n",
                (unsigned long)hr);
        goto fail;
    }

    if (compile(d3d9_ps_present, "PSMain", "ps_3_0", d3d9_common, &blob) != 0)
        goto fail;
    if (FAILED(r->device->lpVtbl->CreatePixelShader(
            r->device, (const DWORD *)blob->lpVtbl->GetBufferPointer(blob),
            &r->ps_present))) {
        fprintf(stderr, "Cannot create the D3D9 presentation shader.\n");
        goto fail;
    }
    blob->lpVtbl->Release(blob);
    blob = NULL;

    if (compile(d3d9_ps_interp, "PSMain", "ps_3_0", d3d9_common, &blob) != 0)
        goto fail;
    if (FAILED(r->device->lpVtbl->CreatePixelShader(
            r->device, (const DWORD *)blob->lpVtbl->GetBufferPointer(blob),
            &r->ps_interp))) {
        fprintf(stderr, "Cannot create the D3D9 interpolation shader.\n");
        goto fail;
    }
    blob->lpVtbl->Release(blob);
    blob = NULL;

    for (slot = 0; slot < 2; slot++) {
        r->planes[slot][0] = make_texture(r, D3DFMT_L8, w, h);
        r->planes[slot][1] = make_texture(r, D3DFMT_L8, (w + 1) / 2,
                                          (h + 1) / 2);
        r->planes[slot][2] = make_texture(r, D3DFMT_L8, (w + 1) / 2,
                                          (h + 1) / 2);
        if (r->planes[slot][0] == NULL || r->planes[slot][1] == NULL ||
            r->planes[slot][2] == NULL) {
            fprintf(stderr, "Cannot create the D3D9 plane textures.\n");
            goto fail;
        }
    }
    r->motion = make_texture(r, D3DFMT_A8L8, (w + 7) / 8, (h + 7) / 8);
    if (r->motion == NULL) {
        fprintf(stderr, "Cannot create the D3D9 motion texture.\n");
        goto fail;
    }

    r->device->lpVtbl->SetRenderState(r->device, D3DRS_CULLMODE, D3DCULL_NONE);
    r->device->lpVtbl->SetRenderState(r->device, D3DRS_LIGHTING, FALSE);
    r->device->lpVtbl->SetRenderState(r->device, D3DRS_ZENABLE, FALSE);
    r->device->lpVtbl->SetRenderState(r->device, D3DRS_ALPHABLENDENABLE, FALSE);
    return r;

fail:
    if (blob != NULL)
        blob->lpVtbl->Release(blob);
    bink_d3d_renderer_destroy(r);
    return NULL;
}

int bink_d3d_renderer_upload(BinkD3DRenderer *r, bink object, int slot)
{
    const unsigned char *y;
    const unsigned char *u;
    const unsigned char *v;
    unsigned long cw;
    unsigned long ch;

    if (r == NULL || slot < 0 || slot > 1)
        return -1;
    y = bink_get_plane(object, BINK_PLANE_Y);
    u = bink_get_plane(object, BINK_PLANE_U);
    v = bink_get_plane(object, BINK_PLANE_V);
    if (y == NULL || u == NULL || v == NULL)
        return -1;
    cw = (r->w + 1) / 2;
    ch = (r->h + 1) / 2;
    if (upload_rows(r->planes[slot][0], y, r->w, r->h,
                    bink_get_stride(object, BINK_PLANE_Y), 1) != 0 ||
        upload_rows(r->planes[slot][1], u, cw, ch,
                    bink_get_stride(object, BINK_PLANE_U), 1) != 0 ||
        upload_rows(r->planes[slot][2], v, cw, ch,
                    bink_get_stride(object, BINK_PLANE_V), 1) != 0)
        return -1;
    return 0;
}

int bink_d3d_renderer_upload_motion(BinkD3DRenderer *r, bink object,
                                    unsigned long *blocks_w,
                                    unsigned long *blocks_h)
{
    const signed char *mx = NULL;
    const signed char *my = NULL;
    unsigned long bw = 0;
    unsigned long bh = 0;
    unsigned char *texels;
    unsigned long i;
    int rc;

    if (blocks_w != NULL)
        *blocks_w = 0;
    if (blocks_h != NULL)
        *blocks_h = 0;
    if (r == NULL)
        return -1;
    if (!bink_get_motion(object, &bw, &bh, &mx, &my) || bw == 0 || bh == 0)
        return -1;
    texels = (unsigned char *)malloc((size_t)bw * (size_t)bh * 2u);
    if (texels == NULL)
        return -1;
    /* A8L8 stores luminance then alpha: x in .r, y in .a. */
    for (i = 0; i < bw * bh; i++) {
        texels[i * 2u] = (unsigned char)((int)mx[i] + 128);
        texels[i * 2u + 1u] = (unsigned char)((int)my[i] + 128);
    }
    rc = upload_rows(r->motion, texels, bw, bh, (int)(bw * 2u), 2);
    free(texels);
    if (rc == 0) {
        if (blocks_w != NULL)
            *blocks_w = bw;
        if (blocks_h != NULL)
            *blocks_h = bh;
    }
    return rc;
}

static void set_sampler(BinkD3DRenderer *r, DWORD stage, D3DTEXTUREFILTERTYPE f)
{
    r->device->lpVtbl->SetSamplerState(r->device, stage, D3DSAMP_MINFILTER, f);
    r->device->lpVtbl->SetSamplerState(r->device, stage, D3DSAMP_MAGFILTER, f);
    r->device->lpVtbl->SetSamplerState(r->device, stage, D3DSAMP_ADDRESSU,
                                       D3DTADDRESS_CLAMP);
    r->device->lpVtbl->SetSamplerState(r->device, stage, D3DSAMP_ADDRESSV,
                                       D3DTADDRESS_CLAMP);
}

int bink_d3d_renderer_draw(BinkD3DRenderer *r, const BinkD3DDraw *draw)
{
    float constants[24];
    ScreenVertex quad[4];
    int cur;
    int next;
    int i;

    if (r == NULL || draw == NULL)
        return -1;
    memset(constants, 0, sizeof(constants));
    constants[0] = 1.164f;
    constants[1] = 0.0f;
    constants[2] = 1.596f;
    constants[4] = 1.164f;
    constants[5] = -0.392f;
    constants[6] = -0.813f;
    constants[8] = 1.164f;
    constants[9] = 2.017f;
    constants[10] = 0.0f;
    {
        const float y0 = 16.0f / 255.0f;
        const float c0 = 128.0f / 255.0f;

        constants[12] = -1.164f * y0 - 1.596f * c0;
        constants[13] = -1.164f * y0 + 0.392f * c0 + 0.813f * c0;
        constants[14] = -1.164f * y0 - 2.017f * c0;
    }
    constants[16] = (float)r->w;
    constants[17] = (float)r->h;
    constants[18] = draw->t;
    constants[20] = (float)(draw->blocks_w > 0 ? draw->blocks_w : 1);
    constants[21] = (float)(draw->blocks_h > 0 ? draw->blocks_h : 1);

    cur = draw->slot_cur & 1;
    next = cur ^ 1;
    quad[0].x = 0.0f;
    quad[0].y = 0.0f;
    quad[0].u = 0.0f;
    quad[0].v = 0.0f;
    quad[1].x = (float)r->w;
    quad[1].y = 0.0f;
    quad[1].u = 1.0f;
    quad[1].v = 0.0f;
    quad[2].x = (float)r->w;
    quad[2].y = (float)r->h;
    quad[2].u = 1.0f;
    quad[2].v = 1.0f;
    quad[3].x = 0.0f;
    quad[3].y = (float)r->h;
    quad[3].u = 0.0f;
    quad[3].v = 1.0f;
    for (i = 0; i < 4; i++) {
        quad[i].z = 0.0f;
        quad[i].rhw = 1.0f;
    }

    /* A device reset from a lost device would be handled here; the player
     * is a test tool, so a lost device simply stops rendering. */
    if (FAILED(r->device->lpVtbl->BeginScene(r->device)))
        return -1;
    r->device->lpVtbl->Clear(r->device, 0, NULL, D3DCLEAR_TARGET, 0x00000000,
                             1.0f, 0);
    r->device->lpVtbl->SetFVF(r->device, BINK_D3D9_FVF);
    r->device->lpVtbl->SetPixelShaderConstantF(r->device, 0, constants, 6);

    if (draw->interp && draw->have_next) {
        r->device->lpVtbl->SetPixelShader(r->device, r->ps_interp);
        r->device->lpVtbl->SetTexture(r->device, 0, (IDirect3DBaseTexture9 *)r->planes[cur][0]);
        r->device->lpVtbl->SetTexture(r->device, 1, (IDirect3DBaseTexture9 *)r->planes[cur][1]);
        r->device->lpVtbl->SetTexture(r->device, 2, (IDirect3DBaseTexture9 *)r->planes[cur][2]);
        r->device->lpVtbl->SetTexture(r->device, 3, (IDirect3DBaseTexture9 *)r->planes[next][0]);
        r->device->lpVtbl->SetTexture(r->device, 4, (IDirect3DBaseTexture9 *)r->planes[next][1]);
        r->device->lpVtbl->SetTexture(r->device, 5, (IDirect3DBaseTexture9 *)r->planes[next][2]);
        r->device->lpVtbl->SetTexture(
            r->device, 6, (IDirect3DBaseTexture9 *)r->motion);
        for (i = 0; i < 6; i++)
            set_sampler(r, (DWORD)i, D3DTEXF_LINEAR);
        set_sampler(r, 6, D3DTEXF_POINT);
    } else {
        r->device->lpVtbl->SetPixelShader(r->device, r->ps_present);
        r->device->lpVtbl->SetTexture(r->device, 0, (IDirect3DBaseTexture9 *)r->planes[cur][0]);
        r->device->lpVtbl->SetTexture(r->device, 1, (IDirect3DBaseTexture9 *)r->planes[cur][1]);
        r->device->lpVtbl->SetTexture(r->device, 2, (IDirect3DBaseTexture9 *)r->planes[cur][2]);
        for (i = 0; i < 3; i++)
            set_sampler(r, (DWORD)i, D3DTEXF_LINEAR);
    }

    r->device->lpVtbl->DrawPrimitiveUP(r->device, D3DPT_TRIANGLEFAN, 2, quad,
                                       sizeof(ScreenVertex));
    r->device->lpVtbl->EndScene(r->device);
    r->device->lpVtbl->Present(r->device, NULL, NULL, NULL, NULL);
    return 0;
}

void bink_d3d_renderer_destroy(BinkD3DRenderer *r)
{
    int slot;
    int plane;

    if (r == NULL)
        return;
    for (slot = 0; slot < 2; slot++)
        for (plane = 0; plane < 3; plane++)
            if (r->planes[slot][plane] != NULL)
                r->planes[slot][plane]->lpVtbl->Release(r->planes[slot][plane]);
    if (r->motion != NULL)
        r->motion->lpVtbl->Release(r->motion);
    if (r->ps_interp != NULL)
        r->ps_interp->lpVtbl->Release(r->ps_interp);
    if (r->ps_present != NULL)
        r->ps_present->lpVtbl->Release(r->ps_present);
    if (r->device != NULL)
        r->device->lpVtbl->Release(r->device);
    if (r->d3d != NULL)
        r->d3d->lpVtbl->Release(r->d3d);
    free(r);
}
