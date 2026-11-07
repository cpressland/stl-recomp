/*
 * d3d9.c - Direct3D 9 for the game's LoadLibrary("d3d9.dll") and
 * GetProcAddress("Direct3DCreate9"): a device that takes every call and
 * draws nothing (Phase 3, docs/PLAN.md). Phase 4 puts Metal behind it.
 *
 * What it is already: the interfaces as the SDK lays them out, one adapter
 * with the caps of a Shader Model 3 card of 2006, the desktop's display
 * modes, and a device that keeps what the game and D3DX read back (render
 * and sampler states, transforms, the viewport, textures, streams, render
 * targets, shaders, declarations). Resources have memory behind Lock, so
 * D3DX fills textures and buffers as it would. Shader bytecode is kept, and
 * STL_DUMP_SHADERS=DIR writes each one (vs_<hash>.bin, ps_<hash>.bin): the
 * test set for Phase 4's translator.
 *
 * Every call is counted; at exit a summary lists them, most made first
 * (--verbose prints each call as it is made).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "win32hle.h"
#include "stl.h"
#include "d3d9_int.h"

#define D3D_OK                 0u
#define D3DERR_NOTAVAILABLE    0x8876086Au
#define D3DERR_INVALIDCALL     0x8876086Cu
#define D3DERR_NOTFOUND        0x88760866u
#define E_NOINTERFACE          0x80004002u

/* ---- the call count ------------------------------------------------------- */
#define MAX_METHODS 512
typedef struct { const char *name; uint64_t n; } call_t;
static call_t g_calls[MAX_METHODS];
static int g_ncalls;
static uint64_t g_frames;

static void count(void) {
    const char *name = g_cur_import ? g_cur_import : "?";
    int i = 0;
    for (; i < g_ncalls; i++) if (g_calls[i].name == name) break;
    if (i < g_ncalls) g_calls[i].n++;
    else if (g_ncalls < MAX_METHODS) g_calls[g_ncalls].name = name, g_calls[g_ncalls++].n = 1;
    if (stl_verbose) fprintf(stderr, "[d3d9] %s\n", name);
}

static int by_count(const void *a, const void *b) {
    uint64_t x = ((const call_t *)a)->n, y = ((const call_t *)b)->n;
    return x < y ? 1 : x > y ? -1 : 0;
}

void stl_d3d9_summary(void) {
    if (!g_ncalls) return;
    qsort(g_calls, (size_t)g_ncalls, sizeof g_calls[0], by_count);
    fprintf(stderr, "\n[d3d9] %d methods called, %llu frames presented:\n", g_ncalls, (unsigned long long)g_frames);
    for (int i = 0; i < g_ncalls; i++) fprintf(stderr, "  %10llu  %s\n", (unsigned long long)g_calls[i].n, g_calls[i].name);
}

/* Methods with nothing to do yet: D3D_OK, popping this and n arguments. */
#define OK_N(n) static void ok##n(void) { count(); RET(D3D_OK, n + 1); }
OK_N(0) OK_N(1) OK_N(2) OK_N(3) OK_N(4) OK_N(5) OK_N(6) OK_N(8)

/* ---- objects: the records are in d3d9_int.h ---------------------------------- */

static uint32_t g_vt_d3d, g_vt_dev, g_vt_tex, g_vt_cube, g_vt_vol, g_vt_surf,
                g_vt_vb, g_vt_ib, g_vt_decl, g_vt_vs, g_vt_ps, g_vt_query, g_vt_sb;

static uint32_t make(uint32_t vt, uint32_t rec_size, void **rec) {
    void *r = hle_alloc(rec_size);
    uint32_t o = hle_com_new(vt, 12);
    MEM32(o + 8) = hle_va(r);
    if (rec) *rec = r;
    return o;
}

static void addref(uint32_t o) { if (o) MEM32(o + 4)++; }
static void out_obj(uint32_t pp, uint32_t o) { if (pp) MEM32(pp) = o; }

/* QueryInterface, AddRef, Release for all of them: any interface is the
 * object itself (no aggregation here, and D3DX asks only for what it made). */
static void u_QueryInterface(void) { count(); addref(A32(0)); out_obj(A32(2), A32(0)); RET(D3D_OK, 3); }
static void u_AddRef(void) { RET(++MEM32(A32(0) + 4), 1); }
static void u_Release(void) {
    /* Never freed: a device can still name a released object (a render
     * target set, then released), and nothing here runs out yet. */
    uint32_t o = A32(0), n = MEM32(o + 4) ? --MEM32(o + 4) : 0;
    RET(n, 1);
}
static void u_GetDevice(void) {          /* every record's first word */
    count();
    uint32_t dev = MEM32(MEM32(A32(0) + 8));
    addref(dev);
    out_obj(A32(1), dev);
    RET(D3D_OK, 2);
}
static void u_GetPrivateData(void) { count(); RET(D3DERR_NOTFOUND, 4); }
static void u_SetPriority(void)    { count(); RET(0, 2); }
static void u_GetPriority(void)    { count(); RET(0, 1); }
static void u_GetType(void)        { count(); RET(OBJ(A32(0), res_t)->kind, 1); }

/* ---- formats -------------------------------------------------------------- */
#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define FMT_DXT1 FOURCC('D', 'X', 'T', '1')
#define FMT_DXT2 FOURCC('D', 'X', 'T', '2')
#define FMT_DXT3 FOURCC('D', 'X', 'T', '3')
#define FMT_DXT4 FOURCC('D', 'X', 'T', '4')
#define FMT_DXT5 FOURCC('D', 'X', 'T', '5')

int block_bytes(uint32_t f) {       /* a compressed format's 4x4 block, else 0 */
    return f == FMT_DXT1 ? 8 : (f == FMT_DXT2 || f == FMT_DXT3 || f == FMT_DXT4 || f == FMT_DXT5) ? 16 : 0;
}
int bits_per_pixel(uint32_t f) {
    switch (f) {
    case 20: return 24;                                         /* R8G8B8 */
    case 23: case 24: case 25: case 26: case 29: case 30: case 51: case 60: case 80: case 81: case 111: return 16;
    case 28: case 50: case 41: return 8;                        /* A8 L8 P8 */
    case 113: case 36: case 115: return 64;                     /* A16B16G16R16F A16B16G16R16 G32R32F */
    case 116: return 128;                                       /* A32B32G32R32F */
    default: return 32;                                         /* the 32-bit colour and depth formats */
    }
}
uint32_t level_dim(uint32_t d, uint32_t level) { d >>= level; return d ? d : 1; }
uint32_t pitch_of(uint32_t f, uint32_t w) {
    int b = block_bytes(f);
    return b ? ((w + 3) / 4) * (uint32_t)b : (w * (uint32_t)bits_per_pixel(f) + 7) / 8;
}
uint32_t rows_of(uint32_t f, uint32_t h) { return block_bytes(f) ? (h + 3) / 4 : h; }
static uint32_t full_levels(uint32_t w, uint32_t h, uint32_t d) {
    uint32_t m = w > h ? w : h, n = 1;
    if (d > m) m = d;
    while (m > 1) m >>= 1, n++;
    return n;
}

static int format_ok(uint32_t f) {
    switch (f) {
    case 21: case 22: case 23: case 24: case 25: case 26: case 28: case 29: case 30: case 32: case 35:
    case 50: case 51: case 60: case 63: case 71: case 75: case 77: case 80:
    case 111: case 112: case 113: case 114: case 115: case 116:
    case FMT_DXT1: case FMT_DXT2: case FMT_DXT3: case FMT_DXT4: case FMT_DXT5:
        return 1;
    }
    return 0;
}
static int is_depth(uint32_t f) { return f == 71 || f == 75 || f == 77 || f == 79 || f == 80 || f == 82; }

/* The bytes of one level (and face) of a resource, made on first use. */
uint32_t level_mem(res_t *r, uint32_t level, uint32_t face, uint32_t *pitch, uint32_t *slice) {
    uint32_t w = level_dim(r->width, level), h = level_dim(r->height, level),
             d = r->kind == R_VOLUMETEXTURE ? level_dim(r->depth, level) : 1;
    uint32_t p = pitch_of(r->format, w), s = p * rows_of(r->format, h);
    uint32_t *slot = &r->mem[(face * 16 + level) % (16 * 6)];
    if (!*slot) *slot = hle_va(hle_alloc(s * d ? s * d : 16));
    if (pitch) *pitch = p;
    if (slice) *slice = s;
    return *slot;
}

/* ---- IDirect3D9 ------------------------------------------------------------- */
static void put_f(uint32_t va, float f) { memcpy(hle_ptr(va), &f, 4); }

