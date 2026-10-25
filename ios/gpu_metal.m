// Metal implementation of gpu.h (iOS). The game thread encodes each Glide frame into an
// off-screen render target (upscaled to the device), and GameView shows the latest finished one.
#import <Metal/Metal.h>
#import <UIKit/UIKit.h>
#include <pthread.h>
#include "gpu.h"
#include "gpu_shaders.h"
#include "plat.h"

extern int g_screen_height_px;     // set by plat_ios.m on the main thread
extern int g_msaa_samples;         // 1 or 4, from the in-app setting

static id<MTLDevice> dev;
static id<MTLCommandQueue> queue;
static id<MTLLibrary> lib;
static MTLVertexDescriptor *vdesc;
static NSMutableDictionary<NSNumber *, id<MTLRenderPipelineState>> *pipes;
static id<MTLDepthStencilState> depths[9][2];
static id<MTLSamplerState> samplers[2][2][2];
static id<MTLTexture> white;

#define NFRAMES 3
static id<MTLTexture> targets[NFRAMES];
static id<MTLTexture> depthTex;
static id<MTLTexture> msaaColor, msaaDepth;     // memoryless (tile memory only) when MSAA is on
static int samples = 1;
static id<MTLBuffer> rings[NFRAMES];
#define RING_BYTES (8u << 20)
static size_t ringPos;
static int cur;
static dispatch_semaphore_t inflight;
static id<MTLCommandBuffer> cmd;
static id<MTLRenderCommandEncoder> enc;
static int W, H, TW, TH;
static float S;
static int setupDone, setupOk, firstFrame;

static pthread_mutex_t displayLock = PTHREAD_MUTEX_INITIALIZER;
static id<MTLTexture> displayTex;

#define MAXTEX 4096
static id<MTLTexture> texs[MAXTEX];

id<MTLTexture> gpu_metal_display_texture(void) {
    pthread_mutex_lock(&displayLock);
    id<MTLTexture> t = displayTex;
    pthread_mutex_unlock(&displayLock);
    return t;
}
id<MTLDevice> gpu_metal_device(void) { return dev; }

static int setup(void) {
    dev = MTLCreateSystemDefaultDevice();
    if (!dev) return 0;
    queue = [dev newCommandQueue];
    NSError *err = nil;
    lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:GPU_MSL] options:nil error:&err];
    if (!lib) { plat_log("metal: shader compile failed: %s", err.localizedDescription.UTF8String); return 0; }
    vdesc = [MTLVertexDescriptor vertexDescriptor];
    vdesc.attributes[0].format = MTLVertexFormatFloat4; vdesc.attributes[0].offset = 0;
    vdesc.attributes[1].format = MTLVertexFormatFloat2; vdesc.attributes[1].offset = 16;
    vdesc.attributes[2].format = MTLVertexFormatFloat; vdesc.attributes[2].offset = 24;
    vdesc.attributes[3].format = MTLVertexFormatUChar4Normalized; vdesc.attributes[3].offset = 28;
    for (int i = 0; i < 4; i++) vdesc.attributes[i].bufferIndex = 0;
    vdesc.layouts[0].stride = sizeof(GpuVertex);
    pipes = [NSMutableDictionary dictionary];
    for (int i = 0; i < NFRAMES; i++) rings[i] = [dev newBufferWithLength:RING_BYTES options:MTLResourceStorageModeShared];
    inflight = dispatch_semaphore_create(NFRAMES);
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
    white = [dev newTextureWithDescriptor:td];
    uint32_t px = 0xFFFFFFFFu;
    [white replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&px bytesPerRow:4];
    return 1;
}

int gpu_init(int w, int h) {
    if (!setupDone) { setupDone = 1; setupOk = setup(); }
    if (!setupOk) return 0;
    if (enc) { [enc endEncoding]; enc = nil; }
    if (cmd) {                                   // finish the frame in progress (returns its slot)
        dispatch_semaphore_t sem = inflight;
        [cmd addCompletedHandler:^(id<MTLCommandBuffer> b) { dispatch_semaphore_signal(sem); }];
        [cmd commit];
        [cmd waitUntilCompleted];
        cmd = nil;
    }
    pthread_mutex_lock(&displayLock);
    displayTex = nil;
    pthread_mutex_unlock(&displayLock);
    W = w; H = h;
    int screenH = g_screen_height_px > 0 ? g_screen_height_px : 1080;
    S = (float)screenH / (float)h;
    if (S < 1) S = 1;
    if (S > 3) S = 3;
    TW = (int)(w * S + 0.5f); TH = (int)(h * S + 0.5f);
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:TW height:TH mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;
    for (int i = 0; i < NFRAMES; i++) targets[i] = [dev newTextureWithDescriptor:td];
    MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:TW height:TH mipmapped:NO];
    dd.usage = MTLTextureUsageRenderTarget;
    dd.storageMode = MTLStorageModePrivate;
    depthTex = [dev newTextureWithDescriptor:dd];
    samples = (g_msaa_samples == 4 && [dev supportsTextureSampleCount:4]) ? 4 : 1;
    msaaColor = msaaDepth = nil;
    if (samples > 1) {
        MTLTextureDescriptor *md = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:TW height:TH mipmapped:NO];
        md.textureType = MTLTextureType2DMultisample; md.sampleCount = samples;
        md.usage = MTLTextureUsageRenderTarget; md.storageMode = MTLStorageModeMemoryless;
        msaaColor = [dev newTextureWithDescriptor:md];
        md.pixelFormat = MTLPixelFormatDepth32Float;
        msaaDepth = [dev newTextureWithDescriptor:md];
        if (!msaaColor || !msaaDepth) samples = 1;
    }
    [pipes removeAllObjects];                  // pipelines depend on the sample count
    firstFrame = 1;
    plat_log("metal: Glide %dx%d rendered at %dx%d, %dx MSAA", w, h, TW, TH, samples);
    return 1;
}

