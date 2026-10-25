// iOS host for the recompiled NFS4: UIKit shell, Metal presentation, touch/gamepad input,
// AudioUnit output and the POSIX side of the platform interface (plat.h).
#import <UIKit/UIKit.h>
#import <MetalKit/MetalKit.h>
#import <GameController/GameController.h>
#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>
#include <stdarg.h>
#include <mach/mach_time.h>
#include <signal.h>
#include <ctype.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <sys/ucontext.h>
#include "plat.h"
#include "rt_host.h"

void hle_key_event(uint8_t sc, int down);
id<MTLTexture> gpu_metal_display_texture(void);
int g_screen_height_px;            // device height in landscape, used for render upscaling
int g_msaa_samples = 4;            // in-app setting, read by gpu_metal.m
static float g_wide_aspect;        // aspect of the area beside the Dynamic Island, 0 = keep 4:3
static BOOL g_overlay_editing;     // overlay editor open: touches move controls, nothing reaches the game
float plat_wide_aspect(void) { return g_wide_aspect; }
void hle_audio_mix(float *out, int frames, int rate);

volatile uint8_t plat_keys[256];
static char g_game[1024], g_save[1024];
static FILE *g_log;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

// ---------------------------------------------------------------- logging / time

void plat_log(const char *fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&g_log_lock);
    if (g_log) { fprintf(g_log, "%s\n", buf); fflush(g_log); }
    pthread_mutex_unlock(&g_log_lock);
    NSLog(@"%s", buf);
}

void plat_fatal(const char *fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    plat_log("FATAL: %s", buf);
    NSString *msg = [NSString stringWithUTF8String:buf];
    dispatch_async(dispatch_get_main_queue(), ^{
        [[NSNotificationCenter defaultCenter] postNotificationName:@"NFS4Fatal" object:msg];
    });
    for (;;) sleep(1000);   // keep the message on screen; the game threads are dead anyway
}

// ---------------------------------------------------------------- crash report
// A crash writes where it happened to nfs4host.log: the host addresses as offsets into this image
// (looked up in the CI link map, NFS4-LinkMap.txt) and the guest thread's last API and call stack.

static void log_host_addr(const char *label, uintptr_t p) {
    Dl_info di;
    if (dladdr((void *)p, &di) && di.dli_fbase)
        plat_log("  %s %p = %s+0x%lx (%s)", label, (void *)p, strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname,
                 (unsigned long)(p - (uintptr_t)di.dli_fbase), di.dli_sname ? di.dli_sname : "?");
    else
        plat_log("  %s %p", label, (void *)p);
}

static void crash_handler(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    plat_log("CRASH: signal %d, fault address %p", sig, si->si_addr);
    log_host_addr("pc", (uintptr_t)__darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss));
    log_host_addr("lr", (uintptr_t)__darwin_arm_thread_state64_get_lr(uc->uc_mcontext->__ss));
    void *bt[48];
    int n = backtrace(bt, 48);
    for (int i = 0; i < n; i++) {
        char label[8];
        snprintf(label, sizeof label, "#%d", i);
        log_host_addr(label, (uintptr_t)bt[i]);
    }
    Cpu *c = rt_current_cpu();
    if (c) {
        plat_log("guest thread %u, last API %s, esp %08X", c->thread_id, c->hle_name ? c->hle_name : "-", c->esp);
        rt_backtrace(c);
    }
    rt_dump_threads();
    signal(sig, SIG_DFL);                      // let iOS write its own crash report too
    raise(sig);
}

static void objc_exception_handler(NSException *e) {
    plat_log("CRASH: Objective-C exception %s: %s", e.name.UTF8String, e.reason.UTF8String);
    for (NSString *s in e.callStackSymbols) plat_log("  %s", s.UTF8String);
}

static void install_crash_handlers(void) {
    struct sigaction sa = { 0 };
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP };
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) sigaction(sigs[i], &sa, NULL);
    NSSetUncaughtExceptionHandler(objc_exception_handler);
}

uint64_t plat_ticks_us(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom / 1000;
}

void plat_sleep_ms(uint32_t ms) {
    if (!ms) { sched_yield(); return; }
    usleep(ms * 1000);
}
void plat_sleep_us(uint32_t us) { usleep(us); }

// 60 fps, or 30 in the power-saving setting or once iOS reports the phone is getting hot
static volatile int g_fps_setting = 60;
int plat_frame_cap(void) {
    if (NSProcessInfo.processInfo.thermalState >= NSProcessInfoThermalStateSerious) return 30;
    return g_fps_setting;
}

void *plat_reserve(size_t size) {
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

// ---------------------------------------------------------------- threads / sync

struct PlatThread { pthread_t t; };
typedef struct { void (*fn)(void *); void *arg; } Tramp;
static void *tramp(void *p) { Tramp t = *(Tramp *)p; free(p); t.fn(t.arg); return NULL; }

PlatThread *plat_thread_start(void (*fn)(void *), void *arg) {
    Tramp *t = malloc(sizeof *t);
    t->fn = fn; t->arg = arg;
    PlatThread *pt = malloc(sizeof *pt);
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 16u << 20);   // recompiled code nests one C frame per guest call
    pthread_create(&pt->t, &a, tramp, t);
    pthread_attr_destroy(&a);
    return pt;
}
uint32_t plat_thread_id(void) { uint64_t id; pthread_threadid_np(NULL, &id); return (uint32_t)id; }

struct PlatMutex { pthread_mutex_t m; };
PlatMutex *plat_mutex_new(void) { PlatMutex *m = malloc(sizeof *m); pthread_mutex_init(&m->m, NULL); return m; }
void plat_mutex_lock(PlatMutex *m) { pthread_mutex_lock(&m->m); }
void plat_mutex_unlock(PlatMutex *m) { pthread_mutex_unlock(&m->m); }

struct PlatCond { pthread_cond_t c; };
PlatCond *plat_cond_new(void) { PlatCond *c = malloc(sizeof *c); pthread_cond_init(&c->c, NULL); return c; }
static struct timespec deadline_after(uint32_t ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return ts;
}
int plat_cond_wait(PlatCond *c, PlatMutex *m, uint32_t ms) {
    if (ms == 0xFFFFFFFFu) { pthread_cond_wait(&c->c, &m->m); return 1; }
    struct timespec ts = deadline_after(ms);
    return pthread_cond_timedwait(&c->c, &m->m, &ts) == 0;
}
void plat_cond_broadcast(PlatCond *c) { pthread_cond_broadcast(&c->c); }

struct PlatEvent { pthread_mutex_t m; pthread_cond_t c; int manual, set; };
PlatEvent *plat_event_new(int manual, int initial) {
    PlatEvent *e = calloc(1, sizeof *e);
    pthread_mutex_init(&e->m, NULL);
    pthread_cond_init(&e->c, NULL);
    e->manual = manual; e->set = initial;
    return e;
}
void plat_event_set(PlatEvent *e) {
    pthread_mutex_lock(&e->m); e->set = 1; pthread_cond_broadcast(&e->c); pthread_mutex_unlock(&e->m);
}
void plat_event_reset(PlatEvent *e) { pthread_mutex_lock(&e->m); e->set = 0; pthread_mutex_unlock(&e->m); }
int plat_event_wait(PlatEvent *e, uint32_t ms) {
    pthread_mutex_lock(&e->m);
    struct timespec ts = deadline_after(ms == 0xFFFFFFFFu ? 0 : ms);
    while (!e->set) {
        if (ms == 0xFFFFFFFFu) pthread_cond_wait(&e->c, &e->m);
        else if (pthread_cond_timedwait(&e->c, &e->m, &ts) != 0) break;
    }
    int ok = e->set;
    if (ok && !e->manual) e->set = 0;
    pthread_mutex_unlock(&e->m);
    return ok;
}

// ---------------------------------------------------------------- files

const char *plat_game_dir(void) { return g_game; }
const char *plat_save_dir(void) { return g_save; }
static char g_mods[1024];
const char *plat_mods_dir(void) { return g_mods[0] ? g_mods : NULL; }
int plat_mkdir(const char *p) { return mkdir(p, 0755); }
int plat_truncate(FILE *f, long size) { return ftruncate(fileno(f), size); }

int plat_list_dir(const char *path, void (*fn)(const char *, int, uint32_t, void *), void *ctx) {
    DIR *d = opendir(path);
    if (!d) return 0;
    struct dirent *e;
    fn(".", 1, 0, ctx);
    fn("..", 1, 0, ctx);
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char full[2048];
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        fn(e->d_name, S_ISDIR(st.st_mode), (uint32_t)st.st_size, ctx);
    }
    closedir(d);
    return 1;
}

void plat_local_time(uint16_t out[8]) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    out[0] = (uint16_t)(tm.tm_year + 1900); out[1] = (uint16_t)(tm.tm_mon + 1); out[2] = (uint16_t)tm.tm_wday;
    out[3] = (uint16_t)tm.tm_mday; out[4] = (uint16_t)tm.tm_hour; out[5] = (uint16_t)tm.tm_min;
    out[6] = (uint16_t)tm.tm_sec; out[7] = (uint16_t)(ts.tv_nsec / 1000000);
}

// ---------------------------------------------------------------- video

static pthread_mutex_t g_frame_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t *g_frame;
static int g_fw, g_fh;
static volatile int g_frame_serial;

void plat_present(const uint32_t *rgba, int w, int h) {
    pthread_mutex_lock(&g_frame_lock);
    if (w != g_fw || h != g_fh) { free(g_frame); g_frame = malloc((size_t)w * h * 4); g_fw = w; g_fh = h; }
    memcpy(g_frame, rgba, (size_t)w * h * 4);
    g_frame_serial++;
    pthread_mutex_unlock(&g_frame_lock);
}

int plat_pump(void) { return 1; }

// ---------------------------------------------------------------- audio

static OSStatus audio_render(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus,
                             UInt32 frames, AudioBufferList *io) {
    (void)ref; (void)flags; (void)ts; (void)bus;
    hle_audio_mix((float *)io->mBuffers[0].mData, (int)frames, 44100);
    return noErr;
}

static void start_audio(void) {
    [[AVAudioSession sharedInstance] setCategory:AVAudioSessionCategoryAmbient error:nil];
    [[AVAudioSession sharedInstance] setActive:YES error:nil];
    AudioComponentDescription d = { kAudioUnitType_Output, kAudioUnitSubType_RemoteIO, kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponent comp = AudioComponentFindNext(NULL, &d);
    AudioUnit unit;
    if (AudioComponentInstanceNew(comp, &unit) != noErr) return;
    AudioStreamBasicDescription f = { 0 };
    f.mSampleRate = 44100; f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    f.mChannelsPerFrame = 2; f.mBitsPerChannel = 32; f.mFramesPerPacket = 1;
    f.mBytesPerFrame = 8; f.mBytesPerPacket = 8;
    AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
    AURenderCallbackStruct cb = { audio_render, NULL };
    AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    AudioUnitInitialize(unit);
    AudioOutputUnitStart(unit);
}

// ---------------------------------------------------------------- input helpers

// The race reads Player 1 from the joystick ("Touch Wheel", hle_dinput.c): with a joystick present
// the game binds steering to its X axis, accelerate/brake to Y and the other actions to buttons
// A..H (look behind stays on the keyboard). So every key the overlay or the pad presses is also
// given to the joystick, and the analog sources (touch stick, pad stick and triggers) add to it.
// On menu screens the keys alone navigate, and the joystick rests so nothing moves twice.
static float g_touch_steer, g_pad_steer, g_pad_throttle;   // -1 .. 1, set on the main thread
static BOOL g_cruise;                                       // auto-accelerate toggle (set_cruise)

static void update_joy(void) {
    if (hle_glide_in_menu()) { plat_joy_x = plat_joy_y = 0; plat_joy_buttons = 0; return; }
    float kx = (plat_keys[0xCD] ? 1.f : 0.f) - (plat_keys[0xCB] ? 1.f : 0.f);       // arrows
    float ky = (plat_keys[0xD0] ? 1.f : 0.f) - (plat_keys[0xC8] || g_cruise ? 1.f : 0.f);   // up = accelerate
    float x = kx + g_touch_steer + g_pad_steer, y = ky + g_pad_throttle;
    plat_joy_x = x < -1 ? -1 : x > 1 ? 1 : x;
    plat_joy_y = y < -1 ? -1 : y > 1 ? 1 : y;
    // buttons A..H: handbrake, camera, shift up, shift down, horn/siren, reset, spike strip, lights
    static const uint8_t button_keys[8] = { 0x39, 0x2E, 0x1E, 0x2C, 0x23, 0x13, 0x1F, 0x26 };
    uint32_t b = 0;
    for (int i = 0; i < 8; i++) if (plat_keys[button_keys[i]]) b |= 1u << i;
    plat_joy_buttons = b;
}

static void key(uint8_t sc, int down) {
    if (!!plat_keys[sc] == !!down) return;
    plat_keys[sc] = down ? 0x80 : 0;
    hle_key_event(sc, down);
    update_joy();
}

// ---------------------------------------------------------------- game thread

static volatile int g_game_running;

static void game_main(void *p) {
    (void)p;
    g_game_running = 1;
    rt_boot();
    plat_log("game ended");
    exit(0);
}

// Find a path ignoring case (the game folder comes from Windows, iOS file systems are case sensitive).
static NSString *find_ci(NSString *dir, NSString *name) {
    for (NSString *e in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:nil])
        if ([e caseInsensitiveCompare:name] == NSOrderedSame) return [dir stringByAppendingPathComponent:e];
    return nil;
}

