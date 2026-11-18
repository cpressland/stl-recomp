/*
 * d3d9_vulkan.c - Direct3D 9 on Vulkan (Linux): the GPU side of d3d9.c, made
 * as d3d9_metal.m is on a Mac, part for part.
 *
 * d3d9.c keeps the device's state as D3D9 has it; at each draw this turns
 * it into Vulkan's: a dynamic-rendering pass for the render targets set
 * (ended and begun again when they change; a full clear is the pass's load
 * op, a partial one vkCmdClearAttachments), a pipeline for the shaders, the
 * vertex declaration, the targets' formats, the blend state and the
 * primitive (cached by all of them), and the rest set as dynamic state:
 * viewport, scissor, depth and stencil, culling, depth bias, blend colour.
 * StretchRect is vkCmdBlitImage.
 *
 * Clip space stays Direct3D's: the viewport has a negative height, so y is
 * flipped and framebuffer coordinates are Direct3D's pixels; front faces are
 * then clockwise, as Direct3D's are.
 *
 * Memory: textures are images, filled from their guest memory when a level
 * was written (Lock marks it), through this frame's ring; render targets
 * live on the GPU and come back on a lock or GetRenderTargetData. Static
 * vertex buffers get a buffer of their own, refilled when locked; dynamic
 * ones, the vertices of the *UP draws, every draw's indices and constants
 * are copied into this frame's ring, a buffer per frame in flight. Images
 * stay in the GENERAL layout, and one memory barrier comes between whatever
 * wrote and whatever follows (pcrecomp's d3d_vulkan.c does the same).
 *
 * Shaders are dxso.c's GLSL, compiled to SPIR-V as they come by shaderc
 * (libshaderc_shared.so.1), once each (a ps_1_x once for each kind of
 * texture it is used with). Draws with no vertex or pixel shader (fixed
 * function) are counted and skipped, as on the Mac.
 *
 * With a window, each Present ends its frame with a copy of the back buffer
 * into a buffer of its frame's, and the window shows the newest one the GPU
 * has finished (win32hle's presenter, as a GDI game's): no swapchain, and
 * the CPU does not wait for the GPU in a frame.
 *
 * Vulkan 1.3, the loader opened at run time (libvulkan.so.1). Without it,
 * or a GPU, or shaderc, the device draws nothing and the game runs on.
 * STL_VULKAN_DEBUG=1 turns on the validation layer, if installed;
 * STL_VULKAN_ANY=1 lets a software Vulkan (llvmpipe) draw;
 * STL_VULKAN_STATS=1 says every 5 seconds where a frame's time went (the
 * game's own code, this renderer, waiting for the GPU) and what it did.
 */
#if !defined(__APPLE__) || defined(STL_VULKAN_CHECK)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32hle.h"
#include "stl.h"
#include "d3d9_int.h"

#if defined(__has_include)
#if __has_include(<vulkan/vulkan.h>)
#define STL_HAVE_VULKAN 1
#endif
#endif

#ifndef STL_HAVE_VULKAN
/* No Vulkan headers: nothing drawn (the game runs, headless or not). */
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
int  gpu_is_render_target(const res_t *r) { return r->usage & 1; }
#else

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <time.h>

/* ---- the entry points, from the loader ------------------------------------------- */
#define VK_GLOBAL(X) X(vkCreateInstance) X(vkEnumerateInstanceVersion) X(vkEnumerateInstanceLayerProperties)
#define VK_INSTANCE(X) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFeatures) \
    X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceFormatProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr)
#define VK_DEVICE(X) X(vkGetDeviceQueue) X(vkQueueSubmit) X(vkDeviceWaitIdle) X(vkCreateCommandPool) \
    X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCreateFence) X(vkWaitForFences) \
    X(vkResetFences) X(vkGetFenceStatus) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory) X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkCreateImage) X(vkDestroyImage) \
    X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkCreateSampler) X(vkCreateShaderModule) X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) \
    X(vkCreatePipelineCache) X(vkCreateDescriptorPool) X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) \
    X(vkUpdateDescriptorSets) X(vkCreateGraphicsPipelines) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) \
    X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdSetViewport) \
    X(vkCmdSetScissor) X(vkCmdSetStencilReference) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) \
    X(vkCmdSetBlendConstants) X(vkCmdSetDepthBias) X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) \
    X(vkCmdCopyImageToBuffer) X(vkCmdBlitImage) X(vkCmdClearAttachments) X(vkCmdClearColorImage) \
    X(vkCmdClearDepthStencilImage) X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdSetCullMode) \
    X(vkCmdSetFrontFace) X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp) \
    X(vkCmdSetStencilTestEnable) X(vkCmdSetStencilOp) X(vkCmdSetDepthBiasEnable)
#define DECL(f) static PFN_##f f;
VK_GLOBAL(DECL) VK_INSTANCE(DECL) VK_DEVICE(DECL)
static PFN_vkGetInstanceProcAddr gipa;

/* ---- shaderc (libshaderc_shared.so.1), opened at run time: its C API ---------------- */
typedef struct shaderc_compiler *shaderc_compiler_t;
typedef struct shaderc_compile_options *shaderc_compile_options_t;
typedef struct shaderc_compilation_result *shaderc_compilation_result_t;
enum { SHADERC_VERTEX = 0, SHADERC_FRAGMENT = 1, SHADERC_TARGET_VULKAN = 0 };
#define SHADERC_ENV_VULKAN_1_3 ((1u << 22) | (3u << 12))
static struct {
    shaderc_compiler_t (*compiler_initialize)(void);
    shaderc_compile_options_t (*compile_options_initialize)(void);
    void (*compile_options_set_target_env)(shaderc_compile_options_t, int, uint32_t);
    shaderc_compilation_result_t (*compile_into_spv)(shaderc_compiler_t, const char *, size_t, int, const char *,
                                                     const char *, shaderc_compile_options_t);
    int (*result_get_compilation_status)(shaderc_compilation_result_t);
    size_t (*result_get_length)(shaderc_compilation_result_t);
    const char *(*result_get_bytes)(shaderc_compilation_result_t);
    const char *(*result_get_error_message)(shaderc_compilation_result_t);
    void (*result_release)(shaderc_compilation_result_t);
    shaderc_compiler_t compiler;
    shaderc_compile_options_t options;
} SC;

/* ---- what this side keeps --------------------------------------------------------------- */
#define FRAMES 3
#define MAX_POOLS 16
#define B_ZEROS 16                                    /* the vertex binding of zeros (an input no stream has) */

typedef struct { VkBuffer b; VkDeviceMemory m; uint8_t *map; VkDeviceSize size; } buf_t;

/* A texture's or surface's image. */
typedef struct {
    VkImage img;
    VkDeviceMemory mem;
    VkFormat vf;
    int convert, depth, stencil, block, rt;
    uint32_t w, h, d, levels, layers;
    VkImageView sample;                               /* every level (and face), for sampling */
    VkImageViewType sample_type;
    VkImageView att[96];                              /* one level of one face: an attachment */
} vtex_t;

/* A frame in flight: its commands, its ring, its descriptor sets, and its
 * copy of the back buffer for the window. */
typedef struct {
    VkCommandBuffer cb;
    VkFence fence;
    int submitted;
    buf_t ring;
    uint32_t used;
    buf_t *extra;                                     /* the ring was full inside a pass: buffers it went on in */
    int nextra;
    uint32_t extra_used;
    VkDescriptorPool pool[MAX_POOLS];
    int npool, pool_i;
    buf_t show;
    uint64_t show_seq;
} frame_t;

static void *g_lib, *g_sclib;
static VkInstance g_inst;
static VkPhysicalDevice g_pd;
static VkDevice g_dev;
static VkQueue g_q;
static uint32_t g_qfam;
static VkPhysicalDeviceMemoryProperties g_mem;
static VkPhysicalDeviceLimits g_limits;
static VkBool32 g_aniso, g_mirror_once, g_fill_lines;
static VkCommandPool g_cpool;
static VkDescriptorSetLayout g_dsl;
static VkPipelineLayout g_pl;
static VkPipelineCache g_pcache;
static buf_t g_zeros;                                 /* 256 bytes of 0: vertex inputs no stream has */
static vtex_t *g_blank[3];                            /* bound for a sampler with no texture: 2D, cube, 3D */
static VkFormat g_d24s8;                              /* the depth-stencil format: D32S8, else D24S8 */
static frame_t F[FRAMES];
static int g_fi, g_recording;
static int g_in_pass, g_need_barrier;
static uint32_t g_pass_rt[4], g_pass_ds;
static int g_pass_w, g_pass_h;
static VkFormat g_pass_fmt[4], g_pass_dfmt;
static uint32_t g_pass_ncolor, g_pass_stencil;
static uint32_t g_clear_flags, g_clear_color, g_clear_stencil;
static float g_clear_z;
static uint64_t g_frame, g_gen;                       /* gen: the ring restarted (a frame, or full) */
static uint64_t g_draws, g_skipped_ff, g_skipped_shader;
static int g_started, g_failed;

/* STL_VULKAN_STATS: time in this renderer (its calls, less their waits for
 * the GPU), the waits, and the rest of each frame (the game's own code). */
static int g_stats = -1, g_in_call;
static uint64_t st_t0, st_last_present, st_call_start;
static uint64_t st_paced, st_renderer, st_wait, st_compile, st_frames, st_draws, st_passes, st_up, st_shaders, st_pipes, st_frame_max;
static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static int stats(void) {
    if (g_stats < 0) g_stats = getenv("STL_VULKAN_STATS") != NULL;
    return g_stats;
}
static void call_begin(void) { if (stats() && !g_in_call++) st_call_start = now_ns(); }
static void call_end(void) { if (g_stats > 0 && !--g_in_call) st_renderer += now_ns() - st_call_start; }
/* A wait for the GPU: its time is the GPU's, not the renderer's. */
static uint64_t wait_begin(void) { return g_stats > 0 ? now_ns() : 0; }
static void wait_end(uint64_t t) { if (g_stats > 0) { uint64_t w = now_ns() - t; st_wait += w; st_renderer -= w; } }
static void stats_frame(void) {
    if (g_stats <= 0) return;
    uint64_t t = now_ns();
    if (st_last_present && t - st_last_present > st_frame_max) st_frame_max = t - st_last_present;
    st_last_present = t, st_frames++;
    if (!st_t0) { st_t0 = t; st_paced = st_renderer = st_wait = st_compile = st_frames = st_draws = st_passes = st_up = st_shaders = st_pipes = st_frame_max = 0; return; }
    if (t - st_t0 < 5000000000ull) return;
    double n = (double)st_frames, frame = (double)(t - st_t0) / n / 1e6;
    double ren = (double)st_renderer / n / 1e6, wait = (double)st_wait / n / 1e6, paced = (double)st_paced / n / 1e6;
    fprintf(stderr, "[vulkan] %.1f fps (slowest %.0f ms): a frame %.1f ms = game %.1f + renderer %.1f (compiling %.1f)"
                    " + waiting for the GPU %.1f + held to the display %.1f;"
                    " %.0f draws, %.1f passes, %.2f MB up a frame; %llu new shaders, %llu new pipelines\n",
            n * 1e9 / (double)(t - st_t0), (double)st_frame_max / 1e6, frame, frame - ren - wait - paced, ren,
            (double)st_compile / n / 1e6, wait, paced, (double)st_draws / n, (double)st_passes / n, (double)st_up / n / 1048576.0,
            (unsigned long long)st_shaders, (unsigned long long)st_pipes);
    st_t0 = t, st_paced = st_renderer = st_wait = st_compile = st_frames = st_draws = st_passes = st_up = st_shaders = st_pipes = st_frame_max = 0;
}

static void vk_fail(const char *what, VkResult r) {
    if (g_failed++ < 10) fprintf(stderr, "[vulkan] %s failed (%d)\n", what, (int)r);
}
#define VKC(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) vk_fail(#call, r_); } while (0)

