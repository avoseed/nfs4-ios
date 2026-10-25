/* GLIDE3X replacement: Voodoo-style pipeline implemented as a software rasterizer.
 * The Thrash driver (voodoo2a.dll, recompiled) talks to this; frames go to plat_present(). */
#include "hle.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern int g_screen_w, g_screen_h;

#define WIDE_CODE 0x9               /* GR_RESOLUTION_960x720, reused for the device-shaped mode */
#define MAXW 4096
#define MAXH 2048

/* ---------------------------------------------------------------- state */

typedef struct { float r, g, b, a; } Col;

static int W = 640, H = 480;
static uint32_t *g_fb[2];             /* RGBA8 (R in low byte), top row first */
static uint16_t *g_depth;
static int g_back = 1, g_render = 1;
static int g_origin_lower_left;
static int g_color_format;             /* GR_COLORFORMAT_* */
static int g_clip[4];
static int g_cull;
static int g_coord_space;

/* vertex layout offsets (-1 = disabled) */
static int L_xy = 0, L_z = -1, L_w = -1, L_q = -1, L_a = -1, L_rgb = -1, L_pargb = -1, L_st0 = -1, L_q0 = -1, L_fog = -1;

static struct { int func, factor, local, other, invert; } g_cc, g_ac;
static struct { int rgb_func, rgb_factor, a_func, a_factor, rgb_inv, a_inv; } g_tc;
static int g_blend_src = 4, g_blend_dst = 0, g_blend_asrc = 4, g_blend_adst = 0;
static int g_atest = 7, g_aref;
static int g_chroma, g_chroma_r, g_chroma_g, g_chroma_b;
static int g_depth_mode, g_depth_func = 1, g_depth_mask = 1, g_depth_bias;
static int g_fog_mode;
static uint8_t g_fog_table[64];
static float g_fog_r, g_fog_g, g_fog_b;
static uint32_t g_const_color = 0xFFFFFFFF;

/* texture memory (Voodoo2 TMU: 4 MB) */
/* Texture memory is far larger than a Voodoo2's 4 MB so Modern Patch's AllowHugeTextures (up to
 * 2048x2048, the size reported by grGet below) has room for high-resolution car and track art. */
#define TMU_SIZE (64u << 20)
static uint8_t g_tmu[TMU_SIZE];
static uint32_t g_palette[256];        /* ARGB */
typedef struct { uint32_t addr; int small_lod, large_lod, aspect, format; int w, h; } Tex;
static Tex g_tex;
static int g_tex_valid;
static int g_filter_bilinear = 1, g_clamp_s, g_clamp_t;

/* LFB */
static uint32_t g_lfb_guest;           /* guest buffer handed to the game */
static uint16_t *g_lfb_snapshot;
static int g_lfb_locked_buffer = -1, g_lfb_write_mode, g_lfb_locked_type;

static int g_frames;
static uint32_t g_err_cb;
static uint64_t g_raster_us, g_swap_t0;   /* profiling */
static uint32_t g_tris, g_lfb_locks;

/* GPU path (see gpu.h): batches of window-space triangles with a state snapshot */
#include "gpu.h"
static int g_gpu;
#define BATCH_MAX (3 * 8192)
static GpuVertex g_batch[BATCH_MAX];
static int g_nbatch;
static GpuState g_bstate;
static int g_tex_used;
static uint32_t g_palette_serial = 1;

/* ---------------------------------------------------------------- helpers */

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static Col unpack_color(uint32_t c) {
    /* GrColor_t in the format chosen at grSstWinOpen */
    uint8_t b0 = c & 0xFF, b1 = (c >> 8) & 0xFF, b2 = (c >> 16) & 0xFF, b3 = c >> 24;
    Col o;
    switch (g_color_format) {
    case 1: o.r = b0; o.g = b1; o.b = b2; o.a = b3; break;      /* ABGR */
    case 2: o.r = b3; o.g = b2; o.b = b1; o.a = b0; break;      /* RGBA */
    case 3: o.r = b1; o.g = b2; o.b = b3; o.a = b0; break;      /* BGRA */
    default: o.r = b2; o.g = b1; o.b = b0; o.a = b3; break;     /* ARGB */
    }
    return o;
}

static inline uint32_t pack_rgba(float r, float g, float b, float a) {
    uint32_t R = (uint32_t)clampf(r, 0, 255), G = (uint32_t)clampf(g, 0, 255), B = (uint32_t)clampf(b, 0, 255),
             A = (uint32_t)clampf(a, 0, 255);
    return R | (G << 8) | (B << 16) | (A << 24);
}

static int tex_dims(int lod, int aspect, int *w, int *h) {
    int big = 1 << lod;
    if (aspect >= 0) { *w = big; *h = big >> aspect; }
    else { *h = big; *w = big >> -aspect; }
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
    return 0;
}

#define TEXFMT_ARGB_8888 0x12               /* Glide 3 Napalm extension */
static int tex_bpp(int fmt) { return fmt == TEXFMT_ARGB_8888 ? 4 : fmt >= 8 ? 2 : 1; }

static uint32_t tex_mem_required(int small_lod, int large_lod, int aspect, int fmt) {
    uint32_t total = 0;
    for (int l = large_lod; l >= small_lod; l--) {
        int w, h;
        tex_dims(l, aspect, &w, &h);
        total += (uint32_t)(w * h * tex_bpp(fmt));
    }
    return (total + 7) & ~7u;
}

/* fetch one texel of the current texture as r,g,b,a 0..255 */
static inline Col texel(int x, int y) {
    const Tex *t = &g_tex;
    if (g_clamp_s) x = x < 0 ? 0 : x >= t->w ? t->w - 1 : x; else x &= t->w - 1;
    if (g_clamp_t) y = y < 0 ? 0 : y >= t->h ? t->h - 1 : y; else y &= t->h - 1;
    uint32_t i = (uint32_t)(y * t->w + x);
    Col c;
    if (t->format == TEXFMT_ARGB_8888) {
        uint32_t a = t->addr + i * 4;
        uint32_t v = a + 3 < TMU_SIZE ? (uint32_t)(g_tmu[a] | (g_tmu[a + 1] << 8) | (g_tmu[a + 2] << 16) | ((uint32_t)g_tmu[a + 3] << 24)) : 0;
        c.a = v >> 24; c.r = (v >> 16) & 0xFF; c.g = (v >> 8) & 0xFF; c.b = v & 0xFF;
    } else if (tex_bpp(t->format) == 2) {
        uint32_t a = t->addr + i * 2;
        uint16_t v = a + 1 < TMU_SIZE ? (uint16_t)(g_tmu[a] | (g_tmu[a + 1] << 8)) : 0;
        switch (t->format) {
        case 0xA: c.r = ((v >> 11) & 31) * 255 / 31; c.g = ((v >> 5) & 63) * 255 / 63; c.b = (v & 31) * 255 / 31; c.a = 255; break;
        case 0xB: c.r = ((v >> 10) & 31) * 255 / 31; c.g = ((v >> 5) & 31) * 255 / 31; c.b = (v & 31) * 255 / 31; c.a = (v & 0x8000) ? 255 : 0; break;
        case 0xC: c.a = ((v >> 12) & 15) * 17; c.r = ((v >> 8) & 15) * 17; c.g = ((v >> 4) & 15) * 17; c.b = (v & 15) * 17; break;
        case 0xD: c.r = c.g = c.b = v & 0xFF; c.a = v >> 8; break;                         /* AI88 */
        case 0xE: { uint32_t p = g_palette[v & 0xFF]; c.r = (p >> 16) & 0xFF; c.g = (p >> 8) & 0xFF; c.b = p & 0xFF; c.a = v >> 8; break; }
        case 0x8: { uint32_t r = (v >> 5) & 7, g = (v >> 2) & 7, b = v & 3; c.r = r * 255 / 7; c.g = g * 255 / 7; c.b = b * 255 / 3; c.a = v >> 8; break; }
        default: c.r = c.g = c.b = 255; c.a = 255;
        }
    } else {
        uint32_t a = t->addr + i;
        uint8_t v = a < TMU_SIZE ? g_tmu[a] : 0;
        switch (t->format) {
        case 0x0: c.r = ((v >> 5) & 7) * 255 / 7; c.g = ((v >> 2) & 7) * 255 / 7; c.b = (v & 3) * 255 / 3; c.a = 255; break;
        case 0x2: c.r = c.g = c.b = 255; c.a = v; break;
        case 0x3: c.r = c.g = c.b = v; c.a = 255; break;
        case 0x4: c.r = c.g = c.b = (v & 15) * 17; c.a = (v >> 4) * 17; break;
        case 0x5: case 0x6: { uint32_t p = g_palette[v]; c.r = (p >> 16) & 0xFF; c.g = (p >> 8) & 0xFF; c.b = p & 0xFF;
                              c.a = t->format == 6 ? (p >> 24) : 255; break; }
        default: c.r = c.g = c.b = v; c.a = 255;
        }
    }
    return c;
}

