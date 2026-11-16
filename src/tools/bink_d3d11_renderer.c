/* Direct3D 11 renderer for the Windows player. */

#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include "tools/bink_d3d_renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct BinkD3DRenderer {
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    IDXGISwapChain *swap;
    ID3D11RenderTargetView *rtv;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps_present;
    ID3D11PixelShader *ps_interp;
    ID3D11InputLayout *layout;
    ID3D11Buffer *vb;
    ID3D11Buffer *cb;
    ID3D11SamplerState *linear;
    ID3D11SamplerState *point;
    ID3D11ShaderResourceView *planes[2][3];
    ID3D11ShaderResourceView *motion;
    unsigned long w;
    unsigned long h;
};

typedef struct {
    float row0[4];
    float row1[4];
    float row2[4];
    float offset[4];
    float luma[4];
    float blocks[4];
} Constants;

static const char *d3d11_vs =
    "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
    "VSOut VSMain(float2 pos : POSITION) {\n"
    "    VSOut o;\n"
    "    o.pos = float4(pos, 0.0, 1.0);\n"
    "    o.uv = float2(pos.x * 0.5 + 0.5, 0.5 - pos.y * 0.5);\n"
    "    return o;\n"
    "}\n";

static const char *d3d11_common =
    "cbuffer Params : register(b0) {\n"
    "    float4 row0; float4 row1; float4 row2;\n"
    "    float4 u_offset; float4 u_luma; float4 u_blocks;\n"
    "};\n"
    "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
    "float4 to_rgb(float3 yuv) {\n"
    "    float3 rgb = float3(dot(row0.xyz, yuv), dot(row1.xyz, yuv),\n"
    "                        dot(row2.xyz, yuv)) + u_offset.xyz;\n"
    "    return float4(saturate(rgb), 1.0);\n"
    "}\n";

static const char *d3d11_ps_present =
    "Texture2D y_tex : register(t0);\n"
    "Texture2D u_tex : register(t1);\n"
    "Texture2D v_tex : register(t2);\n"
    "SamplerState linear_s : register(s0);\n"
    "float4 PSMain(VSOut i) : SV_TARGET {\n"
    "    float3 yuv = float3(y_tex.Sample(linear_s, i.uv).r,\n"
    "                        u_tex.Sample(linear_s, i.uv).r,\n"
    "                        v_tex.Sample(linear_s, i.uv).r);\n"
    "    return to_rgb(yuv);\n"
    "}\n";

static const char *d3d11_ps_interp =
    "Texture2D y0 : register(t0); Texture2D u0 : register(t1);\n"
    "Texture2D v0 : register(t2); Texture2D y1 : register(t3);\n"
    "Texture2D u1 : register(t4); Texture2D v1 : register(t5);\n"
    "Texture2D mv : register(t6);\n"
    "SamplerState linear_s : register(s0);\n"
    "float4 PSMain(VSOut i) : SV_TARGET {\n"
    "    float t = u_luma.z;\n"
    "    int2 b = (int2)floor(i.uv * u_luma.xy / 8.0);\n"
    "    b = clamp(b, int2(0, 0), (int2)(u_blocks.xy) - 1);\n"
    "    float2 px = mv.Load(int3(b, 0)).rg * 255.0 - 128.0;\n"
    "    float2 d = px / u_luma.xy;\n"
    "    float2 uv0 = i.uv + t * d;\n"
    "    float2 uv1 = i.uv - (1.0 - t) * d;\n"
    "    float3 yuv0 = float3(y0.Sample(linear_s, uv0).r,\n"
    "                         u0.Sample(linear_s, uv0).r,\n"
    "                         v0.Sample(linear_s, uv0).r);\n"
    "    float3 yuv1 = float3(y1.Sample(linear_s, uv1).r,\n"
    "                         u1.Sample(linear_s, uv1).r,\n"
    "                         v1.Sample(linear_s, uv1).r);\n"
    "    return to_rgb(lerp(yuv0, yuv1, t));\n"
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
                errors != NULL ? (const char *)errors->lpVtbl->GetBufferPointer(errors) : "");
        if (errors != NULL)
            errors->lpVtbl->Release(errors);
        return -1;
    }
    if (errors != NULL)
        errors->lpVtbl->Release(errors);
    return 0;
}

