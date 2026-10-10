/* Windows.Graphics.Capture of one window (Rift Apart), for the overlay: GPU capture of a window
 * even when it is behind other windows, at the game's own frame rate. Frames are copied to a
 * CPU buffer (BGRA, top-down) through a staging texture. Plain C: the WinRT ABI headers' C++
 * templates don't compile with this toolchain. */
#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <inspectable.h>
#include <roapi.h>
#include <winstring.h>
#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "wgc.h"

typedef __x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureItem Item;
typedef __x_ABI_CWindows_CGraphics_CCapture_CIDirect3D11CaptureFramePool Pool;
typedef __x_ABI_CWindows_CGraphics_CCapture_CIDirect3D11CaptureFramePoolStatics2 PoolStatics2;
typedef __x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureSession Session;
typedef __x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureSession2 Session2;
typedef __x_ABI_CWindows_CGraphics_CCapture_CIDirect3D11CaptureFrame Frame;
typedef __x_ABI_CWindows_CGraphics_CDirectX_CDirect3D11_CIDirect3DDevice RtDevice;
typedef __x_ABI_CWindows_CGraphics_CDirectX_CDirect3D11_CIDirect3DSurface RtSurface;
typedef __x_ABI_CWindows_CGraphics_CSizeInt32 Size32;
typedef __x_ABI_CWindows_CFoundation_CIClosable Closable;

HRESULT WINAPI CreateDirect3D11DeviceFromDXGIDevice(IDXGIDevice* dxgiDevice, IInspectable** graphicsDevice);