static inline Col sample(float s, float t) {
    /* Glide: the larger texture side spans 0..256 in s/t space */
    int maxd = g_tex.w > g_tex.h ? g_tex.w : g_tex.h;
    float k = (float)maxd / 256.0f;
    float u = s * k, v = t * k;
    if (!g_filter_bilinear) return texel((int)floorf(u), (int)floorf(v));
    u -= 0.5f; v -= 0.5f;
    int x0 = (int)floorf(u), y0 = (int)floorf(v);
    float fx = u - x0, fy = v - y0;
    Col a = texel(x0, y0), b = texel(x0 + 1, y0), c = texel(x0, y0 + 1), d = texel(x0 + 1, y0 + 1);
    Col o;
    o.r = (a.r * (1 - fx) + b.r * fx) * (1 - fy) + (c.r * (1 - fx) + d.r * fx) * fy;
    o.g = (a.g * (1 - fx) + b.g * fx) * (1 - fy) + (c.g * (1 - fx) + d.g * fx) * fy;
    o.b = (a.b * (1 - fx) + b.b * fx) * (1 - fy) + (c.b * (1 - fx) + d.b * fx) * fy;
    o.a = (a.a * (1 - fx) + b.a * fx) * (1 - fy) + (c.a * (1 - fx) + d.a * fx) * fy;
    return o;
}

/* Voodoo combine unit, values 0..255 */
static inline float factor_value(int f, float local, float local_a, float other_a, float tex_a) {
    switch (f & 7) {
    case 0: return (f & 8) ? 255 : 0;
    case 1: return (f & 8) ? 255 - local : local;
    case 2: return (f & 8) ? 255 - other_a : other_a;
    case 3: return (f & 8) ? 255 - local_a : local_a;
    case 4: return (f & 8) ? 255 - tex_a : tex_a;
    default: return (f & 8) ? 0 : 255;         /* LOD fraction / texture rgb: approximate */
    }
}

static inline float combine(int func, float fct, float local, float local_a, float other) {
    fct /= 255.0f;
    switch (func) {
    case 0x0: return 0;
    case 0x1: return local;
    case 0x2: return local_a;
    case 0x3: return fct * other;
    case 0x4: return fct * other + local;
    case 0x5: return fct * other + local_a;
    case 0x6: return fct * (other - local);
    case 0x7: return fct * (other - local) + local;
    case 0x8: return fct * (other - local) + local_a;
    case 0x9: return -fct * local + local;
    case 0x10: return -fct * local + local_a;
    }
    return local;
}

static inline int depth_pass(int func, uint32_t z, uint32_t old) {
    switch (func) {
    case 0: return 0;
    case 1: return z < old;
    case 2: return z == old;
    case 3: return z <= old;
    case 4: return z > old;
    case 5: return z != old;
    case 6: return z >= old;
    default: return 1;
    }
}

static inline float blend_factor(int f, int is_src, Col s, Col d, int ch) {
    float sc = ch == 0 ? s.r : ch == 1 ? s.g : ch == 2 ? s.b : s.a;
    float dc = ch == 0 ? d.r : ch == 1 ? d.g : ch == 2 ? d.b : d.a;
    switch (f) {
    case 0: return 0;
    case 1: return s.a / 255.0f;
    case 2: return (is_src ? dc : sc) / 255.0f;       /* DST_COLOR as src factor, SRC_COLOR as dst */
    case 3: return d.a / 255.0f;
    case 4: return 1;
    case 5: return 1 - s.a / 255.0f;
    case 6: return 1 - (is_src ? dc : sc) / 255.0f;
    case 7: return 1 - d.a / 255.0f;
    case 15: return is_src ? fminf(s.a, 255 - d.a) / 255.0f : 1;
    }
    return 1;
}

static float fog_from_w(float w) {
    /* Glide fog table index i covers w = 2^(3+(i>>2)) / (8-(i&3)) */
    if (w <= 1) return g_fog_table[0];
    for (int i = 1; i < 64; i++) {
        float wi = powf(2.0f, 3.0f + (float)(i >> 2)) / (float)(8 - (i & 3));
        if (w < wi) {
            float wp = i == 1 ? 1.0f : powf(2.0f, 3.0f + (float)((i - 1) >> 2)) / (float)(8 - ((i - 1) & 3));
            float t = (w - wp) / (wi - wp);
            return g_fog_table[i - 1] + (g_fog_table[i] - g_fog_table[i - 1]) * t;
        }
    }
    return g_fog_table[63];
}

/* ---------------------------------------------------------------- vertices */

typedef struct { float x, y, z, oow, sow, tow, r, g, b, a, fog; } V;

static float rdf(uint32_t p) { float f; memcpy(&f, MEM + p, 4); return f; }

static V read_vertex(uint32_t p) {
    V v = { 0 };
    v.x = rdf(p + L_xy); v.y = rdf(p + L_xy + 4);
    v.z = L_z >= 0 ? rdf(p + L_z) : 0;
    v.oow = L_q >= 0 ? rdf(p + L_q) : L_w >= 0 ? rdf(p + L_w) : 1.0f;
    float q0 = L_q0 >= 0 ? rdf(p + L_q0) : v.oow;
    if (L_st0 >= 0) { v.sow = rdf(p + L_st0); v.tow = rdf(p + L_st0 + 4); }
    (void)q0;
    if (L_pargb >= 0) {
        uint32_t c = RD32(p + L_pargb);
        v.a = (float)(c >> 24); v.r = (float)((c >> 16) & 0xFF); v.g = (float)((c >> 8) & 0xFF); v.b = (float)(c & 0xFF);
    } else {
        if (L_rgb >= 0) { v.r = rdf(p + L_rgb); v.g = rdf(p + L_rgb + 4); v.b = rdf(p + L_rgb + 8); }
        else v.r = v.g = v.b = 255;
        v.a = L_a >= 0 ? rdf(p + L_a) : 255;
    }
    if (L_fog >= 0) v.fog = rdf(p + L_fog);
    if (g_origin_lower_left) v.y = (float)H - v.y;
    return v;
}

/* ---------------------------------------------------------------- rasterizer */

