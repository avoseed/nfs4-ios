/* KERNEL32 replacement. */
#include "hle.h"
void rt_trace_note(const char *what);
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <setjmp.h>
#include <sys/stat.h>

#define INVALID_HANDLE 0xFFFFFFFFu
#define STD_IN  0x10u
#define STD_OUT 0x14u
#define STD_ERR 0x18u
#define PSEUDO_PROCESS 0xFFFFFFFFu
#define PSEUDO_THREAD  0xFFFFFFFEu
#define PROCESS_HEAP   0x00150000u
#define FAKE_SYSDLL    0x3FD00000u     /* module handles for HLE-only DLLs */

static uint64_t g_start_us;

static void put_str(uint32_t dst, uint32_t size, const char *s) {
    if (!size) return;
    size_t n = strlen(s);
    if (n >= size) n = size - 1;
    memcpy(MEM + dst, s, n);
    MEM[dst + n] = 0;
}

static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

static const char *basename_of(const char *p) {
    const char *b = p;
    for (; *p; p++) if (*p == '\\' || *p == '/') b = p + 1;
    return b;
}

/* ---------------------------------------------------------------- process / version */

HLE(GetVersion) { RETURN(0, 0x0A280105); }          /* Windows XP, build 2600 */
HLE(GetCommandLineA) {
    static uint32_t s;
    if (!s) s = guest_strdup("C:\\NFS4\\nfs4.exe");
    RETURN(0, s);
}
HLE(GetCommandLineW) {
    static uint32_t s;
    if (!s) {
        const char *a = "C:\\NFS4\\nfs4.exe";
        s = guest_alloc(2 * (uint32_t)strlen(a) + 2);
        for (int i = 0; a[i]; i++) WR16(s + 2 * i, (uint8_t)a[i]);
    }
    RETURN(0, s);
}
HLE(GetEnvironmentStrings) {
    static uint32_t s;
    if (!s) {
        static const char env[] = "PATH=C:\\WINDOWS\0WINDIR=C:\\WINDOWS\0\0";
        s = guest_alloc(sizeof env);
        memcpy(MEM + s, env, sizeof env);
    }
    RETURN(0, s);
}
HLE(FreeEnvironmentStringsA) { RETURN(1, 1); }
HLE(SetEnvironmentVariableA) { RETURN(2, 1); }
HLE(SetEnvironmentVariableW) { RETURN(2, 1); }

static void module_path(uint32_t h, char *out, size_t n) {
    for (int i = 0; i < rt_module_count; i++) {
        if ((h == 0 && i == 0) || rt_modules[i].base == h) {
            snprintf(out, n, "C:\\NFS4\\%s", rt_modules[i].file);
            for (char *p = out; *p; p++) if (*p == '/') *p = '\\';
            return;
        }
    }
    snprintf(out, n, "C:\\WINDOWS\\SYSTEM\\unknown.dll");
}

HLE(GetModuleFileNameA) {
    char p[300];
    module_path(ARG(0), p, sizeof p);
    put_str(ARG(1), ARG(2), p);
    RETURN(3, strlen(p) < ARG(2) ? strlen(p) : ARG(2));
}
HLE(GetModuleFileNameW) {
    char p[300];
    module_path(ARG(0), p, sizeof p);
    uint32_t n = (uint32_t)strlen(p), cap = ARG(2);
    if (n >= cap) n = cap ? cap - 1 : 0;
    for (uint32_t i = 0; i < n; i++) WR16(ARG(1) + 2 * i, (uint8_t)p[i]);
    if (cap) WR16(ARG(1) + 2 * n, 0);
    RETURN(3, n);
}

static const char *g_sysdlls[] = { "kernel32.dll", "user32.dll", "gdi32.dll", "advapi32.dll", "winmm.dll",
                                   "ole32.dll", "shell32.dll", "wsock32.dll", "dinput.dll", "ddraw.dll",
                                   "dsound.dll", "glide3x.dll", "ntdll.dll", NULL };

static uint32_t sysdll_handle(const char *name) {
    const char *b = basename_of(name);
    char lower[64];
    size_t k = 0;
    for (; b[k] && k < sizeof lower - 5; k++) lower[k] = (char)tolower((unsigned char)b[k]);
    lower[k] = 0;
    if (!strchr(lower, '.')) strcat(lower, ".dll");
    for (int i = 0; g_sysdlls[i]; i++)
        if (!strcmp(g_sysdlls[i], lower)) return FAKE_SYSDLL + 0x1000 * (uint32_t)i;
    return 0;
}

HLE(GetModuleHandleA) {
    uint32_t p = ARG(0);
    if (!p) RETURN(1, rt_modules[0].base);
    RtModule *m = rt_module_by_name(STR(p));
    if (m && m->loaded) RETURN(1, m->base);
    RETURN(1, sysdll_handle(STR(p)));
}

static uint32_t load_library(Cpu *c, const char *name) {
    RtModule *m = rt_module_by_name(name);
    if (m) {
        int fresh = !m->loaded;
        if (!rt_load_module(m)) return 0;
        if (fresh && m->entry != m->base) {
            uint32_t args[3] = { m->base, 1, 0 };          /* DLL_PROCESS_ATTACH */
            rt_guest_call(c, m->entry, 3, args);
        }
        return m->base;
    }
    uint32_t h = sysdll_handle(name);
    if (!h) plat_log("LoadLibrary(%s): not available", name);
    return h;
}

HLE(LoadLibraryA) {
    const char *name = STR(ARG(0));
    plat_log("LoadLibraryA(%s)", name);
    uint32_t h = load_library(c, name);
    if (!h) set_last_error(c, 126);
    RETURN(1, h);
}
HLE(FreeLibrary) { RETURN(1, 1); }

HLE(GetProcAddress) {
    uint32_t h = ARG(0), np = ARG(1);
    char ord[16];
    const char *name = np < 0x10000 ? (snprintf(ord, sizeof ord, "#%u", np), ord) : STR(np);
    for (int i = 0; i < rt_module_count; i++) {
        RtModule *m = &rt_modules[i];
        if (m->base != h || !m->loaded) continue;
        for (const HleExport *e = m->exports; e && e->name; e++)
            if (!strcmp(e->name, name)) RETURN(2, e->addr);
        plat_log("GetProcAddress: %s has no export %s", m->file, name);
        RETURN(2, 0);
    }
    if (h >= FAKE_SYSDLL && h < FAKE_SYSDLL + 0x20000) {
        const char *dll = g_sysdlls[(h - FAKE_SYSDLL) / 0x1000];
        HleFn fn = rt_resolve_import(dll, name);
        if (fn == hle_unimplemented) {
            plat_log("GetProcAddress(%s, %s): not implemented -> NULL", dll, name);
            RETURN(2, 0);
        }
        RETURN(2, rt_import_address(dll, name, fn));
    }
    plat_log("GetProcAddress(%08X, %s): unknown module", h, name);
    RETURN(2, 0);
}

