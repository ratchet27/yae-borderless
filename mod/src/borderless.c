/*
 * You Are Empty: Absolute Edition - borderless fullscreen.
 *
 * Drop-in version.dll proxy. Every version.dll export jumps to the real
 * system DLL. On load it patches imports of the game modules by name:
 *
 * you_are_empty.exe
 *   ChangeDisplaySettingsA  A fullscreen request for the mode the desktop
 *                           already uses is reported as done without touching
 *                           the display. Other modes are passed through, so
 *                           non-native resolutions still get exclusive mode.
 *   CreateWindowExA         Drops WS_EX_TOPMOST from the fullscreen popup, so
 *                           alt-tab shows other windows on top of the game.
 *   LoadLibrary*            ds2render.dll is loaded at runtime (render_dll);
 *                           its imports are patched as soon as it appears.
 *
 * ds2kernel.dll
 *   CloseWindow             The engine minimizes the window on WM_ACTIVATEAPP
 *                           while scr_fullscreen is set. Skipped in borderless.
 *   Sleep                   The engine sleeps 200ms per frame while inactive
 *                           (5 FPS behind other windows). That one call is the
 *                           only Sleep(200) in ds2kernel; it is skipped.
 *
 * ds2render.dll
 *   SwapBuffers             Frames go through a DXGI flip-model swap chain
 *                           (present.c). If that is unavailable, the window is
 *                           made 1px taller and moved 1px up instead: a window
 *                           that exactly matches the monitor gets direct
 *                           scanout from the NVIDIA OpenGL driver, and leaving
 *                           it on alt-tab blinks. GL draws bottom-up, so the
 *                           extra row is the hidden top one.
 *   wglGetProcAddress       Wraps wglSwapIntervalEXT so vsync carries over to
 *                           the DXGI present.
 *
 * Settings are read from yae_borderless.ini next to this DLL; progress is
 * written to yae_borderless.log.
 */
#include <windows.h>

#include "borderless.h"

static HMODULE self;
static HMODULE real_version;

/* ---- version.dll forwarding ---------------------------------------------- */

#define EXPORTS(X)                                                              \
    X(GetFileVersionInfoA) X(GetFileVersionInfoByHandle) X(GetFileVersionInfoExA) \
    X(GetFileVersionInfoExW) X(GetFileVersionInfoSizeA) X(GetFileVersionInfoSizeExA) \
    X(GetFileVersionInfoSizeExW) X(GetFileVersionInfoSizeW) X(GetFileVersionInfoW) \
    X(VerFindFileA) X(VerFindFileW) X(VerInstallFileA) X(VerInstallFileW)      \
    X(VerLanguageNameA) X(VerLanguageNameW) X(VerQueryValueA) X(VerQueryValueW)

#define DECLARE_POINTER(name) FARPROC real_##name;
EXPORTS(DECLARE_POINTER)

/* Plain jmp thunks keep the caller's stack and registers untouched. */
#define DEFINE_THUNK(name) \
    __asm__(".globl _fwd_" #name "\n_fwd_" #name ":\n\tjmp *_real_" #name "\n");
EXPORTS(DEFINE_THUNK)

static void load_real_version(void)
{
    char path[MAX_PATH];
    UINT len = GetSystemDirectoryA(path, MAX_PATH);
    if (len == 0 || len + sizeof("\\version.dll") > MAX_PATH)
        return;
    lstrcatA(path, "\\version.dll");
    real_version = LoadLibraryA(path);
    if (!real_version)
        return;
#define RESOLVE(name) real_##name = GetProcAddress(real_version, #name);
    EXPORTS(RESOLVE)
}

/* ---- settings and log ----------------------------------------------------- */

static struct {
    BOOL enabled;
    BOOL dxgi;
    BOOL full_background_fps;
    BOOL selftest;
} config;

static HANDLE log_file = INVALID_HANDLE_VALUE;

/* Path of a file next to this DLL. */
static void sibling_path(char *path, const char *name)
{
    DWORD len = GetModuleFileNameA(self, path, MAX_PATH);
    while (len > 0 && path[len - 1] != '\\' && path[len - 1] != '/')
        len--;
    path[len] = 0;
    if (len + lstrlenA(name) < MAX_PATH)
        lstrcatA(path, name);
}

static BOOL setting_is(const char *ini, const char *key, const char *fallback, const char *value)
{
    char buffer[32];
    GetPrivateProfileStringA("borderless", key, fallback, buffer, sizeof(buffer), ini);
    return lstrcmpiA(buffer, value) == 0;
}

static void load_config(void)
{
    char ini[MAX_PATH];
    sibling_path(ini, "yae_borderless.ini");
    config.enabled = !setting_is(ini, "mode", "borderless", "exclusive");
    config.dxgi = !setting_is(ini, "present", "dxgi", "opengl");
    config.full_background_fps = !setting_is(ini, "background_fps", "full", "throttled");
    config.selftest = setting_is(ini, "selftest", "0", "1");
}

static void open_log(void)
{
    char path[MAX_PATH];
    sibling_path(path, "yae_borderless.log");
    log_file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
}

void log_line(const char *format, ...)
{
    char line[512];
    va_list args;
    DWORD written;
    int len;

    if (log_file == INVALID_HANDLE_VALUE)
        return;
    va_start(args, format);
    len = wvsprintfA(line, format, args);
    va_end(args);
    if (len < 0 || len > (int)sizeof(line) - 3)
        return;
    line[len++] = '\r';
    line[len++] = '\n';
    WriteFile(log_file, line, len, &written, NULL);
}

/* ---- import patching ------------------------------------------------------ */

/* Replace one import of `module`, return the original pointer or NULL. */
static void *patch_import(HMODULE module, const char *dll, const char *func, void *hook)
{
    BYTE *base = (BYTE *)module;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *desc;

    if (!dir.VirtualAddress)
        return NULL;
    for (desc = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); desc->Name; desc++) {
        IMAGE_THUNK_DATA *names, *slots;
        if (lstrcmpiA((const char *)(base + desc->Name), dll) != 0)
            continue;
        names = (IMAGE_THUNK_DATA *)(base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk
                                                                      : desc->FirstThunk));
        slots = (IMAGE_THUNK_DATA *)(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            IMAGE_IMPORT_BY_NAME *by_name;
            void *original;
            DWORD old;
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            by_name = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (lstrcmpA((const char *)by_name->Name, func) != 0)
                continue;
            original = (void *)slots->u1.Function;
            if (original == hook)
                return NULL;
            if (!VirtualProtect(&slots->u1.Function, sizeof(void *), PAGE_READWRITE, &old))
                return NULL;
            slots->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&slots->u1.Function, sizeof(void *), old, &old);
            return original;
        }
    }
    return NULL;
}

