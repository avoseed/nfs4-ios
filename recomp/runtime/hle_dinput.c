/* DINPUT replacement: DirectInput 5 COM objects living in guest memory.
 * Keyboard state comes from plat_keys (DIK scan codes), the mouse from plat_mouse_*, and one
 * joystick ("Touch Wheel") from plat_joy_*: the host's analog steering (touch stick, gamepad). */
#include "hle.h"
#include <string.h>

static const uint8_t GUID_SysKeyboard[16] = { 0x61, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
static const uint8_t GUID_SysMouse[16]    = { 0x60, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
/* an instance GUID of our own for the joystick */
static const uint8_t GUID_TouchWheel[16]  = { 0x4E, 0x46, 0x53, 0x33, 0x57, 0x48, 0x45, 0x45, 0x4C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 };
static const uint8_t GUID_XAxis[16]  = { 0x40, 0x55, 0x4F, 0xA3, 0x8B, 0x56, 0xCF, 0x11, 0x80, 0x95, 0x00, 0xA0, 0xC9, 0x02, 0x06, 0x0E };
static const uint8_t GUID_YAxis[16]  = { 0x41, 0x55, 0x4F, 0xA3, 0x8B, 0x56, 0xCF, 0x11, 0x80, 0x95, 0x00, 0xA0, 0xC9, 0x02, 0x06, 0x0E };
static const uint8_t GUID_Button[16] = { 0xF0, 0xA4, 0xF1, 0xA4, 0xC3, 0x56, 0xCF, 0x11, 0x80, 0x95, 0x00, 0xA0, 0xC9, 0x02, 0x06, 0x0E };
#define JOY_TYPE 0x0204u        /* DIDEVTYPE_JOYSTICK, traditional */
#define JOY_BUTTONS 8

volatile float plat_joy_x, plat_joy_y;           /* -1 .. 1 */
volatile uint32_t plat_joy_buttons;

#define DI_OK 0
#define DIERR_NOTFOUND 0x80070002u
#define E_NOINTERFACE 0x80004002u

/* object layout: +0 vtable, +4 refcount, +8 kind (1 keyboard, 2 mouse, 3 joystick), +12 acquired,
 * +16/+20 joystick axis range (DIPROP_RANGE, default 0..65535) */
static uint32_t g_di_vtbl, g_dev_vtbl;

static uint32_t make_vtable(const char *iface, const HleEntry *methods, int n) {
    uint32_t vt = guest_alloc(4 * (uint32_t)n);
    for (int i = 0; i < n; i++) {
        char name[96];
        snprintf(name, sizeof name, "%s::%s", iface, methods[i].name);
        WR32(vt + 4 * i, rt_hle_address("dinput.dll", strdup(name), methods[i].fn));
    }
    return vt;
}

static uint32_t new_object(uint32_t vtbl, uint32_t kind) {
    uint32_t o = guest_alloc(32);
    WR32(o, vtbl); WR32(o + 4, 1); WR32(o + 8, kind);
    WR32(o + 16, 0); WR32(o + 20, 65535);
    return o;
}

/* ---------------------------------------------------------------- IUnknown */
HLE(QueryInterface) { if (ARG(2)) WR32(ARG(2), ARG(0)); WR32(ARG(0) + 4, RD32(ARG(0) + 4) + 1); RETURN(3, DI_OK); }
HLE(AddRef) { uint32_t n = RD32(ARG(0) + 4) + 1; WR32(ARG(0) + 4, n); RETURN(1, n); }
HLE(Release) { uint32_t n = RD32(ARG(0) + 4); if (n) n--; WR32(ARG(0) + 4, n); RETURN(1, n); }

/* ---------------------------------------------------------------- IDirectInputA */
HLE(DI_CreateDevice) {
    const uint8_t *g = MEM + ARG(1);
    uint32_t kind = !memcmp(g, GUID_SysKeyboard, 16) ? 1 : !memcmp(g, GUID_SysMouse, 16) ? 2
                  : !memcmp(g, GUID_TouchWheel, 16) ? 3 : 0;
    if (!kind) { WR32(ARG(2), 0); plat_log("dinput: CreateDevice(unknown guid)"); RETURN(4, DIERR_NOTFOUND); }
    WR32(ARG(2), new_object(g_dev_vtbl, kind));
    plat_log("dinput: CreateDevice(%s)", kind == 1 ? "keyboard" : kind == 2 ? "mouse" : "joystick");
    RETURN(4, DI_OK);
}

static void fill_instance(uint32_t p, uint32_t size, const uint8_t *guid, uint32_t type, const char *name) {
    memset(MEM + p, 0, size);
    WR32(p, size);
    memcpy(MEM + p + 4, guid, 16);
    memcpy(MEM + p + 20, guid, 16);
    WR32(p + 36, type);
    strcpy((char *)MEM + p + 40, name);
    strcpy((char *)MEM + p + 300, name);
}

HLE(DI_EnumDevices) {
    uint32_t type = ARG(1), cb = ARG(2), ref = ARG(3);
    /* DIDEVTYPE_KEYBOARD = 3, DIDEVTYPE_MOUSE = 2; 0 = all */
    uint32_t inst = guest_alloc(0x244);
    if (type == 0 || type == 3) {
        fill_instance(inst, 0x244, GUID_SysKeyboard, 0x0103, "Keyboard");
        uint32_t a[2] = { inst, ref };
        if (rt_guest_call(c, cb, 2, a) == 0) { guest_free(inst); RETURN(5, DI_OK); }
    }
    if (type == 0 || type == 2) {
        fill_instance(inst, 0x244, GUID_SysMouse, 0x0112, "Mouse");
        uint32_t a[2] = { inst, ref };
        if (rt_guest_call(c, cb, 2, a) == 0) { guest_free(inst); RETURN(5, DI_OK); }
    }
    if (type == 0 || type == 4) {                      /* DIDEVTYPE_JOYSTICK */
        fill_instance(inst, 0x244, GUID_TouchWheel, JOY_TYPE, "Touch Wheel");
        uint32_t a[2] = { inst, ref };
        rt_guest_call(c, cb, 2, a);
    }
    guest_free(inst);
    RETURN(5, DI_OK);
}
HLE(DI_GetDeviceStatus) { RETURN(2, DI_OK); }
HLE(DI_RunControlPanel) { RETURN(3, DI_OK); }
HLE(DI_Initialize) { RETURN(3, DI_OK); }

static const HleEntry di_methods[] = {
    { "QueryInterface", QueryInterface }, { "AddRef", AddRef }, { "Release", Release },
    { "CreateDevice", DI_CreateDevice }, { "EnumDevices", DI_EnumDevices }, { "GetDeviceStatus", DI_GetDeviceStatus },
    { "RunControlPanel", DI_RunControlPanel }, { "Initialize", DI_Initialize },
};

/* ---------------------------------------------------------------- IDirectInputDevice2A */
HLE(DEV_GetCapabilities) {
    uint32_t p = ARG(1), kind = RD32(ARG(0) + 8);
    uint32_t size = RD32(p);
    memset(MEM + p + 4, 0, size > 4 ? size - 4 : 0);
    WR32(p + 4, 0x5);                                  /* attached | polled */
    WR32(p + 8, kind == 1 ? 0x0103 : kind == 2 ? 0x0112 : JOY_TYPE);
    WR32(p + 12, kind == 3 ? 2 : kind == 2 ? 2 : 0);   /* axes */
    WR32(p + 16, kind == 1 ? 128 : kind == 2 ? 3 : JOY_BUTTONS);
    RETURN(2, DI_OK);
}

/* the joystick's objects: X and Y absolute axes, then buttons; DIDEVICEOBJECTINSTANCEA (DI5) */
#define OBJ_SIZE 0x13C
static int joy_object(int i, uint32_t p) {
    if (i < 0 || i >= 2 + JOY_BUTTONS) return 0;
    memset(MEM + p, 0, OBJ_SIZE);
    WR32(p, OBJ_SIZE);
    if (i < 2) {
        memcpy(MEM + p + 4, i ? GUID_YAxis : GUID_XAxis, 16);
        WR32(p + 20, (uint32_t)i * 4);                 /* DIJOYSTATE.lX / lY */
        WR32(p + 24, 0x02u | ((uint32_t)i << 8));      /* DIDFT_ABSAXIS | instance */
        strcpy((char *)MEM + p + 32, i ? "Y Axis" : "X Axis (steering)");
    } else {
        memcpy(MEM + p + 4, GUID_Button, 16);
        WR32(p + 20, 48u + (uint32_t)(i - 2));         /* DIJOYSTATE.rgbButtons */
        WR32(p + 24, 0x04u | ((uint32_t)(i - 2) << 8)); /* DIDFT_PSHBUTTON | instance */
        snprintf((char *)MEM + p + 32, 260, "Button %d", i - 1);
    }
    return 1;
}

HLE(DEV_EnumObjects) {
    uint32_t cb = ARG(1), ref = ARG(2), flags = ARG(3), kind = RD32(ARG(0) + 8);
    if (kind != 3) RETURN(4, DI_OK);
    uint32_t p = guest_alloc(OBJ_SIZE);
    for (int i = 0; joy_object(i, p); i++) {
        uint32_t type = RD32(p + 24);
        if (flags && !(type & flags & 0xFF)) continue; /* DIDFT_AXIS 3, DIDFT_BUTTON 0x0C, DIDFT_ALL 0 */
        uint32_t a[2] = { p, ref };
        if (rt_guest_call(c, cb, 2, a) == 0) break;    /* DIENUM_STOP */
    }
    guest_free(p);
    RETURN(4, DI_OK);
}

/* DIPROP_RANGE = 4: DIPROPRANGE { DIPROPHEADER (16 bytes), lMin, lMax }, kept for both axes */
HLE(DEV_GetProperty) {
    uint32_t prop = ARG(1), p = ARG(2), o = ARG(0);
    if (prop == 4) { WR32(p + 16, RD32(o + 16)); WR32(p + 20, RD32(o + 20)); }
    RETURN(3, DI_OK);
}
HLE(DEV_SetProperty) {
    uint32_t prop = ARG(1), p = ARG(2), o = ARG(0);
    if (prop == 4 && RD32(o + 8) == 3) {
        WR32(o + 16, RD32(p + 16)); WR32(o + 20, RD32(p + 20));
        plat_log("dinput: joystick range %d..%d", (int32_t)RD32(p + 16), (int32_t)RD32(p + 20));
    }
    RETURN(3, DI_OK);
}

static int32_t joy_axis(uint32_t o, float v) {
    int32_t lo = (int32_t)RD32(o + 16), hi = (int32_t)RD32(o + 20);
    if (v < -1) v = -1;
    if (v > 1) v = 1;
    return lo + (int32_t)((v + 1) * 0.5f * (float)(hi - lo) + 0.5f);
}
HLE(DEV_Acquire) { WR32(ARG(0) + 12, 1); RETURN(1, DI_OK); }
HLE(DEV_Unacquire) { WR32(ARG(0) + 12, 0); RETURN(1, DI_OK); }
volatile int32_t plat_mouse_dx, plat_mouse_dy;
volatile uint8_t plat_mouse_buttons;

/* scripted/touch click: drive the cursor to the corner, then to (x, y), press, release */
static volatile int g_click_phase, g_click_x, g_click_y;
void plat_mouse_click_at(int x, int y) { g_click_x = x; g_click_y = y; g_click_phase = 1; }

static void mouse_read(int32_t *dx, int32_t *dy, uint8_t *b) {
    switch (g_click_phase) {
    case 1: *dx = -4000; *dy = -4000; *b = 0; g_click_phase = 2; return;
    case 2: *dx = g_click_x; *dy = g_click_y; *b = 0; g_click_phase = 3; return;
    case 3: case 4: case 5: *dx = *dy = 0; *b = 0x80; g_click_phase++; return;
    case 6: *dx = *dy = 0; *b = 0; g_click_phase = 0; return;
    }
    *dx = __atomic_exchange_n(&plat_mouse_dx, 0, __ATOMIC_SEQ_CST);
    *dy = __atomic_exchange_n(&plat_mouse_dy, 0, __ATOMIC_SEQ_CST);
    *b = plat_mouse_buttons;
}

HLE(DEV_GetDeviceState) {
    uint32_t size = ARG(1), p = ARG(2), kind = RD32(ARG(0) + 8);
    memset(MEM + p, 0, size);
    if (kind == 1) for (uint32_t i = 0; i < size && i < 256; i++) MEM[p + i] = plat_keys[i];
    if (kind == 2 && size >= 16) {                     /* DIMOUSESTATE: lX, lY, lZ, rgbButtons[4] */
        int32_t dx, dy; uint8_t b;
        mouse_read(&dx, &dy, &b);
        WR32(p, (uint32_t)dx); WR32(p + 4, (uint32_t)dy); WR32(p + 8, 0);
        MEM[p + 12] = b;
    }
    if (kind == 3 && size >= 80) {                     /* DIJOYSTATE: lX, lY, ..., rgdwPOV[4] @32, rgbButtons @48 */
        uint32_t o = ARG(0);
        int32_t mid = joy_axis(o, 0);
        WR32(p, (uint32_t)joy_axis(o, plat_joy_x)); WR32(p + 4, (uint32_t)joy_axis(o, plat_joy_y));
        for (int i = 2; i < 6; i++) WR32(p + 4 * i, (uint32_t)mid);
        for (int i = 0; i < 4; i++) WR32(p + 32 + 4 * i, 0xFFFFFFFFu);   /* POV centred */
        for (int i = 0; i < JOY_BUTTONS; i++) MEM[p + 48 + i] = (plat_joy_buttons >> i) & 1 ? 0x80 : 0;
    }
    RETURN(3, DI_OK);
}
/* buffered input: DIDEVICEOBJECTDATA { dwOfs, dwData, dwTimeStamp, dwSequence } */
static uint8_t g_last_buttons;
static uint32_t g_seq;
HLE(DEV_GetDeviceData) {
    uint32_t size = ARG(1), out = ARG(2), inout = ARG(3), flags = ARG(4), kind = RD32(ARG(0) + 8);
    uint32_t cap = inout ? RD32(inout) : 0, n = 0;
    if (size < 16) size = 16;
    uint32_t ofs[4], data[4];
    uint32_t k = 0;
    if (kind == 2) {
        int32_t dx, dy; uint8_t b;
        mouse_read(&dx, &dy, &b);
        if (dx) { ofs[k] = 0; data[k++] = (uint32_t)dx; }
        if (dy) { ofs[k] = 4; data[k++] = (uint32_t)dy; }
        if (b != g_last_buttons) { ofs[k] = 12; data[k++] = b; g_last_buttons = b; }
    }
    if (kind == 3) {                                   /* changes since the last read */
        static int32_t last_x = INT32_MIN;
        static uint32_t last_b;
        int32_t x = joy_axis(ARG(0), plat_joy_x);
        if (x != last_x) { ofs[k] = 0; data[k++] = (uint32_t)x; last_x = x; }
        uint32_t jb = plat_joy_buttons;
        for (int i = 0; i < JOY_BUTTONS && k < 4; i++)
            if (((jb ^ last_b) >> i) & 1) { ofs[k] = 48u + (uint32_t)i; data[k++] = (jb >> i) & 1 ? 0x80 : 0; last_b ^= 1u << i; }
    }
    uint32_t now = (uint32_t)(plat_ticks_us() / 1000);
    for (uint32_t i = 0; i < k && n < cap; i++, n++) {
        if (out && !(flags & 1)) {                     /* DIGDD_PEEK = 1 still reports */
            uint32_t p = out + n * size;
            WR32(p, ofs[i]); WR32(p + 4, data[i]); WR32(p + 8, now); WR32(p + 12, g_seq++);
        }
    }
    if (inout) WR32(inout, n);
    RETURN(5, DI_OK);
}
HLE(DEV_SetDataFormat) { RETURN(2, DI_OK); }
HLE(DEV_SetEventNotification) { RETURN(2, DI_OK); }
HLE(DEV_SetCooperativeLevel) { RETURN(3, DI_OK); }
/* GetObjectInfo(pdidoi, dwObj, dwHow): DIPH_BYOFFSET = 1, DIPH_BYID = 2 */
HLE(DEV_GetObjectInfo) {
    uint32_t p = ARG(1), obj = ARG(2), how = ARG(3), kind = RD32(ARG(0) + 8);
    if (kind != 3) RETURN(4, DIERR_NOTFOUND);
    for (int i = 0; i < 2 + JOY_BUTTONS; i++) {
        uint32_t ofs = i < 2 ? (uint32_t)i * 4 : 48u + (uint32_t)(i - 2);
        uint32_t id = i < 2 ? (0x02u | ((uint32_t)i << 8)) : (0x04u | ((uint32_t)(i - 2) << 8));
        if ((how == 1 && obj == ofs) || (how == 2 && (obj & 0xFFFF) == (id & 0xFFFF))) { joy_object(i, p); RETURN(4, DI_OK); }
    }
    RETURN(4, DIERR_NOTFOUND);
}
HLE(DEV_GetDeviceInfo) {
    uint32_t kind = RD32(ARG(0) + 8);
    fill_instance(ARG(1), RD32(ARG(1)) ? RD32(ARG(1)) : 0x244,
                  kind == 1 ? GUID_SysKeyboard : kind == 2 ? GUID_SysMouse : GUID_TouchWheel,
                  kind == 1 ? 0x0103 : kind == 2 ? 0x0112 : JOY_TYPE,
                  kind == 1 ? "Keyboard" : kind == 2 ? "Mouse" : "Touch Wheel");
    RETURN(2, DI_OK);
}
HLE(DEV_RunControlPanel) { RETURN(3, DI_OK); }
HLE(DEV_Initialize) { RETURN(4, DI_OK); }
HLE(DEV_CreateEffect) { WR32(ARG(3), 0); RETURN(5, 0x80004001u); }
HLE(DEV_EnumEffects) { RETURN(4, DI_OK); }
HLE(DEV_GetEffectInfo) { RETURN(3, 0x80004001u); }
HLE(DEV_GetForceFeedbackState) { RETURN(2, 0x80004001u); }
HLE(DEV_SendForceFeedbackCommand) { RETURN(2, 0x80004001u); }
HLE(DEV_EnumCreatedEffectObjects) { RETURN(4, DI_OK); }
HLE(DEV_Escape) { RETURN(2, 0x80004001u); }
HLE(DEV_Poll) { RETURN(1, DI_OK); }
HLE(DEV_SendDeviceData) { RETURN(5, DI_OK); }

static const HleEntry dev_methods[] = {
    { "QueryInterface", QueryInterface }, { "AddRef", AddRef }, { "Release", Release },
    { "GetCapabilities", DEV_GetCapabilities }, { "EnumObjects", DEV_EnumObjects }, { "GetProperty", DEV_GetProperty },
    { "SetProperty", DEV_SetProperty }, { "Acquire", DEV_Acquire }, { "Unacquire", DEV_Unacquire },
    { "GetDeviceState", DEV_GetDeviceState }, { "GetDeviceData", DEV_GetDeviceData }, { "SetDataFormat", DEV_SetDataFormat },
    { "SetEventNotification", DEV_SetEventNotification }, { "SetCooperativeLevel", DEV_SetCooperativeLevel },
    { "GetObjectInfo", DEV_GetObjectInfo }, { "GetDeviceInfo", DEV_GetDeviceInfo }, { "RunControlPanel", DEV_RunControlPanel },
    { "Initialize", DEV_Initialize }, { "CreateEffect", DEV_CreateEffect }, { "EnumEffects", DEV_EnumEffects },
    { "GetEffectInfo", DEV_GetEffectInfo }, { "GetForceFeedbackState", DEV_GetForceFeedbackState },
    { "SendForceFeedbackCommand", DEV_SendForceFeedbackCommand }, { "EnumCreatedEffectObjects", DEV_EnumCreatedEffectObjects },
    { "Escape", DEV_Escape }, { "Poll", DEV_Poll }, { "SendDeviceData", DEV_SendDeviceData },
};

HLE(DirectInputCreateA) {
    if (!g_di_vtbl) {
        g_di_vtbl = make_vtable("IDirectInputA", di_methods, (int)(sizeof di_methods / sizeof di_methods[0]));
        g_dev_vtbl = make_vtable("IDirectInputDevice2A", dev_methods, (int)(sizeof dev_methods / sizeof dev_methods[0]));
    }
    plat_log("DirectInputCreateA(ver=%08X)", ARG(1));
    WR32(ARG(2), new_object(g_di_vtbl, 0));
    RETURN(4, DI_OK);
}

const HleEntry hle_dinput[] = { { "DirectInputCreateA", DirectInputCreateA }, { NULL, NULL } };
