/*
 * dxso.c - Direct3D 9 shader bytecode to Metal Shading Language (dxso.h).
 *
 * The bytecode is a version token, instructions, and an end token
 * (0x0000FFFF). An instruction token has its opcode in bits 0-15, controls
 * in 16-23 (a comparison, texld's project and bias), from shader model 2 its
 * length in 24-27 (shader model 1 lengths come from the opcode), bit 28
 * predication and bit 30 co-issue (ps_1_x). Parameters have bit 31 set:
 *
 *   register   number bits 0-10, type bits 28-30 with 11-12 above them,
 *              relative addressing bit 13 (from SM2, an address token
 *              follows; in vs_1_1 it is a0.x)
 *   dest       write mask 16-19, modifiers 20-23 (saturate, partial
 *              precision, centroid), shift 24-27 (ps_1_x: x2 x4 x8 d2 ...)
 *   source     swizzle 16-23 (two bits a component), modifier 24-27 (negate,
 *              bias, bx2, 1-x, x2, dz, dw, abs, not)
 *
 * Each instruction becomes a block computing a float4 and writing the
 * destination's mask of it. Registers are arrays of float4 (r[], c[], v[]);
 * a constant the shader defines (def) is read as its literal where it is
 * addressed directly. The semantics are Direct3D's, as the reference
 * rasteriser has them: rsq of |x|, log of |x| with log(0) the most negative
 * float, lit's exponent clamped to +-128, vs_1_1's mov to a0 rounding down
 * and mova to nearest, ps_1_1's cnd on > 0.5, ps_1_x's r0 the colour.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dxso.h"

/* ---- a growing string ------------------------------------------------- */
typedef struct { char *p; size_t n, cap; } sb;

static void sb_add(sb *s, const char *fmt, ...) {
    va_list ap;
    for (;;) {
        size_t room = s->cap - s->n;
        va_start(ap, fmt);
        int k = vsnprintf(s->p ? s->p + s->n : NULL, s->p ? room : 0, fmt, ap);
        va_end(ap);
        if (k < 0) return;
        if (s->p && (size_t)k < room) { s->n += (size_t)k; return; }
        size_t cap = s->cap ? s->cap : 4096;
        while (cap - s->n <= (size_t)k) cap *= 2;
        s->p = realloc(s->p, cap);
        s->cap = cap;
        if (s->n == 0) s->p[0] = 0;
    }
}

/* ---- opcodes ---------------------------------------------------------- */
enum {
    OP_NOP = 0, OP_MOV, OP_ADD, OP_SUB, OP_MAD, OP_MUL, OP_RCP, OP_RSQ, OP_DP3, OP_DP4, OP_MIN, OP_MAX,
    OP_SLT, OP_SGE, OP_EXP, OP_LOG, OP_LIT, OP_DST, OP_LRP, OP_FRC, OP_M4x4, OP_M4x3, OP_M3x4, OP_M3x3,
    OP_M3x2, OP_CALL, OP_CALLNZ, OP_LOOP, OP_RET, OP_ENDLOOP, OP_LABEL, OP_DCL, OP_POW, OP_CRS, OP_SGN,
    OP_ABS, OP_NRM, OP_SINCOS, OP_REP, OP_ENDREP, OP_IF, OP_IFC, OP_ELSE, OP_ENDIF, OP_BREAK, OP_BREAKC,
    OP_MOVA, OP_DEFB, OP_DEFI,
    OP_TEXCOORD = 64, OP_TEXKILL, OP_TEX, OP_TEXBEM, OP_TEXBEML, OP_TEXREG2AR, OP_TEXREG2GB, OP_TEXM3x2PAD,
    OP_TEXM3x2TEX, OP_TEXM3x3PAD, OP_TEXM3x3TEX, OP_RESERVED0, OP_TEXM3x3SPEC, OP_TEXM3x3VSPEC, OP_EXPP,
    OP_LOGP, OP_CND, OP_DEF, OP_TEXREG2RGB, OP_TEXDP3TEX, OP_TEXM3x2DEPTH, OP_TEXDP3, OP_TEXM3x3,
    OP_TEXDEPTH, OP_CMP, OP_BEM, OP_DP2ADD, OP_DSX, OP_DSY, OP_TEXLDD, OP_SETP, OP_TEXLDL, OP_BREAKP,
    OP_PHASE = 0xFFFD, OP_COMMENT = 0xFFFE, OP_END = 0xFFFF
};

/* Parameters (destination and sources) of each opcode in shader model 1,
 * whose instruction tokens carry no length. -1: not in shader model 1. */
static int sm1_params(int op, int ps, int minor) {
    switch (op) {
    case OP_NOP: case OP_PHASE: return 0;
    case OP_MOV: case OP_RCP: case OP_RSQ: case OP_EXP: case OP_LOG: case OP_LIT: case OP_FRC:
    case OP_EXPP: case OP_LOGP: case OP_ABS: case OP_NRM: case OP_MOVA: case OP_DCL:
    case OP_TEXBEM: case OP_TEXBEML: case OP_TEXREG2AR: case OP_TEXREG2GB: case OP_TEXM3x2PAD:
    case OP_TEXM3x2TEX: case OP_TEXM3x3PAD: case OP_TEXM3x3TEX: case OP_TEXM3x3VSPEC: case OP_TEXREG2RGB:
    case OP_TEXDP3TEX: case OP_TEXM3x2DEPTH: case OP_TEXDP3: case OP_TEXM3x3: case OP_SINCOS:
        return 2;
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DP3: case OP_DP4: case OP_MIN: case OP_MAX: case OP_SLT:
    case OP_SGE: case OP_DST: case OP_M4x4: case OP_M4x3: case OP_M3x4: case OP_M3x3: case OP_M3x2:
    case OP_POW: case OP_CRS: case OP_TEXM3x3SPEC: case OP_BEM:
        return 3;
    case OP_MAD: case OP_LRP: case OP_CND: case OP_CMP: case OP_DP2ADD: case OP_SGN:
        return 4;
    case OP_DEF: return 5;
    case OP_TEXCOORD: case OP_TEX: return ps && minor >= 4 ? 2 : 1;
    case OP_TEXKILL: case OP_TEXDEPTH: return 1;
    }
    return -1;
}

