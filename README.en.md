# You Are Empty: Absolute Edition — Borderless Fullscreen

[Русский](README.md) · **English**

Adds a borderless fullscreen mode to You Are Empty: Absolute Edition:

- the game no longer switches your monitor's display mode;
- Alt+Tab is instant, and the game stays visible behind other windows instead
  of minimizing;
- the game keeps running at full frame rate in the background instead of
  5 FPS;
- the video settings get a **Display Mode** option: Windowed, Fullscreen or
  Borderless.

No game files are modified: the mod is a single DLL you drop next to the game
and delete to uninstall.

## Install

1. Download the latest zip from [Releases](../../releases).
2. Copy `version.dll` and `yae_borderless.ini` next to `you_are_empty.exe`.
   In Steam: right-click the game → Manage → Browse local files.
3. Start the game. In Options → Video, set **Display Mode: Borderless** and a
   resolution that matches your desktop.

The game's text is localized; in the Russian version the option is called
«Режим экрана» with «Оконный», «Полный экран» and «Без рамки».

Linux / Steam Deck (Proton): set the launch option
`WINEDLLOVERRIDES="version=n,b" %command%`.

### Uninstall

Delete `version.dll` and `yae_borderless.ini` from the game folder.

## Display modes

| Mode | What happens |
|---|---|
| Windowed | A normal window, as in the original game. |
| Fullscreen | The original exclusive fullscreen: Alt+Tab minimizes the game. |
| Borderless | A borderless window covering the screen at desktop resolution. |

Switching between Fullscreen and Borderless applies as soon as you press OK.
Switching to or from Windowed needs a restart, as in the original game.

Borderless needs the game resolution to match your desktop resolution. At any
other resolution the game starts in exclusive fullscreen.

## Settings

`yae_borderless.ini` next to the game. The display mode is set from the game;
edit the rest by hand.

| Key | Values | Default |
|---|---|---|
| `mode` | `borderless`, `exclusive` (fullscreen) | `borderless` |
| `present` | `dxgi`, `opengl` | `dxgi` |
| `background_fps` | `full`, `throttled` (5 FPS, as in the original) | `full` |

`present`:

- `dxgi` presents frames through DXGI, like modern DirectX games. Lowest
  latency and no blink on Alt+Tab. The Steam overlay becomes available a
  couple of seconds after launch.
- `opengl` lets the game present through OpenGL, with the window 1 pixel taller
  than the screen so the driver does not treat it as exclusive fullscreen.
  Slightly more latency (about one frame), maximum compatibility. Used
  automatically when DXGI is unavailable.

## Troubleshooting

After launch, `yae_borderless.log` next to the game says what the mod did.
Attach it to bug reports.

- **Nothing changed and there is no log.** The DLL was not loaded: check that
  it is named `version.dll` and sits next to `you_are_empty.exe`. On Proton,
  set the launch option from Install.
- **Still exclusive fullscreen.** The log says
  `differs from the desktop: exclusive`: the game resolution does not match
  your desktop.
- **Antivirus removed `version.dll`.** Heuristic scanners sometimes flag DLL
  proxies. Check the file against the release `SHA256SUMS.txt` and restore it
  from quarantine, or build the mod yourself.
- **Black screen or rendering issues.** Set `present = opengl` in
  `yae_borderless.ini`.

Tested on Windows 11 with an NVIDIA GPU and the Steam version of the game
(October 2026 update). Not tested on AMD or Intel GPUs.

## How it works

`version.dll` is a proxy for the system library: all its functions are
forwarded to the real `version.dll`, and on load the mod replaces a few
imports of the game modules:

- a display mode change to the desktop resolution is reported as done, and
  the game window is created without "always on top";
- the minimize and the 200 ms per-frame pause the engine does on focus loss
  are skipped;
- OpenGL frames are copied into a D3D11 texture (`WGL_NV_DX_interop2`) and
  presented through a DXGI flip-model swap chain;
- the options window gets a Display Mode dropdown; the original "Full Screen"
  checkbox is hidden and driven by it.

Details are in the comments in `mod/src`.

## Building

Needs `i686-w64-mingw32-gcc` (MinGW-w64).

```
mod/build.sh   # → mod/dist/version.dll, mod/dist/yae_borderless.ini
```

## License

MIT, see [LICENSE](LICENSE).

Unofficial fan mod, not affiliated with or endorsed by the game's developers
or publishers. The repository contains no game files.
