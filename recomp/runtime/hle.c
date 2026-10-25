/* HLE registry, handle table and guest/host path mapping. */
#include "hle.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

static const HleDll g_dlls[] = {
    { "kernel32.dll", hle_kernel32 },
    { "kernel32.dll", hle_kernel32_crt },
    { "user32.dll", hle_user32 },
    { "gdi32.dll", hle_misc_gdi32 },
    { "advapi32.dll", hle_misc_advapi32 },
    { "winmm.dll", hle_misc_winmm },
    { "ole32.dll", hle_misc_ole32 },
    { "shell32.dll", hle_misc_shell32 },
    { "wsock32.dll", hle_misc_wsock32 },
    { "dinput.dll", hle_dinput },
    { "glide3x.dll", hle_glide },
    { "dsound.dll", hle_dsound },
    { "ddraw.dll", hle_ddraw },
};

static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

void hle_unimplemented(Cpu *c) {
    const char *name = c->hle_name;
    if (!name) {
        /* direct `call [iat]`: recover the slot from the call instruction */
        uint32_t ret = RD32(c->esp), slot = RD32(ret - 4);
        name = "?";
        for (int i = 0; i < rt_module_count; i++)
            for (uint32_t k = 0; k < rt_modules[i].nimports; k++)
                if (rt_modules[i].imports[k].slot == slot) name = rt_modules[i].imports[k].name;
    }
    plat_log("UNIMPLEMENTED API %s called from %08X", name, RD32(c->esp));
    rt_trap(c, RD32(c->esp), "unimplemented API");
}

HleFn rt_resolve_import(const char *dll, const char *name) {
    for (size_t i = 0; i < sizeof g_dlls / sizeof g_dlls[0]; i++) {
        if (!ieq(g_dlls[i].dll, dll)) continue;
        for (const HleEntry *e = g_dlls[i].entries; e->name; e++)
            if (!strcmp(e->name, name)) return e->fn;
    }
    return hle_unimplemented;
}

uint32_t rt_import_address(const char *dll, const char *name, HleFn fn) {
    return rt_hle_address(dll, name, fn);
}

/* ---------------------------------------------------------------- handles */

#define MAX_HANDLES 1024
static HObj g_handles[MAX_HANDLES];
static PlatMutex *g_hlock;

uint32_t handle_new(int type) {
    plat_mutex_lock(g_hlock);
    for (int i = 1; i < MAX_HANDLES; i++) {
        if (g_handles[i].type == H_FREE) {
            memset(&g_handles[i], 0, sizeof(HObj));
            g_handles[i].type = type;
            plat_mutex_unlock(g_hlock);
            return 0x1000 + i * 4;
        }
    }
    plat_mutex_unlock(g_hlock);
    plat_fatal("out of handles");
    return 0;
}

HObj *handle_get(uint32_t h, int type) {
    if (h < 0x1000 || ((h - 0x1000) & 3)) return NULL;
    uint32_t i = (h - 0x1000) / 4;
    if (i >= MAX_HANDLES || g_handles[i].type == H_FREE) return NULL;
    if (type && g_handles[i].type != type) return NULL;
    return &g_handles[i];
}

void handle_free(uint32_t h) {
    HObj *o = handle_get(h, 0);
    if (o) {
        free(o->path);
        o->type = H_FREE;
    }
}

void set_last_error(Cpu *c, uint32_t e) { WR32(c->fs_base + 0x34, e); }

/* ---------------------------------------------------------------- paths */

/* The game believes it lives in C:\NFS4. Reads come from the original game
 * folder; anything the game writes goes to the save folder so the original
 * installation is never modified. */
static char g_cwd[260] = "C:\\NFS4";
const char *guest_cwd(void) { return g_cwd; }
void set_guest_cwd(const char *p) { strncpy(g_cwd, p, sizeof g_cwd - 1); }