/* ---- register types ----------------------------------------------------- */
enum {
    RT_TEMP = 0, RT_INPUT, RT_CONST, RT_ADDR, /* ps: RT_TEXTURE */ RT_RASTOUT, RT_ATTROUT, RT_TEXCRDOUT,
    RT_CONSTINT, RT_COLOROUT, RT_DEPTHOUT, RT_SAMPLER, RT_CONST2, RT_CONST3, RT_CONST4, RT_CONSTBOOL,
    RT_LOOP, RT_TEMPFLOAT16, RT_MISCTYPE, RT_LABEL, RT_PREDICATE
};
#define RT_TEXTURE RT_ADDR

static int reg_type(uint32_t t) { return (int)(((t >> 28) & 7) | ((t >> 8) & 0x18)); }
static int reg_num(uint32_t t) { return (int)(t & 0x7FF); }

/* ---- the translation ------------------------------------------------------ */
#define MAX_TEMPS 32

typedef struct {
    const uint32_t *w;
    size_t n, at;
    int ps, major, minor;
    const dxso_options *opt;
    dxso_shader *out;
    sb body;
    int depth;                         /* the body's indentation, in blocks */
    /* what the body used */
    int max_temp, max_v, max_t, max_od, max_ot, max_oc;
    int uses_a0, uses_aL, uses_pos_out, uses_fog_out, uses_pts_out, uses_depth_out, uses_vpos, uses_vface;
    int uses_p0;
    /* the shader's own constants */
    float defc[256][4];
    uint8_t has_defc[256];
    int32_t defi[16][4];
    uint8_t has_defi[16];
    uint32_t defb[16];
    uint8_t has_defb[16];
    /* pixel inputs (ps_2_x and 3): v<n>, t<n> as declared */
    uint16_t ps_t_declared;
    int failed;
} tr;

static void fail(tr *t, const char *fmt, ...) {
    if (t->failed) return;
    t->failed = 1;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t->out->error, sizeof t->out->error, fmt, ap);
    va_end(ap);
}

static void line(tr *t, const char *fmt, ...) {
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    sb_add(&t->body, "%*s%s\n", 4 * (t->depth + 1), "", tmp);
}

static void note(int *max, int n) { if (n > *max) *max = n; }

/* A register's name, as a float4 expression (or its relative form). */
static void reg_name(tr *t, uint32_t p, uint32_t rel, int has_rel, int is_dst, char *out, size_t cap) {
    int type = reg_type(p), n = reg_num(p);
    char idx[96];
    if (has_rel) {
        int rt = reg_type(rel), comp = (int)((rel >> 16) & 3);
        char c = "xyzw"[comp];
        if (rt == RT_LOOP) snprintf(idx, sizeof idx, "(aL + %d)", n), t->uses_aL = 1;
        else snprintf(idx, sizeof idx, "(a0.%c + %d)", c, n), t->uses_a0 = 1;
    } else {
        snprintf(idx, sizeof idx, "%d", n);
    }
    switch (type) {
    case RT_TEMP:
        if (n >= MAX_TEMPS) { fail(t, "temp r%d", n); n = 0; }
        note(&t->max_temp, n);
        snprintf(out, cap, "r[%d]", n);
        return;
    case RT_INPUT:
        if (!t->ps) { note(&t->max_v, n); snprintf(out, cap, "v[%s]", idx); }
        else if (t->major < 3) { if (n > 1) fail(t, "pixel input v%d", n); snprintf(out, cap, "vd[%d]", n); }
        else { fail(t, "ps_3_0 inputs"); snprintf(out, cap, "r[0]"); }
        return;
    case RT_CONST:
        if (!has_rel && n < 256 && t->has_defc[n]) {
            snprintf(out, cap, "float4(%.9g, %.9g, %.9g, %.9g)", t->defc[n][0], t->defc[n][1], t->defc[n][2],
                     t->defc[n][3]);
            return;
        }
        if (has_rel) snprintf(out, cap, "c[clamp(%s, 0, %d)]", idx, t->ps ? 223 : 255);
        else snprintf(out, cap, "c[%d]", n);
        return;
    case RT_ADDR:                                      /* vs: a0; ps: t<n> */
        if (!t->ps) { t->uses_a0 = 1; snprintf(out, cap, is_dst ? "a0" : "float4(a0)"); return; }
        note(&t->max_t, n);
        snprintf(out, cap, "t[%d]", n);
        return;
    case RT_RASTOUT:
        if (n == 0) { t->uses_pos_out = 1; snprintf(out, cap, "o_pos"); }
        else if (n == 1) { t->uses_fog_out = 1; snprintf(out, cap, "o_fog"); }
        else { t->uses_pts_out = 1; snprintf(out, cap, "o_pts"); }
        return;
    case RT_ATTROUT:
        note(&t->max_od, n);
        snprintf(out, cap, "o_d[%d]", n);
        return;
    case RT_TEXCRDOUT:
        if (t->major >= 3) { fail(t, "vs_3_0 outputs"); snprintf(out, cap, "o_t[0]"); return; }
        note(&t->max_ot, n);
        snprintf(out, cap, "o_t[%d]", n);
        return;
    case RT_CONSTINT:
        if (n < 16 && t->has_defi[n])
            snprintf(out, cap, "float4(%d, %d, %d, %d)", t->defi[n][0], t->defi[n][1], t->defi[n][2], t->defi[n][3]);
        else
            snprintf(out, cap, "float4(ci[%d])", n);
        return;
    case RT_COLOROUT:
        note(&t->max_oc, n);
        snprintf(out, cap, "o_c[%d]", n);
        return;
    case RT_DEPTHOUT:
        t->uses_depth_out = 1;
        snprintf(out, cap, "o_depth");
        return;
    case RT_CONSTBOOL:
        snprintf(out, cap, "float4(cb[%d] ? 1.0 : 0.0)", n);
        return;
    case RT_LOOP:
        t->uses_aL = 1;
        snprintf(out, cap, "float4(aL)");
        return;
    case RT_MISCTYPE:
        if (n == 0) { t->uses_vpos = 1; snprintf(out, cap, "vpos"); }
        else { t->uses_vface = 1; snprintf(out, cap, "vface"); }
        return;
    case RT_SAMPLER:                                   /* texld's operand: only its number is used */
        snprintf(out, cap, "float4(0.0)");
        return;
    case RT_PREDICATE:
        t->uses_p0 = 1;
        snprintf(out, cap, is_dst ? "p0" : "float4(p0)");
        return;
    }
    fail(t, "register type %d", type);
    snprintf(out, cap, "r[0]");
}