HLE(ExitProcess) {
    plat_log("ExitProcess(%d)", ARG(0));
    exit((int)ARG(0));
}
HLE(GetCurrentProcess) { RETURN(0, PSEUDO_PROCESS); }
HLE(GetCurrentProcessId) { RETURN(0, 1); }
HLE(GetCurrentThread) { RETURN(0, PSEUDO_THREAD); }
HLE(GetCurrentThreadId) { RETURN(0, c->thread_id); }
HLE(GetLastError) { RETURN(0, RD32(c->fs_base + 0x34)); }
HLE(SetLastError) { set_last_error(c, ARG(0)); RETURN(1, 0); }
HLE(SetErrorMode) { RETURN(1, 0); }
HLE(SetUnhandledExceptionFilter) { RETURN(1, 0); }
HLE(UnhandledExceptionFilter) { RETURN(1, 0); }
HLE(SetConsoleCtrlHandler) { RETURN(2, 1); }
HLE(DebugBreak) { plat_log("DebugBreak from %08X", RD32(c->esp)); RETURN(0, 0); }
HLE(OutputDebugStringA) { plat_log("[debug] %s", STR(ARG(0))); RETURN(1, 0); }
HLE(RtlUnwind) {
    plat_log("RtlUnwind(frame=%08X ip=%08X) from %08X", ARG(0), ARG(1), RD32(c->esp));
    /* unlink SEH frames up to the target frame */
    uint32_t target = ARG(0), fr = RD32(c->fs_base);
    while (fr != 0xFFFFFFFF && fr != target) fr = RD32(fr);
    WR32(c->fs_base, fr);
    RETURN(4, 0);
}

/* ---------------------------------------------------------------- system info / time */

HLE(GetTickCount) { RETURN(0, (uint32_t)((plat_ticks_us() - g_start_us) / 1000)); }
HLE(GetLocalTime) {
    uint16_t t[8];
    plat_local_time(t);
    for (int i = 0; i < 8; i++) WR16(ARG(0) + 2 * i, t[i]);
    RETURN(1, 0);
}
HLE(GetTimeZoneInformation) { memset(MEM + ARG(0), 0, 172); RETURN(1, 0); }
HLE(GetSystemInfo) {
    uint32_t p = ARG(0);
    memset(MEM + p, 0, 36);
    WR32(p + 4, 4096);              /* page size */
    WR32(p + 8, 0x00010000);
    WR32(p + 12, 0x3FEFFFFF);
    WR32(p + 16, 1);                /* active processor mask */
    WR32(p + 20, 1);                /* number of processors */
    WR32(p + 24, 586);              /* processor type */
    WR32(p + 28, 65536);            /* allocation granularity */
    WR16(p + 32, 6); WR16(p + 34, 0x0102);
    RETURN(1, 0);
}
HLE(GlobalMemoryStatus) {
    uint32_t p = ARG(0);
    WR32(p, 32); WR32(p + 4, 30);
    WR32(p + 8, 256u << 20); WR32(p + 12, 192u << 20);
    WR32(p + 16, 512u << 20); WR32(p + 20, 448u << 20);
    WR32(p + 24, 0x7FFE0000); WR32(p + 28, 0x70000000);
    RETURN(1, 0);
}
HLE(GetWindowsDirectoryA) { put_str(ARG(0), ARG(1), "C:\\WINDOWS"); RETURN(2, 10); }
HLE(GetDiskFreeSpaceA) {
    if (ARG(1)) WR32(ARG(1), 8);
    if (ARG(2)) WR32(ARG(2), 512);
    if (ARG(3)) WR32(ARG(3), 0x100000);
    if (ARG(4)) WR32(ARG(4), 0x200000);
    RETURN(5, 1);
}
HLE(GetACP) { RETURN(0, 1252); }
HLE(GetOEMCP) { RETURN(0, 437); }
HLE(GetCPInfo) {
    uint32_t p = ARG(1);
    memset(MEM + p, 0, 20);
    WR32(p, 1);
    MEM[p + 4] = '?';
    RETURN(2, 1);
}
HLE(MultiByteToWideChar) {
    uint32_t src = ARG(2), dst = ARG(4);
    int32_t len = (int32_t)ARG(3), cap = (int32_t)ARG(5);
    if (len < 0) len = (int32_t)strlen(STR(src)) + 1;
    if (cap == 0) RETURN(6, len);
    int32_t n = len < cap ? len : cap;
    for (int32_t i = 0; i < n; i++) WR16(dst + 2 * i, MEM[src + i]);
    RETURN(6, n);
}
HLE(WideCharToMultiByte) {
    uint32_t src = ARG(2), dst = ARG(4);
    int32_t len = (int32_t)ARG(3), cap = (int32_t)ARG(5);
    if (len < 0) { len = 0; while (RD16(src + 2 * len)) len++; len++; }
    if (cap == 0) RETURN(8, len);
    int32_t n = len < cap ? len : cap;
    for (int32_t i = 0; i < n; i++) { uint16_t w = RD16(src + 2 * i); MEM[dst + i] = w < 256 ? (uint8_t)w : '?'; }
    RETURN(8, n);
}

/* ---------------------------------------------------------------- memory */

HLE(VirtualAlloc) {
    uint32_t addr = ARG(0), size = ARG(1), type = ARG(2);
    if (getenv("NFS4_TRACE_MEM")) plat_log("[t%u] VirtualAlloc(%08X, %X, %X) from %08X", c->thread_id, addr, size, type, RD32(c->esp));
    if (addr && guest_vregion(addr)) RETURN(4, addr);   /* commit inside an existing reservation */
    (void)type;
    uint32_t p = guest_valloc(size);
    RETURN(4, p);
}
HLE(VirtualFree) {
    if (getenv("NFS4_TRACE_MEM")) plat_log("[t%u] VirtualFree(%08X, %X, %X)", c->thread_id, ARG(0), ARG(1), ARG(2));
    if (ARG(2) & 0x8000) guest_vfree(ARG(0));
    RETURN(3, 1);
}
HLE(VirtualQuery) {
    uint32_t a = ARG(0), p = ARG(1);
    uint32_t base = guest_vregion(a);
    uint32_t size = base ? guest_vsize(base) : 0x10000;
    if (!base) base = a & ~0xFFFFu;
    WR32(p, a & ~0xFFFu); WR32(p + 4, base); WR32(p + 8, 4);
    WR32(p + 12, base + size - (a & ~0xFFFu)); WR32(p + 16, 0x1000); WR32(p + 20, 4); WR32(p + 24, 0x20000);
    RETURN(3, 28);
}
HLE(GetProcessHeap) { RETURN(0, PROCESS_HEAP); }
HLE(HeapCreate) { RETURN(3, PROCESS_HEAP + 0x10000); }
HLE(HeapDestroy) { RETURN(1, 1); }
HLE(HeapAlloc) { RETURN(3, guest_alloc(ARG(2))); }
HLE(HeapFree) { guest_free(ARG(2)); RETURN(3, 1); }
HLE(HeapReAlloc) { RETURN(4, guest_realloc(ARG(2), ARG(3))); }
HLE(HeapSize) { RETURN(3, guest_size(ARG(2))); }

