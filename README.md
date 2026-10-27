# You Are Empty: Absolute Edition — Borderless Fullscreen

Makes the game's fullscreen mode a borderless window at desktop resolution:
no display mode switch, no minimize on alt-tab, no blink, the game keeps
rendering behind other windows.

The original game switches the display into exclusive fullscreen, minimizes
itself to the taskbar whenever it loses focus, and drops to 5 FPS in the
background. This mod fixes all three without modifying any game file: it is a
single DLL you drop next to the game and delete to uninstall.

Unofficial community mod, not affiliated with or endorsed by the game's
developers or publishers. The repository contains no game files.

## Install

1. Download the latest release zip and copy `version.dll` and
   `yae_borderless.ini` next to `you_are_empty.exe`
   (Steam: right-click the game → Manage → Browse local files).
2. In game settings keep **Fullscreen** on and the resolution equal to your
   desktop resolution (the default `screen_w 0` / `screen_h 0` does this).

Uninstall: delete both files.

Linux / Steam Deck (Proton): also set the launch option
`WINEDLLOVERRIDES="version=n,b" %command%`.

## Settings (`yae_borderless.ini`)

| Key | Values | Default |
|---|---|---|
| `mode` | `borderless`, `exclusive` (mod off) | `borderless` |
| `present` | `dxgi`, `opengl` | `dxgi` |
| `background_fps` | `full`, `throttled` (game's original 5 FPS) | `full` |

`yae_borderless.log` next to the DLL shows what the mod did on the last run.

## Behaviour

- Fullscreen at desktop resolution → borderless window, not topmost.
- Fullscreen at any other resolution → original exclusive fullscreen.
- Windowed mode → unchanged.
- Primary monitor only.

## Troubleshooting

- **Nothing changed.** Check that `yae_borderless.log` appears next to the
  game after launching. If it does not, the DLL was not loaded: make sure it
  is named `version.dll` and sits next to `you_are_empty.exe`. On Proton, set
  the `WINEDLLOVERRIDES` launch option above.
- **Still exclusive / screen flickers on launch.** The log says
  `differs from the desktop: exclusive` when the game resolution does not
  match your desktop. Set the in-game resolution to your desktop resolution.
- **Antivirus removed `version.dll`.** DLL proxies are sometimes flagged by
  heuristic scanners. Check the file against the release `SHA256SUMS.txt`
  and restore it from quarantine, or build it yourself (below).
- **Black screen or rendering issues.** Set `present = opengl` and send the
  log with the bug report.

## How it works

`version.dll` is a proxy that forwards all 17 exports to the system DLL.
`SDL3.dll` and `ds2debug.dll` import it, so it loads at startup. It patches
imports of the game modules by name, so it does not depend on exe offsets.

| Module | Import | Change |
|---|---|---|
| `you_are_empty.exe` | `ChangeDisplaySettingsA` | Fullscreen at the desktop size returns success without changing the display |
| `you_are_empty.exe` | `CreateWindowExA` | Drops `WS_EX_TOPMOST` from the fullscreen popup |
| `you_are_empty.exe`, `ds2kernel.dll`, `ds2base.dll` | `LoadLibrary*` | Hooks `ds2render.dll` once the engine loads it |
| `ds2kernel.dll` | `CloseWindow` | Skipped: the engine minimizes the window on focus loss while `scr_fullscreen` is set |
| `ds2kernel.dll` | `Sleep` | Skips the engine's `Sleep(200)` per frame while inactive |
| `ds2render.dll` | `SwapBuffers` | Presents through DXGI (below) |
| `ds2render.dll` | `wglGetProcAddress` | Wraps `wglSwapIntervalEXT` so vsync carries over to DXGI |

### DXGI present

A borderless OpenGL window that exactly covers the monitor gets direct
scanout from the NVIDIA OpenGL driver, and leaving it on alt-tab blinks.
DXGI flip model makes that switch seamless, so the mod presents through it:

1. The game renders to its default framebuffer as before.
2. At `SwapBuffers` the frame is resolved (MSAA) into a GL renderbuffer, then
   blitted with a vertical flip into a D3D11 texture shared through
   `WGL_NV_DX_interop2`.
3. The texture is copied to a flip-model swap chain back buffer and presented.

If any step fails (no interop support, old Windows, device lost), the log
says why and the mod falls back to the OpenGL present with the window 1px
taller than the screen and moved 1px up, which keeps the driver from treating
it as fullscreen. `present = opengl` forces that fallback.

## Build

Needs `i686-w64-mingw32-gcc` and `windres`.

```
mod/build.sh   # → mod/dist/version.dll, mod/dist/yae_borderless.ini
```

`mod/test/harness.c` is a small OpenGL app that follows the game's window and
present calls. Build it with
`i686-w64-mingw32-gcc -O2 -o harness.exe mod/test/harness.c -lversion -lopengl32 -lgdi32`,
put it next to the DLL with `selftest = 1` in the ini, and the log reports the
presented pixels (red top, green bottom). `harness.exe msaa` tests a 2x MSAA
framebuffer.

## Releases

GitHub Actions (`.github/workflows/build.yml`) builds every push. Pushing a
tag like `v1.0.0` builds that version, packages
`yae-borderless-1.0.0.zip` with `SHA256SUMS.txt`, and publishes a GitHub
release with the matching `CHANGELOG.md` section as notes.

```
git tag v1.0.0 && git push origin v1.0.0
```

Code signing runs when the repository has two secrets: `SIGNING_PFX_BASE64`
(an Authenticode certificate, `base64 -w0 cert.pfx`) and
`SIGNING_PFX_PASSWORD`. Without them, releases are unsigned.

## License

MIT, see [LICENSE](LICENSE).