/* A source parameter at w[*at] (advancing past it and any address token). */
typedef struct { uint32_t tok, rel; int has_rel; char s[512]; } src_t;

static int read_src(tr *t, size_t *at, src_t *s, int bump_reg) {
    if (*at >= t->n) { fail(t, "the shader ends inside an instruction"); return -1; }
    s->tok = t->w[(*at)++];
    s->has_rel = (s->tok & 0x2000) != 0;
    s->rel = 0;
    if (s->has_rel) {
        if (t->major >= 2) { if (*at >= t->n) { fail(t, "truncated"); return -1; } s->rel = t->w[(*at)++]; }
        else s->rel = 0xB0000000u;                     /* vs_1_1: a0.x */
    }
    uint32_t tok = s->tok;
    if (bump_reg) tok = (tok & ~0x7FFu) | (uint32_t)((reg_num(tok) + bump_reg) & 0x7FF);
    char base[256];
    reg_name(t, tok, s->rel, s->has_rel, 0, base, sizeof base);
    uint32_t sw = (s->tok >> 16) & 0xFF;
    char swz[8] = "";
    if (sw != 0xE4) {                                  /* .xyzw is the identity */
        swz[0] = '.';
        for (int i = 0; i < 4; i++) swz[1 + i] = "xyzw"[(sw >> (2 * i)) & 3];
        swz[5] = 0;
    }
    char x[300];
    snprintf(x, sizeof x, "(%s%s)", base, swz);
    switch ((s->tok >> 24) & 0xF) {
    case 0: snprintf(s->s, sizeof s->s, "%s", x); break;
    case 1: snprintf(s->s, sizeof s->s, "(-%s)", x); break;
    case 2: snprintf(s->s, sizeof s->s, "(%s - 0.5)", x); break;
    case 3: snprintf(s->s, sizeof s->s, "(-(%s - 0.5))", x); break;
    case 4: snprintf(s->s, sizeof s->s, "(2.0 * (%s - 0.5))", x); break;
    case 5: snprintf(s->s, sizeof s->s, "(-2.0 * (%s - 0.5))", x); break;
    case 6: snprintf(s->s, sizeof s->s, "(1.0 - %s)", x); break;
    case 7: snprintf(s->s, sizeof s->s, "(2.0 * %s)", x); break;
    case 8: snprintf(s->s, sizeof s->s, "(-2.0 * %s)", x); break;
    case 9: snprintf(s->s, sizeof s->s, "(%s / %s.z)", x, x); break;
    case 10: snprintf(s->s, sizeof s->s, "(%s / %s.w)", x, x); break;
    case 11: snprintf(s->s, sizeof s->s, "abs%s", x); break;
    case 12: snprintf(s->s, sizeof s->s, "(-abs%s)", x); break;
    case 13: snprintf(s->s, sizeof s->s, "(1.0 - %s)", x); break;   /* not: on a bool, as 0/1 */
    default: fail(t, "source modifier %u", (s->tok >> 24) & 0xF); snprintf(s->s, sizeof s->s, "%s", x);
    }
    return 0;
}

/* A scalar source: its replicated component (shader model 1 with no
 * swizzle: w, as the reference rasteriser takes it). */
static void scalar(tr *t, const src_t *s, char *out, size_t cap) {
    uint32_t sw = (s->tok >> 16) & 0xFF;
    snprintf(out, cap, "%s.%c", s->s, t->major < 2 && sw == 0xE4 ? 'w' : 'x');
}

typedef struct { uint32_t tok; char name[256]; char mask[6]; } dst_t;

