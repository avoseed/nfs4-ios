/* Windows development host: logs to nfs4host.log, shows frames in a GDI window. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <direct.h>
#include <io.h>
#include "../plat.h"
#include "../power_policy.h"
#include "../rt_host.h"

static FILE *g_log;
static CRITICAL_SECTION g_log_cs;
static char g_game[1024], g_save[1024];
volatile uint8_t plat_keys[256];
void hle_key_event(uint8_t sc, int down);

void plat_log(const char *fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    EnterCriticalSection(&g_log_cs);
    fprintf(stderr, "%s\n", buf);
    if (g_log) { fprintf(g_log, "%s\n", buf); fflush(g_log); }
    LeaveCriticalSection(&g_log_cs);
}

void plat_fatal(const char *fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    plat_log("FATAL: %s", buf);
    exit(3);
}

uint64_t plat_ticks_us(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (uint64_t)(t.QuadPart * 1000000.0 / f.QuadPart);
}
void plat_sleep_ms(uint32_t ms) { Sleep(ms); }
void plat_sleep_us(uint32_t us) { Sleep((us + 500) / 1000); }
void *plat_reserve(size_t size) { return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); }

struct PlatThread { HANDLE h; };
typedef struct { void (*fn)(void *); void *arg; } Tramp;
static DWORD WINAPI tramp(LPVOID p) { Tramp t = *(Tramp *)p; free(p); t.fn(t.arg); return 0; }
PlatThread *plat_thread_start(void (*fn)(void *), void *arg) {
    Tramp *t = malloc(sizeof *t);
    t->fn = fn; t->arg = arg;
    PlatThread *pt = malloc(sizeof *pt);
    pt->h = CreateThread(NULL, 1 << 20, tramp, t, 0, NULL);
    return pt;
}
uint32_t plat_thread_id(void) { return GetCurrentThreadId(); }

struct PlatMutex { CRITICAL_SECTION cs; };
PlatMutex *plat_mutex_new(void) { PlatMutex *m = malloc(sizeof *m); InitializeCriticalSection(&m->cs); return m; }
void plat_mutex_lock(PlatMutex *m) { EnterCriticalSection(&m->cs); }
void plat_mutex_unlock(PlatMutex *m) { LeaveCriticalSection(&m->cs); }

struct PlatCond { CONDITION_VARIABLE cv; };
PlatCond *plat_cond_new(void) { PlatCond *c = malloc(sizeof *c); InitializeConditionVariable(&c->cv); return c; }
int plat_cond_wait(PlatCond *c, PlatMutex *m, uint32_t ms) {
    return SleepConditionVariableCS(&c->cv, &m->cs, ms == 0xFFFFFFFFu ? INFINITE : ms) ? 1 : 0;
}
void plat_cond_broadcast(PlatCond *c) { WakeAllConditionVariable(&c->cv); }

struct PlatEvent { HANDLE h; };
PlatEvent *plat_event_new(int manual, int initial) {
    PlatEvent *e = malloc(sizeof *e);
    e->h = CreateEventA(NULL, manual, initial, NULL);
    return e;
}
void plat_event_set(PlatEvent *e) { SetEvent(e->h); }
void plat_event_reset(PlatEvent *e) { ResetEvent(e->h); }
int plat_event_wait(PlatEvent *e, uint32_t ms) { return WaitForSingleObject(e->h, ms) == WAIT_OBJECT_0; }

const char *plat_game_dir(void) { return g_game; }
const char *plat_save_dir(void) { return g_save; }
const char *plat_mods_dir(void) { return getenv("NFS4_MODS"); }
int plat_mkdir(const char *p) { return _mkdir(p); }
int plat_truncate(FILE *f, long size) { return _chsize(_fileno(f), size); }

int plat_list_dir(const char *path, void (*fn)(const char *, int, uint32_t, void *), void *ctx) {
    char pat[1200];
    snprintf(pat, sizeof pat, "%s/*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        fn(fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, fd.nFileSizeLow, ctx);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 1;
}

void plat_local_time(uint16_t out[8]) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    out[0] = st.wYear; out[1] = st.wMonth; out[2] = st.wDayOfWeek; out[3] = st.wDay;
    out[4] = st.wHour; out[5] = st.wMinute; out[6] = st.wSecond; out[7] = st.wMilliseconds;
}

/* ---------------------------------------------------------------- display window */

