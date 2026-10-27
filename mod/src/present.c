/*
 * Presents the game's OpenGL frames through a DXGI flip-model swap chain.
 *
 * The game keeps rendering into its default framebuffer. At SwapBuffers the
 * frame is resolved into a GL renderbuffer (handles MSAA), blitted with a
 * vertical flip into a D3D11 texture shared through WGL_NV_DX_interop2, then
 * copied to the swap chain back buffer and presented. Windows can then switch
 * between direct scanout and composition without the blink of the NVIDIA
 * OpenGL present path.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_5.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/wglext.h>

#include "borderless.h"

/* Defined here instead of linking libdxguid, which adds ~30KB of GUIDs. */
const IID IID_ID3D11Texture2D = {0x6f15aaf2, 0xd208, 0x4e89, {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};
const IID IID_IDXGIDevice = {0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};
const IID IID_IDXGIFactory2 = {0x50c83a1c, 0xe072, 0x4c48, {0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0}};
const IID IID_IDXGIFactory5 = {0x7632e1f5, 0xee65, 0x4dca, {0x87, 0xfd, 0x84, 0xcd, 0x75, 0xf8, 0x83, 0x8d}};

typedef void(WINAPI *glGetIntegerv_fn)(GLenum, GLint *);
typedef GLboolean(WINAPI *glIsEnabled_fn)(GLenum);
typedef void(WINAPI *glCap_fn)(GLenum);
typedef void(WINAPI *glReadBuffer_fn)(GLenum);
typedef void(WINAPI *glGenTextures_fn)(GLsizei, GLuint *);
typedef void(WINAPI *glDeleteTextures_fn)(GLsizei, const GLuint *);
typedef GLenum(WINAPI *glGetError_fn)(void);
typedef PROC(WINAPI *wglGetProcAddress_fn)(LPCSTR);
typedef HRESULT(WINAPI *D3D11CreateDevice_fn)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
                                              const D3D_FEATURE_LEVEL *, UINT, UINT,
                                              ID3D11Device **, D3D_FEATURE_LEVEL *,
                                              ID3D11DeviceContext **);

static struct {
    glGetIntegerv_fn GetIntegerv;
    glIsEnabled_fn IsEnabled;
    glCap_fn Enable, Disable;
    glReadBuffer_fn ReadBuffer;
    glGenTextures_fn GenTextures;
    glDeleteTextures_fn DeleteTextures;
    glGetError_fn GetError;
    PFNGLGENFRAMEBUFFERSPROC GenFramebuffers;
    PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers;
    PFNGLBINDFRAMEBUFFERPROC BindFramebuffer;
    PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D;
    PFNGLFRAMEBUFFERRENDERBUFFERPROC FramebufferRenderbuffer;
    PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus;
    PFNGLBLITFRAMEBUFFERPROC BlitFramebuffer;
    PFNGLGENRENDERBUFFERSPROC GenRenderbuffers;
    PFNGLDELETERENDERBUFFERSPROC DeleteRenderbuffers;
    PFNGLBINDRENDERBUFFERPROC BindRenderbuffer;
    PFNGLRENDERBUFFERSTORAGEPROC RenderbufferStorage;
    PFNWGLDXOPENDEVICENVPROC DXOpenDevice;
    PFNWGLDXCLOSEDEVICENVPROC DXCloseDevice;
    PFNWGLDXREGISTEROBJECTNVPROC DXRegisterObject;
    PFNWGLDXUNREGISTEROBJECTNVPROC DXUnregisterObject;
    PFNWGLDXLOCKOBJECTSNVPROC DXLockObjects;
    PFNWGLDXUNLOCKOBJECTSNVPROC DXUnlockObjects;
} gl;

static enum { STATE_IDLE, STATE_READY, STATE_FAILED } state;
static HWND target;
static int width, height;
static int interval = 1;
static BOOL tearing;
static BOOL selftest_done;

static ID3D11Device *device;
static ID3D11DeviceContext *context;
static IDXGISwapChain1 *swap_chain;
static ID3D11Texture2D *back_buffer, *shared_texture;
static HANDLE interop_device, interop_object;
static GLuint shared_gl_texture, resolve_renderbuffer, resolve_fbo, shared_fbo;

