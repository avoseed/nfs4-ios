/* GDI32, ADVAPI32, WINMM, OLE32, SHELL32, WSOCK32 replacements (mostly stubs). */
#include "hle.h"
#include <stdlib.h>
#include <string.h>

extern int g_screen_w, g_screen_h;

/* ---------------------------------------------------------------- gdi32 */
HLE(GetDeviceCaps) {
    uint32_t i = ARG(1);
    RETURN(2, i == 8 ? g_screen_w : i == 10 ? g_screen_h : i == 12 ? 16 : i == 14 ? 1 : 0);
}
HLE(GetStockObject) { RETURN(1, 0x00050000 + ARG(0)); }
const HleEntry hle_misc_gdi32[] = { { "GetDeviceCaps", GetDeviceCaps }, { "GetStockObject", GetStockObject }, { NULL, NULL } };

/* ---------------------------------------------------------------- advapi32: empty registry */
HLE(RegOpenKeyExA) {
    plat_log("RegOpenKeyExA(%08X, %s) -> not found", ARG(0), ARG(1) ? STR(ARG(1)) : "");
    RETURN(5, 2);
}
HLE(RegQueryValueExA) { RETURN(6, 2); }
HLE(RegCloseKey) { RETURN(1, 0); }
HLE(RegFlushKey) { RETURN(1, 0); }
const HleEntry hle_misc_advapi32[] = {
    { "RegOpenKeyExA", RegOpenKeyExA }, { "RegQueryValueExA", RegQueryValueExA },
    { "RegCloseKey", RegCloseKey }, { "RegFlushKey", RegFlushKey }, { NULL, NULL } };

/* ---------------------------------------------------------------- winmm */
static uint64_t g_t0;
/* DirectSound owns audio; no Windows hardware mixer is exposed to the guest. */
HLE(mixerGetNumDevs) { plat_log("mixerGetNumDevs -> 0 (DirectSound mixing)"); RETURN(0, 0); }
HLE(timeGetTime) { RETURN(0, (uint32_t)((plat_ticks_us() - g_t0) / 1000)); }
HLE(timeBeginPeriod) { RETURN(1, 0); }
HLE(timeEndPeriod) { RETURN(1, 0); }
HLE(timeGetDevCaps) { WR32(ARG(0), 1); WR32(ARG(0) + 4, 1000000); RETURN(2, 0); }
HLE(PlaySoundA) { RETURN(3, 1); }

/* Like Windows, all multimedia timer callbacks run on one dedicated thread. */
typedef struct { uint32_t id, delay, cb, user, periodic; uint64_t due; int alive; } Timer;
#define MAXTIMERS 64
static Timer g_timers[MAXTIMERS];
static uint32_t g_timer_serial;
static PlatMutex *g_tlock;
static PlatEvent *g_tev;

static void timer_thread(void *p) {
    (void)p;
    Cpu *c = rt_new_cpu(0x100000);
    rt_bind_cpu(c);
    for (;;) {
        uint64_t now = plat_ticks_us(), next = now + 50000;
        Timer run = { 0 };
        int found = 0;
        plat_mutex_lock(g_tlock);
        for (int i = 0; i < MAXTIMERS; i++) {
            Timer *t = &g_timers[i];
            if (!t->alive) continue;
            if (t->due <= now && !found) {
                run = *t;
                found = 1;
                if (t->periodic) {
                    t->due += (uint64_t)t->delay * 1000;
                    if (t->due + 100000 < now) t->due = now;   /* don't spiral after a stall */
                } else {
                    t->alive = 0;
                }
            } else if (t->due < next) {
                next = t->due;
            }
        }
        plat_mutex_unlock(g_tlock);
        if (found) {
            uint32_t a[5] = { run.id, 0, run.user, 0, 0 };
            rt_guest_call(c, run.cb, 5, a);
            continue;
        }
        uint64_t wait = next > now ? (next - now) / 1000 : 0;
        plat_event_wait(g_tev, wait ? (uint32_t)wait : 1);
    }
}