static HWND g_hwnd;
static CRITICAL_SECTION g_frame_cs;
static uint32_t *g_frame;
static int g_fw, g_fh;
static volatile int g_closed;
static int g_dumped;

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        EnterCriticalSection(&g_frame_cs);
        if (g_frame) {
            BITMAPINFO bi = { 0 };
            bi.bmiHeader.biSize = sizeof bi.bmiHeader;
            bi.bmiHeader.biWidth = g_fw; bi.bmiHeader.biHeight = -g_fh;
            bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
            RECT r; GetClientRect(h, &r);
            StretchDIBits(dc, 0, 0, r.right, r.bottom, 0, 0, g_fw, g_fh, g_frame, &bi, DIB_RGB_COLORS, SRCCOPY);
        }
        LeaveCriticalSection(&g_frame_cs);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: {
        uint32_t sc = (l >> 16) & 0x7F;
        if (l & (1 << 24)) sc |= 0x80;
        int down = (m == WM_KEYDOWN || m == WM_SYSKEYDOWN);
        if (!(down && plat_keys[sc])) hle_key_event((uint8_t)sc, down);   /* no auto-repeat duplicates */
        plat_keys[sc] = down ? 0x80 : 0;
        return 0;
    }
    case WM_MOUSEMOVE: {
        static int lx = -1, ly;
        RECT r; GetClientRect(h, &r);
        int x = (short)LOWORD(l) * 640 / (r.right ? r.right : 1), y = (short)HIWORD(l) * 480 / (r.bottom ? r.bottom : 1);
        if (lx >= 0) { plat_mouse_dx += x - lx; plat_mouse_dy += y - ly; }
        lx = x; ly = y;
        return 0;
    }
    case WM_LBUTTONDOWN: plat_mouse_buttons = 0x80; return 0;
    case WM_LBUTTONUP: plat_mouse_buttons = 0; return 0;
    case WM_CLOSE: g_closed = 1; DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static DWORD WINAPI ui_thread(LPVOID p) {
    (void)p;
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "nfs4host";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    RECT r = { 0, 0, 640 * 3 / 2, 480 * 3 / 2 };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowA("nfs4host", "NFS4 recompiled (dev host)", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    g_closed = 1;
    exit(0);
    return 0;
}

static void dump_frame(const uint32_t *px, int w, int h, const char *name) {
    /* write a BMP so frames can be inspected without the window */
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", g_save, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t sz = 54 + (uint32_t)w * h * 4;
    uint8_t hdr[54] = { 'B', 'M' };
    memcpy(hdr + 2, &sz, 4); hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4); int nh = -h; memcpy(hdr + 22, &nh, 4);
    hdr[26] = 1; hdr[28] = 32;
    fwrite(hdr, 1, 54, f);
    fwrite(px, 4, (size_t)w * h, f);
    fclose(f);
}

void plat_present(const uint32_t *rgba, int w, int h) {
    EnterCriticalSection(&g_frame_cs);
    if (w != g_fw || h != g_fh) { free(g_frame); g_frame = malloc((size_t)w * h * 4); g_fw = w; g_fh = h; }
    /* RGBA -> BGRA for GDI */
    for (int i = 0; i < w * h; i++) {
        uint32_t v = rgba[i];
        g_frame[i] = (v & 0xFF00FF00u) | ((v & 0xFF) << 16) | ((v >> 16) & 0xFF);
    }
    LeaveCriticalSection(&g_frame_cs);
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
    static int count;
    if (++count % 120 == 0 || !g_dumped) { dump_frame(g_frame, w, h, "frame.bmp"); g_dumped = 1; }
}

int plat_pump(void) { return !g_closed; }
float plat_wide_aspect(void) { const char *e = getenv("NFS4_WIDE"); return e ? (float)atof(e) : 0; }
int plat_frame_cap(void) { const char *e = getenv("NFS4_FPS"); return nfs4_scene_cap(e ? atoi(e) : 60,hle_glide_in_menu()); }

/* Scripted input for unattended testing: NFS4_KEYS="3:1C,5.5:D0,..." presses DIK scan code
 * (hex) at t seconds for 150 ms. NFS4_SHOTS=<seconds> saves shot_NNN.bmp at that interval. */