static int read_dst(tr *t, size_t *at, dst_t *d) {
    if (*at >= t->n) { fail(t, "truncated"); return -1; }
    d->tok = t->w[(*at)++];
    uint32_t rel = 0;
    int has_rel = (d->tok & 0x2000) != 0;
    if (has_rel && t->major >= 2) rel = t->w[(*at)++];
    else if (has_rel) rel = 0xB0000000u;
    reg_name(t, d->tok, rel, has_rel, 1, d->name, sizeof d->name);
    uint32_t m = (d->tok >> 16) & 0xF;
    int k = 0;
    for (int i = 0; i < 4; i++) if (m & (1u << i)) d->mask[k++] = "xyzw"[i];
    d->mask[k] = 0;
    if (!k) strcpy(d->mask, "xyzw");
    return 0;
}

/* dst.mask = value.mask, after the destination's modifiers. */
static void write_dst(tr *t, const dst_t *d, const char *value) {
    int rt = reg_type(d->tok);
    char v[1500];
    snprintf(v, sizeof v, "%s", value);
    int shift = (int)((d->tok >> 24) & 0xF);
    if (shift) {
        float k = shift < 8 ? (float)(1 << shift) : 1.0f / (float)(1 << (16 - shift));
        char w[1500];
        snprintf(w, sizeof w, "((%s) * %g)", v, k);
        snprintf(v, sizeof v, "%s", w);
    }
    if ((d->tok >> 20) & 1) {
        char w[1500];
        snprintf(w, sizeof w, "saturate(%s)", v);
        snprintf(v, sizeof v, "%s", w);
    }
    if (rt == RT_ADDR && !t->ps) {
        /* vs_1_1's mov to a0 rounds down; mova (shader model 2) to nearest. */
        line(t, "a0.%s = int4(%s(%s)).%s;", d->mask, t->major < 2 ? "floor" : "rint", v, d->mask);
    } else if (rt == RT_PREDICATE) {
        line(t, "p0.%s = bool4(%s).%s;", d->mask, v, d->mask);
    } else if (rt == RT_RASTOUT && reg_num(d->tok) != 0) {
        line(t, "%s = float4(%s).x;", d->name, v);    /* oFog, oPts: scalars */
    } else if (rt == RT_DEPTHOUT) {
        line(t, "%s = float4(%s).x;", d->name, v);
    } else {
        line(t, "%s.%s = float4(%s).%s;", d->name, d->mask, v, d->mask);
    }
}

/* A texture sample of stage s at coords (a float4 expression), as the
 * stage's texture type has it; control: 1 project, 2 bias, 3 lod. */
static void sample(tr *t, int s, const char *coords, int control, char *out, size_t cap) {
    if (s < 0 || s > 15) { fail(t, "sampler %d", s); snprintf(out, cap, "float4(0)"); return; }
    t->out->samplers_used |= (uint16_t)(1u << s);
    int type = t->out->sampler_type[s];
    if (t->opt && (t->opt->sampler_depth >> s) & 1) {
        /* A depth texture: a comparison against the coordinate's z (w-divided). */
        snprintf(out, cap, "float4(s%d.sample_compare(z%d, (%s).xy / (%s).w, (%s).z / (%s).w))", s, s, coords, coords,
                 coords, coords);
        return;
    }
    const char *sw = type == DXSO_TEX_2D ? "xy" : "xyz";
    char c[700];
    if (control == 1) snprintf(c, sizeof c, "(%s).%s / (%s).w", coords, sw, coords);
    else snprintf(c, sizeof c, "(%s).%s", coords, sw);
    if (control == 2) snprintf(out, cap, "s%d.sample(z%d, %s, bias((%s).w))", s, s, c, coords);
    else if (control == 3) snprintf(out, cap, "s%d.sample(z%d, %s, level((%s).w))", s, s, c, coords);
    else snprintf(out, cap, "s%d.sample(z%d, %s)", s, s, c);
}

static const char *cmp_op(int c) {   /* the comparison in an instruction's controls */
    switch (c) { case 1: return ">"; case 2: return "=="; case 3: return ">="; case 4: return "<"; case 5: return "!="; case 6: return "<="; }
    return NULL;
}

/* The first pass: dcl and def, so the body sees them wherever they are. */
static void declarations(tr *t) {
    for (size_t at = 1; at < t->n && !t->failed;) {
        uint32_t tok = t->w[at];
        int op = (int)(tok & 0xFFFF);
        if (op == OP_END) break;
        if (op == OP_COMMENT) { at += 1 + ((tok >> 16) & 0x7FFF); continue; }
        int len = t->major >= 2 ? (int)((tok >> 24) & 0xF) : sm1_params(op, t->ps, t->minor);
        if (len < 0) { fail(t, "opcode %d in shader model 1", op); return; }
        const uint32_t *p = t->w + at + 1;
        if (at + 1 + (size_t)len > t->n) { fail(t, "the shader ends inside an instruction"); return; }
        if (op == OP_DEF) {
            int n = reg_num(p[0]);
            if (n < 256) { memcpy(t->defc[n], &p[1], 16); t->has_defc[n] = 1; }
        } else if (op == OP_DEFI) {
            int n = reg_num(p[0]);
            if (n < 16) { memcpy(t->defi[n], &p[1], 16); t->has_defi[n] = 1; }
        } else if (op == OP_DEFB) {
            int n = reg_num(p[0]);
            if (n < 16) { t->defb[n] = p[1]; t->has_defb[n] = 1; }
        } else if (op == OP_DCL) {
            uint32_t u = p[0], d = p[1];
            int type = reg_type(d), n = reg_num(d);
            if (type == RT_INPUT && !t->ps && n < DXSO_MAX_INPUTS) {
                t->out->input_usage[n] = (uint8_t)(u & 0x1F);
                t->out->input_index[n] = (uint8_t)((u >> 16) & 0xF);
                note(&t->max_v, n);
            } else if (type == RT_SAMPLER && n < 16) {
                int tt = (int)((u >> 27) & 0xF);              /* 2 2D, 3 cube, 4 volume */
                t->out->sampler_type[n] = tt == 3 ? DXSO_TEX_CUBE : tt == 4 ? DXSO_TEX_3D : DXSO_TEX_2D;
            } else if (type == RT_TEXTURE && t->ps && n < 8) {
                t->ps_t_declared |= (uint16_t)(1u << n);
            }
        }
        at += 1 + (size_t)len;
    }
}

