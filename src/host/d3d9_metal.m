/*
 * d3d9_metal.m - Direct3D 9 on Metal: the GPU side of d3d9.c.
 *
 * d3d9.c keeps the device's state as D3D9 has it; at each draw this turns
 * it into Metal's: a render pass for the render targets set (ended and begun
 * again when they change; a full clear is the pass's load action), a
 * pipeline state for the shaders, the vertex declaration, the targets'
 * formats and the blend state (cached by all of them), a depth-stencil and
 * sampler states (cached), and the raster state set on the encoder.
 *
 * Memory: textures are Metal textures, filled from their guest memory when
 * a level was written (Lock marks it); render targets live on the GPU and
 * come back on a lock or GetRenderTargetData. Static vertex and index
 * buffers get a Metal buffer of their own, refilled when locked; dynamic
 * ones, and the vertices of the *UP draws, are copied into this frame's
 * ring, a buffer per frame in flight.
 *
 * Shaders are dxso.c's translations, compiled once each (a ps_1_x once for
 * each kind of texture it is used with). Draws with no vertex or pixel
 * shader (fixed function) are counted and skipped, for now.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32hle.h"
#include "stl.h"
#include "d3d9_int.h"

#define RING_SIZE (64u << 20)
#define FRAMES 3

@interface STLGpu : NSObject {
@public
    id<MTLDevice> dev;
    id<MTLCommandQueue> queue;
    id<MTLCommandBuffer> cmd;
    id<MTLRenderCommandEncoder> enc;
    uint32_t pass_rt[4], pass_ds;              /* the surfaces the open pass renders to */
    int pass_w, pass_h;
    id<MTLBuffer> ring[FRAMES];
    uint32_t ring_used;
    int ring_i;
    dispatch_semaphore_t inflight;
    uint64_t frame, gen;                       /* gen: the ring restarted (a frame, or full) */
    NSMutableDictionary<NSData *, id<MTLRenderPipelineState>> *pipelines;
    NSMutableDictionary<NSData *, id<MTLDepthStencilState>> *depths;
    NSMutableDictionary<NSData *, id<MTLSamplerState>> *samplers;
    NSMutableDictionary<NSNumber *, id<MTLRenderPipelineState>> *blits;
    id<MTLLibrary> blit_lib;
    id<MTLBuffer> zeros;
    /* pending clear, for the next pass */
    uint32_t clear_flags, clear_color, clear_stencil;
    float clear_z;
    uint64_t skipped_ff, skipped_shader, draws;
}
@end
@implementation STLGpu
@end

static STLGpu *G;

/* STL_TRACE_FRAME=N: every draw, clear and StretchRect of frame N, said. */
static int tracing(void) {
    static long want = -2;
    if (want == -2) { const char *e = getenv("STL_TRACE_FRAME"); want = e ? atol(e) : -1; }
    return G && want >= 0 && (long)G->frame == want;
}
#define TRACE(...) do { if (tracing()) fprintf(stderr, "[trace] " __VA_ARGS__); } while (0)

int gpu_available(void) { return MTLCreateSystemDefaultDevice() != nil; }

/* ---- formats -------------------------------------------------------------- */
enum { CV_NONE, CV_565, CV_1555, CV_X1555, CV_4444, CV_888 };
typedef struct { MTLPixelFormat pf; int convert; MTLTextureSwizzleChannels swz; int has_swz; } fmt_t;

#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
static MTLTextureSwizzleChannels SW(MTLTextureSwizzle r, MTLTextureSwizzle g, MTLTextureSwizzle b, MTLTextureSwizzle a) {
    return MTLTextureSwizzleChannelsMake(r, g, b, a);
}
static fmt_t fmt(uint32_t f) {
    fmt_t x = { MTLPixelFormatBGRA8Unorm, CV_NONE, { 0 }, 0 };
    switch (f) {
    case 21: break;                                                          /* A8R8G8B8 */
    case 22: x.has_swz = 1, x.swz = SW(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleOne); break;
    case 20: x.convert = CV_888, x.has_swz = 1, x.swz = SW(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleOne); break;
    case 23: x.convert = CV_565; break;
    case 24: x.convert = CV_X1555; break;
    case 25: x.convert = CV_1555; break;
    case 26: x.convert = CV_4444; break;
    case 28: x.pf = MTLPixelFormatA8Unorm; break;
    case 31: x.pf = MTLPixelFormatRGB10A2Unorm; break;                       /* A2B10G10R10 */
    case 32: x.pf = MTLPixelFormatRGBA8Unorm; break;                         /* A8B8G8R8 */
    case 33: x.pf = MTLPixelFormatRGBA8Unorm, x.has_swz = 1,
             x.swz = SW(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleOne); break;
    case 34: x.pf = MTLPixelFormatRG16Unorm; break;
    case 35: x.pf = MTLPixelFormatBGR10A2Unorm; break;
    case 36: x.pf = MTLPixelFormatRGBA16Unorm; break;
    case 50: x.pf = MTLPixelFormatR8Unorm, x.has_swz = 1,                    /* L8 */
             x.swz = SW(MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleOne); break;
    case 51: x.pf = MTLPixelFormatRG8Unorm, x.has_swz = 1,                   /* A8L8 */
             x.swz = SW(MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleGreen); break;
    case 81: x.pf = MTLPixelFormatR16Unorm, x.has_swz = 1,                   /* L16 */
             x.swz = SW(MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleOne); break;
    case 60: x.pf = MTLPixelFormatRG8Snorm, x.has_swz = 1,                   /* V8U8: (u, v, 1, 1) */
             x.swz = SW(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleOne, MTLTextureSwizzleOne); break;
    case 63: x.pf = MTLPixelFormatRGBA8Snorm; break;                         /* Q8W8V8U8 */
    case 111: x.pf = MTLPixelFormatR16Float; break;
    case 112: x.pf = MTLPixelFormatRG16Float; break;
    case 113: x.pf = MTLPixelFormatRGBA16Float; break;
    case 114: x.pf = MTLPixelFormatR32Float; break;
    case 115: x.pf = MTLPixelFormatRG32Float; break;
    case 116: x.pf = MTLPixelFormatRGBA32Float; break;
    case 75: case 73: case 79: x.pf = MTLPixelFormatDepth32Float_Stencil8; break;   /* D24S8, D15S1, D24X4S4 */
    case 71: case 77: case 80: case 82: x.pf = MTLPixelFormatDepth32Float; break;   /* D32, D24X8, D16, D24FS8 */
    case FOURCC('D', 'X', 'T', '1'): x.pf = MTLPixelFormatBC1_RGBA; break;
    case FOURCC('D', 'X', 'T', '2'): case FOURCC('D', 'X', 'T', '3'): x.pf = MTLPixelFormatBC2_RGBA; break;
    case FOURCC('D', 'X', 'T', '4'): case FOURCC('D', 'X', 'T', '5'): x.pf = MTLPixelFormatBC3_RGBA; break;
    }
    return x;
}
static int is_depth_pf(MTLPixelFormat pf) { return pf == MTLPixelFormatDepth32Float || pf == MTLPixelFormatDepth32Float_Stencil8; }

