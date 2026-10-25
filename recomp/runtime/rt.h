/* Runtime support for statically recompiled x86 code. */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

typedef struct Cpu {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t ret_to;          /* address popped by the last guest `ret` */
    uint32_t fs_base, gs_base;
    uint32_t df;              /* direction flag */
    uint32_t f_cf, f_pf, f_af, f_zf, f_sf, f_of;   /* flags handed across call/ret (Watcom helpers) */
    uint32_t eflags_hi;       /* AC/ID bits written by popfd (CPU detection) */
    double st[8];             /* x87 stack (double precision) */
    int64_t ist[8];           /* exact 64-bit integer loaded by fild qword (x87 copy idiom) */
    uint8_t iv[8];            /* ist[] valid for this slot */
    int top;
    uint16_t fcw, fsw;
    uint64_t mm[8];
    uint32_t thread_id;
    const char *hle_name;     /* import being called (for logging stubs) */
    void *host;               /* per-thread host data */
} Cpu;

typedef void (*GuestFnPtr)(Cpu *c);
typedef void (*HleFn)(Cpu *c);
typedef struct { uint32_t addr; GuestFnPtr fn; } GuestFn;
typedef struct { uint32_t slot; uint32_t id; const char *dll; const char *name; } HleImport;
typedef struct { uint32_t addr; const char *name; } HleExport;

extern uint8_t *MEM;          /* host address of guest address 0 */

/* ---------------------------------------------------------------- memory */
#ifdef RT_WATCH          /* debug build: report writes into a watched range (NFS4_WATCH=addr:len) */
extern uint32_t rt_watch_lo, rt_watch_len;
void rt_watch_hit(uint32_t a, uint32_t n, uint64_t v);
#define WATCH(a, n, v) do { if ((uint32_t)((a) - rt_watch_lo) < rt_watch_len) rt_watch_hit((a), (n), (v)); } while (0)
#else
#define WATCH(a, n, v) ((void)0)
#endif
static inline uint8_t  RD8(uint32_t a)  { return MEM[a]; }
static inline uint16_t RD16(uint32_t a) { uint16_t v; memcpy(&v, MEM + a, 2); return v; }
static inline uint32_t RD32(uint32_t a) { uint32_t v; memcpy(&v, MEM + a, 4); return v; }
static inline uint64_t RD64(uint32_t a) { uint64_t v; memcpy(&v, MEM + a, 8); return v; }
static inline void WR8(uint32_t a, uint32_t v)  { WATCH(a, 1, v); MEM[a] = (uint8_t)v; }
static inline void WR16(uint32_t a, uint32_t v) { WATCH(a, 2, v); uint16_t t = (uint16_t)v; memcpy(MEM + a, &t, 2); }
static inline void WR32(uint32_t a, uint32_t v) { WATCH(a, 4, v); memcpy(MEM + a, &v, 4); }
static inline void WR64(uint32_t a, uint64_t v) { WATCH(a, 8, v); memcpy(MEM + a, &v, 8); }
static inline double RDF32(uint32_t a) { float f; memcpy(&f, MEM + a, 4); return (double)f; }
static inline double RDF64(uint32_t a) { double d; memcpy(&d, MEM + a, 8); return d; }
static inline void WRF32(uint32_t a, double v) { float f = (float)v; memcpy(MEM + a, &f, 4); }
static inline void WRF64(uint32_t a, double v) { memcpy(MEM + a, &v, 8); }
double f80_to_double(const uint8_t *p);
void double_to_f80(double v, uint8_t *p);
static inline double RDF80(uint32_t a) { return f80_to_double(MEM + a); }
static inline void WRF80(uint32_t a, double v) { double_to_f80(v, MEM + a); }

/* ---------------------------------------------------------------- registers / flags */
#define REGS_DECL uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi
#ifdef RT_FLAGS_FROM_CPU   /* differential tests start each function with given flags */
#define FLAGS_DECL uint32_t cf = c->f_cf, pf = c->f_pf, af = c->f_af, zf = c->f_zf, sf = c->f_sf, of = c->f_of; \
    (void)cf; (void)pf; (void)af; (void)zf; (void)sf; (void)of
#else
#define FLAGS_DECL uint32_t cf = 0, pf = 0, af = 0, zf = 0, sf = 0, of = 0; \
    (void)cf; (void)pf; (void)af; (void)zf; (void)sf; (void)of
#endif
#define SYNC_IN  do { eax = c->eax; ecx = c->ecx; edx = c->edx; ebx = c->ebx; \
                      esp = c->esp; ebp = c->ebp; esi = c->esi; edi = c->edi; } while (0)
