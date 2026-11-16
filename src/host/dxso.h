/*
 * dxso.h - Direct3D 9 shader bytecode (vs/ps 1.1 to 3.0) to Metal Shading
 * Language, or to GLSL 4.50 for Vulkan (dxso.c).
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
 * GLSL (DXSO_GLSL), for Vulkan, the same with Vulkan's bindings, all in
 * descriptor set 0:
 *
 *   vertex inputs        v<n>: location n
 *   vertex -> fragment   locations 0 d0, 1 d1, 2..9 t0..t7, 10 fog
 *   buffers (std140)     vertex 0..3, pixel 4..7, as Metal's 0..3; the bools
 *                        as uvec4[4], so the same 64 bytes as Metal's uint[16]
 *   textures, samplers   s<n>: a combined image sampler at binding 8 + n
 *
 * Clip space is Direct3D's (y up): the Vulkan side flips it with a negative
 * viewport height, so gl_FragCoord is Direct3D's pixel position as it is.
 *
 * Done in the shader because Metal and Vulkan have no fixed function for
 * them: the D3D9 half-pixel offset (vertex), the alpha test and vertex fog
 * (pixel).
 */
#ifndef DXSO_H
#define DXSO_H
#include <stddef.h>
#include <stdint.h>

#define DXSO_MAX_INPUTS 16

enum { DXSO_TEX_2D = 0, DXSO_TEX_CUBE = 1, DXSO_TEX_3D = 2 };
enum { DXSO_MSL = 0, DXSO_GLSL = 1 };

typedef struct {
    /* ps_1_x declares no sampler types: each stage's bound texture's. */
    uint8_t sampler_type[16];
    /* A depth texture bound at a stage: sampled as a comparison
     * (hardware shadow mapping, tex2Dproj on a D24S8 texture). */
    uint16_t sampler_depth;
    uint8_t lang;              /* DXSO_MSL (also with no options), DXSO_GLSL */
} dxso_options;

typedef struct {
    int pixel;                 /* 0 vertex, 1 pixel */
    int major, minor;
    /* Vertex inputs: for v<n>, its D3DDECLUSAGE (0xFF: not declared), index. */
    uint8_t input_usage[DXSO_MAX_INPUTS], input_index[DXSO_MAX_INPUTS];
    uint16_t samplers_used;    /* bit n: s<n> (ps_1_x: stage n) */
    uint8_t sampler_type[16];
    char *text;                /* malloc'd: MSL (entry point dxso_main) or GLSL (main) */
    char error[256];           /* why it failed, when text is NULL */
} dxso_shader;

/* The parameters every shader gets in buffer 3. Kept in step with the MSL
 * and GLSL declarations dxso.c writes (the same layout under std140). */
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