// Data root shared by the iOS port apps (E:\ios-ports\README.md): the app's Documents, or
// <LiveContainer Documents>/Games/<PortDataFolder> inside LiveContainer, whose per-app
// Data/Application/<UUID> folders are hard to find in the Files app.
static NSString *port_data_root(void) {
    static NSString *root;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        root = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
        NSString *home = NSHomeDirectory();
        NSRegularExpression *re = [NSRegularExpression
            regularExpressionWithPattern:@"^(.*/(Documents|LiveContainer))/Data/Application/[0-9A-Fa-f-]{36}/?$"
                                 options:0 error:nil];
        NSTextCheckingResult *m = [re firstMatchInString:home options:0 range:NSMakeRange(0, home.length)];
        if (m) {
            NSDictionary *info = NSBundle.mainBundle.infoDictionary;
            NSString *name = info[@"PortDataFolder"] ?: info[@"CFBundleName"] ?: @"Game";
            root = [[[home substringWithRange:[m rangeAtIndex:1]] stringByAppendingPathComponent:@"Games"]
                    stringByAppendingPathComponent:name];
        }
        [NSFileManager.defaultManager createDirectoryAtPath:[root stringByAppendingPathComponent:@"game"]
                                withIntermediateDirectories:YES attributes:nil error:nil];
    });
    return root;
}

// The PC game is copied into <root>/game; the folder holding nfs4.exe is searched breadth-first
// a few levels down (an unzipped release lands one or two folders deeper). Older builds used the
// root itself, which is searched last.
static NSString *locate_game_dir(void) {
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *root = port_data_root();
    for (NSString *top in @[ [root stringByAppendingPathComponent:@"game"], root ]) {
        NSMutableArray<NSString *> *level = [NSMutableArray arrayWithObject:top];
        for (int depth = 0; depth <= 6 && level.count; depth++) {
            NSMutableArray<NSString *> *next = [NSMutableArray array];
            for (NSString *dir in level) {
                if (find_ci(dir, @"nfs4.exe")) return dir;
                for (NSString *e in [fm contentsOfDirectoryAtPath:dir error:nil]) {
                    NSString *p = [dir stringByAppendingPathComponent:e];
                    BOOL isDir = NO;
                    if ([fm fileExistsAtPath:p isDirectory:&isDir] && isDir && ![e isEqualToString:@"save"])
                        [next addObject:p];
                }
            }
            level = next;
        }
    }
    return nil;
}

// ================================================================ UI

@interface GameView : MTKView <MTKViewDelegate>
@property(nonatomic) id<MTLCommandQueue> queue;
@property(nonatomic) id<MTLRenderPipelineState> pipeline;
@property(nonatomic) id<MTLTexture> texture;
@property(nonatomic) int lastSerial;
@property(nonatomic) CGRect gameRect;     // where the 4:3 picture is drawn, in points
@end

static NSString *const kShader =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "struct V { float4 p [[position]]; float2 uv; };\n"
     "vertex V vs(uint id [[vertex_id]], constant float4 &r [[buffer(0)]]) {\n"
     "  float2 q = float2(id & 1, id >> 1);\n"
     "  V o; o.uv = float2(q.x, q.y);\n"
     "  o.p = float4(r.x + q.x * r.z, r.y - q.y * r.w, 0, 1); return o; }\n"
     "fragment float4 fs(V i [[stage_in]], texture2d<float> t [[texture(0)]]) {\n"
     "  constexpr sampler s(filter::linear); return float4(t.sample(s, i.uv).rgb, 1); }\n";

@implementation GameView

- (instancetype)initWithFrame:(CGRect)f {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    self = [super initWithFrame:f device:dev];
    self.delegate = self;
    self.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    self.preferredFramesPerSecond = 60;
    self.multipleTouchEnabled = YES;
    _queue = [dev newCommandQueue];
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:kShader options:nil error:&err];
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"vs"];
    pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
    pd.colorAttachments[0].pixelFormat = self.colorPixelFormat;
    _pipeline = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    return self;
}

- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size {}

- (void)drawInMTKView:(MTKView *)view {
    id<MTLTexture> gpuTex = gpu_metal_display_texture();
    pthread_mutex_lock(&g_frame_lock);
    if (!gpuTex && g_frame && g_frame_serial != _lastSerial) {
        if (!_texture || (int)_texture.width != g_fw || (int)_texture.height != g_fh) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                          width:g_fw height:g_fh mipmapped:NO];
            _texture = [self.device newTextureWithDescriptor:td];
        }
        [_texture replaceRegion:MTLRegionMake2D(0, 0, g_fw, g_fh) mipmapLevel:0 withBytes:g_frame bytesPerRow:g_fw * 4];
        _lastSerial = g_frame_serial;
    }
    pthread_mutex_unlock(&g_frame_lock);

    MTLRenderPassDescriptor *rp = view.currentRenderPassDescriptor;
    id<CAMetalDrawable> drawable = view.currentDrawable;
    if (!rp || !drawable) return;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    id<MTLCommandBuffer> cb = [_queue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    id<MTLTexture> show = gpuTex ? gpuTex : _texture;
    if (show) {
        // Fit the picture at its own aspect (4:3 menus, 16:9 races) inside the area left and
        // right of the Dynamic Island; top/bottom insets (home indicator) are ignored.
        CGSize s = self.bounds.size;
        UIEdgeInsets in = self.safeAreaInsets;
        CGRect safe = CGRectMake(in.left, 0, s.width - in.left - in.right, s.height);
        CGFloat want = (CGFloat)show.width / (CGFloat)show.height, gw, gh;
        if (safe.size.width / safe.size.height > want) { gh = safe.size.height; gw = gh * want; }
        else { gw = safe.size.width; gh = gw / want; }
        CGRect g = CGRectMake(CGRectGetMidX(safe) - gw / 2, CGRectGetMidY(safe) - gh / 2, gw, gh);
        _gameRect = g;
        float rect[4] = { (float)(g.origin.x / s.width * 2 - 1), (float)(1 - g.origin.y / s.height * 2),
                          (float)(gw / s.width * 2), (float)(gh / s.height * 2) };
        [enc setRenderPipelineState:_pipeline];
        [enc setVertexBytes:rect length:sizeof rect atIndex:0];
        [enc setFragmentTexture:show atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    }
    [enc endEncoding];
    [cb presentDrawable:drawable];
    [cb commit];
}

// a tap on the picture becomes a mouse click at the corresponding game position
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (g_overlay_editing) return;
    UITouch *t = touches.anyObject;
    CGPoint p = [t locationInView:self];
    int gw = 0, gh = 0;
    hle_glide_size(&gw, &gh);                  // the game's current Glide resolution
    if (CGRectContainsPoint(_gameRect, p) && gw) {
        int x = (int)((p.x - _gameRect.origin.x) / _gameRect.size.width * gw);
        int y = (int)((p.y - _gameRect.origin.y) / _gameRect.size.height * gh);
        plat_mouse_click_at(x, y);
    }
}
@end

// Overlay look: frosted translucent "glass" with a hairline edge, white SF Symbols and small captions.
static UIImage *symbol(NSString *name, NSString *fallback, CGFloat pt) {
    UIImageSymbolConfiguration *c = [UIImageSymbolConfiguration configurationWithPointSize:pt weight:UIImageSymbolWeightSemibold];
    UIImage *img = [UIImage systemImageNamed:name withConfiguration:c];
    if (!img && fallback) img = [UIImage systemImageNamed:fallback withConfiguration:c];   // older iOS
    return img;
}

static void pin_edges(UIView *v, UIView *to) {
    v.translatesAutoresizingMaskIntoConstraints = NO;
    [NSLayoutConstraint activateConstraints:@[
        [v.leadingAnchor constraintEqualToAnchor:to.leadingAnchor], [v.trailingAnchor constraintEqualToAnchor:to.trailingAnchor],
        [v.topAnchor constraintEqualToAnchor:to.topAnchor], [v.bottomAnchor constraintEqualToAnchor:to.bottomAnchor],
    ]];
}

static UIView *glass_in(UIView *parent, CGFloat radius) {
    UIVisualEffectView *v = [[UIVisualEffectView alloc] initWithEffect:[UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemUltraThinMaterialDark]];
    v.userInteractionEnabled = NO;
    v.layer.cornerRadius = radius;
    v.layer.cornerCurve = kCACornerCurveContinuous;
    v.layer.borderWidth = 0.5;
    v.layer.borderColor = [UIColor colorWithWhite:1 alpha:0.3].CGColor;
    v.clipsToBounds = YES;
    [parent insertSubview:v atIndex:0];
    pin_edges(v, parent);
    return v;
}

// an icon with an optional caption under it, centred in its parent
static void icon_in(UIView *parent, UIImage *img, NSString *caption) {
    UIImageView *iv = [[UIImageView alloc] initWithImage:img];
    iv.tintColor = [UIColor colorWithWhite:1 alpha:0.92];
    UIStackView *st = [[UIStackView alloc] initWithArrangedSubviews:@[ iv ]];
    st.axis = UILayoutConstraintAxisVertical;
    st.alignment = UIStackViewAlignmentCenter;
    st.spacing = 3;
    st.userInteractionEnabled = NO;
    if (caption) {
        UILabel *l = [UILabel new];
        l.text = caption;
        l.font = [UIFont systemFontOfSize:10 weight:UIFontWeightSemibold];
        l.textColor = [UIColor colorWithWhite:1 alpha:0.7];
        [st addArrangedSubview:l];
    }
    st.translatesAutoresizingMaskIntoConstraints = NO;
    [parent addSubview:st];
    [st.centerXAnchor constraintEqualToAnchor:parent.centerXAnchor].active = YES;
    [st.centerYAnchor constraintEqualToAnchor:parent.centerYAnchor].active = YES;
}

static UIImpactFeedbackGenerator *tap_haptic(void) {
    static UIImpactFeedbackGenerator *h;
    if (!h) h = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];
    return h;
}

// what the overlay editor changes on every control
@protocol OverlayItem
@property(nonatomic) CGFloat scale;      // applied as a transform, so icons and corners scale too
@property(nonatomic) BOOL haptic;
@property(nonatomic) CGFloat radius;
@end

