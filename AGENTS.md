# AGENTS.md: working on Fluid Wallpaper

For AI coding agents, and for people, who want to change this code. [README.md](README.md) says what the program is
and how to install it. This file says how the code is put together and how to change it without breaking the
picture. [HOW-TO-PUSH.md](HOW-TO-PUSH.md) says how to send a change back.

## What this is

A live wallpaper for Windows 10 and 11: one exe, C++20, Direct3D 12, nothing to install beside it. It draws into a
window that sits behind the desktop icons, has a tray icon, and pauses while a fullscreen program covers the desktop.
The only third-party code is Dear ImGui, compiled in, used by the Settings window alone.

What a fresh install shows is the **oil look** (called `liquid_acid` in the code): black oil, as big masses and
small droplets, floating on a lit coloured film. About every ten minutes the **look cycle** moves to another colour
scheme, and the **colour director** moves slowly from one pair of colours to the next.
The program also holds two older looks, plain fluid and ink in water, that the default install does not show.

## Build, run, check

**Build.** `build.bat` writes `build\FluidWallpaper.exe`. It needs Visual Studio 2022 Build Tools with the C++
workload (CMake and Ninja come with it) and the Windows SDK.

The shaders are HLSL text inside the exe and are compiled when the program **starts**, not when it is built. A
shader mistake therefore builds without an error and shows up at the first run: the program prints
`shader '<name>' compile error:` with the compiler's message and exits. After any shader edit, do a check run.

**Run.** Only one copy runs at a time. Turn an installed wallpaper off first (its on-off shortcut, or the tray icon
-> Exit), then start `build\FluidWallpaper.exe`. Starting it a second time opens the Settings window of the copy
that is running. Your build reads the same settings as an installed copy: `%APPDATA%\FluidWallpaper\settings.ini`.
The log is `FluidWallpaper.log` in that folder; when the program exits by itself, the last `exit:` line says why.

**Check without a window.** `--shot` renders frames to PNG files and exits. It opens no window and no tray icon,
writes no settings, and does not disturb a wallpaper that is running:

```powershell
$a = "--shot `"$PWD\out.png`" --ini `"$PWD\tools\installer\data\settings.ini`" --hdr off --shot-size 1280x720 --shot-delay 60 --seed 1 --cycle-seed 1"
Start-Process "$PWD\build\FluidWallpaper.exe" -ArgumentList $a -Wait -NoNewWindow -RedirectStandardOutput "$PWD\out.log"
Get-FileHash -Algorithm MD5 out.png
```

The exe is a window program: called plainly from a shell it returns at once and prints nothing. Start it with
`Start-Process -Wait` and redirect its output, as above, and give full paths. This run takes about two minutes.

- `--ini FILE` the settings to use. `tools\installer\data\settings.ini` is what a fresh install starts with.
- `--shot-delay N` seconds of simulation before the frame. The oil look starts black and needs about a minute.
  Without it the delay is 40 seconds; without `--shot-size` the frame is 2560x1440.
- `--shot-series N:S` N frames, S seconds apart, named `out-<seconds>.png`.
- `--shot-size WxH`, `--hdr on|off`, `--seed N` (the simulation), `--cycle-seed N` (the look cycle).
- Next to `out.png` it writes `out-hdr.png`, `out-pq.png` and `out.jxr`, HDR versions of the same frame.
  `--shot-png-only` leaves those out. Compare and look at `out.png`.
- `--stats` and `--timings` add measurements to the output. The program prints its log to standard output:
  redirect it to a file and read it.
- `--cycle-stage N` starts the cycle on stage N; `--cycle-next-at T` presses "next" at T seconds.
- `--ui-shot FILE.png` renders the Settings window instead (`--ui-pane cycle`, `--ui-size WxH`).

Run one render at a time. A render uses the whole graphics card; two at once can freeze the desktop for seconds.

**The same input gives the same file.** With the same exe and the same arguments on the same PC, `--shot` writes the
same PNG byte for byte (the command above, run twice with the published exe, gave the same MD5 both times). That is
the main test tool here, see "The one rule" below.

## The one rule: off means unchanged

A new feature is **off by default**, and with it off the program draws exactly the picture it drew before.

Check it like this. Before you change anything, render a frame with the command above and note its MD5
(`Get-FileHash -Algorithm MD5 out.png`). After your change, with the feature off, render with the same arguments:
the MD5 must be the same. If it differs, your change leaks into the default picture; find out where before going on.
Then turn the feature on in a copy of the ini and look at what it does.

## Where things are

The three biggest files are far too large to read whole. Search them for a key name or a function name.

| File | Size | What is in it |
| --- | --- | --- |
| `src/main.cpp` | 300 KB | The app: wallpaper window, tray menu, command line, HDR detection, `LoadConfigFromIni` (reads every setting), `WriteConfigKeys` (writes them), `RunShotMode`. |
| `src/fluid.h` | 240 KB | Every setting with its default and a comment that explains it: `LiquidAcidConfig`, `InkConfig`, `DropConfig`, `PostConfig`, `MirrorConfig`, `FluidConfig`. Also the `FluidRenderer` class. |
| `src/fluid.cpp` | 540 KB | The renderer: the fluid simulation's passes, the oil look's own state (masses, droplets, motes), the constant upload `UploadAcidConstants`, `RenderDisplay`, the hue shift. |
| `src/shaders.h` | 420 KB | All HLSL as string literals: `kComputeSrc` (the simulation), `kDisplaySrc` (the looks), `kPostSrc` (the last pass: tone, grain, bloom, output), `kGradientSrc`, `kPhotoSrc`. |
| `src/acid_slots.h` | 20 KB | The table of the oil look's packed shader constants. Read its header before adding one. |
| `src/director.cpp` | 95 KB | The colour director: which pair of colours is shown, and the slow move between pairs. |
| `src/cycle.cpp`, `cycle.h`, `animators.h` | 130 KB | The look cycle: the stage list, the draw of the next stage, the fade through black. |
| `src/clocks.cpp` | 55 KB | Slow random drift of chosen settings around their preset value (`[clocks]`). |
| `src/photo.cpp`, `journey.cpp`, `analyzer.cpp` | | A photo stage for the cycle, an older colour journey, the HDR analyzer window. |
| `src/ui/` | | The Settings window: `keys.inc` is one row per setting; `ui_model` (the table and the one write path), `ui_window` (drawing), `ui_cycle` (the cycle pane's adapter), `ui_presets` (the preset list). |
| `src/third_party/imgui/` | | Dear ImGui, unchanged. Do not edit. |
| `src_oil/`, `src_acid/`, `src_ink3d/` | | Three small stand-alone experiments with their own exes. Not part of the wallpaper. |
| `tools/installer/` | | The installer and updater scripts, and `data/`: the settings, looks and presets a fresh install starts with. |
| `release/` | | The published installer. Never change it. |

**About the comments.** The source comments name briefs, notes and tools such as `reference/briefs/...`, `docs/...`,
`WORKLOG`, `tools\slot-check.ps1` and `tools\keymeta-check.ps1`, and they quote "the user". Those are the author's
private working notes and helper scripts and are not in this repository; "the user" is the author. Do not look for
those files. The comment text itself is the documentation, and it is usually detailed.

## How a frame is made

1. **Simulation.** GPU passes from `kComputeSrc` move a velocity field and a dye field (`[sim] sim_res` and
   `dye_res` set their sizes). All looks share it.
2. **The oil look's own state.** `fluid.cpp` steps the masses, droplets and motes on the CPU every frame and uploads
   them with the look's settings as shader constants.
3. **Display.** `PSMain` in `kDisplaySrc` draws the look. The same source is compiled several times with different
   `#define`s: `LIQUID_ACID` for the oil look, `INK` for ink in water, neither for plain fluid, and `OIL_*` and
   similar defines for parts of the oil look that are switched by a setting. `MirrorFold` at the top folds the
   picture when `[mirror]` is on.
