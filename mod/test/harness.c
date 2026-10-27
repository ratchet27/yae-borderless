/* Mimics the game's window and present path, for testing the DLL without the
 * game: fake fullscreen switch, popup window, wglSwapIntervalEXT, SwapBuffers.
 * Draws red over green; the DLL selftest logs the presented pixels.
 * Usage: harness.exe [msaa] */
#include <windows.h>
#include <GL/gl.h>
#include <GL/wglext.h>
#include <stdio.h>
#include <string.h>

static LRESULT CALLBACK proc(HWND w, UINT m, WPARAM wp, LPARAM lp) { return DefWindowProcA(w, m, wp, lp); }

int main(int argc, char **argv)
{
    BOOL msaa = argc > 1 && !strcmp(argv[1], "msaa");
    DWORD handle;
    DEVMODEA dm = {0};
    WNDCLASSA wc = {0};
    PIXELFORMATDESCRIPTOR pfd = {sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
                                 PFD_TYPE_RGBA, 32, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 24, 8};
    int w = 640, h = 360, format = 0;

    GetFileVersionInfoSizeA("kernel32.dll", &handle); /* pulls in version.dll */
    dm.dmSize = sizeof dm;
    EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm);
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
    printf("ChangeDisplaySettingsA -> %ld\n", ChangeDisplaySettingsA(&dm, CDS_FULLSCREEN));

    wc.style = CS_OWNDC;
    wc.lpfnWndProc = proc;
    wc.lpszClassName = "harness";
    RegisterClassA(&wc);

    if (msaa) { /* WGL_ARB_pixel_format needs a context first */
        HWND tmp = CreateWindowExA(0, "harness", "", WS_POPUP, 0, 0, 1, 1, 0, 0, 0, 0);
        HDC tdc = GetDC(tmp);
        SetPixelFormat(tdc, ChoosePixelFormat(tdc, &pfd), &pfd);
        HGLRC trc = wglCreateContext(tdc);
        wglMakeCurrent(tdc, trc);
        PFNWGLCHOOSEPIXELFORMATARBPROC choose = (void *)wglGetProcAddress("wglChoosePixelFormatARB");
        int attrs[] = {WGL_DRAW_TO_WINDOW_ARB, 1, WGL_SUPPORT_OPENGL_ARB, 1, WGL_DOUBLE_BUFFER_ARB, 1,
                       WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB, WGL_COLOR_BITS_ARB, 32, WGL_DEPTH_BITS_ARB, 24,
                       WGL_SAMPLE_BUFFERS_ARB, 1, WGL_SAMPLES_ARB, 2, 0};
        UINT count = 0;
        if (!choose || !choose(tdc, attrs, NULL, 1, &format, &count) || !count) { puts("no msaa format"); return 1; }
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(trc);
        DestroyWindow(tmp);
    }

    /* Offscreen and non-activating, so it does not disturb the desktop. */
    HWND win = CreateWindowExA(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, "harness", "harness",
                               WS_POPUP, -20000, -20000, w, h, 0, 0, 0, 0);
    ShowWindow(win, SW_SHOWNOACTIVATE);
    HDC dc = GetDC(win);
    if (!format) format = ChoosePixelFormat(dc, &pfd);
    SetPixelFormat(dc, format, &pfd);
    HGLRC rc = wglCreateContext(dc);
    wglMakeCurrent(dc, rc);
    printf("GL %s, msaa %d\n", (const char *)glGetString(GL_VERSION), msaa);

    PFNWGLSWAPINTERVALEXTPROC interval = (void *)wglGetProcAddress("wglSwapIntervalEXT");
    if (interval) interval(1);

    /* Long enough for the DLL's OpenGL warm-up before DXGI takes over. */
    for (DWORD start = GetTickCount(); GetTickCount() - start < 2500;) {
        MSG msg;
        while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(1, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST); /* left enabled across SwapBuffers on purpose */
        glScissor(0, 0, w, h / 2);
        glClearColor(0, 1, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        if (!SwapBuffers(dc)) { puts("SwapBuffers failed"); return 1; }
    }
    GLint fbo = -1;
    glGetIntegerv(0x8CA6 /* GL_DRAW_FRAMEBUFFER_BINDING */, &fbo);
    printf("after swaps: scissor %s, draw fbo %d, glGetError %u\n",
           glIsEnabled(GL_SCISSOR_TEST) ? "on" : "OFF", fbo, glGetError());
    return 0;
}