static void shade_pixel(int x, int y, float z, float oow, float sow, float tow, float r, float g, float b, float a, float fogc) {
    uint32_t *fb = g_fb[g_render];
    int idx = y * W + x;
    /* depth */
    uint32_t depth = 0;
    if (g_depth_mode) {
        if (g_depth_mode == 2 || g_depth_mode == 4) {
            float w = oow > 0 ? 1.0f / oow : 65535.0f;
            depth = (uint32_t)clampf(w * 16.0f, 0, 65535);      /* coarse w-buffer */
        } else {
            depth = (uint32_t)clampf(z, 0, 65535);
        }
        int biased = (int)depth + g_depth_bias;
        depth = (uint32_t)(biased < 0 ? 0 : biased > 65535 ? 65535 : biased);
        if (!depth_pass(g_depth_func, depth, g_depth[idx])) return;
    }
    /* texture unit */
    Col tex = { 255, 255, 255, 255 };
    if (g_tex_used && g_tex_valid) {
        float w = oow != 0 ? 1.0f / oow : 1.0f;
        Col t = sample(sow * w, tow * w);
        if (g_chroma && (int)t.r == g_chroma_r && (int)t.g == g_chroma_g && (int)t.b == g_chroma_b) return;
        /* texture combine: local = texel, other = 0 (single TMU) */
        float fr = factor_value(g_tc.rgb_factor, t.r, t.a, 0, t.a);
        float fa = factor_value(g_tc.a_factor, t.a, t.a, 0, t.a);
        tex.r = combine(g_tc.rgb_func, fr, t.r, t.a, 0);
        tex.g = combine(g_tc.rgb_func, factor_value(g_tc.rgb_factor, t.g, t.a, 0, t.a), t.g, t.a, 0);
        tex.b = combine(g_tc.rgb_func, factor_value(g_tc.rgb_factor, t.b, t.a, 0, t.a), t.b, t.a, 0);
        tex.a = combine(g_tc.a_func, fa, t.a, t.a, 0);
        if (g_tc.rgb_inv) { tex.r = 255 - tex.r; tex.g = 255 - tex.g; tex.b = 255 - tex.b; }
        if (g_tc.a_inv) tex.a = 255 - tex.a;
    }
    Col cst = unpack_color(g_const_color);
    Col iter = { r, g, b, a };
    /* alpha combine */
    float la = g_ac.local == 1 ? cst.a : g_ac.local == 2 ? clampf(z / 256.0f, 0, 255) : iter.a;
    float oa = g_ac.other == 1 ? tex.a : g_ac.other == 2 ? cst.a : iter.a;
    float af = factor_value(g_ac.factor, la, la, oa, tex.a);
    float out_a = combine(g_ac.func, af, la, la, oa);
    if (g_ac.invert) out_a = 255 - out_a;
    out_a = clampf(out_a, 0, 255);
    /* color combine */
    Col lc = g_cc.local == 1 ? cst : iter;
    Col oc = g_cc.other == 1 ? tex : g_cc.other == 2 ? cst : iter;
    float lca = la;
    Col out;
    out.r = combine(g_cc.func, factor_value(g_cc.factor, lc.r, lca, oa, tex.a), lc.r, lca, oc.r);
    out.g = combine(g_cc.func, factor_value(g_cc.factor, lc.g, lca, oa, tex.a), lc.g, lca, oc.g);
    out.b = combine(g_cc.func, factor_value(g_cc.factor, lc.b, lca, oa, tex.a), lc.b, lca, oc.b);
    if (g_cc.invert) { out.r = 255 - out.r; out.g = 255 - out.g; out.b = 255 - out.b; }
    out.a = out_a;
    if (g_chroma && !(g_tex_used && g_tex_valid) && (int)out.r == g_chroma_r && (int)out.g == g_chroma_g && (int)out.b == g_chroma_b) return;
    /* alpha test */
    {
        int av = (int)out.a, ref = g_aref, pass;
        switch (g_atest) {
        case 0: pass = 0; break; case 1: pass = av < ref; break; case 2: pass = av == ref; break;
        case 3: pass = av <= ref; break; case 4: pass = av > ref; break; case 5: pass = av != ref; break;
        case 6: pass = av >= ref; break; default: pass = 1;
        }
        if (!pass) return;
    }
    /* fog */
    if (g_fog_mode & 0xFF) {
        float f;
        int fm = g_fog_mode & 0xFF;
        if (fm == 1) f = fogc;
        else if (fm == 3) f = clampf(z / 256.0f, 0, 255);
        else if (fm == 4) f = iter.a;
        else f = fog_from_w(oow != 0 ? 1.0f / oow : 65535.0f);
        f = clampf(f, 0, 255) / 255.0f;
        out.r = out.r * (1 - f) + g_fog_r * f;
        out.g = out.g * (1 - f) + g_fog_g * f;
        out.b = out.b * (1 - f) + g_fog_b * f;
    }
    /* blend */
    if (!(g_blend_src == 4 && g_blend_dst == 0)) {
        uint32_t d = fb[idx];
        Col dc = { (float)(d & 0xFF), (float)((d >> 8) & 0xFF), (float)((d >> 16) & 0xFF), (float)(d >> 24) };
        Col s = out;
        out.r = s.r * blend_factor(g_blend_src, 1, s, dc, 0) + dc.r * blend_factor(g_blend_dst, 0, s, dc, 0);
        out.g = s.g * blend_factor(g_blend_src, 1, s, dc, 1) + dc.g * blend_factor(g_blend_dst, 0, s, dc, 1);
        out.b = s.b * blend_factor(g_blend_src, 1, s, dc, 2) + dc.b * blend_factor(g_blend_dst, 0, s, dc, 2);
        out.a = s.a * blend_factor(g_blend_asrc, 1, s, dc, 3) + dc.a * blend_factor(g_blend_adst, 0, s, dc, 3);
    }
    fb[idx] = pack_rgba(out.r, out.g, out.b, out.a);
    if (g_depth_mode && g_depth_mask) g_depth[idx] = (uint16_t)depth;
}

/* ---------------------------------------------------------------- GPU path */

/* decoded texture cache keyed by TMU address + format; downloads invalidate overlaps */
typedef struct { uint32_t addr, bytes; int format, large_lod, aspect; uint32_t pal; int id, w, h, stale; } TexEntry;
#define TEXCACHE 2048
static TexEntry g_texcache[TEXCACHE];
static int g_ntex;

static void tex_invalidate(uint32_t a, uint32_t n) {
    for (int i = 0; i < g_ntex; i++)
        if (g_texcache[i].addr < a + n && a < g_texcache[i].addr + g_texcache[i].bytes) g_texcache[i].stale = 1;
}

static int is_palettized(int fmt) { return fmt == 5 || fmt == 6 || fmt == 0xE; }

static int g_last_tex = -1;           /* index into g_texcache of the last lookup */

static int gpu_tex_id(void) {
    if (!(g_tex_used && g_tex_valid)) return -1;
    const Tex *t = &g_tex;
    uint32_t pal = is_palettized(t->format) ? g_palette_serial : 0;
    if (g_last_tex >= 0 && g_last_tex < g_ntex) {
        TexEntry *x = &g_texcache[g_last_tex];
        if (x->addr == t->addr && x->format == t->format && x->large_lod == t->large_lod && x->aspect == t->aspect &&
            !x->stale && x->pal == pal)
            return x->id;
    }
    TexEntry *e = NULL;
    for (int i = 0; i < g_ntex; i++) {
        TexEntry *x = &g_texcache[i];
        if (x->addr == t->addr && x->format == t->format && x->large_lod == t->large_lod && x->aspect == t->aspect) {
            e = x;
            break;
        }
    }
    if (e && !e->stale && e->pal == pal) { g_last_tex = (int)(e - g_texcache); return e->id; }
    if (!e) {
        if (g_ntex >= TEXCACHE) {                         /* full: drop everything */
            for (int i = 0; i < g_ntex; i++) gpu_texture_destroy(g_texcache[i].id);
            g_ntex = 0;
        }
        e = &g_texcache[g_ntex++];
        memset(e, 0, sizeof *e);
        e->id = -1;
    }
    if (e->id >= 0 && (e->w != t->w || e->h != t->h)) { gpu_texture_destroy(e->id); e->id = -1; }
    if (e->id < 0) e->id = gpu_texture_create(t->w, t->h);
    e->addr = t->addr; e->format = t->format; e->large_lod = t->large_lod; e->aspect = t->aspect;
    e->w = t->w; e->h = t->h; e->pal = pal; e->stale = 0;
    e->bytes = tex_mem_required(t->small_lod, t->large_lod, t->aspect, t->format);
    uint32_t *px = malloc((size_t)t->w * t->h * 4);
    int cs = g_clamp_s, ct = g_clamp_t;
    g_clamp_s = g_clamp_t = 1;
    for (int y = 0; y < t->h; y++)
        for (int x = 0; x < t->w; x++) {
            Col k = texel(x, y);
            px[y * t->w + x] = pack_rgba(k.r, k.g, k.b, k.a);
        }
    g_clamp_s = cs; g_clamp_t = ct;
    gpu_texture_upload(e->id, px);
    free(px);
    g_last_tex = (int)(e - g_texcache);
    return e->id;
}

static void gpu_flush(void) {
    if (g_nbatch) gpu_draw(&g_bstate, g_batch, g_nbatch);
    g_nbatch = 0;
}