/* One instruction at w[at]; returns the index after it. */
static size_t instruction(tr *t, size_t at) {
    uint32_t tok = t->w[at];
    int op = (int)(tok & 0xFFFF), control = (int)((tok >> 16) & 0xFF);
    int len = t->major >= 2 ? (int)((tok >> 24) & 0xF) : sm1_params(op, t->ps, t->minor);
    size_t next = at + 1 + (size_t)len, p = at + 1;
    if (tok & 0x10000000u) { fail(t, "predicated instruction (opcode %d)", op); return next; }
    dst_t d;
    src_t s0, s1, s2;
    char v[1600], a[600], b[600];
    switch (op) {
    case OP_NOP: case OP_PHASE: case OP_DCL: case OP_DEF: case OP_DEFI: case OP_DEFB:
        return next;
    case OP_MOV: case OP_MOVA: case OP_FRC: case OP_ABS: case OP_SGN: case OP_NRM:
        read_dst(t, &p, &d), read_src(t, &p, &s0, 0);
        if (op == OP_MOV || op == OP_MOVA) snprintf(v, sizeof v, "%s", s0.s);
        else if (op == OP_FRC) snprintf(v, sizeof v, "(%s - floor(%s))", s0.s, s0.s);
        else if (op == OP_ABS) snprintf(v, sizeof v, "abs(%s)", s0.s);
        else if (op == OP_SGN) snprintf(v, sizeof v, "sign(%s)", s0.s);
        else snprintf(v, sizeof v, "(%s * rsqrt(dot((%s).xyz, (%s).xyz)))", s0.s, s0.s, s0.s);
        write_dst(t, &d, v);
        return next;
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_MIN: case OP_MAX: case OP_SLT: case OP_SGE:
    case OP_DP3: case OP_DP4: case OP_DST: case OP_CRS: case OP_POW:
        read_dst(t, &p, &d), read_src(t, &p, &s0, 0), read_src(t, &p, &s1, 0);
        switch (op) {
        case OP_ADD: snprintf(v, sizeof v, "(%s + %s)", s0.s, s1.s); break;
        case OP_SUB: snprintf(v, sizeof v, "(%s - %s)", s0.s, s1.s); break;
        case OP_MUL: snprintf(v, sizeof v, "(%s * %s)", s0.s, s1.s); break;
        case OP_MIN: snprintf(v, sizeof v, "min(%s, %s)", s0.s, s1.s); break;
        case OP_MAX: snprintf(v, sizeof v, "max(%s, %s)", s0.s, s1.s); break;
        case OP_SLT: snprintf(v, sizeof v, "float4(%s < %s)", s0.s, s1.s); break;
        case OP_SGE: snprintf(v, sizeof v, "float4(%s >= %s)", s0.s, s1.s); break;
        case OP_DP3: snprintf(v, sizeof v, "float4(dot((%s).xyz, (%s).xyz))", s0.s, s1.s); break;
        case OP_DP4: snprintf(v, sizeof v, "float4(dot(%s, %s))", s0.s, s1.s); break;
        case OP_DST: snprintf(v, sizeof v, "float4(1.0, (%s).y * (%s).y, (%s).z, (%s).w)", s0.s, s1.s, s0.s, s1.s); break;
        case OP_CRS: snprintf(v, sizeof v, "float4(cross((%s).xyz, (%s).xyz), 0.0)", s0.s, s1.s); break;
        case OP_POW:
            scalar(t, &s0, a, sizeof a), scalar(t, &s1, b, sizeof b);
            snprintf(v, sizeof v, "float4(pow(abs(%s), %s))", a, b);
            break;
        }
        write_dst(t, &d, v);
        return next;
    case OP_MAD: case OP_LRP: case OP_CND: case OP_CMP: case OP_DP2ADD:
        read_dst(t, &p, &d), read_src(t, &p, &s0, 0), read_src(t, &p, &s1, 0), read_src(t, &p, &s2, 0);
        switch (op) {
        case OP_MAD: snprintf(v, sizeof v, "(%s * %s + %s)", s0.s, s1.s, s2.s); break;
        case OP_LRP: snprintf(v, sizeof v, "mix(%s, %s, %s)", s2.s, s1.s, s0.s); break;
        case OP_CND: snprintf(v, sizeof v, "select(%s, %s, %s > 0.5)", s2.s, s1.s, s0.s); break;
        case OP_CMP: snprintf(v, sizeof v, "select(%s, %s, %s >= 0.0)", s2.s, s1.s, s0.s); break;
        case OP_DP2ADD: snprintf(v, sizeof v, "float4(dot((%s).xy, (%s).xy) + (%s).x)", s0.s, s1.s, s2.s); break;
        }
        write_dst(t, &d, v);
        return next;
    case OP_RCP: case OP_RSQ: case OP_EXP: case OP_LOG: case OP_EXPP: case OP_LOGP:
        read_dst(t, &p, &d), read_src(t, &p, &s0, 0);
        scalar(t, &s0, a, sizeof a);
        switch (op) {
        case OP_RCP: snprintf(v, sizeof v, "float4(1.0 / %s)", a); break;
        case OP_RSQ: snprintf(v, sizeof v, "float4(rsqrt(abs(%s)))", a); break;
        case OP_EXP: snprintf(v, sizeof v, "float4(exp2(%s))", a); break;
        case OP_LOG: snprintf(v, sizeof v, "float4(%s == 0.0 ? -FLT_MAX : log2(abs(%s)))", a, a); break;
        case OP_EXPP:
            if (t->major < 2)
                snprintf(v, sizeof v, "float4(exp2(floor(%s)), %s - floor(%s), exp2(%s), 1.0)", a, a, a, a);
            else
                snprintf(v, sizeof v, "float4(exp2(%s))", a);
            break;
        case OP_LOGP:
            if (t->major < 2)
                snprintf(v, sizeof v, "float4(floor(log2(abs(%s))), abs(%s) / exp2(floor(log2(abs(%s)))), "
                         "%s == 0.0 ? -FLT_MAX : log2(abs(%s)), 1.0)", a, a, a, a, a);
            else
                snprintf(v, sizeof v, "float4(%s == 0.0 ? -FLT_MAX : log2(abs(%s)))", a, a);
            break;
        }
        write_dst(t, &d, v);
        return next;
    case OP_LIT:
        read_dst(t, &p, &d), read_src(t, &p, &s0, 0);
        snprintf(v, sizeof v, "float4(1.0, max((%s).x, 0.0), (%s).x > 0.0 ? pow(max((%s).y, 0.0), clamp((%s).w, -128.0, 128.0)) : 0.0, 1.0)",
                 s0.s, s0.s, s0.s, s0.s);
        write_dst(t, &d, v);
        return next;
    case OP_SINCOS:
        read_dst(t, &p, &d), read_src(t, &p, &s0, 0);
        scalar(t, &s0, a, sizeof a);
        snprintf(v, sizeof v, "float4(cos(%s), sin(%s), 0.0, 0.0)", a, a);
        write_dst(t, &d, v);
        return next;                                    /* shader model 2's two extra constants are not needed */
    case OP_M4x4: case OP_M4x3: case OP_M3x4: case OP_M3x3: case OP_M3x2: {
        int rows = op == OP_M4x4 || op == OP_M3x4 ? 4 : op == OP_M4x3 || op == OP_M3x3 ? 3 : 2;
        int four = op == OP_M4x4 || op == OP_M4x3;
        read_dst(t, &p, &d), read_src(t, &p, &s0, 0);
        size_t m = p;
        char rowv[4][600];
        for (int r = 0; r < rows; r++) {
            size_t q = m;
            read_src(t, &q, &s1, r);
            snprintf(rowv[r], sizeof rowv[r], four ? "dot(%s, %s)" : "dot((%s).xyz, (%s).xyz)", s0.s, s1.s);
            p = q;
        }
        snprintf(v, sizeof v, "float4(%s, %s, %s, %s)", rowv[0], rowv[1], rows > 2 ? rowv[2] : "0.0",
                 rows > 3 ? rowv[3] : "0.0");
        write_dst(t, &d, v);
        return next;
    }
    case OP_TEXCOORD:
        if (t->minor >= 4) { fail(t, "ps_1_4 texcrd"); return next; }
        read_dst(t, &p, &d);
        snprintf(v, sizeof v, "float4(saturate(tin[%d].xyz), 1.0)", reg_num(d.tok));
        write_dst(t, &d, v);
        return next;
    case OP_TEXKILL:
        read_dst(t, &p, &d);
        line(t, "if (any((%s).xyz < 0.0)) discard_fragment();", d.name);
        return next;
    case OP_TEX:
        if (t->major < 2) {
            if (t->minor >= 4) { fail(t, "ps_1_4 texld"); return next; }
            read_dst(t, &p, &d);                          /* tex t<n>: stage n at t<n>'s coordinates */
            snprintf(a, sizeof a, "t[%d]", reg_num(d.tok));
            sample(t, reg_num(d.tok), a, 0, v, sizeof v);
            write_dst(t, &d, v);
        } else {
            read_dst(t, &p, &d), read_src(t, &p, &s0, 0), read_src(t, &p, &s1, 0);
            sample(t, reg_num(s1.tok), s0.s, control & 3, v, sizeof v);
            write_dst(t, &d, v);
        }
        return next;
    case OP_TEXLDL:
        read_dst(t, &p, &d), read_src(t, &p, &s0, 0), read_src(t, &p, &s1, 0);
        sample(t, reg_num(s1.tok), s0.s, 3, v, sizeof v);
        write_dst(t, &d, v);
        return next;
    case OP_DSX: case OP_DSY:
        read_dst(t, &p, &d), read_src(t, &p, &s0, 0);
        snprintf(v, sizeof v, "%s(%s)", op == OP_DSX ? "dfdx" : "dfdy", s0.s);
        write_dst(t, &d, v);
        return next;
    case OP_IF: {
        read_src(t, &p, &s0, 0);
        line(t, "if ((%s).x != 0.0) {", s0.s);
        t->depth++;
        return next;
    }
    case OP_IFC: case OP_BREAKC: {
        const char *c = cmp_op(control & 7);
        read_src(t, &p, &s0, 0), read_src(t, &p, &s1, 0);
        if (!c) { fail(t, "comparison %d", control); return next; }
        scalar(t, &s0, a, sizeof a), scalar(t, &s1, b, sizeof b);
        if (op == OP_IFC) { line(t, "if (%s %s %s) {", a, c, b); t->depth++; }
        else line(t, "if (%s %s %s) break;", a, c, b);
        return next;
    }
    case OP_ELSE:
        t->depth--, line(t, "} else {"), t->depth++;
        return next;
    case OP_ENDIF: case OP_ENDREP: case OP_ENDLOOP:
        t->depth--, line(t, op == OP_ENDLOOP ? "} }" : "}");
        return next;
    case OP_REP:
        read_src(t, &p, &s0, 0);
        line(t, "for (int _rep%d = 0; _rep%d < int((%s).x); _rep%d++) {", t->depth, t->depth, s0.s, t->depth);
        t->depth++;
        return next;
    case OP_LOOP:
        read_src(t, &p, &s0, 0), read_src(t, &p, &s1, 0);      /* loop aL, i<n>: count, start, step */
        t->uses_aL = 1;
        line(t, "{ aL = int((%s).y); for (int _loop%d = 0; _loop%d < int((%s).x); _loop%d++, aL += int((%s).z)) {",
             s1.s, t->depth, t->depth, s1.s, t->depth, s1.s);
        t->depth++;
        return next;
    case OP_BREAK:
        line(t, "break;");
        return next;
    }
    fail(t, "opcode %d is not translated yet", op);
    return next;
}