/* D3DCAPS9 (304 bytes): a GeForce 7800-class Shader Model 3 card. */
static void caps(uint32_t c) {
    memset(hle_ptr(c), 0, 304);
    MEM32(c + 0) = 1;                       /* D3DDEVTYPE_HAL */
    MEM32(c + 8) = 0x00020000u;             /* Caps: READ_SCANLINE */
    MEM32(c + 12) = 0x60020000u;            /* Caps2: DYNAMICTEXTURES, CANAUTOGENMIPMAP, FULLSCREENGAMMA */
    MEM32(c + 16) = 0x000003A0u;            /* Caps3 */
    MEM32(c + 20) = 0x80000003u;            /* PresentationIntervals: IMMEDIATE, ONE, TWO */
    MEM32(c + 24) = 1;                      /* CursorCaps: COLOR */
    MEM32(c + 28) = 0x001BBFD0u;            /* DevCaps: HW T&L, pure device, ... */
    MEM32(c + 32) = 0x001FCEF2u;            /* PrimitiveMiscCaps */
    MEM32(c + 36) = 0x0F7A2191u;            /* RasterCaps: z test, fog, anisotropy, scissor, depth bias */
    MEM32(c + 40) = 0xFF; MEM32(c + 52) = 0xFF;                 /* Z and alpha compare: all */
    MEM32(c + 44) = 0x3FFF; MEM32(c + 48) = 0x3FFF;             /* blend factors: all */
    MEM32(c + 56) = 0x00084208u;            /* ShadeCaps */
    MEM32(c + 60) = 0x0001EC05u;            /* TextureCaps: no POW2 limits; cube, volume, mipmaps */
    MEM32(c + 64) = MEM32(c + 68) = 0x07030700u;               /* filters: point, linear, anisotropic */
    MEM32(c + 72) = 0x03030300u;
    MEM32(c + 76) = MEM32(c + 80) = 0x3F;   /* address modes: all */
    MEM32(c + 84) = 0x1F;                   /* LineCaps */
    MEM32(c + 88) = MEM32(c + 92) = 4096;   /* MaxTextureWidth, Height */
    MEM32(c + 96) = 512;                    /* MaxVolumeExtent */
    MEM32(c + 100) = 8192; MEM32(c + 104) = 4096; MEM32(c + 108) = 16;
    put_f(c + 112, 1e10f);                  /* MaxVertexW */
    put_f(c + 116, -32768.0f); put_f(c + 120, -32768.0f); put_f(c + 124, 32768.0f); put_f(c + 128, 32768.0f);
    MEM32(c + 136) = 0x1FF;                 /* StencilCaps, two-sided */
    MEM32(c + 140) = 0x00100008u;           /* FVFCaps: 8 texture coordinates, point size */
    MEM32(c + 144) = 0x03FFFFFFu;           /* TextureOpCaps: all */
    MEM32(c + 148) = 8; MEM32(c + 152) = 8; /* MaxTextureBlendStages, MaxSimultaneousTextures */
    MEM32(c + 156) = 0x17B;                 /* VertexProcessingCaps */
    MEM32(c + 160) = 8; MEM32(c + 164) = 6; MEM32(c + 168) = 4;
    put_f(c + 176, 64.0f);                  /* MaxPointSize */
    MEM32(c + 180) = 0x000FFFFFu; MEM32(c + 184) = 0x00FFFFFFu; /* MaxPrimitiveCount, MaxVertexIndex */
    MEM32(c + 188) = 16; MEM32(c + 192) = 255;                  /* MaxStreams, MaxStreamStride */
    MEM32(c + 196) = 0xFFFE0300u;           /* vs_3_0 */
    MEM32(c + 200) = 256;                   /* MaxVertexShaderConst */
    MEM32(c + 204) = 0xFFFF0300u;           /* ps_3_0 */
    put_f(c + 208, 65504.0f);               /* PixelShader1xMaxValue */
    MEM32(c + 212) = 0x51;                  /* DevCaps2 */
    MEM32(c + 232) = 1;                     /* NumberOfAdaptersInGroup */
    MEM32(c + 236) = 0x3FF;                 /* DeclTypes */
    MEM32(c + 240) = 4;                     /* NumSimultaneousRTs */
    MEM32(c + 244) = 0x03000300u;           /* StretchRectFilterCaps */
    MEM32(c + 248) = 1; MEM32(c + 252) = 24; MEM32(c + 256) = 32; MEM32(c + 260) = 4;          /* VS20Caps */
    MEM32(c + 264) = 0x1F; MEM32(c + 268) = 24; MEM32(c + 272) = 32; MEM32(c + 276) = 4; MEM32(c + 280) = 512;
    MEM32(c + 284) = 0x03000300u;           /* VertexTextureFilterCaps */
    MEM32(c + 288) = MEM32(c + 292) = 65535;
    MEM32(c + 296) = MEM32(c + 300) = 32768;
}

static int g_modes[HLE_DD_MAX_MODES][2], g_nmodes;
static void modes(void) {
    if (!g_nmodes) g_nmodes = hle_dd_modes(1920, 1080, g_modes);
}

static void d_GetAdapterCount(void) { count(); RET(1, 1); }
static void d_GetAdapterIdentifier(void) {   /* (adapter, flags, D3DADAPTER_IDENTIFIER9*) */
    count();
    uint32_t p = A32(3);
    memset(hle_ptr(p), 0, 1100);
    strcpy((char *)hle_ptr(p), "nv4_disp.dll");
    strcpy((char *)hle_ptr(p + 512), "NVIDIA GeForce 7800 GTX");
    strcpy((char *)hle_ptr(p + 1024), "\\\\.\\DISPLAY1");
    MEM32(p + 1056) = 0x0001000Au; MEM32(p + 1060) = 0x00060009u;   /* a 2006 driver */
    MEM32(p + 1064) = 0x10DE; MEM32(p + 1068) = 0x0091;            /* vendor, device */
    MEM32(p + 1076) = 0xA1;
    MEM32(p + 1096) = 1;                                             /* WHQL */
    RET(D3D_OK, 4);
}
static void d_GetAdapterModeCount(void) {    /* (adapter, format) */
    count();
    modes();
    uint32_t f = A32(2);
    RET(f == 22 || f == 23 ? (uint32_t)g_nmodes : 0, 3);
}
static void put_mode(uint32_t p, uint32_t w, uint32_t h, uint32_t fmt) {
    MEM32(p) = w, MEM32(p + 4) = h, MEM32(p + 8) = 60, MEM32(p + 12) = fmt;
}
static void d_EnumAdapterModes(void) {       /* (adapter, format, mode, D3DDISPLAYMODE*) */
    count();
    modes();
    uint32_t i = A32(3);
    if ((int)i >= g_nmodes) RET(D3DERR_INVALIDCALL, 5);
    put_mode(A32(4), (uint32_t)g_modes[i][0], (uint32_t)g_modes[i][1], A32(2));
    RET(D3D_OK, 5);
}
static void d_GetAdapterDisplayMode(void) {  /* (adapter, D3DDISPLAYMODE*) */
    count();
    put_mode(A32(2), 1920, 1080, 22);
    RET(D3D_OK, 3);
}
static void d_CheckDeviceType(void) {        /* (adapter, type, displayFmt, backFmt, windowed) */
    count();
    uint32_t f = A32(4);
    RET(f == 22 || f == 21 || f == 23 || f == 0 ? D3D_OK : D3DERR_NOTAVAILABLE, 6);
}
static void d_CheckDeviceFormat(void) {      /* (adapter, type, adapterFmt, usage, rtype, checkFmt) */
    count();
    uint32_t usage = A32(4), f = A32(6);
    int ok = format_ok(f);
    if (usage & 2) ok = ok && is_depth(f);                     /* D3DUSAGE_DEPTHSTENCIL */
    if (!ok) {                                                 /* said once a format */
        static uint32_t told[64];
        int k = 0;
        while (k < 64 && told[k] && told[k] != f) k++;
        if (k < 64 && !told[k]) {
            told[k] = f;
            fprintf(stderr, "[d3d9] CheckDeviceFormat: no format %u (0x%08X), usage 0x%X\n", f, f, usage);
        }
    }
    RET(ok ? D3D_OK : D3DERR_NOTAVAILABLE, 7);
}
static void d_CheckDeviceMultiSampleType(void) {   /* (adapter, type, fmt, windowed, msType, *quality) */
    count();
    if (A32(6)) MEM32(A32(6)) = 1;
    RET(A32(5) == 0 ? D3D_OK : D3DERR_NOTAVAILABLE, 7);
}
static void d_GetDeviceCaps(void) { count(); caps(A32(3)); RET(D3D_OK, 4); }   /* (adapter, type, caps) */
static void d_GetAdapterMonitor(void) { count(); RET(0x00010001u, 2); }

static uint32_t new_surface(uint32_t device, uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage, uint32_t pool) {
    res_t *r;
    uint32_t o = make(g_vt_surf, sizeof(res_t), (void **)&r);
    r->kind = R_SURFACE, r->device = device, r->width = w, r->height = h, r->depth = 1, r->levels = 1;
    r->format = fmt, r->usage = usage, r->pool = pool;
    return o;
}