static void gpu_build_state(GpuState *s) {
    memset(s, 0, sizeof *s);
    GpuShade *h = &s->shade;
    h->cc[0] = g_cc.func; h->cc[1] = g_cc.factor; h->cc[2] = g_cc.local; h->cc[3] = g_cc.other;
    h->ac[0] = g_ac.func; h->ac[1] = g_ac.factor; h->ac[2] = g_ac.local; h->ac[3] = g_ac.other;
    h->tc[0] = g_tc.rgb_func; h->tc[1] = g_tc.rgb_factor; h->tc[2] = g_tc.a_func; h->tc[3] = g_tc.a_factor;
    h->flags[0] = g_cc.invert; h->flags[1] = g_ac.invert; h->flags[2] = g_tc.rgb_inv; h->flags[3] = g_tc.a_inv;
    s->tex = gpu_tex_id();
    h->mode[0] = s->tex >= 0; h->mode[1] = g_atest; h->mode[2] = g_chroma; h->mode[3] = g_fog_mode & 0xFF;
    h->aref[0] = g_aref / 255.0f;
    h->chroma[0] = g_chroma_r / 255.0f; h->chroma[1] = g_chroma_g / 255.0f; h->chroma[2] = g_chroma_b / 255.0f;
    h->fogc[0] = g_fog_r / 255.0f; h->fogc[1] = g_fog_g / 255.0f; h->fogc[2] = g_fog_b / 255.0f;
    Col k = unpack_color(g_const_color);
    h->constc[0] = k.r / 255.0f; h->constc[1] = k.g / 255.0f; h->constc[2] = k.b / 255.0f; h->constc[3] = k.a / 255.0f;
    if (g_fog_mode & 0xFF) for (int i = 0; i < 64; i++) h->fogtab[i] = g_fog_table[i] / 255.0f;
    s->blend[0] = g_blend_src; s->blend[1] = g_blend_dst; s->blend[2] = g_blend_asrc; s->blend[3] = g_blend_adst;
    s->depth_func = g_depth_mode ? g_depth_func : -1;
    s->depth_write = g_depth_mode && g_depth_mask;
    s->scissor[0] = g_clip[0]; s->scissor[1] = g_clip[1]; s->scissor[2] = g_clip[2]; s->scissor[3] = g_clip[3];
    s->bilinear = g_filter_bilinear; s->clamp_s = g_clamp_s; s->clamp_t = g_clamp_t;
}

static GpuVertex gpu_vertex(const V *v) {
    GpuVertex o;
    o.x = v->x; o.y = v->y;
    float oow = v->oow;
    if (g_depth_mode == 2 || g_depth_mode == 4) o.d = 1.0f - oow * 0.25f;          /* w-buffer: linear in 1/w */
    else o.d = v->z / 65535.0f;
    if (g_depth_mode) o.d += g_depth_bias / 65535.0f;
    o.oow = oow;
    float w = oow != 0 ? 1.0f / oow : 1.0f;
    int maxd = g_tex.w > g_tex.h ? g_tex.w : g_tex.h;
    float k = g_tex_valid ? (float)maxd / 256.0f : 1.0f;
    o.s = v->sow * w * k / (g_tex.w ? g_tex.w : 1);
    o.t = v->tow * w * k / (g_tex.h ? g_tex.h : 1);
    o.fog = v->fog / 255.0f;
    o.rgba = pack_rgba(v->r, v->g, v->b, v->a);
    return o;
}

static void gpu_triangle(const V *a, const V *b, const V *c) {
    GpuState s;
    gpu_build_state(&s);
    if (g_nbatch && (memcmp(&s, &g_bstate, sizeof s) || g_nbatch + 3 > BATCH_MAX)) gpu_flush();
    if (!g_nbatch) g_bstate = s;
    g_batch[g_nbatch++] = gpu_vertex(a);
    g_batch[g_nbatch++] = gpu_vertex(b);
    g_batch[g_nbatch++] = gpu_vertex(c);
}

static void draw_triangle(V a, V b, V c) {
    float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
    if (area == 0) return;
    if (g_cull == 1 && area < 0) return;     /* GR_CULL_NEGATIVE */
    if (g_cull == 2 && area > 0) return;     /* GR_CULL_POSITIVE */
    if (g_gpu) { gpu_triangle(&a, &b, &c); return; }
    if (g_origin_lower_left && g_cull) { /* y flip reverses winding; handled by caller convention */ }
    if (area < 0) { V t = b; b = c; c = t; area = -area; }
    int minx = (int)floorf(fminf(a.x, fminf(b.x, c.x))), maxx = (int)ceilf(fmaxf(a.x, fmaxf(b.x, c.x)));
    int miny = (int)floorf(fminf(a.y, fminf(b.y, c.y))), maxy = (int)ceilf(fmaxf(a.y, fmaxf(b.y, c.y)));
    if (minx < g_clip[0]) minx = g_clip[0];
    if (miny < g_clip[1]) miny = g_clip[1];
    if (maxx > g_clip[2]) maxx = g_clip[2];
    if (maxy > g_clip[3]) maxy = g_clip[3];
    if (minx >= maxx || miny >= maxy) return;
    float inv = 1.0f / area;
    for (int y = miny; y < maxy; y++) {
        float py = y + 0.5f;
        for (int x = minx; x < maxx; x++) {
            float px = x + 0.5f;
            float w0 = (b.x - px) * (c.y - py) - (c.x - px) * (b.y - py);
            float w1 = (c.x - px) * (a.y - py) - (a.x - px) * (c.y - py);
            float w2 = (a.x - px) * (b.y - py) - (b.x - px) * (a.y - py);
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            w0 *= inv; w1 *= inv; w2 *= inv;
#define I(f) (a.f * w0 + b.f * w1 + c.f * w2)
            shade_pixel(x, y, I(z), I(oow), I(sow), I(tow), I(r), I(g), I(b), I(a), I(fog));
#undef I
        }
    }
}

/* ---------------------------------------------------------------- API */

static void update_tex_usage(void) {
    g_tex_used = g_cc.other == 1 || g_ac.other == 1 || g_cc.factor == 4 || g_cc.factor == 0xC || g_ac.factor == 4;
}

#define GF(name) static void name(Cpu *c)
static float argf(Cpu *c, int i) { uint32_t v = ARG(i); float f; memcpy(&f, &v, 4); return f; }

GF(grGlideInit) { plat_log("glide: grGlideInit"); RETURN(0, 0); }
GF(grGlideShutdown) { RETURN(0, 0); }
GF(grSstSelect) { RETURN(1, 0); }
GF(grErrorSetCallback) { g_err_cb = ARG(0); RETURN(1, 0); }
GF(grFinish) { RETURN(0, 0); }
GF(grDisable) { RETURN(1, 0); }
GF(grEnable) { RETURN(1, 0); }
GF(grDitherMode) { RETURN(1, 0); }
GF(guGammaCorrectionRGB) { RETURN(3, 0); }

static const int g_res_w[] = { 320, 320, 400, 512, 640, 640, 640, 640, 800, 960, 856, 512, 1024, 1280, 1600, 400 };
static const int g_res_h[] = { 200, 240, 256, 384, 200, 350, 400, 480, 600, 720, 480, 256, 768, 1024, 1200, 300 };

static int g_res_w_wide, g_res_h_wide;  /* device-shaped mode replacing 960x720, 0 = off */

/* The race resolution (config.dat, or the last non-640x480 window). The front end always runs at
 * 640x480, so a 640x480 window with a different race resolution is a menu screen. */
static int g_race_w, g_race_h, g_window_open;

static void open_window(uint32_t res, int fmt, int origin) {
    if (res < 16) { W = g_res_w[res]; H = g_res_h[res]; }
    if (res == WIDE_CODE && g_res_w_wide) { W = g_res_w_wide; H = g_res_h_wide; }
    if (W > MAXW) W = MAXW;
    if (H > MAXH) H = MAXH;
    g_window_open = 1;
    g_screen_w = W; g_screen_h = H;
    if (!(W == 640 && H == 480)) { g_race_w = W; g_race_h = H; }
    g_color_format = fmt;
    g_origin_lower_left = origin == 1;
    for (int i = 0; i < 2; i++) { free(g_fb[i]); g_fb[i] = calloc((size_t)W * H, 4); }
    free(g_depth); g_depth = calloc((size_t)W * H, 2);
    free(g_lfb_snapshot); g_lfb_snapshot = calloc((size_t)W * H, 2);
    g_clip[0] = 0; g_clip[1] = 0; g_clip[2] = W; g_clip[3] = H;
    static uint32_t lfb_bytes;
    if ((uint32_t)(W * H * 4) > lfb_bytes) {
        if (g_lfb_guest) guest_free(g_lfb_guest);
        lfb_bytes = (uint32_t)(W * H * 4);
        g_lfb_guest = guest_alloc(lfb_bytes);
    }
    g_nbatch = 0;
    g_gpu = getenv("NFS4_SOFTWARE") ? 0 : gpu_init(W, H);
    plat_log("glide: window %dx%d colorformat %d origin %s, %s renderer", W, H, fmt,
             g_origin_lower_left ? "lower-left" : "upper-left", g_gpu ? "GPU" : "software");
}

