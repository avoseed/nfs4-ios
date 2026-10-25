/* Guest memory, module loading, indirect-call dispatch, threads, x87 helpers. */
#include "rt.h"
#include "rt_host.h"
#include "plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>

uint8_t *MEM;

/* ---------------------------------------------------------------- function lookup */

static GuestFn *g_funcs;
static uint32_t g_nfuncs;

static int cmp_fn(const void *a, const void *b) {
    uint32_t x = ((const GuestFn *)a)->addr, y = ((const GuestFn *)b)->addr;
    return x < y ? -1 : x > y;
}

static void index_functions(void) {
    uint32_t n = 0;
    for (int i = 0; i < rt_module_count; i++) n += rt_modules[i].nfuncs;
    g_funcs = malloc(sizeof(GuestFn) * n);
    g_nfuncs = 0;
    for (int i = 0; i < rt_module_count; i++) {
        memcpy(g_funcs + g_nfuncs, rt_modules[i].funcs, sizeof(GuestFn) * rt_modules[i].nfuncs);
        g_nfuncs += rt_modules[i].nfuncs;
    }
    qsort(g_funcs, g_nfuncs, sizeof(GuestFn), cmp_fn);
}

GuestFnPtr rt_find_function(uint32_t addr) {
    uint32_t lo = 0, hi = g_nfuncs;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (g_funcs[mid].addr < addr) lo = mid + 1;
        else hi = mid;
    }
    if (lo < g_nfuncs && g_funcs[lo].addr == addr) return g_funcs[lo].fn;
    return NULL;
}

/* ---------------------------------------------------------------- HLE magic addresses */

#define HLE_MAX 4096
static HleFn g_hle_fn[HLE_MAX];
static const char *g_hle_name[HLE_MAX];
static uint32_t g_hle_count;

uint32_t rt_hle_address(const char *dll, const char *name, HleFn fn) {
    char *full = malloc(strlen(dll) + strlen(name) + 2);
    sprintf(full, "%s!%s", dll, name);
    for (uint32_t i = 0; i < g_hle_count; i++)
        if (g_hle_fn[i] == fn && !strcmp(g_hle_name[i], full)) { free(full); return HLE_BASE + i * 4; }
    if (g_hle_count >= HLE_MAX) plat_fatal("too many HLE entries");
    g_hle_fn[g_hle_count] = fn;
    g_hle_name[g_hle_count] = full;
    return HLE_BASE + 4 * g_hle_count++;
}

const char *rt_hle_name(uint32_t addr) {
    if (addr >= HLE_BASE && addr < HLE_BASE + 4 * g_hle_count) return g_hle_name[(addr - HLE_BASE) / 4];
    return NULL;
}

void rt_call_indirect(Cpu *c, uint32_t target) {
    if (target >= HLE_BASE && target < HLE_BASE + 4 * HLE_MAX) {
        uint32_t i = (target - HLE_BASE) / 4;
        if (i < g_hle_count) {
            c->hle_name = g_hle_name[i];
            g_hle_fn[i](c);
            return;
        }
    }
    GuestFnPtr f = rt_find_function(target);
    if (f) {
        if (target == 0x01002230u && getenv("NFS4_WIDE")) {
            plat_log("THRASH_setvideomode(index=%u, buffers=%u, depth=%u) from %08X",RD32(c->esp+4),RD32(c->esp+8),RD32(c->esp+12),RD32(c->esp));
            for (uint32_t i=1;i<=6;i++) { uint32_t p=0x010140B8u+40*i;plat_log("driver mode %u: %ux%u enabled=%u",i,RD32(p),RD32(p+4),RD32(p+16)); }
        }
        f(c); return;
    }
    char missing_path[1200];
    snprintf(missing_path,sizeof missing_path,"%s/missing_seeds.txt",plat_save_dir());
    FILE *s = fopen(missing_path, "a");
    if (s) { fprintf(s, "%08X\n", target); fclose(s); }
    rt_trap(c, target, "indirect call/jump to unknown code (added to missing_seeds.txt)");
}

/* ---------------------------------------------------------------- diagnostics */