// on-screen buttons that hold a DirectInput scan code while touched
@interface KeyButton : UIView <OverlayItem>
@property(nonatomic) uint8_t scan;
@property(nonatomic) UIView *glow;
@end

static void set_cruise(BOOL on);

@implementation KeyButton
@synthesize scale = _scale, haptic = _haptic, radius = _radius;
- (instancetype)initWithSymbol:(UIImage *)img caption:(NSString *)caption scan:(uint8_t)scan radius:(CGFloat)r {
    self = [super initWithFrame:CGRectZero];
    _scan = scan;
    _scale = 1;
    _haptic = YES;
    _radius = r;
    glass_in(self, r);
    _glow = [UIView new];
    _glow.backgroundColor = UIColor.whiteColor;
    _glow.alpha = 0;
    _glow.layer.cornerRadius = r;
    _glow.layer.cornerCurve = kCACornerCurveContinuous;
    _glow.userInteractionEnabled = NO;
    [self addSubview:_glow];
    pin_edges(_glow, self);
    icon_in(self, img, caption);
    self.multipleTouchEnabled = YES;
    return self;
}
- (void)press:(BOOL)down {
    if (g_overlay_editing) return;
    if (down && _scan == 0xD0) set_cruise(NO);         // braking ends auto-accelerate
    key(_scan, down);
    if (down && _haptic) [tap_haptic() impactOccurred];
    CGFloat s = down ? _scale * 0.92 : _scale;
    [UIView animateWithDuration:down ? 0.06 : 0.15 delay:0 options:UIViewAnimationOptionBeginFromCurrentState
                     animations:^{
                         self.transform = CGAffineTransformMakeScale(s, s);
                         self.glow.alpha = down ? 0.22 : 0;
                     } completion:nil];
}
// a thumb can land as two touches: the button lets go only when the last of them lifts
- (BOOL)stillTouched:(UIEvent *)e {
    for (UITouch *t in [e touchesForView:self])
        if (t.phase != UITouchPhaseEnded && t.phase != UITouchPhaseCancelled) return YES;
    return NO;
}
- (void)touchesBegan:(NSSet *)t withEvent:(UIEvent *)e { [self press:YES]; }
- (void)touchesEnded:(NSSet *)t withEvent:(UIEvent *)e { if (![self stillTouched:e]) [self press:NO]; }
- (void)touchesCancelled:(NSSet *)t withEvent:(UIEvent *)e { if (![self stillTouched:e]) [self press:NO]; }
@end

// auto-accelerate: a tap turns it on or off; while on it lights up and holds the throttle
@interface CruiseButton : KeyButton
@end
static __weak CruiseButton *g_cruise_button;

static void set_cruise(BOOL on) {
    if (g_cruise == on) return;
    g_cruise = on;
    update_joy();
    dispatch_async(dispatch_get_main_queue(), ^{
        g_cruise_button.glow.alpha = g_cruise ? 0.4 : 0;
        g_cruise_button.glow.backgroundColor = [UIColor colorWithRed:0.3 green:0.85 blue:0.45 alpha:1];
    });
}

@implementation CruiseButton
- (void)press:(BOOL)down {
    if (g_overlay_editing || !down) return;
    set_cruise(!g_cruise);
    if (self.haptic) [tap_haptic() impactOccurred];
}
@end

// steering pad for the left thumb: the finger's side of the centre holds ◀ or ▶, sliding across switches
@interface SteerPad : UIView <OverlayItem>
@property(nonatomic) UIView *leftHalf, *rightHalf, *knob, *fill;
@property(nonatomic) UITouch *active;
@property(nonatomic) CGFloat originX;
@property(nonatomic) BOOL atLimit;
@end
@implementation SteerPad
@synthesize scale = _scale, haptic = _haptic, radius = _radius;
- (instancetype)init {
    self = [super initWithFrame:CGRectZero];
    const CGFloat r = 26;
    _scale = 1;
    _haptic = YES;
    _radius = r;
    glass_in(self, r);
    self.layer.cornerRadius = r;
    self.layer.cornerCurve = kCACornerCurveContinuous;
    self.clipsToBounds = YES;
    self.multipleTouchEnabled = YES;
    NSString *names[2] = { @"chevron.left", @"chevron.right" };
    for (int i = 0; i < 2; i++) {
        UIView *h = [UIView new];
        h.backgroundColor = [UIColor colorWithWhite:1 alpha:0];
        h.userInteractionEnabled = NO;
        h.translatesAutoresizingMaskIntoConstraints = NO;
        [self addSubview:h];
        [NSLayoutConstraint activateConstraints:@[
            [h.topAnchor constraintEqualToAnchor:self.topAnchor], [h.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
            [h.widthAnchor constraintEqualToAnchor:self.widthAnchor multiplier:0.5],
            i ? [h.trailingAnchor constraintEqualToAnchor:self.trailingAnchor] : [h.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
        ]];
        icon_in(h, symbol(names[i], nil, 30), nil);
        if (i) _rightHalf = h; else _leftHalf = h;
    }
    // the bar from where the finger landed to where it is now, and the knob under the finger
    _fill = [UIView new];
    _fill.backgroundColor = [UIColor colorWithWhite:1 alpha:0.3];
    _fill.layer.cornerRadius = 3;
    _fill.userInteractionEnabled = NO;
    _fill.hidden = YES;
    [self addSubview:_fill];
    _knob = [UIView new];
    _knob.backgroundColor = [UIColor colorWithWhite:1 alpha:0.35];
    _knob.layer.cornerRadius = 22;
    _knob.layer.borderWidth = 1;
    _knob.layer.borderColor = [UIColor colorWithWhite:1 alpha:0.6].CGColor;
    _knob.userInteractionEnabled = NO;
    _knob.hidden = YES;
    [self addSubview:_knob];
    return self;
}

// Pressing and sliding both steer: the finger's place relative to the pad's centre sets the
// steering, so a press on ◀ or ▶ (a quarter of the width out) is full lock like a button, a press
// nearer the middle steers less, and sliding changes it continuously. The curve is gentle near
// the centre for small corrections. The value goes to the joystick X axis (update_joy).
- (void)steer:(UIEvent *)e {
    if (g_overlay_editing) e = nil;
    NSSet<UITouch *> *touches = [e touchesForView:self];
    if (_active && (![touches containsObject:_active] || _active.phase == UITouchPhaseEnded || _active.phase == UITouchPhaseCancelled))
        _active = nil;
    if (!_active)
        for (UITouch *t in touches)
            if (t.phase != UITouchPhaseEnded && t.phase != UITouchPhaseCancelled) {
                _active = t;
                if (_haptic) [tap_haptic() impactOccurred];
                break;
            }
    CGFloat w = self.bounds.size.width, h = self.bounds.size.height, full = w * 0.25, v = 0;
    _originX = w / 2;
    if (_active) {
        CGFloat dx = [_active locationInView:self].x - _originX;
        CGFloat d = fmax(-1, fmin(1, dx / full)), a = fabs(d);
        v = a < 0.06 ? 0 : copysign(pow((a - 0.06) / 0.94, 1.4), d);
        CGFloat kx = _originX + d * full;
        _knob.frame = CGRectMake(kx - 22, h / 2 - 22, 44, 44);
        _fill.frame = CGRectMake(fmin(_originX, kx), h / 2 - 3, fabs(kx - _originX), 6);
    }
    _knob.hidden = _fill.hidden = !_active;
    BOOL limit = fabs(v) > 0.999;
    if (limit && !_atLimit && _haptic) [tap_haptic() impactOccurred];
    _atLimit = limit;
    g_touch_steer = (float)v;
    update_joy();
    _leftHalf.backgroundColor = [UIColor colorWithWhite:1 alpha:v < 0 ? -v * 0.25 : 0];
    _rightHalf.backgroundColor = [UIColor colorWithWhite:1 alpha:v > 0 ? v * 0.25 : 0];
}
- (void)touchesBegan:(NSSet *)t withEvent:(UIEvent *)e { [self steer:e]; }
- (void)touchesMoved:(NSSet *)t withEvent:(UIEvent *)e { [self steer:e]; }
- (void)touchesEnded:(NSSet *)t withEvent:(UIEvent *)e { [self steer:e]; }
- (void)touchesCancelled:(NSSet *)t withEvent:(UIEvent *)e { [self steer:e]; }
- (void)releaseAll { [self steer:nil]; }
@end

// container for the buttons that lets touches on empty space fall through to the game view
@interface PassthroughView : UIView
@end
@implementation PassthroughView
- (UIView *)hitTest:(CGPoint)p withEvent:(UIEvent *)e {
    UIView *v = [super hitTest:p withEvent:e];
    return v == self ? nil : v;
}
@end

// NFS4 default key table verified at guest VA 0x005BCA14 in nfs4.exe: arrows, Space handbrake,
// A/Z gear up/down, C camera, B look behind, H horn (cop: siren), L lights, R reset, S spike strip
// (cop), plus the fixed keys Esc pause, Tab map and Enter.
enum {
    K_LEFT = 0xCB, K_RIGHT = 0xCD, K_GAS = 0xC8, K_BRAKE = 0xD0, K_HAND = 0x39, K_UP = 0x1E, K_DOWN = 0x2C,
    K_CAM = 0x2E, K_BACK = 0x30, K_HORN = 0x23, K_LIGHTS = 0x26, K_RESET = 0x13, K_SPIKE = 0x1F,
    K_ESC = 0x01, K_MAP = 0x0F, K_ENTER = 0x1C,
};

static NSString *const kImportAddress = @"wifiImportAddress";   // last PC address for "PC에서 받기"
// Per-control overrides from the overlay editor: id -> {x, y: centre as a fraction of the screen,
// s: scale, h: haptics}. Controls without an entry sit at their default place.
static NSString *const kOverlayLayout = @"overlay";
static NSString *const kOverlayAlpha = @"overlayAlpha";
static NSString *const kImportMarker = @".pcimport-incomplete";  // in <data root>/game while a transfer is unfinished

// a "PC에서 받기" transfer that has not finished: its marker, or a leftover *.part file
static BOOL import_incomplete(void) {
    NSString *game = [port_data_root() stringByAppendingPathComponent:@"game"];
    NSFileManager *fm = NSFileManager.defaultManager;
    if ([fm fileExistsAtPath:[game stringByAppendingPathComponent:kImportMarker]]) return YES;
    for (NSString *rel in [fm enumeratorAtPath:game])
        if ([rel.pathExtension isEqualToString:@"part"]) return YES;
    return NO;
}

@interface GameViewController : UIViewController <UIDocumentPickerDelegate, UIGestureRecognizerDelegate, UITextFieldDelegate>
@property(nonatomic) GameView *gameView;
@property(nonatomic) UIView *controls;
@property(nonatomic) NSMutableDictionary<NSString *, UIView<OverlayItem> *> *items;
@property(nonatomic) NSMutableDictionary *layout;
@property(nonatomic) NSMutableArray<UIGestureRecognizer *> *editGestures;
@property(nonatomic) SteerPad *steerPad;
@property(nonatomic) NSMutableArray<KeyButton *> *menuButtons;
@property(nonatomic) BOOL menuMode, padConnected;
@property(nonatomic) UIButton *gear, *kbd;
@property(nonatomic) UIView *editPanel;
@property(nonatomic) UILabel *editTitle;
@property(nonatomic) UISlider *sizeSlider, *alphaSlider;
@property(nonatomic) UISwitch *hapticSwitch;
@property(nonatomic) NSString *selected;
@property(nonatomic) UIStackView *setup;
@property(nonatomic) UILabel *info, *pcStatus;
@property(nonatomic) UIButton *importButton, *pcButton, *closeButton;
@property(nonatomic) UITextField *pcField;
@property(nonatomic) UIProgressView *pcBar;
@end

@implementation GameViewController

- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
// The pedals sit at the screen's edges: without this, a thumb that moves a little near the bottom
// starts iOS's home gesture, which cancels the touch and lets go of the throttle mid-race.
// Deferred, the first swipe only shows the home indicator and a second one leaves the game.
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures { return UIRectEdgeAll; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;
    CGSize nb = UIScreen.mainScreen.nativeBounds.size;
    g_screen_height_px = (int)MIN(nb.width, nb.height);
    [[NSNotificationCenter defaultCenter] addObserverForName:@"NFS4Fatal" object:nil queue:nil
                                                  usingBlock:^(NSNotification *n) { [self showMessage:n.object import:NO]; }];
    [[NSUserDefaults standardUserDefaults] registerDefaults:@{ @"wide" : @YES, @"msaa" : @YES }];
}

// started here rather than in viewDidLoad: the safe area (Dynamic Island) is known by now
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    static BOOL started;
    if (!started) { started = YES; [self startOrAsk]; }
}

- (void)applySettings {
    NSUserDefaults *d = [NSUserDefaults standardUserDefaults];
    CGSize s = self.view.bounds.size;
    UIEdgeInsets in = self.view.safeAreaInsets;
    CGFloat w = MAX(s.width, s.height) - in.left - in.right, h = MIN(s.width, s.height);
    g_wide_aspect = [d boolForKey:@"wide"] && h > 0 ? (float)(w / h) : 0;
    g_msaa_samples = [d boolForKey:@"msaa"] ? 4 : 1;
    g_fps_setting = [d boolForKey:@"fps30"] ? 30 : 60;
    plat_log("display: %.0fx%.0f pt, usable aspect %.3f, wide %s, MSAA %dx, %d fps", s.width, s.height, w / h,
             g_wide_aspect > 0 ? "on" : "off", g_msaa_samples, g_fps_setting);
}

- (void)showSettings {
    NSUserDefaults *d = [NSUserDefaults standardUserDefaults];
    UIAlertController *a = [UIAlertController alertControllerWithTitle:[NSString stringWithFormat:@"설정 · NFS4 %@", app_version()]
        message:@"화면비·안티에일리어싱은 다음 실행부터 적용됩니다.\n와이드: 레이스를 최대 16:9로 넓게 렌더링 (메뉴는 원본 4:3)" preferredStyle:UIAlertControllerStyleActionSheet];
    BOOL wide = [d boolForKey:@"wide"], msaa = [d boolForKey:@"msaa"];
    [a addAction:[UIAlertAction actionWithTitle:wide ? @"화면비: 와이드 ✓ → 원본 4:3로" : @"화면비: 원본 4:3 ✓ → 와이드로"
        style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) { [d setBool:!wide forKey:@"wide"]; }]];
    [a addAction:[UIAlertAction actionWithTitle:msaa ? @"안티에일리어싱 4x ✓ → 끄기" : @"안티에일리어싱 꺼짐 → 4x 켜기"
        style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) { [d setBool:!msaa forKey:@"msaa"]; }]];
    BOOL fps30 = [d boolForKey:@"fps30"];
    [a addAction:[UIAlertAction actionWithTitle:fps30 ? @"절전 30fps ✓ → 60fps로 (바로 적용)" : @"60fps ✓ → 절전 30fps로 (바로 적용)"
        style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) {
            [d setBool:!fps30 forKey:@"fps30"];
            g_fps_setting = fps30 ? 60 : 30;
        }]];
    [a addAction:[UIAlertAction actionWithTitle:[NSString stringWithFormat:@"버튼 투명도 (지금 %.0f%%)", [self overlayAlpha] * 100]
        style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) { [self askOverlayAlpha]; }]];
    [a addAction:[UIAlertAction actionWithTitle:@"PC에서 받기 (게임 파일 업데이트)" style:UIAlertActionStyleDefault
        handler:^(UIAlertAction *x) { [self showPCUpdate]; }]];
    [a addAction:[UIAlertAction actionWithTitle:@"키보드 입력 (이름·코드)" style:UIAlertActionStyleDefault
        handler:^(UIAlertAction *x) { [self askText]; }]];
    [a addAction:[UIAlertAction actionWithTitle:@"터치 버튼 편집 (위치·크기·진동·투명도)" style:UIAlertActionStyleDefault
        handler:^(UIAlertAction *x) { [self startEditing]; }]];
    [a addAction:[UIAlertAction actionWithTitle:@"닫기" style:UIAlertActionStyleCancel handler:nil]];
    a.popoverPresentationController.sourceView = self.view;
    a.popoverPresentationController.sourceRect = CGRectMake(self.view.bounds.size.width / 2, 40, 1, 1);
    [self presentViewController:a animated:YES completion:nil];
}

