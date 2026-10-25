/* Portable MMX lane operations. All shifts are bounded before evaluating C shifts. */
#include <stdint.h>
static int64_t signed_lane(uint64_t x,unsigned bits){uint64_t mask=UINT64_MAX>>(64-bits);x&=mask;return (x&(UINT64_C(1)<<(bits-1)))?-(int64_t)((~x&mask)+1):(int64_t)x;}
static int64_t clip(int64_t v,int64_t lo,int64_t hi){return v<lo?lo:v>hi?hi:v;}
uint64_t rt_mmx_binary(unsigned op,uint64_t a,uint64_t b){
    if(op==0)return a^b;if(op==1)return a|b;if(op==2)return a&b;
    unsigned bits=16;uint64_t out=0;
    if(op==3||op==4||op==5||op==6||op==7){
        bits=(op==5||op==6||op==7)?32:16;uint64_t mask=UINT64_MAX>>(64-bits);
        for(unsigned i=0;i<64/bits;i++){uint64_t x=(a>>(i*bits))&mask,y=(b>>(i*bits))&mask,z=0;
            if(op==3)z=(uint64_t)clip(signed_lane(x,16)+signed_lane(y,16),-32768,32767);
            if(op==4||op==5)z=x+y;
            if(op==6)z=(uint64_t)((int64_t)(int16_t)(x&65535)*(int16_t)(y&65535)+(int64_t)(int16_t)(x>>16)*(int16_t)(y>>16));
            if(op==7)z=0;
            out|=(z&mask)<<(i*bits);
        }return out;
    }
    if(op==8){for(unsigned i=0;i<4;i++){int64_t z=signed_lane(a>>(i*16),16)*signed_lane(b>>(i*16),16);out|=((uint64_t)(z>>16)&65535)<<(i*16);}return out;}
    /* 9..14: PSRLW, PSRLD, PSRLQ, PSLLQ, PSLLD, PSRAD */
    if(op>=9&&op<=14){bits=op==9?16:(op==11||op==12)?64:32;uint64_t mask=bits==64?UINT64_MAX:UINT64_MAX>>(64-bits);
        for(unsigned i=0;i<64/bits;i++){uint64_t x=(a>>(i*bits))&mask,z;
            if(op==14)z=b>=bits?(signed_lane(x,bits)<0?mask:0):(uint64_t)(signed_lane(x,bits)>>(unsigned)b);
            else if(b>=bits)z=0;else if(op==12||op==13)z=x<<(unsigned)b;else z=x>>(unsigned)b;
            out|=(z&mask)<<(i*bits);
        }return out;
    }
    /* 15..20: unpack low byte/word/dword, high byte/word/dword. */
    if(op>=15&&op<=20){bits=8u<<((op-15)%3);unsigned half=32/bits,start=op>=18?half:0;uint64_t mask=UINT64_MAX>>(64-bits);
        for(unsigned i=0;i<half;i++){out|=((a>>((start+i)*bits))&mask)<<(2*i*bits);out|=((b>>((start+i)*bits))&mask)<<((2*i+1)*bits);}return out;
    }
    /* 21..23: PACKUSWB, PACKSSDW, PACKSSWB. */
    if(op>=21&&op<=23){unsigned srcbits=op==22?32:16,dstbits=srcbits/2,lanes=64/srcbits;uint64_t mask=UINT64_MAX>>(64-dstbits);
        int64_t lo=op==21?0:-(INT64_C(1)<<(dstbits-1)),hi=op==21?255:(INT64_C(1)<<(dstbits-1))-1;
        for(unsigned k=0;k<2;k++)for(unsigned i=0;i<lanes;i++){uint64_t src=k?b:a;uint64_t z=(uint64_t)clip(signed_lane(src>>(i*srcbits),srcbits),lo,hi);out|=(z&mask)<<((k*lanes+i)*dstbits);}return out;
    }
    return 0; /* generator only emits the defined operations */
}