void rt_dump(Cpu *c) {
    plat_log("  eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X",
             c->eax, c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi);
    plat_log("  stack: %08X %08X %08X %08X %08X %08X %08X %08X", RD32(c->esp), RD32(c->esp + 4), RD32(c->esp + 8),
             RD32(c->esp + 12), RD32(c->esp + 16), RD32(c->esp + 20), RD32(c->esp + 24), RD32(c->esp + 28));
}

/* function-entry trace: enabled around interesting API calls (NFS4_TRACE=1), one line per call */
volatile int rt_tracing;
static FILE *g_trace;
static uint32_t g_trace_count;

void rt_trace_enter(Cpu *c, uint32_t addr) {
    if (!g_trace) {
        char p[1200];
        snprintf(p, sizeof p, "%s/trace.txt", plat_save_dir());
        g_trace = fopen(p, "w");
    }
    if (g_trace && g_trace_count++ < 2000000) {
        fprintf(g_trace, "%u %08X eax=%08X edx=%08X ebx=%08X ecx=%08X ret=%08X\n", c->thread_id, addr, c->eax, c->edx,
                c->ebx, c->ecx, RD32(c->esp));
        static uint32_t dump_at = 1;
        if (dump_at == 1) { const char *e = getenv("NFS4_TRACE_DUMP"); dump_at = e ? (uint32_t)strtoul(e, NULL, 16) : 0; }
        if (addr == dump_at && c->eax < GUEST_SPACE - 64) {
            fprintf(g_trace, "    [eax]:");
            for (int i = 0; i < 48; i++) fprintf(g_trace, " %02X", MEM[c->eax + i]);
            fprintf(g_trace, "\n");
        }
    }
}

#ifdef RT_WATCH
uint32_t rt_watch_lo, rt_watch_len;
__thread uint32_t rt_cur_fn;
void rt_watch_hit(uint32_t a, uint32_t n, uint64_t v) {
    Cpu *c = rt_current_cpu();
    plat_log("WATCH write %08X len %u value %llX in function %08X (thread %u)", a, n, (unsigned long long)v, rt_cur_fn,
             c ? c->thread_id : 0);
}
#endif

void rt_trace_api(Cpu *c, const char *name) {
    if (g_trace && g_trace_count++ < 2000000)
        fprintf(g_trace, "%u API %s args %08X %08X %08X %08X ret=%08X\n", c->thread_id, name, RD32(c->esp + 4),
                RD32(c->esp + 8), RD32(c->esp + 12), RD32(c->esp + 16), RD32(c->esp));
}

void rt_trace_note(const char *what) {
    if (g_trace) fprintf(g_trace, "--- %s\n", what);
}

/* heuristic backtrace: stack words that are return addresses (preceded by a call) */
void rt_backtrace(Cpu *c) {
    char line[1024];
    size_t n = 0;
    int found = 0;
    for (uint32_t i = 0; i < 2048 && found < 24; i++) {
        uint32_t v = RD32(c->esp + i * 4);
        for (int m = 0; m < rt_module_count; m++) {
            RtModule *mod = &rt_modules[m];
            if (!mod->loaded || v < mod->base + 0x1000 || v >= mod->base + 0x200000) continue;
            int is_call = RD8(v - 5) == 0xE8 || (RD8(v - 6) == 0xFF && (RD8(v - 5) & 0x38) == 0x10) ||
                          (RD8(v - 2) == 0xFF && (RD8(v - 1) & 0x38) == 0x10) || (RD8(v - 3) == 0xFF && (RD8(v - 2) & 0x38) == 0x10);
            if (is_call) {
                n += snprintf(line + n, sizeof line - n, " %08X", v);
                found++;
            }
            break;
        }
    }
    plat_log("  backtrace:%s", line);
}

void rt_trap(Cpu *c, uint32_t addr, const char *what) {
    plat_log("TRAP at %08X (thread %u): %s", addr, c->thread_id, what);
    rt_dump(c);
    rt_backtrace(c);
    plat_fatal("guest trap at %08X: %s", addr, what);
}

void rt_bad_return(Cpu *c, uint32_t expected) {
    plat_log("unexpected return: expected %08X got %08X", expected, c->ret_to);
    rt_trap(c, c->ret_to, "return to unexpected address");
}

void rt_cpuid(Cpu *c, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    (void)c;
    uint32_t leaf = *eax;
    if (leaf == 0) {           /* "GenuineIntel", max leaf 1 */
        *eax = 1; *ebx = 0x756E6547; *edx = 0x49656E69; *ecx = 0x6C65746E;
    } else {                   /* Pentium Pro class: FPU, TSC, CMOV; no MMX */
        *eax = 0x0612; *ebx = 0; *ecx = 0; *edx = 0x00008011;
    }
}

