/* Car00/dash00 TGA improvement, inspired by the owner's high-resolution car overlay.
 * The verified 004EF920 loader downsizes to EBX even if the source art is larger.
 * The engine keeps a 256px CPU mirror for damage/reflections; downsample that
 * mirror separately while the GPU receives the full-resolution image.
 * RGB uses separable Catmull-Rom reconstruction; categorical alpha stays nearest.
 * All memory is temporary and the original game files remain untouched.
 */
#include "rt_host.h"
#include "plat.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static uint32_t mirror_addresses[16];
static unsigned nmirrors;

static float cubic(float x) {
    x = fabsf(x);
    if (x <= 1) return (1.5f*x - 2.5f)*x*x + 1;
    if (x < 2) return ((-0.5f*x + 2.5f)*x - 4)*x + 2;
    return 0;
}
static unsigned bounded(int x, unsigned n) { return x < 0 ? 0 : (unsigned)x >= n ? n-1 : (unsigned)x; }
static uint8_t channel(float x) { return x < 0 ? 0 : x > 255 ? 255 : (uint8_t)(x + .5f); }

/* Both paths resample 256px square art. Calculate the identical cubic taps once
 * per output size instead of evaluating the polynomial for every channel/pixel. */
typedef struct { unsigned index[4], nearest; float weight[4]; } ResampleTap;
static ResampleTap taps[2048];
static unsigned tap_side;
static const ResampleTap *resample_taps(unsigned side) {
    if (tap_side != side) {
        for(unsigned x=0;x<side;x++) {
            float u=(x+.5f)*256/side-.5f; int ix=(int)floorf(u);
            for(int k=-1;k<=2;k++) {
                taps[x].index[k+1]=bounded(ix+k,256);
                taps[x].weight[k+1]=cubic(u-ix-k);
            }
            taps[x].nearest=x*256/side;
        }
        tap_side=side;
    }
    return taps;
}
/* Two live damage/cockpit mirrors: 4.25 MiB at the default 1024px. Cache the
 * complete 128 KiB source, so equal bytes are required before skipping work. */
typedef struct { uint32_t address; unsigned side; uint64_t used; uint8_t *source, *pixels; } MirrorCache;
static MirrorCache mirror_cache[2];
static uint64_t mirror_use;


uint32_t hle_car_texture_begin(uint32_t *data, uint32_t *limit, uint32_t reuse, uint32_t stack) {
    if (getenv("NFS4_TRACE_TEXTURES")) {
        static unsigned logged;
        if (logged++ < 16) plat_log("car loader: data %08X limit %u reuse %08X copy %08X", *data, *limit, reuse, RD32(stack+12));
    }
    if (getenv("NFS4_ORIGINAL_TEXTURES") || *limit != 256) return 0;
    uint32_t src = *data;
    if (!src || src > GUEST_SPACE-18 || MEM[src+1] || MEM[src+2] != 2 || MEM[src+16] != 32) return 0;
    unsigned w = RD16(src+12), h = RD16(src+14), header = 18 + MEM[src];
    if (w != h || w < 256 || w > 2048 || (w & (w-1)) || src > GUEST_SPACE-header-w*h*4) return 0;
    unsigned side = w > 256 ? w : 1024;
    if (reuse) {
        /* Validate the live Thrash descriptor, including after texture reset/reallocation. */
        unsigned lod=0;for(unsigned n=side;n>1;n>>=1)lod++;
        if (reuse > GUEST_SPACE-36 || RD32(reuse+4)!=lod || RD32(reuse+8)!=0 || RD32(reuse+12)!=0xB) return 0;
    }
    if (w > 256) { *limit = side; return 0; }
    uint32_t scratch = guest_alloc(header + side*side*4);
    if (!scratch) return 0;
    float *rows = malloc((size_t)side*h*3*sizeof(float));
    if (!rows) { guest_free(scratch); return 0; }
    memcpy(MEM+scratch, MEM+src, header);
    WR16(scratch+12, side); WR16(scratch+14, side);
    const uint8_t *in = MEM+src+header;
    uint8_t *out = MEM+scratch+header;
    const ResampleTap *axis=resample_taps(side);
    for (unsigned y=0; y<h; y++) for (unsigned x=0; x<side; x++) {
        const ResampleTap *t=&axis[x];
        for (unsigned c=0;c<3;c++) {
            float v=0;
            for(int k=0;k<4;k++) v+=in[(y*w+t->index[k])*4+c]*t->weight[k];
            rows[(y*side+x)*3+c]=v;
        }
    }
    for (unsigned y=0; y<side; y++) {
        const ResampleTap *t=&axis[y];
        for (unsigned x=0; x<side; x++) {
            for(unsigned c=0;c<3;c++) {
                float value=0;
                for(int k=0;k<4;k++) value+=rows[(t->index[k]*side+x)*3+c]*t->weight[k];
                out[(y*side+x)*4+c]=channel(value);
            }
            out[(y*side+x)*4+3]=in[((y*h/side)*w+x*w/side)*4+3];
        }
    }
    free(rows);
    *data=scratch; *limit=side;
    static unsigned logged;
    if (logged++ < 8) plat_log("car texture: %ux%u -> %ux%u, RGB cubic / alpha nearest",w,h,side,side);
    return scratch;
}
void hle_car_texture_end(uint32_t scratch, uint32_t texture, uint32_t side) {
    (void)texture; (void)side;
    if (scratch) guest_free(scratch);
}