/* The state a device starts with, and Reset puts back: D3D9's defaults. */
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void defaults(dev_t_ *d) {
    memset(d->rs, 0, sizeof d->rs);
    uint32_t *rs = d->rs;
    rs[7] = d->params[9] ? 1 : 0;                       /* ZENABLE: with an automatic depth buffer */
    rs[8] = 3, rs[9] = 2, rs[14] = 1, rs[16] = 1;      /* FILLMODE solid, SHADEMODE gouraud, ZWRITEENABLE, LASTPIXEL */
    rs[19] = 2, rs[20] = 1, rs[22] = 3, rs[23] = 4;    /* SRCBLEND one, DESTBLEND zero, CULLMODE ccw, ZFUNC lessequal */
    rs[25] = 8;                                        /* ALPHAFUNC always */
    rs[36] = f2u(0.0f), rs[37] = f2u(1.0f), rs[38] = f2u(1.0f);   /* FOGSTART, FOGEND, FOGDENSITY */
    rs[53] = rs[54] = rs[55] = 1, rs[56] = 8;          /* stencil keep, keep, keep, always */
    rs[58] = rs[59] = 0xFFFFFFFFu;                     /* STENCILMASK, STENCILWRITEMASK */
    rs[60] = 0xFFFFFFFFu;                              /* TEXTUREFACTOR */
    rs[136] = 1, rs[137] = 1;                          /* CLIPPING, LIGHTING */
    rs[141] = 1, rs[142] = 1;                          /* COLORVERTEX, LOCALVIEWER */
    rs[145] = 1, rs[146] = 2;                          /* DIFFUSEMATERIALSOURCE color1, SPECULAR color2 */
    rs[154] = f2u(1.0f), rs[155] = f2u(1.0f), rs[166] = f2u(64.0f);   /* POINTSIZE, _MIN, _MAX */
    rs[161] = 1, rs[162] = 0xFFFFFFFFu;                /* MULTISAMPLEANTIALIAS, MULTISAMPLEMASK */
    rs[168] = 0xF, rs[190] = 0xF, rs[191] = 0xF, rs[192] = 0xF;   /* COLORWRITEENABLE 0-3 */
    rs[171] = 1;                                       /* BLENDOP add */
    rs[186] = rs[187] = rs[188] = 1, rs[189] = 8;      /* the CCW stencil: keep, keep, keep, always */
    rs[193] = 0xFFFFFFFFu;                             /* BLENDFACTOR */
    rs[207] = 2, rs[208] = 1, rs[209] = 1;             /* SRCBLENDALPHA one, DESTBLENDALPHA zero, BLENDOPALPHA add */
    memset(d->tss, 0, sizeof d->tss);
    for (int i = 0; i < MAX_STAGES; i++) {
        d->tss[i][1] = i == 0 ? 4 : 1;                 /* COLOROP: modulate on stage 0, disable after */
        d->tss[i][2] = 2, d->tss[i][3] = 1;            /* COLORARG1 texture, COLORARG2 current */
        d->tss[i][4] = i == 0 ? 2 : 1;                 /* ALPHAOP: selectarg1 on stage 0 */
        d->tss[i][5] = 2, d->tss[i][6] = 1;            /* ALPHAARG1 texture, ALPHAARG2 current */
        d->tss[i][11] = (uint32_t)i;                   /* TEXCOORDINDEX */
        d->tss[i][26] = d->tss[i][27] = d->tss[i][28] = 1;   /* COLORARG0, ALPHAARG0, RESULTARG: current */
    }
    memset(d->samp, 0, sizeof d->samp);
    for (int i = 0; i < SAMP_SLOTS; i++) {
        d->samp[i][1] = d->samp[i][2] = d->samp[i][3] = 1;   /* ADDRESSU/V/W wrap */
        d->samp[i][5] = d->samp[i][6] = 1;                   /* MAG/MINFILTER point; MIPFILTER none */
        d->samp[i][10] = 1;                                  /* MAXANISOTROPY */
    }
    d->vs = d->ps = d->decl = d->fvf = d->indices = 0;
    memset(d->stream, 0, sizeof d->stream), memset(d->tex, 0, sizeof d->tex);
    for (int i = 0; i < 16; i++) d->stream_freq[i] = 1;
    d->rt[0] = d->backbuffer, d->rt[1] = d->rt[2] = d->rt[3] = 0, d->ds = d->depth;
    d->viewport[0] = d->viewport[1] = 0, d->viewport[2] = d->params[0], d->viewport[3] = d->params[1];
    float zero = 0.0f, one = 1.0f;
    memcpy(&d->viewport[4], &zero, 4), memcpy(&d->viewport[5], &one, 4);
    d->scissor[0] = d->scissor[1] = 0, d->scissor[2] = d->params[0], d->scissor[3] = d->params[1];
}

/* CreateDevice(adapter, type, hwnd, flags, D3DPRESENT_PARAMETERS*, &device) */
static void d_CreateDevice(void) {
    count();
    dev_t_ *d;
    uint32_t pp = A32(5);
    uint32_t o = make(g_vt_dev, sizeof(dev_t_), (void **)&d);
    d->d3d = A32(0), d->adapter = A32(1), d->type = A32(2), d->hwnd = A32(3), d->flags = A32(4);
    addref(d->d3d);
    memcpy(d->params, hle_ptr(pp), sizeof d->params);
    /* A zero size is the window's (here: the display's). */
    if (!d->params[0] || !d->params[1]) d->params[0] = 1920, d->params[1] = 1080;
    if (!d->params[2]) d->params[2] = 22;                     /* D3DFMT_UNKNOWN: the display's */
    memcpy(hle_ptr(pp), d->params, 12);
    fprintf(stderr, "[d3d9] CreateDevice: %ux%u, format %u, %s, behaviour 0x%X\n", d->params[0], d->params[1],
            d->params[2], d->params[8] ? "windowed" : "full screen", d->flags);
    d->backbuffer = new_surface(o, d->params[0], d->params[1], d->params[2], 1, 0);
    d->rt[0] = d->backbuffer;
    if (d->params[9]) {                                       /* EnableAutoDepthStencil */
        d->depth = new_surface(o, d->params[0], d->params[1], d->params[10], 2, 0);
        d->ds = d->depth;
    }
    d->viewport[2] = d->params[0], d->viewport[3] = d->params[1];
    float one = 1.0f;
    memcpy(&d->viewport[5], &one, 4);
    for (int i = 0; i < 512; i++) d->transform[i][0] = d->transform[i][5] = d->transform[i][10] = d->transform[i][15] = 1;
    defaults(d);
    gpu_device(d);
    out_obj(A32(6), o);
    RET(D3D_OK, 7);
}

/* ---- IDirect3DDevice9 --------------------------------------------------------- */
#define DEV(o) OBJ(o, dev_t_)
#define RES(o) OBJ(o, res_t)

static void v_TestCooperativeLevel(void) { count(); RET(D3D_OK, 1); }
static void v_GetAvailableTextureMem(void) { count(); RET(512u << 20, 1); }
static void v_GetDirect3D(void) { count(); uint32_t d3d = DEV(A32(0))->d3d; addref(d3d); out_obj(A32(1), d3d); RET(D3D_OK, 2); }
static void v_GetDeviceCaps(void) { count(); caps(A32(1)); RET(D3D_OK, 2); }
static void v_GetDisplayMode(void) {   /* (swapchain, mode) */
    count();
    dev_t_ *d = DEV(A32(0));
    put_mode(A32(2), d->params[0], d->params[1], d->params[2]);
    RET(D3D_OK, 3);
}
static void v_GetCreationParameters(void) {
    count();
    dev_t_ *d = DEV(A32(0));
    uint32_t p = A32(1);
    MEM32(p) = d->adapter, MEM32(p + 4) = d->type, MEM32(p + 8) = d->hwnd, MEM32(p + 12) = d->flags;
    RET(D3D_OK, 2);
}
static void v_ShowCursor(void) { count(); RET(0, 2); }
static void v_GetNumberOfSwapChains(void) { count(); RET(1, 1); }
static void v_Reset(void) {
    count();
    dev_t_ *d = DEV(A32(0));
    memcpy(d->params, hle_ptr(A32(1)), sizeof d->params);
    if (!d->params[0] || !d->params[1]) d->params[0] = 1920, d->params[1] = 1080;
    res_t *bb = OBJ(d->backbuffer, res_t);
    bb->width = d->params[0], bb->height = d->params[1];
    memset(bb->mem, 0, sizeof bb->mem);
    if (d->depth) { res_t *z = OBJ(d->depth, res_t); z->width = d->params[0], z->height = d->params[1]; }
    defaults(d);
    gpu_device(d);
    fprintf(stderr, "[d3d9] Reset: %ux%u\n", d->params[0], d->params[1]);
    RET(D3D_OK, 2);
}
/* STL_SHOT=DIR writes the back buffer there as frame_<n>.bmp every
 * STL_SHOT_EVERY frames (default 300), just before it is presented. */