uint64_t rt_rdtsc(void) { return plat_ticks_us() * 300; }   /* pretend 300 MHz */

/* ---------------------------------------------------------------- x87 */

void fpu_init(Cpu *c) {
    c->top = 0; c->fcw = 0x037F; c->fsw = 0;
    for (int i = 0; i < 8; i++) { c->st[i] = 0; c->iv[i] = 0; }
}

double f80_to_double(const uint8_t *p) {
    uint64_t mant; uint16_t se;
    memcpy(&mant, p, 8); memcpy(&se, p + 8, 2);
    int sign = se >> 15, exp = se & 0x7FFF;
    if (exp == 0 && mant == 0) return sign ? -0.0 : 0.0;
    if (exp == 0x7FFF) return (mant << 1) ? NAN : (sign ? -INFINITY : INFINITY);
    double v = ldexp((double)mant, exp - 16383 - 63);
    return sign ? -v : v;
}

void double_to_f80(double v, uint8_t *p) {
    uint64_t mant = 0; uint16_t se = 0;
    if (v != 0) {
        if (signbit(v)) { se = 0x8000; v = -v; }
        if (isinf(v)) { se |= 0x7FFF; mant = 0x8000000000000000ull; }
        else if (isnan(v)) { se |= 0x7FFF; mant = 0xC000000000000000ull; }
        else {
            int e; double m = frexp(v, &e);              /* v = m * 2^e, 0.5 <= m < 1 */
            mant = (uint64_t)ldexp(m, 64);
            se |= (uint16_t)(e - 1 + 16383);
        }
    } else if (signbit(v)) se = 0x8000;
    memcpy(p, &mant, 8); memcpy(p + 8, &se, 2);
}

void fpu_save(Cpu *c, uint32_t a) {
    WR32(a, c->fcw); WR32(a + 4, FPU_SW()); WR32(a + 8, 0xFFFF);
    for (int i = 0; i < 16; i += 4) WR32(a + 12 + i, 0);
    for (int i = 0; i < 8; i++) double_to_f80(ST(i), MEM + a + 28 + i * 10);
    fpu_init(c);
}

void fpu_restore(Cpu *c, uint32_t a) {
    c->fcw = RD16(a);
    uint16_t sw = RD16(a + 4);
    c->top = (sw >> 11) & 7;
    c->fsw = sw & ~0x3800;
    for (int i = 0; i < 8; i++) ST(i) = f80_to_double(MEM + a + 28 + i * 10);
}

/* ---------------------------------------------------------------- guest heap */

static PlatMutex *g_heap_lock;
static uint32_t g_brk = GUEST_HEAP_START;
#define NCLASS 24
static uint32_t g_free[NCLASS];            /* singly linked lists through guest memory */

static int size_class(uint32_t n) {
    int k = 4;                              /* 16 bytes minimum */
    while ((1u << k) < n && k < 31) k++;
    return k;
}

uint32_t guest_alloc(uint32_t size) {
    if (size == 0) size = 1;
    uint32_t need = size + 16;
    int k = size_class(need);
    plat_mutex_lock(g_heap_lock);
    uint32_t blk = 0;
    if (k < NCLASS && g_free[k]) {
        blk = g_free[k];
        g_free[k] = RD32(blk);
    } else {
        uint32_t bytes = 1u << k;
        uint32_t align = bytes >= 4096 ? 4096 : 16;
        g_brk = (g_brk + align - 1) & ~(align - 1);
        if (g_brk + bytes > 0x20000000u) { plat_mutex_unlock(g_heap_lock); plat_fatal("guest heap exhausted (%u bytes)", size); }
        blk = g_brk;
        g_brk += bytes;
    }
    plat_mutex_unlock(g_heap_lock);
    WR32(blk, (uint32_t)k);
    WR32(blk + 4, size);
    WR32(blk + 8, 0x48454150);              /* "HEAP" */
    memset(MEM + blk + 16, 0, size);
    return blk + 16;
}

uint32_t guest_size(uint32_t p) {
    if (!p || RD32(p - 8) != 0x48454150) return 0;
    return RD32(p - 12);
}