/* Patch `func` in `module` and keep the first original seen. */
#define HOOK(module, dll, func)                                                      \
    do {                                                                             \
        void *original_ = patch_import(module, dll, #func, (void *)hook_##func);     \
        if (original_ && !real_##func)                                              \
            real_##func = (func##_fn)original_;                                      \
    } while (0)

/* ---- hooks ---------------------------------------------------------------- */

typedef LONG(WINAPI *ChangeDisplaySettingsA_fn)(DEVMODEA *, DWORD);
typedef HWND(WINAPI *CreateWindowExA_fn)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int,
                                         HWND, HMENU, HINSTANCE, LPVOID);
typedef BOOL(WINAPI *CloseWindow_fn)(HWND);
typedef void(WINAPI *Sleep_fn)(DWORD);
typedef HMODULE(WINAPI *LoadLibraryA_fn)(LPCSTR);
typedef HMODULE(WINAPI *LoadLibraryW_fn)(LPCWSTR);
typedef HMODULE(WINAPI *LoadLibraryExW_fn)(LPCWSTR, HANDLE, DWORD);
typedef BOOL(WINAPI *SwapBuffers_fn)(HDC);
typedef PROC(WINAPI *wglGetProcAddress_fn)(LPCSTR);
typedef BOOL(WINAPI *wglSwapIntervalEXT_fn)(int);

#define INACTIVE_FRAME_SLEEP_MS 200

static ChangeDisplaySettingsA_fn real_ChangeDisplaySettingsA;
static CreateWindowExA_fn real_CreateWindowExA;
static CloseWindow_fn real_CloseWindow;
static Sleep_fn real_Sleep;
static LoadLibraryA_fn real_LoadLibraryA;
static LoadLibraryW_fn real_LoadLibraryW;
static LoadLibraryExW_fn real_LoadLibraryExW;
static SwapBuffers_fn real_SwapBuffers;
static wglGetProcAddress_fn real_wglGetProcAddress;
static wglSwapIntervalEXT_fn real_wglSwapIntervalEXT;

static BOOL borderless;
static HWND game_window;
static RECT game_rect;
static BOOL legacy_offset;
static BOOL render_hooked;

static BOOL matches_desktop(const DEVMODEA *mode)
{
    DEVMODEA desktop = {0};
    desktop.dmSize = sizeof(desktop);
    if (!EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &desktop))
        return FALSE;
    return mode->dmPelsWidth == desktop.dmPelsWidth &&
           mode->dmPelsHeight == desktop.dmPelsHeight;
}

static LONG WINAPI hook_ChangeDisplaySettingsA(DEVMODEA *mode, DWORD flags)
{
    if (mode && (flags & CDS_FULLSCREEN) && (mode->dmFields & DM_PELSWIDTH)) {
        if (matches_desktop(mode)) {
            if (!borderless)
                log_line("fullscreen %lux%lu matches the desktop: borderless",
                         mode->dmPelsWidth, mode->dmPelsHeight);
            borderless = TRUE;
            return DISP_CHANGE_SUCCESSFUL;
        }
        log_line("fullscreen %lux%lu differs from the desktop: exclusive", mode->dmPelsWidth,
                 mode->dmPelsHeight);
    }
    return real_ChangeDisplaySettingsA(mode, flags);
}

static void hook_render(void);

