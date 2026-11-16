/* Metal renderer for the shared SDL player. */

#include "tools/bink_metal_renderer.h"

#ifdef BINK_USE_SDL3
#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>
#else
#include <SDL.h>
#include <SDL_metal.h>
#endif

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bink.h"

struct BinkRenderer {
    SDL_MetalView view;
    CAMetalLayer *layer;
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLRenderPipelineState> pipeline_present;
    id<MTLRenderPipelineState> pipeline_interp;
    id<MTLTexture> planes[2][3];
    id<MTLTexture> motion;
    id<MTLTexture> capture;
    unsigned long w;
    unsigned long h;
};

/* Shared by both fragment shaders; 6 float4 matches the C struct. */
#define BINK_METAL_COMMON                                                     \
    "#include <metal_stdlib>\n"                                               \
    "using namespace metal;\n"                                                \
    "struct VSOut { float4 pos [[position]]; float2 uv; };\n"                 \
    "struct Params {\n"                                                       \
    "    float4 row0; float4 row1; float4 row2;\n"                            \
    "    float4 offset; float4 luma; float4 blocks;\n"                        \
    "};\n"                                                                    \
    "static float3 to_rgb(constant Params &p, float3 yuv) {\n"                \
    "    return float3(dot(p.row0.xyz, yuv), dot(p.row1.xyz, yuv),\n"         \
    "                  dot(p.row2.xyz, yuv)) + p.offset.xyz;\n"              \
    "}\n"

static const char *metal_source =
    BINK_METAL_COMMON
    "vertex VSOut vmain(uint vid [[vertex_id]]) {\n"
    "    float2 p = float2(vid == 1 || vid == 3 ? 1.0 : -1.0,\n"
    "                       vid >= 2 ? 1.0 : -1.0);\n"
    "    VSOut o;\n"
    "    o.pos = float4(p, 0.0, 1.0);\n"
    "    o.uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);\n"
    "    return o;\n"
    "}\n"
    "fragment float4 fpresent(VSOut in [[stage_in]],\n"
    "                         texture2d<float> y [[texture(0)]],\n"
    "                         texture2d<float> u [[texture(1)]],\n"
    "                         texture2d<float> v [[texture(2)]],\n"
    "                         constant Params &p [[buffer(0)]]) {\n"
    "    constexpr sampler s(filter::linear, address::clamp_to_edge);\n"
    "    float3 yuv = float3(y.sample(s, in.uv).r, u.sample(s, in.uv).r,\n"
    "                        v.sample(s, in.uv).r);\n"
    "    return float4(saturate(to_rgb(p, yuv)), 1.0);\n"
    "}\n"
    "fragment float4 finterp(VSOut in [[stage_in]],\n"
    "                        texture2d<float> y0 [[texture(0)]],\n"
    "                        texture2d<float> u0 [[texture(1)]],\n"
    "                        texture2d<float> v0 [[texture(2)]],\n"
    "                        texture2d<float> y1 [[texture(3)]],\n"
    "                        texture2d<float> u1 [[texture(4)]],\n"
    "                        texture2d<float> v1 [[texture(5)]],\n"
    "                        texture2d<float> mv [[texture(6)]],\n"
    "                        constant Params &p [[buffer(0)]]) {\n"
    "    constexpr sampler s(filter::linear, address::clamp_to_edge);\n"
    "    constexpr sampler nearest(filter::nearest, address::clamp_to_edge);\n"
    "    float t = p.luma.z;\n"
    "    uint2 b = uint2(floor(in.uv * p.luma.xy / 8.0));\n"
    "    uint2 lim = uint2(p.blocks.xy) - 1;\n"
    "    b = min(b, lim);\n"
    "    float2 px = mv.read(b).rg * 255.0 - 128.0;\n"
    "    float2 d = px / p.luma.xy;\n"
    "    float2 uv0 = in.uv + t * d;\n"
    "    float2 uv1 = in.uv - (1.0 - t) * d;\n"
    "    float3 a = float3(y0.sample(s, uv0).r, u0.sample(s, uv0).r,\n"
    "                      v0.sample(s, uv0).r);\n"
    "    float3 c = float3(y1.sample(s, uv1).r, u1.sample(s, uv1).r,\n"
    "                      v1.sample(s, uv1).r);\n"
    "    return float4(saturate(to_rgb(p, mix(a, c, t))), 1.0);\n"
    "}\n";

