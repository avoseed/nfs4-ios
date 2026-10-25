/* Windows CPU-only benchmark; includes output hashing, not iPhone energy measurements. */
#include "rt_host.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
uint8_t *MEM;
uint32_t guest_alloc(uint32_t n){(void)n;return 0x1000000;}
void guest_free(uint32_t p){(void)p;}
void plat_log(const char *fmt,...){(void)fmt;}
void plat_fatal(const char *fmt,...){abort();}
static double now(void){LARGE_INTEGER n,f;QueryPerformanceCounter(&n);QueryPerformanceFrequency(&f);return n.QuadPart*1000.0/f.QuadPart;}
static uint32_t hash(const uint8_t *p,size_t n){uint32_t h=2166136261u;for(size_t i=0;i<n;i++)h=(h^p[i])*16777619u;return h;}
int main(void){
 MEM=calloc(1,64u<<20);uint32_t src=4096,stack=1024,mirror=0x400000,pixels=0x800000;
 MEM[src+2]=2;MEM[src+16]=32;MEM[src+17]=8;WR16(src+12,256);WR16(src+14,256);
 for(unsigned y=0;y<256;y++)for(unsigned x=0;x<256;x++){uint32_t p=src+18+(y*256+x)*4;MEM[p]=(x*3+y)%256;MEM[p+1]=(x+y*5)%256;MEM[p+2]=(x*7+y*3)%256;MEM[p+3]=((x/32+y/32)%4)*85;}
 uint32_t rgbhash=0,dynhash=0;double t=now();
 for(int i=0;i<4;i++){uint32_t data=src,side=256;uint32_t scratch=hle_car_texture_begin(&data,&side,0,stack);rgbhash=hash(MEM+data+18,1024*1024*4);hle_car_texture_end(scratch,0,side);}
 double rgb=now()-t;
 for(unsigned y=0;y<1024;y++)for(unsigned x=0;x<1024;x++)WR16(pixels+(y*1024+x)*2,(uint16_t)((y/4)*256+x/4));
 hle_car_texture_copy(pixels,mirror,1024*1024*2,1024);t=now();
 for(int i=0;i<8;i++){
 const uint8_t *p=hle_car_texture_upload(mirror,10,10,0,0xB);dynhash=hash(p,1024*1024*2);

 }
 double repeated=now()-t;WR16(mirror+120,0xA12C);t=now();
 const uint8_t *p=hle_car_texture_upload(mirror,10,10,0,0xB);uint32_t changed=hash(p,1024*1024*2);

 double change=now()-t;
 printf("{\"rgb_4_ms\":%.3f,\"mirror_8_ms\":%.3f,\"changed_mirror_ms\":%.3f,\"rgb_hash\":%u,\"mirror_hash\":%u,\"changed_hash\":%u}\n",rgb,repeated,change,rgbhash,dynhash,changed);
 free(MEM);return 0;
}