static ID3D11Texture2D *make_texture(BinkD3DRenderer *r, DXGI_FORMAT format,
                                     unsigned long w, unsigned long h)
{
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D *texture = NULL;

    memset(&desc, 0, sizeof(desc));
    desc.Width = (UINT)w;
    desc.Height = (UINT)h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(r->device->lpVtbl->CreateTexture2D(r->device, &desc, NULL,
                                                  &texture)))
        return NULL;
    return texture;
}

static ID3D11ShaderResourceView *make_view(BinkD3DRenderer *r,
                                           ID3D11Texture2D *texture)
{
    ID3D11ShaderResourceView *view = NULL;

    if (texture == NULL)
        return NULL;
    if (FAILED(r->device->lpVtbl->CreateShaderResourceView(
            r->device, (ID3D11Resource *)texture, NULL, &view)))
        return NULL;
    return view;
}

/* Replace the whole texture from rows `stride` bytes apart. The stride is
 * the source row pitch, so padded decoder planes and tightly packed
 * two-byte motion texels both upload without an intermediate copy. */
static int upload_rows(BinkD3DRenderer *r, ID3D11ShaderResourceView *view,
                       const unsigned char *data, int stride)
{
    ID3D11Resource *resource = NULL;

    if (view == NULL || data == NULL || stride <= 0)
        return -1;
    view->lpVtbl->GetResource(view, &resource);
    if (resource == NULL)
        return -1;
    r->context->lpVtbl->UpdateSubresource(r->context, resource, 0, NULL, data,
                                          (UINT)stride, 0);
    resource->lpVtbl->Release(resource);
    return 0;
}