static void head(tr *t, sb *o) {
    sb_add(o, "#include <metal_stdlib>\nusing namespace metal;\n\n");
    sb_add(o, "struct dxso_params { float2 half_pixel; float alpha_ref; uint alpha_func; float4 fog_color; "
              "uint fog_enable; uint pad0, pad1, pad2; };\n");
    sb_add(o, "struct dxso_varyings {\n    float4 pos [[position]];\n    float4 d0 [[user(d0)]];\n    float4 d1 [[user(d1)]];\n");
    for (int i = 0; i < 8; i++) sb_add(o, "    float4 t%d [[user(t%d)]];\n", i, i);
    sb_add(o, "    float fog [[user(fog)]];\n};\n\n");
    (void)t;
}

static void sampler_args(tr *t, sb *o) {
    for (int s = 0; s < 16; s++) {
        if (!((t->out->samplers_used >> s) & 1)) continue;
        if (t->opt && (t->opt->sampler_depth >> s) & 1)
            sb_add(o, ",\n    depth2d<float> s%d [[texture(%d)]], sampler z%d [[sampler(%d)]]", s, s, s, s);
        else
            sb_add(o, ",\n    %s<float> s%d [[texture(%d)]], sampler z%d [[sampler(%d)]]",
                   t->out->sampler_type[s] == DXSO_TEX_CUBE ? "texturecube" : t->out->sampler_type[s] == DXSO_TEX_3D ? "texture3d" : "texture2d",
                   s, s, s, s);
    }
}