static void key_script(void *p) {
    const char *s = p;
    uint64_t t0 = plat_ticks_us();
    while (*s) {
        double t = atof(s);
        const char *colon = strchr(s, ':');
        if (!colon) break;
        uint64_t due = t0 + (uint64_t)(t * 1e6);
        while (plat_ticks_us() < due) Sleep(5);
        if (colon[1] == 'j') {                     /* jx<v>/<secs>, jy<v>/<secs>, jb<mask>/<secs>: joystick, then rest */
            double hold = 0.5;
            const char *slash = strchr(colon, '/'), *cm0 = strchr(colon, ',');
            if (slash && (!cm0 || slash < cm0)) hold = atof(slash + 1);
            char axis = colon[2];
            float v = (float)atof(colon + 3);
            if (axis == 'x') plat_joy_x = v; else if (axis == 'y') plat_joy_y = v; else plat_joy_buttons = (uint32_t)v;
            plat_log("script: joystick %c %.2f for %.1f s", axis, v, hold);
            Sleep((DWORD)(hold * 1000));
            if (axis == 'x') plat_joy_x = 0; else if (axis == 'y') plat_joy_y = 0; else plat_joy_buttons = 0;
            const char *cm = strchr(colon, ',');
            if (!cm) break;
            s = cm + 1;
            continue;
        }
        if (colon[1] == 'c') {                     /* c<x>x<y>: click at game coordinates */
            int x = atoi(colon + 2), y = atoi(strchr(colon + 2, 'x') + 1);
            plat_log("script: click %d,%d", x, y);
            plat_mouse_click_at(x, y);
            const char *cm = strchr(colon, ',');
            if (!cm) break;
            s = cm + 1;
            continue;
        }
        unsigned sc = (unsigned)strtoul(colon + 1, NULL, 16);
        plat_log("script: key %02X", sc);
        double hold = 0.15;
        const char *slash = strchr(colon, '/');
        const char *comma0 = strchr(colon, ',');
        if (slash && (!comma0 || slash < comma0)) hold = atof(slash + 1);
        plat_keys[sc & 0xFF] = 0x80;
        hle_key_event((uint8_t)sc, 1);
        Sleep((DWORD)(hold * 1000));
        plat_keys[sc & 0xFF] = 0;
        hle_key_event((uint8_t)sc, 0);
        const char *comma = strchr(colon, ',');
        if (!comma) break;
        s = comma + 1;
    }
}

/* NFS4_WAV=<seconds>: record the DirectSound mix to save/audio.wav (no live playback on the dev host) */
void hle_audio_mix(float *out, int frames, int rate);
static void wav_thread(void *p) {
    int secs = atoi((const char *)p);
    char path[1200];
    snprintf(path, sizeof path, "%s/audio.wav", g_save);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t total = (uint32_t)secs * 44100, n = 0;
    uint8_t hdr[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 1,0, 2,0,
                        0x44,0xAC,0,0, 0x10,0xB1,2,0, 4,0, 16,0, 'd','a','t','a', 0,0,0,0 };
    fwrite(hdr, 1, 44, f);
    float buf[2 * 441];
    int16_t pcm[2 * 441];
    uint64_t next = plat_ticks_us();
    while (n < total) {
        next += 10000;
        while (plat_ticks_us() < next) Sleep(1);
        hle_audio_mix(buf, 441, 44100);
        for (int i = 0; i < 882; i++) { float v = buf[i] * 32767.0f; pcm[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
        fwrite(pcm, 2, 882, f);
        n += 441;
    }
    uint32_t data = n * 4, riff = data + 36;
    fseek(f, 4, SEEK_SET); fwrite(&riff, 4, 1, f);
    fseek(f, 40, SEEK_SET); fwrite(&data, 4, 1, f);
    fclose(f);
    plat_log("wrote %s", path);
}

static void shot_thread(void *p) {
    double every = atof((const char *)p);
    for (int n = 0;; n++) {
        Sleep((DWORD)(every * 1000));
        char name[64];
        snprintf(name, sizeof name, "shot_%03d.bmp", n);
        EnterCriticalSection(&g_frame_cs);
        if (g_frame) dump_frame(g_frame, g_fw, g_fh, name);
        LeaveCriticalSection(&g_frame_cs);
    }
}

int main(int argc, char **argv) {
    timeBeginPeriod(1);              /* 1 ms waits, like the game expects from its multimedia timers */
    InitializeCriticalSection(&g_log_cs);
    InitializeCriticalSection(&g_frame_cs);
    const char *game = argc > 1 ? argv[1] : getenv("NFS4_GAME_DIR");
    if (!game) { fprintf(stderr, "usage: nfs4host <game folder> [save folder]\n"); return 2; }
    snprintf(g_game, sizeof g_game, "%s", game);
    snprintf(g_save, sizeof g_save, "%s", argc > 2 ? argv[2] : "save");
    _mkdir(g_save);
    char logp[1200];
    snprintf(logp, sizeof logp, "%s/nfs4host.log", g_save);
    g_log = fopen(logp, "w");
    CreateThread(NULL, 0, ui_thread, NULL, 0, NULL);
    plat_log("game folder: %s, save folder: %s", g_game, g_save);
    if (getenv("NFS4_KEYS")) plat_thread_start(key_script, getenv("NFS4_KEYS"));
    if (getenv("NFS4_SHOTS")) plat_thread_start(shot_thread, getenv("NFS4_SHOTS"));
    if (getenv("NFS4_WAV")) plat_thread_start(wav_thread, getenv("NFS4_WAV"));
    return rt_boot() ? 0 : 1;
}