/* ---------------------------------------------------------------- TLS */

static uint32_t g_tls_next;
HLE(TlsAlloc) {
    uint32_t i = __atomic_fetch_add(&g_tls_next, 1, __ATOMIC_SEQ_CST);
    RETURN(0, i < 64 ? i : 0xFFFFFFFFu);
}
HLE(TlsFree) { RETURN(1, 1); }
HLE(TlsGetValue) { set_last_error(c, 0); RETURN(1, ARG(0) < 64 ? RD32(c->fs_base + 0xE10 + 4 * ARG(0)) : 0); }
HLE(TlsSetValue) { if (ARG(0) < 64) WR32(c->fs_base + 0xE10 + 4 * ARG(0), ARG(1)); RETURN(2, 1); }

/* ---------------------------------------------------------------- critical sections / sync */

static HObj *cs_obj(uint32_t cs) {
    uint32_t h = RD32(cs + 0x10);
    HObj *o = handle_get(h, H_MUTEX);
    if (!o) {
        h = handle_new(H_MUTEX);
        o = handle_get(h, H_MUTEX);
        o->mutex = plat_mutex_new();
        WR32(cs + 0x10, h);
    }
    return o;
}

static void mutex_acquire(HObj *o, uint32_t tid) {
    if (o->owner == tid) { o->count++; return; }
    plat_mutex_lock(o->mutex);
    o->owner = tid;
    o->count = 1;
}
static void mutex_release(HObj *o, uint32_t tid) {
    if (o->owner != tid) return;
    if (--o->count == 0) { o->owner = 0; plat_mutex_unlock(o->mutex); }
}

static PlatMutex *g_cs_init_lock;
HLE(InitializeCriticalSection) {
    memset(MEM + ARG(0), 0, 24);
    cs_obj(ARG(0));
    RETURN(1, 0);
}
HLE(DeleteCriticalSection) { RETURN(1, 0); }
HLE(EnterCriticalSection) {
    plat_mutex_lock(g_cs_init_lock);
    HObj *o = cs_obj(ARG(0));
    plat_mutex_unlock(g_cs_init_lock);
    mutex_acquire(o, c->thread_id);
    RETURN(1, 0);
}
HLE(LeaveCriticalSection) {
    HObj *o = cs_obj(ARG(0));
    mutex_release(o, c->thread_id);
    RETURN(1, 0);
}

/* Waitable kernel objects (events, mutexes, threads) share one lock and one condition
 * variable; every state change broadcasts. This gives exact Win32 semantics including
 * PulseEvent, which a polling implementation would miss. Event fields used in HObj:
 * value = signalled, size = manual reset, count = pulse generation, owner = pending
 * auto-reset pulse. */
static PlatMutex *g_wlock;
static PlatCond *g_wcond;

#define EV_SIGNALLED(o) ((o)->value)
#define EV_MANUAL(o) ((o)->size)

static void kobj_changed(void) { plat_cond_broadcast(g_wcond); }

HLE(CreateEventA) {
    uint32_t h = handle_new(H_EVENT);
    HObj *o = handle_get(h, H_EVENT);
    plat_mutex_lock(g_wlock);
    EV_MANUAL(o) = ARG(1) != 0;
    EV_SIGNALLED(o) = ARG(2) != 0;
    plat_mutex_unlock(g_wlock);
    RETURN(4, h);
}
HLE(SetEvent) {
    HObj *o = handle_get(ARG(0), H_EVENT);
    plat_mutex_lock(g_wlock);
    if (o) { EV_SIGNALLED(o) = 1; kobj_changed(); }
    plat_mutex_unlock(g_wlock);
    RETURN(1, o != NULL);
}
HLE(ResetEvent) {
    HObj *o = handle_get(ARG(0), H_EVENT);
    plat_mutex_lock(g_wlock);
    if (o) EV_SIGNALLED(o) = 0;
    plat_mutex_unlock(g_wlock);
    RETURN(1, o != NULL);
}
HLE(PulseEvent) {
    HObj *o = handle_get(ARG(0), H_EVENT);
    plat_mutex_lock(g_wlock);
    if (o) { o->count++; o->owner = 1; EV_SIGNALLED(o) = 0; kobj_changed(); }
    plat_mutex_unlock(g_wlock);
    RETURN(1, o != NULL);
}
/* Named objects live only in this guest runtime; never touch host named mutexes. */
HLE(OpenMutexA) {
    const char *name = ARG(2) ? STR(ARG(2)) : "";
    for (uint32_t h = 0x1004; h < 0x2000; h += 4) {
        HObj *o = handle_get(h, H_MUTEX);
        if (o && o->path && !strcmp(o->path, name)) { set_last_error(c, 0); RETURN(3, h); }
    }
    plat_log("OpenMutexA(%s) -> not found (guest namespace)", name);
    set_last_error(c, 2); RETURN(3, 0);
}
HLE(CreateMutexA) {
    uint32_t h = handle_new(H_MUTEX);
    HObj *o = handle_get(h, H_MUTEX);
    if (ARG(2)) o->path = strdup(STR(ARG(2)));
    plat_mutex_lock(g_wlock);
    if (ARG(1)) { o->owner = c->thread_id; o->count = 1; }
    plat_mutex_unlock(g_wlock);
    set_last_error(c, 0);
    RETURN(3, h);
}

/* with g_wlock held: can this object satisfy a wait now? gen = pulse generation seen at wait start */
static int kobj_ready(Cpu *c, HObj *o, uint32_t gen) {
    switch (o->type) {
    case H_EVENT: return EV_SIGNALLED(o) || (o->count != gen && (EV_MANUAL(o) || o->owner));
    case H_THREAD: return o->size != 0;          /* finished */
    case H_MUTEX: return o->owner == 0 || o->owner == c->thread_id;
    default: return 1;
    }
}

static void kobj_take(Cpu *c, HObj *o, uint32_t gen) {
    if (o->type == H_EVENT) {
        if (EV_SIGNALLED(o)) { if (!EV_MANUAL(o)) EV_SIGNALLED(o) = 0; }
        else if (o->count != gen && !EV_MANUAL(o)) o->owner = 0;      /* auto-reset pulse wakes one waiter */
    } else if (o->type == H_MUTEX) {
        o->owner = c->thread_id;
        o->count++;
    }
}