static void normalise(const char *guest, char *rel, size_t n) {
    char full[1024];
    if (guest[0] && guest[1] == ':') snprintf(full, sizeof full, "%s", guest);
    else if (guest[0] == '\\' || guest[0] == '/') snprintf(full, sizeof full, "C:%s", guest);
    else snprintf(full, sizeof full, "%s\\%s", g_cwd, guest);
    for (char *p = full; *p; p++) if (*p == '/') *p = '\\';
    /* resolve . and .. components */
    char *parts[128]; int np = 0;
    for (char *p = full + 2; *p && np < 128; ) {
        while (*p == '\\') p++;
        if (!*p) break;
        char *t = p;
        while (*p && *p != '\\') p++;
        if (*p) *p++ = 0;
        if (!strcmp(t, ".")) continue;
        if (!strcmp(t, "..")) { if (np) np--; continue; }
        parts[np++] = t;
    }
    rel[0] = 0;
    int start = (np > 0 && ieq(parts[0], "NFS4")) ? 1 : 0;
    for (int i = start; i < np; i++) {
        if (rel[0]) strncat(rel, "/", n - strlen(rel) - 1);
        strncat(rel, parts[i], n - strlen(rel) - 1);
    }
}

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

#ifndef _WIN32
/* Windows paths are case-insensitive, iOS/macOS file systems may not be: repair each
 * component of `path` (below `root`) by scanning its directory. */
typedef struct { const char *want; char found[256]; } CaseCtx;
static void case_cb(const char *name, int is_dir, uint32_t size, void *p) {
    (void)is_dir; (void)size;
    CaseCtx *c = p;
    if (!c->found[0] && ieq(name, c->want)) snprintf(c->found, sizeof c->found, "%s", name);
}
void host_fix_case(char *path, size_t n, const char *root) {
    if (exists(path)) return;
    size_t rl = strlen(root);
    if (strncmp(path, root, rl) != 0) return;
    char out[1200];
    snprintf(out, sizeof out, "%s", root);
    const char *p = path + rl;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *e = strchr(p, '/');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char comp[256];
        snprintf(comp, sizeof comp, "%.*s", (int)len, p);
        CaseCtx c = { comp, { 0 } };
        plat_list_dir(out, case_cb, &c);
        size_t ol = strlen(out);
        snprintf(out + ol, sizeof out - ol, "/%s", c.found[0] ? c.found : comp);
        p += len;
    }
    snprintf(path, n, "%s", out);
}
#else
void host_fix_case(char *path, size_t n, const char *root) { (void)path; (void)n; (void)root; }
#endif

void map_path_in(const char *root, const char *guest, char *host, size_t n) {
    char rel[1024];
    normalise(guest, rel, sizeof rel);
    snprintf(host, n, "%s/%s", root, rel);
    host_fix_case(host, n, root);
}

void map_path(const char *guest, char *host, size_t n, int for_write) {
    char rel[1024];
    normalise(guest, rel, sizeof rel);
    char save[1200];
    snprintf(save, sizeof save, "%s/%s", plat_save_dir(), rel);
    host_fix_case(save, sizeof save, plat_save_dir());
    if (for_write || exists(save)) {
        if (for_write) {   /* create parent folders in the save tree */
            char tmp[1200];
            snprintf(tmp, sizeof tmp, "%s", save);
            for (char *p = tmp + strlen(plat_save_dir()) + 1; *p; p++) {
                if (*p == '/') { *p = 0; plat_mkdir(tmp); *p = '/'; }
            }
        }
        snprintf(host, n, "%s", save);
        return;
    }
    const char *mods = plat_mods_dir();
    if (mods) {
        char m[1200];
        snprintf(m, sizeof m, "%s/%s", mods, rel);
        host_fix_case(m, sizeof m, mods);
        if (exists(m)) { snprintf(host, n, "%s", m); return; }
    }
    snprintf(host, n, "%s/%s", plat_game_dir(), rel);
    host_fix_case(host, n, plat_game_dir());
}

void hle_kernel32_init(void);
void hle_user32_init(void);
void hle_misc_init(void);
void hle_init(void) {
    g_hlock = plat_mutex_new();
    hle_kernel32_init();
    hle_user32_init();
    hle_misc_init();
    plat_mkdir(plat_save_dir());
}