static void shots(dev_t_ *d) {
    static const char *dir;
    static uint64_t every;
    static int checked;
    if (!checked) {
        checked = 1;
        dir = getenv("STL_SHOT");
        const char *e = getenv("STL_SHOT_EVERY");
        every = e ? strtoull(e, NULL, 0) : 300;
        if (dir) mkdir(dir, 0755);
    }
    if (!dir || !every || g_frames % every) return;
    char path[4096];
    snprintf(path, sizeof path, "%s/frame_%06llu.bmp", dir, (unsigned long long)g_frames);
    if (gpu_shot(d, path) == 0) fprintf(stderr, "[d3d9] shot %s\n", path);
}
static void v_Present(void) {
    count();
    dev_t_ *d = DEV(A32(0));
    ++g_frames;
    shots(d);
    gpu_present(d);
    if (g_frames % 600 == 0) fprintf(stderr, "[d3d9] %llu frames\n", (unsigned long long)g_frames);
    RET(D3D_OK, 5);
}
static void v_GetBackBuffer(void) {   /* (swapchain, index, type, &surface) */
    count();
    uint32_t bb = DEV(A32(0))->backbuffer;
    addref(bb);
    out_obj(A32(4), bb);
    RET(D3D_OK, 5);
}
static void v_GetRasterStatus(void) { count(); memset(hle_ptr(A32(2)), 0, 8); RET(D3D_OK, 3); }
static void v_SetGammaRamp(void) { count(); RET(0, 4); }
static void v_GetGammaRamp(void) {
    count();
    uint16_t *r = (uint16_t *)hle_ptr(A32(2));
    for (int c = 0; c < 3; c++) for (int i = 0; i < 256; i++) r[c * 256 + i] = (uint16_t)(i * 257);
    RET(0, 3);
}

static uint32_t new_texture(uint32_t vt, uint32_t kind, uint32_t dev, uint32_t w, uint32_t h, uint32_t depth,
                            uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool) {
    res_t *r;
    uint32_t o = make(vt, sizeof(res_t), (void **)&r);
    r->kind = kind, r->device = dev, r->width = w, r->height = h, r->depth = depth;
    r->levels = levels ? levels : full_levels(w, h, kind == R_VOLUMETEXTURE ? depth : 1);
    if (usage & 0x400) r->levels = 1;            /* AUTOGENMIPMAP: one level the application sees */
    if (r->levels > 16) r->levels = 16;
    r->format = fmt, r->usage = usage, r->pool = pool;
    return o;
}
static void v_CreateTexture(void) {   /* (w, h, levels, usage, format, pool, &tex, shared) */
    count();
    out_obj(A32(7), new_texture(g_vt_tex, R_TEXTURE, A32(0), A32(1), A32(2), 1, A32(3), A32(4), A32(5), A32(6)));
    RET(D3D_OK, 9);
}
static void v_CreateVolumeTexture(void) {   /* (w, h, d, levels, usage, format, pool, &tex, shared) */
    count();
    out_obj(A32(8), new_texture(g_vt_vol, R_VOLUMETEXTURE, A32(0), A32(1), A32(2), A32(3), A32(4), A32(5), A32(6), A32(7)));
    RET(D3D_OK, 10);
}
static void v_CreateCubeTexture(void) {   /* (edge, levels, usage, format, pool, &tex, shared) */
    count();
    out_obj(A32(6), new_texture(g_vt_cube, R_CUBETEXTURE, A32(0), A32(1), A32(1), 1, A32(2), A32(3), A32(4), A32(5)));
    RET(D3D_OK, 8);
}
static uint32_t new_buffer(uint32_t vt, uint32_t kind, uint32_t dev, uint32_t len, uint32_t usage, uint32_t fmt_or_fvf,
                           uint32_t pool) {
    res_t *r;
    uint32_t o = make(vt, sizeof(res_t), (void **)&r);
    r->kind = kind, r->device = dev, r->size = len, r->usage = usage, r->pool = pool;
    r->width = len, r->height = r->depth = r->levels = 1;
    if (kind == R_VERTEXBUFFER) r->fvf = fmt_or_fvf, r->format = 100;   /* D3DFMT_VERTEXDATA */
    else r->format = fmt_or_fvf;
    r->mem[0] = hle_va(hle_alloc(len ? len : 16));
    return o;
}
static void v_CreateVertexBuffer(void) {   /* (len, usage, fvf, pool, &vb, shared) */
    count();
    out_obj(A32(5), new_buffer(g_vt_vb, R_VERTEXBUFFER, A32(0), A32(1), A32(2), A32(3), A32(4)));
    RET(D3D_OK, 7);
}
static void v_CreateIndexBuffer(void) {   /* (len, usage, format, pool, &ib, shared) */
    count();
    out_obj(A32(5), new_buffer(g_vt_ib, R_INDEXBUFFER, A32(0), A32(1), A32(2), A32(3), A32(4)));
    RET(D3D_OK, 7);
}
static void v_CreateRenderTarget(void) {   /* (w, h, fmt, ms, msq, lockable, &surf, shared) */
    count();
    uint32_t s = new_surface(A32(0), A32(1), A32(2), A32(3), 1, 0);
    OBJ(s, res_t)->multisample = A32(4);
    out_obj(A32(7), s);
    RET(D3D_OK, 9);
}
static void v_CreateDepthStencilSurface(void) {   /* (w, h, fmt, ms, msq, discard, &surf, shared) */
    count();
    out_obj(A32(7), new_surface(A32(0), A32(1), A32(2), A32(3), 2, 0));
    RET(D3D_OK, 9);
}
static void v_CreateOffscreenPlainSurface(void) {   /* (w, h, fmt, pool, &surf, shared) */
    count();
    out_obj(A32(5), new_surface(A32(0), A32(1), A32(2), A32(3), 0, A32(4)));
    RET(D3D_OK, 7);
}
static void v_SetRenderTarget(void) {   /* (index, surface) */
    count();
    uint32_t i = A32(1);
    if (i < 4) DEV(A32(0))->rt[i] = A32(2);
    RET(i < 4 ? D3D_OK : D3DERR_INVALIDCALL, 3);
}
static void v_GetRenderTarget(void) {
    count();
    uint32_t i = A32(1), s = i < 4 ? DEV(A32(0))->rt[i] : 0;
    addref(s);
    out_obj(A32(2), s);
    RET(s ? D3D_OK : D3DERR_NOTFOUND, 3);
}
static void v_SetDepthStencilSurface(void) { count(); DEV(A32(0))->ds = A32(1); RET(D3D_OK, 2); }
static void v_GetDepthStencilSurface(void) {
    count();
    uint32_t s = DEV(A32(0))->ds;
    addref(s);
    out_obj(A32(1), s);
    RET(s ? D3D_OK : D3DERR_NOTFOUND, 2);
}
static void v_SetTransform(void) {   /* (state, matrix) */
    count();
    uint32_t t = A32(1);
    if (t < 512) memcpy(DEV(A32(0))->transform[t], hle_ptr(A32(2)), 64);
    RET(D3D_OK, 3);
}
static void v_GetTransform(void) {
    count();
    uint32_t t = A32(1);
    if (t < 512) memcpy(hle_ptr(A32(2)), DEV(A32(0))->transform[t], 64);
    RET(D3D_OK, 3);
}
static void v_SetViewport(void) { count(); memcpy(DEV(A32(0))->viewport, hle_ptr(A32(1)), 24); RET(D3D_OK, 2); }
static void v_GetViewport(void) { count(); memcpy(hle_ptr(A32(1)), DEV(A32(0))->viewport, 24); RET(D3D_OK, 2); }
static void v_SetMaterial(void) { count(); memcpy(DEV(A32(0))->material, hle_ptr(A32(1)), 68); RET(D3D_OK, 2); }
static void v_GetMaterial(void) { count(); memcpy(hle_ptr(A32(1)), DEV(A32(0))->material, 68); RET(D3D_OK, 2); }
static void v_GetLight(void) { count(); memset(hle_ptr(A32(2)), 0, 104); RET(D3D_OK, 3); }
static void v_GetLightEnable(void) { count(); MEM32(A32(2)) = 0; RET(D3D_OK, 3); }
static void v_SetClipPlane(void) {
    count();
    if (A32(1) < 6) memcpy(DEV(A32(0))->clip[A32(1)], hle_ptr(A32(2)), 16);
    RET(D3D_OK, 3);
}
static void v_GetClipPlane(void) {
    count();
    if (A32(1) < 6) memcpy(hle_ptr(A32(2)), DEV(A32(0))->clip[A32(1)], 16);
    RET(D3D_OK, 3);
}
static void v_SetRenderState(void) {
    count();
    uint32_t s = A32(1);
    if (s < MAX_RS) DEV(A32(0))->rs[s] = A32(2);
    RET(D3D_OK, 3);
}
static void v_GetRenderState(void) {
    count();
    uint32_t s = A32(1);
    MEM32(A32(2)) = s < MAX_RS ? DEV(A32(0))->rs[s] : 0;
    RET(D3D_OK, 3);
}
static uint32_t new_stateblock(uint32_t dev) { void *r; uint32_t o = make(g_vt_sb, 8, &r); *(uint32_t *)r = dev; return o; }
static void v_CreateStateBlock(void) { count(); out_obj(A32(2), new_stateblock(A32(0))); RET(D3D_OK, 3); }
static void v_EndStateBlock(void) { count(); out_obj(A32(1), new_stateblock(A32(0))); RET(D3D_OK, 2); }
/* A sampler: 0-15, D3DDMAPSAMPLER (256), D3DVERTEXTEXTURESAMPLER0-3 (257-260). */
static uint32_t stage_index(uint32_t s) { return s < MAX_SAMP ? s : s >= 257 && s <= 260 ? MAX_SAMP + 1 + (s - 257) : MAX_SAMP; }
static void v_GetTexture(void) {
    count();
    uint32_t t = DEV(A32(0))->tex[stage_index(A32(1))];
    addref(t);
    out_obj(A32(2), t);
    RET(D3D_OK, 3);
}
static void v_SetTexture(void) { count(); DEV(A32(0))->tex[stage_index(A32(1))] = A32(2); RET(D3D_OK, 3); }
static void v_GetTextureStageState(void) {
    count();
    uint32_t st = A32(1), ty = A32(2);
    MEM32(A32(3)) = st < MAX_STAGES && ty < MAX_TSS ? DEV(A32(0))->tss[st][ty] : 0;
    RET(D3D_OK, 4);
}
static void v_SetTextureStageState(void) {
    count();
    uint32_t st = A32(1), ty = A32(2);
    if (st < MAX_STAGES && ty < MAX_TSS) DEV(A32(0))->tss[st][ty] = A32(3);
    RET(D3D_OK, 4);
}
static void v_GetSamplerState(void) {
    count();
    uint32_t s = stage_index(A32(1)), ty = A32(2);
    MEM32(A32(3)) = ty < 14 ? DEV(A32(0))->samp[s][ty] : 0;
    RET(D3D_OK, 4);
}
static void v_SetSamplerState(void) {
    count();
    uint32_t s = stage_index(A32(1)), ty = A32(2);
    if (ty < 14) DEV(A32(0))->samp[s][ty] = A32(3);
    RET(D3D_OK, 4);
}
static void v_ValidateDevice(void) { count(); if (A32(1)) MEM32(A32(1)) = 1; RET(D3D_OK, 2); }
static void v_GetCurrentTexturePalette(void) { count(); MEM32(A32(1)) = 0; RET(D3D_OK, 2); }
static void v_SetScissorRect(void) { count(); memcpy(DEV(A32(0))->scissor, hle_ptr(A32(1)), 16); RET(D3D_OK, 2); }
static void v_GetScissorRect(void) { count(); memcpy(hle_ptr(A32(1)), DEV(A32(0))->scissor, 16); RET(D3D_OK, 2); }
static void v_SetSoftwareVertexProcessing(void) { count(); DEV(A32(0))->sw_vp = A32(1); RET(D3D_OK, 2); }
static void v_GetSoftwareVertexProcessing(void) { count(); RET(DEV(A32(0))->sw_vp, 1); }
static void v_GetNPatchMode(void) { count(); RET(0, 1); }
static float arg_f(int i) { uint32_t u = A32(i); float f; memcpy(&f, &u, 4); return f; }
static void v_Clear(void) {   /* (count, rects, flags, color, z, stencil) */
    count();
    gpu_clear(DEV(A32(0)), A32(1), A32(2), A32(3), A32(4), arg_f(5), A32(6));
    RET(D3D_OK, 7);
}
static void v_DrawPrimitive(void) {   /* (type, start vertex, primitives) */
    count();
    gpu_draw(DEV(A32(0)), A32(1), A32(3), A32(2), 0, 0, 0, 0, 0, 0, 0, 0);
    RET(D3D_OK, 4);
}
static void v_DrawIndexedPrimitive(void) {   /* (type, base vertex, min index, vertices, start index, primitives) */
    count();
    gpu_draw(DEV(A32(0)), A32(1), A32(6), A32(5), 1, (int32_t)A32(2), A32(3), A32(4), 0, 0, 0, 0);
    RET(D3D_OK, 7);
}
static void v_DrawPrimitiveUP(void) {   /* (type, primitives, data, stride) */
    count();
    gpu_draw(DEV(A32(0)), A32(1), A32(2), 0, 0, 0, 0, 0, A32(3), A32(4), 0, 0);
    RET(D3D_OK, 5);
}
static void v_DrawIndexedPrimitiveUP(void) {   /* (type, min index, vertices, primitives, indices, format, data, stride) */
    count();
    gpu_draw(DEV(A32(0)), A32(1), A32(4), 0, 1, 0, A32(2), A32(3), A32(7), A32(8), A32(5), A32(6));
    RET(D3D_OK, 9);
}
/* GetRenderTargetData(render target, system-memory surface): its pixels. */
static void v_GetRenderTargetData(void) {
    count();
    res_t *src = RES(A32(1)), *dst = RES(A32(2));
    res_t *s = src->parent ? RES(src->parent) : src;
    uint32_t sl = src->parent ? src->level : 0, sf = src->parent ? src->face : 0;
    gpu_readback(DEV(A32(0)), s, sl, sf);
    uint32_t sp, ss, dp, ds;
    uint32_t sm = level_mem(s, sl, sf, &sp, &ss);
    res_t *d = dst->parent ? RES(dst->parent) : dst;
    uint32_t dm = level_mem(d, dst->parent ? dst->level : 0, dst->parent ? dst->face : 0, &dp, &ds);
    memcpy(hle_ptr(dm), hle_ptr(sm), ss < ds ? ss : ds);
    RET(D3D_OK, 3);
}
/* UpdateTexture(system-memory texture, texture): every level, by the CPU. */
static void v_UpdateTexture(void) {
    count();
    res_t *src = RES(A32(1)), *dst = RES(A32(2));
    int faces = src->kind == R_CUBETEXTURE ? 6 : 1;
    uint32_t skip = 0;                                   /* the source may have more (larger) levels */
    while (skip < src->levels && level_dim(src->width, skip) > dst->width) skip++;
    for (int f = 0; f < faces; f++)
        for (uint32_t l = 0; l < dst->levels && l + skip < src->levels; l++) {
            uint32_t sp, ss, dp, ds;
            uint32_t sm = level_mem(src, l + skip, (uint32_t)f, &sp, &ss), dm = level_mem(dst, l, (uint32_t)f, &dp, &ds);
            memcpy(hle_ptr(dm), hle_ptr(sm), ss < ds ? ss : ds);
            dst->dirty[f] |= 1u << l;
        }
    RET(D3D_OK, 3);
}
/* UpdateSurface(src, src rect, dst, dst point): the whole surface (the game
 * passes no rectangles), by the CPU. */