typedef struct {
    float row0[4];
    float row1[4];
    float row2[4];
    float offset[4];
    float luma[4];
    float blocks[4];
} MetalParams;

static id<MTLTexture> make_texture(id<MTLDevice> device,
                                   MTLPixelFormat format, unsigned long w,
                                   unsigned long h)
{
    MTLTextureDescriptor *desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                           width:w
                                                          height:h
                                                       mipmapped:NO];

    desc.usage = MTLTextureUsageShaderRead;
    return [device newTextureWithDescriptor:desc];
}

static int upload_plane(id<MTLTexture> texture, const unsigned char *data,
                        unsigned long w, unsigned long h, int stride)
{
    if (texture == nil || data == NULL)
        return -1;
    [texture replaceRegion:MTLRegionMake2D(0, 0, w, h)
               mipmapLevel:0
                 withBytes:data
               bytesPerRow:(NSUInteger)stride];
    return 0;
}

int bink_metal_prepare(void)
{
    return 0;
}

BinkRenderer *bink_metal_renderer_create(void *window, unsigned long w,
                                         unsigned long h)
{
    BinkRenderer *r = calloc(1, sizeof(*r));
    NSError *error = nil;
    id<MTLLibrary> library;
    id<MTLFunction> vfn;
    id<MTLFunction> fpresent;
    id<MTLFunction> finterp;
    MTLRenderPipelineDescriptor *descriptor;
    int slot;

    if (r == NULL)
        return NULL;
    r->w = w;
    r->h = h;
    r->device = MTLCreateSystemDefaultDevice();
    if (r->device == nil) {
        fprintf(stderr, "No Metal device.\n");
        bink_metal_renderer_destroy(r);
        return NULL;
    }
    r->queue = [r->device newCommandQueue];
    r->view = SDL_Metal_CreateView((SDL_Window *)window);
    if (r->view == NULL) {
        fprintf(stderr, "Cannot create the Metal view: %s\n", SDL_GetError());
        bink_metal_renderer_destroy(r);
        return NULL;
    }
    r->layer = (__bridge CAMetalLayer *)SDL_Metal_GetLayer(r->view);
    r->layer.device = r->device;
    r->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    r->layer.framebufferOnly = YES;
    r->layer.drawableSize = CGSizeMake((CGFloat)w, (CGFloat)h);

    library = [r->device newLibraryWithSource:
                                [NSString stringWithUTF8String:metal_source]
                                     options:nil
                                       error:&error];
    if (library == nil) {
        fprintf(stderr, "Metal shader failed to compile: %s\n",
                error != nil ? [[error localizedDescription] UTF8String]
                             : "unknown");
        bink_metal_renderer_destroy(r);
        return NULL;
    }
    vfn = [library newFunctionWithName:@"vmain"];
    fpresent = [library newFunctionWithName:@"fpresent"];
    finterp = [library newFunctionWithName:@"finterp"];
    if (vfn == nil || fpresent == nil || finterp == nil) {
        fprintf(stderr, "Metal shader functions are missing.\n");
        [vfn release];
        [fpresent release];
        [finterp release];
        [library release];
        bink_metal_renderer_destroy(r);
        return NULL;
    }

    descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = vfn;
    descriptor.fragmentFunction = fpresent;
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    r->pipeline_present =
        [r->device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    descriptor.fragmentFunction = finterp;
    r->pipeline_interp =
        [r->device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    /* This file is built without ARC. The pipelines hold what they need,
     * so the objects that built them are released here on every path. */
    [descriptor release];
    [vfn release];
    [fpresent release];
    [finterp release];
    [library release];
    if (r->pipeline_present == nil || r->pipeline_interp == nil) {
        fprintf(stderr, "Cannot create the Metal pipelines: %s\n",
                error != nil ? [[error localizedDescription] UTF8String]
                             : "unknown");
        bink_metal_renderer_destroy(r);
        return NULL;
    }

    for (slot = 0; slot < 2; slot++) {
        r->planes[slot][0] = make_texture(r->device, MTLPixelFormatR8Unorm, w, h);
        r->planes[slot][1] = make_texture(r->device, MTLPixelFormatR8Unorm,
                                          (w + 1) / 2, (h + 1) / 2);
        r->planes[slot][2] = make_texture(r->device, MTLPixelFormatR8Unorm,
                                          (w + 1) / 2, (h + 1) / 2);
        if (r->planes[slot][0] == nil || r->planes[slot][1] == nil ||
            r->planes[slot][2] == nil) {
            fprintf(stderr, "Cannot create the Metal plane textures.\n");
            bink_metal_renderer_destroy(r);
            return NULL;
        }
    }
    r->motion = make_texture(r->device, MTLPixelFormatRG8Unorm, (w + 7) / 8,
                             (h + 7) / 8);
    if (r->motion == nil) {
        fprintf(stderr, "Cannot create the Metal motion texture.\n");
        bink_metal_renderer_destroy(r);
        return NULL;
    }
    {
        MTLTextureDescriptor *desc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                                      MTLPixelFormatBGRA8Unorm
                                                               width:w
                                                              height:h
                                                           mipmapped:NO];

        desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModeShared;
        r->capture = [r->device newTextureWithDescriptor:desc];
        if (r->capture == nil) {
            fprintf(stderr, "Cannot create the Metal capture texture.\n");
            bink_metal_renderer_destroy(r);
            return NULL;
        }
    }
    return r;
}

int bink_metal_renderer_upload(BinkRenderer *r, bink object, int slot)
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
    if (upload_plane(r->planes[slot][0], y, r->w, r->h,
                     bink_get_stride(object, BINK_PLANE_Y)) != 0 ||
        upload_plane(r->planes[slot][1], u, cw, ch,
                     bink_get_stride(object, BINK_PLANE_U)) != 0 ||
        upload_plane(r->planes[slot][2], v, cw, ch,
                     bink_get_stride(object, BINK_PLANE_V)) != 0)
        return -1;
    return 0;
}

