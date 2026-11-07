/*
 * dxso_check - translate D3D9 shader bytecode files to MSL (src/host/dxso.c)
 * and compile each with this Mac's Metal compiler.
 *
 *   tools/dxso/build.sh && work/dxso_check work/shaders        # STL_DUMP_SHADERS' folder
 *   work/dxso_check -v work/shaders/ps_0123456789abcdef.bin    # print the MSL too
 *
 * Exits 0 only if every file translates and compiles.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include "dxso.h"

static int verbose;

static int check(id<MTLDevice> dev, const char *path, int *untranslated, int *uncompiled) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "%s: cannot read\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *w = malloc((size_t)n + 4);
    fread(w, 1, (size_t)n, f);
    fclose(f);
    dxso_shader s;
    dxso_options opt = { { 0 }, 0 };
    if (dxso_translate(w, (size_t)n / 4, &opt, &s) != 0) {
        printf("UNTRANSLATED %s: %s\n", path, s.error);
        (*untranslated)++;
        free(w);
        return 1;
    }
    if (verbose) printf("---- %s (%s_%d_%d)\n%s\n", path, s.pixel ? "ps" : "vs", s.major, s.minor, s.msl);
    NSError *err = nil;
    MTLCompileOptions *o = [MTLCompileOptions new];
    id<MTLLibrary> lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:s.msl] options:o error:&err];
    int bad = !lib || ![lib newFunctionWithName:@"dxso_main"];
    if (bad) {
        printf("UNCOMPILED %s (%s_%d_%d):\n%s\n", path, s.pixel ? "ps" : "vs", s.major, s.minor,
               err ? err.localizedDescription.UTF8String : "no dxso_main");
        if (!verbose) printf("%s\n", s.msl);
        (*uncompiled)++;
    }
    dxso_free(&s);
    free(w);
    return bad;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { fprintf(stderr, "no Metal device\n"); return 2; }
        int files = 0, untranslated = 0, uncompiled = 0;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "-v")) { verbose = 1; continue; }
            DIR *d = opendir(argv[i]);
            if (!d) { files++; check(dev, argv[i], &untranslated, &uncompiled); continue; }
            struct dirent *e;
            while ((e = readdir(d))) {
                if (!strstr(e->d_name, ".bin")) continue;
                char p[4096];
                snprintf(p, sizeof p, "%s/%s", argv[i], e->d_name);
                files++;
                check(dev, p, &untranslated, &uncompiled);
            }
            closedir(d);
        }
        printf("%d shaders: %d translated and compiled, %d not translated, %d not compiled\n", files,
               files - untranslated - uncompiled, untranslated, uncompiled);
        return untranslated || uncompiled;
    }
}