int gpu_texture_create(int w, int h) {
    for (int i = 0; i < MAXTEX; i++) {
        if (texs[i]) continue;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:YES];
        td.usage = MTLTextureUsageShaderRead;
        texs[i] = [dev newTextureWithDescriptor:td];
        return texs[i] ? i : -1;
    }
    return -1;
}

void gpu_texture_upload(int tid, const uint32_t *rgba) {
    if (tid < 0 || tid >= MAXTEX || !texs[tid]) return;
    id<MTLTexture> t = texs[tid];
    [t replaceRegion:MTLRegionMake2D(0, 0, t.width, t.height) mipmapLevel:0 withBytes:rgba bytesPerRow:t.width * 4];
    if (t.mipmapLevelCount > 1) {
        // a separate command buffer committed now runs before the frame's (committed at swap)
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b generateMipmapsForTexture:t];
        [b endEncoding];
        [cb commit];
    }
}

void gpu_texture_destroy(int tid) {
    if (tid >= 0 && tid < MAXTEX) texs[tid] = nil;
}

static MTLBlendFactor map_blend(int f, int src, int alpha) {
    switch (f) {
    case 0: return MTLBlendFactorZero;
    case 1: return MTLBlendFactorSourceAlpha;
    case 2: return alpha ? (src ? MTLBlendFactorDestinationAlpha : MTLBlendFactorSourceAlpha)
                         : (src ? MTLBlendFactorDestinationColor : MTLBlendFactorSourceColor);
    case 3: return MTLBlendFactorDestinationAlpha;
    case 4: return MTLBlendFactorOne;
    case 5: return MTLBlendFactorOneMinusSourceAlpha;
    case 6: return alpha ? (src ? MTLBlendFactorOneMinusDestinationAlpha : MTLBlendFactorOneMinusSourceAlpha)
                         : (src ? MTLBlendFactorOneMinusDestinationColor : MTLBlendFactorOneMinusSourceColor);
    case 7: return MTLBlendFactorOneMinusDestinationAlpha;
    case 15: return src ? MTLBlendFactorSourceAlphaSaturated : MTLBlendFactorOne;
    }
    return MTLBlendFactorOne;
}

static id<MTLRenderPipelineState> pipeline(const int32_t b[4]) {
    NSNumber *key = @((b[0] & 15) | ((b[1] & 15) << 4) | ((b[2] & 15) << 8) | ((b[3] & 15) << 12));
    id<MTLRenderPipelineState> p = pipes[key];
    if (p) return p;
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"vs_main"];
    pd.fragmentFunction = [lib newFunctionWithName:@"ps_main"];
    pd.vertexDescriptor = vdesc;
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    pd.rasterSampleCount = samples;
    MTLRenderPipelineColorAttachmentDescriptor *ca = pd.colorAttachments[0];
    ca.blendingEnabled = !(b[0] == 4 && b[1] == 0 && b[2] == 4 && b[3] == 0);
    ca.sourceRGBBlendFactor = map_blend(b[0], 1, 0); ca.destinationRGBBlendFactor = map_blend(b[1], 0, 0);
    ca.sourceAlphaBlendFactor = map_blend(b[2], 1, 1); ca.destinationAlphaBlendFactor = map_blend(b[3], 0, 1);
    ca.rgbBlendOperation = MTLBlendOperationAdd; ca.alphaBlendOperation = MTLBlendOperationAdd;
    NSError *err = nil;
    p = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!p) plat_log("metal: pipeline failed: %s", err.localizedDescription.UTF8String);
    else pipes[key] = p;
    return p;
}

static id<MTLDepthStencilState> depth_state(int func, int write) {
    int f = func < 0 ? 8 : func & 7;
    id<MTLDepthStencilState> __strong *slot = &depths[f][write ? 1 : 0];
    if (!*slot) {
        MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
        d.depthCompareFunction = func < 0 ? MTLCompareFunctionAlways : (MTLCompareFunction)(func & 7);  // GR_CMP_* order
        d.depthWriteEnabled = func >= 0 && write;
        *slot = [dev newDepthStencilStateWithDescriptor:d];
    }
    return *slot;
}

