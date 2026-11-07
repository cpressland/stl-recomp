/*
 * d3d9_int.h - what d3d9.c (the interfaces, the state) and d3d9_metal.m
 * (the GPU) share: the records behind the COM objects, and the GPU calls.
 *
 * Every object is an hle_com_new block: vtable, reference count, then at +8
 * the VA of its record, in guest memory; every record starts with its
 * device. The GPU's own objects (Metal textures, buffers, functions) hang
 * off the records as host pointers, retained by d3d9_metal.m.
 */
#ifndef D3D9_INT_H
#define D3D9_INT_H
#include <stdint.h>

#include "dxso.h"

#define OBJ(o, T) ((T *)hle_ptr(MEM32((o) + 8)))

enum { R_SURFACE = 1, R_VOLUME, R_TEXTURE, R_VOLUMETEXTURE, R_CUBETEXTURE, R_VERTEXBUFFER, R_INDEXBUFFER };

typedef struct {
    uint32_t device;
    uint32_t kind;            /* D3DRESOURCETYPE */
    uint32_t width, height, depth, levels;
    uint32_t format, usage, pool;
    uint32_t size;            /* a buffer's length */
    uint32_t fvf;             /* a vertex buffer's; an index buffer's format is `format` */
    uint32_t mem[16 * 6];     /* each level's (and face's) bytes, made on first lock */
    uint32_t parent, level, face;     /* a surface of a texture */
    uint32_t multisample;
    /* the GPU's */
    void *gpu;                /* id<MTLTexture> (a surface of a texture: its parent's) or id<MTLBuffer> */
    uint32_t dirty[6];        /* levels (bits) of each face written since they went up */
    uint64_t gpu_frame;       /* a dynamic buffer: the frame its copy is from */
    uint32_t gpu_offset;      /* ...and where in that frame's ring */
} res_t;

typedef struct {
    uint32_t device, len, code;          /* a shader's or declaration's bytes */
    /* a shader: its translation (one per sampler-type combination, ps_1_x) */
    dxso_shader info;
    int translated;                      /* 1 done, -1 failed */
    void *fn[4];                         /* id<MTLFunction>, by variant */
    uint32_t variant_key[4];
} blob_t;

typedef struct { uint32_t device, type; } query_t;

#define MAX_RS 256
#define MAX_TSS 33
#define MAX_SAMP 16
#define MAX_STAGES 16
#define SAMP_SLOTS (MAX_SAMP + 5)        /* 0-15, the displacement map, 4 vertex samplers */

typedef struct {
    uint32_t d3d;
    uint32_t params[14];                   /* D3DPRESENT_PARAMETERS, as last set */
    uint32_t adapter, type, hwnd, flags;
    uint32_t backbuffer, depth;            /* their surfaces */
    uint32_t rt[4], ds;
    uint32_t rs[MAX_RS];
    uint32_t tss[MAX_STAGES][MAX_TSS];
    uint32_t samp[SAMP_SLOTS][14];
    uint32_t tex[SAMP_SLOTS];
    uint32_t stream[16][3];                /* buffer, offset, stride */
    uint32_t stream_freq[16];
    uint32_t indices, decl, fvf, vs, ps;
    uint32_t viewport[6];
    float    transform[512][16];
    uint32_t scissor[4];
    uint32_t material[17];
    uint32_t clip[6][4];
    float    vsc[256][4], psc[224][4];
    int32_t  vsi[16][4], psi[16][4];
    uint32_t vsb[16], psb[16];
    uint32_t sw_vp;
    uint32_t light[8][26];                 /* D3DLIGHT9 */
    uint32_t light_on[8];
    void *gpu;                             /* the backend's device state */
    void *rec;                             /* between BeginStateBlock and EndStateBlock: the block (sblock_t) */
} dev_t_;

/* A state block: pieces of dev_t_ (offset, size) and their values. */
typedef struct { uint32_t off, size, at; } sbop_t;
typedef struct {
    uint32_t device;
    uint32_t n, cap, len, data_cap;
    sbop_t *ops;
    uint8_t *data;
} sblock_t;

/* ---- formats and memory (d3d9.c) ---- */
int      block_bytes(uint32_t format);          /* a compressed format's 4x4 block, else 0 */
int      bits_per_pixel(uint32_t format);
uint32_t level_dim(uint32_t d, uint32_t level);
uint32_t pitch_of(uint32_t format, uint32_t width);
uint32_t rows_of(uint32_t format, uint32_t height);
/* A level's (and face's) bytes in guest memory, made on first use. */
uint32_t level_mem(res_t *r, uint32_t level, uint32_t face, uint32_t *pitch, uint32_t *slice);

/* ---- the GPU (d3d9_metal.m) -------------------------------------------------- */
int  gpu_available(void);
void gpu_device(dev_t_ *d);                        /* after CreateDevice and Reset */
void gpu_clear(dev_t_ *d, uint32_t nrects, uint32_t rects, uint32_t flags, uint32_t color, float z, uint32_t stencil);
/* A draw: indexed (from d->indices) or not, from the streams, or (up) from
 * user memory at up_va with up_stride (and up_idx_va, a 16 or 32-bit index list). */
void gpu_draw(dev_t_ *d, uint32_t prim, uint32_t count, uint32_t start, int indexed, int32_t base_vertex,
              uint32_t min_index, uint32_t num_vertices, uint32_t up_va, uint32_t up_stride,
              uint32_t up_idx_va, uint32_t up_idx_format);
void gpu_present(dev_t_ *d);
void gpu_stretch_rect(dev_t_ *d, uint32_t src, uint32_t src_rect, uint32_t dst, uint32_t dst_rect, uint32_t filter);
void gpu_readback(dev_t_ *d, res_t *r, uint32_t level, uint32_t face);   /* a render target's pixels to its memory */
int  gpu_shot(dev_t_ *d, const char *path);       /* the back buffer as a BMP */

/* What a resource's GPU copy is, by D3D9 format (d3d9_metal.m). */
int  gpu_is_render_target(const res_t *r);

#endif
