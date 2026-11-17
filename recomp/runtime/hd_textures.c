/* Redrawn track textures (the redraw pack, tools/hd_texture_pack.py): when the game hands Glide a texture whose
 * pixels are known, the GPU gets the larger redrawn picture instead. Texture coordinates reach the GPU divided by
 * the Glide size (hle_glide.c gpu_vertex), so a texture of the same shape at any size lines up by itself.
 *
 * The key is the picture itself, not where the game keeps it: FNV-1a over the size and every texel cut to 4 bits a
 * channel plus 1-bit alpha. The game turns its 8-bit palette art into 565, 1555 or 4444 by dropping low bits, so
 * the same QFS picture gives the same key in every format, and the pack tool can work the key out from the QFS
 * without running the game.
 *
 * Pack: hdtex/<key, 16 hex>.rgba = "HDT1", width, height (u32 little-endian), then RGBA bytes, or the same picture
 * compressed, hdtex/<key>.astc = "HDT2", width, height, levels, 0x0404, then ASTC 4x4 blocks for every mip level
 * (tools/hd_astc.py: a quarter of the memory, read only where the GPU samples ASTC, i.e. on the iPhone), in the mods folder
 * or beside nfs4.exe (where "PC에서 받기" puts a PC overlay's files); the mods one wins. No folder, no change. NFS4_ORIGINAL_TEXTURES=1 turns it off; NFS4_DUMP_TEXTURES=<folder> (test host) writes
 * every texture the game uses there in the same format under its key, for checking the keys against the QFS. */
#include "plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t hd_texture_key(const uint32_t *px, int w, int h) {
    uint64_t k = 1469598103934665603ull;
#define MIX(b) (k = (k ^ (uint8_t)(b)) * 1099511628211ull)
    MIX(w); MIX(w >> 8); MIX(h); MIX(h >> 8);
    for (int i = 0; i < w * h; i++) {
        uint32_t p = px[i];
        MIX(((p >> 4) & 0x0F) | ((p >> 8) & 0xF0));        /* R, G high nibbles */
        MIX(((p >> 20) & 0x0F) | ((p >> 31) << 4));       /* B high nibble, alpha bit */
    }
#undef MIX
    return k;
}

int gpu_astc_supported(void);

typedef struct { uint64_t key; int dir, astc; } HdKey;
static HdKey *g_keys;
static int g_nkeys, g_loaded, g_astc;
static char g_dirs[2][1100];

static void add_key(const char *name, int is_dir, uint32_t size, void *ctx) {
    int dir = *(const int *)ctx;
    if (is_dir || size < 12 || strlen(name) != 21) return;
    int astc = !strcmp(name + 16, ".astc");
    if (!astc && strcmp(name + 16, ".rgba")) return;
    if (astc && !g_astc) return;
    char *end;
    uint64_t k = strtoull(name, &end, 16);
    if (end != name + 16) return;
    HdKey *grown = realloc(g_keys, (size_t)(g_nkeys + 1) * sizeof *g_keys);
    if (!grown) return;
    g_keys = grown;
    g_keys[g_nkeys].key = k; g_keys[g_nkeys].dir = dir; g_keys[g_nkeys].astc = astc; g_nkeys++;
}
static int cmp_key(const void *a, const void *b) {     /* by key, the first folder first, then ASTC first */
    const HdKey *x = a, *y = b;
    return x->key != y->key ? (x->key < y->key ? -1 : 1) : x->dir != y->dir ? x->dir - y->dir : y->astc - x->astc;
}
static int cmp_find(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = ((const HdKey *)b)->key;
    return x < y ? -1 : x > y;
}

static void load_index(void) {
    g_loaded = 1;
    if (getenv("NFS4_ORIGINAL_TEXTURES")) return;
    g_astc = gpu_astc_supported();
    const char *roots[2] = { plat_mods_dir(), plat_game_dir() };
    for (int i = 0; i < 2; i++) {
        if (!roots[i] || !roots[i][0]) continue;
        snprintf(g_dirs[i], sizeof g_dirs[i], "%s/hdtex", roots[i]);
        int before = g_nkeys;
        plat_list_dir(g_dirs[i], add_key, &i);
        if (g_nkeys > before) plat_log("hd textures: %d redrawn textures in %s", g_nkeys - before, g_dirs[i]);
    }
    if (g_nkeys) qsort(g_keys, (size_t)g_nkeys, sizeof *g_keys, cmp_key);
}

