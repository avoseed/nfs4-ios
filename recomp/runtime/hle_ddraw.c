/* DirectDraw capability probes used before loading the Thrash Glide renderer.
 * This is not a DirectDraw renderer: unknown operations trap with their COM name. */
#include "hle.h"
#include <string.h>
static uint32_t dd_vt, surf_vt;
static const uint8_t IID_IUnknown[16] = {0,0,0,0,0,0,0,0,0xC0,0,0,0,0,0,0,0x46};
static const uint8_t IID_DDraw[16] = {0xE0,0x02,0x6D,0xA3,0xF3,0xC9,0xCF,0x11,0xBF,0xC7,0x44,0x45,0x53,0x54,0,0};
static const uint8_t IID_DDraw2[16] = {0xE0,0xF3,0xA6,0xB3,0x43,0x2B,0xCF,0x11,0xA2,0xDE,0,0xAA,0,0xB9,0x33,0x56};
static const uint8_t IID_Surface3[16] = {0,0x4E,4,0xDA,0xB2,0x69,0xD0,0x11,0xA1,0xD5,0,0xAA,0,0xB8,0xDF,0xBB};
HLE(DD_QueryInterface) {
    uint32_t obj=ARG(0); const uint8_t *iid=MEM+ARG(1); int surface=RD32(obj)==surf_vt;
    int ok=!memcmp(iid,IID_IUnknown,16) || (!surface && (!memcmp(iid,IID_DDraw,16) || !memcmp(iid,IID_DDraw2,16))) || (surface && !memcmp(iid,IID_Surface3,16));
    if (ARG(2)) WR32(ARG(2),ok?obj:0);
    plat_log("ddraw: QueryInterface(%08X-%04X-%04X, surface=%d) -> %s",RD32(ARG(1)),RD16(ARG(1)+4),RD16(ARG(1)+6),surface,ok?"OK":"E_NOINTERFACE");
    if(ok) WR32(obj+4,RD32(obj+4)+1);
    RETURN(3,ok?0:0x80004002u);
}
HLE(DD_AddRef) { uint32_t n=RD32(ARG(0)+4)+1;WR32(ARG(0)+4,n);RETURN(1,n); }
HLE(DD_Release) { uint32_t n=RD32(ARG(0)+4);if(n)n--;WR32(ARG(0)+4,n);RETURN(1,n); }
HLE(DD_SetCooperativeLevel) { plat_log("ddraw: SetCooperativeLevel(hwnd=%08X, flags=%08X)",ARG(1),ARG(2));RETURN(3,0); }
HLE(DD_CreateSurface) {
    uint32_t desc=ARG(1),obj=guest_alloc(16);WR32(obj,surf_vt);WR32(obj+4,1);
    WR32(ARG(2),obj);plat_log("ddraw: CreateSurface(size=%u, flags=%08X, caps=%08X)",RD32(desc),RD32(desc+4),RD32(desc+104));RETURN(4,0);
}
static void tables(void) {
    if(dd_vt)return;
    static const char *names[]={"QueryInterface","AddRef","Release","Compact","CreateClipper","CreatePalette","CreateSurface","DuplicateSurface","EnumDisplayModes","EnumSurfaces","FlipToGDISurface","GetCaps","GetDisplayMode","GetFourCCCodes","GetGDISurface","GetMonitorFrequency","GetScanLine","GetVerticalBlankStatus","Initialize","RestoreDisplayMode","SetCooperativeLevel","SetDisplayMode","WaitForVerticalBlank","GetAvailableVidMem"};
    dd_vt=guest_alloc(sizeof(names)/sizeof(names[0])*4);surf_vt=guest_alloc(4*40);
    for(int i=0;i<24;i++) {
        HleFn fn=i==0?DD_QueryInterface:i==1?DD_AddRef:i==2?DD_Release:i==6?DD_CreateSurface:i==20?DD_SetCooperativeLevel:hle_unimplemented;
        char name[96];snprintf(name,sizeof name,"IDirectDraw::%s",names[i]);WR32(dd_vt+4*i,rt_hle_address("ddraw.dll",name,fn));
    }
    for(int i=0;i<40;i++) {
        char name[96];snprintf(name,sizeof name,"IDirectDrawSurface::slot%d",i);
        WR32(surf_vt+4*i,rt_hle_address("ddraw.dll",name,i==0?DD_QueryInterface:i==1?DD_AddRef:i==2?DD_Release:hle_unimplemented));
    }
}
HLE(DirectDrawCreate) {
    if(!ARG(1))RETURN(3,0x80070057u);tables();uint32_t obj=guest_alloc(16);WR32(obj,dd_vt);WR32(obj+4,1);WR32(ARG(1),obj);
    plat_log("DirectDrawCreate -> %08X (capability probe)",obj);RETURN(3,0);
}
const HleEntry hle_ddraw[]={{"DirectDrawCreate",DirectDrawCreate},{NULL,NULL}};