/* GPU path: clears are drawn as a quad over the clip window (colour = iterated, depth always) */
static void gpu_clear_rect(uint32_t rgba, float depth, int with_depth) {
    gpu_flush();
    GpuState s;
    memset(&s, 0, sizeof s);
    s.shade.cc[0] = 1; s.shade.cc[2] = 0;          /* out = local = iterated colour */
    s.shade.ac[0] = 1; s.shade.ac[2] = 0;
    s.shade.mode[1] = 7;                           /* alpha test always */
    s.blend[0] = 4; s.blend[1] = 0; s.blend[2] = 4; s.blend[3] = 0;
    s.depth_func = with_depth ? 7 : -1;
    s.depth_write = with_depth;
    memcpy(s.scissor, g_clip, sizeof s.scissor);
    s.tex = -1;
    GpuVertex q[6];
    float xs[2] = { (float)g_clip[0], (float)g_clip[2] }, ys[2] = { (float)g_clip[1], (float)g_clip[3] };
    int order[6][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 0 }, { 1, 1 }, { 0, 1 } };
    for (int i = 0; i < 6; i++) {
        q[i].x = xs[order[i][0]]; q[i].y = ys[order[i][1]];
        q[i].d = depth; q[i].oow = 1; q[i].s = q[i].t = 0; q[i].fog = 0; q[i].rgba = rgba;
    }
    gpu_draw(&s, q, 6);
}

/* GPU path: framebuffer writes through the LFB become a textured quad over the changed pixels */
static void gpu_lfb_blit(const uint32_t *rgba) {
    static int id = -1, tw, th;
    if (id < 0 || tw != W || th != H) { if (id >= 0) gpu_texture_destroy(id); id = gpu_texture_create(W, H); tw = W; th = H; }
    gpu_texture_upload(id, rgba);
    gpu_flush();
    GpuState s;
    memset(&s, 0, sizeof s);
    s.shade.cc[0] = 1; s.shade.cc[2] = 1; s.shade.cc[3] = 1;     /* placeholder, overridden below */
    s.shade.cc[0] = 0x3; s.shade.cc[1] = 0x8; s.shade.cc[2] = 0; s.shade.cc[3] = 1;   /* out = 1 * texture */
    s.shade.ac[0] = 0x3; s.shade.ac[1] = 0x8; s.shade.ac[2] = 0; s.shade.ac[3] = 1;
    s.shade.tc[0] = 1; s.shade.tc[1] = 0; s.shade.tc[2] = 1; s.shade.tc[3] = 0;       /* texel as-is */
    s.shade.mode[0] = 1; s.shade.mode[1] = 4;                                        /* alpha > 0 */
    s.blend[0] = 4; s.blend[1] = 0; s.blend[2] = 4; s.blend[3] = 0;
    s.depth_func = -1;
    s.scissor[2] = W; s.scissor[3] = H;
    s.tex = id; s.clamp_s = s.clamp_t = 1;
    GpuVertex q[6];
    float xs[2] = { 0, (float)W }, ys[2] = { 0, (float)H };
    int order[6][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 0 }, { 1, 1 }, { 0, 1 } };
    for (int i = 0; i < 6; i++) {
        q[i].x = xs[order[i][0]]; q[i].y = ys[order[i][1]];
        q[i].d = 0; q[i].oow = 1; q[i].s = (float)order[i][0]; q[i].t = (float)order[i][1];
        q[i].fog = 0; q[i].rgba = 0xFFFFFFFFu;
    }
    gpu_draw(&s, q, 6);
}

/* NFS4 MSVC Thrash descriptors: 40-byte records of uint32 values. The
 * 960x720 record is at rebased VA 0x01014130 (verified from this DLL).
 * NFS4 config stores display width at 0x120, not NFS3's width/height pair. */
static int wide_mode(int *w, int *h) {
    float a = plat_wide_aspect();
    if (a < 1.4f) return 0;
    if (a > 16.0f / 9.0f) a = 16.0f / 9.0f;
    *h = 768; *w = (int)(768 * a) & ~7; return 1;
}
void hle_glide_patch_module(const char *file, uint32_t base) {
    int w,h;
    if (!strstr(file,"voodoo2a") || base != 0x01000000u || !wide_mode(&w,&h)) return;
    uint32_t p=base+0x14130u;
    if (RD32(p)!=960 || RD32(p+4)!=720 || RD32(p+8)!=16 || RD32(p+16)!=1) {
        plat_log("NFS4 driver mode signature differs; widescreen disabled"); return;
    }
    WR32(p,w); WR32(p+4,h); g_res_w_wide=w; g_res_h_wide=h;
    plat_log("NFS4 driver 960x720 descriptor -> %dx%d",w,h);
}
int hle_glide_wide_mode(uint32_t w,uint32_t h) { return g_res_w_wide && w==(uint32_t)g_res_w_wide && h==(uint32_t)g_res_h_wide; }
void hle_glide_prepare_modes(void) {
    if (!g_res_w_wide) return;
    /* Engine display-memory budget used by the enumerator at 0051A4BA. */
    WR32(0x005BEFA4u,32u<<20);
    WR32(0x01014144u,2);WR32(0x01014148u,2);
}
void hle_glide_select_race_entry(void) {
    if (!g_res_w_wide) return;
    uint32_t n=RD32(0x00727524u);
    if (n>20) return;
    for(uint32_t i=0;i<n;i++) {
        uint32_t p=0x00727528u+276u*i;
        if(RD32(p)==3 && hle_glide_wide_mode(RD32(p+4),RD32(p+8))) {
            WR32(0x007C3D20u,i);
            static int logged;if(!logged){logged=1;plat_log("NFS4 race settings: display record %u/%u is %ux%u",i,n,RD32(p+4),RD32(p+8));}
            return;
        }
    }
    static int warned;if(!warned){warned=1;plat_log("NFS4 wide display record missing (%u modes)",n);}
}
uint32_t hle_glide_set_mode(uint32_t native_mode,uint32_t caller) {
    /* 004F73E0 enters the race display, 004F7740 restores the front end.
     * The initial engine display caller is 0050499E. Keep their native modes.
     * Verified against the current exe call sites and actual mode-switch log. */
    if (!g_res_w_wide || caller!=0x004F73E0u) return native_mode;
    uint32_t p=0x01014130u;
    if (RD32(p)!=(uint32_t)g_res_w_wide || RD32(p+4)!=(uint32_t)g_res_h_wide || RD32(p+8)!=16) return native_mode;
    WR32(p+16,RD32(p+16)|1u);WR32(p+20,2);WR32(p+24,2);
    /* A direct front-end renderer call can change the window behind the
     * engine's cached mode index, so invalidate it only when sizes disagree. */
    if(W!=g_res_w_wide || H!=g_res_h_wide) WR32(0x005BEF94u,0xFFFFFFFFu);
    plat_log("NFS4 race display: mode %u -> 3 (%dx%d)",native_mode,g_res_w_wide,g_res_h_wide);
    return 3;
}
/* Preserve the complete config payload; select wide at the display-mode chooser. */
void hle_glide_fix_config(uint8_t *buf,uint32_t off,uint32_t n){(void)buf;(void)off;(void)n;}
void hle_glide_size(int *w, int *h) { *w = W; *h = H; }

/* The NFS4 front end and race can both render at 640x480. Paired execution
 * traces identify its front-end hit-test and race-control functions; the generator
 * observes those entries without changing game logic. See docs/controls.md. */
static uint64_t g_frontend_seen;
static int g_race_seen;
void hle_frontend_observed(int menu) {
    if (menu) __atomic_store_n(&g_frontend_seen, plat_ticks_us(), __ATOMIC_RELAXED);
    else __atomic_store_n(&g_race_seen, 1, __ATOMIC_RELAXED);
}
int hle_glide_in_menu(void) {
    if (!__atomic_load_n(&g_race_seen, __ATOMIC_RELAXED)) return 1;
    /* Race control still runs behind the pause dialog. A recent front-end
     * hit-test takes priority so the pause menu keeps its menu buttons. */
    uint64_t last=__atomic_load_n(&g_frontend_seen,__ATOMIC_RELAXED);
    return last && plat_ticks_us()-last < 150000u;
}

GF(grSstWinOpen) {
    open_window(ARG(1), (int)ARG(3), (int)ARG(4));
    RETURN(7, 1);
}
GF(grSstWinOpenExt) {
    open_window(ARG(1), (int)ARG(3), (int)ARG(4));
    RETURN(8, 1);
}
GF(grSstWinClose) { RETURN(1, 1); }

