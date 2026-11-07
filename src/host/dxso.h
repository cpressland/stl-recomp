/*
 * dxso.h - Direct3D 9 shader bytecode (vs/ps 1.1 to 3.0) to Metal Shading
 * Language (dxso.c).
 *
 * The interface between the stages is fixed, so any vertex shader pairs
 * with any pixel shader, as in Direct3D 9 (shader model 1 and 2):
 *
 *   vertex stage_in      v<n> [[attribute(n)]]; dxso_shader.input[] says
 *                        each one's dcl usage, for the vertex descriptor
 *   vertex -> fragment   struct dxso_varyings: position, d0 d1 (oD0, oD1),
 *                        t0..t7 (oT0..oT7), fog; all written, defaults 0
 *   buffers              0 float constants (float4[256] VS, [224] PS),
 *                        1 int constants (int4[16]), 2 bool (uint[16]),
 *                        3 struct dxso_params; streams from buffer 8
 *   textures, samplers   s<n> [[texture(n)]], [[sampler(n)]] (ps_1_x: stage n)
 *
 * Done in the shader because Metal has no fixed function for them: the
 * D3D9 half-pixel offset (vertex), the alpha test and vertex fog (pixel).
 */
#ifndef DXSO_H
#define DXSO_H
#include <stddef.h>
#include <stdint.h>

#define DXSO_MAX_INPUTS 16

enum { DXSO_TEX_2D = 0, DXSO_TEX_CUBE = 1, DXSO_TEX_3D = 2 };

typedef struct {
    /* ps_1_x declares no sampler types: each stage's bound texture's. */
    uint8_t sampler_type[16];
    /* A depth texture bound at a stage: sampled as a comparison
     * (hardware shadow mapping, tex2Dproj on a D24S8 texture). */
    uint16_t sampler_depth;
} dxso_options;

typedef struct {
    int pixel;                 /* 0 vertex, 1 pixel */
    int major, minor;
    /* Vertex inputs: for v<n>, its D3DDECLUSAGE (0xFF: not declared), index. */
    uint8_t input_usage[DXSO_MAX_INPUTS], input_index[DXSO_MAX_INPUTS];
    uint16_t samplers_used;    /* bit n: s<n> (ps_1_x: stage n) */
    uint8_t sampler_type[16];
    char *msl;                 /* malloc'd; entry point dxso_main */
    char error[256];           /* why it failed, when msl is NULL */
} dxso_shader;

/* The parameters every shader gets in buffer 3. Kept in step with the MSL
 * declaration dxso.c writes. */
typedef struct {
    float half_pixel[2];       /* (-1 / viewport width, 1 / viewport height) */
    float alpha_ref;           /* 0..1 */
    uint32_t alpha_func;       /* D3DCMP_*; 8 (ALWAYS) or 0: no test */
    float fog_color[4];
    uint32_t fog_enable;       /* vertex fog: oFog blends towards fog_color */
    uint32_t pad[3];
} dxso_params;

/* 0, with out filled; else -1, and out->error says why. */
int dxso_translate(const uint32_t *code, size_t words, const dxso_options *opt, dxso_shader *out);
void dxso_free(dxso_shader *s);

#endif