static void common_args(sb *o, int ps) {
    sb_add(o, "    constant float4 *c [[buffer(0)]], constant int4 *ci [[buffer(1)]], constant uint *cb [[buffer(2)]],\n");
    sb_add(o, "    constant dxso_params &P [[buffer(3)]]");
    (void)ps;
}

static void locals(tr *t, sb *o) {
    sb_add(o, "    float4 r[%d] = {};\n", t->max_temp + 1);
    if (t->uses_a0) sb_add(o, "    int4 a0 = int4(0);\n");
    if (t->uses_aL) sb_add(o, "    int aL = 0;\n");
    if (t->uses_p0) sb_add(o, "    bool4 p0 = bool4(false);\n");
}

static int vertex(tr *t, sb *o) {
    int inputs = 0;
    for (int i = 0; i < DXSO_MAX_INPUTS; i++) inputs += t->out->input_usage[i] != 0xFF;
    if (inputs) {
        sb_add(o, "struct dxso_in {\n");
        for (int i = 0; i < DXSO_MAX_INPUTS; i++)
            if (t->out->input_usage[i] != 0xFF) sb_add(o, "    float4 v%d [[attribute(%d)]];\n", i, i);
        sb_add(o, "};\n\n");
    }
    sb_add(o, "vertex dxso_varyings dxso_main(%s\n", inputs ? "dxso_in in [[stage_in]]," : "");
    common_args(o, 0);
    sb_add(o, ")\n{\n");
    locals(t, o);
    sb_add(o, "    float4 v[%d] = {};\n", t->max_v + 1);
    for (int i = 0; i < DXSO_MAX_INPUTS; i++)
        if (t->out->input_usage[i] != 0xFF) sb_add(o, "    v[%d] = in.v%d;\n", i, i);
    sb_add(o, "    float4 o_pos = float4(0.0, 0.0, 0.0, 1.0), o_d[2] = { float4(0.0), float4(0.0) }, o_t[8] = {};\n");
    sb_add(o, "    float o_fog = 1.0, o_pts = 1.0;\n");
    sb_add(o, "%s", t->body.p ? t->body.p : "");
    sb_add(o, "    dxso_varyings out;\n");
    sb_add(o, "    out.pos = o_pos;\n");
    sb_add(o, "    out.pos.xy += P.half_pixel * o_pos.w;        /* D3D9's pixel centres */\n");
    sb_add(o, "    out.d0 = saturate(o_d[0]); out.d1 = saturate(o_d[1]);   /* D3D9 clamps the colours */\n");
    for (int i = 0; i < 8; i++) sb_add(o, "    out.t%d = o_t[%d];\n", i, i);
    sb_add(o, "    out.fog = o_fog;\n    return out;\n}\n");
    return 0;
}