/* STL_TRACE_FRAME=N: every draw, clear and StretchRect of frame N, said. */
static int tracing(void) {
    static long want = -2;
    if (want == -2) { const char *e = getenv("STL_TRACE_FRAME"); want = e ? atol(e) : -1; }
    return g_started > 0 && want >= 0 && (long)g_frame == want;
}
#define TRACE(...) do { if (tracing()) fprintf(stderr, "[trace] " __VA_ARGS__); } while (0)

/* Each frame's ring of dynamic data: 64 MB (STL_RING_MB for a test). */
static uint32_t ring_size(void) {
    static uint32_t n;
    if (!n) { const char *e = getenv("STL_RING_MB"); n = (e && atoi(e) > 0 ? (uint32_t)atoi(e) : 64u) << 20; }
    return n;
}

/* ---- a small hash map: fixed-size keys to 64-bit values ------------------------------- */
typedef struct { uint8_t *keys; uint64_t *vals; uint8_t *used; uint32_t cap, n, ksize; } map_t;
static uint64_t hash(const void *k, uint32_t n) {
    const uint8_t *p = (const uint8_t *)k;
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
static uint64_t *map_slot(map_t *m, const void *key, int insert);
static void map_grow(map_t *m) {
    map_t o = *m;
    m->cap = o.cap ? o.cap * 2 : 256, m->n = 0;
    m->keys = (uint8_t *)calloc(m->cap, m->ksize), m->vals = (uint64_t *)calloc(m->cap, 8), m->used = (uint8_t *)calloc(m->cap, 1);
    for (uint32_t i = 0; i < o.cap; i++)
        if (o.used[i]) *map_slot(m, o.keys + (size_t)i * o.ksize, 1) = o.vals[i];
    free(o.keys), free(o.vals), free(o.used);
}
/* The value's place for key: NULL if absent and not inserting. */
static uint64_t *map_slot(map_t *m, const void *key, int insert) {
    if (insert && (m->n + 1) * 2 > m->cap) map_grow(m);
    if (!m->cap) return NULL;
    for (uint32_t i = (uint32_t)(hash(key, m->ksize) & (m->cap - 1));; i = (i + 1) & (m->cap - 1)) {
        uint8_t *k = m->keys + (size_t)i * m->ksize;
        if (!m->used[i]) {
            if (!insert) return NULL;
            memcpy(k, key, m->ksize), m->used[i] = 1, m->n++, m->vals[i] = 0;
            return &m->vals[i];
        }
        if (!memcmp(k, key, m->ksize)) return &m->vals[i];
    }
}

/* ---- memory, buffers, images ------------------------------------------------------------ */
static int mem_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer) {
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < g_mem.memoryTypeCount; i++) {
            VkMemoryPropertyFlags f = g_mem.memoryTypes[i].propertyFlags;
            if (!(bits & (1u << i)) || (f & want) != want) continue;
            if (pass == 0 && (f & prefer) != prefer) continue;
            return (int)i;
        }
    return -1;
}
/* A buffer the CPU writes (readback 0) or reads (readback 1), mapped. */
static buf_t buf_new(VkDeviceSize size, VkBufferUsageFlags use, int readback) {
    buf_t o;
    memset(&o, 0, sizeof o);
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size, bi.usage = use, bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_dev, &bi, NULL, &o.b) != VK_SUCCESS) { vk_fail("vkCreateBuffer", VK_ERROR_UNKNOWN); return o; }
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g_dev, o.b, &mr);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = mr.size;
    int t = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     readback ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0);
    ai.memoryTypeIndex = (uint32_t)(t < 0 ? 0 : t);
    VKC(vkAllocateMemory(g_dev, &ai, NULL, &o.m));
    VKC(vkBindBufferMemory(g_dev, o.b, o.m, 0));
    void *p = NULL;
    VKC(vkMapMemory(g_dev, o.m, 0, VK_WHOLE_SIZE, 0, &p));
    o.map = (uint8_t *)p, o.size = size;
    return o;
}
static void buf_free(buf_t *o) {
    if (o->b) vkDestroyBuffer(g_dev, o->b, NULL);
    if (o->m) vkFreeMemory(g_dev, o->m, NULL);
    memset(o, 0, sizeof *o);
}

/* ---- command buffers, frames, the ring ------------------------------------------------- */
static VkCommandBuffer cmd(void) {
    frame_t *f = &F[g_fi];
    if (!g_recording) {
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VKC(vkBeginCommandBuffer(f->cb, &bi));
        g_recording = 1, g_need_barrier = 1;
    }
    return f->cb;
}
/* One barrier between whatever wrote and whatever comes next (outside a pass). */
static void barrier(void) {
    if (!g_need_barrier) return;
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb,
                         0, NULL, 0, NULL);
    g_need_barrier = 0;
}
static void end_pass(void) {
    if (g_in_pass) { vkCmdEndRendering(cmd()); g_in_pass = 0, g_need_barrier = 1; }
    memset(g_pass_rt, 0, sizeof g_pass_rt), g_pass_ds = 0;
}
/* This frame's commands so far to the GPU; wait (1) for them, and go on
 * recording in the same frame. */
static void submit(int wait) {
    end_pass();
    frame_t *f = &F[g_fi];
    if (g_recording) {
        if (wait) {                                   /* the CPU reads what was copied */
            VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(f->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0,
                                 NULL, 0, NULL);
        }
        VKC(vkEndCommandBuffer(f->cb));
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1, si.pCommandBuffers = &f->cb;
        VKC(vkQueueSubmit(g_q, 1, &si, f->fence));
        f->submitted = 1, g_recording = 0;
    }
    if (wait && f->submitted) {
        uint64_t wt = wait_begin();
        VKC(vkWaitForFences(g_dev, 1, &f->fence, VK_TRUE, UINT64_MAX));
        wait_end(wt);
        VKC(vkResetFences(g_dev, 1, &f->fence));
        f->submitted = 0;
    }
}
/* A frame's things, once the GPU is done with it. */
static void frame_reuse(frame_t *f) {
    if (f->submitted) {
        uint64_t wt = wait_begin();
        VKC(vkWaitForFences(g_dev, 1, &f->fence, VK_TRUE, UINT64_MAX));
        wait_end(wt);
        VKC(vkResetFences(g_dev, 1, &f->fence));
        f->submitted = 0;
    }
    for (int i = 0; i < f->nextra; i++) buf_free(&f->extra[i]);
    f->nextra = 0, f->extra_used = 0, f->used = 0;
    for (int i = 0; i < f->npool; i++) vkResetDescriptorPool(g_dev, f->pool[i], 0);
    f->pool_i = 0;
}

/* n bytes of this frame's (256-byte aligned): the buffer, where, and the CPU's view.
 *
 * A frame that fills its ring outside a pass waits for the GPU and starts the
 * ring again (gen moves on, so dynamic buffers are copied anew); inside a
 * pass it goes on in a buffer of its own, kept until the GPU is done with
 * the frame (d3d9_metal.m: ending a pass under a draw crashed). */
typedef struct { VkBuffer b; uint32_t off; uint8_t *p; } ralloc_t;
static ralloc_t ring_alloc(uint32_t n) {
    frame_t *f = &F[g_fi];
    ralloc_t r = { VK_NULL_HANDLE, 0, NULL };
    n = (n + 255) & ~255u;
    if (!f->nextra && f->used + n > ring_size() && !g_in_pass && n <= ring_size()) {
        submit(1);
        f->used = 0, g_gen++;
    }
    if (!f->nextra && f->used + n <= ring_size()) {
        r.b = f->ring.b, r.off = f->used, r.p = f->ring.map + f->used;
        f->used += n;
        return r;
    }
    if (f->nextra && f->extra_used + n <= f->extra[f->nextra - 1].size) {
        buf_t *x = &f->extra[f->nextra - 1];
        r.b = x->b, r.off = f->extra_used, r.p = x->map + f->extra_used;
        f->extra_used += n;
        return r;
    }
    static int said;
    if (!said++) fprintf(stderr, "[vulkan] a frame needed more than its %u MB ring: it goes on in another\n", ring_size() >> 20);
    f->extra = (buf_t *)realloc(f->extra, sizeof *f->extra * (size_t)(f->nextra + 1));
    f->extra[f->nextra] = buf_new(n > ring_size() ? n : ring_size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0);
    if (!f->extra[f->nextra].b) return r;
    buf_t *x = &f->extra[f->nextra++];
    r.b = x->b, r.off = 0, r.p = x->map;
    f->extra_used = n;
    return r;
}
static ralloc_t ring_copy(const void *src, uint32_t n) {
    ralloc_t r = ring_alloc(n);
    if (r.p) memcpy(r.p, src, n);
    return r;
}

/* ---- formats ------------------------------------------------------------------------------ */
enum { CV_NONE, CV_565, CV_1555, CV_X1555, CV_4444, CV_888 };
typedef struct { VkFormat vf; int convert; VkComponentMapping swz; int has_swz; } fmt_t;

#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define SW(r, g, b, a) ((VkComponentMapping){ VK_COMPONENT_SWIZZLE_##r, VK_COMPONENT_SWIZZLE_##g, VK_COMPONENT_SWIZZLE_##b, VK_COMPONENT_SWIZZLE_##a })
static fmt_t fmt(uint32_t f) {
    fmt_t x = { VK_FORMAT_B8G8R8A8_UNORM, CV_NONE, SW(IDENTITY, IDENTITY, IDENTITY, IDENTITY), 0 };
    switch (f) {
    case 21: break;                                                          /* A8R8G8B8 */
    case 22: x.has_swz = 1, x.swz = SW(R, G, B, ONE); break;                 /* X8R8G8B8 */
    case 20: x.convert = CV_888, x.has_swz = 1, x.swz = SW(R, G, B, ONE); break;
    case 23: x.convert = CV_565; break;
    case 24: x.convert = CV_X1555; break;
    case 25: x.convert = CV_1555; break;
    case 26: x.convert = CV_4444; break;
    case 28: x.vf = VK_FORMAT_R8_UNORM, x.has_swz = 1, x.swz = SW(ZERO, ZERO, ZERO, R); break;       /* A8 */
    case 31: x.vf = VK_FORMAT_A2B10G10R10_UNORM_PACK32; break;
    case 32: x.vf = VK_FORMAT_R8G8B8A8_UNORM; break;                         /* A8B8G8R8 */
    case 33: x.vf = VK_FORMAT_R8G8B8A8_UNORM, x.has_swz = 1, x.swz = SW(R, G, B, ONE); break;
    case 34: x.vf = VK_FORMAT_R16G16_UNORM; break;
    case 35: x.vf = VK_FORMAT_A2R10G10B10_UNORM_PACK32; break;
    case 36: x.vf = VK_FORMAT_R16G16B16A16_UNORM; break;
    case 50: x.vf = VK_FORMAT_R8_UNORM, x.has_swz = 1, x.swz = SW(R, R, R, ONE); break;              /* L8 */
    case 51: x.vf = VK_FORMAT_R8G8_UNORM, x.has_swz = 1, x.swz = SW(R, R, R, G); break;             /* A8L8 */
    case 81: x.vf = VK_FORMAT_R16_UNORM, x.has_swz = 1, x.swz = SW(R, R, R, ONE); break;            /* L16 */
    case 60: x.vf = VK_FORMAT_R8G8_SNORM, x.has_swz = 1, x.swz = SW(R, G, ONE, ONE); break;         /* V8U8 */
    case 63: x.vf = VK_FORMAT_R8G8B8A8_SNORM; break;                         /* Q8W8V8U8 */
    case 111: x.vf = VK_FORMAT_R16_SFLOAT; break;
    case 112: x.vf = VK_FORMAT_R16G16_SFLOAT; break;
    case 113: x.vf = VK_FORMAT_R16G16B16A16_SFLOAT; break;
    case 114: x.vf = VK_FORMAT_R32_SFLOAT; break;
    case 115: x.vf = VK_FORMAT_R32G32_SFLOAT; break;
    case 116: x.vf = VK_FORMAT_R32G32B32A32_SFLOAT; break;
    case 75: case 73: case 79: x.vf = g_d24s8; break;                        /* D24S8, D15S1, D24X4S4 */
    case 71: case 77: case 80: case 82: x.vf = VK_FORMAT_D32_SFLOAT; break;  /* D32, D24X8, D16, D24FS8 */
    case FOURCC('D', 'X', 'T', '1'): x.vf = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; break;
    case FOURCC('D', 'X', 'T', '2'): case FOURCC('D', 'X', 'T', '3'): x.vf = VK_FORMAT_BC2_UNORM_BLOCK; break;
    case FOURCC('D', 'X', 'T', '4'): case FOURCC('D', 'X', 'T', '5'): x.vf = VK_FORMAT_BC3_UNORM_BLOCK; break;
    }
    return x;
}
static int is_depth_vf(VkFormat f) {
    return f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT;
}
static VkFormatFeatureFlags features(VkFormat f) {
    static map_t m = { 0 };
    if (!m.ksize) m.ksize = sizeof f;
    uint64_t *v = map_slot(&m, &f, 0);
    if (v) return (VkFormatFeatureFlags)*v;
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(g_pd, f, &p);
    *map_slot(&m, &f, 1) = p.optimalTilingFeatures;
    return p.optimalTilingFeatures;
}

