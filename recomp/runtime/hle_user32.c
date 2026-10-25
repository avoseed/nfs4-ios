/* USER32 replacement: one top-level window, per-thread message queues. */
#include "hle.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define HWND_MAIN 0x00010010u
#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_SHOWWINDOW 0x0018
#define WM_ACTIVATEAPP 0x001C
#define WM_NCCREATE 0x0081
#define WM_QUIT 0x0012

int g_screen_w = 640, g_screen_h = 480;

typedef struct { uint32_t hwnd, msg, wp, lp; } Msg;
typedef struct Queue { uint32_t tid; Msg q[256]; int head, tail; PlatEvent *ev; } Queue;
static Queue g_queues[16];
static PlatMutex *g_qlock;

static struct { char name[64]; uint32_t proc; } g_classes[16];
static int g_nclasses;
static uint32_t g_wndproc, g_window_tid, g_style;

static Queue *queue_for(uint32_t tid) {
    plat_mutex_lock(g_qlock);
    for (int i = 0; i < 16; i++) if (g_queues[i].tid == tid) { plat_mutex_unlock(g_qlock); return &g_queues[i]; }
    for (int i = 0; i < 16; i++) if (!g_queues[i].tid) {
        g_queues[i].tid = tid;
        g_queues[i].ev = plat_event_new(0, 0);
        plat_mutex_unlock(g_qlock);
        return &g_queues[i];
    }
    plat_mutex_unlock(g_qlock);
    plat_fatal("too many message queues");
    return NULL;
}

void post_message(uint32_t tid, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    Queue *q = queue_for(tid);
    plat_mutex_lock(g_qlock);
    int next = (q->tail + 1) % 256;
    if (next != q->head) { q->q[q->tail] = (Msg){ hwnd, msg, wp, lp }; q->tail = next; }
    plat_mutex_unlock(g_qlock);
    plat_event_set(q->ev);
}

static uint32_t call_wndproc(Cpu *c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    if (!g_wndproc) return 0;
    uint32_t a[4] = { hwnd, msg, wp, lp };
    return rt_guest_call(c, g_wndproc, 4, a);
}

/* ---------------------------------------------------------------- windows */

HLE(RegisterClassA) {
    uint32_t wc = ARG(0);
    if (g_nclasses < 16) {
        g_classes[g_nclasses].proc = RD32(wc + 4);
        uint32_t name = RD32(wc + 36);
        snprintf(g_classes[g_nclasses].name, 64, "%s", name >= 0x10000 ? STR(name) : "#atom");
        plat_log("RegisterClassA(%s, proc=%08X)", g_classes[g_nclasses].name, g_classes[g_nclasses].proc);
        g_nclasses++;
    }
    RETURN(1, 0xC000 + g_nclasses);
}

HLE(CreateWindowExA) {
    uint32_t cls = ARG(1), title = ARG(2), param = ARG(11);
    const char *cname = cls >= 0x10000 ? STR(cls) : "";
    for (int i = 0; i < g_nclasses; i++)
        if (!strcmp(g_classes[i].name, cname) || cls < 0x10000) g_wndproc = g_classes[i].proc;
    g_style = ARG(3);
    g_window_tid = c->thread_id;
    plat_log("[t%u] CreateWindowExA(class=%s, title=%s, %dx%d) proc=%08X", c->thread_id, cname, title ? STR(title) : "", ARG(6), ARG(7), g_wndproc);
    uint32_t cs = guest_alloc(48);
    WR32(cs, param); WR32(cs + 4, rt_modules[0].base); WR32(cs + 8, 0); WR32(cs + 12, 0);
    WR32(cs + 16, ARG(7)); WR32(cs + 20, ARG(6)); WR32(cs + 24, ARG(5)); WR32(cs + 28, ARG(4));
    WR32(cs + 32, ARG(3)); WR32(cs + 36, title); WR32(cs + 40, cls); WR32(cs + 44, ARG(0));
    call_wndproc(c, HWND_MAIN, WM_NCCREATE, 0, cs);
    call_wndproc(c, HWND_MAIN, WM_CREATE, 0, cs);
    RETURN(12, HWND_MAIN);
}