#define SYNC_OUT do { c->eax = eax; c->ecx = ecx; c->edx = edx; c->ebx = ebx; \
                      c->esp = esp; c->ebp = ebp; c->esi = esi; c->edi = edi; } while (0)

#define PARITY(x) ((uint32_t)!__builtin_parity((uint32_t)(x) & 0xFFu))
#define BSWAP32(x) __builtin_bswap32(x)
#define CTZ32(x) ((uint32_t)__builtin_ctz(x))
#define CLZ32(x) ((uint32_t)__builtin_clz(x))

#define MAKE_EFLAGS() (cf | 2u | (pf << 2) | (af << 4) | (zf << 6) | (sf << 7) | 0x200u | \
                       (c->df << 10) | (of << 11) | c->eflags_hi)
#define SPLIT_EFLAGS(t) do { cf = (t) & 1; pf = ((t) >> 2) & 1; af = ((t) >> 4) & 1; zf = ((t) >> 6) & 1; \
                             sf = ((t) >> 7) & 1; c->df = ((t) >> 10) & 1; of = ((t) >> 11) & 1; \
                             c->eflags_hi = (t) & 0x00240000u; } while (0)

#define DEF_RC(W, M) \
static inline uint32_t RCL##W(uint32_t a, uint32_t n, uint32_t *cf, uint32_t *of) { \
    n %= (W + 1); uint32_t r = a, c0 = *cf; \
    while (n--) { uint32_t hi = (r >> (W - 1)) & 1; r = ((r << 1) | c0) & M; c0 = hi; } \
    *cf = c0; *of = ((r >> (W - 1)) & 1) ^ c0; return r; } \
static inline uint32_t RCR##W(uint32_t a, uint32_t n, uint32_t *cf, uint32_t *of) { \
    n %= (W + 1); uint32_t r = a, c0 = *cf; \
    *of = ((r >> (W - 1)) & 1) ^ c0; \
    while (n--) { uint32_t lo = r & 1; r = ((r >> 1) | (c0 << (W - 1))) & M; c0 = lo; } \
    *cf = c0; return r; }
DEF_RC(8, 0xFFu)
DEF_RC(16, 0xFFFFu)
DEF_RC(32, 0xFFFFFFFFu)

/* ---------------------------------------------------------------- string ops (df = 0 fast paths) */
static inline void rt_movsd(uint32_t *esi, uint32_t *edi, uint32_t n) {
    uint32_t s = *esi, d = *edi;
#ifdef RT_WATCH
    if (rt_watch_len && d < rt_watch_lo + rt_watch_len && d + n * 4 > rt_watch_lo) rt_watch_hit(d, n * 4, RD32(s));
#endif
    if (d <= s || d >= s + n * 4) memmove(MEM + d, MEM + s, (size_t)n * 4);
    else for (uint32_t k = 0; k < n; k++) WR32(d + k * 4, RD32(s + k * 4));
    *esi = s + n * 4; *edi = d + n * 4;
}
static inline void rt_movsb(uint32_t *esi, uint32_t *edi, uint32_t n) {
    uint32_t s = *esi, d = *edi;
#ifdef RT_WATCH
    if (rt_watch_len && d < rt_watch_lo + rt_watch_len && d + n > rt_watch_lo) rt_watch_hit(d, n, MEM[s]);
#endif
    if (d <= s || d >= s + n) memmove(MEM + d, MEM + s, n);
    else for (uint32_t k = 0; k < n; k++) MEM[d + k] = MEM[s + k];
    *esi = s + n; *edi = d + n;
}
static inline void rt_stosd(uint32_t *edi, uint32_t v, uint32_t n) {
    uint32_t d = *edi;
    for (uint32_t k = 0; k < n; k++) WR32(d + k * 4, v);
    *edi = d + n * 4;
}

/* ---------------------------------------------------------------- x87 */
#define ST(i) (c->st[(c->top + (i)) & 7])
#define STSLOT(i) ((c->top + (i)) & 7)
#define FPUSH(v) do { double _v = (v); c->top = (c->top - 1) & 7; c->st[c->top] = _v; c->iv[c->top] = 0; } while (0)
#define FPOP() (c->top = (c->top + 1) & 7)
/* Watcom's memcpy moves 8 bytes at a time with fild/fistp qword; 64-bit integers must survive
 * exactly even though the stack is double precision, so the integer is shadowed per slot. */