void guest_free(uint32_t p) {
    if (!p) return;
    uint32_t blk = p - 16;
    if (RD32(blk + 8) != 0x48454150) { plat_log("guest_free: bad pointer %08X", p); return; }
    int k = (int)RD32(blk);
    WR32(blk + 8, 0);
    if (k >= NCLASS) return;                /* huge blocks are not recycled */
    plat_mutex_lock(g_heap_lock);
    WR32(blk, g_free[k]);
    g_free[k] = blk;
    plat_mutex_unlock(g_heap_lock);
}

uint32_t guest_realloc(uint32_t p, uint32_t size) {
    if (!p) return guest_alloc(size);
    uint32_t old = guest_size(p);
    if (size <= old && size + 16 > (1u << (RD32(p - 16) - 1))) { WR32(p - 12, size); return p; }
    uint32_t q = guest_alloc(size);
    memcpy(MEM + q, MEM + p, old < size ? old : size);
    guest_free(p);
    return q;
}

uint32_t guest_strdup(const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1;
    uint32_t p = guest_alloc(n);
    memcpy(MEM + p, s, n);
    return p;
}

/* ---------------------------------------------------------------- VirtualAlloc regions (64 KB granular) */

#define VREGION_START 0x20000000u
#define MAX_VREGIONS 4096
static struct { uint32_t base, size; int used; } g_vr[MAX_VREGIONS];
static int g_nvr;
static uint32_t g_vbrk = VREGION_START;

uint32_t guest_valloc(uint32_t size) {
    size = (size + 0xFFFF) & ~0xFFFFu;
    plat_mutex_lock(g_heap_lock);
    for (int i = 0; i < g_nvr; i++) {
        if (!g_vr[i].used && g_vr[i].size >= size) {
            g_vr[i].used = 1;
            uint32_t b = g_vr[i].base, s = g_vr[i].size;
            plat_mutex_unlock(g_heap_lock);
            memset(MEM + b, 0, s);
            return b;
        }
    }
    if (g_nvr >= MAX_VREGIONS || g_vbrk + size > GUEST_HEAP_END) {
        plat_mutex_unlock(g_heap_lock);
        plat_log("VirtualAlloc: out of guest space (%u bytes)", size);
        return 0;
    }
    uint32_t b = g_vbrk;
    g_vbrk += size;
    g_vr[g_nvr].base = b; g_vr[g_nvr].size = size; g_vr[g_nvr].used = 1; g_nvr++;
    plat_mutex_unlock(g_heap_lock);
    return b;
}

void guest_vfree(uint32_t base) {
    plat_mutex_lock(g_heap_lock);
    for (int i = 0; i < g_nvr; i++) if (g_vr[i].base == base) g_vr[i].used = 0;
    plat_mutex_unlock(g_heap_lock);
}

uint32_t guest_vregion(uint32_t a) {
    for (int i = 0; i < g_nvr; i++)
        if (g_vr[i].used && a >= g_vr[i].base && a < g_vr[i].base + g_vr[i].size) return g_vr[i].base;
    return 0;
}

uint32_t guest_vsize(uint32_t base) {
    for (int i = 0; i < g_nvr; i++) if (g_vr[i].base == base) return g_vr[i].size;
    return 0;
}

/* ---------------------------------------------------------------- calling guest code from the host */

uint32_t rt_guest_call(Cpu *c, uint32_t fn, int nargs, const uint32_t *args) {
    uint32_t save_esp = c->esp;
    for (int i = nargs - 1; i >= 0; i--) { c->esp -= 4; WR32(c->esp, args[i]); }
    c->esp -= 4; WR32(c->esp, RET_MAGIC);
    rt_call_indirect(c, fn);
    if (c->ret_to != RET_MAGIC) rt_bad_return(c, RET_MAGIC);
    c->esp = save_esp;     /* works for both stdcall and cdecl callees */
    return c->eax;
}

/* ---------------------------------------------------------------- threads */

static __thread Cpu *t_cpu;
static uint32_t g_next_tid = 1;

Cpu *rt_current_cpu(void) { return t_cpu; }

static Cpu *g_all_cpus[256];
static int g_ncpus;

