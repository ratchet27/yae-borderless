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
 *                           alt-tab shows other windows on top of the game,
 *                           and subclasses it to keep it from being minimized.
 *   LoadLibrary*            ds2kernel.dll and ds2render.dll are loaded at
 *                           runtime; their imports are patched as soon as they
 *                           appear. Hooks are re-checked every second, since
 *                           the exe has its own hook layer.
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
 *                           (present.c) after a short OpenGL warm-up. With
 *                           present = opengl, or if DXGI is unavailable, the
 *                           window is made 1px taller and moved 1px up instead:
 *                           a window that exactly matches the monitor gets
 *                           direct scanout from the NVIDIA OpenGL driver, and
 *                           leaving it on alt-tab blinks. GL draws bottom-up,
 *                           so the extra row is the hidden top one.
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
    BOOL borderless;
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
    config.borderless = !setting_is(ini, "mode", "borderless", "exclusive");
    config.dxgi = !setting_is(ini, "present", "dxgi", "opengl");
    config.full_background_fps = !setting_is(ini, "background_fps", "full", "throttled");
    config.selftest = setting_is(ini, "selftest", "0", "1");
}

/* Written by the Display Mode option; takes effect on the next launch. */
static void save_mode(BOOL borderless)
{
    char ini[MAX_PATH];
    sibling_path(ini, "yae_borderless.ini");
    WritePrivateProfileStringA("borderless", "mode", borderless ? "borderless" : "exclusive", ini);
    log_line("display mode saved: %s", borderless ? "borderless" : "exclusive");
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

/* The import address table slot of `func` in `module`, or NULL. */
static ULONG_PTR *find_import(HMODULE module, const char *dll, const char *func)
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
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            by_name = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (lstrcmpA((const char *)by_name->Name, func) == 0)
                return &slots->u1.Function;
        }
    }
    return NULL;
}

static void write_slot(ULONG_PTR *slot, void *value)
{
    DWORD old;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old))
        return;
    *slot = (ULONG_PTR)value;
    VirtualProtect(slot, sizeof(*slot), old, &old);
}

/* Replace one import of `module`, return the original pointer or NULL. */
static void *patch_import(HMODULE module, const char *dll, const char *func, void *hook)
{
    ULONG_PTR *slot = find_import(module, dll, func);
    void *original;
    if (!slot || (void *)*slot == hook)
        return NULL;
    original = (void *)*slot;
    write_slot(slot, hook);
    return original;
}

/* Hooks installed with HOOK, re-checked by verify_hooks. */
static struct installed_hook {
    ULONG_PTR *slot;
    void *hook;
    void **real;
    const char *func;
} installed[32];
static int installed_count;

static void remember_hook(HMODULE module, const char *dll, const char *func, void *hook,
                          void **real)
{
    ULONG_PTR *slot = find_import(module, dll, func);
    if (slot && (void *)*slot == hook && installed_count < 32)
        installed[installed_count++] = (struct installed_hook){slot, hook, real, func};
}

/* Other hook layers (the remaster patches engine imports at runtime too) can
 * overwrite a slot after us. Chain to whatever is there now and hook again. */
static void verify_hooks(void)
{
    for (int i = 0; i < installed_count; i++) {
        struct installed_hook *h = &installed[i];
        void *current = (void *)*h->slot;
        HMODULE owner = NULL;
        char name[MAX_PATH] = "?";
        if (current == h->hook)
            continue;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)current, &owner))
            GetModuleFileNameA(owner, name, MAX_PATH);
        log_line("hook on %s was replaced by %s, chaining", h->func, name);
        *h->real = current;
        write_slot(h->slot, h->hook);
    }
}