void present_set_interval(int value)
{
    interval = value < 0 ? 0 : value > 4 ? 4 : value;
}

static BOOL load_gl(void)
{
    HMODULE opengl = GetModuleHandleA("opengl32.dll");
    wglGetProcAddress_fn get_proc;

    if (!opengl)
        return FALSE;
    get_proc = (wglGetProcAddress_fn)(void *)GetProcAddress(opengl, "wglGetProcAddress");
    if (!get_proc)
        return FALSE;

#define CORE(field, name) gl.field = (void *)GetProcAddress(opengl, name);
#define EXT(field, name) gl.field = (void *)get_proc(name);
    CORE(GetIntegerv, "glGetIntegerv")
    CORE(IsEnabled, "glIsEnabled")
    CORE(Enable, "glEnable")
    CORE(Disable, "glDisable")
    CORE(ReadBuffer, "glReadBuffer")
    CORE(GenTextures, "glGenTextures")
    CORE(DeleteTextures, "glDeleteTextures")
    CORE(GetError, "glGetError")
    EXT(GenFramebuffers, "glGenFramebuffers")
    EXT(DeleteFramebuffers, "glDeleteFramebuffers")
    EXT(BindFramebuffer, "glBindFramebuffer")
    EXT(FramebufferTexture2D, "glFramebufferTexture2D")
    EXT(FramebufferRenderbuffer, "glFramebufferRenderbuffer")
    EXT(CheckFramebufferStatus, "glCheckFramebufferStatus")
    EXT(BlitFramebuffer, "glBlitFramebuffer")
    EXT(GenRenderbuffers, "glGenRenderbuffers")
    EXT(DeleteRenderbuffers, "glDeleteRenderbuffers")
    EXT(BindRenderbuffer, "glBindRenderbuffer")
    EXT(RenderbufferStorage, "glRenderbufferStorage")
    EXT(DXOpenDevice, "wglDXOpenDeviceNV")
    EXT(DXCloseDevice, "wglDXCloseDeviceNV")
    EXT(DXRegisterObject, "wglDXRegisterObjectNV")
    EXT(DXUnregisterObject, "wglDXUnregisterObjectNV")
    EXT(DXLockObjects, "wglDXLockObjectsNV")
    EXT(DXUnlockObjects, "wglDXUnlockObjectsNV")
#undef CORE
#undef EXT

    for (void **p = (void **)&gl; p < (void **)(&gl + 1); p++)
        if (!*p)
            return FALSE;
    return TRUE;
}

static void release_resources(void)
{
    if (interop_object)
        gl.DXUnregisterObject(interop_device, interop_object);
    if (interop_device)
        gl.DXCloseDevice(interop_device);
    if (resolve_fbo)
        gl.DeleteFramebuffers(1, &resolve_fbo);
    if (shared_fbo)
        gl.DeleteFramebuffers(1, &shared_fbo);
    if (resolve_renderbuffer)
        gl.DeleteRenderbuffers(1, &resolve_renderbuffer);
    if (shared_gl_texture)
        gl.DeleteTextures(1, &shared_gl_texture);
    interop_object = interop_device = NULL;
    resolve_fbo = shared_fbo = resolve_renderbuffer = shared_gl_texture = 0;

    if (shared_texture)
        ID3D11Texture2D_Release(shared_texture);
    if (back_buffer)
        ID3D11Texture2D_Release(back_buffer);
    if (swap_chain)
        IDXGISwapChain1_Release(swap_chain);
    if (context)
        ID3D11DeviceContext_Release(context);
    if (device)
        ID3D11Device_Release(device);
    shared_texture = back_buffer = NULL;
    swap_chain = NULL;
    context = NULL;
    device = NULL;
}

static BOOL fail(const char *what, long code)
{
    log_line("dxgi: %s failed (0x%08lx), using OpenGL present", what, code);
    release_resources();
    state = STATE_FAILED;
    return FALSE;
}