int gpu_is_render_target(const res_t *r) {
    const res_t *t = r->parent ? OBJ(r->parent, res_t) : r;
    return (t->usage & 3) && t->gpu != NULL;                 /* D3DUSAGE_RENDERTARGET, _DEPTHSTENCIL */
}

/* ---- textures -------------------------------------------------------------- */
static res_t *owner(res_t *r) { return r->parent ? OBJ(r->parent, res_t) : r; }

static id<MTLTexture> make_texture(res_t *r) {
    fmt_t f = fmt(r->format);
    MTLTextureDescriptor *td = [MTLTextureDescriptor new];
    td.pixelFormat = f.pf;
    td.width = r->width ? r->width : 1, td.height = r->height ? r->height : 1;
    td.mipmapLevelCount = r->levels ? r->levels : 1;
    td.textureType = r->kind == R_CUBETEXTURE ? MTLTextureTypeCube : r->kind == R_VOLUMETEXTURE ? MTLTextureType3D
                                                                                                : MTLTextureType2D;
    if (r->kind == R_VOLUMETEXTURE) td.depth = r->depth ? r->depth : 1;
    if (r->kind == R_CUBETEXTURE) td.height = td.width;
    td.usage = MTLTextureUsageShaderRead;
    if ((r->usage & 3) || r->kind == R_SURFACE) td.usage |= MTLTextureUsageRenderTarget;
    td.storageMode = is_depth_pf(f.pf) ? MTLStorageModePrivate : MTLStorageModeShared;
    if (f.has_swz && !(td.usage & MTLTextureUsageRenderTarget)) td.swizzle = f.swz;   /* not on a render target */
    id<MTLTexture> t = [G->dev newTextureWithDescriptor:td];
    if (!t) fprintf(stderr, "[metal] cannot make a %ux%u texture of format %u\n", r->width, r->height, r->format);
    return t;
}

static id<MTLTexture> texture_of(res_t *r) {
    res_t *o = owner(r);
    if (!o->gpu) {
        id<MTLTexture> t = make_texture(o);
        if (!t) return nil;
        o->gpu = (__bridge_retained void *)t;
        /* everything already written, and the levels never written are zero */
        for (int f = 0; f < 6; f++) o->dirty[f] = 0;
        for (int f = 0; f < 6; f++) for (int l = 0; l < 16; l++) if (o->mem[(f * 16 + l) % 96]) o->dirty[f] |= 1u << l;
    }
    return (__bridge id<MTLTexture>)o->gpu;
}

static void convert_row(int cv, const uint8_t *src, uint8_t *dst, uint32_t w) {
    for (uint32_t x = 0; x < w; x++) {
        uint32_t r, g, b, a = 255;
        if (cv == CV_888) { b = src[3 * x], g = src[3 * x + 1], r = src[3 * x + 2]; }
        else {
            uint16_t v = ((const uint16_t *)src)[x];
            if (cv == CV_565) r = (v >> 11) * 255 / 31, g = ((v >> 5) & 63) * 255 / 63, b = (v & 31) * 255 / 31;
            else if (cv == CV_4444) a = (v >> 12) * 17, r = ((v >> 8) & 15) * 17, g = ((v >> 4) & 15) * 17, b = (v & 15) * 17;
            else {
                r = ((v >> 10) & 31) * 255 / 31, g = ((v >> 5) & 31) * 255 / 31, b = (v & 31) * 255 / 31;
                a = cv == CV_1555 ? (v >> 15 ? 255 : 0) : 255;
            }
        }
        dst[4 * x] = (uint8_t)b, dst[4 * x + 1] = (uint8_t)g, dst[4 * x + 2] = (uint8_t)r, dst[4 * x + 3] = (uint8_t)a;
    }
}

/* The levels written since they went up, up. */
static void sync_texture(res_t *r) {
    res_t *o = owner(r);
    id<MTLTexture> t = texture_of(o);
    if (!t || is_depth_pf(t.pixelFormat)) { for (int f = 0; f < 6; f++) o->dirty[f] = 0; return; }
    fmt_t fm = fmt(o->format);
    int faces = o->kind == R_CUBETEXTURE ? 6 : 1;
    for (int f = 0; f < faces; f++) {
        uint32_t bits = o->dirty[f];
        o->dirty[f] = 0;
        for (uint32_t l = 0; bits && l < o->levels; l++, bits >>= 1) {
            if (!(bits & 1) || !o->mem[(f * 16 + l) % 96]) continue;
            uint32_t pitch, slice, mem = level_mem(o, l, (uint32_t)f, &pitch, &slice);
            uint32_t w = level_dim(o->width, l), h = level_dim(o->kind == R_CUBETEXTURE ? o->width : o->height, l);
            uint32_t d = o->kind == R_VOLUMETEXTURE ? level_dim(o->depth, l) : 1;
            const uint8_t *src = (const uint8_t *)hle_ptr(mem);
            MTLRegion rg = o->kind == R_VOLUMETEXTURE ? MTLRegionMake3D(0, 0, 0, w, h, d) : MTLRegionMake2D(0, 0, w, h);
            if (fm.convert) {
                uint8_t *tmp = malloc((size_t)w * 4 * h * d);
                for (uint32_t z = 0; z < d; z++)
                    for (uint32_t y = 0; y < h; y++)
                        convert_row(fm.convert, src + z * slice + y * pitch, tmp + ((size_t)z * h + y) * w * 4, w);
                [t replaceRegion:rg mipmapLevel:l slice:(NSUInteger)f withBytes:tmp bytesPerRow:w * 4 bytesPerImage:(NSUInteger)w * 4 * h];
                free(tmp);
            } else {
                [t replaceRegion:rg mipmapLevel:l slice:(NSUInteger)f withBytes:src bytesPerRow:pitch bytesPerImage:slice];
            }
        }
    }
}

/* ---- command buffers and the ring ---------------------------------------- */
static void end_encoder(void) {
    if (G->enc) { [G->enc endEncoding]; G->enc = nil; }
    memset(G->pass_rt, 0, sizeof G->pass_rt), G->pass_ds = 0;
}
static id<MTLCommandBuffer> command(void) {
    if (!G->cmd) G->cmd = [G->queue commandBuffer];
    return G->cmd;
}
/* Everything so far to the GPU, and wait for it (a readback, a shot). */
static void flush_wait(void) {
    end_encoder();
    if (G->cmd) { [G->cmd commit]; [G->cmd waitUntilCompleted]; G->cmd = nil; }
}

