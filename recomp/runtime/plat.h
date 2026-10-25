/* Host platform interface (Windows dev host, iOS). */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

void plat_log(const char *fmt, ...);
void plat_fatal(const char *fmt, ...);
uint64_t plat_ticks_us(void);           /* monotonic microseconds */
void plat_sleep_ms(uint32_t ms);
void plat_sleep_us(uint32_t us);
void *plat_reserve(size_t size);         /* zero-filled, read/write */

typedef struct PlatThread PlatThread;
typedef struct PlatMutex PlatMutex;
typedef struct PlatEvent PlatEvent;
PlatThread *plat_thread_start(void (*fn)(void *), void *arg);
uint32_t plat_thread_id(void);
PlatMutex *plat_mutex_new(void);
void plat_mutex_lock(PlatMutex *m);
void plat_mutex_unlock(PlatMutex *m);
typedef struct PlatCond PlatCond;
PlatCond *plat_cond_new(void);
int plat_cond_wait(PlatCond *c, PlatMutex *m, uint32_t timeout_ms);   /* 0 = timed out */
void plat_cond_broadcast(PlatCond *c);
PlatEvent *plat_event_new(int manual_reset, int initial);
void plat_event_set(PlatEvent *e);
void plat_event_reset(PlatEvent *e);
int plat_event_wait(PlatEvent *e, uint32_t timeout_ms);   /* 1 = signalled, 0 = timeout */

/* Root folder of the original game files (contains nfs4.exe, gamedata, ...). */
const char *plat_game_dir(void);
/* Writable folder for config/saves (may equal the game dir on desktop). */
const char *plat_save_dir(void);
/* Optional folder of replacement game files (e.g. high-resolution car textures), laid out like the
 * game folder; read after the save folder and before the game. NULL when there is none. */
const char *plat_mods_dir(void);
int plat_mkdir(const char *path);
/* Calls fn(name, is_dir, size, ctx) for each entry; returns 0 if the folder does not exist. */
int plat_list_dir(const char *path, void (*fn)(const char *name, int is_dir, uint32_t size, void *ctx), void *ctx);
int plat_truncate(FILE *f, long size);
/* Wall clock for GetLocalTime: fills y, mo, dow, d, h, mi, s, ms */
void plat_local_time(uint16_t out[8]);

/* Aspect ratio (width/height) the race view should fill, or 0 to keep the original 4:3. */
float plat_wide_aspect(void);
/* Highest frame rate the game may render at (0 = uncapped); checked on every buffer swap. */
int plat_frame_cap(void);

/* Video: the host shows a w*h RGBA8 frame (top row first). */
void plat_present(const uint32_t *rgba, int w, int h);
/* Pump host events; returns 0 when the user closed the app. */
int plat_pump(void);
/* Keyboard state for DirectInput/GetAsyncKeyState, indexed by DIK scan code. */
extern volatile uint8_t plat_keys[256];
/* Mouse for DirectInput: hosts add relative motion (game pixels) and set button state.
 * plat_mouse_click_at() moves the game's cursor to an absolute position and clicks
 * (used for touch input: the cursor is first pushed into the top-left corner). */
extern volatile int32_t plat_mouse_dx, plat_mouse_dy;
extern volatile uint8_t plat_mouse_buttons;
void plat_mouse_click_at(int x, int y);
/* Analog joystick for DirectInput ("Touch Wheel"): hosts set the axes (-1 .. 1, x = steering)
 * and the button bits. */
extern volatile float plat_joy_x, plat_joy_y;
extern volatile uint32_t plat_joy_buttons;
