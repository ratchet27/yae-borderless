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

#endif
