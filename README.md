# Fluid Wallpaper

A live wallpaper for Windows: slow oil, ink and fluid simulations drawn behind your desktop icons with Direct3D 12.
It changes its colours and its look by itself over minutes, pauses while a game or a fullscreen window covers the
desktop, and works with Windows HDR on or off.

One exe, no runtime to install, no network access.

## Install

**[Download FluidWallpaper-Setup-2026.11.18.exe](https://github.com/Nickoo321/fluid-wallpaper/raw/main/release/FluidWallpaper-Setup-2026.11.18.exe)**
(1.7 MB) and run it. No admin rights are needed. The same package as a zip and the SHA-256 sums are in
[`release/`](release).

- Windows 10 or 11, 64-bit, and a graphics card with DirectX 12.
- It starts dark for about the first minute while the oil builds up.
- The Setup is not code-signed, so SmartScreen says "Windows protected your PC": click **More info**, then
  **Run anyway**. (Or download the `.zip`, extract all of it and double-click `Install.cmd`.)
- It installs for the current user only, to `%LOCALAPPDATA%\Programs\FluidWallpaper`. Settings live in
  `%APPDATA%\FluidWallpaper`. Setup asks whether it should start with Windows.

**On / off:** the "Fluid Wallpaper on-off" shortcut on the desktop, or the tray icon -> Exit.
**Next look:** the "Fluid Wallpaper next" shortcut, or the tray menu.
**Settings:** the tray icon -> Settings.
**Uninstall:** Windows Settings -> Apps -> Fluid Wallpaper, or the Start menu entry. It asks whether to delete your
settings too.

## Privacy

The program never uses the network. Its log file (`%APPDATA%\FluidWallpaper\FluidWallpaper.log`) stays on your PC.
When the wallpaper pauses for a fullscreen program, the log names that program's exe (never its window title).

## Build from source

Needs Visual Studio 2022 Build Tools (the C++ workload, which brings CMake and Ninja) and the Windows SDK.

```bat
build.bat
```

That builds `build\FluidWallpaper.exe` (static CRT, system DLLs only). The same CMake project also builds three
small stand-alone proofs of concept: `OilWallpaper`, `AcidWallpaper` and `Ink3DWallpaper`.

Useful switches: `--console` (a diagnostics console), `--stats`, `--hdr on|off`, and `--shot file.png` (render one
frame without a window and exit; `--shot-delay N` seconds of simulation first).

Configuration is `%APPDATA%\FluidWallpaper\settings.ini`. The Settings window writes it live; a hand-edited file is
read at the next start. The look files the default cycle uses are in `reference\configs` and `reference\presets`.

### Build the installer

```powershell
tools\installer\build-installer.ps1 -Exe build\FluidWallpaper.exe -Data tools\installer\data -Version 1.0.0
```

It writes `dist\FluidWallpaper-<version>.zip` and `dist\FluidWallpaper-Setup-<version>.exe`. `tools\installer\data`
is the default settings, looks and presets a fresh install starts with. See `tools\installer\README.md`.

## Layout

- `src/` the wallpaper: `main.cpp` (app shell, tray, HDR detection, ini), `fluid.cpp` + `shaders.h` (simulation and
  rendering), `director.cpp` (the colour director), `cycle.cpp` (the look cycle), `ui/` (the Settings window).
- `src/third_party/imgui/` Dear ImGui, unchanged.
- `src_oil/`, `src_acid/`, `src_ink3d/` the proofs of concept.
- `reference/` look files (ini).
- `tools/installer/` the installer and its default data.

## Licence and credits

MIT, see [LICENSE](LICENSE).

The fluid simulation started as a C++ / Direct3D 12 port of
[WebGL Fluid Simulation](https://github.com/PavelDoGreat/WebGL-Fluid-Simulation) by Pavel Dobryakov (MIT).
The Settings window uses [Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut (MIT).
Full notices: [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