// "1.0.0 (빌드 31)" from Info.plist, which CI fills from APP_VERSION and the run number
static NSString *app_version(void) {
    NSDictionary *i = NSBundle.mainBundle.infoDictionary;
    return [NSString stringWithFormat:@"%@ (빌드 %@)", i[@"CFBundleShortVersionString"] ?: @"?", i[@"CFBundleVersion"] ?: @"?"];
}

// quick opacity presets for the touch buttons (the editor has a slider for finer steps)
- (void)askOverlayAlpha {
    UIAlertController *a = [UIAlertController alertControllerWithTitle:@"버튼 투명도"
        message:@"낮을수록 화면이 잘 보입니다. 세밀한 조절은 '터치 버튼 편집'의 투명도 슬라이더로." preferredStyle:UIAlertControllerStyleActionSheet];
    for (NSNumber *v in @[ @1.0, @0.75, @0.6, @0.45, @0.3, @0.15 ]) {
        [a addAction:[UIAlertAction actionWithTitle:[NSString stringWithFormat:@"%.0f%%", v.doubleValue * 100]
            style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) {
                [[NSUserDefaults standardUserDefaults] setDouble:v.doubleValue forKey:kOverlayAlpha];
                self.controls.alpha = v.doubleValue;
            }]];
    }
    [a addAction:[UIAlertAction actionWithTitle:@"닫기" style:UIAlertActionStyleCancel handler:nil]];
    a.popoverPresentationController.sourceView = self.view;
    a.popoverPresentationController.sourceRect = CGRectMake(self.view.bounds.size.width / 2, 40, 1, 1);
    [self presentViewController:a animated:YES completion:nil];
}

// DirectInput scan codes of the characters the game's text fields take
static uint8_t char_scan(unichar ch) {
    static const char *rows[] = { "1234567890-", "qwertyuiop", "asdfghjkl", "zxcvbnm" };
    static const uint8_t first[] = { 0x02, 0x10, 0x1E, 0x2C };
    ch = (unichar)tolower(ch);
    for (int r = 0; r < 4; r++) {
        const char *p = strchr(rows[r], ch);
        if (ch && p) return (uint8_t)(first[r] + (p - rows[r]));
    }
    if (ch == ' ') return 0x39;
    if (ch == '.') return 0x34;
    return 0;
}

// types text into the game as key presses (player names, codes typed in the menus)
- (void)askText {
    UIAlertController *a = [UIAlertController alertControllerWithTitle:@"키보드 입력"
        message:@"입력한 글자를 게임에 키보드로 보냅니다.\n이름 바꾸기: 메인 메뉴의 'Player 1 Name'을 눌러 이름 창을 연 뒤 여기서 새 이름을 넣고 '지우고 보내고 Enter'를 누르세요."
                 preferredStyle:UIAlertControllerStyleAlert];
    [a addTextFieldWithConfigurationHandler:^(UITextField *f) {
        f.autocorrectionType = UITextAutocorrectionTypeNo;
        f.autocapitalizationType = UITextAutocapitalizationTypeNone;
        f.keyboardType = UIKeyboardTypeASCIICapable;
    }];
    __weak UIAlertController *weak = a;
    void (^send)(BOOL, BOOL) = ^(BOOL clear, BOOL enter) {
        NSString *text = weak.textFields.firstObject.text ?: @"";
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            // a name box opens full with the old name (up to 10 letters): Backspace it away first
            if (clear) for (int i = 0; i < 16; i++) { key(0x0E, 1); usleep(30000); key(0x0E, 0); usleep(30000); }
            for (NSUInteger i = 0; i < text.length; i++) {
                unichar ch = [text characterAtIndex:i];
                uint8_t sc = char_scan(ch);
                if (!sc) continue;
                BOOL upper = ch >= 'A' && ch <= 'Z';
                if (upper) key(0x2A, 1);
                key(sc, 1); usleep(40000); key(sc, 0); usleep(40000);
                if (upper) key(0x2A, 0);
            }
            if (enter) { key(K_ENTER, 1); usleep(40000); key(K_ENTER, 0); }
        });
    };
    [a addAction:[UIAlertAction actionWithTitle:@"취소" style:UIAlertActionStyleCancel handler:nil]];
    [a addAction:[UIAlertAction actionWithTitle:@"지우고 보내고 Enter (이름 바꾸기)" style:UIAlertActionStyleDefault
                                        handler:^(UIAlertAction *x) { send(YES, YES); }]];
    [a addAction:[UIAlertAction actionWithTitle:@"보내기" style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) { send(NO, NO); }]];
    [a addAction:[UIAlertAction actionWithTitle:@"보내고 Enter" style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) { send(NO, YES); }]];
    [self presentViewController:a animated:YES completion:nil];
}

// The setup screen: a message and, when the game is missing, the two ways to bring it in:
// "PC에서 받기" over Wi-Fi (E:\ios-ports\README.md section 9) or copying a folder from Files.
- (void)showMessage:(NSString *)text import:(BOOL)import {
    if (!_setup) {
        _info = [UILabel new];
        _info.numberOfLines = 0;
        _info.textColor = UIColor.whiteColor;
        _info.font = [UIFont systemFontOfSize:14];

        _pcField = [UITextField new];
        _pcField.placeholder = @"PC 주소 (비워 두면 자동으로 찾습니다)";
        _pcField.text = [[NSUserDefaults standardUserDefaults] stringForKey:kImportAddress];
        _pcField.borderStyle = UITextBorderStyleRoundedRect;
        _pcField.keyboardType = UIKeyboardTypeURL;
        _pcField.autocapitalizationType = UITextAutocapitalizationTypeNone;
        _pcField.autocorrectionType = UITextAutocorrectionTypeNo;
        _pcField.returnKeyType = UIReturnKeyGo;
        _pcField.delegate = self;
        [_pcField.widthAnchor constraintEqualToConstant:260].active = YES;
        _pcButton = [UIButton buttonWithType:UIButtonTypeSystem];
        [_pcButton setTitle:@"PC에서 받기" forState:UIControlStateNormal];
        _pcButton.titleLabel.font = [UIFont boldSystemFontOfSize:17];
        [_pcButton addTarget:self action:@selector(startPCImport) forControlEvents:UIControlEventTouchUpInside];
        UIStackView *pcRow = [[UIStackView alloc] initWithArrangedSubviews:@[ _pcField, _pcButton ]];
        pcRow.spacing = 12;
        pcRow.alignment = UIStackViewAlignmentCenter;

        _pcBar = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];
        _pcBar.hidden = YES;
        [_pcBar.widthAnchor constraintEqualToConstant:380].active = YES;
        _pcStatus = [UILabel new];
        _pcStatus.numberOfLines = 0;
        _pcStatus.textAlignment = NSTextAlignmentCenter;
        _pcStatus.textColor = [UIColor colorWithWhite:1 alpha:0.7];
        _pcStatus.font = [UIFont systemFontOfSize:13];

        _importButton = [UIButton buttonWithType:UIButtonTypeSystem];
        [_importButton setTitle:@"파일 앱에서 게임 폴더 가져오기" forState:UIControlStateNormal];
        _importButton.titleLabel.font = [UIFont systemFontOfSize:15];
        [_importButton addTarget:self action:@selector(pickFolder) forControlEvents:UIControlEventTouchUpInside];

        _closeButton = [UIButton buttonWithType:UIButtonTypeSystem];   // when opened from ⚙︎ during a game
        [_closeButton setTitle:@"닫기" forState:UIControlStateNormal];
        _closeButton.titleLabel.font = [UIFont systemFontOfSize:15];
        [_closeButton addTarget:self action:@selector(closeSetup) forControlEvents:UIControlEventTouchUpInside];

        _setup = [[UIStackView alloc] initWithArrangedSubviews:@[ _info, pcRow, _pcBar, _pcStatus, _importButton, _closeButton ]];
        _setup.axis = UILayoutConstraintAxisVertical;
        _setup.alignment = UIStackViewAlignmentCenter;
        _setup.spacing = 12;
        [_setup setCustomSpacing:20 afterView:_info];
        _setup.translatesAutoresizingMaskIntoConstraints = NO;
        [self.view addSubview:_setup];
        [NSLayoutConstraint activateConstraints:@[
            [_setup.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
            [_setup.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor],
            [_setup.widthAnchor constraintLessThanOrEqualToAnchor:self.view.widthAnchor multiplier:0.85],
        ]];
    }
    [self.view bringSubviewToFront:_setup];
    _setup.hidden = NO;
    _info.text = text;
    for (UIView *v in _setup.arrangedSubviews)
        if (v != _info) v.hidden = !import;
    _pcBar.hidden = YES;
    _pcStatus.hidden = !import || !_pcStatus.text.length;
    _importButton.hidden = !import || g_game_running;     // a folder copy replaces the game; only before it runs
    _closeButton.hidden = !import || !g_game_running;
    _setup.backgroundColor = g_game_running ? [UIColor colorWithWhite:0 alpha:0.85] : UIColor.clearColor;
}