HLE(ShowWindow) {
    post_message(g_window_tid, HWND_MAIN, WM_SHOWWINDOW, 1, 0);
    post_message(g_window_tid, HWND_MAIN, WM_ACTIVATEAPP, 1, 0);
    post_message(g_window_tid, HWND_MAIN, WM_ACTIVATE, 1, 0);
    post_message(g_window_tid, HWND_MAIN, WM_SETFOCUS, 0, 0);
    RETURN(2, 0);
}
HLE(DestroyWindow) { post_message(g_window_tid, HWND_MAIN, WM_DESTROY, 0, 0); RETURN(1, 1); }
HLE(DefWindowProcA) { RETURN(4, 0); }
HLE(GetWindowLongA) {
    int32_t i = (int32_t)ARG(1);
    RETURN(2, i == -4 ? g_wndproc : i == -16 ? g_style : 0);
}
HLE(SetWindowPos) {
    uint32_t w=ARG(4),h=ARG(5),flags=ARG(6);
    if(!(flags&1u)&&w&&h&&w<=4096&&h<=2048){g_screen_w=(int)w;g_screen_h=(int)h;}
    plat_log("SetWindowPos(%ux%u, flags=%08X) -> client %dx%d",w,h,flags,g_screen_w,g_screen_h);
    RETURN(7,1);
}
HLE(AdjustWindowRect) { RETURN(3, 1); }
HLE(GetClientRect) {
    static int logged; if(logged++<8)plat_log("GetClientRect -> %dx%d from %08X",g_screen_w,g_screen_h,RD32(c->esp));
    uint32_t r = ARG(1);
    WR32(r, 0); WR32(r + 4, 0); WR32(r + 8, g_screen_w); WR32(r + 12, g_screen_h);
    RETURN(2, 1);
}
HLE(ClientToScreen) { RETURN(2, 1); }
HLE(GetDesktopWindow) { RETURN(0, 0x00010001); }
HLE(GetWindowDC) { RETURN(1, 0x00020001); }
HLE(BeginPaint) { memset(MEM + ARG(1), 0, 64); RETURN(2, 0x00020001); }
HLE(EndPaint) { RETURN(2, 1); }
HLE(IsIconic) { RETURN(1, 0); }
HLE(SetActiveWindow) { RETURN(1, HWND_MAIN); }
HLE(SetForegroundWindow) { RETURN(1, 1); }
HLE(FindWindowA) { RETURN(2, 0); }
HLE(LoadIconA) { RETURN(2, 0x00030001); }
HLE(SetCursor) { RETURN(1, 0); }
HLE(ShowCursor) { RETURN(1, 0); }
HLE(SetCursorPos) { RETURN(2, 1); }
HLE(SwapMouseButton) { RETURN(1, 0); }
HLE(GetSystemMetrics) {
    uint32_t i = ARG(0);
    RETURN(1, i == 0 ? g_screen_w : i == 1 ? g_screen_h : 0);
}
HLE(SystemParametersInfoA) { RETURN(4, 1); }
HLE(SetWindowsHookExA) { RETURN(4, 0x00040001); }
HLE(UnhookWindowsHookEx) { RETURN(1, 1); }
HLE(CallNextHookEx) { RETURN(4, 0); }
HLE(MessageBoxA) {
    plat_log("MessageBoxA: [%s] %s", ARG(2) ? STR(ARG(2)) : "", ARG(1) ? STR(ARG(1)) : "");
    rt_backtrace(c);
    rt_tracing = 0;
    if (getenv("NFS4_DUMP_ON_MSG")) {
        uint32_t a = (uint32_t)strtoul(getenv("NFS4_DUMP_ON_MSG"), NULL, 16);
        char line[400]; int k = 0;
        for (int i = 0; i < 64; i += 4) k += snprintf(line + k, sizeof line - k, " %08X", RD32(a + i));
        plat_log("  dump %08X:%s", a, line);
    }
    RETURN(4, 1);
}

/* ---------------------------------------------------------------- messages */

HLE(PostMessageA) {
    if (getenv("NFS4_TRACE_WAITS")) plat_log("[t%u] PostMessageA(%08X, %04X, %08X, %08X) -> queue of t%u", c->thread_id, ARG(0), ARG(1), ARG(2), ARG(3), g_window_tid); post_message(g_window_tid ? g_window_tid : c->thread_id, ARG(0), ARG(1), ARG(2), ARG(3)); RETURN(4, 1); }
HLE(SendNotifyMessageA) { post_message(g_window_tid ? g_window_tid : c->thread_id, ARG(0), ARG(1), ARG(2), ARG(3)); RETURN(4, 1); }
HLE(PostThreadMessageA) { post_message(ARG(0), 0, ARG(1), ARG(2), ARG(3)); RETURN(4, 1); }
HLE(PostQuitMessage) { post_message(c->thread_id, 0, WM_QUIT, ARG(0), 0); RETURN(1, 0); }

