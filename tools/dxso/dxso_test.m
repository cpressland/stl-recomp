/*
 * dxso_test - D3D9 shader semantics, on the GPU, through dxso.c.
 *
 * Each case is a hand-assembled shader: a vertex case writes its result to
 * oT0 (unclamped), which a ps_2_0 passes to the colour; a pixel case gets
 * its inputs from constants. One triangle covers a 1x1 RGBA32Float target,
 * which is read back and compared with what Direct3D 9 gives.
 *
 *   tools/dxso/build.sh && work/dxso_test
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "dxso.h"

/* ---- an assembler, by the token formats ---- */
#define VS(a, b) (0xFFFE0000u | (a) << 8 | (b))
#define PS(a, b) (0xFFFF0000u | (a) << 8 | (b))
#define END 0x0000FFFFu
#define REG(t, n) (0x80000000u | ((uint32_t)(t) & 7) << 28 | ((uint32_t)(t) & 0x18) << 8 | (uint32_t)(n))
#define D(t, n, m) (REG(t, n) | (uint32_t)(m) << 16)
#define S(t, n, sw, mod) (REG(t, n) | (uint32_t)(sw) << 16 | (uint32_t)(mod) << 24)
#define SR(t, n, sw) (S(t, n, sw, 0) | 0x2000u)            /* relative: a0.x in vs_1_1 */
#define OP(op, len) ((uint32_t)(op) | (uint32_t)(len) << 24)  /* shader model 2: the length */
#define DCL_SM1(usage, idx) 31u, (0x80000000u | (usage) | (idx) << 16)
#define ID 0xE4
#define XXXX 0x00
#define YYYY 0x55
#define ZZZZ 0xAA
#define WWWW 0xFF
#define TEMP 0
#define INPUT 1
#define CONST 2
#define ADDR 3
#define TEX 3
#define RASTOUT 4
#define ATTROUT 5
#define TEXOUT 6
#define COLOROUT 8
#define FULL 0xF

/* vs_1_1 prefix: dcl_position v0; mov oPos, v0 */
#define VS_HEAD VS(1, 1), DCL_SM1(0, 0), D(INPUT, 0, FULL), 1u, D(RASTOUT, 0, FULL), S(INPUT, 0, ID, 0)
/* ... and its end: mov oT0, r0 */
#define VS_OUT_R0 1u, D(TEXOUT, 0, FULL), S(TEMP, 0, ID, 0), END

static const uint32_t ps_pass_t0[] = {       /* ps_2_0: dcl t0; mov oC0, t0 */
    PS(2, 0), OP(31, 2), 0x80000000u, D(TEX, 0, FULL), OP(1, 2), D(COLOROUT, 0, FULL), S(TEX, 0, ID, 0), END };
static const uint32_t ps_pass_v0[] = {       /* ps_2_0: dcl v0; mov oC0, v0 */
    PS(2, 0), OP(31, 2), 0x80000000u, D(INPUT, 0, FULL), OP(1, 2), D(COLOROUT, 0, FULL), S(INPUT, 0, ID, 0), END };
static const uint32_t vs_colour[] = {        /* vs_1_1: oPos = v0, oD0 = c0 */
    VS_HEAD, 1u, D(ATTROUT, 0, FULL), S(CONST, 0, ID, 0), END };

typedef struct {
    const char *name;
    const uint32_t *vs, *ps;
    size_t vs_n, ps_n;
    float vc[16][4], pc[16][4];
    float want[4];
    int discarded;                           /* want the clear colour */
} test_t;

#define N(a) (sizeof(a) / 4)

static id<MTLDevice> g_dev;
static id<MTLCommandQueue> g_q;

static id<MTLFunction> fn(const uint32_t *code, size_t n, const char *what) {
    dxso_shader s;
    if (dxso_translate(code, n, NULL, &s) != 0) { printf("  %s: not translated: %s\n", what, s.error); return nil; }
    NSError *e = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:[NSString stringWithUTF8String:s.msl] options:nil error:&e];
    if (!lib) { printf("  %s: not compiled: %s\n%s\n", what, e.localizedDescription.UTF8String, s.msl); dxso_free(&s); return nil; }
    dxso_free(&s);
    return [lib newFunctionWithName:@"dxso_main"];
}

static int close_to(float a, float b) {
    /* rcp(0): +inf in the reference rasteriser, FLT_MAX with Metal's fast
     * maths; either is a huge positive number, and FLT_MAX keeps a later
     * 0 * rcp(0) a 0 rather than a NaN. */
    if (isinf(b)) return (isinf(a) || fabsf(a) >= 1e38f) && signbit(a) == signbit(b);
    if (b == -FLT_MAX) return a <= -1e38f;
    return fabsf(a - b) <= 1e-5f + 1e-5f * fabsf(b);
}