GF(grQueryResolutions) {
    /* Glide GrResolution templates use 0xFFFFFFFF for an unconstrained field.
     * Honor requested buffer counts; returning an unrelated fixed list made the
     * NFS4 driver reject its larger modes. GPU/CPU presentation has two color
     * buffers and one optional depth buffer. */
    uint32_t t=ARG(0),out=ARG(1),want[4]={0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu};
    if(t)for(int i=0;i<4;i++)want[i]=RD32(t+4*i);
    static const uint32_t modes[]={7,8,9,10,12,13,14};
    uint32_t count=0;
    for(unsigned k=0;k<sizeof modes/sizeof modes[0];k++)for(uint32_t colors=1;colors<=2;colors++)for(uint32_t aux=0;aux<=1;aux++) {
        uint32_t v[4]={modes[k],0,colors,aux};int match=1;
        for(int i=0;i<4;i++)if(want[i]!=0xFFFFFFFFu&&want[i]!=v[i])match=0;
        if(!match)continue;
        if(out)for(int i=0;i<4;i++)WR32(out+count*16+4*i,v[i]);
        count++;
    }
    RETURN(2,count*16);
}

GF(grGet) {
    uint32_t pname = ARG(0), len = ARG(1), p = ARG(2);
    uint32_t v[4] = { 0 };
    int n = 1;
    switch (pname) {
    case 0x01: v[0] = 16; break;                        /* bits depth */
    case 0x02: v[0] = 5; v[1] = 6; v[2] = 5; v[3] = 0; n = 4; break;
    case 0x04: v[0] = 64; break;                        /* fog table entries */
    case 0x05: v[0] = 32; break;
    case 0x06: v[0] = 1024; break;
    case 0x07: v[0] = 256; break;
    case 0x0a: v[0] = 2048; break;                      /* max texture size: the game caps AllowHugeTextures to it */
    case 0x0b: v[0] = 3; break;
    case 0x0c: v[0] = 32u << 20; break;                  /* frame buffer memory: lets wide modes pass the game's check */
    case 0x0d: v[0] = TMU_SIZE; break;
    case 0x0e: v[0] = 0; break;                         /* unified texture memory: none (asked every frame) */
    case 0x0f: v[0] = 1; break;                         /* boards */
    case 0x11: v[0] = 1; break;
    case 0x13: v[0] = 1; break;                         /* TMUs */
    case 0x14: v[0] = 0; break;                         /* pending buffer swaps */
    case 0x15: v[0] = 2; break;
    case 0x16: v[0] = 1; break;
    case 0x24: v[0] = 8; break;
    case 0x26: v[0] = 0; v[1] = 0; v[2] = W; v[3] = H; n = 4; break; /* GR_VIEWPORT */
    case 0x27: { float lo = 0, hi = 65535; memcpy(&v[0], &lo, 4); memcpy(&v[1], &hi, 4); n = 2; break; }
    case 0x28: v[0] = 0; v[1] = 0xFFFF; n = 2; break;
    case 0x2a: v[0] = 8; break;
    default: {
        static uint8_t logged[256];                     /* once each: some are asked every frame */
        if (pname < 256 && !logged[pname]) { logged[pname] = 1; plat_log("glide: grGet(%02X) unknown", pname); }
    }
    }
    for (int i = 0; i < n && (uint32_t)(i * 4) < len; i++) WR32(p + i * 4, v[i]);
    RETURN(3, (uint32_t)n * 4 < len ? (uint32_t)n * 4 : len);
}

GF(grGetString) {
    static uint32_t s[5];
    static const char *txt[5] = { "", "Voodoo2", "Glide", "3Dfx Interactive", "3.10" };
    uint32_t i = ARG(0) - 0xA0;
    if (i > 4) i = 0;
    if (!s[i]) s[i] = guest_strdup(txt[i]);
    RETURN(1, s[i]);
}

GF(grGetProcAddress) {
    const char *name = STR(ARG(0));
    HleFn fn = rt_resolve_import("glide3x.dll", name);
    plat_log("glide: grGetProcAddress(%s)%s", name, fn == hle_unimplemented ? " -> NULL" : "");
    RETURN(1, fn == hle_unimplemented ? 0 : rt_import_address("glide3x.dll", name, fn));
}

GF(grVertexLayout) {
    uint32_t param = ARG(0);
    int off = (int)ARG(1), on = ARG(2) != 0;
    int v = on ? off : -1;
    switch (param) {
    case 0x01: L_xy = on ? off : 0; break;
    case 0x02: L_z = v; break;
    case 0x03: L_w = v; break;
    case 0x04: L_q = v; break;
    case 0x05: L_fog = v; break;
    case 0x10: L_a = v; break;
    case 0x20: L_rgb = v; break;
    case 0x30: L_pargb = v; break;
    case 0x40: L_st0 = v; break;
    case 0x50: L_q0 = v; break;
    }
    RETURN(3, 0);
}
GF(grCoordinateSpace) { g_coord_space = (int)ARG(0); RETURN(1, 0); }

GF(grClipWindow) {
    int x0 = (int)ARG(0), y0 = (int)ARG(1), x1 = (int)ARG(2), y1 = (int)ARG(3);
    if (g_origin_lower_left) { int t = H - y1; y1 = H - y0; y0 = t; }
    g_clip[0] = x0 < 0 ? 0 : x0; g_clip[1] = y0 < 0 ? 0 : y0;
    g_clip[2] = x1 > W ? W : x1; g_clip[3] = y1 > H ? H : y1;
    RETURN(4, 0);
}
GF(grCullMode) { g_cull = (int)ARG(0); RETURN(1, 0); }
GF(grColorCombine) {
    g_cc.func = (int)ARG(0); g_cc.factor = (int)ARG(1); g_cc.local = (int)ARG(2); g_cc.other = (int)ARG(3); g_cc.invert = (int)ARG(4);
    update_tex_usage();
    RETURN(5, 0);
}
GF(grAlphaCombine) {
    g_ac.func = (int)ARG(0); g_ac.factor = (int)ARG(1); g_ac.local = (int)ARG(2); g_ac.other = (int)ARG(3); g_ac.invert = (int)ARG(4);
    update_tex_usage();
    RETURN(5, 0);
}
GF(grTexCombine) {
    g_tc.rgb_func = (int)ARG(1); g_tc.rgb_factor = (int)ARG(2); g_tc.a_func = (int)ARG(3); g_tc.a_factor = (int)ARG(4);
    g_tc.rgb_inv = (int)ARG(5); g_tc.a_inv = (int)ARG(6);
    RETURN(7, 0);
}
GF(grAlphaBlendFunction) {
    g_blend_src = (int)ARG(0); g_blend_dst = (int)ARG(1); g_blend_asrc = (int)ARG(2); g_blend_adst = (int)ARG(3);
    RETURN(4, 0);
}
GF(grAlphaTestFunction) { g_atest = (int)ARG(0); RETURN(1, 0); }
GF(grAlphaTestReferenceValue) { g_aref = (int)(ARG(0) & 0xFF); RETURN(1, 0); }
GF(grChromakeyMode) { g_chroma = (int)ARG(0); RETURN(1, 0); }
GF(grChromakeyValue) {
    Col k = unpack_color(ARG(0));
    g_chroma_r = (int)k.r; g_chroma_g = (int)k.g; g_chroma_b = (int)k.b;
    RETURN(1, 0);
}
GF(grDepthBufferMode) { g_depth_mode = (int)ARG(0); RETURN(1, 0); }
GF(grDepthBufferFunction) { g_depth_func = (int)ARG(0); RETURN(1, 0); }
GF(grDepthMask) { g_depth_mask = ARG(0) != 0; RETURN(1, 0); }
GF(grDepthBiasLevel) { g_depth_bias = (int16_t)ARG(0); RETURN(1, 0); }
GF(grFogMode) { g_fog_mode = (int)ARG(0); RETURN(1, 0); }
GF(grFogColorValue) {
    Col k = unpack_color(ARG(0));
    g_fog_r = k.r; g_fog_g = k.g; g_fog_b = k.b;
    RETURN(1, 0);
}
GF(grFogTable) { memcpy(g_fog_table, MEM + ARG(0), 64); RETURN(1, 0); }
GF(grRenderBuffer) { g_render = ARG(0) == 0 ? (1 - g_back) : g_back; RETURN(1, 0); }

GF(grBufferClear) {
    Col k = unpack_color(ARG(0));
    uint32_t v = pack_rgba(k.r, k.g, k.b, (float)(ARG(1) & 0xFF));
    uint16_t d = (uint16_t)ARG(2);
    if (g_gpu) {
        gpu_flush();
        float dv = (g_depth_mode == 2 || g_depth_mode == 4) ? (d >= 0xFFF0 ? 1.0f : d / 65535.0f) : d / 65535.0f;
        gpu_clear_rect(v, dv, g_depth_mode != 0);
        RETURN(3, 0);
    }
    uint32_t *fb = g_fb[g_render];
    for (int y = g_clip[1]; y < g_clip[3]; y++) {
        for (int x = g_clip[0]; x < g_clip[2]; x++) {
            fb[y * W + x] = v;
            if (g_depth_mode) g_depth[y * W + x] = d;
        }
    }
    RETURN(3, 0);
}