- (void)closeSetup {
    [_pcField resignFirstResponder];
    _setup.hidden = YES;
}

// ⚙︎ › PC에서 받기 while the game is installed: only files that differ from the PC's are fetched
// (e.g. the high-resolution cars the PC server adds from its overlay folder)
- (void)showPCUpdate {
    _pcStatus.text = @"";
    [self showMessage:@"PC에서 받기\n\nPC에서 'PC에서-받기-서버'를 켜고 버튼을 누르세요 (PC는 자동으로 찾습니다). PC와 다른 파일만 받습니다 "
                      "(고해상도 차량 등). 다 받은 뒤 앱을 완전히 종료했다가 다시 열면 적용됩니다."
               import:YES];
}

- (BOOL)textFieldShouldReturn:(UITextField *)field {
    [self startPCImport];
    return YES;
}

- (void)pcStatus:(NSString *)text {
    dispatch_async(dispatch_get_main_queue(), ^{
        self.pcStatus.text = text;
        self.pcStatus.hidden = NO;
    });
}

// ---- finding the PC without an address (E:\ios-ports\README.md section 9)

static NSData *fetch_data(NSURLSession *s, NSURL *url, NSInteger *status, NSError **err);

static NSURL *address_url(NSString *address) {
    NSURL *url = [NSURL URLWithString:[address containsString:@"://"] ? address : [@"http://" stringByAppendingString:address]];
    return url.host ? url : nil;
}

// serve-games answers "/" with {"apps": [...]}: true if it offers this app
static BOOL probe_address(NSURLSession *s, NSString *address, NSString *app) {
    NSURL *url = address_url(address);
    if (!url) return NO;
    NSInteger code = 0;
    NSData *data = fetch_data(s, url, &code, NULL);
    if (code != 200 || !data) return NO;
    NSDictionary *json = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    return [json isKindOfClass:NSDictionary.class] && [json[@"apps"] isKindOfClass:NSArray.class] && [json[@"apps"] containsObject:app];
}

// "a.b.c.d:port" for every other host on the Wi-Fi (en*) IPv4 subnet; a subnet larger than /22
// is cut to the phone's /24
static NSArray<NSString *> *subnet_hosts(int port) {
    NSMutableArray *hosts = [NSMutableArray array];
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list) != 0) return hosts;
    for (struct ifaddrs *ifa = list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || !ifa->ifa_netmask || ifa->ifa_addr->sa_family != AF_INET || strncmp(ifa->ifa_name, "en", 2) != 0)
            continue;
        uint32_t ip = ntohl(((struct sockaddr_in *)ifa->ifa_addr)->sin_addr.s_addr);
        uint32_t mask = ntohl(((struct sockaddr_in *)ifa->ifa_netmask)->sin_addr.s_addr);
        if (__builtin_popcount(mask) < 22) mask = 0xFFFFFF00;
        if (~mask < 2) continue;
        uint32_t net = ip & mask;
        for (uint32_t i = 1; i < ~mask; i++) {
            uint32_t h = net | i;
            if (h != ip) [hosts addObject:[NSString stringWithFormat:@"%u.%u.%u.%u:%d", h >> 24, (h >> 16) & 255, (h >> 8) & 255, h & 255, port]];
        }
    }
    freeifaddrs(list);
    return hosts;
}

// blocking: the given addresses, the PC this was set up with (LAN, Tailscale), then every host
// on the Wi-Fi subnet, 64 at a time; nil when no PC answers
static NSString *discover_pc(NSArray<NSString *> *preferred, NSString *app) {
    NSURLSessionConfiguration *config = NSURLSessionConfiguration.ephemeralSessionConfiguration;
    config.timeoutIntervalForRequest = 1.5;
    config.connectionProxyDictionary = @{};   // the PC is on the LAN: never through a VPN/proxy app
    NSURLSession *session = [NSURLSession sessionWithConfiguration:config];
    NSMutableOrderedSet<NSString *> *first = [NSMutableOrderedSet orderedSet];
    for (NSString *a in [preferred arrayByAddingObjectsFromArray:@[ @"192.168.0.2:8765" ]])
        if (a.length) [first addObject:a];
    for (NSString *a in first)
        if (probe_address(session, a, app)) { [session finishTasksAndInvalidate]; return a; }
    NSArray<NSString *> *hosts = subnet_hosts(8765);
    __block NSString *found = nil;
    NSObject *lock = [NSObject new];
    for (NSUInteger start = 0; start < hosts.count && !found; start += 64) {
        dispatch_group_t group = dispatch_group_create();
        for (NSUInteger i = start; i < MIN(start + 64, hosts.count); i++) {
            NSString *host = hosts[i];
            dispatch_group_async(group, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                @synchronized (lock) { if (found) return; }
                if (probe_address(session, host, app))
                    @synchronized (lock) { if (!found) found = host; }
            });
        }
        dispatch_group_wait(group, DISPATCH_TIME_FOREVER);
    }
    [session finishTasksAndInvalidate];
    return found;
}

- (void)startPCImport {
    NSString *typed = [_pcField.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] ?: @"";
    NSString *saved = [[NSUserDefaults standardUserDefaults] stringForKey:kImportAddress] ?: @"";
    NSString *app = NSBundle.mainBundle.infoDictionary[@"PortDataFolder"] ?: @"NFS4";
    [_pcField resignFirstResponder];
    _pcButton.enabled = _importButton.enabled = NO;
    _pcBar.hidden = NO;
    _pcBar.progress = 0;
    [self pcStatus:@"같은 와이파이에서 PC를 찾는 중…"];
    UIApplication.sharedApplication.idleTimerDisabled = YES;       // no auto-lock mid-transfer
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSString *address = discover_pc(@[ typed, saved ], app);
        NSString *error;
        if (!address) {
            error = @"PC를 찾지 못했습니다. '로컬 네트워크' 허용 창이 떴다면 허용한 뒤 다시 누르세요.\n"
                    "그래도 안 되면 확인해 주세요:\n"
                    "① PC에서 'PC에서-받기-서버.cmd'가 켜져 있나요?\n"
                    "② 휴대폰이 PC와 같은 와이파이에 있나요? (셀룰러·VPN 끄기)\n"
                    "③ 설정 › 개인정보 보호 및 보안 › 로컬 네트워크에서 LiveContainer(또는 NFS IV)가 켜져 있나요?";
        } else {
            [[NSUserDefaults standardUserDefaults] setObject:address forKey:kImportAddress];
            dispatch_async(dispatch_get_main_queue(), ^{ self.pcField.text = address; });
            [self pcStatus:[address stringByAppendingString:@"에 연결했습니다."]];
            error = [self runPCImportFrom:address_url(address)];
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            UIApplication.sharedApplication.idleTimerDisabled = NO;
            self.pcButton.enabled = self.importButton.enabled = YES;
            if (error) [self pcStatus:[error stringByAppendingString:@"\n다시 누르면 받은 데까지 건너뛰고 이어서 받습니다."]];
            else if (g_game_running) [self pcStatus:@"다 받았습니다. 앱을 완전히 종료했다가 다시 열면 적용됩니다."];
            else { [self pcStatus:@"다 받았습니다."]; [self startOrAsk]; }
        });
    });
}

// blocking request for the worker threads
static NSData *fetch_data(NSURLSession *s, NSURL *url, NSInteger *status, NSError **err) {
    __block NSData *out = nil;
    __block NSError *error = nil;
    __block NSInteger code = 0;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [[s dataTaskWithURL:url completionHandler:^(NSData *d, NSURLResponse *r, NSError *e) {
        out = d; code = [(NSHTTPURLResponse *)r statusCode]; error = e;
        dispatch_semaphore_signal(done);
    }] resume];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    *status = code;
    if (err) *err = error;
    return out;
}

// appends a downloaded piece to a .part file
static BOOL append_file(NSString *from, NSString *to) {
    NSFileHandle *in = [NSFileHandle fileHandleForReadingAtPath:from], *out = [NSFileHandle fileHandleForWritingAtPath:to];
    if (!in || !out) return NO;
    [out seekToEndOfFile];
    for (;;) {
        @autoreleasepool {
            NSData *d = [in readDataOfLength:1 << 20];
            if (!d.length) break;
            [out writeData:d];
        }
    }
    [in closeFile];
    [out closeFile];
    return YES;
}