BinkD3DRenderer *bink_d3d11_renderer_create(void *hwnd, unsigned long w,
                                            unsigned long h)
{
    BinkD3DRenderer *r;
    DXGI_SWAP_CHAIN_DESC swap_desc;
    D3D11_VIEWPORT viewport;
    D3D11_SAMPLER_DESC sampler;
    D3D11_BUFFER_DESC buffer;
    D3D11_INPUT_ELEMENT_DESC element;
    ID3DBlob *vs_blob = NULL;
    ID3DBlob *ps_blob = NULL;
    ID3D11Texture2D *back = NULL;
    static const float quad[12] = {
        -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f,
        -1.0f, 1.0f,  1.0f, -1.0f, 1.0f, 1.0f
    };
    HRESULT hr;

    r = (BinkD3DRenderer *)calloc(1, sizeof(*r));
    if (r == NULL)
        return NULL;
    r->w = w;
    r->h = h;

    {
        D3D_FEATURE_LEVEL levels[3] = { D3D_FEATURE_LEVEL_11_0,
                                        D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0 };
        D3D_FEATURE_LEVEL got;

        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, levels,
                               3, D3D11_SDK_VERSION, &r->device, &got,
                               &r->context);
    }
    if (FAILED(hr)) {
        fprintf(stderr, "D3D11CreateDevice failed: 0x%08lx\n",
                (unsigned long)hr);
        goto fail;
    }

    memset(&swap_desc, 0, sizeof(swap_desc));
    swap_desc.BufferDesc.Width = (UINT)w;
    swap_desc.BufferDesc.Height = (UINT)h;
    swap_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_desc.BufferDesc.RefreshRate.Numerator = 0;
    swap_desc.BufferDesc.RefreshRate.Denominator = 1;
    swap_desc.SampleDesc.Count = 1;
    swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_desc.BufferCount = 1;
    swap_desc.OutputWindow = (HWND)hwnd;
    swap_desc.Windowed = TRUE;
    swap_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    {
        IDXGIDevice *dxgi_device = NULL;
        IDXGIAdapter *adapter = NULL;
        IDXGIFactory *factory = NULL;

        if (FAILED(r->device->lpVtbl->QueryInterface(
                r->device, &IID_IDXGIDevice, (void **)&dxgi_device)) ||
            FAILED(dxgi_device->lpVtbl->GetAdapter(dxgi_device, &adapter)) ||
            FAILED(adapter->lpVtbl->GetParent(adapter, &IID_IDXGIFactory,
                                              (void **)&factory)) ||
            FAILED(factory->lpVtbl->CreateSwapChain(
                factory, (IUnknown *)r->device, &swap_desc, &r->swap))) {
            fprintf(stderr, "Cannot create the D3D11 swap chain.\n");
            if (factory != NULL)
                factory->lpVtbl->Release(factory);
            if (adapter != NULL)
                adapter->lpVtbl->Release(adapter);
            if (dxgi_device != NULL)
                dxgi_device->lpVtbl->Release(dxgi_device);
            goto fail;
        }
        factory->lpVtbl->Release(factory);
        adapter->lpVtbl->Release(adapter);
        dxgi_device->lpVtbl->Release(dxgi_device);
    }

    if (FAILED(r->swap->lpVtbl->GetBuffer(r->swap, 0, &IID_ID3D11Texture2D,
                                          (void **)&back)) ||
        FAILED(r->device->lpVtbl->CreateRenderTargetView(
            r->device, (ID3D11Resource *)back, NULL, &r->rtv))) {
        fprintf(stderr, "Cannot create the D3D11 render target.\n");
        goto fail;
    }
    back->lpVtbl->Release(back);
    back = NULL;

    if (compile(d3d11_vs, "VSMain", "vs_4_0", NULL, &vs_blob) != 0 ||
        compile(d3d11_ps_present, "PSMain", "ps_4_0", d3d11_common,
                &ps_blob) != 0) {
        goto fail;
    }
    if (FAILED(r->device->lpVtbl->CreateVertexShader(
            r->device, vs_blob->lpVtbl->GetBufferPointer(vs_blob),
            vs_blob->lpVtbl->GetBufferSize(vs_blob), NULL, &r->vs)) ||
        FAILED(r->device->lpVtbl->CreatePixelShader(
            r->device, ps_blob->lpVtbl->GetBufferPointer(ps_blob),
            ps_blob->lpVtbl->GetBufferSize(ps_blob), NULL, &r->ps_present))) {
        fprintf(stderr, "Cannot create the D3D11 shaders.\n");
        goto fail;
    }
    ps_blob->lpVtbl->Release(ps_blob);
    ps_blob = NULL;
    if (compile(d3d11_ps_interp, "PSMain", "ps_4_0", d3d11_common, &ps_blob) !=
        0) {
        goto fail;
    }
    if (FAILED(r->device->lpVtbl->CreatePixelShader(
            r->device, ps_blob->lpVtbl->GetBufferPointer(ps_blob),
            ps_blob->lpVtbl->GetBufferSize(ps_blob), NULL, &r->ps_interp))) {
        fprintf(stderr, "Cannot create the interpolation shader.\n");
        goto fail;
    }
    ps_blob->lpVtbl->Release(ps_blob);
    ps_blob = NULL;

    element.SemanticName = "POSITION";
    element.SemanticIndex = 0;
    element.Format = DXGI_FORMAT_R32G32_FLOAT;
    element.InputSlot = 0;
    element.AlignedByteOffset = 0;
    element.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
    element.InstanceDataStepRate = 0;
    if (vs_blob == NULL ||
        FAILED(r->device->lpVtbl->CreateInputLayout(
            r->device, &element, 1, vs_blob->lpVtbl->GetBufferPointer(vs_blob),
            vs_blob->lpVtbl->GetBufferSize(vs_blob), &r->layout))) {
        fprintf(stderr, "Cannot create the D3D11 input layout.\n");
        goto fail;
    }
    vs_blob->lpVtbl->Release(vs_blob);
    vs_blob = NULL;

    memset(&buffer, 0, sizeof(buffer));
    buffer.Usage = D3D11_USAGE_DEFAULT;
    buffer.ByteWidth = (UINT)sizeof(quad);
    buffer.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    {
        D3D11_SUBRESOURCE_DATA init;

        memset(&init, 0, sizeof(init));
        init.pSysMem = quad;
        if (FAILED(r->device->lpVtbl->CreateBuffer(r->device, &buffer, &init,
                                                   &r->vb))) {
            fprintf(stderr, "Cannot create the D3D11 vertex buffer.\n");
            goto fail;
        }
    }
    buffer.ByteWidth = (UINT)sizeof(Constants);
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(r->device->lpVtbl->CreateBuffer(r->device, &buffer, NULL,
                                               &r->cb))) {
        fprintf(stderr, "Cannot create the D3D11 constant buffer.\n");
        goto fail;
    }

    memset(&sampler, 0, sizeof(sampler));
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(r->device->lpVtbl->CreateSamplerState(r->device, &sampler,
                                                     &r->linear)))
        goto fail;
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (FAILED(r->device->lpVtbl->CreateSamplerState(r->device, &sampler,
                                                     &r->point)))
        goto fail;

    {
        int slot;

        for (slot = 0; slot < 2; slot++) {
            ID3D11Texture2D *texture;

            texture = make_texture(r, DXGI_FORMAT_R8_UNORM, w, h);
            r->planes[slot][0] = make_view(r, texture);
            if (texture != NULL)
                texture->lpVtbl->Release(texture);
            texture = make_texture(r, DXGI_FORMAT_R8_UNORM, (w + 1) / 2,
                                   (h + 1) / 2);
            r->planes[slot][1] = make_view(r, texture);
            if (texture != NULL)
                texture->lpVtbl->Release(texture);
            texture = make_texture(r, DXGI_FORMAT_R8_UNORM, (w + 1) / 2,
                                   (h + 1) / 2);
            r->planes[slot][2] = make_view(r, texture);
            if (texture != NULL)
                texture->lpVtbl->Release(texture);
            if (r->planes[slot][0] == NULL || r->planes[slot][1] == NULL ||
                r->planes[slot][2] == NULL) {
                fprintf(stderr, "Cannot create the D3D11 plane textures.\n");
                goto fail;
            }
        }
    }
    {
        ID3D11Texture2D *texture =
            make_texture(r, DXGI_FORMAT_R8G8_UNORM, (w + 7) / 8, (h + 7) / 8);

        r->motion = make_view(r, texture);
        if (texture != NULL)
            texture->lpVtbl->Release(texture);
        if (r->motion == NULL) {
            fprintf(stderr, "Cannot create the D3D11 motion texture.\n");
            goto fail;
        }
    }

    memset(&viewport, 0, sizeof(viewport));
    viewport.Width = (float)w;
    viewport.Height = (float)h;
    viewport.MaxDepth = 1.0f;
    r->context->lpVtbl->RSSetViewports(r->context, 1, &viewport);
    return r;

