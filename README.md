# PerfOverlay

![License: Unlicense](https://img.shields.io/badge/License-Unlicense-blue.svg)
![Platform: Windows](https://img.shields.io/badge/platform-Windows%2010%2F11-blue.svg)
[![Latest Release](https://img.shields.io/github/v/release/Vinodhariharan/PerfOverlay)](https://github.com/Vinodhariharan/PerfOverlay/releases/latest)

A tiny always-on-top Windows HUD showing live CPU, RAM, and per-GPU usage.

- Pure Win32 + GDI + PDH (Performance Data Helper) + DXGI — no .NET, no Electron, no injection.
- Single source file, no third-party dependencies.
- Idle resource use is ~15MB RAM and ~0% CPU (measured), one timer tick per second.
- GPU usage comes from the same `GPU Engine` performance counters Task Manager uses,
  so it works with NVIDIA/AMD/Intel GPUs without any vendor SDK.
- Shows **every physical GPU separately, by name** (e.g. an integrated + discrete
  laptop GPU both show up as their own item) — adapter names come from DXGI,
  engine-usage counters are matched back to each adapter by LUID.
- Fully transparent by default — no box, no border, just "label value%" text
  floating directly over your desktop. A solid background card is one menu
  click away if you'd rather have that.
- Two layouts: **vertical** (stacked rows) and **horizontal** (one line, items
  separated by thin dividers), switchable at runtime — the window resizes and
  stays clamped inside the screen when it does.
- Remembers everything: layout, background, lock state, sparklines, hotkeys,
  auto-hide, and window position are saved and restored automatically between
  runs.
- Optional per-row history sparklines (vertical layout), global hotkeys to
  toggle Show/Hide or Lock without touching the tray, and auto-hide whenever
  a fullscreen app/game takes focus — all configured from a native Settings
  window (menu → Settings...).

## Download

Don't want to build it yourself? Grab the latest build from
**[Releases](https://github.com/Vinodhariharan/PerfOverlay/releases/latest)** —
download `PerfOverlay.exe` and run it, no compiler needed.

> **Windows will likely warn you on first run** ("Windows protected your PC")
> because the exe isn't code-signed. That's expected for small unsigned
> freeware like this, not a sign anything's wrong — click **More info → Run
> anyway**. (If you'd rather not trust an unsigned binary at all, build it
> yourself from source below — you can read every line of what you're
> compiling.)

## Requirements

- Windows 10 or 11, 64-bit.
- No runtime dependencies to install — the built exe is self-contained.

## Build

You need a C++ compiler. Pick one:

**MSVC (Visual Studio Build Tools)** — if you have Visual Studio 2022 installed but
without the C++ workload, add it via the Visual Studio Installer
("Desktop development with C++"), or install the standalone
[Build Tools for Visual Studio](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022).
Then run:

```bash
build.bat
```

**MinGW-w64** — e.g. [w64devkit](https://github.com/skeeto/w64devkit) (portable, just
unzip and add its `bin` to PATH) or MSYS2. Then run:

```bash
build_mingw.bat
```

Either produces `PerfOverlay.exe` in this folder.

## Run

```bash
PerfOverlay.exe
```

The HUD appears pinned in the top-right corner the first time you run it (after
that, it reopens wherever you last left it).

- **Drag** anywhere on it to reposition.
- **Right-click** the HUD, or the tray icon, for the action menu:
  - **Hide / Show** — left-click the tray icon to do this instantly without
    opening the menu.
  - **Horizontal Layout / Vertical Layout** — switches between stacked rows
    and a single line; the window is resized and kept fully on-screen.
  - **Add Background / Remove Background** — toggles the solid card look on
    or off.
  - **Lock Position / Unlock Position** — locking makes the HUD click-through
    (`WS_EX_TRANSPARENT`): mouse clicks pass straight to whatever's
    underneath. While locked you can no longer drag or right-click the HUD
    itself — use the tray icon to unlock it again.
  - **Enable/Disable Start with Windows** — adds or removes a per-user
    `HKCU\...\Run` entry pointing at this exe. No installer, no scheduled
    task.
  - **Settings...** — opens a small native settings window (plain Win32
    checkboxes, no dialog resource template) with every toggle above plus:
    - **Show history graphs** — a small sparkline per row showing the last
      ~40 seconds, vertical layout only (horizontal is too tight for it).
    - **Global hotkey Ctrl+Alt+O: Show/Hide** and **Ctrl+Alt+L: Lock/Unlock**
      — work from anywhere, no need to find the tray icon.
    - **Auto-hide when a game is fullscreen** — hides the HUD while any
      exclusive-fullscreen/borderless-fullscreen app has focus (detected by
      checking whether the foreground window's rect exactly covers a
      monitor), and brings it back once that app loses focus — unless you'd
      separately hidden it yourself, in which case it stays hidden.
  - **Exit**.
- It also adds an icon to the notification area (the "hidden icons" overflow
  in the taskbar) so you can find/control it even if the HUD itself is
  hidden or off-screen.
- It won't steal focus from whatever app/game is active (`WS_EX_NOACTIVATE`).

## Settings

Every toggle above, plus the window position, is written to
`HKCU\Software\PerfOverlay` on each change (and on exit) and reloaded the next
time the app starts — so however you leave it is how it opens next time.
Deleting that registry key resets everything to defaults.

## How it works

- **CPU** — PDH counter `\Processor Information(_Total)\% Processor Utility`, sampled
  once per second. This is the same counter Task Manager uses (not the older
  `% Processor Time`, which reads far lower/higher than Task Manager whenever the
  CPU is running below or boosting above its nominal clock).
- **RAM** — `GlobalMemoryStatusEx` (`dwMemoryLoad`, plus used/total in GB).
- **GPU** — DXGI (`IDXGIFactory1::EnumAdapters1`) enumerates every physical adapter
  once at startup for its name and LUID. PDH wildcard-expands
  `\GPU Engine(*)\Utilization Percentage` to find engine instances (these come and
  go as processes use the GPU — the instance list is rescanned every 5s, not every
  tick, since rebuilding the whole PDH query every second cost ~8% CPU). Each engine
  instance's LUID (embedded in its counter path) is matched back to a physical
  adapter; per adapter, the max utilization per engine type is taken, then the max
  across engine types is that GPU's overall load — the same rollup Task Manager's
  GPU graph uses.
- **Transparency** — the window is layered (`WS_EX_LAYERED`) with color-key
  transparency (`LWA_COLORKEY`): the whole card is painted in one reserved
  color first, which Windows then renders as a see-through hole, so only the
  text and divider pixels drawn on top stay visible. Text uses grayscale
  antialiasing rather than ClearType, since ClearType's subpixel blending
  assumes a known solid background and fringes once that "background" is
  actually an arbitrary desktop showing through.
- **Window sizing** — both layouts measure their actual text (via
  `GetTextExtentPoint32`) before the window is created or resized, so GPU
  names are never truncated and the card is always exactly as wide as it
  needs to be.

## Notes

- Counter names are matched in English; on a non-English Windows display language
  the GPU counter object name may need localizing (CPU uses `PdhAddEnglishCounterW`,
  which is locale-independent).

## License

[Unlicense](LICENSE) — public domain. Do anything you want with it, no
attribution required.