// GET /api/manifest/NFS4, then every file not already here at its size, 4 at a time, into
// <data root>/game. Big files go through <name>.part and resume with Range. nil = success.
- (NSString *)runPCImportFrom:(NSURL *)base {
    NSString *app = NSBundle.mainBundle.infoDictionary[@"PortDataFolder"] ?: @"NFS4";
    NSString *dest = [port_data_root() stringByAppendingPathComponent:@"game"];
    NSURLSessionConfiguration *config = NSURLSessionConfiguration.defaultSessionConfiguration;
    config.timeoutIntervalForRequest = 30;
    config.HTTPMaximumConnectionsPerHost = 4;
    config.connectionProxyDictionary = @{};   // straight to the PC, past any VPN/proxy app
    NSURLSession *session = [NSURLSession sessionWithConfiguration:config];
    NSFileManager *fm = NSFileManager.defaultManager;

    NSInteger code = 0;
    NSError *err = nil;
    NSData *data = fetch_data(session, [[base URLByAppendingPathComponent:@"api/manifest"] URLByAppendingPathComponent:app], &code, &err);
    if (!data || code != 200)
        return err ? [NSString stringWithFormat:@"PC에 연결하지 못했습니다: %@", err.localizedDescription]
                   : [NSString stringWithFormat:@"PC가 %ld 오류를 보냈습니다. 서버의 games.json에 %@가 있는지 확인하세요.", (long)code, app];
    NSDictionary *manifest = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    NSArray<NSDictionary *> *files = manifest[@"files"];
    long long total = [manifest[@"total"] longLongValue];
    if (!files.count) return @"PC가 보낸 파일 목록이 비어 있습니다.";

    // until every file is here the game must not start on half-copied data (see import_incomplete)
    [fm createDirectoryAtPath:dest withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *marker = [dest stringByAppendingPathComponent:kImportMarker];
    [fm createFileAtPath:marker contents:[NSData data] attributes:nil];

    NSMutableArray<NSDictionary *> *todo = [NSMutableArray array];
    long long have = 0;
    for (NSDictionary *f in files) {
        NSNumber *size = [fm attributesOfItemAtPath:[dest stringByAppendingPathComponent:f[@"p"]] error:nil][NSFileSize];
        if (size && size.longLongValue == [f[@"s"] longLongValue]) have += size.longLongValue;
        else [todo addObject:f];
    }

    __block long long done = have;
    __block NSString *failure = nil;
    NSObject *lock = [NSObject new];
    NSOperationQueue *queue = [NSOperationQueue new];
    queue.maxConcurrentOperationCount = 4;
    void (^report)(void) = ^{
        long long d;
        @synchronized (lock) { d = done; }
        dispatch_async(dispatch_get_main_queue(), ^{
            self.pcBar.progress = total > 0 ? (float)((double)d / (double)total) : 0;
            self.pcStatus.text = [NSString stringWithFormat:@"받는 중 %.0f / %.0f MB", d / 1e6, total / 1e6];
            self.pcStatus.hidden = NO;
        });
    };
    report();

    for (NSDictionary *f in todo) {
        [queue addOperationWithBlock:^{
            @synchronized (lock) { if (failure) return; }
            NSString *rel = f[@"p"];
            long long want = [f[@"s"] longLongValue];
            NSURL *url = [[base URLByAppendingPathComponent:@"api/file"] URLByAppendingPathComponent:app];
            for (NSString *part in [rel componentsSeparatedByString:@"/"]) url = [url URLByAppendingPathComponent:part];
            NSString *target = [dest stringByAppendingPathComponent:rel], *part = [target stringByAppendingString:@".part"];
            [fm createDirectoryAtPath:target.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
            long long partial = [[fm attributesOfItemAtPath:part error:nil][NSFileSize] longLongValue];
            if (partial >= want) { [fm removeItemAtPath:part error:nil]; partial = 0; }
            NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:url];
            if (partial > 0) [req setValue:[NSString stringWithFormat:@"bytes=%lld-", partial] forHTTPHeaderField:@"Range"];

            __block NSString *problem = nil;
            dispatch_semaphore_t sem = dispatch_semaphore_create(0);
            [[session downloadTaskWithRequest:req completionHandler:^(NSURL *tmp, NSURLResponse *r, NSError *e) {
                NSInteger status = [(NSHTTPURLResponse *)r statusCode];
                if (e || (status != 200 && status != 206)) {
                    problem = e ? e.localizedDescription : [NSString stringWithFormat:@"%ld 오류 (%@)", (long)status, rel];
                } else if (status == 206 && partial > 0) {          // the rest of a .part
                    if (!append_file(tmp.path, part)) problem = [NSString stringWithFormat:@"저장 실패 (%@)", rel];
                } else {                                            // the whole file: the temp file goes away after this
                    [fm removeItemAtPath:part error:nil];
                    NSError *moveErr = nil;
                    if (![fm moveItemAtURL:tmp toURL:[NSURL fileURLWithPath:part] error:&moveErr]) problem = moveErr.localizedDescription;
                }
                dispatch_semaphore_signal(sem);
            }] resume];
            dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
            if (!problem) {
                long long got = [[fm attributesOfItemAtPath:part error:nil][NSFileSize] longLongValue];
                if (got != want) problem = [NSString stringWithFormat:@"크기가 다릅니다 (%@)", rel];
                else {
                    [fm removeItemAtPath:target error:nil];
                    if (![fm moveItemAtPath:part toPath:target error:nil]) problem = [NSString stringWithFormat:@"저장 실패 (%@)", rel];
                }
            }
            @synchronized (lock) {
                if (problem) { if (!failure) failure = problem; return; }
                done += want;
            }
            report();
        }];
    }
    [queue waitUntilAllOperationsAreFinished];
    [session finishTasksAndInvalidate];
    if (!failure) [fm removeItemAtPath:marker error:nil];
    return failure;
}

- (void)startOrAsk {
    if (!g_game_running && import_incomplete()) {
        // an interrupted "PC에서 받기": half-copied data would hang or crash the game
        _pcStatus.text = @"";
        [self showMessage:[@"PC에서 받기가 끝나지 않았습니다.\n\nPC에서 'PC에서-받기-서버'를 켜고 아래 버튼을 누르면 "
                           "받은 파일은 건너뛰고 이어서 받습니다. 다 받으면 게임이 시작됩니다.\n\nNFS4 " stringByAppendingString:app_version()]
                   import:YES];
        return;
    }
    NSString *dir = locate_game_dir();
    if (!dir) {
        [self showMessage:[@"NFS High Stakes PC판 게임 폴더(Modern Patch판 nfs4.exe)가 필요합니다.\n\n"
                           "• PC에서 받기: PC에서 'PC에서-받기-서버'를 켜고, 같은 와이파이에서 아래 버튼을 누르세요 (PC는 자동으로 찾습니다).\n"
                           "• 또는 nfs4.exe가 든 폴더 전체를 파일 앱의 game 폴더에 복사한 뒤 다시 실행하세요.\n"
                           "   단독 설치: 나의 iPhone › NFS IV › game\n"
                           "   LiveContainer: LiveContainer › Games › NFS4 › game\n\nNFS4 " stringByAppendingString:app_version()]
                   import:YES];
        return;
    }
    _setup.hidden = YES;
    NSString *root = port_data_root();
    NSString *save = [root stringByAppendingPathComponent:@"save"];
    NSString *oldSave = [root stringByAppendingPathComponent:@"NFS4-save"];   // builds before the shared layout
    NSFileManager *fm = [NSFileManager defaultManager];
    if (![fm fileExistsAtPath:save] && [fm fileExistsAtPath:oldSave]) [fm moveItemAtPath:oldSave toPath:save error:nil];
    snprintf(g_game, sizeof g_game, "%s", dir.fileSystemRepresentation);
    snprintf(g_save, sizeof g_save, "%s", save.fileSystemRepresentation);
    mkdir(g_save, 0755);
    // replacement game files (high-resolution textures etc.), laid out like the game folder
    snprintf(g_mods, sizeof g_mods, "%s", [root stringByAppendingPathComponent:@"mods"].fileSystemRepresentation);
    mkdir(g_mods, 0755);
    char logp[1200], prevp[1200];
    snprintf(logp, sizeof logp, "%s/nfs4host.log", g_save);
    snprintf(prevp, sizeof prevp, "%s/nfs4host.prev.log", g_save);
    rename(logp, prevp);                       // keep the last session's log, e.g. after a crash
    g_log = fopen(logp, "w");
    install_crash_handlers();
    plat_log("NFS4 %s, game folder: %s", app_version().UTF8String, g_game);
    [self applySettings];

    _gameView = [[GameView alloc] initWithFrame:self.view.bounds];
    _gameView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [self.view insertSubview:_gameView atIndex:0];
    [self buildControls];
    [self setupGamepads];
    start_audio();
    if (!g_game_running) plat_thread_start(game_main, NULL);
}

// ---------------------------------------------------------------- touch overlay


static NSString *item_name(NSString *ident) {
    NSDictionary *n = @{ @"steer" : @"조향", @"back" : @"후방", @"gas" : @"가속", @"brake" : @"브레이크",
                         @"spike" : @"스파이크", @"horn" : @"경적·사이렌", @"pause" : @"일시정지", @"map" : @"지도",
                         @"cam" : @"시점", @"enter" : @"확인", @"reset" : @"리셋", @"cruise" : @"오토 가속" };
    return n[ident] ?: ident;
}

- (void)addItem:(UIView<OverlayItem> *)v ident:(NSString *)ident {
    v.accessibilityIdentifier = ident;
    [_controls addSubview:v];
    _items[ident] = v;
    UIPanGestureRecognizer *pan = [[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(editPan:)];
    UIPinchGestureRecognizer *pinch = [[UIPinchGestureRecognizer alloc] initWithTarget:self action:@selector(editPinch:)];
    UITapGestureRecognizer *tap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(editTap:)];
    for (UIGestureRecognizer *g in @[ pan, pinch, tap ]) {
        g.enabled = NO;                        // only while the editor is open
        g.delegate = self;
        [v addGestureRecognizer:g];
        [_editGestures addObject:g];
    }
}

- (KeyButton *)button:(NSString *)ident symbol:(NSString *)sym or:(NSString *)fallback caption:(NSString *)caption
                 scan:(uint8_t)sc icon:(CGFloat)pt radius:(CGFloat)r {
    KeyButton *b = [[KeyButton alloc] initWithSymbol:symbol(sym, fallback, pt) caption:caption scan:sc radius:r];
    [self addItem:b ident:ident];
    return b;
}

// Default places inside the safe area: left thumb steering pad and look-behind; right thumb pedals,
// horn/siren and spike strip; top corners pause, map, camera, reset and Enter
// (handbrake, gears and lights are on the gamepad only).
- (NSDictionary<NSString *, NSValue *> *)defaultFrames {
    CGRect g = UIEdgeInsetsInsetRect(self.view.bounds, self.view.safeAreaInsets);
    const CGFloat m = 14, s = 10;
    CGFloat L = CGRectGetMinX(g) + m, R = CGRectGetMaxX(g) - m, T = CGRectGetMinY(g) + m, B = CGRectGetMaxY(g) - m;
    CGRect steer = CGRectMake(L, B - 112, 232, 112);
    CGRect gas = CGRectMake(R - 92, B - 128, 92, 128);
    CGRect brake = CGRectMake(gas.origin.x - s - 92, B - 100, 92, 100);
    CGRect pause = CGRectMake(L, T, 54, 50), enter = CGRectMake(R - 54, T, 54, 50);
    return @{
        @"steer" : [NSValue valueWithCGRect:steer],
        @"back" : [NSValue valueWithCGRect:CGRectMake(L, steer.origin.y - s - 56, 64, 56)],
        @"gas" : [NSValue valueWithCGRect:gas],
        @"brake" : [NSValue valueWithCGRect:brake],
        @"spike" : [NSValue valueWithCGRect:CGRectMake(gas.origin.x, gas.origin.y - s - 54, 92, 54)],
        @"cruise" : [NSValue valueWithCGRect:CGRectMake(gas.origin.x, gas.origin.y - 2 * (s + 54), 92, 54)],
        @"horn" : [NSValue valueWithCGRect:CGRectMake(brake.origin.x, brake.origin.y - s - 54, 92, 54)],
        @"pause" : [NSValue valueWithCGRect:pause],
        @"map" : [NSValue valueWithCGRect:CGRectOffset(pause, 54 + s, 0)],
        @"cam" : [NSValue valueWithCGRect:CGRectOffset(pause, 2 * (54 + s), 0)],
        @"enter" : [NSValue valueWithCGRect:enter],
        @"reset" : [NSValue valueWithCGRect:CGRectOffset(enter, -(54 + s), 0)],
    };
}

- (CGFloat)overlayAlpha {
    NSNumber *a = [[NSUserDefaults standardUserDefaults] objectForKey:kOverlayAlpha];
    return a ? a.doubleValue : 0.6;
}

- (void)layoutOverlay {
    if (!_items) return;
    CGSize sz = self.view.bounds.size;
    NSDictionary *def = [self defaultFrames];
    for (NSString *ident in _items) {
        UIView<OverlayItem> *v = _items[ident];
        CGRect d = [def[ident] CGRectValue];
        NSDictionary *o = _layout[ident];
        v.transform = CGAffineTransformIdentity;
        v.bounds = CGRectMake(0, 0, d.size.width, d.size.height);
        v.center = o[@"x"] ? CGPointMake([o[@"x"] doubleValue] * sz.width, [o[@"y"] doubleValue] * sz.height)
                           : CGPointMake(CGRectGetMidX(d), CGRectGetMidY(d));
        v.scale = o[@"s"] ? [o[@"s"] doubleValue] : 1;
        v.haptic = o[@"h"] ? [o[@"h"] boolValue] : YES;
        v.transform = CGAffineTransformMakeScale(v.scale, v.scale);
    }
    _controls.alpha = [self overlayAlpha];
    [self layoutMenuControls];
}

// Menu screens are 4:3 with black bars beside them (the Dynamic Island side too): their buttons
// go in the bars, ▲▼◀▶ on the left and Enter/Esc on the right, so nothing covers the menu.
- (void)layoutMenuControls {
    CGSize sz = self.view.bounds.size;
    UIEdgeInsets in = self.view.safeAreaInsets;
    CGFloat availW = sz.width - in.left - in.right, picH = sz.height, picW = picH * 4 / 3;
    if (picW > availW) { picW = availW; picH = picW * 3 / 4; }
    CGFloat picX = in.left + (availW - picW) / 2;
    CGFloat leftW = picX - in.left, rightW = sz.width - in.right - (picX + picW);
    CGFloat cxL = in.left + leftW / 2, cxR = picX + picW + rightW / 2, cy = sz.height / 2;
    CGFloat k = MIN(1, MIN(leftW, rightW) / 124);       // narrower bars (iPad): smaller buttons
    if (k < 0.5) k = 0.5;
    NSDictionary *place = @{
        @"m_up" : @[ @(cxL), @(cy - 58 * k), @52, @52 ], @"m_down" : @[ @(cxL), @(cy + 58 * k), @52, @52 ],
        @"m_left" : @[ @(cxL - 32 * k), @(cy), @52, @52 ], @"m_right" : @[ @(cxL + 32 * k), @(cy), @52, @52 ],
        @"m_enter" : @[ @(cxR), @(cy - 38 * k), @76, @64 ], @"m_esc" : @[ @(cxR), @(cy + 38 * k), @76, @64 ],
    };
    for (KeyButton *b in _menuButtons) {
        NSArray *p = place[b.accessibilityIdentifier];
        b.transform = CGAffineTransformIdentity;
        b.bounds = CGRectMake(0, 0, [p[2] doubleValue], [p[3] doubleValue]);
        b.center = CGPointMake([p[0] doubleValue], [p[1] doubleValue]);
        b.scale = k;
        b.transform = CGAffineTransformMakeScale(k, k);
    }
    // ⚙︎: top centre in a race, top of the right bar on menu screens
    _gear.bounds = CGRectMake(0, 0, 36, 36);
    _gear.center = _menuMode ? CGPointMake(cxR, in.top + 24) : CGPointMake(sz.width / 2, in.top + 24);
    _kbd.bounds = CGRectMake(0, 0, 36, 36);
    _kbd.center = CGPointMake(cxR, sz.height - in.bottom - 24);
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    [self layoutOverlay];
}

- (void)buildControls {
    _controls = [[PassthroughView alloc] initWithFrame:self.view.bounds];
    _controls.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _controls.multipleTouchEnabled = YES;
    _controls.backgroundColor = UIColor.clearColor;
    [self.view addSubview:_controls];
    _items = [NSMutableDictionary dictionary];
    _editGestures = [NSMutableArray array];
    _layout = [[[NSUserDefaults standardUserDefaults] dictionaryForKey:kOverlayLayout] mutableCopy] ?: [NSMutableDictionary dictionary];

    _steerPad = [SteerPad new];
    [self addItem:_steerPad ident:@"steer"];
    [self button:@"back" symbol:@"arrow.uturn.backward" or:nil caption:@"후방" scan:K_BACK icon:18 radius:18];
    [self button:@"gas" symbol:@"pedal.accelerator.fill" or:@"arrowtriangle.up.fill" caption:@"가속" scan:K_GAS icon:30 radius:26];
    [self button:@"brake" symbol:@"pedal.brake.fill" or:@"arrowtriangle.down.fill" caption:@"브레이크" scan:K_BRAKE icon:26 radius:26];
    [self button:@"spike" symbol:@"exclamationmark.triangle.fill" or:nil caption:@"스파이크" scan:K_SPIKE icon:17 radius:18];
    [self button:@"horn" symbol:@"megaphone.fill" or:nil caption:@"경적·사이렌" scan:K_HORN icon:17 radius:18];
    CruiseButton *cruise = [[CruiseButton alloc] initWithSymbol:symbol(@"speedometer", @"gauge", 17) caption:@"오토 가속"
                                                           scan:K_GAS radius:18];
    [self addItem:cruise ident:@"cruise"];
    g_cruise_button = cruise;
    [self button:@"pause" symbol:@"pause.fill" or:nil caption:@"일시정지" scan:K_ESC icon:16 radius:16];
    [self button:@"map" symbol:@"map.fill" or:nil caption:@"지도" scan:K_MAP icon:16 radius:16];
    [self button:@"cam" symbol:@"video.fill" or:nil caption:@"시점" scan:K_CAM icon:16 radius:16];
    [self button:@"enter" symbol:@"return" or:nil caption:@"확인" scan:K_ENTER icon:16 radius:16];
    [self button:@"reset" symbol:@"arrow.counterclockwise" or:nil caption:@"리셋" scan:K_RESET icon:16 radius:16];

    // menu screens: arrows and Enter/Esc in the black bars (not editable: they follow the picture)
    _menuButtons = [NSMutableArray array];
    struct { NSString *ident, *sym, *caption; uint8_t scan; CGFloat icon; } menu[] = {
        { @"m_up", @"chevron.up", nil, K_GAS, 20 }, { @"m_down", @"chevron.down", nil, K_BRAKE, 20 },
        { @"m_left", @"chevron.left", nil, K_LEFT, 20 }, { @"m_right", @"chevron.right", nil, K_RIGHT, 20 },
        { @"m_enter", @"return", @"확인", K_ENTER, 18 }, { @"m_esc", @"arrow.uturn.backward", @"뒤로", K_ESC, 18 },
    };
    for (size_t i = 0; i < sizeof menu / sizeof menu[0]; i++) {
        KeyButton *b = [[KeyButton alloc] initWithSymbol:symbol(menu[i].sym, nil, menu[i].icon) caption:menu[i].caption
                                                    scan:menu[i].scan radius:16];
        b.accessibilityIdentifier = menu[i].ident;
        b.hidden = YES;
        [_controls addSubview:b];
        [_menuButtons addObject:b];
    }

    // the settings button is always there so the editor can be reached; the icon is an image view
    // over the glass (a UIButton's own image ended up under it and the button showed blank)
    _gear = [self roundButton:@"gearshape.fill" action:@selector(showSettings)];
    // menu screens: keyboard (player name, codes) at the bottom of the right bar
    _kbd = [self roundButton:@"keyboard" action:@selector(askText)];
    _kbd.hidden = YES;
    [self layoutOverlay];

    // menu or race: the game's Glide window tells (hle_glide_in_menu)
    [NSTimer scheduledTimerWithTimeInterval:0.25 repeats:YES block:^(NSTimer *t) {
        BOOL m = hle_glide_in_menu() != 0;
        if (m == self.menuMode) return;
        self.menuMode = m;
        [self releaseOverlayKeys];
        [self applyOverlayVisibility];
        [self layoutMenuControls];
    }];
}

- (UIButton *)roundButton:(NSString *)sym action:(SEL)action {
    UIButton *b = [UIButton buttonWithType:UIButtonTypeCustom];
    glass_in(b, 18);
    icon_in(b, symbol(sym, nil, 15), nil);
    [b addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
    [_controls addSubview:b];
    return b;
}

- (void)releaseOverlayKeys {
    set_cruise(NO);                            // menu screen, gamepad or editor: no throttle left on
    [_steerPad releaseAll];
    for (UIView *v in [_items.allValues arrayByAddingObjectsFromArray:_menuButtons])
        if ([v isKindOfClass:KeyButton.class]) { key(((KeyButton *)v).scan, 0); ((KeyButton *)v).glow.alpha = 0; }
}

// race controls in a race, menu buttons on menu screens, neither with a gamepad (⚙︎ always);
// the editor shows the race controls
- (void)applyOverlayVisibility {
    BOOL race = g_overlay_editing || (!_padConnected && !_menuMode);
    BOOL menu = !g_overlay_editing && !_padConnected && _menuMode;
    for (UIView *v in _items.allValues) v.hidden = !race;
    for (UIView *v in _menuButtons) v.hidden = !menu;
    _gear.hidden = g_overlay_editing;
    _kbd.hidden = g_overlay_editing || !_menuMode;
}

// the touch controls are hidden while a gamepad is connected (taps on the picture and ⚙︎ stay)
- (void)updateOverlay {
    if (g_overlay_editing) return;
    BOOL pad = NO;
    for (GCController *c in GCController.controllers)
        if (c.extendedGamepad) pad = YES;
    _padConnected = pad;
    [self releaseOverlayKeys];                 // nothing stays held when a button disappears or a pad leaves
    [self applyOverlayVisibility];
    plat_log("gamepad %s, touch controls %s", pad ? "connected" : "none", pad ? "hidden" : "shown");
}

// ---------------------------------------------------------------- overlay editor

- (BOOL)gestureRecognizer:(UIGestureRecognizer *)a shouldRecognizeSimultaneouslyWithGestureRecognizer:(UIGestureRecognizer *)b {
    return YES;                                // move and pinch at the same time
}

- (void)storeItem:(UIView<OverlayItem> *)v persist:(BOOL)persist {
    CGSize sz = self.view.bounds.size;
    _layout[v.accessibilityIdentifier] = @{ @"x" : @(v.center.x / sz.width), @"y" : @(v.center.y / sz.height),
                                            @"s" : @(v.scale), @"h" : @(v.haptic) };
    if (persist) [[NSUserDefaults standardUserDefaults] setObject:_layout forKey:kOverlayLayout];
}

- (void)selectItem:(UIView<OverlayItem> *)v {
    for (UIView *o in _items.allValues) o.layer.borderWidth = 0;
    _selected = v.accessibilityIdentifier;
    v.layer.cornerRadius = v.radius;
    v.layer.cornerCurve = kCACornerCurveContinuous;
    v.layer.borderWidth = 2;
    v.layer.borderColor = UIColor.systemYellowColor.CGColor;
    _editTitle.text = [NSString stringWithFormat:@"선택: %@", item_name(_selected)];
    _sizeSlider.enabled = _hapticSwitch.enabled = YES;
    _sizeSlider.value = v.scale;
    _hapticSwitch.on = v.haptic;
}

- (void)setScale:(CGFloat)s of:(UIView<OverlayItem> *)v {
    v.scale = MIN(MAX(s, 0.5), 2.0);
    v.transform = CGAffineTransformMakeScale(v.scale, v.scale);
    if ([v.accessibilityIdentifier isEqualToString:_selected]) _sizeSlider.value = v.scale;
}

- (void)editTap:(UITapGestureRecognizer *)g { [self selectItem:(UIView<OverlayItem> *)g.view]; }

- (void)editPan:(UIPanGestureRecognizer *)g {
    UIView<OverlayItem> *v = (UIView<OverlayItem> *)g.view;
    if (g.state == UIGestureRecognizerStateBegan) [self selectItem:v];
    CGPoint t = [g translationInView:self.view];
    CGSize sz = self.view.bounds.size;
    v.center = CGPointMake(MIN(MAX(v.center.x + t.x, 0), sz.width), MIN(MAX(v.center.y + t.y, 0), sz.height));
    [g setTranslation:CGPointZero inView:self.view];
    [self storeItem:v persist:g.state == UIGestureRecognizerStateEnded];
}

- (void)editPinch:(UIPinchGestureRecognizer *)g {
    UIView<OverlayItem> *v = (UIView<OverlayItem> *)g.view;
    if (g.state == UIGestureRecognizerStateBegan) [self selectItem:v];
    [self setScale:v.scale * g.scale of:v];
    g.scale = 1;
    [self storeItem:v persist:g.state == UIGestureRecognizerStateEnded];
}

- (void)sizeChanged:(UISlider *)s {
    UIView<OverlayItem> *v = _items[_selected];
    if (!v) return;
    [self setScale:s.value of:v];
    [self storeItem:v persist:YES];
}

- (void)hapticChanged:(UISwitch *)w {
    UIView<OverlayItem> *v = _items[_selected];
    if (!v) return;
    v.haptic = w.on;
    [self storeItem:v persist:YES];
    if (w.on) [tap_haptic() impactOccurred];
}

- (void)alphaChanged:(UISlider *)s {
    [[NSUserDefaults standardUserDefaults] setDouble:s.value forKey:kOverlayAlpha];
    _controls.alpha = s.value;
}

- (void)resetLayout {
    UIAlertController *a = [UIAlertController alertControllerWithTitle:@"기본값으로 되돌리기"
        message:@"모든 버튼의 위치·크기·진동과 투명도를 처음 상태로 되돌립니다." preferredStyle:UIAlertControllerStyleAlert];
    [a addAction:[UIAlertAction actionWithTitle:@"취소" style:UIAlertActionStyleCancel handler:nil]];
    [a addAction:[UIAlertAction actionWithTitle:@"되돌리기" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *x) {
        NSUserDefaults *d = [NSUserDefaults standardUserDefaults];
        [self.layout removeAllObjects];
        [d removeObjectForKey:kOverlayLayout];
        [d removeObjectForKey:kOverlayAlpha];
        self.alphaSlider.value = [self overlayAlpha];
        [self layoutOverlay];
        UIView<OverlayItem> *v = self.items[self.selected];
        if (v) [self selectItem:v];
    }]];
    [self presentViewController:a animated:YES completion:nil];
}

- (UIView *)editRow:(NSString *)title control:(UIView *)c {
    UILabel *l = [UILabel new];
    l.text = title;
    l.font = [UIFont systemFontOfSize:13 weight:UIFontWeightSemibold];
    l.textColor = [UIColor colorWithWhite:1 alpha:0.85];
    [l.widthAnchor constraintEqualToConstant:52].active = YES;
    UIStackView *row = [[UIStackView alloc] initWithArrangedSubviews:@[ l, c ]];
    row.spacing = 10;
    row.alignment = UIStackViewAlignmentCenter;
    return row;
}

- (UIView *)buildEditPanel {
    UIView *p = [UIView new];
    glass_in(p, 20);
    _editTitle = [UILabel new];
    _editTitle.font = [UIFont systemFontOfSize:15 weight:UIFontWeightBold];
    _editTitle.textColor = UIColor.whiteColor;
    _sizeSlider = [UISlider new];
    _sizeSlider.minimumValue = 0.5;
    _sizeSlider.maximumValue = 2.0;
    [_sizeSlider addTarget:self action:@selector(sizeChanged:) forControlEvents:UIControlEventValueChanged];
    _hapticSwitch = [UISwitch new];
    [_hapticSwitch addTarget:self action:@selector(hapticChanged:) forControlEvents:UIControlEventValueChanged];
    _alphaSlider = [UISlider new];
    _alphaSlider.minimumValue = 0.1;
    _alphaSlider.maximumValue = 1.0;
    [_alphaSlider addTarget:self action:@selector(alphaChanged:) forControlEvents:UIControlEventValueChanged];
    UILabel *hint = [UILabel new];
    hint.text = @"버튼을 끌어서 옮기고, 두 손가락으로 벌리거나 좁혀 크기를 바꿉니다. 이 창도 끌어서 옮길 수 있습니다.";
    hint.numberOfLines = 0;
    hint.font = [UIFont systemFontOfSize:11];
    hint.textColor = [UIColor colorWithWhite:1 alpha:0.6];
    UIButton *reset = [UIButton buttonWithType:UIButtonTypeSystem];
    [reset setTitle:@"기본값" forState:UIControlStateNormal];
    reset.tintColor = UIColor.systemRedColor;
    [reset addTarget:self action:@selector(resetLayout) forControlEvents:UIControlEventTouchUpInside];
    UIButton *done = [UIButton buttonWithType:UIButtonTypeSystem];
    [done setTitle:@"완료" forState:UIControlStateNormal];
    done.titleLabel.font = [UIFont systemFontOfSize:17 weight:UIFontWeightBold];
    [done addTarget:self action:@selector(finishEditing) forControlEvents:UIControlEventTouchUpInside];
    UIStackView *buttons = [[UIStackView alloc] initWithArrangedSubviews:@[ reset, done ]];
    buttons.distribution = UIStackViewDistributionFillEqually;
    UIStackView *st = [[UIStackView alloc] initWithArrangedSubviews:@[
        _editTitle, [self editRow:@"크기" control:_sizeSlider], [self editRow:@"진동" control:_hapticSwitch],
        [self editRow:@"투명도" control:_alphaSlider], hint, buttons ]];
    st.axis = UILayoutConstraintAxisVertical;
    st.spacing = 10;
    st.translatesAutoresizingMaskIntoConstraints = NO;
    [p addSubview:st];
    [NSLayoutConstraint activateConstraints:@[
        [st.leadingAnchor constraintEqualToAnchor:p.leadingAnchor constant:16],
        [st.trailingAnchor constraintEqualToAnchor:p.trailingAnchor constant:-16],
        [st.topAnchor constraintEqualToAnchor:p.topAnchor constant:14],
        [st.bottomAnchor constraintEqualToAnchor:p.bottomAnchor constant:-12],
    ]];
    CGSize fit = [p systemLayoutSizeFittingSize:CGSizeMake(300, 0) withHorizontalFittingPriority:UILayoutPriorityRequired
                          verticalFittingPriority:UILayoutPriorityFittingSizeLevel];
    p.frame = CGRectMake(0, 0, 300, fit.height);
    [p addGestureRecognizer:[[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(panelPan:)]];
    return p;
}

- (void)panelPan:(UIPanGestureRecognizer *)g {
    CGPoint t = [g translationInView:self.view];
    g.view.center = CGPointMake(g.view.center.x + t.x, g.view.center.y + t.y);
    [g setTranslation:CGPointZero inView:self.view];
}

- (void)startEditing {
    if (g_overlay_editing) return;
    [self releaseOverlayKeys];
    g_overlay_editing = YES;
    [self applyOverlayVisibility];             // race controls, also when a gamepad or a menu hid them
    for (UIGestureRecognizer *g in _editGestures) g.enabled = YES;
    if (!_editPanel) _editPanel = [self buildEditPanel];
    [self.view addSubview:_editPanel];
    _editPanel.center = CGPointMake(self.view.bounds.size.width / 2, self.view.bounds.size.height / 2);
    _editTitle.text = @"편집할 버튼을 누르세요";
    _selected = nil;
    _sizeSlider.enabled = _hapticSwitch.enabled = NO;
    _alphaSlider.value = [self overlayAlpha];
    [self layoutOverlay];
}

- (void)finishEditing {
    for (UIGestureRecognizer *g in _editGestures) g.enabled = NO;
    for (UIView *v in _items.allValues) v.layer.borderWidth = 0;
    _selected = nil;
    [_editPanel removeFromSuperview];
    _gear.hidden = NO;
    [[NSUserDefaults standardUserDefaults] setObject:_layout forKey:kOverlayLayout];
    g_overlay_editing = NO;
    [self updateOverlay];
}

// A ⏎, B/Menu Esc, X handbrake, Y camera, R2/L2 gas/brake, R1/L1 gear up/down, L3 horn/siren,
// R3 spike strip, right stick: down look behind, up lights, left map, right reset; Options map
- (void)setupGamepads {
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    for (NSNotificationName n in @[ GCControllerDidConnectNotification, GCControllerDidDisconnectNotification ])
        [nc addObserverForName:n object:nil queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification *x) { [self updateOverlay]; }];
    [self updateOverlay];
    [NSTimer scheduledTimerWithTimeInterval:1.0 / 60 repeats:YES block:^(NSTimer *t) {
        GCExtendedGamepad *p = GCController.current.extendedGamepad;
        if (!p) {
            if (g_pad_steer || g_pad_throttle) { g_pad_steer = g_pad_throttle = 0; update_joy(); }
            return;
        }
        float x = p.leftThumbstick.xAxis.value;
        float rx = p.rightThumbstick.xAxis.value, ry = p.rightThumbstick.yAxis.value;
        float rt = p.rightTrigger.value, lt = p.leftTrigger.value;
        // menus: the stick and triggers act as arrow keys; races: analog steering and throttle
        BOOL menu = hle_glide_in_menu() != 0;
        key(K_LEFT, (menu && x < -0.5f) || p.dpad.left.isPressed);
        key(K_RIGHT, (menu && x > 0.5f) || p.dpad.right.isPressed);
        key(K_GAS, (menu && rt > 0.2f) || p.dpad.up.isPressed);
        key(K_BRAKE, (menu && lt > 0.2f) || p.dpad.down.isPressed);
        g_pad_steer = menu || fabsf(x) < 0.08f ? 0 : x;
        g_pad_throttle = menu ? 0 : lt - rt;
        if (lt > 0.2f || p.dpad.down.isPressed) set_cruise(NO);
        update_joy();
        key(K_ENTER, p.buttonA.isPressed);
        key(K_ESC, p.buttonB.isPressed || p.buttonMenu.isPressed);
        key(K_HAND, p.buttonX.isPressed);
        key(K_CAM, p.buttonY.isPressed);
        key(K_UP, p.rightShoulder.isPressed);
        key(K_DOWN, p.leftShoulder.isPressed);
        key(K_HORN, p.leftThumbstickButton.isPressed);
        key(K_SPIKE, p.rightThumbstickButton.isPressed);
        key(K_BACK, ry < -0.6f);
        key(K_LIGHTS, ry > 0.6f);
        key(K_MAP, rx < -0.6f || p.buttonOptions.isPressed);
        key(K_RESET, rx > 0.8f);
    }];
}

// ---------------------------------------------------------------- import

- (void)pickFolder {
    UIDocumentPickerViewController *p = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ UTTypeFolder ]];
    p.delegate = self;
    [self presentViewController:p animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController *)c didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    NSURL *src = urls.firstObject;
    if (!src) return;
    [self showMessage:@"복사 중… (수백 MB라 몇 분 걸릴 수 있습니다)" import:NO];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        BOOL scoped = [src startAccessingSecurityScopedResource];
        NSString *dst = [[port_data_root() stringByAppendingPathComponent:@"game"] stringByAppendingPathComponent:@"NFS4"];
        NSFileManager *fm = [NSFileManager defaultManager];
        [fm removeItemAtPath:dst error:nil];
        NSError *err = nil;
        BOOL ok = [fm copyItemAtURL:src toURL:[NSURL fileURLWithPath:dst] error:&err];
        if (scoped) [src stopAccessingSecurityScopedResource];
        // the scan-bypass video in some releases is huge and unused
        for (NSString *rel in [fm enumeratorAtPath:dst])
            if ([rel.pathExtension caseInsensitiveCompare:@"mp4"] == NSOrderedSame)
                [fm removeItemAtPath:[dst stringByAppendingPathComponent:rel] error:nil];
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!ok) [self showMessage:[NSString stringWithFormat:@"복사 실패: %@", err.localizedDescription] import:YES];
            else [self startOrAsk];
        });
    });
}
@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic) UIWindow *window;
@end
@implementation AppDelegate
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)opts {
    _window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    _window.rootViewController = [GameViewController new];
    [_window makeKeyAndVisible];
    [UIApplication sharedApplication].idleTimerDisabled = YES;
    return YES;
}
@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class]));
    }
}