static void v_UpdateSurface(void) {
    count();
    res_t *a = RES(A32(1)), *b = RES(A32(3));
    res_t *sa = a->parent ? RES(a->parent) : a, *sb_ = b->parent ? RES(b->parent) : b;
    uint32_t al = a->parent ? a->level : 0, af = a->parent ? a->face : 0, bl = b->parent ? b->level : 0, bf = b->parent ? b->face : 0;
    uint32_t sp, ss, dp, ds;
    uint32_t sm = level_mem(sa, al, af, &sp, &ss), dm = level_mem(sb_, bl, bf, &dp, &ds);
    memcpy(hle_ptr(dm), hle_ptr(sm), ss < ds ? ss : ds);
    sb_->dirty[bf] |= 1u << bl;
    RET(D3D_OK, 5);
}
static void v_StretchRect(void) {   /* (src, src rect, dst, dst rect, filter) */
    count();
    gpu_stretch_rect(DEV(A32(0)), A32(1), A32(2), A32(3), A32(4), A32(5));
    RET(D3D_OK, 6);
}

/* Shader bytecode: version token, instructions, end token 0x0000FFFF.
 * Kept whole; dumped when STL_DUMP_SHADERS names a folder. */
static uint32_t token_len(uint32_t code) {
    /* The end token, found where an instruction would start: a comment
     * (D3DX's constant table opens every shader) is skipped by its length,
     * a def's four raw values by its, as either may hold 0x0000FFFF. */
    uint32_t ver = MEM32(code), n = 1;
    int sm2 = ((ver >> 8) & 0xFF) >= 2;
    while (n < (1u << 20)) {
        uint32_t t = MEM32(code + 4 * n);
        if (t == 0x0000FFFFu) return 4 * (n + 1);
        if ((t & 0xFFFF) == 0xFFFE) { n += 1 + ((t >> 16) & 0x7FFF); continue; }
        if (sm2 && !(t & 0x80000000u)) { n += 1 + ((t >> 24) & 0xF); continue; }
        if ((t & 0xFFFF) == 81 && !(t & 0x80000000u)) { n += 6; continue; }   /* shader model 1's def */
        n++;
    }
    return 4 * n;
}
static void dump(const char *kind, uint32_t code, uint32_t len) {
    static const char *dir;
    static int checked;
    if (!checked) { checked = 1; dir = getenv("STL_DUMP_SHADERS"); if (dir) mkdir(dir, 0755); }
    if (!dir) return;
    uint64_t h = 1469598103934665603ull;                          /* FNV-1a */
    const uint8_t *p = (const uint8_t *)hle_ptr(code);
    for (uint32_t i = 0; i < len; i++) h = (h ^ p[i]) * 1099511628211ull;
    char path[4096];
    snprintf(path, sizeof path, "%s/%s_%016llx.bin", dir, kind, (unsigned long long)h);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(p, 1, len, f); fclose(f); }
}
static uint32_t new_blob(uint32_t vt, uint32_t dev, uint32_t src, uint32_t len) {
    blob_t *b;
    uint32_t o = make(vt, sizeof(blob_t), (void **)&b);
    b->device = dev, b->len = len, b->code = hle_va(hle_alloc(len ? len : 4));
    memcpy(hle_ptr(b->code), hle_ptr(src), len);
    return o;
}
static void v_CreateVertexShader(void) {   /* (function, &shader) */
    count();
    uint32_t len = token_len(A32(1));
    dump("vs", A32(1), len);
    out_obj(A32(2), new_blob(g_vt_vs, A32(0), A32(1), len));
    RET(D3D_OK, 3);
}
static void v_CreatePixelShader(void) {
    count();
    uint32_t len = token_len(A32(1));
    dump("ps", A32(1), len);
    out_obj(A32(2), new_blob(g_vt_ps, A32(0), A32(1), len));
    RET(D3D_OK, 3);
}
static void v_CreateVertexDeclaration(void) {   /* (elements, &decl): 8-byte elements to D3DDECL_END */
    count();
    uint32_t e = A32(1), n = 1;
    while (n < 65 && MEM16(e + 8 * (n - 1)) != 0xFF) n++;
    out_obj(A32(2), new_blob(g_vt_decl, A32(0), e, 8 * n));
    RET(D3D_OK, 3);
}
#define GETSET(name, field)                                                        \
    static void v_Set##name(void) { count(); DEV(A32(0))->field = A32(1); RET(D3D_OK, 2); } \
    static void v_Get##name(void) { count(); uint32_t x = DEV(A32(0))->field; addref(x); out_obj(A32(1), x); RET(D3D_OK, 2); }
