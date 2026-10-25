/* GPU backend interface used by the Glide replacement (Metal on iOS, Direct3D 11 on the
 * Windows dev host). The Glide layer converts everything to window-space triangles with a
 * state snapshot; backends only translate state into pipelines and draw. */
#pragma once
#include <stdint.h>

typedef struct {
    float x, y;          /* Glide window coordinates (pixels, top-left origin) */
    float d;             /* depth value 0..1 (already mapped from z or w buffering) */
    float oow;           /* 1/w: perspective-correct texturing, fog, w */
    float s, t;          /* texture coordinates normalised to 0..1 over the texture */
    float fog;           /* fog coordinate for GR_FOG_WITH_TABLE_ON_FOGCOORD_EXT */
    uint32_t rgba;       /* iterated colour, R in the low byte */
} GpuVertex;             /* 32 bytes */

/* Fragment-shader constants. Layout is shared by HLSL and MSL: keep 16-byte groups. */
typedef struct {
    int32_t cc[4];       /* colour combine: function, factor, local, other */
    int32_t ac[4];       /* alpha combine:  function, factor, local, other */
    int32_t tc[4];       /* texture combine: rgb function, rgb factor, alpha function, alpha factor */
    int32_t flags[4];    /* cc invert, ac invert, tc rgb invert, tc alpha invert */
    int32_t mode[4];     /* texture on, alpha test function, chroma key on, fog mode */
    float aref[4];       /* alpha reference (0..1) */
    float chroma[4];
    float fogc[4];
    float constc[4];
    float fogtab[64];
} GpuShade;

typedef struct {
    GpuShade shade;
    int32_t blend[4];    /* Glide rgb src, rgb dst, alpha src, alpha dst factors */
    int32_t depth_func;  /* GR_CMP_*, or -1 for depth test off */
    int32_t depth_write;
    int32_t scissor[4];  /* x0, y0, x1, y1 in Glide pixels */
    int32_t tex;         /* backend texture id, or -1 */
    int32_t bilinear, clamp_s, clamp_t;
} GpuState;

int gpu_init(int w, int h);                       /* (re)open at Glide resolution; 0 = not available */
int gpu_texture_create(int w, int h);
void gpu_texture_upload(int id, const uint32_t *rgba);    /* full level 0; backend builds mips */
void gpu_texture_destroy(int id);
void gpu_draw(const GpuState *s, const GpuVertex *v, int nverts);
void gpu_swap(void);