fail:
    if (vs_blob != NULL)
        vs_blob->lpVtbl->Release(vs_blob);
    if (ps_blob != NULL)
        ps_blob->lpVtbl->Release(ps_blob);
    if (back != NULL)
        back->lpVtbl->Release(back);
    bink_d3d_renderer_destroy(r);
    return NULL;
}

int bink_d3d_renderer_upload(BinkD3DRenderer *r, bink object, int slot)
{
    const unsigned char *y;
    const unsigned char *u;
    const unsigned char *v;

    if (r == NULL || slot < 0 || slot > 1)
        return -1;
    y = bink_get_plane(object, BINK_PLANE_Y);
    u = bink_get_plane(object, BINK_PLANE_U);
    v = bink_get_plane(object, BINK_PLANE_V);
    if (y == NULL || u == NULL || v == NULL)
        return -1;
    if (upload_rows(r, r->planes[slot][0], y,
                    bink_get_stride(object, BINK_PLANE_Y)) != 0 ||
        upload_rows(r, r->planes[slot][1], u,
                    bink_get_stride(object, BINK_PLANE_U)) != 0 ||
        upload_rows(r, r->planes[slot][2], v,
                    bink_get_stride(object, BINK_PLANE_V)) != 0)
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
    for (i = 0; i < bw * bh; i++) {
        texels[i * 2u] = (unsigned char)((int)mx[i] + 128);
        texels[i * 2u + 1u] = (unsigned char)((int)my[i] + 128);
    }
    {
        int rc = upload_rows(r, r->motion, texels, (int)(bw * 2u));

        free(texels);
        if (rc == 0) {
            if (blocks_w != NULL)
                *blocks_w = bw;
            if (blocks_h != NULL)
                *blocks_h = bh;
        }
        return rc;
    }
}