static uint32_t wait_objects(Cpu *c, uint32_t n, uint32_t hs, int all, uint32_t ms) {
    HObj *objs[64];
    uint32_t gens[64];
    if (n > 64) n = 64;
    plat_mutex_lock(g_wlock);
    for (uint32_t i = 0; i < n; i++) {
        objs[i] = handle_get(RD32(hs + 4 * i), 0);
        gens[i] = objs[i] && objs[i]->type == H_EVENT ? objs[i]->count : 0;
    }
    uint64_t deadline = plat_ticks_us() + (uint64_t)ms * 1000;
    for (;;) {
        if (all) {
            int ok = 1;
            for (uint32_t i = 0; i < n; i++) if (objs[i] && !kobj_ready(c, objs[i], gens[i])) ok = 0;
            if (ok) {
                for (uint32_t i = 0; i < n; i++) if (objs[i]) kobj_take(c, objs[i], gens[i]);
                plat_mutex_unlock(g_wlock);
                return 0;
            }
        } else {
            for (uint32_t i = 0; i < n; i++) {
                if (!objs[i]) { plat_mutex_unlock(g_wlock); return 0xFFFFFFFFu; }
                if (kobj_ready(c, objs[i], gens[i])) {
                    kobj_take(c, objs[i], gens[i]);
                    plat_mutex_unlock(g_wlock);
                    return i;
                }
            }
        }
        uint32_t left = 0xFFFFFFFFu;
        if (ms != 0xFFFFFFFFu) {
            uint64_t now = plat_ticks_us();
            if (now >= deadline) { plat_mutex_unlock(g_wlock); return 0x102; }
            left = (uint32_t)((deadline - now) / 1000) + 1;
        }
        plat_cond_wait(g_wcond, g_wlock, left);
    }
}

HLE(WaitForSingleObject) {
    if (getenv("NFS4_TRACE_WAITS")) { HObj *o = handle_get(ARG(0), 0); plat_log("[t%u] WaitForSingleObject(%08X type %d, %d ms) from %08X", c->thread_id, ARG(0), o ? o->type : -1, (int)ARG(1), RD32(c->esp)); }
    uint32_t h = c->esp + 4;           /* the handle argument doubles as a one-element array */
    RETURN(2, wait_objects(c, 1, h, 0, ARG(1)));
}
HLE(WaitForMultipleObjectsEx) {
    uint32_t n = ARG(0), hs = ARG(1), all = ARG(2), ms = ARG(3);
    RETURN(5, wait_objects(c, n, hs, all != 0, ms));
}

/* ---------------------------------------------------------------- threads */

typedef struct { jmp_buf exit; uint32_t code; } ThreadHost;

typedef struct {
    Cpu *cpu;
    uint32_t fn, arg, handle;
} PendingThread;

static void guest_thread_main(void *p) {
    PendingThread *pt = p;
    Cpu *c = pt->cpu;
    rt_bind_cpu(c);
    ThreadHost th;
    c->host = &th;
    if (!setjmp(th.exit)) {
        uint32_t arg = pt->arg;
        th.code = rt_guest_call(c, pt->fn, 1, &arg);
    }
    HObj *o = handle_get(pt->handle, H_THREAD);
    plat_mutex_lock(g_wlock);
    if (o) { o->value = th.code; o->size = 1; kobj_changed(); }
    plat_mutex_unlock(g_wlock);
    free(pt);
}

HLE(CreateThread) {
    uint32_t stack = ARG(1), fn = ARG(2), arg = ARG(3), flags = ARG(4), tidp = ARG(5);
    uint32_t h = handle_new(H_THREAD);
    HObj *o = handle_get(h, H_THREAD);
    PendingThread *pt = malloc(sizeof *pt);
    pt->cpu = rt_new_cpu(stack > 0x40000 ? stack : 0x100000);
    pt->fn = fn; pt->arg = arg; pt->handle = h;
    o->thread = pt->cpu;
    if (tidp) WR32(tidp, pt->cpu->thread_id);
    plat_log("CreateThread(fn=%08X arg=%08X%s) -> tid %u", fn, arg, (flags & 4) ? ", suspended" : "", pt->cpu->thread_id);
    if (flags & 4) o->extra = pt;
    else plat_thread_start(guest_thread_main, pt);
    RETURN(6, h);
}
HLE(ResumeThread) {
    HObj *o = handle_get(ARG(0), H_THREAD);
    plat_log("ResumeThread(%08X) %s", ARG(0), o && o->extra ? "starts thread" : "(not suspended)");
    if (o && o->extra) { plat_thread_start(guest_thread_main, o->extra); o->extra = NULL; RETURN(1, 1); }
    RETURN(1, 0);
}
HLE(ExitThread) {
    ThreadHost *th = c->host;
    if (!th) { plat_log("ExitThread on main thread"); exit((int)ARG(0)); }
    th->code = ARG(0);
    longjmp(th->exit, 1);
}
HLE(TerminateThread) { plat_log("TerminateThread(%08X) ignored", ARG(0)); RETURN(2, 1); }
HLE(SetThreadPriority) { RETURN(2, 1); }
HLE(GetThreadPriority) { RETURN(1, 0); }
HLE(SleepEx) {
    plat_sleep_ms(ARG(0) == 0xFFFFFFFF ? 1000000 : ARG(0));
    RETURN(2, 0);
}
HLE(DuplicateHandle) {
    uint32_t src = ARG(1), dstp = ARG(3);
    uint32_t h = src;
    if (src == PSEUDO_THREAD) {
        h = handle_new(H_THREAD);
        handle_get(h, H_THREAD)->thread = c;
    }
    if (dstp) WR32(dstp, h);
    RETURN(7, 1);
}

/* ---------------------------------------------------------------- files */

HLE(GetStdHandle) {
    int32_t n = (int32_t)ARG(0);
    RETURN(1, n == -10 ? STD_IN : n == -11 ? STD_OUT : STD_ERR);
}
HLE(SetStdHandle) { RETURN(2, 1); }
HLE(GetFileType) {
    uint32_t h = ARG(0);
    if (h == STD_IN || h == STD_OUT || h == STD_ERR) RETURN(1, 2);
    RETURN(1, handle_get(h, H_FILE) ? 1 : 0);
}

static int copy_file(const char *from, const char *to) {
    FILE *a = fopen(from, "rb");
    if (!a) return 0;
    FILE *b = fopen(to, "wb");
    if (!b) { fclose(a); return 0; }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, n, b);
    fclose(a); fclose(b);
    return 1;
}

static int file_exists(const char *p) { struct stat st; return stat(p, &st) == 0 && !(st.st_mode & S_IFDIR); }

/* Windows' NUL device ("nul", "nul.txt", "c:\x\nul" ...): the game opens it when a menu
 * screen is entered and aborts if that fails, which it does on iOS, where there is no NUL. */
