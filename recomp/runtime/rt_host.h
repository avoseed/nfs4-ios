/* Host-side runtime API shared by the loader and the HLE (Win32 replacement) layer. */
#pragma once
#include "rt.h"

/* Guest address space layout */
#define GUEST_SPACE       0x40000000u
#define GUEST_HEAP_START  0x02000000u     /* HeapAlloc: 0x02000000-0x1FFFFFFF, VirtualAlloc above */
#define GUEST_HEAP_END    0x3FE00000u
#define RET_MAGIC         0x3FEFFFF0u     /* return address of host->guest calls */
#define HLE_BASE          0x3FF00000u     /* magic addresses of HLE functions */

typedef struct RtModule {
    const char *file;                     /* relative to the game folder */
    uint32_t base, entry, timestamp;
    const GuestFn *funcs; uint32_t nfuncs;
    const HleImport *imports; uint32_t nimports;
    HleFn *imp;
    const HleExport *exports;
    int is_dll;
    int loaded;
} RtModule;

extern RtModule rt_modules[];
extern int rt_module_count;

void rt_register_modules(void);
int rt_boot(void);
int rt_load_module(RtModule *m);
RtModule *rt_module_by_name(const char *file);
GuestFnPtr rt_find_function(uint32_t addr);
uint32_t rt_hle_address(const char *dll, const char *name, HleFn fn);
const char *rt_hle_name(uint32_t addr);
void rt_dump(Cpu *c);
void rt_backtrace(Cpu *c);
void rt_dump_threads(void);
int hle_glide_in_menu(void);     /* hle_glide.c: 1 on a 640x480 front-end screen */

Cpu *rt_current_cpu(void);
Cpu *rt_new_cpu(uint32_t stack_size);
void rt_bind_cpu(Cpu *c);
Cpu *rt_spawn_guest_thread(uint32_t fn, uint32_t arg, uint32_t stack_size);
uint32_t rt_guest_call(Cpu *c, uint32_t fn, int nargs, const uint32_t *args);

uint32_t guest_alloc(uint32_t size);
uint32_t guest_realloc(uint32_t p, uint32_t size);
uint32_t guest_size(uint32_t p);
void guest_free(uint32_t p);
uint32_t guest_strdup(const char *s);
uint32_t guest_valloc(uint32_t size);      /* 64 KB aligned, zeroed */
void guest_vfree(uint32_t base);
uint32_t guest_vregion(uint32_t addr);     /* base of the VirtualAlloc region holding addr, or 0 */
uint32_t guest_vsize(uint32_t base);

/* HLE layer */
void hle_init(void);
HleFn rt_resolve_import(const char *dll, const char *name);
uint32_t rt_import_address(const char *dll, const char *name, HleFn fn);
void hle_unimplemented(Cpu *c);
void hle_glide_patch_module(const char *file, uint32_t base);   /* widescreen table patch */
void hle_glide_size(int *w, int *h);
void hle_glide_fix_config(uint8_t *buf, uint32_t file_off, uint32_t n);
void host_fix_case(char *path, size_t n, const char *root);   /* case-insensitive lookup on POSIX hosts */

/* stdcall helpers for HLE functions: ARG(0) is the first argument */
#define ARG(n) RD32(c->esp + 4 + 4 * (n))
#define STR(p) ((const char *)(MEM + (p)))
#define RETURN(nargs, v) do { c->eax = (uint32_t)(v); c->ret_to = RD32(c->esp); c->esp += 4 + 4 * (nargs); return; } while (0)
#define RETURN_CDECL(v) do { c->eax = (uint32_t)(v); c->ret_to = RD32(c->esp); c->esp += 4; return; } while (0)
