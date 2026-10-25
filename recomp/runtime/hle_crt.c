/* CRT compatibility required by the NFS4 MSVC-built renderer. Guest buffers only. */
#include "hle.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
static uint32_t wlen(uint32_t p){uint32_t n=0;while(RD16(p+2*n))n++;return n;}
HLE(GetVersionExA){uint32_t p=ARG(0),n=RD32(p);if(n<148)RETURN(1,0);memset(MEM+p+4,0,n-4);WR32(p+4,5);WR32(p+8,1);WR32(p+12,2600);WR32(p+16,2);plat_log("GetVersionExA -> Windows 5.1 (guest)");RETURN(1,1);}
HLE(GetStartupInfoA){uint32_t p=ARG(0);memset(MEM+p,0,68);WR32(p,68);WR16(p+48,1);RETURN(1,0);}
HLE(GetEnvironmentStringsW){static uint32_t p;if(!p)p=guest_alloc(4);WR32(p,0);RETURN(0,p);}
HLE(FreeEnvironmentStringsW){RETURN(1,1);}
HLE(GetEnvironmentVariableA){/* Native process environment is deliberately not exposed. */if(ARG(2))WR8(ARG(1),0);set_last_error(c,203);RETURN(3,0);}
HLE(SetHandleCount){RETURN(1,ARG(0));}
HLE(InterlockedIncrement){int32_t n=__atomic_add_fetch((int32_t*)(MEM+ARG(0)),1,__ATOMIC_SEQ_CST);RETURN(1,n);}
HLE(InterlockedDecrement){int32_t n=__atomic_sub_fetch((int32_t*)(MEM+ARG(0)),1,__ATOMIC_SEQ_CST);RETURN(1,n);}
static uint16_t ctype1(unsigned ch){uint16_t f=0;if(ch<128){if(isupper(ch))f|=1;if(islower(ch))f|=2;if(isdigit(ch))f|=4;if(isspace(ch))f|=8;if(ispunct(ch))f|=16;if(iscntrl(ch))f|=32;if(ch==' '||ch=='\t')f|=64;if(isxdigit(ch))f|=128;if(isalpha(ch))f|=256;}return f;}
HLE(GetStringTypeA){uint32_t type=ARG(1),s=ARG(2),n=ARG(3),out=ARG(4);if(type!=1){set_last_error(c,87);RETURN(5,0);}if(n==0xFFFFFFFFu)n=(uint32_t)strlen(STR(s))+1;for(uint32_t i=0;i<n;i++)WR16(out+2*i,ctype1(RD8(s+i)));RETURN(5,1);}
HLE(GetStringTypeW){uint32_t type=ARG(0),s=ARG(1),n=ARG(2),out=ARG(3);if(type!=1){set_last_error(c,87);RETURN(4,0);}if(n==0xFFFFFFFFu)n=wlen(s)+1;for(uint32_t i=0;i<n;i++)WR16(out+2*i,ctype1(RD16(s+2*i)));RETURN(4,1);}
HLE(LCMapStringA){uint32_t flags=ARG(1),s=ARG(2),n=ARG(3),out=ARG(4),cap=ARG(5);if(n==0xFFFFFFFFu)n=(uint32_t)strlen(STR(s))+1;if(!cap)RETURN(6,n);if(cap<n){set_last_error(c,122);RETURN(6,0);}for(uint32_t i=0;i<n;i++){unsigned ch=RD8(s+i);if(flags&0x200)ch=toupper(ch);else if(flags&0x100)ch=tolower(ch);WR8(out+i,ch);}RETURN(6,n);}
HLE(LCMapStringW){uint32_t flags=ARG(1),s=ARG(2),n=ARG(3),out=ARG(4),cap=ARG(5);if(n==0xFFFFFFFFu)n=wlen(s)+1;if(!cap)RETURN(6,n);if(cap<n){set_last_error(c,122);RETURN(6,0);}for(uint32_t i=0;i<n;i++){unsigned ch=RD16(s+2*i);if(ch<128){if(flags&0x200)ch=toupper(ch);else if(flags&0x100)ch=tolower(ch);}WR16(out+2*i,ch);}RETURN(6,n);}
static int compare(uint32_t a,uint32_t an,uint32_t b,uint32_t bn,int wide,int fold){if(an==0xFFFFFFFFu)an=wide?wlen(a):(uint32_t)strlen(STR(a));if(bn==0xFFFFFFFFu)bn=wide?wlen(b):(uint32_t)strlen(STR(b));uint32_t n=an<bn?an:bn;for(uint32_t i=0;i<n;i++){unsigned x=wide?RD16(a+2*i):RD8(a+i),y=wide?RD16(b+2*i):RD8(b+i);if(fold&&x<128)x=tolower(x);if(fold&&y<128)y=tolower(y);if(x!=y)return x<y?1:3;}return an==bn?2:an<bn?1:3;}
HLE(CompareStringA){RETURN(6,compare(ARG(2),ARG(3),ARG(4),ARG(5),0,ARG(1)&1));}
HLE(CompareStringW){RETURN(6,compare(ARG(2),ARG(3),ARG(4),ARG(5),1,ARG(1)&1));}
HLE(Sleep){plat_sleep_ms(ARG(0));RETURN(1,0);}
HLE(GlobalAlloc){RETURN(2,guest_alloc(ARG(1)));}
HLE(GlobalFree){if(ARG(0))guest_free(ARG(0));RETURN(1,0);}
HLE(RaiseException){plat_log("RaiseException(code=%08X, flags=%08X, nargs=%u)",ARG(0),ARG(1),ARG(2));rt_trap(c,RD32(c->esp),"guest exception");}
HLE(TerminateProcess){plat_log("TerminateProcess guest request %08X code %u",ARG(0),ARG(1));rt_trap(c,RD32(c->esp),"guest termination");}
#define E(n) {#n,n}
const HleEntry hle_kernel32_crt[]={E(GetVersionExA),E(GetStartupInfoA),E(GetEnvironmentStringsW),E(FreeEnvironmentStringsW),E(GetEnvironmentVariableA),E(SetHandleCount),E(InterlockedIncrement),E(InterlockedDecrement),E(GetStringTypeA),E(GetStringTypeW),E(LCMapStringA),E(LCMapStringW),E(CompareStringA),E(CompareStringW),E(Sleep),E(GlobalAlloc),E(GlobalFree),E(RaiseException),E(TerminateProcess),{NULL,NULL}};