static BOOL create_swap_chain(void)
{
    IDXGIDevice *dxgi_device = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIFactory2 *factory = NULL;
    IDXGIFactory5 *factory5 = NULL;
    DXGI_SWAP_CHAIN_DESC1 desc = {0};
    HRESULT hr;

    hr = ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgi_device);
    if (SUCCEEDED(hr))
        hr = IDXGIDevice_GetAdapter(dxgi_device, &adapter);
    if (SUCCEEDED(hr))
        hr = IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2, (void **)&factory);
    if (dxgi_device)
        IDXGIDevice_Release(dxgi_device);
    if (adapter)
        IDXGIAdapter_Release(adapter);
    if (FAILED(hr))
        return fail("IDXGIFactory2", hr);

    tearing = FALSE;
    if (SUCCEEDED(IDXGIFactory2_QueryInterface(factory, &IID_IDXGIFactory5, (void **)&factory5))) {
        BOOL allow = FALSE;
        if (SUCCEEDED(IDXGIFactory5_CheckFeatureSupport(
                factory5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
            tearing = allow;
        IDXGIFactory5_Release(factory5);
    }

    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_NONE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    hr = IDXGIFactory2_CreateSwapChainForHwnd(factory, (IUnknown *)device, target, &desc, NULL,
                                              NULL, &swap_chain);
    if (FAILED(hr)) {
        /* Windows 8 has no FLIP_DISCARD. */
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        hr = IDXGIFactory2_CreateSwapChainForHwnd(factory, (IUnknown *)device, target, &desc,
                                                  NULL, NULL, &swap_chain);
    }
    if (SUCCEEDED(hr))
        IDXGIFactory2_MakeWindowAssociation(factory, target,
                                            DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    IDXGIFactory2_Release(factory);
    if (FAILED(hr))
        return fail("CreateSwapChainForHwnd", hr);

    hr = IDXGISwapChain1_GetBuffer(swap_chain, 0, &IID_ID3D11Texture2D, (void **)&back_buffer);
    if (FAILED(hr))
        return fail("GetBuffer", hr);
    return TRUE;
}

static BOOL create_interop(void)
{
    D3D11_TEXTURE2D_DESC desc = {0};
    GLint read_fbo, draw_fbo, renderbuffer;
    GLenum status;
    HRESULT hr;

    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &shared_texture);
    if (FAILED(hr))
        return fail("CreateTexture2D", hr);

    interop_device = gl.DXOpenDevice(device);
    if (!interop_device)
        return fail("wglDXOpenDeviceNV", GetLastError());
    gl.GenTextures(1, &shared_gl_texture);
    interop_object = gl.DXRegisterObject(interop_device, shared_texture, shared_gl_texture,
                                         GL_TEXTURE_2D, WGL_ACCESS_WRITE_DISCARD_NV);
    if (!interop_object)
        return fail("wglDXRegisterObjectNV", GetLastError());

    gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
    gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
    gl.GetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);

    gl.GenRenderbuffers(1, &resolve_renderbuffer);
    gl.BindRenderbuffer(GL_RENDERBUFFER, resolve_renderbuffer);
    gl.RenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
    gl.BindRenderbuffer(GL_RENDERBUFFER, renderbuffer);

    gl.GenFramebuffers(1, &resolve_fbo);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, resolve_fbo);
    gl.FramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                               resolve_renderbuffer);
    status = gl.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);

    if (status == GL_FRAMEBUFFER_COMPLETE && gl.DXLockObjects(interop_device, 1, &interop_object)) {
        gl.GenFramebuffers(1, &shared_fbo);
        gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, shared_fbo);
        gl.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                shared_gl_texture, 0);
        status = gl.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
        gl.DXUnlockObjects(interop_device, 1, &interop_object);
    } else if (status == GL_FRAMEBUFFER_COMPLETE) {
        status = 0;
    }

    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        return fail("framebuffer setup", status);
    return TRUE;
}