static const IID kIID_DxgiAccess = {0xA9B3D012, 0x3DF2, 0x4EE3, {0xB8, 0xD1, 0x86, 0x95, 0xF4, 0x57, 0xD3, 0xC1}};
static const IID kIID_ItemInterop = {0x3628E81B, 0x3CAC, 0x4C60, {0xB7, 0xF4, 0x23, 0xCE, 0x0E, 0x0C, 0x33, 0x56}};
static const IID kIID_Item = {0x79C3F95B, 0x31F7, 0x4EC2, {0xA4, 0x64, 0x63, 0x2E, 0xF5, 0xD3, 0x07, 0x60}};
static const IID kIID_PoolStatics2 = {0x589B103F, 0x6BBC, 0x5DF5, {0xA9, 0x91, 0x02, 0xE2, 0x8B, 0x3B, 0x66, 0xD5}};
static const IID kIID_D3DDevice = {0xA37624AB, 0x8D5F, 0x4650, {0x9D, 0x3E, 0x9E, 0xAE, 0x3D, 0x9B, 0xC6, 0x70}};
static const IID kIID_Session2 = {0x2C39AE40, 0x7D2E, 0x5044, {0x80, 0x4E, 0x8B, 0x67, 0x99, 0xD4, 0xCF, 0x9E}};
static const IID kIID_Session3 = {0xF2CDD966, 0x22AE, 0x5EA1, {0x95, 0x96, 0x3A, 0x28, 0x93, 0x44, 0xC3, 0xBE}};
static const IID kIID_Closable = {0x30D5A829, 0x7FA4, 0x4026, {0x83, 0xBB, 0xD7, 0x5B, 0xAE, 0x4E, 0xA9, 0x9E}};
static const IID kIID_DXGIDevice = {0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};
static const IID kIID_Tex2D = {0x6f15aaf2, 0xd208, 0x4e89, {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};

/* IDirect3DDxgiInterfaceAccess: IUnknown + GetInterface */
typedef struct DxgiAccess DxgiAccess;
typedef struct {
    HRESULT(STDMETHODCALLTYPE* QueryInterface)(DxgiAccess*, REFIID, void**);
    ULONG(STDMETHODCALLTYPE* AddRef)(DxgiAccess*);
    ULONG(STDMETHODCALLTYPE* Release)(DxgiAccess*);
    HRESULT(STDMETHODCALLTYPE* GetInterface)(DxgiAccess*, REFIID, void**);
} DxgiAccessVtbl;
struct DxgiAccess { const DxgiAccessVtbl* lpVtbl; };

#define REL(p) do { if (p) { (p)->lpVtbl->Release(p); (p) = NULL; } } while (0)

static struct {
    HWND hwnd;
    ID3D11Device* dev;
    ID3D11DeviceContext* ctx;
    RtDevice* rtDev;
    Item* item;
    Pool* pool;
    Session* session;
    ID3D11Texture2D* staging;
    int sw, sh;
    Size32 poolSize;
    uint32_t* buf;
    size_t bufCap;
    int w, h;
} g;
static int g_roInit = 0;

static void close_obj(IUnknown* o) {
    Closable* c = NULL;
    if (o && SUCCEEDED(o->lpVtbl->QueryInterface(o, &kIID_Closable, (void**)&c)) && c) {
        c->lpVtbl->Close(c);
        c->lpVtbl->Release(c);
    }
}

static void wgc_reset(void) {
    close_obj((IUnknown*)g.session);
    close_obj((IUnknown*)g.pool);
    REL(g.session);
    REL(g.pool);
    REL(g.item);
    REL(g.staging);
    REL(g.rtDev);
    REL(g.ctx);
    REL(g.dev);
    g.hwnd = NULL, g.sw = g.sh = 0, g.w = g.h = 0;
}

static int fail(char* err, size_t n, const char* what, HRESULT hr) {
    if (err) snprintf(err, n, "%s failed (0x%08lx)", what, (unsigned long)hr);
    wgc_reset();
    return 0;
}

int wgc_start(HWND hwnd, char* err, size_t errLen) {
    if (g.hwnd == hwnd && g.session) return 1;
    wgc_reset();
    HRESULT hr;
    if (!g_roInit) {
        hr = RoInitialize(RO_INIT_MULTITHREADED);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return fail(err, errLen, "RoInitialize", hr);
        g_roInit = 1;
    }
    D3D_FEATURE_LEVEL fl;
    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION,
                           &g.dev, &fl, &g.ctx);
    if (FAILED(hr)) return fail(err, errLen, "D3D11CreateDevice", hr);
    IDXGIDevice* dxgi = NULL;
    hr = g.dev->lpVtbl->QueryInterface(g.dev, &kIID_DXGIDevice, (void**)&dxgi);
    if (FAILED(hr)) return fail(err, errLen, "IDXGIDevice", hr);
    IInspectable* insp = NULL;
    hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi, &insp);
    dxgi->lpVtbl->Release(dxgi);
    if (FAILED(hr)) return fail(err, errLen, "CreateDirect3D11DeviceFromDXGIDevice", hr);
    hr = insp->lpVtbl->QueryInterface(insp, &kIID_D3DDevice, (void**)&g.rtDev);
    insp->lpVtbl->Release(insp);
    if (FAILED(hr)) return fail(err, errLen, "IDirect3DDevice", hr);

    HSTRING_HEADER hh;
    HSTRING cls;
    const wchar_t* itemName = L"Windows.Graphics.Capture.GraphicsCaptureItem";
    WindowsCreateStringReference(itemName, (UINT32)wcslen(itemName), &hh, &cls);
    IGraphicsCaptureItemInterop* interop = NULL;
    hr = RoGetActivationFactory(cls, &kIID_ItemInterop, (void**)&interop);
    if (FAILED(hr)) return fail(err, errLen, "GraphicsCaptureItem factory", hr);
    hr = interop->lpVtbl->CreateForWindow(interop, hwnd, &kIID_Item, (void**)&g.item);
    interop->lpVtbl->Release(interop);
    if (FAILED(hr)) return fail(err, errLen, "CreateForWindow", hr);
    Size32 size;
    hr = g.item->lpVtbl->get_Size(g.item, &size);
    if (FAILED(hr)) return fail(err, errLen, "item size", hr);

    const wchar_t* poolName = L"Windows.Graphics.Capture.Direct3D11CaptureFramePool";
    WindowsCreateStringReference(poolName, (UINT32)wcslen(poolName), &hh, &cls);
    PoolStatics2* statics = NULL;
    hr = RoGetActivationFactory(cls, &kIID_PoolStatics2, (void**)&statics);
    if (FAILED(hr)) return fail(err, errLen, "frame pool factory", hr);
    hr = statics->lpVtbl->CreateFreeThreaded(statics, g.rtDev, DirectXPixelFormat_B8G8R8A8UIntNormalized, 2, size, &g.pool);
    statics->lpVtbl->Release(statics);
    if (FAILED(hr)) return fail(err, errLen, "CreateFreeThreaded", hr);
    g.poolSize = size;
    hr = g.pool->lpVtbl->CreateCaptureSession(g.pool, g.item, &g.session);
    if (FAILED(hr)) return fail(err, errLen, "CreateCaptureSession", hr);
    Session2* s2 = NULL;
    if (SUCCEEDED(g.session->lpVtbl->QueryInterface(g.session, &kIID_Session2, (void**)&s2)) && s2) {
        s2->lpVtbl->put_IsCursorCaptureEnabled(s2, 0);
        s2->lpVtbl->Release(s2);
    }
    IInspectable* s3 = NULL;
    if (SUCCEEDED(g.session->lpVtbl->QueryInterface(g.session, &kIID_Session3, (void**)&s3)) && s3) {
        /* IGraphicsCaptureSession3: IInspectable (6 slots), get_IsBorderRequired, put_IsBorderRequired */
        typedef HRESULT(STDMETHODCALLTYPE * PutBool)(void*, boolean);
        PutBool put = (PutBool)(*(void***)s3)[7];
        put(s3, 0);
        s3->lpVtbl->Release(s3);
    }
    hr = g.session->lpVtbl->StartCapture(g.session);
    if (FAILED(hr)) return fail(err, errLen, "StartCapture", hr);
    g.hwnd = hwnd;
    return 1;
}

