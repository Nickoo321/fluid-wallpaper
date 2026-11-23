Fluid Wallpaper
===============

A live, slowly moving oil wallpaper. It draws behind your desktop icons and
changes its colours by itself: it rests on one pair of colours for a while,
then shifts slowly to another. It starts dark for about the first minute. It
pauses on its own while a game or another fullscreen or maximized window
covers the desktop.

Needs: Windows 10 or 11, 64-bit, and a graphics card with DirectX 12.

INSTALL
  Run FluidWallpaper-Setup-<version>.exe (or unzip the .zip and double-click
  Install.cmd). No admin rights are needed.
  Windows SmartScreen will say "Windows protected your PC" / "unknown
  publisher", because the program is not code-signed. Click "More info", then
  "Run anyway".
  It installs to %LOCALAPPDATA%\Programs\FluidWallpaper, keeps its settings in
  %APPDATA%\FluidWallpaper, asks whether it should start with Windows, and
  starts right away.

ON / OFF
  Double-click "Fluid Wallpaper on-off" on the desktop (also in the Start
  menu). Running -> it turns off; not running -> it turns on.
  Keyboard shortcut: {HOTKEY}
  You can also right-click the Fluid Wallpaper tray icon -> Exit.
  When it is off, your normal Windows wallpaper is back.

NEXT
  To move on to other colours sooner, double-click "Fluid Wallpaper next" on
  the desktop (also in the Start menu and in the tray menu; Ctrl+Alt+N when
  the on-off shortcut has a keyboard shortcut too). The shift to the next
  pair of colours then starts right away. It is a slow change on purpose: the
  first new colour shows after about half a minute, and it takes a few
  minutes to finish. A press while a change is already under way does nothing.

SETTINGS
  Click / right-click the Fluid Wallpaper icon in the taskbar tray (maybe
  under the ^ arrow) -> Settings. Presets are in the same tray menu.
  The window opens on Home: on the coloured side what ends by itself (Next,
  Keep this for 30 minutes, Pause, a 30-minute visit to Ink or Fluid); on the
  black side what is kept (the colour pairs and how often each comes up,
  Never show this, Add a pair, and a list of what you changed, each with an X
  to undo it). Every older setting is under Advanced.
  "Start with Windows" can be switched off in Settings -> Display and system.

HDR
  Optional. It looks right with Windows HDR on or off.

UPDATE
  It updates itself. At most once a day (when you sign in to Windows, or turn
  the wallpaper on with the on-off shortcut) it looks at the project's GitHub
  page; when a newer version is there it downloads it, checks it, and installs
  it without asking. Your settings are kept, and a backup of settings.ini is
  saved first as settings-backup-<date>.ini in %APPDATA%\FluidWallpaper. It
  waits while a game or film fills the screen.
  To turn that off: create an empty file named no-auto-update.txt in
  %APPDATA%\FluidWallpaper. What it did is in update.log in the same folder.
  By hand: run the newer Setup over the old one. It asks whether to keep your
  settings.

UNINSTALL
  Start menu -> Fluid Wallpaper -> "Uninstall Fluid Wallpaper", or Windows
  Settings -> Apps -> Installed apps -> Fluid Wallpaper -> Uninstall.
  It asks whether to delete the settings too.

PRIVACY
  The wallpaper itself never uses the network. The update check (see UPDATE)
  downloads a short list, and a new version when there is one, from the
  project's GitHub page; it sends nothing about you or your PC. The log files
  (FluidWallpaper.log, update.log in %APPDATA%\FluidWallpaper) never leave
  your PC.

LICENCE
  MIT. See LICENSE.txt and THIRD-PARTY-NOTICES.txt in the program folder.