/* Patch `func` in `module` and keep the first original seen. */
#define HOOK(module, dll, func)                                                      \
    do {                                                                             \
        void *original_ = patch_import(module, dll, #func, (void *)hook_##func);     \
        if (original_ && !real_##func)                                              \
            real_##func = (func##_fn)original_;                                      \
        remember_hook(module, dll, #func, (void *)hook_##func, (void **)&real_##func); \
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
typedef HMODULE(WINAPI *LoadLibraryExA_fn)(LPCSTR, HANDLE, DWORD);
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
static LoadLibraryExA_fn real_LoadLibraryExA;
static SwapBuffers_fn real_SwapBuffers;
static wglGetProcAddress_fn real_wglGetProcAddress;
static wglSwapIntervalEXT_fn real_wglSwapIntervalEXT;

static BOOL borderless;
static HWND game_window;
static RECT game_rect; /* as the game created it */
static DEVMODEA fullscreen_mode;
static BOOL have_fullscreen_mode;
static BOOL legacy_offset;
static BOOL render_hooked, kernel_hooked, base_hooked;
static BOOL dxgi_failed;
#define DXGI_WARMUP_LAUNCH_MS 1500 /* long enough for the Steam overlay */
#define DXGI_WARMUP_SWITCH_MS 150  /* frames are not shown yet; keep it short */
static DWORD warmup_start, warmup_ms = DXGI_WARMUP_LAUNCH_MS;

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
    BOOL fullscreen = mode && (flags & CDS_FULLSCREEN) && (mode->dmFields & DM_PELSWIDTH);
    /* The first fullscreen request is the game's mode; later ones restore the
     * desktop on exit. The mode may have been switched live since startup. */
    BOOL want_borderless = game_window ? borderless : config.borderless;

    if (fullscreen && !game_window) {
        fullscreen_mode = *mode;
        have_fullscreen_mode = TRUE;
    }
    if (want_borderless && fullscreen) {
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

static void hook_modules(void);
static WNDPROC game_wndproc;
static LRESULT CALLBACK hook_wndproc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

static HWND WINAPI hook_CreateWindowExA(DWORD ex_style, LPCSTR class_name, LPCSTR title,
                                        DWORD style, int x, int y, int w, int h,
                                        HWND parent, HMENU menu, HINSTANCE inst,
                                        LPVOID param)
{
    BOOL game = have_fullscreen_mode && (style & WS_POPUP) && !parent;
    RECT rect;
    HWND window;

    SetRect(&rect, x, y, x + w, y + h);
    if (game && borderless) {
        /* Starts with the OpenGL present; see present_borderless. */
        ex_style &= ~WS_EX_TOPMOST;
        y -= 1;
        h += 1;
        legacy_offset = TRUE;
    }
    window = real_CreateWindowExA(ex_style, class_name, title, style, x, y, w, h, parent, menu,
                                  inst, param);
    if (game && window) {
        game_window = window;
        game_rect = rect;
        game_wndproc = (WNDPROC)SetWindowLongPtrA(window, GWLP_WNDPROC, (LONG_PTR)hook_wndproc);
        log_line("game window %dx%d at %d,%d", w, h, x, y);
        hook_modules();
    }
    return window;
}

/* Borderless must stay visible on focus loss. The engine's own minimize is
 * skipped in hook_CloseWindow; anything else that minimizes the window is
 * blocked or undone here. */
static LRESULT CALLBACK hook_wndproc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (borderless && message == WM_SYSCOMMAND && (wparam & 0xfff0) == SC_MINIMIZE)
        return 0;
    if (borderless && message == WM_SIZE && wparam == SIZE_MINIMIZED) {
        log_line("window minimized by something other than the engine, restoring");
        ShowWindowAsync(window, SW_SHOWNOACTIVATE);
    }
    return CallWindowProcA(game_wndproc, window, message, wparam, lparam);
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
    hook_modules();
    return module;
}

static HMODULE WINAPI hook_LoadLibraryW(LPCWSTR name)
{
    HMODULE module = real_LoadLibraryW(name);
    hook_modules();
    return module;
}

static HMODULE WINAPI hook_LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags)
{
    HMODULE module = real_LoadLibraryExA(name, file, flags);
    hook_modules();
    return module;
}

static HMODULE WINAPI hook_LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags)
{
    HMODULE module = real_LoadLibraryExW(name, file, flags);
    hook_modules();
    return module;
}

/* Exclusive and borderless at the desktop resolution differ only in what this
 * mod changes, so the Display Mode option switches between them live. */
static void switch_mode(BOOL to_borderless)
{
    int w = game_rect.right - game_rect.left, h = game_rect.bottom - game_rect.top;

    if (!game_window || !have_fullscreen_mode || to_borderless == borderless ||
        !matches_desktop(&fullscreen_mode))
        return;
    if (to_borderless) {
        real_ChangeDisplaySettingsA(NULL, 0);
        borderless = TRUE;
        legacy_offset = TRUE;
        warmup_start = 0;
        warmup_ms = DXGI_WARMUP_SWITCH_MS;
        SetWindowPos(game_window, HWND_NOTOPMOST, game_rect.left, game_rect.top - 1, w, h + 1,
                     SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    } else {
        /* SwapBuffers stops the DXGI present on the next frame. */
        borderless = FALSE;
        legacy_offset = FALSE;
        SetWindowPos(game_window, HWND_TOPMOST, game_rect.left, game_rect.top, w, h,
                     SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        real_ChangeDisplaySettingsA(&fullscreen_mode, CDS_FULLSCREEN);
    }
    log_line("switched to %s", to_borderless ? "borderless" : "exclusive");
}

void on_display_mode_committed(BOOL to_borderless)
{
    save_mode(to_borderless);
    switch_mode(to_borderless);
}

/* Fallback for when DXGI is unavailable; see the header comment. */
static void apply_legacy_offset(void)
{
    legacy_offset = TRUE;
    SetWindowPos(game_window, NULL, game_rect.left, game_rect.top - 1,
                 game_rect.right - game_rect.left, game_rect.bottom - game_rect.top + 1,
                 SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

/* Each start of the DXGI present (launch, switch from exclusive) is preceded
 * by a short OpenGL warm-up with the window 1px taller. It takes the window
 * out of the NVIDIA driver's fullscreen present after exclusive mode, which
 * otherwise hides the DXGI frames until the next alt-tab, and lets the Steam
 * overlay, which hooks the OpenGL present, initialize. Returns TRUE when the
 * frame went through DXGI. */
static BOOL present_borderless(void)
{
    int w = game_rect.right - game_rect.left, h = game_rect.bottom - game_rect.top;
    DWORD now = GetTickCount();
    RECT client;

    if (!config.dxgi || dxgi_failed) {
        if (!legacy_offset)
            apply_legacy_offset();
        return FALSE;
    }
    if (!warmup_start)
        warmup_start = now ? now : 1;
    if (now - warmup_start < warmup_ms)
        return FALSE;
    if (legacy_offset) {
        legacy_offset = FALSE;
        SetWindowPos(game_window, NULL, game_rect.left, game_rect.top, w, h,
                     SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    }
    /* Wait for the resize so the swap chain is created at the final size. */
    GetClientRect(game_window, &client);
    if (client.bottom - client.top != h)
        return FALSE;
    if (present_frame(game_window, config.selftest))
        return TRUE;
    dxgi_failed = TRUE;
    apply_legacy_offset();
    return FALSE;
}

static BOOL WINAPI hook_SwapBuffers(HDC dc)
{
    /* Engine modules can appear late and other hook layers can replace ours. */
    static DWORD last_check;
    DWORD now = GetTickCount();
    HWND window = WindowFromDC(dc);
    if (now - last_check >= 1000) {
        last_check = now;
        hook_modules();
        verify_hooks();
    }
    if (game_window && window == game_window) {
        if (!borderless)
            present_stop();
        else if (present_borderless())
            return TRUE;
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

/* Patch engine modules once each appears. Since the October 2026 update the
 * exe loads ds2kernel.dll at runtime instead of importing it. */
static void hook_modules(void)
{
    HMODULE module;
    if (!kernel_hooked && (module = GetModuleHandleA("ds2kernel.dll"))) {
        kernel_hooked = TRUE;
        HOOK(module, "user32.dll", CloseWindow);
        if (config.full_background_fps)
            HOOK(module, "kernel32.dll", Sleep);
        HOOK(module, "kernel32.dll", LoadLibraryA);
        log_line("ds2kernel.dll hooked (CloseWindow %s, Sleep %s)",
                 real_CloseWindow ? "ok" : "missing", real_Sleep ? "ok" : "off");
    }
    if (!base_hooked && (module = GetModuleHandleA("ds2base.dll"))) {
        base_hooked = TRUE;
        HOOK(module, "kernel32.dll", LoadLibraryA);
    }
    if (!render_hooked && (module = GetModuleHandleA("ds2render.dll"))) {
        render_hooked = TRUE;
        HOOK(module, "gdi32.dll", SwapBuffers);
        HOOK(module, "opengl32.dll", wglGetProcAddress);
        log_line("ds2render.dll hooked (SwapBuffers %s)", real_SwapBuffers ? "ok" : "missing");
    }
    ui_hook_modules(patch_import);
}

static void install_hooks(void)
{
    HMODULE exe = GetModuleHandleA(NULL);

    HOOK(exe, "user32.dll", ChangeDisplaySettingsA);
    if (!real_ChangeDisplaySettingsA)
        return;
    open_log();
    log_line("yae_borderless loaded (mode %s, present %s, background fps %s)",
             config.borderless ? "borderless" : "exclusive", config.dxgi ? "dxgi" : "opengl",
             config.full_background_fps ? "full" : "throttled");

    HOOK(exe, "user32.dll", CreateWindowExA);
    HOOK(exe, "kernel32.dll", LoadLibraryA);
    HOOK(exe, "kernel32.dll", LoadLibraryW);
    HOOK(exe, "kernel32.dll", LoadLibraryExW);
    HOOK(exe, "kernel32.dll", LoadLibraryExA);
    /* The game swaps from ds2render.dll; these only matter for test hosts. */
    HOOK(exe, "gdi32.dll", SwapBuffers);
    HOOK(exe, "opengl32.dll", wglGetProcAddress);
    hook_modules();
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        self = inst;
        DisableThreadLibraryCalls(inst);
        load_real_version();
        load_config();
        ui_set_mode(config.borderless);
        install_hooks();
    }
    return TRUE;
}
