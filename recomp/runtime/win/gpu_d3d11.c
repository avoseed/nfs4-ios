/* Direct3D 11 implementation of gpu.h for the Windows dev host. Renders off-screen at the
 * Glide resolution and reads every frame back into plat_present(), so screenshots and the
 * GDI window keep working. Its job is to validate the GPU path that iOS runs on Metal. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../gpu.h"
#include "../gpu_shaders.h"
#include "../plat.h"

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static ID3D11Texture2D *rt, *staging, *depth;
static ID3D11RenderTargetView *rtv;
static ID3D11DepthStencilView *dsv;
static ID3D11VertexShader *vs;
static ID3D11PixelShader *ps;
static ID3D11InputLayout *layout;
static ID3D11Buffer *vbuf, *vscb, *pscb;
static ID3D11RasterizerState *rast;
static ID3D11BlendState *blends[16][16][16][16];
static ID3D11DepthStencilState *depths[9][2];
static ID3D11SamplerState *samplers[2][2][2];
static int W, H, ok, setup_done, setup_ok;
static uint32_t *frame;

#define VBUF_VERTS (3 * 8192)
#define MAXTEX 4096
static struct { ID3D11Texture2D *t; ID3D11ShaderResourceView *srv; int w, h; } texs[MAXTEX];

typedef HRESULT(WINAPI *PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR,
                                         UINT, UINT, ID3DBlob **, ID3DBlob **);

static ID3DBlob *compile(PFN_D3DCompile fn, const char *entry, const char *target) {
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = fn(GPU_HLSL, strlen(GPU_HLSL), "glide.hlsl", NULL, NULL, entry, target, 0, 0, &code, &err);
    if (FAILED(hr)) {
        plat_log("d3d11: shader %s failed: %s", entry, err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        return NULL;
    }
    return code;
}

static int setup_device(void) {
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1, D3D11_SDK_VERSION, &dev, NULL, &ctx)) &&
        FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, &fl, 1, D3D11_SDK_VERSION, &dev, NULL, &ctx)))
        return 0;
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    PFN_D3DCompile fn = dc ? (PFN_D3DCompile)(void *)GetProcAddress(dc, "D3DCompile") : NULL;
    if (!fn) { plat_log("d3d11: d3dcompiler_47.dll not available"); return 0; }
    ID3DBlob *vsb = compile(fn, "vs_main", "vs_5_0"), *psb = compile(fn, "ps_main", "ps_5_0");
    if (!vsb || !psb) return 0;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &ps);
    D3D11_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ID3D11Device_CreateInputLayout(dev, il, 4, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &layout);
    D3D11_BUFFER_DESC bd = { sizeof(GpuVertex) * VBUF_VERTS, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
    ID3D11Device_CreateBuffer(dev, &bd, NULL, &vbuf);
    D3D11_BUFFER_DESC cb = { 16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
    ID3D11Device_CreateBuffer(dev, &cb, NULL, &vscb);
    cb.ByteWidth = (sizeof(GpuShade) + 15) & ~15u;
    ID3D11Device_CreateBuffer(dev, &cb, NULL, &pscb);
    D3D11_RASTERIZER_DESC rd = { D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0, 0, FALSE, TRUE, FALSE, FALSE };
    ID3D11Device_CreateRasterizerState(dev, &rd, &rast);
    return 1;
}

int gpu_init(int w, int h) {
    if (!setup_done) { setup_done = 1; setup_ok = setup_device(); }
    if (!setup_ok) return 0;
    if (rt) { ID3D11Texture2D_Release(rt); ID3D11Texture2D_Release(staging); ID3D11Texture2D_Release(depth);
              ID3D11RenderTargetView_Release(rtv); ID3D11DepthStencilView_Release(dsv); }
    W = w; H = h;
    D3D11_TEXTURE2D_DESC td = { (UINT)w, (UINT)h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_RENDER_TARGET, 0, 0 };
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &rt);
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)rt, NULL, &rtv);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &staging);
    td.Format = DXGI_FORMAT_D32_FLOAT; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL; td.CPUAccessFlags = 0;
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &depth);
    ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)depth, NULL, &dsv);
    free(frame);
    frame = malloc((size_t)w * h * 4);
    float clear[4] = { 0, 0, 0, 1 };
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, clear);
    ID3D11DeviceContext_ClearDepthStencilView(ctx, dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    ok = 1;
    plat_log("d3d11: GPU path at %dx%d", w, h);
    return 1;
}

int gpu_texture_create(int w, int h) {
    for (int i = 0; i < MAXTEX; i++) {
        if (texs[i].t) continue;
        D3D11_TEXTURE2D_DESC td = { (UINT)w, (UINT)h, 0, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT,
                                    D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, D3D11_RESOURCE_MISC_GENERATE_MIPS };
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &texs[i].t))) return -1;
        ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)texs[i].t, NULL, &texs[i].srv);
        texs[i].w = w; texs[i].h = h;
        return i;
    }
    return -1;
}

void gpu_texture_upload(int id, const uint32_t *rgba) {
    if (id < 0 || id >= MAXTEX || !texs[id].t) return;
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)texs[id].t, 0, NULL, rgba, (UINT)texs[id].w * 4, 0);
    ID3D11DeviceContext_GenerateMips(ctx, texs[id].srv);
}

void gpu_texture_destroy(int id) {
    if (id < 0 || id >= MAXTEX || !texs[id].t) return;
    ID3D11ShaderResourceView_Release(texs[id].srv);
    ID3D11Texture2D_Release(texs[id].t);
    texs[id].t = NULL;
}

static D3D11_BLEND map_blend(int f, int src, int alpha) {
    switch (f) {
    case 0: return D3D11_BLEND_ZERO;
    case 1: return D3D11_BLEND_SRC_ALPHA;
    case 2: return alpha ? (src ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_SRC_ALPHA) : (src ? D3D11_BLEND_DEST_COLOR : D3D11_BLEND_SRC_COLOR);
    case 3: return D3D11_BLEND_DEST_ALPHA;
    case 4: return D3D11_BLEND_ONE;
    case 5: return D3D11_BLEND_INV_SRC_ALPHA;
    case 6: return alpha ? (src ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_SRC_ALPHA) : (src ? D3D11_BLEND_INV_DEST_COLOR : D3D11_BLEND_INV_SRC_COLOR);
    case 7: return D3D11_BLEND_INV_DEST_ALPHA;
    case 15: return src ? D3D11_BLEND_SRC_ALPHA_SAT : D3D11_BLEND_ONE;
    }
    return D3D11_BLEND_ONE;
}

static ID3D11BlendState *blend_state(const int32_t b[4]) {
    int k[4];
    for (int i = 0; i < 4; i++) k[i] = b[i] & 15;
    ID3D11BlendState **slot = &blends[k[0]][k[1]][k[2]][k[3]];
    if (!*slot) {
        D3D11_BLEND_DESC d;
        memset(&d, 0, sizeof d);
        D3D11_RENDER_TARGET_BLEND_DESC *r = &d.RenderTarget[0];
        r->BlendEnable = !(b[0] == 4 && b[1] == 0 && b[2] == 4 && b[3] == 0);
        r->SrcBlend = map_blend(b[0], 1, 0); r->DestBlend = map_blend(b[1], 0, 0); r->BlendOp = D3D11_BLEND_OP_ADD;
        r->SrcBlendAlpha = map_blend(b[2], 1, 1); r->DestBlendAlpha = map_blend(b[3], 0, 1); r->BlendOpAlpha = D3D11_BLEND_OP_ADD;
        r->RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ID3D11Device_CreateBlendState(dev, &d, slot);
    }
    return *slot;
}

static ID3D11DepthStencilState *depth_state(int func, int write) {
    int f = func < 0 ? 8 : func & 7;
    ID3D11DepthStencilState **slot = &depths[f][write ? 1 : 0];
    if (!*slot) {
        D3D11_DEPTH_STENCIL_DESC d;
        memset(&d, 0, sizeof d);
        d.DepthEnable = func >= 0;
        d.DepthWriteMask = write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        d.DepthFunc = (D3D11_COMPARISON_FUNC)((func < 0 ? 7 : func) + 1);    /* Glide GR_CMP_* order matches */
        ID3D11Device_CreateDepthStencilState(dev, &d, slot);
    }
    return *slot;
}

