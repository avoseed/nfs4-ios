/* Shared declarations for the Win32 replacement (HLE) layer. */
#pragma once
#include "rt_host.h"
#include "plat.h"
#include <stdio.h>

typedef struct { const char *name; HleFn fn; } HleEntry;
typedef struct { const char *dll; const HleEntry *entries; } HleDll;

#define HLE(name) static void name(Cpu *c)

/* object handles */
enum { H_FREE, H_FILE, H_EVENT, H_MUTEX, H_THREAD, H_FIND, H_MAPPING, H_PROCESS, H_MISC };
typedef struct HObj {
    int type;
    FILE *file;
    PlatEvent *event;
    PlatMutex *mutex;
    uint32_t owner, count;      /* mutex owner thread / recursion */
    Cpu *thread;
    uint32_t value;             /* misc: mapping address/size, find index ... */
    uint32_t size;
    char *path;
    void *extra;
} HObj;

uint32_t handle_new(int type);
HObj *handle_get(uint32_t h, int type);
void handle_free(uint32_t h);

void set_last_error(Cpu *c, uint32_t e);

/* path mapping: guest path -> host path for reading / writing */
void map_path(const char *guest, char *host, size_t n, int for_write);
/* the same guest path inside one particular root (game, mods or save folder) */
void map_path_in(const char *root, const char *guest, char *host, size_t n);
const char *guest_cwd(void);

extern const HleEntry hle_kernel32[];
extern const HleEntry hle_user32[];
extern const HleEntry hle_misc_gdi32[];
extern const HleEntry hle_misc_advapi32[];
extern const HleEntry hle_misc_winmm[];
extern const HleEntry hle_misc_ole32[];
extern const HleEntry hle_misc_shell32[];
extern const HleEntry hle_misc_wsock32[];
extern const HleEntry hle_dinput[];
extern const HleEntry hle_glide[];
extern const HleEntry hle_dsound[];

extern const HleEntry hle_ddraw[];

extern const HleEntry hle_kernel32_crt[];