static HWND WINAPI hook_CreateWindowExA(DWORD ex_style, LPCSTR class_name, LPCSTR title,
                                        DWORD style, int x, int y, int w, int h,
                                        HWND parent, HMENU menu, HINSTANCE inst,
                                        LPVOID param)
{
    BOOL game = borderless && (style & WS_POPUP) && !parent;
    HWND window;

    if (game) {
        ex_style &= ~WS_EX_TOPMOST;
        if (!config.dxgi) {
            y -= 1;
            h += 1;
            legacy_offset = TRUE;
        }
    }
    window = real_CreateWindowExA(ex_style, class_name, title, style, x, y, w, h, parent, menu,
                                  inst, param);
    if (game && window) {
        game_window = window;
        SetRect(&game_rect, x, y, x + w, y + h);
        log_line("game window %dx%d at %d,%d", w, h, x, y);
        hook_render();
    }
    return window;
}

static BOOL WINAPI hook_CloseWindow(HWND window)
{
    if (borderless)
        return TRUE;
    return real_CloseWindow(window);
}

static void WINAPI hook_Sleep(DWORD ms)
{
    if (borderless && ms == INACTIVE_FRAME_SLEEP_MS)
        return;
    real_Sleep(ms);
}

static HMODULE WINAPI hook_LoadLibraryA(LPCSTR name)
{
    HMODULE module = real_LoadLibraryA(name);
    hook_render();
    return module;
}

static HMODULE WINAPI hook_LoadLibraryW(LPCWSTR name)
{
    HMODULE module = real_LoadLibraryW(name);
    hook_render();
    return module;
}

static HMODULE WINAPI hook_LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags)
{
    HMODULE module = real_LoadLibraryExW(name, file, flags);
    hook_render();
    return module;
}

/* Fallback for when DXGI is unavailable; see the header comment. */
static void apply_legacy_offset(void)
{
    legacy_offset = TRUE;
    SetWindowPos(game_window, NULL, game_rect.left, game_rect.top - 1,
                 game_rect.right - game_rect.left, game_rect.bottom - game_rect.top + 1,
                 SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

static BOOL WINAPI hook_SwapBuffers(HDC dc)
{
    if (game_window && WindowFromDC(dc) == game_window) {
        if (config.dxgi && present_frame(game_window, config.selftest))
            return TRUE;
        if (!legacy_offset)
            apply_legacy_offset();
    }
    return real_SwapBuffers(dc);
}

static BOOL WINAPI hook_wglSwapIntervalEXT(int interval)
{
    present_set_interval(interval);
    return real_wglSwapIntervalEXT(interval);
}

static PROC WINAPI hook_wglGetProcAddress(LPCSTR name)
{
    PROC proc = real_wglGetProcAddress(name);
    if (proc && ((ULONG_PTR)name >> 16) && lstrcmpA(name, "wglSwapIntervalEXT") == 0) {
        real_wglSwapIntervalEXT = (wglSwapIntervalEXT_fn)proc;
        return (PROC)hook_wglSwapIntervalEXT;
    }
    return proc;
}

/* Patch ds2render.dll once it is loaded. */
static void hook_render(void)
{
    HMODULE render;
    if (render_hooked)
        return;
    render = GetModuleHandleA("ds2render.dll");
    if (!render)
        return;
    render_hooked = TRUE;
    HOOK(render, "gdi32.dll", SwapBuffers);
    HOOK(render, "opengl32.dll", wglGetProcAddress);
    log_line("ds2render.dll hooked (SwapBuffers %s)", real_SwapBuffers ? "ok" : "missing");
}

static void install_hooks(void)
{
    HMODULE exe = GetModuleHandleA(NULL);
    HMODULE kernel = GetModuleHandleA("ds2kernel.dll");
    HMODULE base = GetModuleHandleA("ds2base.dll");

    HOOK(exe, "user32.dll", ChangeDisplaySettingsA);
    if (!real_ChangeDisplaySettingsA)
        return;
    open_log();
    log_line("yae_borderless loaded (present %s, background fps %s)",
             config.dxgi ? "dxgi" : "opengl", config.full_background_fps ? "full" : "throttled");

    HOOK(exe, "user32.dll", CreateWindowExA);
    HOOK(exe, "kernel32.dll", LoadLibraryA);
    HOOK(exe, "kernel32.dll", LoadLibraryW);
    HOOK(exe, "kernel32.dll", LoadLibraryExW);
    /* The game swaps from ds2render.dll; these only matter for test hosts. */
    HOOK(exe, "gdi32.dll", SwapBuffers);
    HOOK(exe, "opengl32.dll", wglGetProcAddress);
    if (kernel) {
        HOOK(kernel, "user32.dll", CloseWindow);
        if (config.full_background_fps)
            HOOK(kernel, "kernel32.dll", Sleep);
        HOOK(kernel, "kernel32.dll", LoadLibraryA);
    }
    if (base)
        HOOK(base, "kernel32.dll", LoadLibraryA);
    hook_render();
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        self = inst;
        DisableThreadLibraryCalls(inst);
        load_real_version();
        load_config();
        if (config.enabled)
            install_hooks();
    }
    return TRUE;
}
