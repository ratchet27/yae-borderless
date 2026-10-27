#ifndef BORDERLESS_H
#define BORDERLESS_H

#include <windows.h>

void log_line(const char *format, ...);

/* DXGI presenter (present.c). Called from the SwapBuffers hook with the
 * game's GL context current. Returns FALSE when the frame was not presented;
 * after the first failure it stays disabled and the caller falls back to the
 * driver's SwapBuffers. */
BOOL present_frame(HWND window, BOOL selftest);
void present_set_interval(int interval);
/* Releases the swap chain so the driver's SwapBuffers presents again. Needs
 * the GL context current. */
void present_stop(void);

/* Display Mode option in the video settings (ui.c). `patch` replaces one
 * import of a module and returns the original. */
void ui_hook_modules(void *(*patch)(HMODULE, const char *, const char *, void *));
void ui_set_mode(BOOL borderless);
BOOL ui_mode_borderless(void);
/* Called by ui.c when OK applies a new Display Mode (borderless.c). */
void on_display_mode_committed(BOOL borderless);

#endif