#define FILD64(a) do { int64_t _q = (int64_t)RD64(a); FPUSH((double)_q); c->ist[c->top] = _q; c->iv[c->top] = 1; } while (0)
#define FCOPYTAG(dst, src) do { c->iv[STSLOT(dst)] = c->iv[STSLOT(src)]; c->ist[STSLOT(dst)] = c->ist[STSLOT(src)]; } while (0)
static inline void FXCH(Cpu *c, int k) {
    int a = c->top & 7, b = (c->top + k) & 7;
    double t = c->st[a]; c->st[a] = c->st[b]; c->st[b] = t;
    int64_t q = c->ist[a]; c->ist[a] = c->ist[b]; c->ist[b] = q;
    uint8_t v = c->iv[a]; c->iv[a] = c->iv[b]; c->iv[b] = v;
}
#define FPU_SW() ((uint32_t)(uint16_t)((c->fsw & ~0x3800u) | ((uint32_t)(c->top & 7) << 11)))
#define M_PI_X87 3.14159265358979323846
#define M_LOG2E_X87 1.44269504088896340736
#define M_LN2_X87 0.693147180559945309417
#define M_LOG2T_X87 3.32192809488736234787
#define M_LOG10_2_X87 0.301029995663981195214

static inline void fpu_compare(Cpu *c, double a, double b) {
    c->fsw &= ~0x4500u;
    if (a != a || b != b) c->fsw |= 0x4500u;
    else if (a < b) c->fsw |= 0x0100u;
    else if (a == b) c->fsw |= 0x4000u;
}
static inline double fpu_round(Cpu *c, double x) {
    switch ((c->fcw >> 10) & 3) {
    case 0: return nearbyint(x);   /* host default mode is round-to-nearest-even */
    case 1: return floor(x);
    case 2: return ceil(x);
    default: return trunc(x);
    }
}
static inline int32_t fpu_toi32(Cpu *c, double x) {
    double r = fpu_round(c, x);
    if (!(r >= -2147483648.0 && r <= 2147483647.0)) return INT32_MIN;
    return (int32_t)r;
}
static inline int16_t fpu_toi16(Cpu *c, double x) {
    double r = fpu_round(c, x);
    if (!(r >= -32768.0 && r <= 32767.0)) return INT16_MIN;
    return (int16_t)r;
}
static inline int64_t fpu_toi64(Cpu *c, double x) {
    double r = fpu_round(c, x);
    if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0)) return INT64_MIN;
    return (int64_t)r;
}
/* fistp qword of ST(0): exact if the slot still holds an untouched fild qword value */
static inline int64_t fpu_toi64_st0(Cpu *c) {
    int s = c->top & 7;
    if (c->iv[s] && (double)c->ist[s] == c->st[s]) return c->ist[s];
    return fpu_toi64(c, c->st[s]);
}
void fpu_init(Cpu *c);
void fpu_save(Cpu *c, uint32_t addr);
void fpu_restore(Cpu *c, uint32_t addr);

/* Barrier placed at loop headers of recompiled code so polling loops re-read guest memory.
 * x86 hosts only need a compiler barrier; on ARM64 (weaker ordering than the x86 the game was
 * written for) a load barrier also makes other threads' stores visible in order. */
#if defined(__aarch64__)
#define RT_BARRIER() __asm__ volatile("dmb ishld" ::: "memory")
#else
#define RT_BARRIER() __asm__ volatile("" ::: "memory")
#endif

/* ---------------------------------------------------------------- function-entry tracing */
extern volatile int rt_tracing;
void rt_trace_enter(Cpu *c, uint32_t addr);
void rt_trace_api(Cpu *c, const char *name);
#ifdef RT_FLAGS_FROM_CPU
#define RT_ENTER(a) ((void)0)
#define RT_API(n) ((void)0)
#else
#ifdef RT_WATCH
extern __thread uint32_t rt_cur_fn;
#define RT_ENTER(a) do { rt_cur_fn = (a); if (__builtin_expect(rt_tracing, 0)) rt_trace_enter(c, (a)); } while (0)
#else
#define RT_ENTER(a) do { if (__builtin_expect(rt_tracing, 0)) rt_trace_enter(c, (a)); } while (0)
#endif
#define RT_API(n) do { c->hle_name = (n); if (__builtin_expect(rt_tracing, 0)) rt_trace_api(c, (n)); } while (0)
#endif

/* ---------------------------------------------------------------- runtime services */
void rt_call_indirect(Cpu *c, uint32_t target);
void rt_bad_return(Cpu *c, uint32_t expected);
void rt_trap(Cpu *c, uint32_t addr, const char *what);
void rt_cpuid(Cpu *c, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx);
uint64_t rt_rdtsc(void);

uint64_t rt_mmx_binary(unsigned op, uint64_t a, uint64_t b);

void hle_frontend_observed(int menu);



uint32_t hle_glide_set_mode(uint32_t native_mode, uint32_t caller);

void hle_glide_prepare_modes(void);
void hle_glide_select_race_entry(void);
int hle_glide_wide_mode(uint32_t width, uint32_t height);