static ID3D11SamplerState *sampler(int bilinear, int cs, int ct) {
    ID3D11SamplerState **slot = &samplers[bilinear ? 1 : 0][cs ? 1 : 0][ct ? 1 : 0];
    if (!*slot) {
        D3D11_SAMPLER_DESC d;
        memset(&d, 0, sizeof d);
        d.Filter = bilinear ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
        d.AddressU = cs ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
        d.AddressV = ct ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
        d.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        d.MaxLOD = D3D11_FLOAT32_MAX;
        ID3D11Device_CreateSamplerState(dev, &d, slot);
    }
    return *slot;
}

void gpu_draw(const GpuState *s, const GpuVertex *v, int n) {
    if (!ok || n <= 0) return;
    D3D11_MAPPED_SUBRESOURCE m;
    float vsz[4] = { (float)W, (float)H, 0, 0 };
    ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)vscb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    memcpy(m.pData, vsz, sizeof vsz);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)vscb, 0);
    ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)pscb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    memcpy(m.pData, &s->shade, sizeof s->shade);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)pscb, 0);

    UINT stride = sizeof(GpuVertex), offset = 0;
    ID3D11DeviceContext_IASetInputLayout(ctx, layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vbuf, &stride, &offset);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &vscb);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 1, 1, &pscb);
    ID3D11SamplerState *smp = sampler(s->bilinear, s->clamp_s, s->clamp_t);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &smp);
    ID3D11ShaderResourceView *srv = (s->tex >= 0 && s->tex < MAXTEX) ? texs[s->tex].srv : NULL;
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &srv);
    ID3D11DeviceContext_RSSetState(ctx, rast);
    D3D11_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    D3D11_RECT sc = { s->scissor[0], s->scissor[1], s->scissor[2], s->scissor[3] };
    ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &sc);
    float bf[4] = { 0, 0, 0, 0 };
    ID3D11DeviceContext_OMSetBlendState(ctx, blend_state(s->blend), bf, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, depth_state(s->depth_func, s->depth_write), 0);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, dsv);

    for (int done = 0; done < n; ) {
        int k = n - done > VBUF_VERTS ? VBUF_VERTS : n - done;
        ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)vbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        memcpy(m.pData, v + done, sizeof(GpuVertex) * (size_t)k);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)vbuf, 0);
        ID3D11DeviceContext_Draw(ctx, (UINT)k, 0);
        done += k;
    }
}

void gpu_swap(void) {
    if (!ok) return;
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)rt);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m))) {
        for (int y = 0; y < H; y++) memcpy(frame + (size_t)y * W, (uint8_t *)m.pData + (size_t)y * m.RowPitch, (size_t)W * 4);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);
        plat_present(frame, W, H);
    }
}