HLE(timeSetEvent) {
    plat_mutex_lock(g_tlock);
    for (int i = 0; i < MAXTIMERS; i++) {
        Timer *t = &g_timers[i];
        if (!t->alive) {
            g_timer_serial = (g_timer_serial + 1) & 0xFFFF;
            t->id = ((uint32_t)i + 1) | (g_timer_serial << 8);
            t->delay = ARG(0) ? ARG(0) : 1; t->cb = ARG(2); t->user = ARG(3); t->periodic = ARG(4) & 1;
            t->due = plat_ticks_us() + (uint64_t)t->delay * 1000;
            t->alive = 1;
            uint32_t id = t->id;
            if (t->periodic) plat_log("timeSetEvent(%u ms, cb=%08X, periodic)", t->delay, t->cb);
            plat_mutex_unlock(g_tlock);
            plat_event_set(g_tev);
            RETURN(5, id);
        }
    }
    plat_mutex_unlock(g_tlock);
    RETURN(5, 0);
}
HLE(timeKillEvent) {
    plat_mutex_lock(g_tlock);
    for (int i = 0; i < MAXTIMERS; i++) if (g_timers[i].alive && g_timers[i].id == ARG(0)) g_timers[i].alive = 0;
    plat_mutex_unlock(g_tlock);
    RETURN(1, 0);
}
const HleEntry hle_misc_winmm[] = {
    { "mixerGetNumDevs", mixerGetNumDevs }, { "timeGetTime", timeGetTime }, { "timeBeginPeriod", timeBeginPeriod }, { "timeEndPeriod", timeEndPeriod },
    { "timeGetDevCaps", timeGetDevCaps }, { "PlaySoundA", PlaySoundA }, { "timeSetEvent", timeSetEvent },
    { "timeKillEvent", timeKillEvent }, { NULL, NULL } };

/* ---------------------------------------------------------------- ole32 / shell32 */
HLE(CoCreateGuid) {
    for (int i = 0; i < 16; i++) MEM[ARG(0) + i] = (uint8_t)rand();
    RETURN(1, 0);
}
const HleEntry hle_misc_ole32[] = { { "CoCreateGuid", CoCreateGuid }, { NULL, NULL } };
HLE(ShellExecuteA) { plat_log("ShellExecuteA(%s) ignored", ARG(2) ? STR(ARG(2)) : ""); RETURN(6, 2); }
const HleEntry hle_misc_shell32[] = { { "ShellExecuteA", ShellExecuteA }, { NULL, NULL } };

/* ---------------------------------------------------------------- wsock32: networking unavailable */
/* (names are prefixed: several of these are macros or libc functions on POSIX hosts) */
HLE(ws_WSAStartup) { RETURN(2, 10091); }
HLE(ws_WSACleanup) { RETURN(0, 0); }
HLE(ws_WSAGetLastError) { RETURN(0, 10091); }
#define SOCKFAIL(name, n) HLE(ws_##name) { RETURN(n, 0xFFFFFFFF); }
SOCKFAIL(socket, 3) SOCKFAIL(bind, 3) SOCKFAIL(listen, 2) SOCKFAIL(accept, 3) SOCKFAIL(connect, 3)
SOCKFAIL(send, 4) SOCKFAIL(recv, 4) SOCKFAIL(sendto, 6) SOCKFAIL(recvfrom, 6) SOCKFAIL(select, 5)
SOCKFAIL(closesocket, 1) SOCKFAIL(shutdown, 2) SOCKFAIL(ioctlsocket, 3) SOCKFAIL(setsockopt, 5)
SOCKFAIL(getsockname, 3) SOCKFAIL(gethostname, 2)
HLE(ws_gethostbyname) { RETURN(1, 0); }
HLE(ws_inet_addr) { RETURN(1, 0xFFFFFFFF); }
HLE(ws_inet_ntoa) { static uint32_t s; if (!s) s = guest_strdup("0.0.0.0"); RETURN(1, s); }
HLE(ws_htons) { uint32_t v = ARG(0) & 0xFFFF; RETURN(1, ((v >> 8) | (v << 8)) & 0xFFFF); }
HLE(ws_ntohs) { uint32_t v = ARG(0) & 0xFFFF; RETURN(1, ((v >> 8) | (v << 8)) & 0xFFFF); }
HLE(ws___WSAFDIsSet) { RETURN(2, 0); }
#define S(n) { #n, ws_##n }
const HleEntry hle_misc_wsock32[] = {
    S(WSAStartup), S(WSACleanup), S(WSAGetLastError), S(socket), S(bind), S(listen), S(accept), S(connect),
    S(send), S(recv), S(sendto), S(recvfrom), S(select), S(closesocket), S(shutdown), S(ioctlsocket),
    S(setsockopt), S(getsockname), S(gethostname), S(gethostbyname), S(inet_addr), S(inet_ntoa), S(htons),
    S(ntohs), S(__WSAFDIsSet), { NULL, NULL } };

void hle_misc_init(void) {
    g_t0 = plat_ticks_us();
    g_tlock = plat_mutex_new();
    g_tev = plat_event_new(0, 0);
    plat_thread_start(timer_thread, NULL);
}
