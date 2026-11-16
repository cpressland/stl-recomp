/*
 * dxso_glsl - translate D3D9 shader bytecode files to GLSL (src/host/dxso.c,
 * DXSO_GLSL) and compile each to SPIR-V for Vulkan 1.3 with glslangValidator,
 * as d3d9_vulkan.c's shaderc will. Any computer with a C compiler and
 * glslang (Homebrew's, or the freedesktop SDK's), and spirv-val (SPIRV-Tools)
 * to validate the SPIR-V when it is on the PATH.
 *
 *   tools/dxso/build.sh && work/dxso_glsl work/shaders        # STL_DUMP_SHADERS' folder
 *   work/dxso_glsl -v work/shaders/ps_0123456789abcdef.bin    # print the GLSL too
 *   work/dxso_glsl -o DIR work/shaders                         # keep the GLSL and SPIR-V in DIR
 *   work/dxso_glsl -depth work/shaders     # every texture a depth texture (shadow maps' comparisons)
 *   work/dxso_glsl -cube work/shaders      # ps_1_x's textures cube maps (-3d: volumes)
 *
 * Exits 0 only if every file translates and compiles.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dxso.h"

static int verbose, depth, tex_type, have_val;
static const char *keep;
static char tmp[1024];

static int check(const char *path, int *untranslated, int *uncompiled) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "%s: cannot read\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *w = malloc((size_t)n + 4);
    size_t got = fread(w, 1, (size_t)n, f);
    fclose(f);
    dxso_shader s;
    dxso_options opt;
    memset(&opt, 0, sizeof opt);
    opt.lang = DXSO_GLSL;
    opt.sampler_depth = depth ? 0xFFFF : 0;
    memset(opt.sampler_type, tex_type, sizeof opt.sampler_type);
    if (dxso_translate(w, got / 4, &opt, &s) != 0) {
        printf("UNTRANSLATED %s: %s\n", path, s.error);
        (*untranslated)++;
        free(w);
        return 1;
    }
    free(w);
    if (verbose) printf("---- %s (%s_%d_%d)\n%s\n", path, s.pixel ? "ps" : "vs", s.major, s.minor, s.text);
    const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    char src[2048], spv[2048], log[2048], cmd[16384];
    snprintf(src, sizeof src, "%s/%.*s.%s", keep ? keep : tmp, (int)strcspn(base, "."), base, s.pixel ? "frag" : "vert");
    snprintf(spv, sizeof spv, "%s.spv", src);
    snprintf(log, sizeof log, "%s/glslang.log", tmp);
    f = fopen(src, "w");
    if (!f) { fprintf(stderr, "%s: cannot write\n", src); dxso_free(&s); return 1; }
    fputs(s.text, f);
    fclose(f);
    int k = snprintf(cmd, sizeof cmd, "glslangValidator -V --target-env vulkan1.3 -o '%s' '%s' > '%s' 2>&1", spv, src, log);
    if (have_val && k > 0 && (size_t)k < sizeof cmd)
        snprintf(cmd + k, sizeof cmd - (size_t)k, " && spirv-val --target-env vulkan1.3 '%s' >> '%s' 2>&1", spv, log);
    int bad = system(cmd) != 0;
    if (bad) {
        printf("UNCOMPILED %s (%s_%d_%d):\n", path, s.pixel ? "ps" : "vs", s.major, s.minor);
        if ((f = fopen(log, "r"))) {
            char l[1024];
            while (fgets(l, sizeof l, f)) fputs(l, stdout);
            fclose(f);
        }
        if (!verbose) printf("%s\n", s.text);
        (*uncompiled)++;
    }
    if (!keep) unlink(src), unlink(spv);
    dxso_free(&s);
    return bad;
}

int main(int argc, char **argv) {
    if (system("glslangValidator --version > /dev/null 2>&1") != 0) {
        fprintf(stderr, "no glslangValidator on the PATH (brew install glslang spirv-tools)\n");
        return 2;
    }
    have_val = system("spirv-val --version > /dev/null 2>&1") == 0;
    if (!have_val) fprintf(stderr, "(no spirv-val on the PATH: the SPIR-V is not validated, only compiled)\n");
    snprintf(tmp, sizeof tmp, "%s/dxso_glsl.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(tmp)) { perror("mkdtemp"); return 2; }
    int files = 0, untranslated = 0, uncompiled = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) { verbose = 1; continue; }
        if (!strcmp(argv[i], "-depth")) { depth = 1; continue; }
        if (!strcmp(argv[i], "-cube")) { tex_type = DXSO_TEX_CUBE; continue; }
        if (!strcmp(argv[i], "-3d")) { tex_type = DXSO_TEX_3D; continue; }
        if (!strcmp(argv[i], "-o") && i + 1 < argc) { keep = argv[++i]; mkdir(keep, 0755); continue; }
        DIR *d = opendir(argv[i]);
        if (!d) { files++; check(argv[i], &untranslated, &uncompiled); continue; }
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strstr(e->d_name, ".bin")) continue;
            char p[4096];
            snprintf(p, sizeof p, "%s/%s", argv[i], e->d_name);
            files++;
            check(p, &untranslated, &uncompiled);
        }
        closedir(d);
    }
    char log[1100];
    snprintf(log, sizeof log, "%s/glslang.log", tmp);
    unlink(log);
    rmdir(tmp);
    printf("%d shaders: %d translated and compiled to SPIR-V, %d not translated, %d not compiled\n", files,
           files - untranslated - uncompiled, untranslated, uncompiled);
    return untranslated || uncompiled;
}