static id<MTLSamplerState> sampler(int bilinear, int cs, int ct) {
    id<MTLSamplerState> __strong *slot = &samplers[bilinear ? 1 : 0][cs ? 1 : 0][ct ? 1 : 0];
    if (!*slot) {
        MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
        d.minFilter = d.magFilter = bilinear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        d.mipFilter = bilinear ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
        d.sAddressMode = cs ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
        d.tAddressMode = ct ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
        d.maxAnisotropy = bilinear ? 8 : 1;       // road textures at grazing angles
        *slot = [dev newSamplerStateWithDescriptor:d];
    }
    return *slot;
}

static void begin_frame(void) {
    if (enc) return;
    if (!cmd) {
        dispatch_semaphore_wait(inflight, DISPATCH_TIME_FOREVER);
        cmd = [queue commandBuffer];
        ringPos = 0;
    }
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    if (samples > 1) {                         // tile-memory MSAA: clear, render, resolve
        rp.colorAttachments[0].texture = msaaColor;
        rp.colorAttachments[0].resolveTexture = targets[cur];
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        rp.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
        rp.depthAttachment.texture = msaaDepth;
        rp.depthAttachment.loadAction = MTLLoadActionClear;
        rp.depthAttachment.clearDepth = 1.0;
        rp.depthAttachment.storeAction = MTLStoreActionDontCare;
    } else {
    rp.colorAttachments[0].texture = targets[cur];
    rp.colorAttachments[0].loadAction = firstFrame ? MTLLoadActionClear : MTLLoadActionLoad;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.depthAttachment.texture = depthTex;
    rp.depthAttachment.loadAction = firstFrame ? MTLLoadActionClear : MTLLoadActionLoad;
    rp.depthAttachment.clearDepth = 1.0;
    rp.depthAttachment.storeAction = MTLStoreActionStore;
    }
    firstFrame = 0;
    enc = [cmd renderCommandEncoderWithDescriptor:rp];
    [enc setDepthClipMode:MTLDepthClipModeClamp];
    [enc setCullMode:MTLCullModeNone];
    MTLViewport vp = { 0, 0, (double)TW, (double)TH, 0, 1 };
    [enc setViewport:vp];
}

void gpu_draw(const GpuState *s, const GpuVertex *v, int n) {
    if (n <= 0) return;
    begin_frame();
    size_t bytes = sizeof(GpuVertex) * (size_t)n;
    if (ringPos + bytes > RING_BYTES) return;            // frame too big: drop (should not happen)
    memcpy((uint8_t *)rings[cur].contents + ringPos, v, bytes);
    id<MTLRenderPipelineState> p = pipeline(s->blend);
    if (!p) return;
    [enc setRenderPipelineState:p];
    [enc setDepthStencilState:depth_state(s->depth_func, s->depth_write)];
    int x0 = (int)(s->scissor[0] * S), y0 = (int)(s->scissor[1] * S);
    int x1 = (int)(s->scissor[2] * S + 0.5f), y1 = (int)(s->scissor[3] * S + 0.5f);
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > TW) x1 = TW; if (y1 > TH) y1 = TH;
    if (x1 <= x0 || y1 <= y0) { ringPos += (bytes + 255) & ~(size_t)255; return; }
    MTLScissorRect sc = { (NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0) };
    [enc setScissorRect:sc];
    [enc setVertexBuffer:rings[cur] offset:ringPos atIndex:0];
    float vsize[4] = { (float)W, (float)H, 0, 0 };
    [enc setVertexBytes:vsize length:sizeof vsize atIndex:1];
    [enc setFragmentBytes:&s->shade length:sizeof s->shade atIndex:0];
    id<MTLTexture> t = (s->tex >= 0 && s->tex < MAXTEX && texs[s->tex]) ? texs[s->tex] : white;
    [enc setFragmentTexture:t atIndex:0];
    [enc setFragmentSamplerState:sampler(s->bilinear, s->clamp_s, s->clamp_t) atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:(NSUInteger)n];
    ringPos += (bytes + 255) & ~(size_t)255;
}

void gpu_swap(void) {
    if (!cmd) begin_frame();
    if (enc) { [enc endEncoding]; enc = nil; }
    id<MTLTexture> done = targets[cur];
    dispatch_semaphore_t sem = inflight;
    [cmd addCompletedHandler:^(id<MTLCommandBuffer> b) {
        pthread_mutex_lock(&displayLock);
        displayTex = done;
        pthread_mutex_unlock(&displayLock);
        dispatch_semaphore_signal(sem);
    }];
    [cmd commit];
    cmd = nil;
    cur = (cur + 1) % NFRAMES;
}
