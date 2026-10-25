#include "rt_host.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
uint8_t *MEM;
static uint32_t next=0x100000, frees;
uint32_t guest_alloc(uint32_t n) { uint32_t p=next;next+=(n+15)&~15u;return p; }
void guest_free(uint32_t p) { if(p) frees++; }
void plat_log(const char *fmt, ...) { (void)fmt; }
void plat_fatal(const char *fmt, ...) { abort(); }
int main(void) {
    MEM=calloc(1,32u<<20);uint32_t src=4096,stack=1024,limit=256,data=src;
    MEM[src+2]=2;MEM[src+16]=32;MEM[src+17]=8;WR16(src+12,256);WR16(src+14,256);
    for(unsigned y=0;y<256;y++)for(unsigned x=0;x<256;x++) {
        uint32_t p=src+18+(y*256+x)*4;
        MEM[p]=42;MEM[p+1]=96;MEM[p+2]=173;MEM[p+3]=(uint8_t)(((x/32+y/32)%4)*85);
    }
    uint32_t scratch=hle_car_texture_begin(&data,&limit,0,stack);
    assert(scratch && data==scratch && limit==1024);
    assert(RD16(data+12)==1024 && RD16(data+14)==1024 && MEM[data+17]==8);
    for(unsigned y=0;y<1024;y++)for(unsigned x=0;x<1024;x++) {
        uint32_t p=data+18+(y*1024+x)*4;
        assert(MEM[p]==42 && MEM[p+1]==96 && MEM[p+2]==173);
        assert(MEM[p+3]==MEM[src+18+((y/4)*256+x/4)*4+3]);
    }
    assert(RD16(src+12)==256); /* source untouched */
    hle_car_texture_end(scratch,123,limit); assert(frees==1);
    data=src;limit=256;assert(hle_car_texture_begin(&data,&limit,999,stack)==0 && limit==256);
    WR32(123+4,10);WR32(123+8,0);WR32(123+12,0xB);
    data=src;limit=256;scratch=hle_car_texture_begin(&data,&limit,123,stack);assert(scratch && limit==1024);
    hle_car_texture_end(scratch,123,limit);
    WR32(stack+12,0x70000);data=src;limit=256;
    scratch=hle_car_texture_begin(&data,&limit,0,stack);assert(scratch && limit==1024);
    hle_car_texture_end(scratch,0,limit);
    uint32_t mirror=0x700000, pixels=0x800000;
    memset(MEM+mirror,0xA5,256*256*2+64);
    for(unsigned y=0;y<1024;y++) for(unsigned x=0;x<1024;x++) WR16(pixels+(y*1024+x)*2,(uint16_t)((y/4)*256+x/4));
    hle_car_texture_copy(pixels,mirror,1024*1024*2,1024);
    for(unsigned i=0;i<256*256;i++) assert(RD16(mirror+i*2)==(uint16_t)i);
    for(unsigned i=0;i<64;i++) assert(MEM[mirror+256*256*2+i]==0xA5);
    for(unsigned i=0;i<256*256;i++) WR16(mirror+i*2,0x8000|0x1234);
    const uint8_t *dynamic=hle_car_texture_upload(mirror,10,10,0,0xB);assert(dynamic);
    for(unsigned i=0;i<1024*1024;i++)assert((dynamic[i*2]|(dynamic[i*2+1]<<8))==(0x8000|0x1234));
    assert(hle_car_texture_upload(mirror,10,10,0,0xB)==dynamic);
    WR16(mirror,0xFFFF);
    const uint8_t *changed=hle_car_texture_upload(mirror,10,10,0,0xB);
    assert(changed && (changed[0]!=0x34 || changed[1]!=0x92));
    assert(changed[(1024*1024-1)*2]==0x34 && changed[(1024*1024-1)*2+1]==0x92);
    /* A third mirror evicts one of the two bounded entries; revisiting the
     * first source must reconstruct its own bytes, not another car's cache. */
    for(unsigned j=0;j<2;j++) {
        uint32_t other=mirror+0x40000*(j+1);
        hle_car_texture_copy(pixels,other,1024*1024*2,1024);
        for(unsigned i=0;i<256*256;i++)WR16(other+i*2,0xABCD+j);
        assert(hle_car_texture_upload(other,10,10,0,0xB));
    }
    dynamic=hle_car_texture_upload(mirror,10,10,0,0xB);
    assert(dynamic[(1024*1024-1)*2]==0x34 && dynamic[(1024*1024-1)*2+1]==0x92);
    assert(!hle_car_texture_upload(mirror+4,10,10,0,0xB));
    assert(!hle_car_texture_upload(mirror,8,10,0,0xB));
    WR32(stack+12,0);WR16(src+12,255);data=src;limit=256;
    assert(!hle_car_texture_begin(&data,&limit,0,stack));
    puts("car_texture: 1048576 RGB/alpha samples, source preservation, reuse and bounded 256px mirror and dynamic uploads passed");
    free(MEM);return 0;
}