static BOOL initialize(HWND window)
{
    D3D11CreateDevice_fn create_device;
    HMODULE d3d11;
    RECT client;
    HRESULT hr;

    if (!load_gl())
        return fail("WGL_NV_DX_interop2 / framebuffer functions", 0);
    d3d11 = LoadLibraryA("d3d11.dll");
    create_device = d3d11 ? (D3D11CreateDevice_fn)GetProcAddress(d3d11, "D3D11CreateDevice") : NULL;
    if (!create_device)
        return fail("loading d3d11.dll", GetLastError());

    GetClientRect(window, &client);
    target = window;
    width = client.right - client.left;
    height = client.bottom - client.top;

    hr = create_device(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION,
                       &device, NULL, &context);
    if (FAILED(hr))
        return fail("D3D11CreateDevice", hr);
    if (!create_swap_chain() || !create_interop())
        return FALSE;

    log_line("dxgi: presenting %dx%d through flip model (tearing %s)", width, height,
             tearing ? "supported" : "unsupported");
    state = STATE_READY;
    return TRUE;
}

/* Logs a few back buffer pixels after the first frame, to check orientation. */
static void run_selftest(void)
{
    D3D11_TEXTURE2D_DESC desc;
    D3D11_MAPPED_SUBRESOURCE map;
    ID3D11Texture2D *staging = NULL;

    selftest_done = TRUE;
    ID3D11Texture2D_GetDesc(shared_texture, &desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging)))
        return;
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging,
                                     (ID3D11Resource *)shared_texture);
    if (SUCCEEDED(ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ,
                                          0, &map))) {
        const BYTE *rows = map.pData;
        const DWORD *top = (const DWORD *)rows;
        const DWORD *bottom = (const DWORD *)(rows + (height - 1) * map.RowPitch);
        log_line("selftest: top-left %08lx top-right %08lx bottom-left %08lx (ABGR)", top[0],
                 top[width - 1], bottom[0]);
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
    }
    ID3D11Texture2D_Release(staging);
}

void present_stop(void)
{
    if (state != STATE_READY)
        return;
    release_resources();
    state = STATE_IDLE;
    log_line("dxgi: stopped");
}

BOOL present_frame(HWND window, BOOL selftest)
{
    GLint read_fbo, draw_fbo, read_buffer;
    GLboolean scissor, srgb;
    RECT client;
    HRESULT hr;

    if (state == STATE_FAILED)
        return FALSE;
    if (state == STATE_READY) {
        GetClientRect(window, &client);
        if (window != target || client.right - client.left != width ||
            client.bottom - client.top != height) {
            log_line("dxgi: window changed, recreating");
            release_resources();
            state = STATE_IDLE;
        }
    }
    if (state == STATE_IDLE && !initialize(window))
        return FALSE;

    if (!gl.DXLockObjects(interop_device, 1, &interop_object))
        return fail("wglDXLockObjectsNV", GetLastError());

    gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
    gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
    scissor = gl.IsEnabled(GL_SCISSOR_TEST);
    srgb = gl.IsEnabled(GL_FRAMEBUFFER_SRGB);
    gl.Disable(GL_SCISSOR_TEST);
    gl.Disable(GL_FRAMEBUFFER_SRGB);

    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    gl.GetIntegerv(GL_READ_BUFFER, &read_buffer);
    gl.ReadBuffer(GL_BACK);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, resolve_fbo);
    gl.BlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    gl.ReadBuffer(read_buffer);

    /* GL rows go bottom-up, D3D rows top-down. */
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, resolve_fbo);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, shared_fbo);
    gl.BlitFramebuffer(0, 0, width, height, 0, height, width, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    gl.DXUnlockObjects(interop_device, 1, &interop_object);

    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
    if (scissor)
        gl.Enable(GL_SCISSOR_TEST);
    if (srgb)
        gl.Enable(GL_FRAMEBUFFER_SRGB);

    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)back_buffer,
                                     (ID3D11Resource *)shared_texture);
    if (selftest && !selftest_done)
        run_selftest();

    hr = IDXGISwapChain1_Present(swap_chain, interval,
                                 interval == 0 && tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    if (FAILED(hr))
        return fail("Present", hr);
    return TRUE;
}
