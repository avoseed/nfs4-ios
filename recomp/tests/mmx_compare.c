#include <stdint.h>
#include <stdio.h>
uint64_t rt_mmx_binary(unsigned,uint64_t,uint64_t);
#define NATIVE(k,ins) case k: __asm__ volatile("movq %1, %%mm0\n\tmovq %2, %%mm1\n\t" #ins " %%mm1, %%mm0\n\tmovq %%mm0, %0\n\temms" : "=m"(out):"m"(a),"m"(b):"mm0","mm1"); break
static uint64_t native(unsigned op,uint64_t a,uint64_t b){uint64_t out=0;switch(op){NATIVE(0,pxor);NATIVE(1,por);NATIVE(2,pand);NATIVE(3,paddsw);NATIVE(4,paddw);NATIVE(5,paddd);NATIVE(6,pmaddwd);NATIVE(8,pmulhw);NATIVE(9,psrlw);NATIVE(10,psrld);NATIVE(11,psrlq);NATIVE(12,psllq);NATIVE(13,pslld);NATIVE(14,psrad);NATIVE(15,punpcklbw);NATIVE(16,punpcklwd);NATIVE(17,punpckldq);NATIVE(18,punpckhbw);NATIVE(19,punpckhwd);NATIVE(20,punpckhdq);NATIVE(21,packuswb);NATIVE(22,packssdw);NATIVE(23,packsswb);}return out;}
static uint64_t state=0xFACED123ABCDEF01ULL;
static uint64_t rng(void){state^=state<<13;state^=state>>7;state^=state<<17;return state;}
int main(void){unsigned total=0;uint64_t corner[]={0,1,15,16,17,31,32,33,63,64,65,0x8000800080008000ULL,0x7FFF7FFF7FFF7FFFULL,0xFFFFFFFFFFFFFFFFULL,0x8000000080000000ULL,0x7FFFFFFF7FFFFFFFULL};
for(unsigned op=0;op<24;op++){if(op==7)continue;for(unsigned n=0;n<10000;n++){uint64_t a=n<16?corner[n]:rng(),b=n<256?corner[(n/16)%16]:rng();if(op>=9&&op<=14&&n>=256)b=n%70;
uint64_t x=native(op,a,b),y=rt_mmx_binary(op,a,b);total++;if(x!=y){printf("FAIL op=%u a=%016llX b=%016llX native=%016llX portable=%016llX\n",op,(unsigned long long)a,(unsigned long long)b,(unsigned long long)x,(unsigned long long)y);return 1;}}}
printf("PASS: %u MMX native CPU comparisons across 23 operations\n",total);return 0;}