#ifdef _WIN32
#define NUL_DEVICE "NUL"
#else
#define NUL_DEVICE "/dev/null"
#endif
static int is_nul_device(const char *name) {
    const char *b = name;
    for (const char *p = name; *p; p++) if (*p == '\\' || *p == '/' || *p == ':') b = p + 1;
    if (!*b && b > name && b[-1] == ':') {         /* "nul:" */
        const char *s = b - 1;
        while (s > name && s[-1] != '\\' && s[-1] != '/') s--;
        b = s;
    }
    return (b[0] | 0x20) == 'n' && (b[1] | 0x20) == 'u' && (b[2] | 0x20) == 'l' && (b[3] == 0 || b[3] == '.' || b[3] == ':');
}

HLE(CreateFileA) {
    const char *name = STR(ARG(0));
    uint32_t access = ARG(1), disp = ARG(4);
    int write = (access & 0x40000000u) != 0;
    char rd[1200], wr[1200];
    map_path(name, rd, sizeof rd, 0);
    FILE *f = NULL;
    const char *mode;
    if (!write) {
        if (disp == 1 || disp == 2) { /* create for read-only access: treat as write */ write = 1; }
    }
    if (is_nul_device(name)) {                         /* reads end at once, writes vanish */
        f = fopen(NUL_DEVICE, write ? "r+b" : "rb");
        snprintf(rd, sizeof rd, "%s", NUL_DEVICE);
    } else if (!write) {
        f = fopen(rd, "rb");
    } else {
        map_path(name, wr, sizeof wr, 1);
        int ex = file_exists(wr);
        if (!ex && file_exists(rd) && strcmp(rd, wr) && (disp == 3 || disp == 4)) {
            copy_file(rd, wr);    /* copy-on-write into the save folder */
            ex = 1;
        }
        switch (disp) {
        case 1: mode = ex ? NULL : "w+b"; break;           /* CREATE_NEW */
        case 2: mode = "w+b"; break;                        /* CREATE_ALWAYS */
        case 3: mode = ex ? "r+b" : NULL; break;            /* OPEN_EXISTING */
        case 4: mode = ex ? "r+b" : "w+b"; break;           /* OPEN_ALWAYS */
        case 5: mode = ex ? "w+b" : NULL; break;            /* TRUNCATE_EXISTING */
        default: mode = NULL;
        }
        if (mode) f = fopen(wr, mode);
        snprintf(rd, sizeof rd, "%s", wr);
    }
    if (!f) {
        set_last_error(c, 2);
        plat_log("CreateFileA(%s, %s) failed (%s)", name, write ? "write" : "read", rd);
        if (!name[0]) { plat_log("  empty name from thread %u, name ptr %08X", c->thread_id, ARG(0)); rt_backtrace(c); rt_tracing = 0; }
        RETURN(7, INVALID_HANDLE);
    }
    uint32_t h = handle_new(H_FILE);
    HObj *o = handle_get(h, H_FILE);
    o->file = f;
    o->path = strdup(rd);
    if (getenv("NFS4_TRACE_FILES")) plat_log("CreateFileA(%s, %s) -> %08X (%s)", name, write ? "write" : "read", h, rd);
    set_last_error(c, 0);
    RETURN(7, h);
}

HLE(ReadFile) {
    uint32_t h = ARG(0), buf = ARG(1), n = ARG(2), outp = ARG(3);
    HObj *o = handle_get(h, H_FILE);
    if (!o) { if (outp) WR32(outp, 0); RETURN(5, 0); }
    long before = ftell(o->file);
    size_t r = fread(MEM + buf, 1, n, o->file);
    if (outp) WR32(outp, (uint32_t)r);
    size_t pl = strlen(o->path);
    if (pl >= 10 && !strcmp(o->path + pl - 10, "config.dat")) hle_glide_fix_config(MEM + buf, (uint32_t)before, (uint32_t)r);
    if (rt_tracing) { char m[300]; snprintf(m, sizeof m, "ReadFile(%s, %u) -> %u at %ld", o->path, n, (unsigned)r, ftell(o->file)); rt_trace_note(m); }
    RETURN(5, 1);
}
HLE(WriteFile) {
    uint32_t h = ARG(0), buf = ARG(1), n = ARG(2), outp = ARG(3);
    if (h == STD_OUT || h == STD_ERR) {
        plat_log("[stdout] %.*s", (int)n, STR(buf));
        if (outp) WR32(outp, n);
        RETURN(5, 1);
    }
    HObj *o = handle_get(h, H_FILE);
    if (!o) RETURN(5, 0);
    size_t r = fwrite(MEM + buf, 1, n, o->file);
    if (outp) WR32(outp, (uint32_t)r);
    RETURN(5, 1);
}
HLE(SetFilePointer) {
    HObj *o = handle_get(ARG(0), H_FILE);
    if (!o) RETURN(4, 0xFFFFFFFF);
    int32_t d = (int32_t)ARG(1);
    if (ARG(2)) WR32(ARG(2), 0);
    fseek(o->file, d, ARG(3) == 0 ? SEEK_SET : ARG(3) == 1 ? SEEK_CUR : SEEK_END);
    if (rt_tracing) { char m[300]; snprintf(m, sizeof m, "SetFilePointer(%d, %u) -> %ld", d, ARG(3), ftell(o->file)); rt_trace_note(m); }
    RETURN(4, (uint32_t)ftell(o->file));
}
HLE(GetFileSize) {
    HObj *o = handle_get(ARG(0), H_FILE);
    if (!o) RETURN(2, 0xFFFFFFFF);
    long cur = ftell(o->file);
    fseek(o->file, 0, SEEK_END);
    long n = ftell(o->file);
    fseek(o->file, cur, SEEK_SET);
    if (ARG(1)) WR32(ARG(1), 0);
    RETURN(2, (uint32_t)n);
}
HLE(FlushFileBuffers) { HObj *o = handle_get(ARG(0), H_FILE); if (o) fflush(o->file); RETURN(1, 1); }
HLE(SetEndOfFile) {
    HObj *o = handle_get(ARG(0), H_FILE);
    if (o) { fflush(o->file); plat_truncate(o->file, ftell(o->file)); }
    RETURN(1, o != NULL);
}
HLE(CloseHandle) {
    uint32_t h = ARG(0);
    HObj *o = handle_get(h, 0);
    if (o) {
        if (o->type == H_FILE && o->file) fclose(o->file);
        if (o->type == H_THREAD || o->type == H_EVENT || o->type == H_MUTEX) { RETURN(1, 1); }  /* keep objects alive */
        handle_free(h);
    }
    RETURN(1, 1);
}
HLE(DeleteFileA) {
    char p[1200];
    map_path(STR(ARG(0)), p, sizeof p, 1);
    RETURN(1, remove(p) == 0);
}
HLE(MoveFileA) {
    char a[1200], b[1200];
    map_path(STR(ARG(0)), a, sizeof a, 1);
    map_path(STR(ARG(1)), b, sizeof b, 1);
    if (!file_exists(a)) {
        char source[1200]; map_path(STR(ARG(0)), source, sizeof source, 0);
        if (!file_exists(source)) { set_last_error(c, 2); RETURN(2, 0); }
        copy_file(source, a);
    }
    RETURN(2, rename(a, b) == 0);
}
HLE(GetFileAttributesA) {
    char p[1200];
    map_path(STR(ARG(0)), p, sizeof p, 0);
    struct stat st;
    if (getenv("NFS4_TRACE_FILES")) plat_log("GetFileAttributesA(%s)", STR(ARG(0)));
    if (stat(p, &st) != 0) { set_last_error(c, 2); RETURN(1, 0xFFFFFFFF); }
    RETURN(1, (st.st_mode & S_IFDIR) ? 0x10 : 0x20);
}
HLE(SetFileAttributesA) { RETURN(2, 1); }
HLE(CreateDirectoryA) {
    char p[1200];
    map_path(STR(ARG(0)), p, sizeof p, 1);
    plat_mkdir(p);
    RETURN(2, 1);
}
HLE(RemoveDirectoryA) { RETURN(1, 1); }
HLE(GetCurrentDirectoryA) {
    put_str(ARG(1), ARG(0), guest_cwd());
    RETURN(2, strlen(guest_cwd()));
}
void set_guest_cwd(const char *p);
HLE(SetCurrentDirectoryA) {
    const char *p = STR(ARG(0));
    char full[300];
    if (p[0] && p[1] == ':') snprintf(full, sizeof full, "%s", p);
    else snprintf(full, sizeof full, "%s\\%s", guest_cwd(), p);
    size_t n = strlen(full);
    while (n > 3 && (full[n - 1] == '\\' || full[n - 1] == '/')) full[--n] = 0;
    set_guest_cwd(full);
    RETURN(1, 1);
}
HLE(GetFullPathNameA) {
    const char *p = STR(ARG(0));
    char full[600];
    if (p[0] && p[1] == ':') snprintf(full, sizeof full, "%s", p);
    else snprintf(full, sizeof full, "%s\\%s", guest_cwd(), p);
    for (char *q = full; *q; q++) if (*q == '/') *q = '\\';
    put_str(ARG(2), ARG(1), full);
    if (ARG(3)) WR32(ARG(3), ARG(2) + (uint32_t)(basename_of(full) - full));
    RETURN(4, strlen(full));
}