/* The original memcpy at 004EFB62 copies ARGB1555 into a fixed 256px mirror.
 * Keep that ABI even when the independently allocated GPU texture is larger. */
void hle_car_texture_copy(uint32_t src, uint32_t dst, uint32_t bytes, uint32_t side) {
    if (side <= 256 || bytes != side*side*2) { memcpy(MEM+dst,MEM+src,bytes); return; }
    unsigned i;
    for (i=0;i<nmirrors;i++) if(mirror_addresses[i]==dst) break;
    if(i==nmirrors) {
        if(nmirrors==16) plat_fatal("car mirror registry full");
        mirror_addresses[nmirrors++]=dst;
    }
    for (unsigned y=0;y<256;y++) for(unsigned x=0;x<256;x++) {
        unsigned sx=(2*x+1)*side/512, sy=(2*y+1)*side/512;
        WR16(dst+(y*256+x)*2,RD16(src+(sy*side+sx)*2));
    }
}

/* Dynamic damage/reflection uploads still point at the engine's fixed CPU mirror.
 * Reconstruct that mirror before a large Glide upload so it cannot overread it. */
const uint8_t *hle_car_texture_upload(uint32_t data, int small, int large, int aspect, int fmt) {
    if (fmt != 0xB || aspect || large != small || large < 9 || large > 11) return 0;
    unsigned i;
    for(i=0;i<nmirrors;i++) if(mirror_addresses[i]==data) break;
    if(i==nmirrors) return 0;
    unsigned side=1u<<large;
    MirrorCache *cache=NULL;
    for(unsigned j=0;j<2;j++) if(mirror_cache[j].address==data && mirror_cache[j].side==side) { cache=&mirror_cache[j]; break; }
    if(cache && !memcmp(cache->source,MEM+data,256*256*2)) { cache->used=++mirror_use; return cache->pixels; }
    if(!cache) {
        cache=mirror_cache[0].used<=mirror_cache[1].used ? &mirror_cache[0] : &mirror_cache[1];
        uint8_t *pixels=realloc(cache->pixels,side*side*2);
        if(!pixels) plat_fatal("car mirror cache allocation failed");
        cache->pixels=pixels;
        if(!cache->source)cache->source=malloc(256*256*2);
        if(!cache->source)plat_fatal("car mirror source allocation failed");
        cache->address=data;cache->side=side;
    }
    float *rows=malloc((size_t)side*256*3*sizeof(float));
    if (!rows) plat_fatal("car mirror reconstruction allocation failed");
    const ResampleTap *axis=resample_taps(side);
    for(unsigned y=0;y<256;y++) for(unsigned x=0;x<side;x++) {
        const ResampleTap *t=&axis[x];
        for(unsigned c=0;c<3;c++) {
            float v=0;
            for(int k=0;k<4;k++) v+=((RD16(data+(y*256+t->index[k])*2)>>(c*5))&31)*t->weight[k];
            rows[(y*side+x)*3+c]=v;
        }
    }
    for(unsigned y=0;y<side;y++) {
        const ResampleTap *t=&axis[y];
        for(unsigned x=0;x<side;x++) {
            unsigned pixel=RD16(data+((y*256/side)*256+x*256/side)*2)&0x8000;
            for(unsigned c=0;c<3;c++) {
                float value=0;
                for(int k=0;k<4;k++)value+=rows[(t->index[k]*side+x)*3+c]*t->weight[k];
                unsigned q=value<0?0:value>31?31:(unsigned)(value+.5f);
                pixel|=q<<(c*5);
            }
            cache->pixels[(y*side+x)*2]=(uint8_t)pixel;
            cache->pixels[(y*side+x)*2+1]=(uint8_t)(pixel>>8);
        }
    }
    free(rows);
    static unsigned logged;
    if(logged++<4)plat_log("car texture: dynamic 256px mirror -> %upx upload",side);
    memcpy(cache->source,MEM+data,256*256*2);
    cache->used=++mirror_use;
    return cache->pixels;
}