/* n bytes in this frame's ring (16-byte aligned); its offset. */
static uint32_t ring_alloc(uint32_t n, id<MTLBuffer> *buf) {
    n = (n + 255) & ~255u;
    if (G->ring_used + n > RING_SIZE) {
        /* full: everything so far goes, and the ring starts again */
        flush_wait();
        G->ring_used = 0;
        G->gen++;
        if (n > RING_SIZE) { *buf = nil; return 0; }
    }
    uint32_t at = G->ring_used;
    G->ring_used += n;
    *buf = G->ring[G->ring_i];
    return at;
}
static uint32_t ring_copy(const void *src, uint32_t n, id<MTLBuffer> *buf) {
    uint32_t at = ring_alloc(n, buf);
    if (*buf) memcpy((uint8_t *)[*buf contents] + at, src, n);
    return at;
}

/* A vertex or index buffer's bytes on the GPU: its buffer and offset. */
static id<MTLBuffer> buffer_of(res_t *r, uint32_t *offset) {
    *offset = 0;
    if (!(r->usage & 0x200)) {                              /* static: its own buffer, refilled when locked */
        if (!r->gpu) {
            id<MTLBuffer> b = [G->dev newBufferWithLength:r->size ? r->size : 16 options:MTLResourceStorageModeShared];
            r->gpu = (__bridge_retained void *)b;
            r->dirty[0] = 1;
        }
        id<MTLBuffer> b = (__bridge id<MTLBuffer>)r->gpu;
        if (r->dirty[0]) { memcpy([b contents], hle_ptr(r->mem[0]), r->size); r->dirty[0] = 0; }
        return b;
    }
    id<MTLBuffer> b;                                        /* dynamic: copied into the ring when it changed */
    if (r->dirty[0] || r->gpu_frame != G->gen + 1) {
        r->gpu_offset = ring_copy(hle_ptr(r->mem[0]), r->size, &b);
        r->gpu_frame = G->gen + 1;
        r->dirty[0] = 0;
    }
    *offset = r->gpu_offset;
    return G->ring[G->ring_i];
}

/* ---- the device -------------------------------------------------------------- */
static NSString *const BLIT_MSL =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "struct V { float4 pos [[position]]; float2 uv; };\n"
     "vertex V blit_v(uint id [[vertex_id]], constant float4 *r [[buffer(0)]]) {\n"
     "    float2 c = float2(id & 1, id >> 1);\n"
     "    V o; o.pos = float4(mix(r[0].xy, r[0].zw, c), 0, 1); o.uv = mix(r[1].xy, r[1].zw, c); return o; }\n"
     "fragment float4 blit_f(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
     "    return t.sample(s, in.uv); }\n";