int bink_d3d_renderer_draw(BinkD3DRenderer *r, const BinkD3DDraw *draw)
{
    Constants constants;
    ID3D11ShaderResourceView *views[7];
    ID3D11SamplerState *samplers[2];
    UINT stride = 2 * sizeof(float);
    UINT offset = 0;

    if (r == NULL || draw == NULL)
        return -1;
    memset(&constants, 0, sizeof(constants));
    constants.row0[0] = 1.164f;
    constants.row0[1] = 0.0f;
    constants.row0[2] = 1.596f;
    constants.row1[0] = 1.164f;
    constants.row1[1] = -0.392f;
    constants.row1[2] = -0.813f;
    constants.row2[0] = 1.164f;
    constants.row2[1] = 2.017f;
    constants.row2[2] = 0.0f;
    {
        const float y0 = 16.0f / 255.0f;
        const float c0 = 128.0f / 255.0f;

        constants.offset[0] = -1.164f * y0 - 1.596f * c0;
        constants.offset[1] = -1.164f * y0 + 0.392f * c0 + 0.813f * c0;
        constants.offset[2] = -1.164f * y0 - 2.017f * c0;
    }
    constants.luma[0] = (float)r->w;
    constants.luma[1] = (float)r->h;
    constants.luma[2] = draw->t;
    constants.blocks[0] = (float)(draw->blocks_w > 0 ? draw->blocks_w : 1);
    constants.blocks[1] = (float)(draw->blocks_h > 0 ? draw->blocks_h : 1);

    r->context->lpVtbl->OMSetRenderTargets(r->context, 1, &r->rtv, NULL);
    {
        const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

        r->context->lpVtbl->ClearRenderTargetView(r->context, r->rtv, black);
    }
    r->context->lpVtbl->UpdateSubresource(
        r->context, (ID3D11Resource *)r->cb, 0, NULL, &constants, 0, 0);
    r->context->lpVtbl->IASetInputLayout(r->context, r->layout);
    r->context->lpVtbl->IASetPrimitiveTopology(
        r->context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    r->context->lpVtbl->IASetVertexBuffers(r->context, 0, 1, &r->vb, &stride,
                                           &offset);
    r->context->lpVtbl->VSSetShader(r->context, r->vs, NULL, 0);
    r->context->lpVtbl->VSSetConstantBuffers(r->context, 0, 1, &r->cb);
    r->context->lpVtbl->PSSetConstantBuffers(r->context, 0, 1, &r->cb);
    samplers[0] = r->linear;
    samplers[1] = r->point;
    r->context->lpVtbl->PSSetSamplers(r->context, 0, 2, samplers);

    if (draw->interp && draw->have_next) {
        int cur = draw->slot_cur & 1;
        int next = cur ^ 1;

        r->context->lpVtbl->PSSetShader(r->context, r->ps_interp, NULL, 0);
        views[0] = r->planes[cur][0];
        views[1] = r->planes[cur][1];
        views[2] = r->planes[cur][2];
        views[3] = r->planes[next][0];
        views[4] = r->planes[next][1];
        views[5] = r->planes[next][2];
        views[6] = r->motion;
        r->context->lpVtbl->PSSetShaderResources(r->context, 0, 7, views);
    } else {
        int cur = draw->slot_cur & 1;

        r->context->lpVtbl->PSSetShader(r->context, r->ps_present, NULL, 0);
        views[0] = r->planes[cur][0];
        views[1] = r->planes[cur][1];
        views[2] = r->planes[cur][2];
        r->context->lpVtbl->PSSetShaderResources(r->context, 0, 3, views);
    }

    r->context->lpVtbl->Draw(r->context, 6, 0);
    r->swap->lpVtbl->Present(r->swap, 1, 0);
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
    if (r->linear != NULL)
        r->linear->lpVtbl->Release(r->linear);
    if (r->point != NULL)
        r->point->lpVtbl->Release(r->point);
    if (r->cb != NULL)
        r->cb->lpVtbl->Release(r->cb);
    if (r->vb != NULL)
        r->vb->lpVtbl->Release(r->vb);
    if (r->layout != NULL)
        r->layout->lpVtbl->Release(r->layout);
    if (r->ps_interp != NULL)
        r->ps_interp->lpVtbl->Release(r->ps_interp);
    if (r->ps_present != NULL)
        r->ps_present->lpVtbl->Release(r->ps_present);
    if (r->vs != NULL)
        r->vs->lpVtbl->Release(r->vs);
    if (r->rtv != NULL)
        r->rtv->lpVtbl->Release(r->rtv);
    if (r->swap != NULL)
        r->swap->lpVtbl->Release(r->swap);
    if (r->context != NULL)
        r->context->lpVtbl->Release(r->context);
    if (r->device != NULL)
        r->device->lpVtbl->Release(r->device);
    free(r);
}