int bink_metal_renderer_upload_motion(BinkRenderer *r, bink object,
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

    if (r == NULL)
        return -1;
    *blocks_w = 0;
    *blocks_h = 0;
    if (!bink_get_motion(object, &bw, &bh, &mx, &my) || bw == 0 || bh == 0)
        return -1;
    texels = (unsigned char *)malloc((size_t)bw * (size_t)bh * 2u);
    if (texels == NULL)
        return -1;
    for (i = 0; i < bw * bh; i++) {
        texels[i * 2u] = (unsigned char)((int)mx[i] + 128);
        texels[i * 2u + 1u] = (unsigned char)((int)my[i] + 128);
    }
    rc = upload_plane(r->motion, texels, bw, bh, (int)(bw * 2u));
    free(texels);
    *blocks_w = bw;
    *blocks_h = bh;
    return rc;
}

static void fill_params(MetalParams *p, const BinkRenderParams *params,
                        unsigned long w, unsigned long h)
{
    memset(p, 0, sizeof(*p));
    p->row0[0] = 1.164f;
    p->row0[2] = 1.596f;
    p->row1[0] = 1.164f;
    p->row1[1] = -0.392f;
    p->row1[2] = -0.813f;
    p->row2[0] = 1.164f;
    p->row2[1] = 2.017f;
    {
        const float y0 = 16.0f / 255.0f;
        const float c0 = 128.0f / 255.0f;

        p->offset[0] = -1.164f * y0 - 1.596f * c0;
        p->offset[1] = -1.164f * y0 + 0.392f * c0 + 0.813f * c0;
        p->offset[2] = -1.164f * y0 - 2.017f * c0;
    }
    p->luma[0] = (float)w;
    p->luma[1] = (float)h;
    p->luma[2] = params->t;
    p->blocks[0] = (float)(params->blocks_w > 0 ? params->blocks_w : 1);
    p->blocks[1] = (float)(params->blocks_h > 0 ? params->blocks_h : 1);
}

