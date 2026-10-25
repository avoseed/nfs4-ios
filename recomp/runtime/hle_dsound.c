/* DSOUND replacement. Buffers live in guest memory; play cursors advance in real time.
 * Mixing to a real audio device is done by hle_audio_mix() (called from the host audio callback). */
#include "hle.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define DS_OK 0
#define E_NOINTERFACE 0x80004002u
#define DSBCAPS_PRIMARYBUFFER 0x1
#define DSBPLAY_LOOPING 0x1

typedef struct {
    int used, primary, playing, looping;
    uint32_t obj, data, size;          /* guest object and sample memory */
    uint32_t rate, channels, bits, block;
    double pos;                        /* play cursor in bytes (fractional) */
    uint64_t last_us;
    int32_t volume, pan;               /* hundredths of dB */
    uint32_t freq;
} DsBuf;

#define MAXBUF 64
static DsBuf g_buf[MAXBUF];
static PlatMutex *g_lock;
static uint32_t g_ds_vtbl, g_buf_vtbl;

static DsBuf *buf_of(uint32_t obj) {
    uint32_t i = RD32(obj + 8);
    return i < MAXBUF && g_buf[i].used && g_buf[i].obj == obj ? &g_buf[i] : NULL;
}

/* advance the play cursor to "now" */
static void advance(DsBuf *b) {
    uint64_t now = plat_ticks_us();
    if (b->playing && b->block && b->size) {
        double bytes_per_sec = (double)(b->freq ? b->freq : b->rate) * b->block;
        b->pos += bytes_per_sec * (double)(now - b->last_us) / 1e6;
        if (b->pos >= b->size) {
            if (b->looping) b->pos = fmod(b->pos, (double)b->size);
            else { b->pos = 0; b->playing = 0; }
        }
    }
    b->last_us = now;
}

static uint32_t make_vtable(const char *iface, const HleEntry *m, int n) {
    uint32_t vt = guest_alloc(4 * (uint32_t)n);
    for (int i = 0; i < n; i++) {
        char name[96];
        snprintf(name, sizeof name, "%s::%s", iface, m[i].name);
        WR32(vt + 4 * i, rt_hle_address("dsound.dll", strdup(name), m[i].fn));
    }
    return vt;
}

/* ---------------------------------------------------------------- IUnknown */
HLE(QueryInterface) { if (ARG(2)) WR32(ARG(2), 0); RETURN(3, E_NOINTERFACE); }
HLE(AddRef) { uint32_t n = RD32(ARG(0) + 4) + 1; WR32(ARG(0) + 4, n); RETURN(1, n); }
HLE(Release) {
    uint32_t n = RD32(ARG(0) + 4);
    if (n) n--;
    WR32(ARG(0) + 4, n);
    if (!n && RD32(ARG(0)) == g_buf_vtbl) {
        plat_mutex_lock(g_lock);
        DsBuf *b = buf_of(ARG(0));
        if (b) { b->used = 0; if (b->data) guest_free(b->data); }
        plat_mutex_unlock(g_lock);
    }
    RETURN(1, n);
}