int hle_glide_in_menu(void);

static void dump(uint64_t key, const uint32_t *px, int w, int h) {
    const char *dir = getenv("NFS4_DUMP_TEXTURES");
    /* races only (the menus stream thousands of movie tiles), unless NFS4_DUMP_MENUS is set too */
    if (!dir || (hle_glide_in_menu() && !getenv("NFS4_DUMP_MENUS"))) return;
    char path[1200];
    snprintf(path, sizeof path, "%s/%016llx.rgba", dir, (unsigned long long)key);
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return; }
    f = fopen(path, "wb");
    if (!f) return;
    uint32_t head[3] = { 0x31544448u /* "HDT1" */, (uint32_t)w, (uint32_t)h };
    fwrite(head, sizeof head, 1, f);
    fwrite(px, 4, (size_t)w * h, f);
    fclose(f);
}

/* the redrawn picture for these pixels, *ow x *oh, same shape, malloc'd: RGBA with *levels = 0, or ASTC 4x4 blocks
 * of *levels mip levels (largest first); NULL if there is none */
void *hd_texture_find(const uint32_t *px, int w, int h, int *ow, int *oh, int *levels) {
    *levels = 0;
    if (!g_loaded) load_index();
    if (!g_nkeys && !getenv("NFS4_DUMP_TEXTURES")) return NULL;
    uint64_t key = hd_texture_key(px, w, h);
    dump(key, px, w, h);
    const HdKey *hit = g_nkeys ? bsearch(&key, g_keys, (size_t)g_nkeys, sizeof *g_keys, cmp_find) : NULL;
    if (!hit) return NULL;
    while (hit > g_keys && hit[-1].key == key) hit--;     /* the mods folder's copy, if both have one */
    char path[1200];
    snprintf(path, sizeof path, "%s/%016llx.%s", g_dirs[hit->dir], (unsigned long long)key, hit->astc ? "astc" : "rgba");
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint32_t head[5];
    void *out = NULL;
    int nlev = 0;
    if (fread(head, 12, 1, f) == 1 && head[0] == (hit->astc ? 0x32544448u /* "HDT2" */ : 0x31544448u /* "HDT1" */) &&
        head[1] >= (uint32_t)w && head[2] >= (uint32_t)h && head[1] <= 4096 && head[2] <= 4096 &&
        (uint64_t)head[1] * (uint32_t)h == (uint64_t)head[2] * (uint32_t)w) {
        size_t bytes = (size_t)head[1] * head[2] * 4;
        if (hit->astc) {
            bytes = 0;
            if (fread(head + 3, 8, 1, f) == 1 && head[4] == 0x0404 && head[3] >= 1 && head[3] <= 13) {
                uint32_t lw = head[1], lh = head[2];
                nlev = (int)head[3];
                for (int l = 0; l < nlev; l++) { bytes += (size_t)((lw + 3) / 4) * ((lh + 3) / 4) * 16; if (lw > 1) lw >>= 1; if (lh > 1) lh >>= 1; }
            }
        }
        out = bytes ? malloc(bytes) : NULL;
        if (out && fread(out, 1, bytes, f) != bytes) { free(out); out = NULL; }
    }
    fclose(f);
    if (!out) { plat_log("hd textures: %s is not a usable %dx%d redraw", path, w, h); return NULL; }
    *levels = nlev;
    *ow = (int)head[1]; *oh = (int)head[2];
    static int logged;
    if (logged++ < 4 || getenv("NFS4_TRACE_HD")) plat_log("hd textures: %dx%d -> %dx%d%s (%016llx)", w, h, *ow, *oh, nlev ? " ASTC" : "", (unsigned long long)key);
    return out;
}
