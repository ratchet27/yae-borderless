# Changelog

## 1.1.0

- New **Display Mode** option in Options → Video: Windowed, Fullscreen
  (exclusive) or Borderless. Fullscreen ↔ Borderless applies immediately.
- Works with the October 2026 game update, which loads `ds2kernel.dll` at
  runtime: the game no longer minimizes on Alt+Tab and keeps full frame rate
  in the background again.
- DXGI present starts after a short OpenGL warm-up, so the Steam overlay works
  from launch and switching from Fullscreen to Borderless does not freeze.
- Hooks replaced by other hook layers are detected and restored.
- `mode = exclusive` now means the game's original fullscreen; the Display
  Mode option stays available.

## 1.0.0

- Borderless fullscreen at desktop resolution instead of a display mode switch.
- No minimize on alt-tab; the window is no longer topmost.
- Frames are presented through DXGI flip model, so alt-tab does not blink.
  Falls back to the OpenGL present when the driver lacks `WGL_NV_DX_interop2`.
- Full frame rate while the game is in the background (`background_fps`).
- `yae_borderless.ini` settings and `yae_borderless.log` diagnostics.