/* ---------------------------------------------------------------- IDirectSound */
HLE(DS_CreateSoundBuffer) {
    uint32_t desc = ARG(1), out = ARG(2);
    uint32_t flags = RD32(desc + 4), bytes = RD32(desc + 8), wfx = RD32(desc + 16);
    plat_mutex_lock(g_lock);
    int k = -1;
    for (int i = 0; i < MAXBUF; i++) if (!g_buf[i].used) { k = i; break; }
    if (k < 0) { plat_mutex_unlock(g_lock); RETURN(4, 0x8007000Eu); }
    DsBuf *b = &g_buf[k];
    memset(b, 0, sizeof *b);
    b->used = 1;
    b->primary = (flags & DSBCAPS_PRIMARYBUFFER) != 0;
    b->rate = 22050; b->channels = 2; b->bits = 16;
    if (wfx) {
        b->channels = RD16(wfx + 2); b->rate = RD32(wfx + 4); b->bits = RD16(wfx + 14);
    }
    b->block = b->channels * (b->bits / 8);
    b->size = b->primary ? 0x10000 : bytes;
    b->data = guest_alloc(b->size ? b->size : 4);
    b->last_us = plat_ticks_us();
    b->obj = guest_alloc(16);
    WR32(b->obj, g_buf_vtbl); WR32(b->obj + 4, 1); WR32(b->obj + 8, (uint32_t)k);
    plat_mutex_unlock(g_lock);
    WR32(out, b->obj);
    plat_log("dsound: CreateSoundBuffer(%s, %u bytes, %u Hz %u ch %u bit)", b->primary ? "primary" : "secondary",
             b->size, b->rate, b->channels, b->bits);
    RETURN(4, DS_OK);
}
HLE(DS_GetCaps) {
    uint32_t p = ARG(1);
    uint32_t sz = RD32(p);
    memset(MEM + p + 4, 0, sz > 4 ? sz - 4 : 0);
    WR32(p + 4, 0x0F5F);                                /* mono/stereo 8/16 primary+secondary */
    WR32(p + 8, 100); WR32(p + 12, 100000);             /* min/max secondary rate */
    WR32(p + 20, 32); WR32(p + 24, 32); WR32(p + 28, 32);
    RETURN(2, DS_OK);
}
HLE(DS_DuplicateSoundBuffer) { WR32(ARG(2), 0); RETURN(3, 0x80004001u); }
HLE(DS_SetCooperativeLevel) { RETURN(3, DS_OK); }
HLE(DS_Compact) { RETURN(1, DS_OK); }
HLE(DS_GetSpeakerConfig) { WR32(ARG(1), 4); RETURN(2, DS_OK); }
HLE(DS_SetSpeakerConfig) { RETURN(2, DS_OK); }
HLE(DS_Initialize) { RETURN(2, DS_OK); }

static const HleEntry ds_methods[] = {
    { "QueryInterface", QueryInterface }, { "AddRef", AddRef }, { "Release", Release },
    { "CreateSoundBuffer", DS_CreateSoundBuffer }, { "GetCaps", DS_GetCaps },
    { "DuplicateSoundBuffer", DS_DuplicateSoundBuffer }, { "SetCooperativeLevel", DS_SetCooperativeLevel },
    { "Compact", DS_Compact }, { "GetSpeakerConfig", DS_GetSpeakerConfig },
    { "SetSpeakerConfig", DS_SetSpeakerConfig }, { "Initialize", DS_Initialize },
};