static int pixel(tr *t, sb *o) {
    int sm1 = t->major < 2;
    sb_add(o, "struct dxso_out {\n");
    int ncol = sm1 ? 1 : t->max_oc + 1;
    if (ncol < 1) ncol = 1;
    for (int i = 0; i < ncol; i++) sb_add(o, "    float4 c%d [[color(%d)]];\n", i, i);
    if (t->uses_depth_out) sb_add(o, "    float depth [[depth(any)]];\n");
    sb_add(o, "};\n\n");
    sb_add(o, "fragment dxso_out dxso_main(dxso_varyings in [[stage_in]],%s%s\n", t->uses_vface ? " bool front [[front_facing]]," : "", "");
    common_args(o, 1);
    sampler_args(t, o);
    sb_add(o, ")\n{\n");
    locals(t, o);
    sb_add(o, "    float4 vd[2] = { in.d0, in.d1 };\n");
    sb_add(o, "    float4 tin[8] = { in.t0, in.t1, in.t2, in.t3, in.t4, in.t5, in.t6, in.t7 };\n");
    sb_add(o, "    float4 t[8] = { in.t0, in.t1, in.t2, in.t3, in.t4, in.t5, in.t6, in.t7 };\n");
    sb_add(o, "    float4 o_c[%d] = {};\n    float o_depth = in.pos.z;\n", ncol);
    if (t->uses_vpos) sb_add(o, "    float4 vpos = float4(in.pos.xy - 0.5, 0.0, 0.0);\n");
    if (t->uses_vface) sb_add(o, "    float4 vface = float4(front ? 1.0 : -1.0);\n");
    sb_add(o, "%s", t->body.p ? t->body.p : "");
    if (sm1) sb_add(o, "    o_c[0] = r[0];                                 /* ps_1_x: r0 is the colour */\n");
    sb_add(o, "    float4 col = o_c[0];\n");
    sb_add(o, "    switch (P.alpha_func) {                        /* the alpha test (D3DCMP_*) */\n"
              "    case 1: discard_fragment(); break;\n"
              "    case 2: if (!(col.a <  P.alpha_ref)) discard_fragment(); break;\n"
              "    case 3: if (!(col.a == P.alpha_ref)) discard_fragment(); break;\n"
              "    case 4: if (!(col.a <= P.alpha_ref)) discard_fragment(); break;\n"
              "    case 5: if (!(col.a >  P.alpha_ref)) discard_fragment(); break;\n"
              "    case 6: if (!(col.a != P.alpha_ref)) discard_fragment(); break;\n"
              "    case 7: if (!(col.a >= P.alpha_ref)) discard_fragment(); break;\n"
              "    default: break;\n    }\n");
    sb_add(o, "    if (P.fog_enable) col.rgb = mix(P.fog_color.rgb, col.rgb, saturate(in.fog));\n");
    sb_add(o, "    dxso_out out;\n    out.c0 = col;\n");
    for (int i = 1; i < ncol; i++) sb_add(o, "    out.c%d = o_c[%d];\n", i, i);
    if (t->uses_depth_out) sb_add(o, "    out.depth = o_depth;\n");
    sb_add(o, "    return out;\n}\n");
    return 0;
}

int dxso_translate(const uint32_t *code, size_t words, const dxso_options *opt, dxso_shader *out) {
    memset(out, 0, sizeof *out);
    memset(out->input_usage, 0xFF, sizeof out->input_usage);
    if (!code || words < 2) { snprintf(out->error, sizeof out->error, "no bytecode"); return -1; }
    tr *t = calloc(1, sizeof *t);
    t->w = code, t->n = words, t->opt = opt, t->out = out;
    uint32_t ver = code[0];
    t->ps = (ver >> 16) == 0xFFFF;
    if (!t->ps && (ver >> 16) != 0xFFFE) { snprintf(out->error, sizeof out->error, "not a shader (0x%08X)", ver); free(t); return -1; }
    t->major = (int)((ver >> 8) & 0xFF), t->minor = (int)(ver & 0xFF);
    out->pixel = t->ps, out->major = t->major, out->minor = t->minor;
    if (t->major > 3 || t->major < 1) { snprintf(out->error, sizeof out->error, "shader model %d", t->major); free(t); return -1; }
    t->max_temp = 0, t->max_v = 0, t->max_oc = 0;
    for (int s = 0; s < 16; s++) out->sampler_type[s] = opt && t->major < 2 ? opt->sampler_type[s] : DXSO_TEX_2D;
    declarations(t);
    for (size_t at = 1; at < words && !t->failed;) {
        uint32_t tok = code[at];
        int op = (int)(tok & 0xFFFF);
        if (op == OP_END) break;
        if (op == OP_COMMENT) { at += 1 + ((tok >> 16) & 0x7FFF); continue; }
        at = instruction(t, at);
    }
    if (t->depth != 0 && !t->failed) fail(t, "unbalanced flow control");
    if (!t->failed) {
        sb o = { 0 };
        head(t, &o);
        if (t->ps) pixel(t, &o); else vertex(t, &o);
        out->msl = o.p;
    }
    free(t->body.p);
    int rc = t->failed ? -1 : 0;
    free(t);
    return rc;
}

void dxso_free(dxso_shader *s) {
    free(s->msl);
    s->msl = NULL;
}