int wgc_frame(const uint32_t** px, int* w, int* h) {
    if (!g.pool) return 0;
    Frame* frame = NULL;
    for (;;) { /* newest frame only */
        Frame* f = NULL;
        if (FAILED(g.pool->lpVtbl->TryGetNextFrame(g.pool, &f)) || !f) break;
        if (frame) frame->lpVtbl->Release(frame);
        frame = f;
    }
    if (!frame) return 0;
    Size32 cs = {0, 0};
    frame->lpVtbl->get_ContentSize(frame, &cs);
    RtSurface* surf = NULL;
    frame->lpVtbl->get_Surface(frame, &surf);
    ID3D11Texture2D* tex = NULL;
    if (surf) {
        DxgiAccess* acc = NULL;
        if (SUCCEEDED(surf->lpVtbl->QueryInterface(surf, &kIID_DxgiAccess, (void**)&acc)) && acc) {
            acc->lpVtbl->GetInterface(acc, &kIID_Tex2D, (void**)&tex);
            acc->lpVtbl->Release(acc);
        }
        surf->lpVtbl->Release(surf);
    }
    int ok = 0;
    if (tex) {
        D3D11_TEXTURE2D_DESC d;
        tex->lpVtbl->GetDesc(tex, &d);
        int cw = cs.Width < (int)d.Width ? cs.Width : (int)d.Width, ch = cs.Height < (int)d.Height ? cs.Height : (int)d.Height;
        if (!g.staging || g.sw != (int)d.Width || g.sh != (int)d.Height) {
            REL(g.staging);
            D3D11_TEXTURE2D_DESC sd = d;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.BindFlags = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.MiscFlags = 0;
            sd.MipLevels = 1;
            sd.ArraySize = 1;
            if (SUCCEEDED(g.dev->lpVtbl->CreateTexture2D(g.dev, &sd, NULL, &g.staging))) g.sw = d.Width, g.sh = d.Height;
        }
        if (g.staging && cw > 0 && ch > 0) {
            g.ctx->lpVtbl->CopyResource(g.ctx, (ID3D11Resource*)g.staging, (ID3D11Resource*)tex);
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(g.ctx->lpVtbl->Map(g.ctx, (ID3D11Resource*)g.staging, 0, D3D11_MAP_READ, 0, &m))) {
                size_t need = (size_t)cw * ch;
                if (need > g.bufCap) {
                    free(g.buf);
                    g.buf = (uint32_t*)malloc(need * 4);
                    g.bufCap = g.buf ? need : 0;
                }
                if (g.buf) {
                    for (int y = 0; y < ch; y++)
                        memcpy(g.buf + (size_t)y * cw, (uint8_t*)m.pData + (size_t)y * m.RowPitch, (size_t)cw * 4);
                    g.w = cw, g.h = ch;
                    ok = 1;
                }
                g.ctx->lpVtbl->Unmap(g.ctx, (ID3D11Resource*)g.staging, 0);
            }
        }
        tex->lpVtbl->Release(tex);
    }
    frame->lpVtbl->Release(frame);
    if (cs.Width > 0 && cs.Height > 0 && (cs.Width != g.poolSize.Width || cs.Height != g.poolSize.Height)) {
        g.pool->lpVtbl->Recreate(g.pool, g.rtDev, DirectXPixelFormat_B8G8R8A8UIntNormalized, 2, cs);
        g.poolSize = cs;
    }
    if (!ok) return 0;
    *px = g.buf, *w = g.w, *h = g.h;
    return 1;
}

void wgc_stop(void) { wgc_reset(); }