int gpu_is_render_target(const res_t *r) {
    const res_t *t = r->parent ? OBJ(r->parent, res_t) : r;
    return (t->usage & 3) && t->gpu != NULL;                 /* D3DUSAGE_RENDERTARGET, _DEPTHSTENCIL */
}

/* ---- textures -------------------------------------------------------------------------------- */
static res_t *owner(res_t *r) { return r->parent ? OBJ(r->parent, res_t) : r; }

static VkImageView view(vtex_t *t, VkImageViewType type, uint32_t level, uint32_t nlevels, uint32_t layer,
                        uint32_t nlayers, int swizzle, VkComponentMapping swz, int depth_only) {
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = t->img, vi.viewType = type, vi.format = t->vf;
    if (swizzle) vi.components = swz;
    vi.subresourceRange.aspectMask = !t->depth ? VK_IMAGE_ASPECT_COLOR_BIT
                                   : depth_only || !t->stencil ? VK_IMAGE_ASPECT_DEPTH_BIT
                                   : VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    vi.subresourceRange.baseMipLevel = level, vi.subresourceRange.levelCount = nlevels;
    vi.subresourceRange.baseArrayLayer = layer, vi.subresourceRange.layerCount = nlayers;
    VkImageView v = VK_NULL_HANDLE;
    VKC(vkCreateImageView(g_dev, &vi, NULL, &v));
    return v;
}

/* A new image: made, put in the GENERAL layout, and cleared (what Metal's
 * new textures are: zeros; a depth buffer: far, stencil 0). */