GETSET(VertexDeclaration, decl)
GETSET(VertexShader, vs)
GETSET(PixelShader, ps)
GETSET(Indices, indices)
static void v_SetFVF(void) { count(); DEV(A32(0))->fvf = A32(1); RET(D3D_OK, 2); }
static void v_GetFVF(void) { count(); MEM32(A32(1)) = DEV(A32(0))->fvf; RET(D3D_OK, 2); }
#define CONSTS(name, arr, max, sz)                                                  \
    static void v_Set##name(void) {                                                 \
        count();                                                                    \
        uint32_t s = A32(1), n = A32(3);                                            \
        if (s < max) memcpy(&DEV(A32(0))->arr[s], hle_ptr(A32(2)), (s + n > max ? max - s : n) * sz); \
        RET(D3D_OK, 4);                                                             \
    }                                                                               \
    static void v_Get##name(void) {                                                 \
        count();                                                                    \
        uint32_t s = A32(1), n = A32(3);                                            \
        if (s < max) memcpy(hle_ptr(A32(2)), &DEV(A32(0))->arr[s], (s + n > max ? max - s : n) * sz); \
        RET(D3D_OK, 4);                                                             \
    }
CONSTS(VertexShaderConstantF, vsc, 256, 16)
CONSTS(PixelShaderConstantF, psc, 224, 16)
CONSTS(VertexShaderConstantI, vsi, 16, 16)
CONSTS(PixelShaderConstantI, psi, 16, 16)
CONSTS(VertexShaderConstantB, vsb, 16, 4)
CONSTS(PixelShaderConstantB, psb, 16, 4)
static void v_SetStreamSource(void) {   /* (stream, vb, offset, stride) */
    count();
    uint32_t s = A32(1);
    if (s < 16) { uint32_t *x = DEV(A32(0))->stream[s]; x[0] = A32(2), x[1] = A32(3), x[2] = A32(4); }
    RET(D3D_OK, 5);
}
static void v_GetStreamSource(void) {
    count();
    uint32_t s = A32(1);
    uint32_t *x = DEV(A32(0))->stream[s < 16 ? s : 0];
    addref(x[0]);
    out_obj(A32(2), x[0]);
    if (A32(3)) MEM32(A32(3)) = x[1];
    if (A32(4)) MEM32(A32(4)) = x[2];
    RET(D3D_OK, 5);
}
static void v_SetStreamSourceFreq(void) { count(); if (A32(1) < 16) DEV(A32(0))->stream_freq[A32(1)] = A32(2); RET(D3D_OK, 3); }
static void v_GetStreamSourceFreq(void) { count(); MEM32(A32(2)) = A32(1) < 16 ? DEV(A32(0))->stream_freq[A32(1)] : 1; RET(D3D_OK, 3); }
static void v_CreateQuery(void) {   /* (type, &query): NULL asks whether the type is supported */
    count();
    if (!A32(2)) RET(D3D_OK, 3);
    query_t *q;
    uint32_t o = make(g_vt_query, sizeof(query_t), (void **)&q);
    q->device = A32(0), q->type = A32(1);
    out_obj(A32(2), o);
    RET(D3D_OK, 3);
}

/* ---- resources ----------------------------------------------------------- */
static void r_GetLevelCount(void) { count(); RET(RES(A32(0))->levels, 1); }
static void r_GetLOD(void) { count(); RET(0, 1); }
static void r_SetLOD(void) { count(); RET(0, 2); }
static void r_GetAutoGenFilterType(void) { count(); RET(2, 1); }   /* D3DTEXF_LINEAR */

/* D3DSURFACE_DESC: format, type, usage, pool, ms, msq, width, height. */
static void put_desc(uint32_t p, res_t *r, uint32_t level, uint32_t kind) {
    MEM32(p) = r->format, MEM32(p + 4) = kind, MEM32(p + 8) = r->usage, MEM32(p + 12) = r->pool;
    MEM32(p + 16) = r->multisample, MEM32(p + 20) = 0;
    MEM32(p + 24) = level_dim(r->width, level), MEM32(p + 28) = level_dim(r->height, level);
}
static void t_GetLevelDesc(void) {   /* (level, desc) */
    count();
    res_t *r = RES(A32(0));
    if (A32(1) >= r->levels) RET(D3DERR_INVALIDCALL, 3);
    put_desc(A32(2), r, A32(1), R_SURFACE);
    RET(D3D_OK, 3);
}
static uint32_t level_surface(uint32_t tex, uint32_t level, uint32_t face) {
    res_t *t = RES(tex), *s;
    uint32_t o = make(g_vt_surf, sizeof(res_t), (void **)&s);
    s->kind = R_SURFACE, s->device = t->device;
    s->width = level_dim(t->width, level), s->height = level_dim(t->height, level), s->depth = s->levels = 1;
    s->format = t->format, s->usage = t->usage, s->pool = t->pool;
    s->parent = tex, s->level = level, s->face = face;
    addref(tex);
    return o;
}
static void t_GetSurfaceLevel(void) {   /* (level, &surface) */
    count();
    if (A32(1) >= RES(A32(0))->levels) RET(D3DERR_INVALIDCALL, 3);
    out_obj(A32(2), level_surface(A32(0), A32(1), 0));
    RET(D3D_OK, 3);
}
static void put_locked(uint32_t lr, res_t *r, uint32_t level, uint32_t face, uint32_t rect) {
    if (gpu_is_render_target(r)) gpu_readback(OBJ(r->device, dev_t_), r, level, face);   /* the GPU has its pixels */
    uint32_t pitch, slice, mem = level_mem(r, level, face, &pitch, &slice);
    r->dirty[face % 6] |= 1u << level;                     /* goes up to the GPU before it is next used */
    if (rect) {                                                /* RECT: left, top */
        uint32_t x = MEM32(rect), y = MEM32(rect + 4);
        int b = block_bytes(r->format);
        mem += b ? (y / 4) * pitch + (x / 4) * (uint32_t)b : y * pitch + x * (uint32_t)bits_per_pixel(r->format) / 8;
    }
    MEM32(lr) = pitch, MEM32(lr + 4) = mem;
}
static void t_LockRect(void) {   /* (level, D3DLOCKED_RECT*, rect, flags) */
    count();
    res_t *r = RES(A32(0));
    if (A32(1) >= r->levels) RET(D3DERR_INVALIDCALL, 5);
    put_locked(A32(2), r, A32(1), 0, A32(3));
    RET(D3D_OK, 5);
}
static void c_GetCubeMapSurface(void) {   /* (face, level, &surface) */
    count();
    out_obj(A32(3), level_surface(A32(0), A32(2), A32(1)));
    RET(D3D_OK, 4);
}
static void c_LockRect(void) {   /* (face, level, locked, rect, flags) */
    count();
    put_locked(A32(3), RES(A32(0)), A32(2), A32(1) % 6, A32(4));
    RET(D3D_OK, 6);
}
static void w_GetLevelDesc(void) {   /* D3DVOLUME_DESC: format, type, usage, pool, w, h, d */
    count();
    res_t *r = RES(A32(0));
    uint32_t p = A32(2), l = A32(1);
    MEM32(p) = r->format, MEM32(p + 4) = R_VOLUME, MEM32(p + 8) = r->usage, MEM32(p + 12) = r->pool;
    MEM32(p + 16) = level_dim(r->width, l), MEM32(p + 20) = level_dim(r->height, l), MEM32(p + 24) = level_dim(r->depth, l);
    RET(D3D_OK, 3);
}
static void w_LockBox(void) {   /* (level, D3DLOCKED_BOX*, box, flags) */
    count();
    uint32_t pitch, slice, mem = level_mem(RES(A32(0)), A32(1), 0, &pitch, &slice);
    RES(A32(0))->dirty[0] |= 1u << A32(1);
    MEM32(A32(2)) = pitch, MEM32(A32(2) + 4) = slice, MEM32(A32(2) + 8) = mem;
    RET(D3D_OK, 5);
}
static void s_GetDesc(void) { count(); put_desc(A32(1), RES(A32(0)), 0, R_SURFACE); RET(D3D_OK, 2); }
static void s_GetContainer(void) {   /* (riid, &container) */
    count();
    uint32_t p = RES(A32(0))->parent;
    if (!p) { out_obj(A32(2), 0); RET(E_NOINTERFACE, 3); }
    addref(p);
    out_obj(A32(2), p);
    RET(D3D_OK, 3);
}
static void s_LockRect(void) {   /* (locked, rect, flags) */
    count();
    res_t *s = RES(A32(0));
    if (s->parent) put_locked(A32(1), RES(s->parent), s->level, s->face, A32(2));
    else put_locked(A32(1), s, 0, 0, A32(2));
    RET(D3D_OK, 4);
}
static void s_GetDC(void) { count(); RET(D3DERR_INVALIDCALL, 2); }
static void b_Lock(void) {   /* (offset, size, &data, flags) */
    count();
    res_t *r = RES(A32(0));
    r->dirty[0] = 1;
    MEM32(A32(3)) = r->mem[0] + (A32(1) < r->size ? A32(1) : 0);
    RET(D3D_OK, 5);
}
static void b_GetDesc(void) {   /* D3DVERTEXBUFFER_DESC: format, type, usage, pool, size, fvf (index: no fvf) */
    count();
    res_t *r = RES(A32(0));
    uint32_t p = A32(1);
    MEM32(p) = r->format, MEM32(p + 4) = r->kind, MEM32(p + 8) = r->usage, MEM32(p + 12) = r->pool, MEM32(p + 16) = r->size;
    if (r->kind == R_VERTEXBUFFER) MEM32(p + 20) = r->fvf;
    RET(D3D_OK, 2);
}
static void blob_GetFunction(void) {   /* (data, &size): the bytes, or their size with no buffer */
    count();
    blob_t *b = OBJ(A32(0), blob_t);
    uint32_t data = A32(1), sp = A32(2);
    if (!data) { MEM32(sp) = b->len; RET(D3D_OK, 3); }
    if (MEM32(sp) < b->len) RET(D3DERR_INVALIDCALL, 3);
    memcpy(hle_ptr(data), hle_ptr(b->code), b->len);
    MEM32(sp) = b->len;
    RET(D3D_OK, 3);
}
static void decl_GetDeclaration(void) {   /* (elements, &count) */
    count();
    blob_t *b = OBJ(A32(0), blob_t);
    if (A32(1)) memcpy(hle_ptr(A32(1)), hle_ptr(b->code), b->len);
    MEM32(A32(2)) = b->len / 8;
    RET(D3D_OK, 3);
}
static void q_GetType(void) { count(); RET(OBJ(A32(0), query_t)->type, 1); }
static void q_GetDataSize(void) {
    count();
    uint32_t t = OBJ(A32(0), query_t)->type;
    RET(t == 10 ? 8 : 4, 1);                    /* TIMESTAMP is a UINT64; EVENT, OCCLUSION a DWORD */
}
static void q_GetData(void) {   /* (data, size, flags): done at once; an occlusion query sees everything */
    count();
    uint32_t p = A32(1), n = A32(2), t = OBJ(A32(0), query_t)->type;
    if (p && n) { memset(hle_ptr(p), 0, n); MEM32(p) = t == 9 ? 1000000 : 1; }
    RET(D3D_OK, 4);
}