static int run(const test_t *t) {
    id<MTLFunction> v = fn(t->vs, t->vs_n, "vertex"), f = fn(t->ps, t->ps_n, "pixel");
    if (!v || !f) return 0;
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    vd.attributes[0].format = MTLVertexFormatFloat4, vd.attributes[0].bufferIndex = 8, vd.attributes[0].offset = 0;
    vd.layouts[8].stride = 16;
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = v, pd.fragmentFunction = f, pd.vertexDescriptor = vd;
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    NSError *e = nil;
    id<MTLRenderPipelineState> ps = [g_dev newRenderPipelineStateWithDescriptor:pd error:&e];
    if (!ps) { printf("  pipeline: %s\n", e.localizedDescription.UTF8String); return 0; }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                                  width:1 height:1 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget, td.storageMode = MTLStorageModeShared;
    id<MTLTexture> rt = [g_dev newTextureWithDescriptor:td];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = rt, rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(42, 42, 42, 42);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    static const float tri[12] = { -1, -1, 0.5f, 1, 3, -1, 0.5f, 1, -1, 3, 0.5f, 1 };
    float vc[256][4] = { { 0 } }, pc[224][4] = { { 0 } };
    memcpy(vc, t->vc, sizeof t->vc), memcpy(pc, t->pc, sizeof t->pc);
    int ci[16][4] = { { 0 } };
    uint32_t cb[16] = { 0 };
    dxso_params P = { { 0, 0 }, 0, 0, { 0, 0, 0, 0 }, 0, { 0 } };
    id<MTLCommandBuffer> cmd = [g_q commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:ps];
    [enc setVertexBytes:tri length:sizeof tri atIndex:8];
    [enc setVertexBytes:vc length:sizeof vc atIndex:0];
    [enc setVertexBytes:ci length:sizeof ci atIndex:1];
    [enc setVertexBytes:cb length:sizeof cb atIndex:2];
    [enc setVertexBytes:&P length:sizeof P atIndex:3];
    [enc setFragmentBytes:pc length:sizeof pc atIndex:0];
    [enc setFragmentBytes:ci length:sizeof ci atIndex:1];
    [enc setFragmentBytes:cb length:sizeof cb atIndex:2];
    [enc setFragmentBytes:&P length:sizeof P atIndex:3];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
    [cmd commit];
    [cmd waitUntilCompleted];
    float got[4];
    [rt getBytes:got bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0];
    const float *want = t->want;
    float clear[4] = { 42, 42, 42, 42 };
    if (t->discarded) want = clear;
    int ok = 1;
    for (int i = 0; i < 4; i++) ok &= close_to(got[i], want[i]);
    printf("%s %-36s got (%g, %g, %g, %g)", ok ? "ok  " : "FAIL", t->name, got[0], got[1], got[2], got[3]);
    if (!ok) printf(" want (%g, %g, %g, %g)", want[0], want[1], want[2], want[3]);
    printf("\n");
    return ok;
}

