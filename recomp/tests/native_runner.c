/* 32-bit native runner: executes test instruction bytes on the real CPU.
 * Build: zig cc -target x86-windows-gnu -O1 -Wl,--image-base,0x30000000 native_runner.c */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "difftest.h"

__attribute__((used)) uint32_t in_regs[8], in_fl, out_regs[8], out_fl, host_esp, code_ptr;

/* load test registers + flags, call the code (which ends with pushfd/pop [out_fl]/ret), store registers */
__attribute__((naked, noinline)) static void trampoline(void) {
    __asm__ volatile(
        "pushal\n\t"
        "movl %esp, _host_esp\n\t"
        "movl _in_regs+16, %esp\n\t"          /* test stack */
        "pushl _in_fl\n\t"
        "popfl\n\t"
        "movl _in_regs+0, %eax\n\t"
        "movl _in_regs+4, %ecx\n\t"
        "movl _in_regs+8, %edx\n\t"
        "movl _in_regs+12, %ebx\n\t"
        "movl _in_regs+20, %ebp\n\t"
        "movl _in_regs+24, %esi\n\t"
        "movl _in_regs+28, %edi\n\t"
        "movl $1f, 0x1007F010\n\t"
        "jmp *_code_ptr\n\t"
        "1:\n\t"
        "movl %eax, _out_regs+0\n\t"
        "movl %ecx, _out_regs+4\n\t"
        "movl %edx, _out_regs+8\n\t"
        "movl %ebx, _out_regs+12\n\t"
        "movl %esp, _out_regs+16\n\t"
        "movl %ebp, _out_regs+20\n\t"
        "movl %esi, _out_regs+24\n\t"
        "movl %edi, _out_regs+28\n\t"
        "cld\n\t"
        "movl _host_esp, %esp\n\t"
        "popal\n\t"
        "ret\n\t");
}

__attribute__((used)) volatile uint32_t faulted;

/* resume point after a faulting test instruction */
__attribute__((naked, noinline, used)) static void recover(void) {
    __asm__ volatile(
        "cld\n\t"
        "movl $1, _faulted\n\t"
        "movl _host_esp, %esp\n\t"
        "popal\n\t"
        "ret\n\t");
}

static LONG CALLBACK veh(EXCEPTION_POINTERS *e) {
    CONTEXT *ctx = e->ContextRecord;
    if (ctx->Eip < DT_CODE || ctx->Eip >= DT_CODE + 64) {
        fprintf(stderr, "exception %08lX outside test code at %08lX\n", e->ExceptionRecord->ExceptionCode, ctx->Eip);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    ctx->Eip = (DWORD)(uintptr_t)recover;
    ctx->EFlags &= ~0x500u;    /* clear TF and DF */
    return EXCEPTION_CONTINUE_EXECUTION;
}

int main(int argc, char **argv) {
    AddVectoredExceptionHandler(1, veh);
    uint8_t *mem = VirtualAlloc((void *)DT_REGION, DT_REGION_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (mem != (uint8_t *)DT_REGION) { fprintf(stderr, "cannot map test region at %08X (got %p)\n", DT_REGION, (void *)mem); return 1; }
    FILE *f = fopen(argv[1], "rb"), *o = fopen(argv[2], "wb");
    uint32_t n;
    fread(&n, 4, 1, f);
    static DtTest t;
    static uint8_t pattern[DT_DATA_SIZE];
    for (uint32_t k = 0; k < n; k++) {
        fread(&t, sizeof t, 1, f);
        if (getenv("DT_TRACE")) { fprintf(stderr, "%u ", k); fflush(stderr); }
        dt_pattern(pattern, t.seed);
        memcpy(mem, pattern, DT_DATA_SIZE);
        memcpy((uint8_t *)DT_CODE, t.code, sizeof t.code);
        memcpy(in_regs, t.regs, 32);
        in_fl = t.eflags | 0x202;
        code_ptr = DT_CODE;
        FlushInstructionCache(GetCurrentProcess(), (void *)DT_CODE, sizeof t.code);
        faulted = 0;
        trampoline();
        DtResult r = { 0 };
        if (faulted) { r.nchanges = 0xFFFFFFFFu; fwrite(&r, sizeof r, 1, o); continue; }
        memcpy(r.regs, out_regs, 32);
        r.eflags = *(uint32_t *)DT_FLAGS_OUT;
        r.regs[4] = *(uint32_t *)(DT_FLAGS_OUT + 4);
        for (uint32_t a = 0; a < DT_DATA_SIZE && r.nchanges < DT_MAX_CHANGES; a++)
            if (mem[a] != pattern[a] && !(DT_REGION + a >= DT_FLAGS_OUT && DT_REGION + a < DT_FLAGS_OUT + 20)) {
                r.change_addr[r.nchanges] = DT_REGION + a;
                r.change_val[r.nchanges] = mem[a];
                r.nchanges++;
            }
        fwrite(&r, sizeof r, 1, o);
    }
    fclose(o);
    return 0;
}