4. **Post.** `kPostSrc` does the last pass and the output for HDR or SDR.

## Settings

- A setting is a field of `FluidConfig` or one of its parts (`cfg.acid`, `cfg.ink`, `cfg.drops`, `cfg.post`,
  `cfg.mirror`) in `src/fluid.h`. The ini sections are `[sim]`, `[color]`, `[behavior]`, `[hdr]`, `[general]`,
  `[look]`, `[liquid_acid]`, `[ink]`, `[post]`, `[drops]`, `[mirror]`, `[clocks]`, `[ui]`, `[cycle]` and `[looks]`.
- `settings.ini` is read once at start. The Settings window writes it while the program runs; a file edited by hand
  is read at the next start.
- A **preset** is a partial ini: it changes only the keys it names. A preset must never carry `sim_res` or `dye_res`.
- The **cycle** builds each stage in memory and never writes it to `settings.ini`: the code defaults, then the
  stage's base file (`stage_N_base`), then the layout file (`[cycle] oil_layout`), then the stage's own file
  (`stage_N_file`). The later file wins.
- In `tools/installer/data`: `looks\configs\monotone-post-0924.ini` is the base, `looks\presets\Layout - Few giants
  + Oil3D.ini` is the layout (it holds nearly every decision about how the oil look looks), and the five
  `Scheme - *.ini` files are the colour schemes the cycle walks. An installed copy has the same files in
  `%APPDATA%\FluidWallpaper`.
- The colour director is switched by `[liquid_acid] colour_director` and tuned by the `director_*` keys. The ratings
  of its colour pairs are in `[looks]` (the Settings window, Cycle pane).

## Recipe: add a setting

Follow an existing one. `droplet_ring_clump` is a good model: search for it and you find all five places.

1. **`src/fluid.h`**: a field with its default and a comment, in the struct of its section. The default is the
   value that leaves the picture as it is.
   `float dropletRingClump = 0.0f;  // raft attraction 0..1       droplet_ring_clump`
2. **`src/main.cpp`, `LoadConfigFromIni`**: read it.
   `a.dropletRingClump = getF(S, L"droplet_ring_clump", a.dropletRingClump);`
3. **`src/main.cpp`, `WriteConfigKeys`**: write it.
   `putF(S, L"droplet_ring_clump", a.dropletRingClump, 3);`
4. **`src/ui/keys.inc`**: one `KEY_SLIDER` or `KEY_CHECK` row, so it shows in the Settings window. The header of
   that file explains every column. Give the slider the range that is useful, not the range that is possible.
5. **Use it.** On the CPU side that is ordinary code in `src/fluid.cpp`. In a shader, see the next recipe.

## Recipe: get a value into the oil look's shader

The oil look reads its numbers from one packed constant buffer. `src/acid_slots.h` is the table: one row per number,
with the `float4` and the component it lives in. Its header comment is the full manual. In short:

1. Add a row to the table for `LA_<NAME>`.
2. Write it in `UploadAcidConstants` (`src/fluid.cpp`) with `slot(LA_<NAME>, value)`.
3. Read `LA_<NAME>` in `kDisplaySrc` (`src/shaders.h`).

Two things will bite you:

- **The table is full** (`laP0` to `laP45`, 184 numbers, no free component). A new number needs a new `float4
  laP46`: raise `kAcidSlotVecs`, and add the member to **both** `struct AcidParamsGPU` (`fluid.cpp`) and `cbuffer
  AcidCB` (`shaders.h`) at the same position. If the two orders differ, every number after it reads the wrong value.