/* ---------------------------------------------------------------- IDirectSoundBuffer */
HLE(B_GetCaps) {
    DsBuf *b = buf_of(ARG(0));
    uint32_t p = ARG(1);
    WR32(p + 4, b && b->primary ? 1 : 0x8); WR32(p + 8, b ? b->size : 0);
    RETURN(2, DS_OK);
}
HLE(B_GetCurrentPosition) {
    plat_mutex_lock(g_lock);
    DsBuf *b = buf_of(ARG(0));
    uint32_t play = 0, write = 0;
    if (b) {
        advance(b);
        play = ((uint32_t)b->pos) / (b->block ? b->block : 1) * (b->block ? b->block : 1);
        write = b->size ? (play + b->block * (b->rate / 50)) % b->size : 0;   /* ~20 ms ahead */
    }
    plat_mutex_unlock(g_lock);
    if (ARG(1)) WR32(ARG(1), play);
    if (ARG(2)) WR32(ARG(2), write);
    RETURN(3, DS_OK);
}
HLE(B_GetFormat) {
    DsBuf *b = buf_of(ARG(0));
    uint32_t w = ARG(1);
    if (w && b) {
        WR16(w, 1); WR16(w + 2, b->channels); WR32(w + 4, b->rate); WR32(w + 8, b->rate * b->block);
        WR16(w + 12, b->block); WR16(w + 14, b->bits); if (ARG(2) >= 18) WR16(w + 16, 0);
    }
    if (ARG(3)) WR32(ARG(3), 18);
    RETURN(4, DS_OK);
}
HLE(B_GetVolume) { DsBuf *b = buf_of(ARG(0)); WR32(ARG(1), b ? (uint32_t)b->volume : 0); RETURN(2, DS_OK); }
HLE(B_GetPan) { DsBuf *b = buf_of(ARG(0)); WR32(ARG(1), b ? (uint32_t)b->pan : 0); RETURN(2, DS_OK); }
HLE(B_GetFrequency) { DsBuf *b = buf_of(ARG(0)); WR32(ARG(1), b ? (b->freq ? b->freq : b->rate) : 22050); RETURN(2, DS_OK); }
HLE(B_GetStatus) {
    plat_mutex_lock(g_lock);
    DsBuf *b = buf_of(ARG(0));
    uint32_t s = 0;
    if (b) { advance(b); s = (b->playing ? 1 : 0) | (b->looping ? 4 : 0); }
    plat_mutex_unlock(g_lock);
    WR32(ARG(1), s);
    RETURN(2, DS_OK);
}
HLE(B_Initialize) { RETURN(3, DS_OK); }
HLE(B_Lock) {
    DsBuf *b = buf_of(ARG(0));
    uint32_t off = ARG(1), n = ARG(2), flags = ARG(7);
    if (!b) RETURN(8, 0x80070057u);
    if (flags & 2) { off = 0; n = b->size; }            /* DSBLOCK_ENTIREBUFFER */
    if (flags & 1) { plat_mutex_lock(g_lock); advance(b); off = ((uint32_t)b->pos + b->block * (b->rate / 50)) % b->size; plat_mutex_unlock(g_lock); }
    if (n > b->size) n = b->size;
    off %= b->size ? b->size : 1;
    uint32_t first = n, second = 0;
    if (off + n > b->size) { first = b->size - off; second = n - first; }
    WR32(ARG(3), b->data + off); WR32(ARG(4), first);
    if (ARG(5)) WR32(ARG(5), second ? b->data : 0);
    if (ARG(6)) WR32(ARG(6), second);
    RETURN(8, DS_OK);
}
HLE(B_Play) {
    plat_mutex_lock(g_lock);
    DsBuf *b = buf_of(ARG(0));
    if (b) { advance(b); b->playing = 1; b->looping = (ARG(3) & DSBPLAY_LOOPING) != 0; b->last_us = plat_ticks_us(); }
    plat_mutex_unlock(g_lock);
    RETURN(4, DS_OK);
}
HLE(B_SetCurrentPosition) {
    plat_mutex_lock(g_lock);
    DsBuf *b = buf_of(ARG(0));
    if (b) { b->pos = ARG(1) % (b->size ? b->size : 1); b->last_us = plat_ticks_us(); }
    plat_mutex_unlock(g_lock);
    RETURN(2, DS_OK);
}
HLE(B_SetFormat) {
    DsBuf *b = buf_of(ARG(0));
    uint32_t w = ARG(1);
    if (b && w) {
        b->channels = RD16(w + 2); b->rate = RD32(w + 4); b->bits = RD16(w + 14);
        b->block = b->channels * (b->bits / 8);
        plat_log("dsound: primary format %u Hz %u ch %u bit", b->rate, b->channels, b->bits);
    }
    RETURN(2, DS_OK);
}
HLE(B_SetVolume) { DsBuf *b = buf_of(ARG(0)); if (b) b->volume = (int32_t)ARG(1); RETURN(2, DS_OK); }
HLE(B_SetPan) { DsBuf *b = buf_of(ARG(0)); if (b) b->pan = (int32_t)ARG(1); RETURN(2, DS_OK); }
HLE(B_SetFrequency) { DsBuf *b = buf_of(ARG(0)); if (b) { plat_mutex_lock(g_lock); advance(b); b->freq = ARG(1); plat_mutex_unlock(g_lock); } RETURN(2, DS_OK); }
HLE(B_Stop) {
    plat_mutex_lock(g_lock);
    DsBuf *b = buf_of(ARG(0));
    if (b) { advance(b); b->playing = 0; }
    plat_mutex_unlock(g_lock);
    RETURN(1, DS_OK);
}
HLE(B_Unlock) { RETURN(5, DS_OK); }
HLE(B_Restore) { RETURN(1, DS_OK); }

