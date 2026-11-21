# Fluid Wallpaper on/off switch (installed copy; started hidden by toggle.vbs).
# Running     -> ask it to exit (tray Exit), force after 5 s.
# Not running -> start FluidWallpaper.exe from this folder.
# This runs with no console window, so a failure is shown as a message box.
$ErrorActionPreference = 'Stop'
try {
    . (Join-Path $PSScriptRoot 'fw-common.ps1')
    if ((Get-FwProcesses).Count -gt 0) {
        if (-not (Stop-FluidWallpaper 5)) { throw 'FluidWallpaper.exe did not stop. Close it from Task Manager.' }
    } else {
        $exe = Join-Path $PSScriptRoot 'FluidWallpaper.exe'
        if (-not (Test-Path -LiteralPath $exe)) { throw "$exe is missing. Run the Fluid Wallpaper setup again." }
        Start-Process -FilePath $exe -WorkingDirectory $PSScriptRoot
        # the silent self-update: update.ps1 decides (once a day at most); it must never be in the way of the start
        $upd = Join-Path $PSScriptRoot 'update.vbs'
        if (Test-Path -LiteralPath $upd) {
            try { Start-Process -FilePath (Join-Path $env:SystemRoot 'System32\wscript.exe') -ArgumentList ('"' + $upd + '" -DelaySec 60') } catch { }
        }
    }
} catch {
    try {
        Add-Type -AssemblyName System.Windows.Forms
        [void][System.Windows.Forms.MessageBox]::Show("Fluid Wallpaper on/off:`r`n$_", 'Fluid Wallpaper', 'OK', 'Error')
    } catch { }
    exit 1
}
