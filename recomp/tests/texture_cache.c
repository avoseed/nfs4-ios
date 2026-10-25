#include "../runtime/hle_glide.c"
#include <assert.h>
uint8_t *MEM;
int g_screen_w,g_screen_h;
static int destroys,draws,last_destroy;
void gpu_texture_destroy(int id){destroys++;last_destroy=id;}
void gpu_draw(const GpuState *s,const GpuVertex *v,int n){(void)s;(void)v;assert(n==3);draws++;}
const uint8_t *hle_car_texture_upload(uint32_t d,int s,int l,int a,int f){(void)d;(void)s;(void)l;(void)a;(void)f;return NULL;}
/* Unused platform entry points fail if this focused cache test reaches them. */
void plat_log(const char *fmt,...) {(void)fmt;}
float plat_wide_aspect(void){abort();}
uint64_t plat_ticks_us(void){abort();}
uint32_t guest_strdup(const char *s){(void)s;abort();}
HleFn rt_resolve_import(const char *d,const char *n){(void)d;(void)n;abort();}
void hle_unimplemented(Cpu *c){(void)c;abort();}
uint32_t rt_import_address(const char *d,const char *n,HleFn f){(void)d;(void)n;(void)f;abort();}
int plat_frame_cap(void){abort();}
void plat_sleep_us(uint32_t n){(void)n;abort();}
void gpu_swap(void){abort();}
void plat_present(const uint32_t *p,int w,int h){(void)p;(void)w;(void)h;abort();}
int gpu_texture_create(int w,int h,int m){(void)w;(void)h;(void)m;abort();}
void gpu_texture_upload(int id,const uint32_t *p){(void)id;(void)p;abort();}
void guest_free(uint32_t p){(void)p;abort();}
uint32_t guest_alloc(uint32_t n){(void)n;abort();}
int gpu_init(int w,int h){(void)w;(void)h;abort();}
static void args(Cpu *c,uint32_t type,uint32_t data){c->esp=16;WR32(16,0x12345678);WR32(20,type);WR32(24,data);}
int main(void){
 MEM=calloc(1,1u<<20);assert(texture_bytes(1024,1024)==5592404);
 g_ntex=3;g_gpu_texture_bytes=48u<<20;
 for(int i=0;i<3;i++){g_texcache[i].id=i+1;g_texcache[i].gpu_bytes=16u<<20;g_texcache[i].used=i+1;}
 g_texcache[0].stale=1;g_texcache[0].used=100;g_nbatch=3;
 make_texture_room(32u<<20,&g_texcache[2]);
 assert(g_gpu_texture_bytes==(32u<<20) && destroys==1 && last_destroy==1 && draws==1);
 make_texture_room(48u<<20,&g_texcache[2]);
 assert(g_gpu_texture_bytes==(16u<<20) && destroys==2 && last_destroy==2 && g_texcache[2].id==3);
 Cpu c={0};g_gpu=1;g_ntex=1;g_nbatch=3;
 g_texcache[0].addr=0;g_texcache[0].bytes=65536;g_texcache[0].stale=0;
 uint32_t info=128,data=4096;
 WR32(info,8);WR32(info+4,8);WR32(info+8,0);WR32(info+12,3);WR32(info+16,data);
 memset(MEM+data,77,65536);
 c.esp=16;WR32(24,0);WR32(32,info);grTexDownloadMipMap(&c);
 assert(g_texcache[0].stale && g_tmu[500]==77);
 int previous=draws;g_texcache[0].stale=0;g_nbatch=3;
 c.esp=16;grTexDownloadMipMap(&c);
 assert(!g_texcache[0].stale && draws==previous && g_nbatch==3);
 for(int i=0;i<256;i++)WR32(data+i*4,0x11223344+i);
 args(&c,2,data);grTexDownloadTable(&c);uint32_t serial=g_palette_serial;
 g_nbatch=3;previous=draws;args(&c,2,data);grTexDownloadTable(&c);
 assert(serial==g_palette_serial && previous==draws && g_nbatch==3);
 free(MEM);puts("texture_cache: byte budget, stale/LRU eviction, queued draw flush, identical texture/palette skips passed");return 0;
}