static const HleEntry buf_methods[] = {
    { "QueryInterface", QueryInterface }, { "AddRef", AddRef }, { "Release", Release },
    { "GetCaps", B_GetCaps }, { "GetCurrentPosition", B_GetCurrentPosition }, { "GetFormat", B_GetFormat },
    { "GetVolume", B_GetVolume }, { "GetPan", B_GetPan }, { "GetFrequency", B_GetFrequency },
    { "GetStatus", B_GetStatus }, { "Initialize", B_Initialize }, { "Lock", B_Lock }, { "Play", B_Play },
    { "SetCurrentPosition", B_SetCurrentPosition }, { "SetFormat", B_SetFormat }, { "SetVolume", B_SetVolume },
    { "SetPan", B_SetPan }, { "SetFrequency", B_SetFrequency }, { "Stop", B_Stop }, { "Unlock", B_Unlock },
    { "Restore", B_Restore },
};

HLE(DirectSoundCreate) {
    if (!g_lock) g_lock = plat_mutex_new();
    if (!g_ds_vtbl) {
        g_ds_vtbl = make_vtable("IDirectSound", ds_methods, (int)(sizeof ds_methods / sizeof ds_methods[0]));
        g_buf_vtbl = make_vtable("IDirectSoundBuffer", buf_methods, (int)(sizeof buf_methods / sizeof buf_methods[0]));
    }
    uint32_t o = guest_alloc(16);
    WR32(o, g_ds_vtbl); WR32(o + 4, 1);
    WR32(ARG(1), o);
    plat_log("DirectSoundCreate -> %08X", o);
    RETURN(3, DS_OK);
}

/* Mix every playing secondary buffer into interleaved stereo float (host audio thread). */
void hle_audio_mix(float *out, int frames, int out_rate) {
    memset(out, 0, sizeof(float) * 2 * (size_t)frames);
    if (!g_lock) return;
    plat_mutex_lock(g_lock);
    for (int i = 0; i < MAXBUF; i++) {
        DsBuf *b = &g_buf[i];
        if (!b->used || b->primary || !b->playing || !b->block || !b->size) continue;
        float vol = powf(10.0f, (float)b->volume / 2000.0f);
        float pan_l = b->pan > 0 ? powf(10.0f, (float)-b->pan / 2000.0f) : 1.0f;
        float pan_r = b->pan < 0 ? powf(10.0f, (float)b->pan / 2000.0f) : 1.0f;
        double step = (double)(b->freq ? b->freq : b->rate) / out_rate * b->block;
        double pos = b->pos;
        for (int f = 0; f < frames; f++) {
            uint32_t p = ((uint32_t)pos / b->block) * b->block;
            float l, r;
            if (b->bits == 16) {
                l = (int16_t)RD16(b->data + p) / 32768.0f;
                r = b->channels == 2 ? (int16_t)RD16(b->data + p + 2) / 32768.0f : l;
            } else {
                l = (RD8(b->data + p) - 128) / 128.0f;
                r = b->channels == 2 ? (RD8(b->data + p + 1) - 128) / 128.0f : l;
            }
            out[2 * f] += l * vol * pan_l;
            out[2 * f + 1] += r * vol * pan_r;
            pos += step;
            if (pos >= b->size) {
                if (!b->looping) break;
                pos -= b->size;
            }
        }
    }
    plat_mutex_unlock(g_lock);
}

const HleEntry hle_dsound[] = { { "DirectSoundCreate", DirectSoundCreate }, { "#1", DirectSoundCreate }, { NULL, NULL } };