static vtex_t *image_new(VkFormat vf, uint32_t w, uint32_t h, uint32_t d, uint32_t levels, int kind, int want_rt,
                         int convert, int has_swz, VkComponentMapping swz) {
    VkFormatFeatureFlags ff = features(vf);
    if (!(ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) && !is_depth_vf(vf)) {
        static int said;
        if (said++ < 5) fprintf(stderr, "[vulkan] format %d cannot be sampled here: drawn as BGRA8\n", (int)vf);
        vf = VK_FORMAT_B8G8R8A8_UNORM, convert = CV_NONE, ff = features(vf);
    }
    vtex_t *t = (vtex_t *)calloc(1, sizeof *t);
    t->vf = vf, t->convert = convert;
    t->depth = is_depth_vf(vf), t->stencil = vf == VK_FORMAT_D32_SFLOAT_S8_UINT || vf == VK_FORMAT_D24_UNORM_S8_UINT;
    t->block = vf == VK_FORMAT_BC1_RGBA_UNORM_BLOCK || vf == VK_FORMAT_BC2_UNORM_BLOCK || vf == VK_FORMAT_BC3_UNORM_BLOCK;
    t->w = w ? w : 1, t->h = h ? h : 1, t->d = kind == R_VOLUMETEXTURE ? (d ? d : 1) : 1;
    t->levels = levels ? levels : 1, t->layers = kind == R_CUBETEXTURE ? 6 : 1;
    if (kind == R_CUBETEXTURE) t->h = t->w;
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = kind == R_VOLUMETEXTURE ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ii.flags = kind == R_CUBETEXTURE ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ii.format = vf, ii.extent.width = t->w, ii.extent.height = t->h, ii.extent.depth = t->d;
    ii.mipLevels = t->levels, ii.arrayLayers = t->layers, ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL, ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (t->depth) ii.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, t->rt = 1;
    else if (want_rt && kind != R_VOLUMETEXTURE && (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
        ii.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, t->rt = 1;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(g_dev, &ii, NULL, &t->img) != VK_SUCCESS) {
        fprintf(stderr, "[vulkan] cannot make a %ux%u image of format %d\n", w, h, (int)vf);
        free(t);
        return NULL;
    }
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g_dev, t->img, &mr);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = mr.size;
    int mt = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (mt < 0) mt = mem_type(mr.memoryTypeBits, 0, 0);
    ai.memoryTypeIndex = (uint32_t)(mt < 0 ? 0 : mt);
    VKC(vkAllocateMemory(g_dev, &ai, NULL, &t->mem));
    VKC(vkBindImageMemory(g_dev, t->img, t->mem, 0));
    t->sample_type = kind == R_CUBETEXTURE ? VK_IMAGE_VIEW_TYPE_CUBE : kind == R_VOLUMETEXTURE ? VK_IMAGE_VIEW_TYPE_3D
                                                                                                : VK_IMAGE_VIEW_TYPE_2D;
    t->sample = view(t, t->sample_type, 0, t->levels, 0, t->layers, has_swz && !t->rt, swz, 1);

    /* into GENERAL, and cleared: outside a pass */
    end_pass();
    VkImageSubresourceRange all = { 0, 0, t->levels, 0, t->layers };
    all.aspectMask = !t->depth ? VK_IMAGE_ASPECT_COLOR_BIT
                   : t->stencil ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    VkImageMemoryBarrier ib = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    ib.srcAccessMask = 0, ib.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = t->img, ib.subresourceRange = all;
    vkCmdPipelineBarrier(cmd(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                         NULL, 1, &ib);
    if (t->depth) {
        VkClearDepthStencilValue z = { 1.0f, 0 };
        vkCmdClearDepthStencilImage(cmd(), t->img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &all);
    } else if (!t->block) {
        VkClearColorValue c;
        memset(&c, 0, sizeof c);
        vkCmdClearColorImage(cmd(), t->img, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &all);
    }
    g_need_barrier = 1;
    return t;
}

static vtex_t *texture_of(res_t *r) {
    res_t *o = owner(r);
    if (!o->gpu) {
        fmt_t f = fmt(o->format);
        vtex_t *t = image_new(f.vf, o->width, o->height, o->depth, o->levels, (int)o->kind,
                              (o->usage & 3) || o->kind == R_SURFACE, f.convert, f.has_swz, f.swz);
        if (!t) return NULL;
        o->gpu = t;
        /* everything already written, and the levels never written are zero */
        for (int f2 = 0; f2 < 6; f2++) o->dirty[f2] = 0;
        for (int f2 = 0; f2 < 6; f2++) for (int l = 0; l < 16; l++) if (o->mem[(f2 * 16 + l) % 96]) o->dirty[f2] |= 1u << l;
    }
    return (vtex_t *)o->gpu;
}
/* One level of one face, as an attachment. */
static VkImageView att_view(vtex_t *t, uint32_t level, uint32_t face) {
    uint32_t i = (face * 16 + level) % 96;
    if (!t->att[i]) t->att[i] = view(t, VK_IMAGE_VIEW_TYPE_2D, level, 1, face, 1, 0, SW(IDENTITY, IDENTITY, IDENTITY, IDENTITY), 0);
    return t->att[i];
}

static void convert_row(int cv, const uint8_t *src, uint8_t *dst, uint32_t w) {
    for (uint32_t x = 0; x < w; x++) {
        uint32_t r, g, b, a = 255;
        if (cv == CV_888) { b = src[3 * x], g = src[3 * x + 1], r = src[3 * x + 2]; }
        else {
            uint16_t v;
            memcpy(&v, src + 2 * x, 2);
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

/* The levels written since they went up, up (outside a pass). */
static void sync_texture(res_t *r) {
    res_t *o = owner(r);
    vtex_t *t = texture_of(o);
    if (!t) return;
    int faces = o->kind == R_CUBETEXTURE ? 6 : 1, any = 0;
    for (int f = 0; f < faces; f++) any |= o->dirty[f] != 0;
    if (!any) return;
    if (t->depth) { for (int f = 0; f < 6; f++) o->dirty[f] = 0; return; }
    end_pass();
    barrier();
    int bpp = bits_per_pixel(o->format), bb = block_bytes(o->format);
    for (int f = 0; f < faces; f++) {
        uint32_t bits = o->dirty[f];
        o->dirty[f] = 0;
        for (uint32_t l = 0; bits && l < t->levels; l++, bits >>= 1) {
            if (!(bits & 1) || !o->mem[(f * 16 + l) % 96]) continue;
            uint32_t pitch, slice, mem = level_mem(o, l, (uint32_t)f, &pitch, &slice);
            uint32_t w = level_dim(t->w, l), h = level_dim(t->h, l), d = level_dim(t->d, l);
            const uint8_t *src = (const uint8_t *)hle_ptr(mem);
            VkBufferImageCopy rg;
            memset(&rg, 0, sizeof rg);
            ralloc_t a;
            if (t->convert) {
                a = ring_alloc(w * 4 * h * d);
                if (!a.p) return;
                for (uint32_t z = 0; z < d; z++)
                    for (uint32_t y = 0; y < h; y++)
                        convert_row(t->convert, src + z * slice + y * pitch, a.p + ((size_t)z * h + y) * w * 4, w);
                rg.bufferRowLength = w, rg.bufferImageHeight = h;
            } else {
                a = ring_copy(src, slice * d);
                if (!a.p) return;
                if (bb) rg.bufferRowLength = pitch / (uint32_t)bb * 4, rg.bufferImageHeight = slice / pitch * 4;
                else rg.bufferRowLength = bpp >= 8 ? pitch * 8 / (uint32_t)bpp : w, rg.bufferImageHeight = slice / pitch;
            }
            rg.bufferOffset = a.off;
            rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            rg.imageSubresource.mipLevel = l, rg.imageSubresource.baseArrayLayer = (uint32_t)f, rg.imageSubresource.layerCount = 1;
            rg.imageExtent.width = w, rg.imageExtent.height = h, rg.imageExtent.depth = d;
            vkCmdCopyBufferToImage(cmd(), a.b, t->img, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
            st_up += t->convert ? (uint64_t)w * 4 * h * d : (uint64_t)slice * d;
        }
    }
    g_need_barrier = 1;
}

/* A vertex buffer's bytes on the GPU: its buffer and offset. */
static VkBuffer buffer_of(res_t *r, uint32_t *offset) {
    *offset = 0;
    if (!(r->usage & 0x200)) {                              /* static: its own buffer, refilled when locked */
        if (!r->gpu) {
            buf_t *b = (buf_t *)calloc(1, sizeof *b);
            *b = buf_new(r->size ? r->size : 16, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 0);
            r->gpu = b;
            r->dirty[0] = 1;
        }
        buf_t *b = (buf_t *)r->gpu;
        if (r->dirty[0] && b->map) { memcpy(b->map, hle_ptr(r->mem[0]), r->size); r->dirty[0] = 0; }
        return b->b;
    }
    if (r->dirty[0] || r->gpu_frame != g_gen + 1 || !r->gpu_ring) {   /* dynamic: into the ring when it changed */
        ralloc_t a = ring_copy(hle_ptr(r->mem[0]), r->size);
        r->gpu_offset = a.off;
        r->gpu_ring = (void *)(uintptr_t)a.b;
        r->gpu_frame = g_gen + 1;
        r->dirty[0] = 0;
    }
    *offset = r->gpu_offset;
    return (VkBuffer)(uintptr_t)r->gpu_ring;
}

/* ---- starting ------------------------------------------------------------------------------- */
static int has_layer(const char *name) {
    uint32_t n = 0;
    if (!vkEnumerateInstanceLayerProperties || vkEnumerateInstanceLayerProperties(&n, NULL) != VK_SUCCESS) return 0;
    VkLayerProperties *p = (VkLayerProperties *)calloc(n ? n : 1, sizeof *p);
    vkEnumerateInstanceLayerProperties(&n, p);
    int found = 0;
    for (uint32_t i = 0; i < n; i++) if (!strcmp(p[i].layerName, name)) found = 1;
    free(p);
    return found;
}
static int pick_device(void) {
    uint32_t n = 0;
    if (vkEnumeratePhysicalDevices(g_inst, &n, NULL) != VK_SUCCESS || !n) return -1;
    VkPhysicalDevice *pds = (VkPhysicalDevice *)calloc(n, sizeof *pds);
    vkEnumeratePhysicalDevices(g_inst, &n, pds);
    int best = -1, best_score = 0, any = getenv("STL_VULKAN_ANY") != NULL;
    uint32_t best_q = 0;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(pds[i], &pp);
        if (pp.apiVersion < VK_API_VERSION_1_3) continue;
        VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        f2.pNext = &f13;
        vkGetPhysicalDeviceFeatures2(pds[i], &f2);
        if (!f13.dynamicRendering || !f13.shaderDemoteToHelperInvocation) continue;
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &nq, NULL);
        VkQueueFamilyProperties *qp = (VkQueueFamilyProperties *)calloc(nq ? nq : 1, sizeof *qp);
        vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &nq, qp);
        int q = -1;
        for (uint32_t j = 0; j < nq && q < 0; j++) if (qp[j].queueFlags & VK_QUEUE_GRAPHICS_BIT) q = (int)j;
        free(qp);
        if (q < 0) continue;
        int score = pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 4
                  : pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 3
                  : pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU ? 2
                  : pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? (any ? 1 : 0) : 1;
        if (score > best_score) best = (int)i, best_score = score, best_q = (uint32_t)q;
    }
    if (best >= 0) g_pd = pds[best], g_qfam = best_q;
    free(pds);
    return best;
}
static int start_shaderc(void) {
    g_sclib = dlopen("libshaderc_shared.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!g_sclib) g_sclib = dlopen("libshaderc_shared.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_sclib) { fprintf(stderr, "[vulkan] no shaderc (libshaderc_shared.so.1) for the game's shaders\n"); return -1; }
#define SCF(field, name) if (!(*(void **)&SC.field = dlsym(g_sclib, "shaderc_" name))) { fprintf(stderr, "[vulkan] shaderc lacks %s\n", name); return -1; }
    SCF(compiler_initialize, "compiler_initialize")
    SCF(compile_options_initialize, "compile_options_initialize")
    SCF(compile_options_set_target_env, "compile_options_set_target_env")
    SCF(compile_into_spv, "compile_into_spv")
    SCF(result_get_compilation_status, "result_get_compilation_status")
    SCF(result_get_length, "result_get_length")
    SCF(result_get_bytes, "result_get_bytes")
    SCF(result_get_error_message, "result_get_error_message")
    SCF(result_release, "result_release")
    SC.compiler = SC.compiler_initialize();
    SC.options = SC.compile_options_initialize();
    if (!SC.compiler || !SC.options) return -1;
    SC.compile_options_set_target_env(SC.options, SHADERC_TARGET_VULKAN, SHADERC_ENV_VULKAN_1_3);
    return 0;
}
static int start(void) {
    if (g_started) return g_started > 0 ? 0 : -1;
    g_started = -1;
    g_lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!g_lib) g_lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_lib) { fprintf(stderr, "[vulkan] no Vulkan loader (libvulkan.so.1): nothing is drawn\n"); return -1; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(g_lib, "vkGetInstanceProcAddr");
    if (!gipa) return -1;
#define LOAD_G(f) f = (PFN_##f)gipa(VK_NULL_HANDLE, #f);
    VK_GLOBAL(LOAD_G)
    uint32_t ver = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&ver);
    if (!vkCreateInstance || ver < VK_API_VERSION_1_3) {
        fprintf(stderr, "[vulkan] Vulkan 1.3 needed (the loader has %u.%u): nothing is drawn\n",
                VK_API_VERSION_MAJOR(ver), VK_API_VERSION_MINOR(ver));
        return -1;
    }
    if (start_shaderc() != 0) { fprintf(stderr, "[vulkan] nothing is drawn\n"); return -1; }
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "Star Trek: Legacy", app.pEngineName = "stl-recomp", app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ii = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ii.pApplicationInfo = &app;
    const char *layer = "VK_LAYER_KHRONOS_validation";
    if (getenv("STL_VULKAN_DEBUG")) {
        if (has_layer(layer)) ii.enabledLayerCount = 1, ii.ppEnabledLayerNames = &layer;
        else fprintf(stderr, "[vulkan] STL_VULKAN_DEBUG: no validation layer installed\n");
    }
    VkResult r = vkCreateInstance(&ii, NULL, &g_inst);
    if (r != VK_SUCCESS) { fprintf(stderr, "[vulkan] no instance (%d): nothing is drawn\n", (int)r); return -1; }
#define LOAD_I(f) f = (PFN_##f)gipa(g_inst, #f);
    VK_INSTANCE(LOAD_I)
    if (pick_device() < 0) { fprintf(stderr, "[vulkan] no GPU with Vulkan 1.3: nothing is drawn\n"); return -1; }
    VkPhysicalDeviceFeatures have;
    vkGetPhysicalDeviceFeatures(g_pd, &have);
    VkPhysicalDeviceVulkan12Features h12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceFeatures2 hf2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    hf2.pNext = &h12;
    vkGetPhysicalDeviceFeatures2(g_pd, &hf2);
    g_aniso = have.samplerAnisotropy, g_fill_lines = have.fillModeNonSolid, g_mirror_once = h12.samplerMirrorClampToEdge;
    float prio = 1;
    VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qi.queueFamilyIndex = g_qfam, qi.queueCount = 1, qi.pQueuePriorities = &prio;
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    f13.dynamicRendering = VK_TRUE, f13.shaderDemoteToHelperInvocation = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    f12.pNext = &f13, f12.samplerMirrorClampToEdge = g_mirror_once;
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    f2.pNext = &f12;
    f2.features.samplerAnisotropy = g_aniso, f2.features.fillModeNonSolid = g_fill_lines;
    f2.features.textureCompressionBC = have.textureCompressionBC;
    VkDeviceCreateInfo di = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    di.pNext = &f2, di.queueCreateInfoCount = 1, di.pQueueCreateInfos = &qi;
    r = vkCreateDevice(g_pd, &di, NULL, &g_dev);
    if (r != VK_SUCCESS) { fprintf(stderr, "[vulkan] no device (%d): nothing is drawn\n", (int)r); return -1; }
#define LOAD_D(f) f = (PFN_##f)vkGetDeviceProcAddr(g_dev, #f); if (!f) { fprintf(stderr, "[vulkan] no %s\n", #f); return -1; }
    VK_DEVICE(LOAD_D)
    vkGetDeviceQueue(g_dev, g_qfam, 0, &g_q);
    vkGetPhysicalDeviceMemoryProperties(g_pd, &g_mem);
    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(g_pd, &pp);
    g_limits = pp.limits;
    g_d24s8 = (features(VK_FORMAT_D32_SFLOAT_S8_UINT) & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
            ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D24_UNORM_S8_UINT;

    VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, pi.queueFamilyIndex = g_qfam;
    VKC(vkCreateCommandPool(g_dev, &pi, NULL, &g_cpool));
    for (int i = 0; i < FRAMES; i++) {
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = g_cpool, ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, ai.commandBufferCount = 1;
        VKC(vkAllocateCommandBuffers(g_dev, &ai, &F[i].cb));
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VKC(vkCreateFence(g_dev, &fi, NULL, &F[i].fence));
        F[i].ring = buf_new(ring_size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                            VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0);
    }
    /* one set per draw: the vertex shader's constants 0-3, the pixel shader's 4-7, samplers 8-23 (dxso.h) */
    VkDescriptorSetLayoutBinding lb[24];
    memset(lb, 0, sizeof lb);
    for (uint32_t i = 0; i < 24; i++) {
        lb[i].binding = i, lb[i].descriptorCount = 1;
        lb[i].descriptorType = i < 8 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        lb[i].stageFlags = i < 4 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 24, li.pBindings = lb;
    VKC(vkCreateDescriptorSetLayout(g_dev, &li, NULL, &g_dsl));
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1, pli.pSetLayouts = &g_dsl;
    VKC(vkCreatePipelineLayout(g_dev, &pli, NULL, &g_pl));
    VkPipelineCacheCreateInfo pci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    VKC(vkCreatePipelineCache(g_dev, &pci, NULL, &g_pcache));
    g_zeros = buf_new(256, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 0);
    if (g_zeros.map) memset(g_zeros.map, 0, 256);
    if (g_failed) { fprintf(stderr, "[vulkan] could not start: nothing is drawn\n"); return -1; }
    g_started = 1;
    VkComponentMapping id = SW(IDENTITY, IDENTITY, IDENTITY, IDENTITY);
    g_blank[0] = image_new(VK_FORMAT_B8G8R8A8_UNORM, 1, 1, 1, 1, R_TEXTURE, 0, CV_NONE, 0, id);
    g_blank[1] = image_new(VK_FORMAT_B8G8R8A8_UNORM, 1, 1, 1, 1, R_CUBETEXTURE, 0, CV_NONE, 0, id);
    g_blank[2] = image_new(VK_FORMAT_B8G8R8A8_UNORM, 1, 1, 1, 1, R_VOLUMETEXTURE, 0, CV_NONE, 0, id);
    submit(1);
    fprintf(stderr, "[vulkan] %s (Vulkan %u.%u)\n", pp.deviceName, VK_API_VERSION_MAJOR(pp.apiVersion),
            VK_API_VERSION_MINOR(pp.apiVersion));
    return 0;
}

int gpu_available(void) { return start() == 0; }

void gpu_device(dev_t_ *d) {
    if (start() != 0) return;
    end_pass();
    submit(1);
    vkDeviceWaitIdle(g_dev);                          /* the old back buffer's images: nothing uses them now */
    res_t *bb = OBJ(d->backbuffer, res_t);
    bb->gpu = NULL;                                   /* (its images are left: a Reset is rare) */
    texture_of(bb);
    if (d->depth) {
        res_t *z = OBJ(d->depth, res_t);
        z->width = bb->width, z->height = bb->height;
        z->gpu = NULL;
        texture_of(z);
    }
}

/* ---- render passes -------------------------------------------------------------------------- */
static uint32_t lvl(const res_t *s) { return s->parent ? s->level : 0; }
static uint32_t face(const res_t *s) { return s->parent ? s->face : 0; }

/* The open pass, for the render targets now set (begun again if they changed). */
static int pass(dev_t_ *d) {
    if (g_in_pass && !memcmp(g_pass_rt, d->rt, sizeof g_pass_rt) && g_pass_ds == d->ds && !g_clear_flags) return 1;
    end_pass();
    VkRenderingAttachmentInfo ca[4], da;
    memset(ca, 0, sizeof ca), memset(&da, 0, sizeof da);
    int w = 0, h = 0;
    uint32_t ncolor = 0;
    memset(g_pass_fmt, 0, sizeof g_pass_fmt), g_pass_dfmt = VK_FORMAT_UNDEFINED, g_pass_stencil = 0;
    for (int i = 0; i < 4; i++) {
        ca[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        if (!d->rt[i]) continue;
        res_t *s = OBJ(d->rt[i], res_t);
        vtex_t *t = texture_of(s);
        if (!t || !t->rt || t->depth) continue;
        sync_texture(s);
        uint32_t tw = level_dim(t->w, lvl(s)), th = level_dim(t->h, lvl(s));
        ca[i].imageView = att_view(t, lvl(s), face(s));
        ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        ca[i].loadOp = (g_clear_flags & 1) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        uint32_t c = g_clear_color;
        ca[i].clearValue.color.float32[0] = ((c >> 16) & 255) / 255.0f, ca[i].clearValue.color.float32[1] = ((c >> 8) & 255) / 255.0f;
        ca[i].clearValue.color.float32[2] = (c & 255) / 255.0f, ca[i].clearValue.color.float32[3] = (c >> 24) / 255.0f;
        g_pass_fmt[i] = t->vf, ncolor = (uint32_t)i + 1;
        if (!w || (int)tw < w) w = (int)tw;
        if (!h || (int)th < h) h = (int)th;
    }
    int has_depth = 0;
    if (d->ds) {
        res_t *z = OBJ(d->ds, res_t);
        vtex_t *t = texture_of(z);
        if (t && t->depth) {
            da.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            da.imageView = att_view(t, 0, 0), da.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            da.clearValue.depthStencil.depth = g_clear_z, da.clearValue.depthStencil.stencil = g_clear_stencil;
            g_pass_dfmt = t->vf, g_pass_stencil = (uint32_t)t->stencil, has_depth = 1;
            if (!w || (int)t->w < w) w = (int)t->w;
            if (!h || (int)t->h < h) h = (int)t->h;
        }
    }
    VkRenderingAttachmentInfo sa = da;
    if (has_depth) {
        da.loadOp = (g_clear_flags & 2) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        sa.loadOp = (g_clear_flags & 4) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    }
    g_clear_flags = 0;
    if (!w || !h) return 0;                           /* nothing to draw into */
    barrier();
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent.width = (uint32_t)w, ri.renderArea.extent.height = (uint32_t)h, ri.layerCount = 1;
    ri.colorAttachmentCount = ncolor, ri.pColorAttachments = ca;
    if (has_depth) ri.pDepthAttachment = &da;
    if (has_depth && g_pass_stencil) ri.pStencilAttachment = &sa;
    vkCmdBeginRendering(cmd(), &ri);
    g_in_pass = 1, st_passes++;
    memcpy(g_pass_rt, d->rt, sizeof g_pass_rt), g_pass_ds = d->ds;
    g_pass_w = w, g_pass_h = h, g_pass_ncolor = ncolor;
    return 1;
}

/* Clear(count, rects, flags, color, z, stencil): of the whole target, the
 * pass's load op; of part (the viewport, the scissor, rectangles),
 * vkCmdClearAttachments. */
static void clear(dev_t_ *d, uint32_t nrects, uint32_t rects, uint32_t flags, uint32_t color, float z, uint32_t stencil) {
    if (g_started <= 0) return;
    TRACE("Clear flags %X color %08X z %g rt %08X ds %08X vp %u,%u %ux%u rects %u\n", flags, color, z, d->rt[0], d->ds,
          d->viewport[0], d->viewport[1], d->viewport[2], d->viewport[3], nrects);
    int tw = 0, th = 0;
    uint32_t first = d->rt[0] ? d->rt[0] : d->ds;
    if (first) {
        res_t *s = OBJ(first, res_t);
        vtex_t *t = texture_of(s);
        if (t) tw = (int)level_dim(t->w, lvl(s)), th = (int)level_dim(t->h, lvl(s));
    }
    int32_t box[4] = { (int32_t)d->viewport[0], (int32_t)d->viewport[1], (int32_t)(d->viewport[0] + d->viewport[2]),
                       (int32_t)(d->viewport[1] + d->viewport[3]) };
    if (d->rs[174]) {
        box[0] = box[0] > (int32_t)d->scissor[0] ? box[0] : (int32_t)d->scissor[0];
        box[1] = box[1] > (int32_t)d->scissor[1] ? box[1] : (int32_t)d->scissor[1];
        box[2] = box[2] < (int32_t)d->scissor[2] ? box[2] : (int32_t)d->scissor[2];
        box[3] = box[3] < (int32_t)d->scissor[3] ? box[3] : (int32_t)d->scissor[3];
    }
    if (box[0] < 0) box[0] = 0;
    if (box[1] < 0) box[1] = 0;
    if (box[2] > tw) box[2] = tw;
    if (box[3] > th) box[3] = th;
    if (!nrects && box[0] == 0 && box[1] == 0 && box[2] >= tw && box[3] >= th) {
        end_pass();
        g_clear_flags = flags & 7, g_clear_color = color, g_clear_z = z, g_clear_stencil = stencil;
        pass(d);                                            /* the pass that clears */
        return;
    }
    if (!pass(d)) return;
    VkClearAttachment ca[6];
    uint32_t nca = 0;
    if (flags & 1)
        for (uint32_t i = 0; i < g_pass_ncolor; i++) {
            if (!g_pass_fmt[i]) continue;
            memset(&ca[nca], 0, sizeof ca[nca]);
            ca[nca].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, ca[nca].colorAttachment = i;
            ca[nca].clearValue.color.float32[0] = ((color >> 16) & 255) / 255.0f;
            ca[nca].clearValue.color.float32[1] = ((color >> 8) & 255) / 255.0f;
            ca[nca].clearValue.color.float32[2] = (color & 255) / 255.0f;
            ca[nca].clearValue.color.float32[3] = (color >> 24) / 255.0f;
            nca++;
        }
    VkImageAspectFlags za = 0;
    if ((flags & 2) && g_pass_dfmt) za |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if ((flags & 4) && g_pass_stencil) za |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (za) {
        memset(&ca[nca], 0, sizeof ca[nca]);
        ca[nca].aspectMask = za, ca[nca].clearValue.depthStencil.depth = z, ca[nca].clearValue.depthStencil.stencil = stencil;
        nca++;
    }
    if (!nca) return;
    uint32_t n = nrects ? nrects : 1;
    for (uint32_t i = 0; i < n; i++) {
        int32_t q[4];
        if (nrects) for (int k = 0; k < 4; k++) q[k] = (int32_t)MEM32(rects + 16 * i + 4 * k);
        else memcpy(q, box, 16);
        if (q[0] < box[0]) q[0] = box[0];
        if (q[1] < box[1]) q[1] = box[1];
        if (q[2] > box[2]) q[2] = box[2];
        if (q[3] > box[3]) q[3] = box[3];
        if (q[2] > g_pass_w) q[2] = g_pass_w;
        if (q[3] > g_pass_h) q[3] = g_pass_h;
        if (q[2] <= q[0] || q[3] <= q[1]) continue;
        VkClearRect cr = { { { q[0], q[1] }, { (uint32_t)(q[2] - q[0]), (uint32_t)(q[3] - q[1]) } }, 0, 1 };
        vkCmdClearAttachments(cmd(), nca, ca, 1, &cr);
    }
}

/* ---- shaders --------------------------------------------------------------------------------- */
static VkShaderModule shader(dev_t_ *d, uint32_t obj, int pixel) {
    if (!obj) return VK_NULL_HANDLE;
    blob_t *b = OBJ(obj, blob_t);
    if (b->translated < 0) return VK_NULL_HANDLE;
    /* ps_1_x and depth textures: the kinds of texture bound make the variant */
    uint32_t key = 0;
    if (pixel) {
        for (int s = 0; s < 8; s++) {
            uint32_t t = d->tex[s];
            if (!t) continue;
            res_t *r = OBJ(t, res_t);
            int kind = r->kind == R_CUBETEXTURE ? 1 : r->kind == R_VOLUMETEXTURE ? 2 : 0;
            if (is_depth_vf(fmt(r->format).vf)) kind = 3;
            key |= (uint32_t)kind << (2 * s);
        }
    }
    for (int i = 0; i < 4; i++) if (b->fn[i] && b->variant_key[i] == key) return (VkShaderModule)(uintptr_t)b->fn[i];
    dxso_options opt;
    memset(&opt, 0, sizeof opt);
    opt.lang = DXSO_GLSL;
    for (int s = 0; s < 8; s++) {
        int k = (key >> (2 * s)) & 3;
        opt.sampler_type[s] = (uint8_t)(k == 1 ? DXSO_TEX_CUBE : k == 2 ? DXSO_TEX_3D : DXSO_TEX_2D);
        if (k == 3) opt.sampler_depth |= (uint16_t)(1u << s);
    }
    dxso_shader s;
    if (dxso_translate((const uint32_t *)hle_ptr(b->code), b->len / 4, &opt, &s) != 0) {
        fprintf(stderr, "[vulkan] a %s shader is not translated: %s\n", pixel ? "pixel" : "vertex", s.error);
        b->translated = -1;
        return VK_NULL_HANDLE;
    }
    uint64_t ct = wait_begin();
    st_shaders++;
    shaderc_compilation_result_t res = SC.compile_into_spv(SC.compiler, s.text, strlen(s.text),
                                                           pixel ? SHADERC_FRAGMENT : SHADERC_VERTEX, "dxso", "main", SC.options);
    VkShaderModule m = VK_NULL_HANDLE;
    if (!res || SC.result_get_compilation_status(res) != 0) {
        fprintf(stderr, "[vulkan] a shader does not compile: %s\n%s\n", res ? SC.result_get_error_message(res) : "shaderc failed", s.text);
    } else {
        VkShaderModuleCreateInfo mi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        mi.codeSize = SC.result_get_length(res), mi.pCode = (const uint32_t *)SC.result_get_bytes(res);
        VKC(vkCreateShaderModule(g_dev, &mi, NULL, &m));
    }
    if (res) SC.result_release(res);
    if (g_stats > 0) st_compile += now_ns() - ct;
    if (!m) { b->translated = -1; dxso_free(&s); return VK_NULL_HANDLE; }
    free(b->info.text);
    b->info = s;
    b->info.text = NULL;
    free(s.text);
    b->translated = 1;
    int slot = 0;
    while (slot < 4 && b->fn[slot]) slot++;
    if (slot == 4) slot = 3;                              /* (the module it replaces is left: pipelines may hold it) */
    b->fn[slot] = (void *)(uintptr_t)m;
    b->variant_key[slot] = key;
    return m;
}

/* ---- vertex layouts ---------------------------------------------------------------------------- */
typedef struct { uint16_t stream, offset; uint8_t type, method, usage, index; } elem_t;

static int fvf_elements(uint32_t fvf, elem_t *e) {
    int n = 0;
    uint16_t off = 0;
    uint32_t pos = fvf & 0x400E;
    if (pos == 0x4) { e[n++] = (elem_t){ 0, off, 3, 0, 9, 0 }; off += 16; }             /* XYZRHW: POSITIONT */
    else if (pos) {
        e[n++] = (elem_t){ 0, off, 2, 0, 0, 0 }; off += 12;                              /* XYZ */
        int blend = pos == 0x6 ? 1 : pos == 0x8 ? 2 : pos == 0xA ? 3 : pos == 0xC ? 4 : pos == 0xE ? 5 : 0;
        if (pos == 0x4002) { e[n++] = (elem_t){ 0, off, 3, 0, 0, 1 }; off += 16; }        /* XYZW */
        off += (uint16_t)(4 * blend);                                                     /* blend weights: unused here */
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
    return n;
}
static int elements(dev_t_ *d, elem_t *e) {
    if (d->decl) {
        blob_t *b = OBJ(d->decl, blob_t);
        const uint8_t *p = (const uint8_t *)hle_ptr(b->code);
        int n = 0;
        for (uint32_t k = 0; k + 8 <= b->len && n < 32; k += 8) {
            uint16_t stream, offset;
            memcpy(&stream, p + k, 2), memcpy(&offset, p + k + 2, 2);
            if (stream == 0xFF) break;
            e[n++] = (elem_t){ stream, offset, p[k + 4], p[k + 5], p[k + 6], p[k + 7] };
        }
        return n;
    }
    return fvf_elements(d->fvf, e);
}
static VkFormat vformat(int type) {                        /* D3DDECLTYPE, read as float4 by the shader */
    switch (type) {
    case 0: return VK_FORMAT_R32_SFLOAT;
    case 1: return VK_FORMAT_R32G32_SFLOAT;
    case 2: return VK_FORMAT_R32G32B32_SFLOAT;
    case 3: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 4: return VK_FORMAT_B8G8R8A8_UNORM;                /* D3DCOLOR */
    case 5: return VK_FORMAT_R8G8B8A8_USCALED;              /* UBYTE4: 0..255 */
    case 6: return VK_FORMAT_R16G16_SSCALED;
    case 7: return VK_FORMAT_R16G16B16A16_SSCALED;
    case 8: return VK_FORMAT_R8G8B8A8_UNORM;
    case 9: return VK_FORMAT_R16G16_SNORM;
    case 10: return VK_FORMAT_R16G16B16A16_SNORM;
    case 11: return VK_FORMAT_R16G16_UNORM;
    case 12: return VK_FORMAT_R16G16B16A16_UNORM;
    case 14: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    case 15: return VK_FORMAT_R16G16_SFLOAT;
    case 16: return VK_FORMAT_R16G16B16A16_SFLOAT;
    }
    return VK_FORMAT_R32G32B32A32_SFLOAT;
}

/* ---- pipelines ----------------------------------------------------------------------------------- */
static const VkBlendFactor BF[16] = {
    VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR,
    VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_DST_COLOR,
    VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE, VK_BLEND_FACTOR_SRC_ALPHA,
    VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR };
static VkBlendFactor bf(uint32_t b) { return b < 16 ? BF[b] : VK_BLEND_FACTOR_ONE; }
static VkBlendOp bop(uint32_t o) {
    switch (o) { case 2: return VK_BLEND_OP_SUBTRACT; case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
                 case 4: return VK_BLEND_OP_MIN; case 5: return VK_BLEND_OP_MAX; }
    return VK_BLEND_OP_ADD;
}
static VkPrimitiveTopology topology(uint32_t prim) {
    switch (prim) {
    case 1: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case 2: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case 5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;            /* lists, and fans made lists */
}

typedef struct {
    uint64_t vs, ps;
    elem_t e[32];
    uint32_t ne, strides[16];
    uint32_t vs_usage[16];
    uint32_t color[4], ncolor, depth, stencil;
    uint32_t blend, src, dst, op, sep, srca, dsta, opa, mask[4];
    uint32_t topo, lines;
} pkey_t;
static map_t g_pipes = { 0 };

static VkPipeline pipeline(dev_t_ *d, VkShaderModule vs, VkShaderModule ps, blob_t *vsb, const uint32_t *strides, uint32_t prim) {
    pkey_t k;
    memset(&k, 0, sizeof k);
    k.vs = (uint64_t)(uintptr_t)vs, k.ps = (uint64_t)(uintptr_t)ps;
    k.ne = (uint32_t)elements(d, k.e);
    memcpy(k.strides, strides, sizeof k.strides);
    for (int i = 0; i < 16; i++) k.vs_usage[i] = (uint32_t)vsb->info.input_usage[i] << 8 | vsb->info.input_index[i];
    for (int i = 0; i < 4; i++) k.color[i] = (uint32_t)g_pass_fmt[i];
    k.ncolor = g_pass_ncolor, k.depth = (uint32_t)g_pass_dfmt, k.stencil = g_pass_stencil;
    k.blend = d->rs[27] != 0;
    if (k.blend) {
        k.src = d->rs[19], k.dst = d->rs[20], k.op = d->rs[171];
        k.sep = d->rs[206] != 0;
        if (k.sep) k.srca = d->rs[207], k.dsta = d->rs[208], k.opa = d->rs[209];
    }
    k.mask[0] = d->rs[168] & 15, k.mask[1] = d->rs[190] & 15, k.mask[2] = d->rs[191] & 15, k.mask[3] = d->rs[192] & 15;
    k.topo = (uint32_t)topology(prim), k.lines = g_fill_lines && d->rs[8] == 2;
    if (!g_pipes.ksize) g_pipes.ksize = sizeof k;
    uint64_t *v = map_slot(&g_pipes, &k, 0);
    if (v) return (VkPipeline)(uintptr_t)*v;

    VkVertexInputAttributeDescription at[DXSO_MAX_INPUTS];
    VkVertexInputBindingDescription bd[17];
    uint32_t nat = 0, nbd = 0, bound = 0;
    int need_zero = 0;
    for (int r = 0; r < DXSO_MAX_INPUTS; r++) {
        if (vsb->info.input_usage[r] == 0xFF) continue;
        int found = -1;
        for (uint32_t i = 0; i < k.ne; i++) {
            int u = k.e[i].usage;
            if (u == 9) u = 0;                                  /* POSITIONT feeds a dcl_position */
            if (u == vsb->info.input_usage[r] && k.e[i].index == vsb->info.input_index[r]) { found = (int)i; break; }
        }
        at[nat].location = (uint32_t)r;
        if (found < 0) {                                        /* not in the declaration: zeros */
            at[nat].binding = B_ZEROS, at[nat].format = VK_FORMAT_R32G32B32A32_SFLOAT, at[nat].offset = 0;
            need_zero = 1;
        } else {
            uint32_t s = k.e[found].stream & 15;
            at[nat].binding = s, at[nat].format = vformat(k.e[found].type), at[nat].offset = k.e[found].offset;
            if (!(bound & (1u << s))) {
                bound |= 1u << s;
                bd[nbd].binding = s, bd[nbd].stride = strides[s], bd[nbd].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
                nbd++;
            }
        }
        nat++;
    }
    if (need_zero) bd[nbd].binding = B_ZEROS, bd[nbd].stride = 0, bd[nbd].inputRate = VK_VERTEX_INPUT_RATE_VERTEX, nbd++;
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount = nbd, vi.pVertexBindingDescriptions = bd;
    vi.vertexAttributeDescriptionCount = nat, vi.pVertexAttributeDescriptions = at;
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = (VkPrimitiveTopology)k.topo;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1, vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = k.lines ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL, rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState cb[4];
    memset(cb, 0, sizeof cb);
    for (uint32_t i = 0; i < k.ncolor; i++) {
        VkPipelineColorBlendAttachmentState *c = &cb[i];
        c->colorWriteMask = (k.mask[i] & 1 ? VK_COLOR_COMPONENT_R_BIT : 0) | (k.mask[i] & 2 ? VK_COLOR_COMPONENT_G_BIT : 0) |
                            (k.mask[i] & 4 ? VK_COLOR_COMPONENT_B_BIT : 0) | (k.mask[i] & 8 ? VK_COLOR_COMPONENT_A_BIT : 0);
        if (!k.blend) continue;
        c->blendEnable = VK_TRUE;
        VkBlendFactor s = bf(k.src), dd = bf(k.dst);
        if (k.src == 12) s = VK_BLEND_FACTOR_SRC_ALPHA, dd = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;     /* BOTHSRCALPHA */
        if (k.src == 13) s = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, dd = VK_BLEND_FACTOR_SRC_ALPHA;     /* BOTHINVSRCALPHA */
        c->srcColorBlendFactor = s, c->dstColorBlendFactor = dd, c->colorBlendOp = bop(k.op);
        if (k.sep) c->srcAlphaBlendFactor = bf(k.srca), c->dstAlphaBlendFactor = bf(k.dsta), c->alphaBlendOp = bop(k.opa);
        else c->srcAlphaBlendFactor = s, c->dstAlphaBlendFactor = dd, c->alphaBlendOp = bop(k.op);
    }
    VkPipelineColorBlendStateCreateInfo bs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    bs.attachmentCount = k.ncolor, bs.pAttachments = cb;
    static const VkDynamicState dyn[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = sizeof dyn / sizeof *dyn, dy.pDynamicStates = dyn;
    VkPipelineShaderStageCreateInfo st[2];
    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT, st[0].module = vs, st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT, st[1].module = ps, st[1].pName = "main";
    VkFormat cf[4];
    for (int i = 0; i < 4; i++) cf[i] = (VkFormat)k.color[i];
    VkPipelineRenderingCreateInfo rci = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    rci.colorAttachmentCount = k.ncolor, rci.pColorAttachmentFormats = cf;
    rci.depthAttachmentFormat = (VkFormat)k.depth;
    rci.stencilAttachmentFormat = k.stencil ? (VkFormat)k.depth : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo gi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gi.pNext = &rci, gi.stageCount = 2, gi.pStages = st;
    gi.pVertexInputState = &vi, gi.pInputAssemblyState = &ia, gi.pViewportState = &vp, gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms, gi.pDepthStencilState = &ds, gi.pColorBlendState = &bs, gi.pDynamicState = &dy;
    gi.layout = g_pl;
    VkPipeline p = VK_NULL_HANDLE;
    uint64_t ct = wait_begin();
    VkResult r = vkCreateGraphicsPipelines(g_dev, g_pcache, 1, &gi, NULL, &p);
    if (g_stats > 0) st_compile += now_ns() - ct;
    st_pipes++;
    if (r != VK_SUCCESS) {
        static int told;
        if (told++ < 10) fprintf(stderr, "[vulkan] pipeline: vkCreateGraphicsPipelines failed (%d)\n", (int)r);
        return VK_NULL_HANDLE;
    }
    *map_slot(&g_pipes, &k, 1) = (uint64_t)(uintptr_t)p;
    return p;
}

static VkCompareOp cmpf(uint32_t c) {
    switch (c) { case 1: return VK_COMPARE_OP_NEVER; case 2: return VK_COMPARE_OP_LESS;
                 case 3: return VK_COMPARE_OP_EQUAL; case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
                 case 5: return VK_COMPARE_OP_GREATER; case 6: return VK_COMPARE_OP_NOT_EQUAL;
                 case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL; }
    return VK_COMPARE_OP_ALWAYS;
}
static VkStencilOp sop(uint32_t o) {
    switch (o) { case 2: return VK_STENCIL_OP_ZERO; case 3: return VK_STENCIL_OP_REPLACE;
                 case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP; case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
                 case 6: return VK_STENCIL_OP_INVERT; case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
                 case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP; }
    return VK_STENCIL_OP_KEEP;
}
/* Depth and stencil, as dynamic state (d3d9_metal.m's depth_state). */
static void depth_state(dev_t_ *d, VkCommandBuffer c) {
    int on = d->ds && d->rs[7];
    vkCmdSetDepthTestEnable(c, on ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthWriteEnable(c, on && d->rs[14] ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthCompareOp(c, on ? cmpf(d->rs[23]) : VK_COMPARE_OP_ALWAYS);
    int st = d->ds && d->rs[52];
    vkCmdSetStencilTestEnable(c, st ? VK_TRUE : VK_FALSE);
    if (st) {
        vkCmdSetStencilOp(c, VK_STENCIL_FACE_FRONT_BIT, sop(d->rs[53]), sop(d->rs[55]), sop(d->rs[54]), cmpf(d->rs[56]));
        if (d->rs[185])
            vkCmdSetStencilOp(c, VK_STENCIL_FACE_BACK_BIT, sop(d->rs[186]), sop(d->rs[188]), sop(d->rs[187]), cmpf(d->rs[189]));
        else
            vkCmdSetStencilOp(c, VK_STENCIL_FACE_BACK_BIT, sop(d->rs[53]), sop(d->rs[55]), sop(d->rs[54]), cmpf(d->rs[56]));
    } else {
        vkCmdSetStencilOp(c, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                          VK_COMPARE_OP_ALWAYS);
    }
    vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_FRONT_AND_BACK, d->rs[58] & 255);
    vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_FRONT_AND_BACK, d->rs[59] & 255);
    vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_AND_BACK, d->rs[57] & 255);
}

static VkSamplerAddressMode addr(uint32_t a) {
    switch (a) {
    case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 3: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 5: return g_mirror_once ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}
static map_t g_samplers = { 0 };
static VkSampler sampler_state(dev_t_ *d, int s, int compare) {
    uint32_t k[12];
    memcpy(k, &d->samp[s][1], 10 * 4);
    k[10] = (uint32_t)compare, k[11] = 0;
    if (!g_samplers.ksize) g_samplers.ksize = sizeof k;
    uint64_t *v = map_slot(&g_samplers, k, 0);
    if (v) return (VkSampler)(uintptr_t)*v;
    VkSamplerCreateInfo sd = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sd.addressModeU = addr(k[0]), sd.addressModeV = addr(k[1]), sd.addressModeW = addr(k[2]);
    uint32_t border = k[3];
    sd.borderColor = (border >> 24) < 128 ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
                   : (border & 0xFFFFFF) > 0x7F7F7F ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    sd.magFilter = k[4] >= 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sd.minFilter = k[5] >= 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sd.mipmapMode = k[6] == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sd.minLod = (float)k[8];                                 /* MAXMIPLEVEL: the largest level used */
    sd.maxLod = k[6] == 0 ? sd.minLod + 0.25f : VK_LOD_CLAMP_NONE;   /* no mip filter: that one level */
    if (g_aniso && (k[4] == 3 || k[5] == 3) && k[9] > 1) {
        float m = (float)(k[9] > 16 ? 16 : k[9]);
        sd.anisotropyEnable = VK_TRUE, sd.maxAnisotropy = m < g_limits.maxSamplerAnisotropy ? m : g_limits.maxSamplerAnisotropy;
    }
    if (compare) sd.compareEnable = VK_TRUE, sd.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkSampler smp = VK_NULL_HANDLE;
    VKC(vkCreateSampler(g_dev, &sd, NULL, &smp));
    *map_slot(&g_samplers, k, 1) = (uint64_t)(uintptr_t)smp;
    return smp;
}

/* A descriptor set of this frame's. */
static VkDescriptorSet dset(void) {
    frame_t *f = &F[g_fi];
    for (;;) {
        if (f->pool_i >= f->npool) {
            if (f->npool == MAX_POOLS) return VK_NULL_HANDLE;
            VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8 * 2048 },
                                           { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 * 2048 } };
            VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pi.maxSets = 2048, pi.poolSizeCount = 2, pi.pPoolSizes = ps;
            if (vkCreateDescriptorPool(g_dev, &pi, NULL, &f->pool[f->npool]) != VK_SUCCESS) return VK_NULL_HANDLE;
            f->npool++;
        }
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = f->pool[f->pool_i], ai.descriptorSetCount = 1, ai.pSetLayouts = &g_dsl;
        VkDescriptorSet s = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(g_dev, &ai, &s) == VK_SUCCESS) return s;
        f->pool_i++;                                          /* full: the next pool */
    }
}

/* ---- draws ---------------------------------------------------------------------------------------- */
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
static float rsf(dev_t_ *d, int i) { float f; memcpy(&f, &d->rs[i], 4); return f; }

static void draw(dev_t_ *d, uint32_t prim, uint32_t count, uint32_t start, int indexed, int32_t base_vertex,
                 uint32_t min_index, uint32_t num_vertices, uint32_t up_va, uint32_t up_stride,
                 uint32_t up_idx_va, uint32_t up_idx_format) {
    if (g_started <= 0 || !count) return;
    g_draws++, st_draws++;
    TRACE("Draw prim %u count %u %s rt %08X vs %08X ps %08X decl %08X fvf %X tex0 %08X up %u z %u/%u blend %u %u/%u cull %u vp %ux%u\n",
          prim, count, indexed ? "indexed" : "", d->rt[0], d->vs, d->ps, d->decl, d->fvf, d->tex[0], up_va != 0,
          d->rs[7], d->rs[14], d->rs[27], d->rs[19], d->rs[20], d->rs[22], d->viewport[2], d->viewport[3]);
    if (!d->vs || !d->ps) { g_skipped_ff++; return; }     /* fixed function: not yet */
    VkShaderModule vs = shader(d, d->vs, 0), ps = shader(d, d->ps, 1);
    if (!vs || !ps) { g_skipped_shader++; return; }
    blob_t *vsb = OBJ(d->vs, blob_t), *psb = OBJ(d->ps, blob_t);
    /* textures first: an upload ends the pass */
    VkDescriptorImageInfo ii[16];
    uint16_t used = psb->info.samplers_used;
    for (int s = 0; s < 16; s++) {
        if (!((used >> s) & 1)) continue;
        int want = psb->info.sampler_type[s];
        vtex_t *t = NULL;
        int depth = 0;
        if (d->tex[s]) {
            res_t *r = OBJ(d->tex[s], res_t);
            t = texture_of(r);
            if (t) sync_texture(r), depth = t->depth;
            VkImageViewType vt = want == DXSO_TEX_CUBE ? VK_IMAGE_VIEW_TYPE_CUBE : want == DXSO_TEX_3D ? VK_IMAGE_VIEW_TYPE_3D
                                                                                                        : VK_IMAGE_VIEW_TYPE_2D;
            if (t && t->sample_type != vt) t = NULL, depth = 0;   /* not the kind the shader samples */
        }
        if (!t) t = g_blank[want == DXSO_TEX_CUBE ? 1 : want == DXSO_TEX_3D ? 2 : 0];
        ii[s].sampler = sampler_state(d, s, depth);
        ii[s].imageView = t->sample, ii[s].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    if (!pass(d)) return;
    VkCommandBuffer c = cmd();
    uint32_t strides[16] = { 0 };
    for (int i = 0; i < 16; i++) strides[i] = d->stream[i][2];
    if (up_va) strides[0] = up_stride;
    VkPipeline pl = pipeline(d, vs, ps, vsb, strides, prim == 6 ? 4 : prim);
    if (!pl) { g_skipped_shader++; return; }
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
    depth_state(d, c);
    uint32_t bfc = d->rs[193];
    float blend[4] = { ((bfc >> 16) & 255) / 255.0f, ((bfc >> 8) & 255) / 255.0f, (bfc & 255) / 255.0f, (bfc >> 24) / 255.0f };
    vkCmdSetBlendConstants(c, blend);
    vkCmdSetFrontFace(c, VK_FRONT_FACE_CLOCKWISE);
    uint32_t cull = d->rs[22];
    vkCmdSetCullMode(c, cull == 2 ? VK_CULL_MODE_FRONT_BIT : cull == 3 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE);
    float bias = rsf(d, 195), slope = rsf(d, 175);
    vkCmdSetDepthBiasEnable(c, bias != 0 || slope != 0 ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthBias(c, bias * 16777216.0f, 0, slope);
    /* viewport (y flipped: Direct3D's clip space), scissor: within the pass's target */
    float vx = (float)d->viewport[0], vy = (float)d->viewport[1], vw = (float)d->viewport[2], vh = (float)d->viewport[3];
    float zn, zf;
    memcpy(&zn, &d->viewport[4], 4), memcpy(&zf, &d->viewport[5], 4);
    if (vw <= 0 || vh <= 0) vw = (float)g_pass_w, vh = (float)g_pass_h;
    VkViewport vp = { vx, vy + vh, vw, -vh, zn, zf };
    vkCmdSetViewport(c, 0, 1, &vp);
    VkRect2D sc = { { 0, 0 }, { (uint32_t)g_pass_w, (uint32_t)g_pass_h } };
    if (d->rs[174]) {
        int x0 = (int)d->scissor[0], y0 = (int)d->scissor[1], x1 = (int)d->scissor[2], y1 = (int)d->scissor[3];
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > g_pass_w) x1 = g_pass_w;
        if (y1 > g_pass_h) y1 = g_pass_h;
        if (x1 <= x0 || y1 <= y0) return;
        sc.offset.x = x0, sc.offset.y = y0, sc.extent.width = (uint32_t)(x1 - x0), sc.extent.height = (uint32_t)(y1 - y0);
    }
    vkCmdSetScissor(c, 0, 1, &sc);
    /* constants and parameters, into the ring; textures; one set */
    dxso_params P = { { -1.0f / vw, 1.0f / vh }, (d->rs[24] & 255) / 255.0f, d->rs[15] ? d->rs[25] : 0,
                      { ((d->rs[34] >> 16) & 255) / 255.0f, ((d->rs[34] >> 8) & 255) / 255.0f, (d->rs[34] & 255) / 255.0f, 1.0f },
                      d->rs[28] != 0, { 0 } };
    ralloc_t ub[7] = { ring_copy(d->vsc, sizeof d->vsc), ring_copy(d->vsi, sizeof d->vsi), ring_copy(d->vsb, sizeof d->vsb),
                       ring_copy(&P, sizeof P), ring_copy(d->psc, sizeof d->psc), ring_copy(d->psi, sizeof d->psi),
                       ring_copy(d->psb, sizeof d->psb) };
    static const uint32_t sizes[8] = { sizeof d->vsc, sizeof d->vsi, sizeof d->vsb, sizeof P, sizeof d->psc,
                                       sizeof d->psi, sizeof d->psb, sizeof P };
    VkDescriptorSet set = dset();
    if (!set) return;
    VkDescriptorBufferInfo bi[8];
    VkWriteDescriptorSet w[24];
    uint32_t nw = 0;
    for (uint32_t i = 0; i < 8; i++) {
        ralloc_t a = ub[i == 7 ? 3 : i];
        if (!a.b) return;
        bi[i].buffer = a.b, bi[i].offset = a.off, bi[i].range = sizes[i];
        memset(&w[nw], 0, sizeof w[nw]);
        w[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, w[nw].dstSet = set, w[nw].dstBinding = i;
        w[nw].descriptorCount = 1, w[nw].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, w[nw].pBufferInfo = &bi[i];
        nw++;
    }
    for (uint32_t s = 0; s < 16; s++) {
        if (!((used >> s) & 1)) continue;
        memset(&w[nw], 0, sizeof w[nw]);
        w[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, w[nw].dstSet = set, w[nw].dstBinding = 8 + s;
        w[nw].descriptorCount = 1, w[nw].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, w[nw].pImageInfo = &ii[s];
        nw++;
    }
    vkUpdateDescriptorSets(g_dev, nw, w, 0, NULL);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pl, 0, 1, &set, 0, NULL);
    /* vertices */
    VkBuffer zb[B_ZEROS + 1];                           /* every binding something: zeros, then the streams */
    VkDeviceSize zoff[B_ZEROS + 1];
    for (int i = 0; i <= B_ZEROS; i++) zb[i] = g_zeros.b, zoff[i] = 0;
    vkCmdBindVertexBuffers(c, 0, B_ZEROS + 1, zb, zoff);
    uint32_t nverts = indexed ? min_index + num_vertices : vertices_of(prim, count);
    if (up_va) {
        ralloc_t a = ring_copy(hle_ptr(up_va), nverts * up_stride);
        if (!a.b) return;
        VkDeviceSize off = a.off;
        vkCmdBindVertexBuffers(c, 0, 1, &a.b, &off);
    } else {
        for (uint32_t i = 0; i < 16; i++) {
            if (!d->stream[i][0]) continue;
            uint32_t off;
            VkBuffer b = buffer_of(OBJ(d->stream[i][0], res_t), &off);
            if (!b) continue;
            VkDeviceSize o = (VkDeviceSize)off + d->stream[i][1];
            vkCmdBindVertexBuffers(c, i, 1, &b, &o);
        }
    }
    uint32_t n = vertices_of(prim, count);
    if (!indexed && prim != 6) {
        vkCmdDraw(c, n, 1, start, 0);
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
        uint32_t tris = count, *list = (uint32_t *)malloc((size_t)tris * 12);
        for (uint32_t i = 0; i < tris; i++) {
            uint32_t a0 = 0, a1 = i + 1, a2 = i + 2;
            if (ip) {
                if (isize == 4) memcpy(&a0, ip, 4), memcpy(&a1, ip + 4 * (i + 1), 4), memcpy(&a2, ip + 4 * (i + 2), 4);
                else {
                    uint16_t x0, x1, x2;
                    memcpy(&x0, ip, 2), memcpy(&x1, ip + 2 * (i + 1), 2), memcpy(&x2, ip + 2 * (i + 2), 2);
                    a0 = x0, a1 = x1, a2 = x2;
                }
            } else a0 += start, a1 += start, a2 += start;
            list[3 * i] = a0, list[3 * i + 1] = a1, list[3 * i + 2] = a2;
        }
        ralloc_t a = ring_copy(list, tris * 12);
        free(list);
        if (!a.b) return;
        vkCmdBindIndexBuffer(c, a.b, a.off, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(c, tris * 3, 1, 0, indexed ? base_vertex : 0, 0);
        return;
    }
    ralloc_t a = ring_copy(ip, n * isize);
    if (!a.b) return;
    vkCmdBindIndexBuffer(c, a.b, a.off, isize == 4 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(c, n, 1, 0, base_vertex, 0);
}

/* ---- StretchRect, readback, shots, present ------------------------------------------------------- */
static void stretch_rect(dev_t_ *d, uint32_t src, uint32_t src_rect, uint32_t dst, uint32_t dst_rect, uint32_t filter) {
    (void)d;
    if (g_started <= 0 || !src || !dst) return;
    TRACE("StretchRect %08X -> %08X\n", src, dst);
    res_t *s = OBJ(src, res_t), *t = OBJ(dst, res_t);
    vtex_t *st = texture_of(s), *dt = texture_of(t);
    if (!st || !dt || st->depth || dt->depth) return;
    sync_texture(s);
    uint32_t sl = lvl(s), dl = lvl(t);
    int32_t sw = (int32_t)level_dim(st->w, sl), sh = (int32_t)level_dim(st->h, sl);
    int32_t dw = (int32_t)level_dim(dt->w, dl), dh = (int32_t)level_dim(dt->h, dl);
    int32_t sr[4] = { 0, 0, sw, sh }, dr[4] = { 0, 0, dw, dh };
    if (src_rect) for (int i = 0; i < 4; i++) sr[i] = (int32_t)MEM32(src_rect + 4 * i);
    if (dst_rect) for (int i = 0; i < 4; i++) dr[i] = (int32_t)MEM32(dst_rect + 4 * i);
    if (sr[2] <= sr[0] || sr[3] <= sr[1] || dr[2] <= dr[0] || dr[3] <= dr[1]) return;
    end_pass();
    barrier();
    VkImageBlit b;
    memset(&b, 0, sizeof b);
    b.srcSubresource.aspectMask = b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.srcSubresource.mipLevel = sl, b.srcSubresource.baseArrayLayer = face(s), b.srcSubresource.layerCount = 1;
    b.dstSubresource.mipLevel = dl, b.dstSubresource.baseArrayLayer = face(t), b.dstSubresource.layerCount = 1;
    b.srcOffsets[0] = (VkOffset3D){ sr[0], sr[1], 0 }, b.srcOffsets[1] = (VkOffset3D){ sr[2], sr[3], 1 };
    b.dstOffsets[0] = (VkOffset3D){ dr[0], dr[1], 0 }, b.dstOffsets[1] = (VkOffset3D){ dr[2], dr[3], 1 };
    int linear = filter >= 2 && (features(st->vf) & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    vkCmdBlitImage(cmd(), st->img, VK_IMAGE_LAYOUT_GENERAL, dt->img, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                   linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    g_need_barrier = 1;
}

/* One level of one face, to the CPU: copied, and waited for. */
static uint8_t *download(vtex_t *t, uint32_t level, uint32_t layer, uint32_t row_texels, uint32_t bytes) {
    end_pass();
    barrier();
    ralloc_t a = ring_alloc(bytes);
    if (!a.b) return NULL;
    VkBufferImageCopy rg;
    memset(&rg, 0, sizeof rg);
    rg.bufferOffset = a.off, rg.bufferRowLength = row_texels;
    rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rg.imageSubresource.mipLevel = level, rg.imageSubresource.baseArrayLayer = layer, rg.imageSubresource.layerCount = 1;
    rg.imageExtent.width = level_dim(t->w, level), rg.imageExtent.height = level_dim(t->h, level), rg.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cmd(), t->img, VK_IMAGE_LAYOUT_GENERAL, a.b, 1, &rg);
    submit(1);
    return a.p;
}

void gpu_readback(dev_t_ *d, res_t *r, uint32_t level, uint32_t face_) {
    (void)d;
    if (g_started <= 0) return;
    res_t *o = owner(r);
    if (!o->gpu) return;
    vtex_t *t = (vtex_t *)o->gpu;
    if (t->depth || t->convert || t->block) return;              /* not read back (yet) */
    uint32_t pitch, slice, mem = level_mem(o, level, face_, &pitch, &slice);
    int bpp = bits_per_pixel(o->format);
    if (bpp < 8) return;
    uint8_t *p = download(t, level, face_, pitch * 8 / (uint32_t)bpp, pitch * level_dim(t->h, level));
    if (p) memcpy(hle_ptr(mem), p, (size_t)pitch * level_dim(t->h, level));
}

int gpu_shot(dev_t_ *d, const char *path) {
    if (g_started <= 0) return -1;
    res_t *bb = OBJ(d->backbuffer, res_t);
    if (!bb->gpu) return -1;
    vtex_t *t = (vtex_t *)bb->gpu;
    uint32_t w = t->w, h = t->h;
    uint8_t *px = download(t, 0, 0, w, w * h * 4);
    if (!px) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint32_t row = w * 3 + (4 - (w * 3) % 4) % 4, size = 54 + row * h;
    uint8_t hdr[54] = { 'B', 'M' };
    memcpy(hdr + 2, &size, 4), hdr[10] = 54, hdr[14] = 40;
    memcpy(hdr + 18, &w, 4), memcpy(hdr + 22, &h, 4), hdr[26] = 1, hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    uint8_t *line = (uint8_t *)calloc(1, row);
    for (int y = (int)h - 1; y >= 0; y--) {
        for (uint32_t x = 0; x < w; x++) memcpy(line + 3 * x, px + ((size_t)y * w + x) * 4, 3);   /* BGRA: BGR */
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    fprintf(stderr, "[vulkan] %llu draws, %llu fixed-function skipped, %llu without a shader\n",
            (unsigned long long)g_draws, (unsigned long long)g_skipped_ff, (unsigned long long)g_skipped_shader);
    return 0;
}

/* With a window: the back buffer to win32hle's window surface, which its
 * presenter scales into the window, as d3d9_metal.m does: each Present ends
 * its frame with a copy of the back buffer into its frame's buffer, and the
 * window shows the newest copy the GPU has finished, a frame or two behind. */
static uint64_t g_show_seq, g_shown;
static uint32_t g_show_w, g_show_h;
static uint8_t *g_win_px;

static void window_copy(dev_t_ *d) {
    res_t *bb = OBJ(d->backbuffer, res_t);
    if (!bb->gpu) return;
    vtex_t *t = (vtex_t *)bb->gpu;
    if (t->w != g_show_w || t->h != g_show_h) {               /* a new size: the old copies are of no use */
        submit(1);
        vkDeviceWaitIdle(g_dev);
        for (int i = 0; i < FRAMES; i++) buf_free(&F[i].show), F[i].show_seq = 0;
        free(g_win_px);
        g_win_px = (uint8_t *)calloc((size_t)t->w * t->h, 4);
        g_show_w = t->w, g_show_h = t->h, g_shown = 0;
        hle_gdi_window_surface(g_win_px, (int)t->w, (int)t->h, (int)t->w * 4, 32);
    }
    frame_t *f = &F[g_fi];
    if (!f->show.b) f->show = buf_new((VkDeviceSize)t->w * t->h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1);
    if (!f->show.b) return;
    end_pass();
    barrier();
    VkBufferImageCopy rg;
    memset(&rg, 0, sizeof rg);
    rg.bufferRowLength = t->w;
    rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, rg.imageSubresource.layerCount = 1;
    rg.imageExtent.width = t->w, rg.imageExtent.height = t->h, rg.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cmd(), t->img, VK_IMAGE_LAYOUT_GENERAL, f->show.b, 1, &rg);
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    f->show_seq = ++g_show_seq;
}
/* The newest finished copy to the window, if it is newer than what it shows. */
static void window_show(void) {
    int best = -1;
    uint64_t newest = g_shown;
    for (int i = 0; i < FRAMES; i++) {
        frame_t *f = &F[i];
        if (!f->show.b || f->show_seq <= newest) continue;
        if (f->submitted && vkGetFenceStatus(g_dev, f->fence) != VK_SUCCESS) continue;
        newest = f->show_seq, best = i;
    }
    if (best < 0) return;
    memcpy(g_win_px, F[best].show.map, (size_t)g_show_w * g_show_h * 4);
    g_shown = newest;
    hle_gdi_window_changed();                           /* shown by the flip at the end of Present */
}

static void present(dev_t_ *d) {
    if (g_started <= 0) return;
    end_pass();
    if (!stl_headless) window_copy(d);
    cmd();                                              /* (a frame with nothing in it is submitted too) */
    submit(0);
    g_fi = (g_fi + 1) % FRAMES;
    frame_t *f = &F[g_fi];
    if (f->submitted) {                                 /* the frame before last: at most two in flight */
        uint64_t wt = wait_begin();
        VKC(vkWaitForFences(g_dev, 1, &f->fence, VK_TRUE, UINT64_MAX));
        wait_end(wt);
    }
    if (!stl_headless) window_show();
    frame_reuse(f);
    if (!stl_headless) {                                /* paced as a flip (HLE_MAX_FPS) */
        uint64_t pt = wait_begin();
        hle_screen_flip();
        if (g_stats > 0) { uint64_t p = now_ns() - pt; st_paced += p; st_renderer -= p; }
    }
    g_frame++;
    g_gen++;
}

/* The calls d3d9.c makes, timed for STL_VULKAN_STATS. */
void gpu_draw(dev_t_ *d, uint32_t prim, uint32_t count, uint32_t start, int indexed, int32_t base_vertex,
              uint32_t min_index, uint32_t num_vertices, uint32_t up_va, uint32_t up_stride,
              uint32_t up_idx_va, uint32_t up_idx_format) {
    call_begin();
    draw(d, prim, count, start, indexed, base_vertex, min_index, num_vertices, up_va, up_stride, up_idx_va, up_idx_format);
    call_end();
}
void gpu_clear(dev_t_ *d, uint32_t nrects, uint32_t rects, uint32_t flags, uint32_t color, float z, uint32_t stencil) {
    call_begin();
    clear(d, nrects, rects, flags, color, z, stencil);
    call_end();
}
void gpu_stretch_rect(dev_t_ *d, uint32_t src, uint32_t src_rect, uint32_t dst, uint32_t dst_rect, uint32_t filter) {
    call_begin();
    stretch_rect(d, src, src_rect, dst, dst_rect, filter);
    call_end();
}
void gpu_present(dev_t_ *d) {
    call_begin();
    present(d);
    call_end();
    stats_frame();
}

#endif /* STL_HAVE_VULKAN */
#endif /* not a Mac */