/* The game renders as fast as it can and the host GPU is far faster than a Voodoo2, so swaps are
 * paced to the host's frame cap: frames above the display rate are never seen, only cost heat. */
static uint64_t g_next_swap;
static void pace_swap(void) {
    int cap = plat_frame_cap();
    if (cap <= 0) return;
    uint64_t period = 1000000 / (uint64_t)cap, now = plat_ticks_us();
    if (g_next_swap > now + period) g_next_swap = now;          /* first frame, or the cap was raised */
    if (now < g_next_swap) plat_sleep_us((uint32_t)(g_next_swap - now));
    else if (now - g_next_swap > 2 * period) g_next_swap = now; /* far behind: restart the schedule */
    /* a slightly late frame keeps the schedule, so the next fast frames catch up instead of
     * every slow frame lowering the average */
    g_next_swap += period;
}

GF(grBufferSwap) {
    pace_swap();
    if (g_gpu) { gpu_flush(); gpu_swap(); }
    else plat_present(g_fb[g_back], W, H);
    g_back = 1 - g_back;
    g_render = g_back;
    if (++g_frames % 300 == 0) {
        uint64_t now = plat_ticks_us();
        double secs = g_swap_t0 ? (now - g_swap_t0) / 1e6 : 0;
        plat_log("glide: %d frames, %.1f fps, raster %.1f ms/frame, %u tris/frame, %s", g_frames, secs > 0 ? 300 / secs : 0,
                 g_raster_us / 1000.0 / 300, g_tris / 300, hle_glide_in_menu() ? "menu" : "race");
        plat_log("glide:   lfb locks %u in last 300 frames", g_lfb_locks);
        g_swap_t0 = now; g_raster_us = 0; g_tris = 0; g_lfb_locks = 0;
    }
    RETURN(1, 0);
}

GF(grDrawTriangle) {
    uint64_t t0 = plat_ticks_us();
    draw_triangle(read_vertex(ARG(0)), read_vertex(ARG(1)), read_vertex(ARG(2)));
    g_raster_us += plat_ticks_us() - t0;
    g_tris++;
    RETURN(3, 0);
}
GF(grAADrawTriangle) {
    g_tris++;
    draw_triangle(read_vertex(ARG(0)), read_vertex(ARG(1)), read_vertex(ARG(2)));
    RETURN(6, 0);
}
GF(grDrawLine) {
    V a = read_vertex(ARG(0)), b = read_vertex(ARG(1));
    if (g_gpu) {                       /* one-pixel-wide quad */
        float dx = b.x - a.x, dy = b.y - a.y, len = sqrtf(dx * dx + dy * dy);
        if (len > 0) {
            float nx = -dy / len * 0.5f, ny = dx / len * 0.5f;
            V p0 = a, p1 = a, p2 = b, p3 = b;
            p0.x += nx; p0.y += ny; p1.x -= nx; p1.y -= ny; p2.x -= nx; p2.y -= ny; p3.x += nx; p3.y += ny;
            gpu_triangle(&p0, &p1, &p2); gpu_triangle(&p0, &p2, &p3);
        }
        RETURN(2, 0);
    }
    int n = (int)fmaxf(fabsf(b.x - a.x), fabsf(b.y - a.y)) + 1;
    for (int i = 0; i <= n; i++) {
        float t = (float)i / (float)n;
        int x = (int)(a.x + (b.x - a.x) * t), y = (int)(a.y + (b.y - a.y) * t);
        if (x >= g_clip[0] && x < g_clip[2] && y >= g_clip[1] && y < g_clip[3])
            shade_pixel(x, y, a.z + (b.z - a.z) * t, a.oow + (b.oow - a.oow) * t, a.sow + (b.sow - a.sow) * t,
                        a.tow + (b.tow - a.tow) * t, a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                        a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t, a.fog);
    }
    RETURN(2, 0);
}
GF(grDrawPoint) {
    V a = read_vertex(ARG(0));
    if (g_gpu) {
        V p0 = a, p1 = a, p2 = a, p3 = a;
        p1.x += 1; p2.x += 1; p2.y += 1; p3.y += 1;
        gpu_triangle(&p0, &p1, &p2); gpu_triangle(&p0, &p2, &p3);
        RETURN(1, 0);
    }
    int x = (int)a.x, y = (int)a.y;
    if (x >= g_clip[0] && x < g_clip[2] && y >= g_clip[1] && y < g_clip[3])
        shade_pixel(x, y, a.z, a.oow, a.sow, a.tow, a.r, a.g, a.b, a.a, a.fog);
    RETURN(1, 0);
}

/* textures */
GF(grTexMinAddress) { RETURN(1, 0); }
GF(grTexMaxAddress) { RETURN(1, TMU_SIZE - 0x10000); }
GF(grTexCalcMemRequired) { RETURN(4, tex_mem_required((int)ARG(0), (int)ARG(1), (int)ARG(2), (int)ARG(3))); }

static void read_texinfo(uint32_t info, int *small, int *large, int *aspect, int *fmt, uint32_t *data) {
    *small = (int)RD32(info); *large = (int)RD32(info + 4); *aspect = (int)RD32(info + 8);
    *fmt = (int)RD32(info + 12); *data = RD32(info + 16);
}

GF(grTexDownloadMipMap) {
    uint32_t start = ARG(1), info = ARG(3);
    int small, large, aspect, fmt; uint32_t data;
    read_texinfo(info, &small, &large, &aspect, &fmt, &data);
    uint32_t n = tex_mem_required(small, large, aspect, fmt);
    if (large > 8) {
        static int logged;
        int w, h;
        tex_dims(large, aspect, &w, &h);
        if (logged++ < 8) plat_log("glide: huge texture %dx%d format %X at %08X", w, h, fmt, start);
    }
    if (start + n <= TMU_SIZE) memcpy(g_tmu + start, MEM + data, n);
    if (g_gpu) { gpu_flush(); tex_invalidate(start, n); }
    RETURN(4, 0);
}
GF(grTexDownloadMipMapLevelPartial) {
    uint32_t start = ARG(1);
    int lod = (int)ARG(2), large = (int)ARG(3), aspect = (int)ARG(4), fmt = (int)ARG(5);
    uint32_t data = ARG(7);
    int row0 = (int)ARG(8), row1 = (int)ARG(9);
    /* offset of this LOD inside the mip chain */
    uint32_t off = 0;
    for (int l = large; l > lod; l--) { int w, h; tex_dims(l, aspect, &w, &h); off += (uint32_t)(w * h * tex_bpp(fmt)); }
    int w, h;
    tex_dims(lod, aspect, &w, &h);
    if (large > 8) {
        static int logged;
        if (logged++ < 8) plat_log("glide: huge texture level %dx%d format %X at %08X", w, h, fmt, start);
    }
    uint32_t stride = (uint32_t)(w * tex_bpp(fmt));
    uint32_t dst = start + off + (uint32_t)row0 * stride, n = (uint32_t)(row1 - row0 + 1) * stride;
    if (dst + n <= TMU_SIZE) memcpy(g_tmu + dst, MEM + data, n);
    if (g_gpu) { gpu_flush(); tex_invalidate(dst, n); }
    RETURN(10, 0);
}
GF(grTexDownloadTable) {
    uint32_t type = ARG(0), data = ARG(1);
    if (type == 2 || type == 3) for (int i = 0; i < 256; i++) g_palette[i] = RD32(data + 4 * i);
    if (g_gpu) gpu_flush();
    g_palette_serial++;
    RETURN(2, 0);
}
GF(grTexSource) {
    uint32_t start = ARG(1), info = ARG(3);
    int small, large, aspect, fmt; uint32_t data;
    read_texinfo(info, &small, &large, &aspect, &fmt, &data);
    g_tex.addr = start; g_tex.small_lod = small; g_tex.large_lod = large; g_tex.aspect = aspect; g_tex.format = fmt;
    tex_dims(large, aspect, &g_tex.w, &g_tex.h);
    g_tex_valid = 1;
    RETURN(4, 0);
}
GF(grTexFilterMode) { g_filter_bilinear = ARG(2) != 0; RETURN(3, 0); }
GF(grTexClampMode) { g_clamp_s = ARG(1) == 1; g_clamp_t = ARG(2) == 1; RETURN(3, 0); }
GF(grTexMipMapMode) { RETURN(3, 0); }
GF(grTexLodBiasValue) { RETURN(2, 0); }

