/* Runs the recompiled versions of the differential tests (64-bit host). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"
#include "difftest.h"
#include <setjmp.h>
static jmp_buf g_jb;

uint8_t *MEM;
extern void (*const dt_funcs[])(Cpu *);
extern const uint32_t dt_count;

void rt_call_indirect(Cpu *c, uint32_t t) { longjmp(g_jb, 1); }
void rt_bad_return(Cpu *c, uint32_t e) { fprintf(stderr, "bad return\n"); exit(5); }
void rt_trap(Cpu *c, uint32_t a, const char *w) { longjmp(g_jb, 1); }
void rt_cpuid(Cpu *c, uint32_t *a, uint32_t *b, uint32_t *cc, uint32_t *d) { *a = *b = *cc = *d = 0; }
uint64_t rt_rdtsc(void) { return 0; }
void fpu_init(Cpu *c) { c->top = 0; c->fcw = 0x37F; c->fsw = 0; }
void fpu_save(Cpu *c, uint32_t a) { (void)c; (void)a; }
void fpu_restore(Cpu *c, uint32_t a) { (void)c; (void)a; }
double f80_to_double(const uint8_t *p) { (void)p; return 0; }
void double_to_f80(double v, uint8_t *p) { (void)v; memset(p, 0, 10); }

int main(int argc, char **argv) {
    MEM = calloc(1, 0x10200000);
    FILE *f = fopen(argv[1], "rb"), *o = fopen(argv[2], "wb");
    uint32_t n;
    fread(&n, 4, 1, f);
    static DtTest t;
    static uint8_t pattern[DT_DATA_SIZE];
    for (uint32_t k = 0; k < n; k++) {
        fread(&t, sizeof t, 1, f);
        dt_pattern(pattern, t.seed);
        memcpy(MEM + DT_REGION, pattern, DT_DATA_SIZE);
        Cpu c;
        memset(&c, 0, sizeof c);
        c.eax = t.regs[0]; c.ecx = t.regs[1]; c.edx = t.regs[2]; c.ebx = t.regs[3];
        c.esp = t.regs[4]; c.ebp = t.regs[5]; c.esi = t.regs[6]; c.edi = t.regs[7];
        uint32_t fl = t.eflags;
        c.f_cf = fl & 1; c.f_pf = (fl >> 2) & 1; c.f_af = (fl >> 4) & 1; c.f_zf = (fl >> 6) & 1;
        c.f_sf = (fl >> 7) & 1; c.df = (fl >> 10) & 1; c.f_of = (fl >> 11) & 1;
        fpu_init(&c);
        WR32(DT_FLAGS_OUT + 16, 0xDEADBEEF);   /* the epilogue returns through this slot */
        int trapped = setjmp(g_jb);
        if (!trapped && k < dt_count) dt_funcs[k](&c);
        DtResult r;
        memset(&r, 0, sizeof r);
        r.regs[0] = c.eax; r.regs[1] = c.ecx; r.regs[2] = c.edx; r.regs[3] = c.ebx;
        r.regs[4] = c.esp; r.regs[5] = c.ebp; r.regs[6] = c.esi; r.regs[7] = c.edi;
        if (trapped) { r.nchanges = 0xFFFFFFFEu; fwrite(&r, sizeof r, 1, o); continue; }
        r.eflags = RD32(DT_FLAGS_OUT);
        r.regs[4] = RD32(DT_FLAGS_OUT + 4);
        for (uint32_t a = 0; a < DT_DATA_SIZE && r.nchanges < DT_MAX_CHANGES; a++)
            if (MEM[DT_REGION + a] != pattern[a] && !(DT_REGION + a >= DT_FLAGS_OUT && DT_REGION + a < DT_FLAGS_OUT + 20)) {
                r.change_addr[r.nchanges] = DT_REGION + a;
                r.change_val[r.nchanges] = MEM[DT_REGION + a];
                r.nchanges++;
            }
        fwrite(&r, sizeof r, 1, o);
    }
    fclose(o);
    return 0;
}