- **A string literal has a size limit.** The compiler accepts at most 16,380 bytes per piece of a string literal, so
  the HLSL sources are cut into pieces (`)hlsl"` ends one, `R"hlsl(` starts the next). The biggest pieces are near
  15,500 bytes. If your added shader text pushes a piece over the limit the build fails: cut that piece in two
  between two functions.

Keep new shader code behind a switch that costs nothing when it is off, so "off means unchanged" holds: a `#define`
passed only when the setting is on (the way the `OIL_*` parts are built), or a branch on a constant that is zero.

## Recipe: add a look without code

A look is an ini file. Copy one of the `Scheme - *.ini` files in `tools/installer/data/looks/presets`, change the
keys, and add it to the cycle in `settings.ini`: raise `[cycle] stage_count` and add the `stage_N_file`,
`stage_N_base`, `stage_N_tier` (`proven`, `moderate` or `wild`: drawn about 70, 20 and 10 percent of the time),
`stage_N_weight` and `stage_N_dwell` lines, following the five that are there. Every key and its meaning is in
`src/fluid.h`.

To try it on your own PC, do the same in `%APPDATA%\FluidWallpaper` and restart the wallpaper.

## Rules that keep the picture right

These come from many rounds of looking at the result. A change that breaks one will be sent back.

- **Off means unchanged**, as above.
- **HDR on and off.** Everything has to look right with Windows HDR on and with it off. Render both. The two
  differ in colour on purpose, so compare on with on and off with off.
- **Black stays black.** The oil is black on an OLED screen. Nothing may lift it to grey: no haze, no pale tint, no
  glow over the whole picture. Bright things stay small.
- **No full-screen bloom.** The only bloom is the keyed one in the post pass, and its default is 0.
- **Slow.** Nothing snaps. Colours and shapes change over tens of seconds, and a change to a different kind of look
  fades through black. Never set the hue angle to a value in one step: glide to it.
- **Hue.** The hue rotation is a step after the look is drawn (`CssHueRotate`). While a hue shift is running the
  emitters counter-rotate, so what is already on screen keeps its colour. Keep that when you touch colour code.
- **Any frame rate.** The wallpaper runs at whatever rate the screen and `[general] fps_limit` give. Anything that
  decays or moves per step must be scaled by the time step, or the look changes with the frame rate.
- **`sim_res` and `dye_res`** stay what the settings say. Looks and presets never set them.
- **`[mirror]`** has its own constants that every display draw must bind, also a new one you add.
- **No new dependencies.** One exe, static runtime, system DLLs only.
- **The wallpaper itself never uses the network**, and the log never records window titles. The update check is a
  separate script in `tools/installer`.
- **It must survive the desktop.** It pauses for fullscreen programs and recovers from a lost graphics device. A
  feature must not keep the graphics card busy while paused.

## Before you send it

1. It builds with `build.bat`, with no new warnings from your code.
2. A check run starts and exits without a shader error.
3. Feature off: the MD5 of a `--shot` frame is the same as before your change, with `--hdr off` and with `--hdr on`.
4. Feature on: you looked at frames with `--hdr off` and `--hdr on`, early (60 s) and late (300 s or more).
5. The setting is in all the places of the recipe, and its row shows in the Settings window.
6. The real wallpaper ran with it for a few minutes: start it, open Settings, move the new control.
7. `git diff --stat` shows only what you meant to change. The source files use CRLF line endings and UTF-8; an
   editor that re-saves a file with other line endings turns the whole file into a change.

Then follow [HOW-TO-PUSH.md](HOW-TO-PUSH.md).

## Leave alone

- `release/` and its `SHA256.txt`: the published installer and the list every installed copy checks for updates.
- `tools/installer/update.ps1`, `update.vbs`, `install.ps1`, `uninstall.ps1`, unless the installer is what you are
  changing. A mistake there reaches every installed copy.
- `src/third_party/imgui/`.
- `LICENSE` and `THIRD-PARTY-NOTICES.md`.