/* linear frame buffer */
static uint16_t to565(uint32_t v) {
    uint32_t r = v & 0xFF, g = (v >> 8) & 0xFF, b = (v >> 16) & 0xFF;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
static uint32_t from565(uint16_t p) {
    uint32_t r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
    return (r * 255 / 31) | ((g * 255 / 63) << 8) | ((b * 255 / 31) << 16) | 0xFF000000u;
}
static uint32_t from555(uint16_t p) {
    uint32_t r = (p >> 10) & 31, g = (p >> 5) & 31, b = p & 31;
    return (r * 255 / 31) | ((g * 255 / 31) << 8) | ((b * 255 / 31) << 16) | 0xFF000000u;
}

GF(grLfbLock) {
    g_lfb_locks++;
    if (g_lfb_locks < 20) plat_log("glide: grLfbLock(type %X, buffer %u, mode %u, pipeline %u) from %08X", ARG(0), ARG(1), ARG(2), ARG(4), RD32(c->esp));
    uint32_t type = ARG(0) & 0xF, buffer = ARG(1), mode = ARG(2), info = ARG(5);
    int b = buffer == 0 ? 1 - g_back : buffer == 1 ? g_back : g_render;
    if (mode == 0xFF) mode = 0;   /* ANY -> 565 */
    g_lfb_locked_buffer = b;
    g_lfb_write_mode = (int)mode;
    g_lfb_locked_type = (int)type;
    uint32_t *fb = g_fb[b];
    int bpp = (mode == 4 || mode == 5) ? 4 : 2;
    for (int i = 0; i < W * H; i++) {
        uint16_t p = to565(fb[i]);
        g_lfb_snapshot[i] = p;
        int y = i / W, x = i % W;
        uint32_t a = g_lfb_guest + (uint32_t)(y * W + x) * (uint32_t)bpp;
        if (bpp == 2) WR16(a, p);
        else WR32(a, ((fb[i] & 0xFF) << 16) | (fb[i] & 0xFF00) | ((fb[i] >> 16) & 0xFF) | 0xFF000000u);
    }
    WR32(info + 4, g_lfb_guest);
    WR32(info + 8, (uint32_t)(W * bpp));
    WR32(info + 12, mode);
    WR32(info + 16, 0);
    RETURN(6, 1);
}

GF(grLfbUnlock) {
    if (g_gpu && g_lfb_locked_buffer >= 0 && g_lfb_locked_type == 1) {
        /* only pixels the game changed are drawn (alpha 0 elsewhere) */
        uint32_t *px = calloc((size_t)W * H, 4);
        int bpp = (g_lfb_write_mode == 4 || g_lfb_write_mode == 5) ? 4 : 2, any = 0;
        for (int i = 0; i < W * H; i++) {
            uint32_t a = g_lfb_guest + (uint32_t)i * (uint32_t)bpp;
            if (bpp == 2) {
                uint16_t p = RD16(a);
                if (p != g_lfb_snapshot[i]) { px[i] = g_lfb_write_mode == 1 || g_lfb_write_mode == 2 ? from555(p) : from565(p); any = 1; }
            } else {
                uint32_t p = RD32(a);
                px[i] = ((p >> 16) & 0xFF) | (p & 0xFF00) | ((p & 0xFF) << 16) | 0xFF000000u;
                any = 1;
            }
        }
        if (any) gpu_lfb_blit(px);
        free(px);
        g_lfb_locked_buffer = -1;
        RETURN(2, 1);
    }
    if (g_lfb_locked_buffer >= 0 && g_lfb_locked_type == 1) {
        uint32_t *fb = g_fb[g_lfb_locked_buffer];
        int bpp = (g_lfb_write_mode == 4 || g_lfb_write_mode == 5) ? 4 : 2;
        for (int i = 0; i < W * H; i++) {
            uint32_t a = g_lfb_guest + (uint32_t)i * (uint32_t)bpp;
            if (bpp == 2) {
                uint16_t p = RD16(a);
                if (p != g_lfb_snapshot[i]) fb[i] = g_lfb_write_mode == 1 || g_lfb_write_mode == 2 ? from555(p) : from565(p);
            } else {
                uint32_t p = RD32(a);
                fb[i] = ((p >> 16) & 0xFF) | (p & 0xFF00) | ((p & 0xFF) << 16) | 0xFF000000u;
            }
        }
    }
    g_lfb_locked_buffer = -1;
    RETURN(2, 1);
}

GF(grLfbReadRegion) {
    uint32_t buffer = ARG(0), x0 = ARG(1), y0 = ARG(2), w = ARG(3), h = ARG(4), stride = ARG(5), dst = ARG(6);
    int b = buffer == 0 ? 1 - g_back : g_back;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t sx = x0 + x, sy = y0 + y;
            uint16_t p = (sx < (uint32_t)W && sy < (uint32_t)H) ? to565(g_fb[b][sy * W + sx]) : 0;
            WR16(dst + y * stride + x * 2, p);
        }
    RETURN(7, 1);
}

GF(grLfbWriteRegion) {
    uint32_t buffer = ARG(0), x0 = ARG(1), y0 = ARG(2), fmt = ARG(3), w = ARG(4), h = ARG(5), stride = ARG(7), src = ARG(8);
    int b = buffer == 0 ? 1 - g_back : g_back;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t dx = x0 + x, dy = y0 + y;
            if (dx >= (uint32_t)W || dy >= (uint32_t)H) continue;
            uint32_t v;
            if (fmt == 4 || fmt == 5) {
                uint32_t p = RD32(src + y * stride + x * 4);
                v = ((p >> 16) & 0xFF) | (p & 0xFF00) | ((p & 0xFF) << 16) | 0xFF000000u;
            } else {
                uint16_t p = RD16(src + y * stride + x * 2);
                v = (fmt == 1 || fmt == 2) ? from555(p) : from565(p);
            }
            g_fb[b][dy * W + dx] = v;
        }
    RETURN(9, 1);
}

#define G(n) { "_" #n, n }
#define G2(n, k) { "_" #n "@" #k, n }
const HleEntry hle_glide[] = {
    G2(grGlideInit, 0), G2(grGlideShutdown, 0), G2(grSstSelect, 4), G2(grErrorSetCallback, 4), G2(grFinish, 0),
    G2(grDisable, 4), G2(grEnable, 4), G2(grDitherMode, 4), G2(guGammaCorrectionRGB, 12),
    G2(grSstWinOpen, 28), { "grSstWinOpenExt", grSstWinOpenExt }, G2(grSstWinClose, 4), G2(grQueryResolutions, 8),
    G2(grGet, 12), G2(grGetString, 4), G2(grGetProcAddress, 4), G2(grVertexLayout, 12), G2(grCoordinateSpace, 4),
    G2(grClipWindow, 16), G2(grCullMode, 4), G2(grColorCombine, 20), G2(grAlphaCombine, 20), G2(grTexCombine, 28),
    G2(grAlphaBlendFunction, 16), G2(grAlphaTestFunction, 4), G2(grAlphaTestReferenceValue, 4),
    G2(grChromakeyMode, 4), G2(grChromakeyValue, 4), G2(grDepthBufferMode, 4), G2(grDepthBufferFunction, 4),
    G2(grDepthMask, 4), G2(grDepthBiasLevel, 4), G2(grFogMode, 4), G2(grFogColorValue, 4), G2(grFogTable, 4),
    G2(grRenderBuffer, 4), G2(grBufferClear, 12), G2(grBufferSwap, 4), G2(grDrawTriangle, 12),
    G2(grAADrawTriangle, 24), G2(grDrawLine, 8), G2(grDrawPoint, 4), G2(grTexMinAddress, 4), G2(grTexMaxAddress, 4),
    G2(grTexCalcMemRequired, 16), G2(grTexDownloadMipMap, 16), G2(grTexDownloadMipMapLevelPartial, 40),
    G2(grTexDownloadTable, 8), G2(grTexSource, 16), G2(grTexFilterMode, 12), G2(grTexClampMode, 12),
    G2(grTexMipMapMode, 12), G2(grTexLodBiasValue, 8), G2(grLfbLock, 24), G2(grLfbUnlock, 8),
    G2(grLfbReadRegion, 28), G2(grLfbWriteRegion, 36),
    { NULL, NULL }
};