void gpu_device(dev_t_ *d) {
    if (!G) {
        G = [STLGpu new];
        G->dev = MTLCreateSystemDefaultDevice();
        if (!G->dev) { fprintf(stderr, "[metal] no Metal device\n"); return; }
        G->queue = [G->dev newCommandQueue];
        for (int i = 0; i < FRAMES; i++) G->ring[i] = [G->dev newBufferWithLength:RING_SIZE options:MTLResourceStorageModeShared];
        G->inflight = dispatch_semaphore_create(FRAMES - 1);
        G->pipelines = [NSMutableDictionary new];
        G->depths = [NSMutableDictionary new];
        G->samplers = [NSMutableDictionary new];
        G->blits = [NSMutableDictionary new];
        G->zeros = [G->dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
        NSError *e = nil;
        G->blit_lib = [G->dev newLibraryWithSource:BLIT_MSL options:nil error:&e];
        if (!G->blit_lib) fprintf(stderr, "[metal] blit shaders: %s\n", e.localizedDescription.UTF8String);
        fprintf(stderr, "[metal] %s\n", G->dev.name.UTF8String);
    }
    end_encoder();
    /* the back buffer and its depth buffer: textures now, at their size */
    res_t *bb = OBJ(d->backbuffer, res_t);
    if (bb->gpu) { CFBridgingRelease(bb->gpu); bb->gpu = NULL; }
    texture_of(bb);
    if (d->depth) {
        res_t *z = OBJ(d->depth, res_t);
        z->width = bb->width, z->height = bb->height;
        if (z->gpu) { CFBridgingRelease(z->gpu); z->gpu = NULL; }
        texture_of(z);
    }
}

/* ---- render passes ---------------------------------------------------------------- */
static MTLClearColor argb(uint32_t c) {
    return MTLClearColorMake(((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0, (c & 255) / 255.0, (c >> 24) / 255.0);
}

/* The open pass, for the render targets now set (begun again if they changed). */
static id<MTLRenderCommandEncoder> pass(dev_t_ *d) {
    if (G->enc && !memcmp(G->pass_rt, d->rt, sizeof G->pass_rt) && G->pass_ds == d->ds && !G->clear_flags) return G->enc;
    end_encoder();
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    int w = 0, h = 0;
    for (int i = 0; i < 4; i++) {
        if (!d->rt[i]) continue;
        res_t *s = OBJ(d->rt[i], res_t);
        id<MTLTexture> t = texture_of(s);
        if (!t) continue;
        sync_texture(s);
        rp.colorAttachments[i].texture = t;
        rp.colorAttachments[i].level = s->parent ? s->level : 0;
        rp.colorAttachments[i].slice = s->parent ? s->face : 0;
        rp.colorAttachments[i].loadAction = (G->clear_flags & 1) ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.colorAttachments[i].clearColor = argb(G->clear_color);
        rp.colorAttachments[i].storeAction = MTLStoreActionStore;
        if (!w) w = (int)level_dim((uint32_t)t.width, s->parent ? s->level : 0), h = (int)level_dim((uint32_t)t.height, s->parent ? s->level : 0);
    }
    if (d->ds) {
        res_t *z = OBJ(d->ds, res_t);
        id<MTLTexture> t = texture_of(z);
        if (t && is_depth_pf(t.pixelFormat)) {
            rp.depthAttachment.texture = t;
            rp.depthAttachment.loadAction = (G->clear_flags & 2) ? MTLLoadActionClear : MTLLoadActionLoad;
            rp.depthAttachment.clearDepth = G->clear_z;
            rp.depthAttachment.storeAction = MTLStoreActionStore;
            if (t.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) {
                rp.stencilAttachment.texture = t;
                rp.stencilAttachment.loadAction = (G->clear_flags & 4) ? MTLLoadActionClear : MTLLoadActionLoad;
                rp.stencilAttachment.clearStencil = G->clear_stencil;
                rp.stencilAttachment.storeAction = MTLStoreActionStore;
            }
            if (!w) w = (int)t.width, h = (int)t.height;
        }
    }
    G->clear_flags = 0;
    G->enc = [command() renderCommandEncoderWithDescriptor:rp];
    memcpy(G->pass_rt, d->rt, sizeof G->pass_rt), G->pass_ds = d->ds;
    G->pass_w = w, G->pass_h = h;
    return G->enc;
}

/* Clear(count, rects, flags, color, z, stencil): the targets' load action,
 * for a clear of all of them (what the game does). */
void gpu_clear(dev_t_ *d, uint32_t nrects, uint32_t rects, uint32_t flags, uint32_t color, float z, uint32_t stencil) {
    if (!G) return;
    TRACE("Clear flags %X color %08X z %g rt %08X ds %08X vp %u,%u %ux%u\n", flags, color, z, d->rt[0], d->ds,
          d->viewport[0], d->viewport[1], d->viewport[2], d->viewport[3]);
    static int told;
    if ((nrects || d->viewport[0] || d->viewport[1]) && !told++)
        fprintf(stderr, "[metal] a Clear of part of the target: cleared whole (not done yet)\n");
    (void)rects;
    end_encoder();
    G->clear_flags = flags & 7, G->clear_color = color, G->clear_z = z, G->clear_stencil = stencil;
    pass(d);                                                /* the pass that clears */
}

/* ---- shaders ------------------------------------------------------------------------ */
static id<MTLFunction> shader(dev_t_ *d, uint32_t obj, int pixel) {
    if (!obj) return nil;
    blob_t *b = OBJ(obj, blob_t);
    if (b->translated < 0) return nil;
    /* ps_1_x and depth textures: the kinds of texture bound make the variant */
    uint32_t key = 0;
    if (pixel) {
        for (int s = 0; s < 8; s++) {
            uint32_t t = d->tex[s];
            if (!t) continue;
            res_t *r = OBJ(t, res_t);
            int kind = r->kind == R_CUBETEXTURE ? 1 : r->kind == R_VOLUMETEXTURE ? 2 : 0;
            if (is_depth_pf(fmt(r->format).pf)) kind = 3;
            key |= (uint32_t)kind << (2 * s);
        }
    }
    for (int i = 0; i < 4; i++) if (b->fn[i] && b->variant_key[i] == key) return (__bridge id<MTLFunction>)b->fn[i];
    dxso_options opt = { { 0 }, 0 };
    for (int s = 0; s < 8; s++) {
        int k = (key >> (2 * s)) & 3;
        opt.sampler_type[s] = (uint8_t)(k == 1 ? DXSO_TEX_CUBE : k == 2 ? DXSO_TEX_3D : DXSO_TEX_2D);
        if (k == 3) opt.sampler_depth |= (uint16_t)(1u << s);
    }
    dxso_shader s;
    if (dxso_translate((const uint32_t *)hle_ptr(b->code), b->len / 4, &opt, &s) != 0) {
        fprintf(stderr, "[metal] a %s shader is not translated: %s\n", pixel ? "pixel" : "vertex", s.error);
        b->translated = -1;
        return nil;
    }
    NSError *e = nil;
    id<MTLLibrary> lib = [G->dev newLibraryWithSource:[NSString stringWithUTF8String:s.msl] options:nil error:&e];
    id<MTLFunction> f = [lib newFunctionWithName:@"dxso_main"];
    if (!f) {
        fprintf(stderr, "[metal] a shader does not compile: %s\n", e.localizedDescription.UTF8String);
        b->translated = -1;
        dxso_free(&s);
        return nil;
    }
    free(b->info.msl);
    b->info = s;
    b->info.msl = NULL;
    free(s.msl);
    b->translated = 1;
    int slot = 0;
    while (slot < 4 && b->fn[slot]) slot++;
    if (slot == 4) { CFBridgingRelease(b->fn[3]); slot = 3; }
    b->fn[slot] = (__bridge_retained void *)f;
    b->variant_key[slot] = key;
    return f;
}

/* ---- vertex layouts ---------------------------------------------------------------- */
typedef struct { uint16_t stream, offset; uint8_t type, method, usage, index; } elem_t;

static int fvf_elements(uint32_t fvf, elem_t *e, uint32_t *stride) {
    int n = 0;
    uint16_t off = 0;
    uint32_t pos = fvf & 0x400E;
    if (pos == 0x4) { e[n++] = (elem_t){ 0, off, 3, 0, 9, 0 }; off += 16; }             /* XYZRHW: POSITIONT */
    else if (pos) {
        e[n++] = (elem_t){ 0, off, 2, 0, 0, 0 }; off += 12;                              /* XYZ */
        int blend = pos == 0x6 ? 1 : pos == 0x8 ? 2 : pos == 0xA ? 3 : pos == 0xC ? 4 : pos == 0xE ? 5 : 0;
        if (pos == 0x4002) { e[n++] = (elem_t){ 0, off, 3, 0, 0, 1 }; off += 16; }        /* XYZW */
        for (int b = 0; b < blend; b++) off += 4;                                         /* blend weights: unused here */
    }
    if (fvf & 0x10) { e[n++] = (elem_t){ 0, off, 2, 0, 3, 0 }; off += 12; }              /* NORMAL */
    if (fvf & 0x20) { e[n++] = (elem_t){ 0, off, 0, 0, 4, 0 }; off += 4; }               /* PSIZE */
    if (fvf & 0x40) { e[n++] = (elem_t){ 0, off, 4, 0, 10, 0 }; off += 4; }              /* DIFFUSE */
    if (fvf & 0x80) { e[n++] = (elem_t){ 0, off, 4, 0, 10, 1 }; off += 4; }              /* SPECULAR */
    int tex = (int)((fvf >> 8) & 0xF);
    for (int i = 0; i < tex; i++) {
        int sz = (int)((fvf >> (16 + 2 * i)) & 3);                                        /* 0: 2, 1: 3, 2: 4, 3: 1 */
        int comps = sz == 0 ? 2 : sz == 1 ? 3 : sz == 2 ? 4 : 1;
        e[n++] = (elem_t){ 0, off, (uint8_t)(comps - 1), 0, 5, (uint8_t)i };
        off += (uint16_t)(4 * comps);
    }
    if (stride) *stride = off;
    return n;
}

static int elements(dev_t_ *d, elem_t *e) {
    if (d->decl) {
        blob_t *b = OBJ(d->decl, blob_t);
        const uint8_t *p = (const uint8_t *)hle_ptr(b->code);
        int n = 0;
        for (uint32_t k = 0; k + 8 <= b->len && n < 64; k += 8) {
            uint16_t stream = *(const uint16_t *)(p + k);
            if (stream == 0xFF) break;
            e[n++] = (elem_t){ stream, *(const uint16_t *)(p + k + 2), p[k + 4], p[k + 5], p[k + 6], p[k + 7] };
        }
        return n;
    }
    return fvf_elements(d->fvf, e, NULL);
}

static MTLVertexFormat vformat(int type) {
    switch (type) {
    case 0: return MTLVertexFormatFloat;
    case 1: return MTLVertexFormatFloat2;
    case 2: return MTLVertexFormatFloat3;
    case 3: return MTLVertexFormatFloat4;
    case 4: return MTLVertexFormatUChar4Normalized_BGRA;     /* D3DCOLOR */
    case 5: return MTLVertexFormatUChar4;
    case 6: return MTLVertexFormatShort2;
    case 7: return MTLVertexFormatShort4;
    case 8: return MTLVertexFormatUChar4Normalized;
    case 9: return MTLVertexFormatShort2Normalized;
    case 10: return MTLVertexFormatShort4Normalized;
    case 11: return MTLVertexFormatUShort2Normalized;
    case 12: return MTLVertexFormatUShort4Normalized;
    case 14: return MTLVertexFormatInt1010102Normalized;
    case 15: return MTLVertexFormatHalf2;
    case 16: return MTLVertexFormatHalf4;
    }
    return MTLVertexFormatFloat4;
}

/* ---- pipeline state ---------------------------------------------------------------- */
static const MTLBlendFactor BF[16] = {
    MTLBlendFactorZero, MTLBlendFactorZero, MTLBlendFactorOne, MTLBlendFactorSourceColor, MTLBlendFactorOneMinusSourceColor,
    MTLBlendFactorSourceAlpha, MTLBlendFactorOneMinusSourceAlpha, MTLBlendFactorDestinationAlpha,
    MTLBlendFactorOneMinusDestinationAlpha, MTLBlendFactorDestinationColor, MTLBlendFactorOneMinusDestinationColor,
    MTLBlendFactorSourceAlphaSaturated, MTLBlendFactorSourceAlpha, MTLBlendFactorOneMinusSourceAlpha,
    MTLBlendFactorBlendColor, MTLBlendFactorOneMinusBlendColor };
static MTLBlendFactor bf(uint32_t b) { return b < 16 ? BF[b] : MTLBlendFactorOne; }
static MTLBlendOperation bop(uint32_t o) {
    switch (o) { case 2: return MTLBlendOperationSubtract; case 3: return MTLBlendOperationReverseSubtract;
                 case 4: return MTLBlendOperationMin; case 5: return MTLBlendOperationMax; }
    return MTLBlendOperationAdd;
}

typedef struct {
    void *vs, *ps;
    elem_t e[32];
    uint32_t ne, strides[16];
    uint32_t vs_usage[16];
    uint32_t color[4], depth, stencil;
    uint32_t blend, src, dst, op, sep, srca, dsta, opa, mask[4];
} pkey_t;

static id<MTLRenderPipelineState> pipeline(dev_t_ *d, id<MTLFunction> vs, id<MTLFunction> ps, blob_t *vsb, const uint32_t *strides) {
    pkey_t k;
    memset(&k, 0, sizeof k);
    k.vs = (__bridge void *)vs, k.ps = (__bridge void *)ps;
    k.ne = (uint32_t)elements(d, k.e);
    memcpy(k.strides, strides, sizeof k.strides);
    for (int i = 0; i < 16; i++) k.vs_usage[i] = (uint32_t)vsb->info.input_usage[i] << 8 | vsb->info.input_index[i];
    for (int i = 0; i < 4; i++)
        if (d->rt[i] && G->enc) {
            id<MTLTexture> t = texture_of(OBJ(d->rt[i], res_t));
            k.color[i] = t ? (uint32_t)t.pixelFormat : 0;
        }
    if (d->ds) {
        id<MTLTexture> t = texture_of(OBJ(d->ds, res_t));
        if (t && is_depth_pf(t.pixelFormat)) {
            k.depth = (uint32_t)t.pixelFormat;
            k.stencil = t.pixelFormat == MTLPixelFormatDepth32Float_Stencil8;
        }
    }
    k.blend = d->rs[27] != 0;
    if (k.blend) {
        k.src = d->rs[19], k.dst = d->rs[20], k.op = d->rs[171];
        k.sep = d->rs[206] != 0;
        if (k.sep) k.srca = d->rs[207], k.dsta = d->rs[208], k.opa = d->rs[209];
    }
    k.mask[0] = d->rs[168] & 15, k.mask[1] = d->rs[190] & 15, k.mask[2] = d->rs[191] & 15, k.mask[3] = d->rs[192] & 15;
    NSData *key = [NSData dataWithBytes:&k length:sizeof k];
    id<MTLRenderPipelineState> st = G->pipelines[key];
    if (st) return st;

    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = vs, pd.fragmentFunction = ps;
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    int need_zero = 0;
    for (int r = 0; r < DXSO_MAX_INPUTS; r++) {
        if (vsb->info.input_usage[r] == 0xFF) continue;
        int found = -1;
        for (uint32_t i = 0; i < k.ne; i++) {
            int u = k.e[i].usage;
            if (u == 9) u = 0;                                  /* POSITIONT feeds a dcl_position */
            if (u == vsb->info.input_usage[r] && k.e[i].index == vsb->info.input_index[r]) { found = (int)i; break; }
        }
        if (found < 0) {                                        /* not in the declaration: zeros */
            vd.attributes[r].format = MTLVertexFormatFloat4, vd.attributes[r].offset = 0, vd.attributes[r].bufferIndex = 7;
            need_zero = 1;
            continue;
        }
        vd.attributes[r].format = vformat(k.e[found].type);
        vd.attributes[r].offset = k.e[found].offset;
        vd.attributes[r].bufferIndex = 8 + k.e[found].stream;
        vd.layouts[8 + k.e[found].stream].stride = strides[k.e[found].stream] ? strides[k.e[found].stream] : 4;
    }
    if (need_zero) {
        vd.layouts[7].stride = 16;
        vd.layouts[7].stepFunction = MTLVertexStepFunctionConstant;
        vd.layouts[7].stepRate = 0;
    }
    pd.vertexDescriptor = vd;
    for (int i = 0; i < 4; i++) {
        if (!k.color[i]) continue;
        MTLRenderPipelineColorAttachmentDescriptor *c = pd.colorAttachments[i];
        c.pixelFormat = (MTLPixelFormat)k.color[i];
        c.writeMask = (k.mask[i] & 1 ? MTLColorWriteMaskRed : 0) | (k.mask[i] & 2 ? MTLColorWriteMaskGreen : 0) |
                      (k.mask[i] & 4 ? MTLColorWriteMaskBlue : 0) | (k.mask[i] & 8 ? MTLColorWriteMaskAlpha : 0);
        if (k.blend) {
            c.blendingEnabled = YES;
            MTLBlendFactor s = bf(k.src), dd = bf(k.dst);
            if (k.src == 12) s = MTLBlendFactorSourceAlpha, dd = MTLBlendFactorOneMinusSourceAlpha;     /* BOTHSRCALPHA */
            if (k.src == 13) s = MTLBlendFactorOneMinusSourceAlpha, dd = MTLBlendFactorSourceAlpha;     /* BOTHINVSRCALPHA */
            c.sourceRGBBlendFactor = s, c.destinationRGBBlendFactor = dd, c.rgbBlendOperation = bop(k.op);
            if (k.sep) c.sourceAlphaBlendFactor = bf(k.srca), c.destinationAlphaBlendFactor = bf(k.dsta), c.alphaBlendOperation = bop(k.opa);
            else c.sourceAlphaBlendFactor = s, c.destinationAlphaBlendFactor = dd, c.alphaBlendOperation = bop(k.op);
        }
    }
    if (k.depth) pd.depthAttachmentPixelFormat = (MTLPixelFormat)k.depth;
    if (k.stencil) pd.stencilAttachmentPixelFormat = (MTLPixelFormat)k.depth;
    NSError *e = nil;
    st = [G->dev newRenderPipelineStateWithDescriptor:pd error:&e];
    if (!st) {
        static int told;
        if (told++ < 10) fprintf(stderr, "[metal] pipeline: %s\n", e.localizedDescription.UTF8String);
        return nil;
    }
    G->pipelines[key] = st;
    return st;
}

static MTLCompareFunction cmpf(uint32_t c) {
    switch (c) { case 1: return MTLCompareFunctionNever; case 2: return MTLCompareFunctionLess;
                 case 3: return MTLCompareFunctionEqual; case 4: return MTLCompareFunctionLessEqual;
                 case 5: return MTLCompareFunctionGreater; case 6: return MTLCompareFunctionNotEqual;
                 case 7: return MTLCompareFunctionGreaterEqual; }
    return MTLCompareFunctionAlways;
}
static MTLStencilOperation sop(uint32_t o) {
    switch (o) { case 2: return MTLStencilOperationZero; case 3: return MTLStencilOperationReplace;
                 case 4: return MTLStencilOperationIncrementClamp; case 5: return MTLStencilOperationDecrementClamp;
                 case 6: return MTLStencilOperationInvert; case 7: return MTLStencilOperationIncrementWrap;
                 case 8: return MTLStencilOperationDecrementWrap; }
    return MTLStencilOperationKeep;
}

static id<MTLDepthStencilState> depth_state(dev_t_ *d) {
    uint32_t k[16] = { 0 };
    if (d->ds) {
        k[0] = d->rs[7] != 0, k[1] = d->rs[14] != 0, k[2] = d->rs[23];
        k[3] = d->rs[52] != 0;
        if (k[3]) {
            k[4] = d->rs[53], k[5] = d->rs[54], k[6] = d->rs[55], k[7] = d->rs[56], k[8] = d->rs[58], k[9] = d->rs[59];
            k[10] = d->rs[185] != 0;
            if (k[10]) k[11] = d->rs[186], k[12] = d->rs[187], k[13] = d->rs[188], k[14] = d->rs[189];
        }
    }
    NSData *key = [NSData dataWithBytes:k length:sizeof k];
    id<MTLDepthStencilState> st = G->depths[key];
    if (st) return st;
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = k[0] ? cmpf(k[2]) : MTLCompareFunctionAlways;
    dd.depthWriteEnabled = k[0] && k[1];
    if (k[3]) {
        MTLStencilDescriptor *f = [MTLStencilDescriptor new];
        f.stencilFailureOperation = sop(k[4]), f.depthFailureOperation = sop(k[5]), f.depthStencilPassOperation = sop(k[6]);
        f.stencilCompareFunction = cmpf(k[7]), f.readMask = k[8], f.writeMask = k[9];
        dd.frontFaceStencil = f;
        if (k[10]) {
            MTLStencilDescriptor *b = [MTLStencilDescriptor new];
            b.stencilFailureOperation = sop(k[11]), b.depthFailureOperation = sop(k[12]), b.depthStencilPassOperation = sop(k[13]);
            b.stencilCompareFunction = cmpf(k[14]), b.readMask = k[8], b.writeMask = k[9];
            dd.backFaceStencil = b;
        } else {
            dd.backFaceStencil = f;
        }
    }
    st = [G->dev newDepthStencilStateWithDescriptor:dd];
    G->depths[key] = st;
    return st;
}

static MTLSamplerAddressMode addr(uint32_t a) {
    switch (a) { case 2: return MTLSamplerAddressModeMirrorRepeat; case 3: return MTLSamplerAddressModeClampToEdge;
                 case 4: return MTLSamplerAddressModeClampToBorderColor; case 5: return MTLSamplerAddressModeMirrorClampToEdge; }
    return MTLSamplerAddressModeRepeat;
}
static id<MTLSamplerState> sampler_state(dev_t_ *d, int s, int compare) {
    uint32_t k[12];
    memcpy(k, &d->samp[s][1], 10 * 4);
    k[10] = (uint32_t)compare, k[11] = 0;
    NSData *key = [NSData dataWithBytes:k length:sizeof k];
    id<MTLSamplerState> st = G->samplers[key];
    if (st) return st;
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.sAddressMode = addr(k[0]), sd.tAddressMode = addr(k[1]), sd.rAddressMode = addr(k[2]);
    uint32_t border = k[3];
    sd.borderColor = (border >> 24) < 128 ? MTLSamplerBorderColorTransparentBlack
                   : (border & 0xFFFFFF) > 0x7F7F7F ? MTLSamplerBorderColorOpaqueWhite : MTLSamplerBorderColorOpaqueBlack;
    sd.magFilter = k[4] >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.minFilter = k[5] >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.mipFilter = k[6] == 0 ? MTLSamplerMipFilterNotMipmapped : k[6] == 1 ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
    if (k[8]) sd.lodMinClamp = (float)k[8];                  /* MAXMIPLEVEL: the largest level used */
    if (k[4] == 3 || k[5] == 3) sd.maxAnisotropy = k[9] > 1 ? (k[9] > 16 ? 16 : k[9]) : 1;
    if (compare) sd.compareFunction = MTLCompareFunctionLessEqual;
    st = [G->dev newSamplerStateWithDescriptor:sd];
    G->samplers[key] = st;
    return st;
}

/* ---- draws -------------------------------------------------------------------------- */
static uint32_t vertices_of(uint32_t prim, uint32_t count) {
    switch (prim) {
    case 1: return count;                                   /* points */
    case 2: return count * 2;                               /* lines */
    case 3: return count + 1;                               /* line strip */
    case 4: return count * 3;                               /* triangles */
    case 5: case 6: return count + 2;                       /* strip, fan */
    }
    return 0;
}
static MTLPrimitiveType mprim(uint32_t prim) {
    switch (prim) {
    case 1: return MTLPrimitiveTypePoint;
    case 2: return MTLPrimitiveTypeLine;
    case 3: return MTLPrimitiveTypeLineStrip;
    case 5: return MTLPrimitiveTypeTriangleStrip;
    }
    return MTLPrimitiveTypeTriangle;                        /* lists, and fans made lists */
}

static float rsf(dev_t_ *d, int i) { float f; memcpy(&f, &d->rs[i], 4); return f; }

void gpu_draw(dev_t_ *d, uint32_t prim, uint32_t count, uint32_t start, int indexed, int32_t base_vertex,
              uint32_t min_index, uint32_t num_vertices, uint32_t up_va, uint32_t up_stride,
              uint32_t up_idx_va, uint32_t up_idx_format) {
    if (!G || !count) return;
    G->draws++;
    TRACE("Draw prim %u count %u %s rt %08X vs %08X ps %08X decl %08X fvf %X tex0 %08X up %u z %u/%u blend %u %u/%u cull %u vp %ux%u\n",
          prim, count, indexed ? "indexed" : "", d->rt[0], d->vs, d->ps, d->decl, d->fvf, d->tex[0], up_va != 0,
          d->rs[7], d->rs[14], d->rs[27], d->rs[19], d->rs[20], d->rs[22], d->viewport[2], d->viewport[3]);
    if (!d->vs || !d->ps) { G->skipped_ff++; return; }     /* fixed function: not yet */
    id<MTLFunction> vs = shader(d, d->vs, 0), ps = shader(d, d->ps, 1);
    if (!vs || !ps) { G->skipped_shader++; return; }
    id<MTLRenderCommandEncoder> enc = pass(d);
    uint32_t strides[16] = { 0 };
    for (int i = 0; i < 16; i++) strides[i] = d->stream[i][2];
    if (up_va) strides[0] = up_stride;
    blob_t *vsb = OBJ(d->vs, blob_t);
    id<MTLRenderPipelineState> pl = pipeline(d, vs, ps, vsb, strides);
    if (!pl) { G->skipped_shader++; return; }
    [enc setRenderPipelineState:pl];
    [enc setDepthStencilState:depth_state(d)];
    [enc setStencilReferenceValue:d->rs[57]];
    uint32_t bfc = d->rs[193];
    [enc setBlendColorRed:((bfc >> 16) & 255) / 255.0f green:((bfc >> 8) & 255) / 255.0f blue:(bfc & 255) / 255.0f alpha:(bfc >> 24) / 255.0f];
    [enc setFrontFacingWinding:MTLWindingClockwise];
    uint32_t cull = d->rs[22];
    [enc setCullMode:cull == 2 ? MTLCullModeFront : cull == 3 ? MTLCullModeBack : MTLCullModeNone];
    [enc setTriangleFillMode:d->rs[8] == 2 ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
    [enc setDepthBias:rsf(d, 195) * 16777216.0f slopeScale:rsf(d, 175) clamp:0];
    /* viewport, scissor: within the pass's target */
    float vx = (float)d->viewport[0], vy = (float)d->viewport[1], vw = (float)d->viewport[2], vh = (float)d->viewport[3];
    float zn, zf;
    memcpy(&zn, &d->viewport[4], 4), memcpy(&zf, &d->viewport[5], 4);
    if (vw <= 0 || vh <= 0) vw = (float)G->pass_w, vh = (float)G->pass_h;
    [enc setViewport:(MTLViewport){ vx, vy, vw, vh, zn, zf }];
    MTLScissorRect sc = { 0, 0, (NSUInteger)G->pass_w, (NSUInteger)G->pass_h };
    if (d->rs[174]) {
        int x0 = (int)d->scissor[0], y0 = (int)d->scissor[1], x1 = (int)d->scissor[2], y1 = (int)d->scissor[3];
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > G->pass_w) x1 = G->pass_w;
        if (y1 > G->pass_h) y1 = G->pass_h;
        if (x1 <= x0 || y1 <= y0) return;
        sc = (MTLScissorRect){ (NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0) };
    }
    [enc setScissorRect:sc];
    /* constants and parameters */
    dxso_params P = { { -1.0f / vw, 1.0f / vh }, (d->rs[24] & 255) / 255.0f, d->rs[15] ? d->rs[25] : 0,
                      { ((d->rs[34] >> 16) & 255) / 255.0f, ((d->rs[34] >> 8) & 255) / 255.0f, (d->rs[34] & 255) / 255.0f, 1.0f },
                      d->rs[28] != 0, { 0 } };
    [enc setVertexBytes:d->vsc length:sizeof d->vsc atIndex:0];
    [enc setVertexBytes:d->vsi length:sizeof d->vsi atIndex:1];
    [enc setVertexBytes:d->vsb length:sizeof d->vsb atIndex:2];
    [enc setVertexBytes:&P length:sizeof P atIndex:3];
    [enc setFragmentBytes:d->psc length:sizeof d->psc atIndex:0];
    [enc setFragmentBytes:d->psi length:sizeof d->psi atIndex:1];
    [enc setFragmentBytes:d->psb length:sizeof d->psb atIndex:2];
    [enc setFragmentBytes:&P length:sizeof P atIndex:3];
    [enc setVertexBuffer:G->zeros offset:0 atIndex:7];
    /* textures */
    blob_t *psb = OBJ(d->ps, blob_t);
    for (int s = 0; s < 16; s++) {
        if (!((psb->info.samplers_used >> s) & 1)) continue;
        id<MTLTexture> t = nil;
        int depth = 0;
        if (d->tex[s]) {
            res_t *r = OBJ(d->tex[s], res_t);
            t = texture_of(r);
            sync_texture(r);
            depth = t && is_depth_pf(t.pixelFormat);
        }
        if (!t) continue;
        [enc setFragmentTexture:t atIndex:(NSUInteger)s];
        [enc setFragmentSamplerState:sampler_state(d, s, depth) atIndex:(NSUInteger)s];
    }
    /* vertices */
    uint32_t nverts = indexed ? min_index + num_vertices : vertices_of(prim, count);
    if (up_va) {
        id<MTLBuffer> b;
        uint32_t at = ring_copy(hle_ptr(up_va), nverts * up_stride, &b);
        if (!b) return;
        [enc setVertexBuffer:b offset:at atIndex:8];
    } else {
        for (int i = 0; i < 16; i++) {
            if (!d->stream[i][0]) continue;
            uint32_t off;
            id<MTLBuffer> b = buffer_of(OBJ(d->stream[i][0], res_t), &off);
            [enc setVertexBuffer:b offset:off + d->stream[i][1] atIndex:8 + (NSUInteger)i];
        }
    }
    MTLPrimitiveType mp = mprim(prim);
    uint32_t n = vertices_of(prim, count);
    if (!indexed && prim != 6) {
        [enc drawPrimitives:mp vertexStart:start vertexCount:n];
        return;
    }
    /* indices: the draw's, or a fan's made a list */
    uint32_t isize = 2;
    const uint8_t *ip = NULL;
    if (indexed) {
        if (up_idx_va) { ip = (const uint8_t *)hle_ptr(up_idx_va); isize = up_idx_format == 102 ? 4 : 2; }
        else if (d->indices) {
            res_t *ib = OBJ(d->indices, res_t);
            ip = (const uint8_t *)hle_ptr(ib->mem[0]);
            isize = ib->format == 102 ? 4 : 2;                /* D3DFMT_INDEX32 */
            ip += start * isize;
        } else return;
    }
    if (prim == 6) {                                        /* a fan: (0, i, i+1) */
        uint32_t tris = count, *list = malloc(tris * 3 * 4);
        for (uint32_t i = 0; i < tris; i++) {
            uint32_t a0 = 0, a1 = i + 1, a2 = i + 2;
            if (ip) {
                a0 = isize == 4 ? ((const uint32_t *)ip)[0] : ((const uint16_t *)ip)[0];
                a1 = isize == 4 ? ((const uint32_t *)ip)[i + 1] : ((const uint16_t *)ip)[i + 1];
                a2 = isize == 4 ? ((const uint32_t *)ip)[i + 2] : ((const uint16_t *)ip)[i + 2];
            } else a0 += start, a1 += start, a2 += start;
            list[3 * i] = a0, list[3 * i + 1] = a1, list[3 * i + 2] = a2;
        }
        id<MTLBuffer> b;
        uint32_t at = ring_copy(list, tris * 12, &b);
        free(list);
        if (!b) return;
        [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:tris * 3 indexType:MTLIndexTypeUInt32 indexBuffer:b
                 indexBufferOffset:at instanceCount:1 baseVertex:base_vertex baseInstance:0];
        return;
    }
    id<MTLBuffer> b;
    uint32_t at = ring_copy(ip, n * isize, &b);
    if (!b) return;
    [enc drawIndexedPrimitives:mp indexCount:n indexType:isize == 4 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16 indexBuffer:b
             indexBufferOffset:at instanceCount:1 baseVertex:base_vertex baseInstance:0];
}

/* ---- StretchRect, readback, present ------------------------------------------------ */
static id<MTLRenderPipelineState> blit_pipeline(MTLPixelFormat pf) {
    id<MTLRenderPipelineState> st = G->blits[@(pf)];
    if (st) return st;
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [G->blit_lib newFunctionWithName:@"blit_v"];
    pd.fragmentFunction = [G->blit_lib newFunctionWithName:@"blit_f"];
    pd.colorAttachments[0].pixelFormat = pf;
    NSError *e = nil;
    st = [G->dev newRenderPipelineStateWithDescriptor:pd error:&e];
    if (st) G->blits[@(pf)] = st;
    return st;
}

void gpu_stretch_rect(dev_t_ *d, uint32_t src, uint32_t src_rect, uint32_t dst, uint32_t dst_rect, uint32_t filter) {
    if (!G || !src || !dst) return;
    TRACE("StretchRect %08X -> %08X\n", src, dst);
    res_t *s = OBJ(src, res_t), *t = OBJ(dst, res_t);
    id<MTLTexture> st = texture_of(s), dt = texture_of(t);
    if (!st || !dt || is_depth_pf(st.pixelFormat) || is_depth_pf(dt.pixelFormat)) return;
    sync_texture(s);
    uint32_t sl = s->parent ? s->level : 0, dl = t->parent ? t->level : 0;
    float sw = (float)level_dim((uint32_t)st.width, sl), sh = (float)level_dim((uint32_t)st.height, sl);
    float dw = (float)level_dim((uint32_t)dt.width, dl), dh = (float)level_dim((uint32_t)dt.height, dl);
    float sr[4] = { 0, 0, sw, sh }, dr[4] = { 0, 0, dw, dh };
    if (src_rect) for (int i = 0; i < 4; i++) sr[i] = (float)(int32_t)MEM32(src_rect + 4 * i);
    if (dst_rect) for (int i = 0; i < 4; i++) dr[i] = (float)(int32_t)MEM32(dst_rect + 4 * i);
    end_encoder();
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = dt, rp.colorAttachments[0].level = dl, rp.colorAttachments[0].slice = t->parent ? t->face : 0;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad, rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [command() renderCommandEncoderWithDescriptor:rp];
    id<MTLRenderPipelineState> pl = blit_pipeline(dt.pixelFormat);
    if (pl) {
        float r[8] = { dr[0] / dw * 2 - 1, 1 - dr[1] / dh * 2, dr[2] / dw * 2 - 1, 1 - dr[3] / dh * 2,
                       sr[0] / sw, sr[1] / sh, sr[2] / sw, sr[3] / sh };
        [enc setRenderPipelineState:pl];
        [enc setVertexBytes:r length:sizeof r atIndex:0];
        id<MTLTexture> view = st;
        if (st.textureType != MTLTextureType2D || st.mipmapLevelCount > 1)
            view = [st newTextureViewWithPixelFormat:st.pixelFormat textureType:MTLTextureType2D
                                              levels:NSMakeRange(sl, 1) slices:NSMakeRange(s->parent ? s->face : 0, 1)];
        [enc setFragmentTexture:view atIndex:0];
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = sd.magFilter = filter >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        [enc setFragmentSamplerState:[G->dev newSamplerStateWithDescriptor:sd] atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    }
    [enc endEncoding];
    (void)d;
}

void gpu_readback(dev_t_ *d, res_t *r, uint32_t level, uint32_t face) {
    (void)d;
    if (!G) return;
    res_t *o = owner(r);
    if (!o->gpu) return;
    id<MTLTexture> t = (__bridge id<MTLTexture>)o->gpu;
    if (is_depth_pf(t.pixelFormat) || fmt(o->format).convert) return;   /* not read back (yet) */
    flush_wait();
    uint32_t pitch, slice, mem = level_mem(o, level, face, &pitch, &slice);
    uint32_t w = level_dim((uint32_t)t.width, level), h = level_dim((uint32_t)t.height, level);
    [t getBytes:hle_ptr(mem) bytesPerRow:pitch bytesPerImage:slice fromRegion:MTLRegionMake2D(0, 0, w, h)
        mipmapLevel:level slice:face];
}

int gpu_shot(dev_t_ *d, const char *path) {
    if (!G) return -1;
    res_t *bb = OBJ(d->backbuffer, res_t);
    if (!bb->gpu) return -1;
    id<MTLTexture> t = (__bridge id<MTLTexture>)bb->gpu;
    flush_wait();
    uint32_t w = (uint32_t)t.width, h = (uint32_t)t.height;
    uint8_t *px = malloc((size_t)w * h * 4);
    [t getBytes:px bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    FILE *f = fopen(path, "wb");
    if (!f) { free(px); return -1; }
    uint32_t row = w * 3 + (4 - (w * 3) % 4) % 4, size = 54 + row * h;
    uint8_t hdr[54] = { 'B', 'M' };
    memcpy(hdr + 2, &size, 4), hdr[10] = 54, hdr[14] = 40;
    memcpy(hdr + 18, &w, 4), memcpy(hdr + 22, &h, 4), hdr[26] = 1, hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    uint8_t *line = calloc(1, row);
    for (int y = (int)h - 1; y >= 0; y--) {
        for (uint32_t x = 0; x < w; x++) memcpy(line + 3 * x, px + ((size_t)y * w + x) * 4, 3);   /* BGRA: BGR */
        fwrite(line, 1, row, f);
    }
    free(line), free(px);
    fclose(f);
    fprintf(stderr, "[metal] %llu draws, %llu fixed-function skipped, %llu without a shader\n",
            (unsigned long long)G->draws, (unsigned long long)G->skipped_ff, (unsigned long long)G->skipped_shader);
    return 0;
}

void gpu_present(dev_t_ *d) {
    (void)d;
    if (!G) return;
    end_encoder();
    id<MTLCommandBuffer> c = command();
    dispatch_semaphore_t sem = G->inflight;
    [c addCompletedHandler:^(id<MTLCommandBuffer> b) { (void)b; dispatch_semaphore_signal(sem); }];
    [c commit];
    G->cmd = nil;
    dispatch_semaphore_wait(G->inflight, DISPATCH_TIME_FOREVER);
    G->frame++;
    G->ring_i = (G->ring_i + 1) % FRAMES, G->ring_used = 0;
    G->gen++;
}