int main(void) {
    @autoreleasepool {
        g_dev = MTLCreateSystemDefaultDevice();
        g_q = [g_dev newCommandQueue];
        if (!g_dev) { printf("no Metal device\n"); return 2; }

        /* rcp of 0 is +inf; rsq takes |x|; log(0) is the most negative float. */
        static const uint32_t vs_scalar[] = { VS_HEAD,
            6u, D(TEMP, 0, 1), S(CONST, 0, XXXX, 0),          /* rcp r0.x, c0.x   (2)   */
            7u, D(TEMP, 0, 2), S(CONST, 0, YYYY, 0),          /* rsq r0.y, c0.y   (-4)  */
            6u, D(TEMP, 0, 4), S(CONST, 0, ZZZZ, 0),          /* rcp r0.z, c0.z   (0)   */
            15u, D(TEMP, 0, 8), S(CONST, 0, ZZZZ, 0),         /* log r0.w, c0.z   (0)   */
            VS_OUT_R0 };
        /* lit: (1, max(x,0), x > 0 ? max(y,0)^w : 0, 1) */
        static const uint32_t vs_lit[] = { VS_HEAD, 16u, D(TEMP, 0, FULL), S(CONST, 1, ID, 0), VS_OUT_R0 };
        /* expp in vs_1_1: (2^floor(x), frac(x), 2^x, 1) */
        static const uint32_t vs_expp[] = { VS_HEAD, 78u, D(TEMP, 0, FULL), S(CONST, 2, XXXX, 0), VS_OUT_R0 };
        /* mov a0.x rounds down in vs_1_1: c[a0.x + 4] with a0.x from 1.7 is c5 */
        static const uint32_t vs_rel[] = { VS_HEAD,
            1u, D(ADDR, 0, 1), S(CONST, 3, XXXX, 0),
            1u, D(TEMP, 0, FULL), SR(CONST, 4, ID),
            VS_OUT_R0 };
        /* m4x4 r0, c12, c8 */
        static const uint32_t vs_m4x4[] = { VS_HEAD, 20u, D(TEMP, 0, FULL), S(CONST, 12, ID, 0), S(CONST, 8, ID, 0), VS_OUT_R0 };
        /* sge, slt, frc, dst into one register */
        static const uint32_t vs_misc[] = { VS_HEAD,
            13u, D(TEMP, 0, 1), S(CONST, 0, XXXX, 0), S(CONST, 0, YYYY, 0),   /* sge 2 >= -4: 1 */
            12u, D(TEMP, 0, 2), S(CONST, 0, XXXX, 0), S(CONST, 0, YYYY, 0),   /* slt 2 < -4: 0 */
            19u, D(TEMP, 0, 4), S(CONST, 2, XXXX, 0),                         /* frc 2.5: 0.5 */
            4u, D(TEMP, 0, 8), S(CONST, 0, XXXX, 1), S(CONST, 0, WWWW, 0), S(CONST, 0, YYYY, 0),  /* mad -2*8 + -4 */
            VS_OUT_R0 };
        /* oD0 is clamped to [0, 1] */
        static const uint32_t vs_clamp[] = { VS_HEAD, 1u, D(ATTROUT, 0, FULL), S(CONST, 4, ID, 0), END };

        /* ps_1_1: mul_x2 r0, v0_bx2, c0 */
        static const uint32_t ps_bx2[] = { PS(1, 1),
            5u, D(TEMP, 0, FULL) | 1u << 24, S(INPUT, 0, ID, 4), S(CONST, 0, ID, 0), END };
        /* ps_1_1: mov r0, v0; cnd r0, r0.a, c1, c2 (r0.a = 1 > 0.5: c1) */
        static const uint32_t ps_cnd[] = { PS(1, 1),
            1u, D(TEMP, 0, FULL), S(INPUT, 0, ID, 0),
            80u, D(TEMP, 0, FULL), S(TEMP, 0, WWWW, 0), S(CONST, 1, ID, 0), S(CONST, 2, ID, 0), END };
        /* ps_1_1: mov_sat r0, c3 */
        static const uint32_t ps_sat[] = { PS(1, 1), 1u, D(TEMP, 0, FULL) | 1u << 20, S(CONST, 3, ID, 0), END };
        /* ps_2_0: cmp r0, c0, c1, c2; mov oC0, r0 */
        static const uint32_t ps_cmp[] = { PS(2, 0),
            OP(88, 4), D(TEMP, 0, FULL), S(CONST, 0, ID, 0), S(CONST, 1, ID, 0), S(CONST, 2, ID, 0),
            OP(1, 2), D(COLOROUT, 0, FULL), S(TEMP, 0, ID, 0), END };
        /* ps_2_0: lrp r0, c0, c1, c2; mov oC0, r0 */
        static const uint32_t ps_lrp[] = { PS(2, 0),
            OP(18, 4), D(TEMP, 0, FULL), S(CONST, 0, ID, 0), S(CONST, 1, ID, 0), S(CONST, 2, ID, 0),
            OP(1, 2), D(COLOROUT, 0, FULL), S(TEMP, 0, ID, 0), END };
        /* ps_2_0: dp2add r0.x, c0, c1, c2.x; nrm r0.yzw?: nrm r1, c3; mov r0.yzw, r1.xxyz */
        static const uint32_t ps_dp2add[] = { PS(2, 0),
            OP(90, 4), D(TEMP, 0, 1), S(CONST, 0, ID, 0), S(CONST, 1, ID, 0), S(CONST, 2, XXXX, 0),
            OP(36, 2), D(TEMP, 1, FULL), S(CONST, 3, ID, 0),
            OP(1, 2), D(TEMP, 0, 0xE), S(TEMP, 1, 0x90, 0),   /* .xxyz: r0.y = r1.x, r0.z = r1.y, r0.w = r1.z */
            OP(1, 2), D(COLOROUT, 0, FULL), S(TEMP, 0, ID, 0), END };
        /* ps_2_0: mov r0, c0; texkill r0 (x < 0): the pixel stays the clear colour */
        static const uint32_t ps_kill[] = { PS(2, 0),
            OP(1, 2), D(TEMP, 0, FULL), S(CONST, 0, ID, 0),
            OP(65, 1), D(TEMP, 0, FULL),
            OP(1, 2), D(COLOROUT, 0, FULL), S(CONST, 1, ID, 0), END };

        test_t tests[] = {
            { "vs rcp, rsq |x|, rcp 0, log 0", vs_scalar, ps_pass_t0, N(vs_scalar), N(ps_pass_t0),
              { { 2, -4, 0, 8 } }, { { 0 } }, { 0.5f, 0.5f, INFINITY, -FLT_MAX }, 0 },
            { "vs lit", vs_lit, ps_pass_t0, N(vs_lit), N(ps_pass_t0),
              { { 0 }, { 0.5f, 0.25f, 0, 2 } }, { { 0 } }, { 1, 0.5f, 0.0625f, 1 }, 0 },
            { "vs expp (vs_1_1: four parts)", vs_expp, ps_pass_t0, N(vs_expp), N(ps_pass_t0),
              { { 0 }, { 0 }, { 2.5f } }, { { 0 } }, { 4, 0.5f, 5.65685425f, 1 }, 0 },
            { "vs mov a0 floor, c[a0.x + 4]", vs_rel, ps_pass_t0, N(vs_rel), N(ps_pass_t0),
              { [3] = { 1.7f }, [5] = { 11, 12, 13, 14 }, [6] = { 21, 22, 23, 24 } }, { { 0 } }, { 11, 12, 13, 14 }, 0 },
            { "vs m4x4", vs_m4x4, ps_pass_t0, N(vs_m4x4), N(ps_pass_t0),
              { [8] = { 1, 2, 3, 4 }, [9] = { 0, 1, 0, 0 }, [10] = { 0, 0, 2, 0 }, [11] = { 1, 1, 1, 1 }, [12] = { 1, 2, 3, 4 } },
              { { 0 } }, { 30, 2, 6, 10 }, 0 },
            { "vs sge, slt, frc, mad of -x", vs_misc, ps_pass_t0, N(vs_misc), N(ps_pass_t0),
              { { 2, -4, 0, 8 }, { 0 }, { 2.5f } }, { { 0 } }, { 1, 0, 0.5f, -20 }, 0 },
            { "vs oD0 clamped to [0, 1]", vs_clamp, ps_pass_v0, N(vs_clamp), N(ps_pass_v0),
              { [4] = { 2, -1, 0.5f, 1 } }, { { 0 } }, { 1, 0, 0.5f, 1 }, 0 },
            { "ps_1_1 mul_x2 of v0_bx2", vs_colour, ps_bx2, N(vs_colour), N(ps_bx2),
              { { 0.25f, 0.5f, 0.75f, 1 } }, { { 1, 1, 1, 1 } }, { -1, 0, 1, 2 }, 0 },
            { "ps_1_1 cnd on r0.a > 0.5", vs_colour, ps_cnd, N(vs_colour), N(ps_cnd),
              { { 0.25f, 0.5f, 0.75f, 1 } }, { { 0 }, { 1, 2, 3, 4 }, { 5, 6, 7, 8 } }, { 1, 2, 3, 4 }, 0 },
            { "ps_1_1 mov_sat", vs_colour, ps_sat, N(vs_colour), N(ps_sat),
              { { 0 } }, { [3] = { -2, 0.25f, 3, 1 } }, { 0, 0.25f, 1, 1 }, 0 },
            { "ps_2_0 cmp >= 0", vs_colour, ps_cmp, N(vs_colour), N(ps_cmp),
              { { 0 } }, { { -1, 0, 2, -3 }, { 1, 2, 3, 4 }, { 5, 6, 7, 8 } }, { 5, 2, 3, 8 }, 0 },
            { "ps_2_0 lrp", vs_colour, ps_lrp, N(vs_colour), N(ps_lrp),
              { { 0 } }, { { 0.25f, 0.5f, 1, 0 }, { 4, 4, 4, 4 }, { 0, 2, 8, 6 } }, { 1, 3, 4, 6 }, 0 },
            { "ps_2_0 dp2add, nrm, swizzled mov", vs_colour, ps_dp2add, N(vs_colour), N(ps_dp2add),
              { { 0 } }, { { 1, 2, 9, 9 }, { 3, 4, 9, 9 }, { 0.5f }, { 0, 3, 4, 7 } }, { 11.5f, 0, 0.6f, 0.8f }, 0 },
            { "ps_2_0 texkill on x < 0", vs_colour, ps_kill, N(vs_colour), N(ps_kill),
              { { 0 } }, { { -1, 1, 1, 1 }, { 1, 1, 1, 1 } }, { 0 }, 1 },
        };
        int n = (int)(sizeof tests / sizeof tests[0]), good = 0;
        for (int i = 0; i < n; i++) good += run(&tests[i]);
        printf("%d of %d\n", good, n);
        return good != n;
    }
}