HLE(GetMessageA) {
    uint32_t mp = ARG(0);
    static int logged;
    if (!logged++) plat_log("[t%u] first GetMessageA", c->thread_id);
    Queue *q = queue_for(c->thread_id);
    for (;;) {
        plat_mutex_lock(g_qlock);
        if (q->head != q->tail) {
            Msg m = q->q[q->head];
            q->head = (q->head + 1) % 256;
            plat_mutex_unlock(g_qlock);
            WR32(mp, m.hwnd); WR32(mp + 4, m.msg); WR32(mp + 8, m.wp); WR32(mp + 12, m.lp);
            WR32(mp + 16, (uint32_t)(plat_ticks_us() / 1000)); WR32(mp + 20, 0); WR32(mp + 24, 0);
            RETURN(4, m.msg == WM_QUIT ? 0 : 1);
        }
        plat_mutex_unlock(g_qlock);
        plat_event_wait(q->ev, 50);
    }
}
HLE(TranslateMessage) { RETURN(1, 0); }
HLE(DispatchMessageA) {
    uint32_t mp = ARG(0);
    uint32_t r = 0;
    if (RD32(mp) == HWND_MAIN) r = call_wndproc(c, RD32(mp), RD32(mp + 4), RD32(mp + 8), RD32(mp + 12));
    RETURN(1, r);
}

/* ---------------------------------------------------------------- keyboard */

/* virtual-key -> DirectInput scan code (just the keys the game cares about) */
static uint8_t vk_to_dik(uint32_t vk) {
    if (vk >= 'A' && vk <= 'Z') {
        static const uint8_t az[26] = { 0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,
                                        0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C };
        return az[vk - 'A'];
    }
    if (vk >= '1' && vk <= '9') return (uint8_t)(0x02 + vk - '1');
    if (vk >= 0x70 && vk <= 0x79) return (uint8_t)(0x3B + vk - 0x70);   /* F1-F10 */
    switch (vk) {
    case '0': return 0x0B; case 0x1B: return 0x01; case 0x0D: return 0x1C; case 0x20: return 0x39;
    case 0x25: return 0xCB; case 0x26: return 0xC8; case 0x27: return 0xCD; case 0x28: return 0xD0;
    case 0x10: case 0xA0: return 0x2A; case 0xA1: return 0x36; case 0x11: case 0xA2: return 0x1D;
    case 0xA3: return 0x9D; case 0x12: return 0x38; case 0x09: return 0x0F; case 0x08: return 0x0E;
    }
    return 0;
}

static uint32_t dik_to_vk(uint8_t sc) {
    for (uint32_t vk = 1; vk < 256; vk++) {
        if (vk == 0x10 || vk == 0x11) continue;     /* prefer left/right specific forms below */
        if (vk_to_dik(vk) == sc) return vk;
    }
    return 0;
}

/* Called by the host for every key transition (DIK scan code): becomes window messages. */
void hle_key_event(uint8_t sc, int down) {
    if (!g_window_tid) return;
    uint32_t vk = dik_to_vk(sc);
    if (sc == 0x2A || sc == 0x36) vk = 0x10;        /* shift */
    if (sc == 0x1D || sc == 0x9D) vk = 0x11;        /* control */
    if (!vk) return;
    uint32_t lp = 1u | ((uint32_t)(sc & 0x7F) << 16) | ((sc & 0x80) ? (1u << 24) : 0) | (down ? 0 : 0xC0000000u);
    post_message(g_window_tid, HWND_MAIN, down ? 0x0100 : 0x0101, vk, lp);      /* WM_KEYDOWN / WM_KEYUP */
    if (down) {
        uint32_t ch = 0;
        if (vk >= 'A' && vk <= 'Z') ch = (plat_keys[0x2A] || plat_keys[0x36]) ? vk : vk + 32;   /* shift held: upper case */
        else if ((vk >= '0' && vk <= '9') || vk == 0x20 || vk == 0x0D || vk == 0x1B || vk == 0x08) ch = vk;
        if (ch) post_message(g_window_tid, HWND_MAIN, 0x0102, ch, lp);          /* WM_CHAR */
    }
}

HLE(GetAsyncKeyState) { uint8_t k = vk_to_dik(ARG(0)); RETURN(1, k && plat_keys[k] ? 0x8000 : 0); }
HLE(GetKeyState) { uint8_t k = vk_to_dik(ARG(0)); RETURN(1, k && plat_keys[k] ? 0xFF80 : 0); }
HLE(GetKeyboardType) { int32_t t = (int32_t)ARG(0); RETURN(1, t == 0 ? 4 : t == 2 ? 12 : 0); }
HLE(keybd_event) { RETURN(4, 0); }

/* ---------------------------------------------------------------- strings */