/* diagnostic: where is every guest thread right now? */
void rt_dump_threads(void) {
    for (int i = 0; i < g_ncpus; i++) {
        Cpu *c = g_all_cpus[i];
        plat_log("thread %u: last API %s, esp=%08X", c->thread_id, c->hle_name ? c->hle_name : "-", c->esp);
        rt_backtrace(c);
    }
}

static void dump_after(void *p) {
    plat_sleep_ms((uint32_t)(atof((const char *)p) * 1000));
    rt_dump_threads();
}

Cpu *rt_new_cpu(uint32_t stack_size) {
    Cpu *c = calloc(1, sizeof(Cpu));
    if (g_ncpus < 256) g_all_cpus[g_ncpus++] = c;
    fpu_init(c);
    uint32_t stack = guest_alloc(stack_size);
    c->esp = (stack + stack_size - 64) & ~15u;
    uint32_t teb = guest_alloc(0x1000);
    WR32(teb + 0x00, 0xFFFFFFFF);           /* SEH chain end */
    WR32(teb + 0x04, stack + stack_size);   /* stack base */
    WR32(teb + 0x08, stack);                /* stack limit */
    WR32(teb + 0x18, teb);                  /* self */
    c->fs_base = teb;
    c->thread_id = __atomic_fetch_add(&g_next_tid, 1, __ATOMIC_SEQ_CST);
    WR32(teb + 0x20, 1);
    WR32(teb + 0x24, c->thread_id);
    return c;
}

void rt_bind_cpu(Cpu *c) { t_cpu = c; }

typedef struct { Cpu *c; uint32_t fn; uint32_t arg; } ThreadStart;

static void thread_main(void *p) {
    ThreadStart *ts = p;
    rt_bind_cpu(ts->c);
    uint32_t arg = ts->arg;
    rt_guest_call(ts->c, ts->fn, 1, &arg);
    free(ts);
}

Cpu *rt_spawn_guest_thread(uint32_t fn, uint32_t arg, uint32_t stack_size) {
    Cpu *c = rt_new_cpu(stack_size ? stack_size : 0x100000);
    ThreadStart *ts = malloc(sizeof *ts);
    ts->c = c; ts->fn = fn; ts->arg = arg;
    plat_thread_start(thread_main, ts);
    return c;
}

/* ---------------------------------------------------------------- PE loading */