/* FindFirstFile: collect matches from the save, mods and game folders (first one wins, like reads) */
typedef struct { char names[512][64]; uint8_t dir[512]; uint32_t size[512]; int n, pos; char pattern[64]; } FindState;

static int wild(const char *p, const char *s) {
    if (!*p) return !*s;
    if (*p == '*') { for (;;) { if (wild(p + 1, s)) return 1; if (!*s) return 0; s++; } }
    if (!*s) return 0;
    if (*p == '?' || tolower((unsigned char)*p) == tolower((unsigned char)*s)) return wild(p + 1, s + 1);
    return 0;
}

static void find_cb(const char *name, int is_dir, uint32_t size, void *ctx) {
    FindState *fs = ctx;
    if (fs->n >= 512 || strlen(name) >= 64) return;
    if (!wild(fs->pattern, name) && !(strcmp(fs->pattern, "*.*") == 0)) return;
    for (int i = 0; i < fs->n; i++) if (ieq(fs->names[i], name)) return;
    strcpy(fs->names[fs->n], name);
    fs->dir[fs->n] = (uint8_t)is_dir;
    fs->size[fs->n] = size;
    fs->n++;
}

static void fill_find(FindState *fs, uint32_t p) {
    memset(MEM + p, 0, 320);
    int i = fs->pos++;
    WR32(p, fs->dir[i] ? 0x10 : 0x20);
    WR32(p + 32, fs->size[i]);
    strcpy((char *)MEM + p + 44, fs->names[i]);
}

HLE(FindFirstFileA) {
    const char *pat = STR(ARG(0));
    if (getenv("NFS4_TRACE_FILES")) plat_log("FindFirstFileA(%s)", pat);
    if (getenv("NFS4_TRACE") && strstr(pat, "carmodel")) rt_tracing = 1;
    char dirpart[600];
    snprintf(dirpart, sizeof dirpart, "%s", pat);
    char *b = (char *)basename_of(dirpart);
    FindState *fs = calloc(1, sizeof *fs);
    snprintf(fs->pattern, sizeof fs->pattern, "%s", b);
    *b = 0;
    if (!dirpart[0]) strcpy(dirpart, ".");
    const char *roots[3] = { plat_save_dir(), plat_mods_dir(), plat_game_dir() };
    for (int r = 0; r < 3; r++) {
        if (!roots[r]) continue;
        char host[1200];
        map_path_in(roots[r], dirpart, host, sizeof host);
        plat_list_dir(host, find_cb, fs);
    }
    if (!fs->n) { free(fs); set_last_error(c, 2); RETURN(2, INVALID_HANDLE); }
    uint32_t h = handle_new(H_FIND);
    handle_get(h, H_FIND)->extra = fs;
    fill_find(fs, ARG(1));
    RETURN(2, h);
}
HLE(FindNextFileA) {
    HObj *o = handle_get(ARG(0), H_FIND);
    if (!o) RETURN(2, 0);
    FindState *fs = o->extra;
    if (fs->pos >= fs->n) { set_last_error(c, 18); RETURN(2, 0); }
    fill_find(fs, ARG(1));
    RETURN(2, 1);
}
HLE(FindClose) {
    HObj *o = handle_get(ARG(0), H_FIND);
    if (o) { free(o->extra); handle_free(ARG(0)); }
    RETURN(1, 1);
}

/* file mappings: the whole file is read into guest memory */
HLE(CreateFileMappingA) {
    HObj *f = handle_get(ARG(0), H_FILE);
    uint32_t h = handle_new(H_MAPPING);
    HObj *o = handle_get(h, H_MAPPING);
    if (f) {
        long cur = ftell(f->file);
        fseek(f->file, 0, SEEK_END);
        o->size = (uint32_t)ftell(f->file);
        fseek(f->file, 0, SEEK_SET);
        o->value = guest_valloc(o->size ? o->size : 1);
        if (fread(MEM + o->value, 1, o->size, f->file) != o->size) plat_log("CreateFileMapping: short read");
        fseek(f->file, cur, SEEK_SET);
    } else {
        o->size = ARG(4);
        o->value = guest_valloc(o->size ? o->size : 1);
    }
    RETURN(6, h);
}
HLE(MapViewOfFile) {
    HObj *o = handle_get(ARG(0), H_MAPPING);
    RETURN(5, o ? o->value + ARG(3) : 0);
}
HLE(UnmapViewOfFile) { RETURN(1, 1); }

