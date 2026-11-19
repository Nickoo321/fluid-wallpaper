# Installer (tools/installer)

Builds a per-user installer (no admin rights): `dist\FluidWallpaper-<version>.zip` and
`dist\FluidWallpaper-Setup-<version>.exe`. The Setup exe is an IExpress package that carries the zip and
`Install.cmd`; `Install.cmd` unpacks it to `%TEMP%` and runs `install.ps1`.

## Build it

From the repo alone (the default data in `tools\installer\data`):

```powershell
tools\installer\build-installer.ps1 -Exe build\FluidWallpaper.exe -Data tools\installer\data -Version 1.0.0
```

From what runs on your own PC (your `%APPDATA%\FluidWallpaper\settings.ini`, every look file its `[cycle]` names,
and your tray presets):

```powershell
tools\installer\build-installer.ps1 -Exe build\FluidWallpaper.exe -Public -Version 1.0.0
```

`-Public` drops every comment line from the shipped ini files, carries no hotkey over and adds the licence files.
Without it the package is a personal one (comments kept unless they name this PC, the hotkey of your own on/off
shortcut carried over). To refresh `tools\installer\data`, copy `dist\stage\data` of a `-Public` build over it.

Other options: `-Commit <hash>` (the label in version.txt), `-OutDir <dir>` (default `dist`), `-Settings <ini>`,
`-PresetsDir <dir>`, `-Hotkey none|<key>`, `-KeepLookPeak`, `-LicenseDir <dir>` (where LICENSE and
THIRD-PARTY-NOTICES.md are; default the repo root), `-NoSetupExe`, `-NoSourceCheck`.

## What the build does

* `dist\stage\` = `Install.cmd`, `install.ps1`, `README.txt`, `LICENSE.txt`, `program\` (the exe, toggle.vbs/.ps1,
  fw-common.ps1, uninstall.ps1, README.txt, licence files, version.txt), `data\` (settings.ini, presets\,
  looks\configs\, looks\presets\).
* settings.ini: comments dropped, every `[cycle]` path rewritten to `looks\...` (the app resolves a relative
  `stage_N_file/base` and `oil_layout` against the folder of settings.ini), `[hdr] peak_nits=-1` (auto),
  `[general] mirror_second=0`, `[cycle] current` removed, `[ui] active_preset/overlays/window` removed, any other
  absolute path dropped; with `-Public` also `[general] fps_limit` (the app default applies).
* Look files and presets: `[hdr] peak_nits` removed (a stage file's peak would override the user's own while that
  stage runs); presets also lose `sim_res`, `dye_res`, `fps_limit`, `mirror_second`. The encoding of each file is kept.
* Checks that fail the build: every stage file / base / oil_layout / `[meta] base` resolves to a staged file; no
  shipped file (text, exe or the Setup exe) holds the Windows user name of whoever builds, a profile path, or a
  word listed in the optional `tools\installer\private-words.txt` (one regex per line; keep that file out of any
  public repo); with `-Public` no comment line is left in an ini; the staged settings.ini has `peak_nits=-1` and no
  absolute path.

## What Setup does on the user's PC

* Stops a running copy, copies `program\` to `%LOCALAPPDATA%\Programs\FluidWallpaper` and `data\` to
  `%APPDATA%\FluidWallpaper`. On an update the existing settings.ini is first saved as `settings-backup-<date>.ini`,
  then Setup asks whether to keep it (`-KeepSettings`: kept; `-Quiet` alone: replaced); presets and look files with
  the same name are replaced, the user's others stay.
* Shortcuts: on the desktop on-off and next; in the Start menu the app itself (starts it, or opens its Settings when
  it runs), on-off, next and uninstall (its own icon). A shortcut Windows refuses to create (a protected Desktop) is
  reported at the end and does not stop the install.
* Asks whether to start with Windows (HKCU `Run`); an update keeps the user's earlier choice. Adds the entry under
  Windows Settings -> Apps.
* Starts the wallpaper.

Silent: `FluidWallpaper-Setup-<version>.exe /Q` (starts with Windows; `Install.cmd -Quiet -NoAutostart` from the
zip for a silent install that does not).

## Testing without touching your own install

```powershell
dist\stage\install.ps1 -InstallDir <scratch>\prog -DataDir <scratch>\data -ShortcutDir <scratch>\links -NoRegistry -NoLaunch -NoStop -Quiet
<scratch>\prog\uninstall.ps1 -InstallDir <scratch>\prog -DataDir <scratch>\data -ShortcutDir <scratch>\links -NoRegistry -NoStop -Quiet -RemoveData
FluidWallpaper-Setup-<version>.exe /Q /C /T:<scratch>\x      # extract only, runs nothing
```

Without those switches install.ps1 / uninstall.ps1 / toggle stop the running FluidWallpaper.exe and write the real
`%APPDATA%`, Desktop, Start Menu and HKCU.
