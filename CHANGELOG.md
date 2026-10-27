# Changelog

## 1.0.0

- Borderless fullscreen at desktop resolution instead of a display mode switch.
- No minimize on alt-tab; the window is no longer topmost.
- Frames are presented through DXGI flip model, so alt-tab does not blink.
  Falls back to the OpenGL present when the driver lacks `WGL_NV_DX_interop2`.
- Full frame rate while the game is in the background (`background_fps`).
- `yae_borderless.ini` settings and `yae_borderless.log` diagnostics.
