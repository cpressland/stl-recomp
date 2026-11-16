/*
 * d3d9_null.c - the GPU calls (d3d9_int.h) with no GPU behind them: off a
 * Mac until d3d9_vulkan.c is written (docs/linux.md). The device takes every
 * call and draws nothing, as the first host did (docs/PLAN.md, Phase 3), so
 * the game runs headless: its menus, its missions, their scripts and sound.
 */
#ifndef __APPLE__
#include <stdio.h>

#include "win32hle.h"
#include "stl.h"
#include "d3d9_int.h"

int  gpu_available(void) { return 0; }
void gpu_device(dev_t_ *d) { (void)d; }
void gpu_clear(dev_t_ *d, uint32_t nrects, uint32_t rects, uint32_t flags, uint32_t color, float z, uint32_t stencil) {
    (void)d, (void)nrects, (void)rects, (void)flags, (void)color, (void)z, (void)stencil;
}
void gpu_draw(dev_t_ *d, uint32_t prim, uint32_t count, uint32_t start, int indexed, int32_t base_vertex,
              uint32_t min_index, uint32_t num_vertices, uint32_t up_va, uint32_t up_stride,
              uint32_t up_idx_va, uint32_t up_idx_format) {
    (void)d, (void)prim, (void)count, (void)start, (void)indexed, (void)base_vertex, (void)min_index,
    (void)num_vertices, (void)up_va, (void)up_stride, (void)up_idx_va, (void)up_idx_format;
}
void gpu_present(dev_t_ *d) { (void)d; }
void gpu_stretch_rect(dev_t_ *d, uint32_t src, uint32_t src_rect, uint32_t dst, uint32_t dst_rect, uint32_t filter) {
    (void)d, (void)src, (void)src_rect, (void)dst, (void)dst_rect, (void)filter;
}
void gpu_readback(dev_t_ *d, res_t *r, uint32_t level, uint32_t face) { (void)d, (void)r, (void)level, (void)face; }
int  gpu_shot(dev_t_ *d, const char *path) { (void)d, (void)path; return -1; }
int  gpu_is_render_target(const res_t *r) { return r->usage & 1; }   /* D3DUSAGE_RENDERTARGET */
#endif