/* ---------------------------------------------------------------- INI files */

static char *read_ini(const char *guest_name) {
    char host[1200];
    map_path(guest_name, host, sizeof host, 0);
    FILE *f = fopen(host, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc(n + 1);
    s[fread(s, 1, n, f)] = 0;
    fclose(f);
    return s;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

/* Iterate lines of the ini; returns the value for (section, key), or with
 * key==NULL appends every key/"key=value" of the section to out. */
static int ini_lookup(const char *text, const char *section, const char *key, char *out, size_t n, int pairs) {
    if (!text) return 0;
    char *copy = strdup(text);
    int in = 0, found = 0;
    size_t used = 0;
    for (char *line = copy; line && *line; ) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        char *l = trim(line);
        if (*l == '[') {
            char *e = strchr(l, ']');
            if (e) *e = 0;
            in = section && ieq(l + 1, section);
        } else if (in && *l && *l != ';') {
            char *eq = strchr(l, '=');
            if (eq) {
                *eq = 0;
                char *k = trim(l), *v = trim(eq + 1);
                if (key) {
                    if (ieq(k, key)) { snprintf(out, n, "%s", v); found = 1; break; }
                } else {
                    char item[600];
                    if (pairs) snprintf(item, sizeof item, "%s=%s", k, v);
                    else snprintf(item, sizeof item, "%s", k);
                    size_t len = strlen(item) + 1;
                    if (used + len + 1 < n) { memcpy(out + used, item, len); used += len; }
                    found = 1;
                }
            }
        }
        line = next;
    }
    if (!key && n) { out[used] = 0; if (used + 1 < n) out[used + 1] = 0; }
    free(copy);
    return key ? found : (int)used;
}

HLE(GetPrivateProfileIntA) {
    char *t = read_ini(STR(ARG(3)));
    char v[256];
    uint32_t r = ARG(2);
    if (ini_lookup(t, STR(ARG(0)), STR(ARG(1)), v, sizeof v, 0)) r = (uint32_t)strtol(v, NULL, 0);
    /* the MAD movie player does not work yet (bad Thrash window); skip movies until it does */
    if (ARG(1) && ieq(STR(ARG(1)), "NoMovies") && !getenv("NFS4_ALLOW_MOVIES")) r = 1;
    /* Modern Patch: texture size limit 256 << n; the game caps it to grGet's 2048, and original
     * 256x256 art is unaffected, so high-resolution replacements in the mods folder just work */
    if (ARG(1) && ieq(STR(ARG(1)), "AllowHugeTextures")) r = 3;
    free(t);
    RETURN(4, r);
}
HLE(GetPrivateProfileStringA) {
    char *t = read_ini(STR(ARG(5)));
    uint32_t buf = ARG(3), size = ARG(4);
    const char *section = ARG(0) ? STR(ARG(0)) : NULL;
    if (!ARG(1)) {
        char *tmp = calloc(1, size + 2);
        int n = ini_lookup(t, section, NULL, tmp, size, 0);
        memcpy(MEM + buf, tmp, size);
        free(tmp); free(t);
        RETURN(6, n);
    }
    char v[1024];
    if (!ini_lookup(t, section, STR(ARG(1)), v, sizeof v, 0)) snprintf(v, sizeof v, "%s", ARG(2) ? STR(ARG(2)) : "");
    /* Select the NFS4 Glide driver without modifying the PC installation. */
    if (ieq(STR(ARG(1)), "ThrashDriver")) snprintf(v, sizeof v, "glide3x");
    put_str(buf, size, v);
    free(t);
    RETURN(6, size ? (strlen(v) < size ? strlen(v) : size - 1) : 0);
}
HLE(GetPrivateProfileSectionA) {
    char *t = read_ini(STR(ARG(3)));
    uint32_t buf = ARG(1), size = ARG(2);
    char *tmp = calloc(1, size + 2);
    int n = ini_lookup(t, STR(ARG(0)), NULL, tmp, size, 1);
    memcpy(MEM + buf, tmp, size);
    free(tmp); free(t);
    RETURN(4, n);
}

/* ---------------------------------------------------------------- resources (PE .rsrc in guest memory) */

static uint32_t rsrc_root(uint32_t module) {
    uint32_t pe = module + RD32(module + 0x3C);
    uint32_t rva = RD32(pe + 24 + 96 + 2 * 8);
    return rva ? module + rva : 0;
}

static uint32_t rsrc_find(uint32_t root, uint32_t dir, uint32_t id) {
    uint16_t named = RD16(dir + 12), ids = RD16(dir + 14);
    uint32_t e = dir + 16;
    for (int i = 0; i < named + ids; i++, e += 8) {
        uint32_t name = RD32(e), off = RD32(e + 4);
        int match;
        if (id >= 0x10000) {                     /* string name */
            if (!(name & 0x80000000)) continue;
            uint32_t s = root + (name & 0x7FFFFFFF);
            uint16_t len = RD16(s);
            const char *want = STR(id);
            if (want[0] == '#') { match = 0; }
            else {
                match = strlen(want) == len;
                for (int k = 0; match && k < len; k++)
                    if (toupper(RD16(s + 2 + 2 * k)) != toupper((unsigned char)want[k])) match = 0;
            }
        } else {
            match = !(name & 0x80000000) && name == id;
        }
        if (match) return off;
    }
    return 0;
}

HLE(FindResourceA) {
    uint32_t module = ARG(0) ? ARG(0) : rt_modules[0].base, name = ARG(1), type = ARG(2);
    uint32_t root = rsrc_root(module);
    if (!root) RETURN(3, 0);
    if (name >= 0x10000 && STR(name)[0] == '#') name = (uint32_t)atoi(STR(name) + 1);
    if (type >= 0x10000 && STR(type)[0] == '#') type = (uint32_t)atoi(STR(type) + 1);
    uint32_t t = rsrc_find(root, root, type);
    if (!(t & 0x80000000)) RETURN(3, 0);
    uint32_t n = rsrc_find(root, root + (t & 0x7FFFFFFF), name);
    if (!(n & 0x80000000)) RETURN(3, 0);
    uint32_t langdir = root + (n & 0x7FFFFFFF);
    uint32_t first = RD32(langdir + 16 + 4);
    if (first & 0x80000000) RETURN(3, 0);
    RETURN(3, root + first);                     /* IMAGE_RESOURCE_DATA_ENTRY */
}
HLE(LoadResource) { RETURN(2, ARG(1)); }
HLE(LockResource) {
    uint32_t e = ARG(0);
    RETURN(1, e ? rt_modules[0].base + RD32(e) : 0);
}
HLE(SizeofResource) { RETURN(2, ARG(1) ? RD32(ARG(1) + 4) : 0); }

/* ---------------------------------------------------------------- console / comm stubs */

HLE(GetConsoleMode) { RETURN(2, 0); }
HLE(SetConsoleMode) { RETURN(2, 0); }
HLE(PeekConsoleInputA) { if (ARG(3)) WR32(ARG(3), 0); RETURN(4, 0); }
HLE(ReadConsoleInputA) { if (ARG(3)) WR32(ARG(3), 0); RETURN(4, 0); }
HLE(WriteConsoleA) { plat_log("[console] %.*s", (int)ARG(2), STR(ARG(1))); if (ARG(3)) WR32(ARG(3), ARG(2)); RETURN(5, 1); }
HLE(EscapeCommFunction) { RETURN(2, 0); }
HLE(GetCommModemStatus) { RETURN(2, 0); }
HLE(GetCommProperties) { RETURN(2, 0); }
HLE(GetCommState) { RETURN(2, 0); }
HLE(SetCommState) { RETURN(2, 0); }
HLE(SetCommTimeouts) { RETURN(2, 0); }
HLE(PurgeComm) { RETURN(2, 0); }
HLE(GetOverlappedResult) { RETURN(4, 0); }

static void write_filetime(uint32_t out, int64_t unix_time) {
    if (out) WR64(out, (uint64_t)(unix_time + 11644473600LL) * 10000000ULL);
}
HLE(GetFileTime) {
    HObj *o=handle_get(ARG(0),H_FILE); struct stat st;
    if(!o||!o->path||stat(o->path,&st)){set_last_error(c,6);RETURN(4,0);}
    write_filetime(ARG(1),st.st_ctime);write_filetime(ARG(2),st.st_atime);write_filetime(ARG(3),st.st_mtime);RETURN(4,1);
}
HLE(CompareFileTime) {uint64_t a=RD64(ARG(0)),b=RD64(ARG(1));RETURN(2,a==b?0:a<b?0xFFFFFFFFu:1);}
/* Legacy HFILE APIs use the same overlay and handle table as CreateFileA. */
HLE(_lopen) {
    const char *name=STR(ARG(0));uint32_t access=ARG(1)&3;char rd[1200],wr[1200];
    map_path(name,rd,sizeof rd,0);const char *path=rd;
    if(access){map_path(name,wr,sizeof wr,1);if(!file_exists(wr)&&file_exists(rd)&&strcmp(rd,wr))copy_file(rd,wr);path=wr;}
    FILE *f=fopen(is_nul_device(name)?NUL_DEVICE:path,access?"r+b":"rb");
    if(!f){set_last_error(c,2);plat_log("_lopen(%s,%u) -> not found",name,access);RETURN(2,0xFFFFFFFFu);}
    uint32_t h=handle_new(H_FILE);HObj *o=handle_get(h,H_FILE);o->file=f;o->path=strdup(path);
    plat_log("_lopen(%s,%u) -> %08X",name,access,h);RETURN(2,h);
}
HLE(_lclose) {HObj *o=handle_get(ARG(0),H_FILE);if(!o)RETURN(1,0xFFFFFFFFu);if(o->file)fclose(o->file);handle_free(ARG(0));RETURN(1,0);}
/* ---------------------------------------------------------------- table */

#define E(n) { #n, n }
const HleEntry hle_kernel32[] = {
    E(GetFileTime), E(CompareFileTime), E(_lopen), E(_lclose), E(GetVersion), E(GetCommandLineA), E(GetCommandLineW), E(GetEnvironmentStrings), E(FreeEnvironmentStringsA),
    E(SetEnvironmentVariableA), E(SetEnvironmentVariableW), E(GetModuleFileNameA), E(GetModuleFileNameW),
    E(GetModuleHandleA), E(LoadLibraryA), E(FreeLibrary), E(GetProcAddress), E(ExitProcess),
    E(GetCurrentProcess), E(GetCurrentProcessId), E(GetCurrentThread), E(GetCurrentThreadId), E(GetLastError),
    E(SetLastError), E(SetErrorMode), E(SetUnhandledExceptionFilter), E(UnhandledExceptionFilter),
    E(SetConsoleCtrlHandler), E(DebugBreak), E(OutputDebugStringA), E(RtlUnwind),
    E(GetTickCount), E(GetLocalTime), E(GetTimeZoneInformation), E(GetSystemInfo), E(GlobalMemoryStatus),
    E(GetWindowsDirectoryA), E(GetDiskFreeSpaceA), E(GetACP), E(GetOEMCP), E(GetCPInfo),
    E(MultiByteToWideChar), E(WideCharToMultiByte),
    E(VirtualAlloc), E(VirtualFree), E(VirtualQuery), E(GetProcessHeap), E(HeapCreate), E(HeapDestroy),
    E(HeapAlloc), E(HeapFree), E(HeapReAlloc), E(HeapSize),
    E(TlsAlloc), E(TlsFree), E(TlsGetValue), E(TlsSetValue),
    E(InitializeCriticalSection), E(DeleteCriticalSection), E(EnterCriticalSection), E(LeaveCriticalSection),
    E(OpenMutexA), E(CreateEventA), E(SetEvent), E(ResetEvent), E(PulseEvent), E(CreateMutexA),
    E(WaitForSingleObject), E(WaitForMultipleObjectsEx),
    E(CreateThread), E(ResumeThread), E(ExitThread), E(TerminateThread), E(SetThreadPriority),
    E(GetThreadPriority), E(SleepEx), E(DuplicateHandle),
    E(GetStdHandle), E(SetStdHandle), E(GetFileType), E(CreateFileA), E(ReadFile), E(WriteFile),
    E(SetFilePointer), E(GetFileSize), E(FlushFileBuffers), E(SetEndOfFile), E(CloseHandle), E(DeleteFileA),
    E(MoveFileA), E(GetFileAttributesA), E(SetFileAttributesA), E(CreateDirectoryA), E(RemoveDirectoryA),
    E(GetCurrentDirectoryA), E(SetCurrentDirectoryA), E(GetFullPathNameA),
    E(FindFirstFileA), E(FindNextFileA), E(FindClose),
    E(CreateFileMappingA), E(MapViewOfFile), E(UnmapViewOfFile),
    E(GetPrivateProfileIntA), E(GetPrivateProfileStringA), E(GetPrivateProfileSectionA),
    E(FindResourceA), E(LoadResource), E(LockResource), E(SizeofResource),
    E(GetConsoleMode), E(SetConsoleMode), E(PeekConsoleInputA), E(ReadConsoleInputA), E(WriteConsoleA),
    E(EscapeCommFunction), E(GetCommModemStatus), E(GetCommProperties), E(GetCommState), E(SetCommState),
    E(SetCommTimeouts), E(PurgeComm), E(GetOverlappedResult),
    { NULL, NULL }
};

void hle_kernel32_init(void) {
    g_start_us = plat_ticks_us();
    g_wlock = plat_mutex_new();
    g_wcond = plat_cond_new();
    g_cs_init_lock = plat_mutex_new();
}