static uint32_t rd32f(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16f(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static int load_pe(RtModule *m) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", plat_game_dir(), m->file);
    host_fix_case(path, sizeof path, plat_game_dir());
    FILE *f = fopen(path, "rb");
    if (!f) { plat_log("cannot open %s", path); return 0; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(n);
    if (fread(d, 1, n, f) != (size_t)n) { fclose(f); free(d); return 0; }
    fclose(f);
    uint32_t pe = rd32f(d + 0x3C);
    if (n < 0x200 || rd32f(d + pe) != 0x4550) { free(d); plat_log("%s: not a PE file", path); return 0; }
    uint32_t stamp = rd32f(d + pe + 8);
    if (stamp != m->timestamp) {
        plat_log("%s: wrong version (timestamp %08X, recompiled from %08X)", path, stamp, m->timestamp);
        free(d); return 0;
    }
    uint16_t nsec = rd16f(d + pe + 6), opt = rd16f(d + pe + 20);
    uint32_t file_base = rd32f(d + pe + 24 + 28), hdr_size = rd32f(d + pe + 24 + 60);
    memcpy(MEM + m->base, d, hdr_size);
    const uint8_t *sec = d + pe + 24 + opt;
    for (int i = 0; i < nsec; i++, sec += 40) {
        uint32_t va = rd32f(sec + 12), raw = rd32f(sec + 16), ptr = rd32f(sec + 20), flags = rd32f(sec + 36);
        /* Watcom marks .bss as uninitialised with PointerToRawData = 0: it must stay zero-filled */
        if ((flags & 0x80) || !ptr) continue;
        if (raw && ptr + raw <= (uint32_t)n) memcpy(MEM + m->base + va, d + ptr, raw);
    }
    /* rebase (DLLs are recompiled at a fixed low base) */
    int32_t delta = (int32_t)(m->base - file_base);
    uint32_t rel_rva = rd32f(d + pe + 24 + 96 + 5 * 8), rel_size = rd32f(d + pe + 24 + 96 + 5 * 8 + 4);
    if (delta && rel_rva) {
        uint32_t p = m->base + rel_rva, end = p + rel_size;
        while (p < end) {
            uint32_t page = RD32(p), size = RD32(p + 4);
            if (!size) break;
            for (uint32_t k = 8; k < size; k += 2) {
                uint16_t e = RD16(p + k);
                if ((e >> 12) == 3) {
                    uint32_t a = m->base + page + (e & 0xFFF);
                    WR32(a, RD32(a) + (uint32_t)delta);
                }
            }
            p += size;
        }
    }
    free(d);
    /* imports: resolve by name, and put magic addresses in the IAT */
    for (uint32_t i = 0; i < m->nimports; i++) {
        const HleImport *im = &m->imports[i];
        HleFn fn = rt_resolve_import(im->dll, im->name);
        m->imp[im->id] = fn;
        WR32(im->slot, rt_import_address(im->dll, im->name, fn));
    }
    plat_log("loaded %s at %08X (%u functions, %u imports, reloc delta %08X)", m->file, m->base, m->nfuncs, m->nimports,
             (uint32_t)delta);
    if (getenv("NFS4_DUMP_MODULES")) {
        char dp[1200];
        snprintf(dp, sizeof dp, "%s/%08X.bin", plat_save_dir(), m->base);
        FILE *df = fopen(dp, "wb");
        if (df) { fwrite(MEM + m->base, 1, 0x20000, df); fclose(df); }
    }
    m->loaded = 1;
    hle_glide_patch_module(m->file, m->base);
    return 1;
}

RtModule *rt_module_by_name(const char *file) {
    const char *b = file;
    for (const char *p = file; *p; p++) if (*p == '\\' || *p == '/') b = p + 1;
    for (int i = 0; i < rt_module_count; i++) {
        const char *x = rt_modules[i].file, *xb = x;
        for (const char *p = x; *p; p++) if (*p == '\\' || *p == '/') xb = p + 1;
        int eq = 1;
        for (const char *p = b, *q = xb; ; p++, q++) {
            if (tolower((unsigned char)*p) != tolower((unsigned char)*q)) { eq = 0; break; }
            if (!*p) break;
        }
        if (eq) return &rt_modules[i];
    }
    return NULL;
}

int rt_load_module(RtModule *m) {
    if (m->loaded) return 1;
    return load_pe(m);
}

/* ---------------------------------------------------------------- boot */

static void trace_window(void *p) {
    double after = atof((const char *)p);
    plat_sleep_ms((uint32_t)(after * 1000));
    plat_log("trace window open");
    rt_tracing = 1;
    plat_sleep_ms(500);
    rt_tracing = 0;
    if (g_trace) fflush(g_trace);
    plat_log("trace window closed (%u entries)", g_trace_count);
}

int rt_boot(void) {
    if (getenv("NFS4_TRACE_AFTER")) plat_thread_start(trace_window, (void *)getenv("NFS4_TRACE_AFTER"));
    if (getenv("NFS4_DUMP_AFTER")) plat_thread_start(dump_after, (void *)getenv("NFS4_DUMP_AFTER"));
#ifdef RT_WATCH
    if (getenv("NFS4_WATCH")) {
        rt_watch_lo = (uint32_t)strtoul(getenv("NFS4_WATCH"), NULL, 16);
        const char *l = strchr(getenv("NFS4_WATCH"), ':');
        rt_watch_len = l ? (uint32_t)strtoul(l + 1, NULL, 16) : 4;
        plat_log("watching %08X+%X", rt_watch_lo, rt_watch_len);
    }
#endif
    MEM = plat_reserve(GUEST_SPACE);
    if (!MEM) plat_fatal("cannot reserve guest address space");
    g_heap_lock = plat_mutex_new();
    rt_register_modules();
    hle_init();
    index_functions();
    RtModule *exe = &rt_modules[0];
    if (!rt_load_module(exe)) return 0;
    Cpu *c = rt_new_cpu(0x400000);
    rt_bind_cpu(c);
    plat_log("starting %s at %08X", exe->file, exe->entry);
    c->esp -= 4; WR32(c->esp, RET_MAGIC);
    GuestFnPtr f = rt_find_function(exe->entry);
    if (!f) plat_fatal("entry point not recompiled");
    f(c);
    plat_log("guest returned from entry point, eax=%08X", c->eax);
    return 1;
}