/* ---- the vtables ---------------------------------------------------------------- */
#define UNK(i) { i "::QueryInterface", u_QueryInterface }, { i "::AddRef", u_AddRef }, { i "::Release", u_Release }
#define RESOURCE(i) UNK(i), { i "::GetDevice", u_GetDevice }, { i "::SetPrivateData", ok4 },              \
    { i "::GetPrivateData", u_GetPrivateData }, { i "::FreePrivateData", ok1 }, { i "::SetPriority", u_SetPriority }, \
    { i "::GetPriority", u_GetPriority }, { i "::PreLoad", ok0 }, { i "::GetType", u_GetType }
#define BASETEX(i) RESOURCE(i), { i "::SetLOD", r_SetLOD }, { i "::GetLOD", r_GetLOD },                     \
    { i "::GetLevelCount", r_GetLevelCount }, { i "::SetAutoGenFilterType", ok1 },                           \
    { i "::GetAutoGenFilterType", r_GetAutoGenFilterType }, { i "::GenerateMipSubLevels", ok0 }

static void vtables(void) {
    if (g_vt_d3d) return;
    static const win32hle_shim d3d[] = {
        UNK("IDirect3D9"), { "IDirect3D9::RegisterSoftwareDevice", ok1 },
        { "IDirect3D9::GetAdapterCount", d_GetAdapterCount }, { "IDirect3D9::GetAdapterIdentifier", d_GetAdapterIdentifier },
        { "IDirect3D9::GetAdapterModeCount", d_GetAdapterModeCount }, { "IDirect3D9::EnumAdapterModes", d_EnumAdapterModes },
        { "IDirect3D9::GetAdapterDisplayMode", d_GetAdapterDisplayMode }, { "IDirect3D9::CheckDeviceType", d_CheckDeviceType },
        { "IDirect3D9::CheckDeviceFormat", d_CheckDeviceFormat },
        { "IDirect3D9::CheckDeviceMultiSampleType", d_CheckDeviceMultiSampleType },
        { "IDirect3D9::CheckDepthStencilMatch", ok5 }, { "IDirect3D9::CheckDeviceFormatConversion", ok4 },
        { "IDirect3D9::GetDeviceCaps", d_GetDeviceCaps }, { "IDirect3D9::GetAdapterMonitor", d_GetAdapterMonitor },
        { "IDirect3D9::CreateDevice", d_CreateDevice }, { 0, 0 } };
#define D "IDirect3DDevice9::"
    static const win32hle_shim dev[] = {
        UNK("IDirect3DDevice9"), { D "TestCooperativeLevel", v_TestCooperativeLevel },
        { D "GetAvailableTextureMem", v_GetAvailableTextureMem }, { D "EvictManagedResources", ok0 },
        { D "GetDirect3D", v_GetDirect3D }, { D "GetDeviceCaps", v_GetDeviceCaps }, { D "GetDisplayMode", v_GetDisplayMode },
        { D "GetCreationParameters", v_GetCreationParameters }, { D "SetCursorProperties", ok3 },
        { D "SetCursorPosition", ok3 }, { D "ShowCursor", v_ShowCursor }, { D "CreateAdditionalSwapChain", ok2 },
        { D "GetSwapChain", ok2 }, { D "GetNumberOfSwapChains", v_GetNumberOfSwapChains }, { D "Reset", v_Reset },
        { D "Present", v_Present }, { D "GetBackBuffer", v_GetBackBuffer }, { D "GetRasterStatus", v_GetRasterStatus },
        { D "SetDialogBoxMode", ok1 }, { D "SetGammaRamp", v_SetGammaRamp }, { D "GetGammaRamp", v_GetGammaRamp },
        { D "CreateTexture", v_CreateTexture }, { D "CreateVolumeTexture", v_CreateVolumeTexture },
        { D "CreateCubeTexture", v_CreateCubeTexture }, { D "CreateVertexBuffer", v_CreateVertexBuffer },
        { D "CreateIndexBuffer", v_CreateIndexBuffer }, { D "CreateRenderTarget", v_CreateRenderTarget },
        { D "CreateDepthStencilSurface", v_CreateDepthStencilSurface }, { D "UpdateSurface", v_UpdateSurface },
        { D "UpdateTexture", v_UpdateTexture }, { D "GetRenderTargetData", v_GetRenderTargetData }, { D "GetFrontBufferData", ok2 },
        { D "StretchRect", v_StretchRect }, { D "ColorFill", ok3 }, { D "CreateOffscreenPlainSurface", v_CreateOffscreenPlainSurface },
        { D "SetRenderTarget", v_SetRenderTarget }, { D "GetRenderTarget", v_GetRenderTarget },
        { D "SetDepthStencilSurface", v_SetDepthStencilSurface }, { D "GetDepthStencilSurface", v_GetDepthStencilSurface },
        { D "BeginScene", ok0 }, { D "EndScene", ok0 }, { D "Clear", v_Clear }, { D "SetTransform", v_SetTransform },
        { D "GetTransform", v_GetTransform }, { D "MultiplyTransform", ok2 }, { D "SetViewport", v_SetViewport },
        { D "GetViewport", v_GetViewport }, { D "SetMaterial", v_SetMaterial }, { D "GetMaterial", v_GetMaterial },
        { D "SetLight", ok2 }, { D "GetLight", v_GetLight }, { D "LightEnable", ok2 }, { D "GetLightEnable", v_GetLightEnable },
        { D "SetClipPlane", v_SetClipPlane }, { D "GetClipPlane", v_GetClipPlane }, { D "SetRenderState", v_SetRenderState },
        { D "GetRenderState", v_GetRenderState }, { D "CreateStateBlock", v_CreateStateBlock }, { D "BeginStateBlock", ok0 },
        { D "EndStateBlock", v_EndStateBlock }, { D "SetClipStatus", ok1 }, { D "GetClipStatus", ok1 },
        { D "GetTexture", v_GetTexture }, { D "SetTexture", v_SetTexture },
        { D "GetTextureStageState", v_GetTextureStageState }, { D "SetTextureStageState", v_SetTextureStageState },
        { D "GetSamplerState", v_GetSamplerState }, { D "SetSamplerState", v_SetSamplerState },
        { D "ValidateDevice", v_ValidateDevice }, { D "SetPaletteEntries", ok2 }, { D "GetPaletteEntries", ok2 },
        { D "SetCurrentTexturePalette", ok1 }, { D "GetCurrentTexturePalette", v_GetCurrentTexturePalette },
        { D "SetScissorRect", v_SetScissorRect }, { D "GetScissorRect", v_GetScissorRect },
        { D "SetSoftwareVertexProcessing", v_SetSoftwareVertexProcessing },
        { D "GetSoftwareVertexProcessing", v_GetSoftwareVertexProcessing }, { D "SetNPatchMode", ok1 },
        { D "GetNPatchMode", v_GetNPatchMode }, { D "DrawPrimitive", v_DrawPrimitive },
        { D "DrawIndexedPrimitive", v_DrawIndexedPrimitive }, { D "DrawPrimitiveUP", v_DrawPrimitiveUP },
        { D "DrawIndexedPrimitiveUP", v_DrawIndexedPrimitiveUP }, { D "ProcessVertices", ok6 },
        { D "CreateVertexDeclaration", v_CreateVertexDeclaration }, { D "SetVertexDeclaration", v_SetVertexDeclaration },
        { D "GetVertexDeclaration", v_GetVertexDeclaration }, { D "SetFVF", v_SetFVF }, { D "GetFVF", v_GetFVF },
        { D "CreateVertexShader", v_CreateVertexShader }, { D "SetVertexShader", v_SetVertexShader },
        { D "GetVertexShader", v_GetVertexShader },
        { D "SetVertexShaderConstantF", v_SetVertexShaderConstantF }, { D "GetVertexShaderConstantF", v_GetVertexShaderConstantF },
        { D "SetVertexShaderConstantI", v_SetVertexShaderConstantI }, { D "GetVertexShaderConstantI", v_GetVertexShaderConstantI },
        { D "SetVertexShaderConstantB", v_SetVertexShaderConstantB }, { D "GetVertexShaderConstantB", v_GetVertexShaderConstantB },
        { D "SetStreamSource", v_SetStreamSource }, { D "GetStreamSource", v_GetStreamSource },
        { D "SetStreamSourceFreq", v_SetStreamSourceFreq }, { D "GetStreamSourceFreq", v_GetStreamSourceFreq },
        { D "SetIndices", v_SetIndices }, { D "GetIndices", v_GetIndices },
        { D "CreatePixelShader", v_CreatePixelShader }, { D "SetPixelShader", v_SetPixelShader },
        { D "GetPixelShader", v_GetPixelShader },
        { D "SetPixelShaderConstantF", v_SetPixelShaderConstantF }, { D "GetPixelShaderConstantF", v_GetPixelShaderConstantF },
        { D "SetPixelShaderConstantI", v_SetPixelShaderConstantI }, { D "GetPixelShaderConstantI", v_GetPixelShaderConstantI },
        { D "SetPixelShaderConstantB", v_SetPixelShaderConstantB }, { D "GetPixelShaderConstantB", v_GetPixelShaderConstantB },
        { D "DrawRectPatch", ok3 }, { D "DrawTriPatch", ok3 }, { D "DeletePatch", ok1 }, { D "CreateQuery", v_CreateQuery },
        { 0, 0 } };
#undef D
    static const win32hle_shim tex[] = {
        BASETEX("IDirect3DTexture9"), { "IDirect3DTexture9::GetLevelDesc", t_GetLevelDesc },
        { "IDirect3DTexture9::GetSurfaceLevel", t_GetSurfaceLevel }, { "IDirect3DTexture9::LockRect", t_LockRect },
        { "IDirect3DTexture9::UnlockRect", ok1 }, { "IDirect3DTexture9::AddDirtyRect", ok1 }, { 0, 0 } };
    static const win32hle_shim cube[] = {
        BASETEX("IDirect3DCubeTexture9"), { "IDirect3DCubeTexture9::GetLevelDesc", t_GetLevelDesc },
        { "IDirect3DCubeTexture9::GetCubeMapSurface", c_GetCubeMapSurface }, { "IDirect3DCubeTexture9::LockRect", c_LockRect },
        { "IDirect3DCubeTexture9::UnlockRect", ok2 }, { "IDirect3DCubeTexture9::AddDirtyRect", ok2 }, { 0, 0 } };
    static const win32hle_shim vol[] = {
        BASETEX("IDirect3DVolumeTexture9"), { "IDirect3DVolumeTexture9::GetLevelDesc", w_GetLevelDesc },
        { "IDirect3DVolumeTexture9::GetVolumeLevel", ok2 }, { "IDirect3DVolumeTexture9::LockBox", w_LockBox },
        { "IDirect3DVolumeTexture9::UnlockBox", ok1 }, { "IDirect3DVolumeTexture9::AddDirtyBox", ok1 }, { 0, 0 } };
    static const win32hle_shim surf[] = {
        RESOURCE("IDirect3DSurface9"), { "IDirect3DSurface9::GetContainer", s_GetContainer },
        { "IDirect3DSurface9::GetDesc", s_GetDesc }, { "IDirect3DSurface9::LockRect", s_LockRect },
        { "IDirect3DSurface9::UnlockRect", ok0 }, { "IDirect3DSurface9::GetDC", s_GetDC },
        { "IDirect3DSurface9::ReleaseDC", ok1 }, { 0, 0 } };
    static const win32hle_shim vb[] = {
        RESOURCE("IDirect3DVertexBuffer9"), { "IDirect3DVertexBuffer9::Lock", b_Lock },
        { "IDirect3DVertexBuffer9::Unlock", ok0 }, { "IDirect3DVertexBuffer9::GetDesc", b_GetDesc }, { 0, 0 } };
    static const win32hle_shim ib[] = {
        RESOURCE("IDirect3DIndexBuffer9"), { "IDirect3DIndexBuffer9::Lock", b_Lock },
        { "IDirect3DIndexBuffer9::Unlock", ok0 }, { "IDirect3DIndexBuffer9::GetDesc", b_GetDesc }, { 0, 0 } };
    static const win32hle_shim decl[] = {
        UNK("IDirect3DVertexDeclaration9"), { "IDirect3DVertexDeclaration9::GetDevice", u_GetDevice },
        { "IDirect3DVertexDeclaration9::GetDeclaration", decl_GetDeclaration }, { 0, 0 } };
    static const win32hle_shim vs[] = {
        UNK("IDirect3DVertexShader9"), { "IDirect3DVertexShader9::GetDevice", u_GetDevice },
        { "IDirect3DVertexShader9::GetFunction", blob_GetFunction }, { 0, 0 } };
    static const win32hle_shim ps[] = {
        UNK("IDirect3DPixelShader9"), { "IDirect3DPixelShader9::GetDevice", u_GetDevice },
        { "IDirect3DPixelShader9::GetFunction", blob_GetFunction }, { 0, 0 } };
    static const win32hle_shim query[] = {
        UNK("IDirect3DQuery9"), { "IDirect3DQuery9::GetDevice", u_GetDevice }, { "IDirect3DQuery9::GetType", q_GetType },
        { "IDirect3DQuery9::GetDataSize", q_GetDataSize }, { "IDirect3DQuery9::Issue", ok1 },
        { "IDirect3DQuery9::GetData", q_GetData }, { 0, 0 } };
    static const win32hle_shim sb[] = {
        UNK("IDirect3DStateBlock9"), { "IDirect3DStateBlock9::GetDevice", u_GetDevice },
        { "IDirect3DStateBlock9::Capture", ok0 }, { "IDirect3DStateBlock9::Apply", ok0 }, { 0, 0 } };
    g_vt_d3d = hle_com_vtable(d3d);
    g_vt_dev = hle_com_vtable(dev);
    g_vt_tex = hle_com_vtable(tex);
    g_vt_cube = hle_com_vtable(cube);
    g_vt_vol = hle_com_vtable(vol);
    g_vt_surf = hle_com_vtable(surf);
    g_vt_vb = hle_com_vtable(vb);
    g_vt_ib = hle_com_vtable(ib);
    g_vt_decl = hle_com_vtable(decl);
    g_vt_vs = hle_com_vtable(vs);
    g_vt_ps = hle_com_vtable(ps);
    g_vt_query = hle_com_vtable(query);
    g_vt_sb = hle_com_vtable(sb);
}

static void x_Direct3DCreate9(void) {            /* (SDKVersion) */
    vtables();
    fprintf(stderr, "[d3d9] Direct3DCreate9(SDK %u)\n", A32(0));
    RET(make(g_vt_d3d, 8, NULL), 1);
}

const win32hle_shim stl_d3d9_shims[] = {
    { "Direct3DCreate9", x_Direct3DCreate9 },
    { NULL, NULL }
};