static void render_into(BinkRenderer *r, id<MTLTexture> target,
                        const BinkRenderParams *params,
                        id<CAMetalDrawable> drawable)
{
    @autoreleasepool {
        MTLRenderPassDescriptor *pass =
            [MTLRenderPassDescriptor renderPassDescriptor];
        id<MTLCommandBuffer> buffer = [r->queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder;
        MetalParams values;
        int cur = params->slot_cur & 1;
        int next = cur ^ 1;

        pass.colorAttachments[0].texture = target;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        encoder = [buffer renderCommandEncoderWithDescriptor:pass];
        fill_params(&values, params, r->w, r->h);
        [encoder setFragmentBytes:&values length:sizeof(values) atIndex:0];
        if (params->interp && params->have_next) {
            [encoder setRenderPipelineState:r->pipeline_interp];
            [encoder setFragmentTexture:r->planes[cur][0] atIndex:0];
            [encoder setFragmentTexture:r->planes[cur][1] atIndex:1];
            [encoder setFragmentTexture:r->planes[cur][2] atIndex:2];
            [encoder setFragmentTexture:r->planes[next][0] atIndex:3];
            [encoder setFragmentTexture:r->planes[next][1] atIndex:4];
            [encoder setFragmentTexture:r->planes[next][2] atIndex:5];
            [encoder setFragmentTexture:r->motion atIndex:6];
        } else {
            [encoder setRenderPipelineState:r->pipeline_present];
            [encoder setFragmentTexture:r->planes[cur][0] atIndex:0];
            [encoder setFragmentTexture:r->planes[cur][1] atIndex:1];
            [encoder setFragmentTexture:r->planes[cur][2] atIndex:2];
        }
        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0
                    vertexCount:4];
        [encoder endEncoding];
        if (drawable != nil)
            [buffer presentDrawable:drawable];
        [buffer commit];
        if (drawable == nil)
            [buffer waitUntilCompleted];
    }
}

int bink_metal_renderer_draw(BinkRenderer *r, const BinkRenderParams *params)
{
    id<CAMetalDrawable> drawable;

    if (r == NULL || params == NULL)
        return -1;
    if (params->capture) {
        render_into(r, r->capture, params, nil);
        return 0;
    }
    drawable = [r->layer nextDrawable];
    if (drawable == nil)
        return -1;
    render_into(r, drawable.texture, params, drawable);
    return 0;
}

int bink_metal_renderer_readback(BinkRenderer *r, unsigned char *rgb,
                                 unsigned long w, unsigned long h)
{
    size_t bytes;
    unsigned char *pixels;
    unsigned long y;
    unsigned long x;

    if (r == NULL || rgb == NULL || w == 0 || h == 0)
        return -1;
    if (r->capture == nil)
        return -1;
    bytes = (size_t)r->w * (size_t)r->h * 4u;
    pixels = (unsigned char *)malloc(bytes);
    if (pixels == NULL)
        return -1;
    [r->capture getBytes:pixels
             bytesPerRow:(NSUInteger)(r->w * 4u)
              fromRegion:MTLRegionMake2D(0, 0, r->w, r->h)
             mipmapLevel:0];
    for (y = 0; y < h; y++) {
        const unsigned char *row =
            pixels + (size_t)(y * r->h / h) * (size_t)r->w * 4u;

        for (x = 0; x < w; x++) {
            const unsigned char *p = row + (size_t)(x * r->w / w) * 4u;

            rgb[(y * w + x) * 3u + 0] = p[2];
            rgb[(y * w + x) * 3u + 1] = p[1];
            rgb[(y * w + x) * 3u + 2] = p[0];
        }
    }
    free(pixels);
    return 0;
}

void bink_metal_renderer_destroy(BinkRenderer *r)
{
    int slot;
    int plane;

    if (r == NULL)
        return;
    for (slot = 0; slot < 2; slot++)
        for (plane = 0; plane < 3; plane++)
            if (r->planes[slot][plane] != nil)
                [r->planes[slot][plane] release];
    if (r->motion != nil)
        [r->motion release];
    if (r->capture != nil)
        [r->capture release];
    if (r->pipeline_present != nil)
        [r->pipeline_present release];
    if (r->pipeline_interp != nil)
        [r->pipeline_interp release];
    if (r->queue != nil)
        [r->queue release];
    if (r->device != nil)
        [r->device release];
    if (r->view != NULL) {
        SDL_Metal_DestroyView(r->view);
        r->view = NULL;
    }
    free(r);
}