HLE(CharUpperA) {
    uint32_t p = ARG(0);
    if (p < 0x10000) RETURN(1, (uint32_t)toupper((int)p));
    for (uint32_t q = p; MEM[q]; q++) MEM[q] = (uint8_t)toupper(MEM[q]);
    RETURN(1, p);
}
HLE(CharUpperBuffA) {
    for (uint32_t i = 0; i < ARG(1); i++) MEM[ARG(0) + i] = (uint8_t)toupper(MEM[ARG(0) + i]);
    RETURN(2, ARG(1));
}

/* wsprintfA(buf, fmt, ...) - cdecl */
HLE(wsprintfA) {
    uint32_t out = ARG(0);
    const char *f = STR(ARG(1));
    int argi = 2;
    char buf[2048];
    size_t n = 0;
    while (*f && n < sizeof buf - 64) {
        if (*f != '%') { buf[n++] = *f++; continue; }
        const char *start = f++;
        if (*f == '%') { buf[n++] = '%'; f++; continue; }
        char spec[32];
        while (*f && strchr("-+ #0123456789.", *f)) f++;
        int is_long = 0;
        while (*f == 'l' || *f == 'h') { is_long = 1; f++; }
        (void)is_long;
        char conv = *f++;
        size_t sl = (size_t)(f - start);
        if (sl >= sizeof spec - 2) sl = sizeof spec - 2;
        memcpy(spec, start, sl);
        spec[sl] = 0;
        /* strip length modifiers for host printf */
        char clean[32]; size_t k = 0;
        for (size_t i = 0; i < sl; i++) if (spec[i] != 'l' && spec[i] != 'h') clean[k++] = spec[i];
        clean[k] = 0;
        uint32_t v = ARG(argi++);
        switch (conv) {
        case 's': n += snprintf(buf + n, sizeof buf - n, clean, v ? STR(v) : "(null)"); break;
        case 'c': n += snprintf(buf + n, sizeof buf - n, clean, (int)v); break;
        case 'd': case 'i': n += snprintf(buf + n, sizeof buf - n, clean, (int)v); break;
        default: n += snprintf(buf + n, sizeof buf - n, clean, v); break;
        }
    }
    buf[n] = 0;
    memcpy(MEM + out, buf, n + 1);
    RETURN_CDECL(n);
}

/* ---------------------------------------------------------------- DDE (unused) */
HLE(DdeInitializeA) { WR32(ARG(0), 0); RETURN(4, 0x4000); }
HLE(DdeUninitialize) { RETURN(1, 1); }
HLE(DdeConnect) { RETURN(4, 0); }
HLE(DdeClientTransaction) { RETURN(8, 0); }
HLE(DdeCreateStringHandleA) { RETURN(3, 0); }
HLE(DdeGetLastError) { RETURN(1, 0); }
HLE(DdeQueryStringA) { RETURN(5, 0); }

/* Cursor handles are virtual: presentation and input belong to the platform host. */
HLE(LoadCursorA) { RETURN(2, 0x00030000u | (ARG(1) & 0xFFFFu)); }
HLE(UnregisterClassA) { RETURN(2, 1); }
HLE(GetWindowThreadProcessId) { if (ARG(1)) WR32(ARG(1), 1); RETURN(2, g_window_tid ? g_window_tid : c->thread_id); }
#define E(n) { #n, n }
const HleEntry hle_user32[] = {
    E(LoadCursorA), E(UnregisterClassA), E(GetWindowThreadProcessId), E(RegisterClassA), E(CreateWindowExA), E(ShowWindow), E(DestroyWindow), E(DefWindowProcA), E(GetWindowLongA),
    E(SetWindowPos), E(AdjustWindowRect), E(GetClientRect), E(ClientToScreen), E(GetDesktopWindow), E(GetWindowDC),
    E(BeginPaint), E(EndPaint), E(IsIconic), E(SetActiveWindow), E(SetForegroundWindow), E(FindWindowA),
    E(LoadIconA), E(SetCursor), E(ShowCursor), E(SetCursorPos), E(SwapMouseButton), E(GetSystemMetrics),
    E(SystemParametersInfoA), E(SetWindowsHookExA), E(UnhookWindowsHookEx), E(CallNextHookEx), E(MessageBoxA),
    E(PostMessageA), E(SendNotifyMessageA), E(PostThreadMessageA), E(PostQuitMessage), E(GetMessageA),
    E(TranslateMessage), E(DispatchMessageA), E(GetAsyncKeyState), E(GetKeyState), E(GetKeyboardType),
    E(keybd_event), E(CharUpperA), E(CharUpperBuffA), E(wsprintfA),
    E(DdeInitializeA), E(DdeUninitialize), E(DdeConnect), E(DdeClientTransaction), E(DdeCreateStringHandleA),
    E(DdeGetLastError), E(DdeQueryStringA),
    { NULL, NULL }
};

void hle_user32_init(void) { g_qlock = plat_mutex_new(); }
