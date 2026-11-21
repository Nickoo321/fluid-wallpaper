# Fluid Wallpaper - silent self-update (installed copy; started hidden by update.vbs:
# at sign-in, and each time the on-off switch turns the wallpaper on).
#
# At most once a day it reads the list of published files from the project's GitHub
# page. When the package there is newer than this copy it downloads it, checks its
# SHA-256 against the published list, and runs the package's own install.ps1
# (-Quiet -KeepSettings): the settings stay, and the wallpaper comes back on only if
# it was running. No window, no question. A fullscreen window in front (a game, a
# film) postpones it to the next start. Every failure is silent and only logged.
#
#   Turn it off:  create an empty file "no-auto-update.txt" in %APPDATA%\FluidWallpaper
#   Log:          %APPDATA%\FluidWallpaper\update.log
#   By hand:      powershell -ExecutionPolicy Bypass -File update.ps1 -Force
#
# What it sends: two plain downloads from raw.githubusercontent.com. Nothing about
# this PC or its user is sent.
#
# Test switches (so nothing real is touched): -InstallDir -DataDir -ShortcutDir
# -NoRegistry -NoLaunch -NoStop are handed on to install.ps1; -BaseUrl <url or local
# folder> is where SHA256.txt and the zip are read from; -DryRun stops after the
# version check; -NoFullscreenCheck installs even with a fullscreen window in front. Windows PowerShell 5.1 compatible. Keep this file pure ASCII.
[CmdletBinding()]
param(
    [switch]$Force,
    [int]$DelaySec = 0,
    [string]$BaseUrl = '',
    [switch]$DryRun,
    [switch]$NoFullscreenCheck,
    [string]$InstallDir = '',
    [string]$DataDir = '',
    [string]$ShortcutDir = '',
    [switch]$NoRegistry,
    [switch]$NoLaunch,
    [switch]$NoStop
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

if (-not $BaseUrl)    { $BaseUrl = 'https://raw.githubusercontent.com/Nickoo321/fluid-wallpaper/main/release' }
if (-not $InstallDir) { $InstallDir = $PSScriptRoot }
if (-not $DataDir)    { $DataDir = Join-Path ([Environment]::GetFolderPath('ApplicationData')) 'FluidWallpaper' }
$logPath   = Join-Path $DataDir 'update.log'
$stampPath = Join-Path $DataDir 'update-last-check.txt'

function Log([string]$t) {
    try {
        New-Item -ItemType Directory -Force -Path $DataDir | Out-Null
        # keep the log small: start over past 200 KB
        if ((Test-Path -LiteralPath $logPath) -and (Get-Item -LiteralPath $logPath).Length -gt 200KB) { Remove-Item -LiteralPath $logPath -Force }
        Add-Content -LiteralPath $logPath -Value ("{0}  {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $t) -Encoding ASCII
    } catch { }
    Write-Host $t
}

# "2026.11.19" -> a comparable [version]; anything after the dotted number is ignored
function ConvertTo-FwVersion([string]$t) {
    $m = [regex]::Match($t, '\d+(\.\d+){0,3}')
    if (-not $m.Success) { return $null }
    $parts = $m.Value.Split('.')
    while ($parts.Count -lt 2) { $parts += '0' }
    try { return [version]($parts -join '.') } catch { return $null }
}

# A local folder works as -BaseUrl too (tests, or a copy on a stick): then the files are copied.
function Get-FwFile([string]$name, [string]$to) {
    if ($BaseUrl -match '^https?://') {
        Invoke-WebRequest -Uri ($BaseUrl.TrimEnd('/') + '/' + $name) -OutFile $to -UseBasicParsing -TimeoutSec 120
    } else {
        Copy-Item -LiteralPath (Join-Path $BaseUrl $name) -Destination $to -Force
    }
}

# Is a window covering the whole primary screen in front (a game, a film)? Then not now.
function Test-FwFullscreenInFront {
    try {
        if (-not ('FluidWp.Upd' -as [type])) {
            Add-Type -Namespace 'FluidWp' -Name 'Upd' -MemberDefinition @'
[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
public struct RECT { public int L, T, R, B; }
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern System.IntPtr GetForegroundWindow();
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool GetWindowRect(System.IntPtr h, out RECT r);
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern int GetClassNameW(System.IntPtr h, System.Text.StringBuilder s, int n);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
'@
        }
        $h = [FluidWp.Upd]::GetForegroundWindow()
        if ($h -eq [IntPtr]::Zero) { return $false }
        $sb = New-Object System.Text.StringBuilder 128
        [void][FluidWp.Upd]::GetClassNameW($h, $sb, 128)
        if (@('Progman', 'WorkerW', 'Shell_TrayWnd', 'FluidWallpaperWnd', 'FluidWallpaperTray') -contains $sb.ToString()) { return $false }
        $r = New-Object FluidWp.Upd+RECT
        if (-not [FluidWp.Upd]::GetWindowRect($h, [ref]$r)) { return $false }
        $w = [FluidWp.Upd]::GetSystemMetrics(0); $ht = [FluidWp.Upd]::GetSystemMetrics(1)
        return ($r.L -le 0 -and $r.T -le 0 -and $r.R -ge $w -and $r.B -ge $ht)
    } catch { return $false }
}

$mutex = $null
$work = ''
try {
    if (Test-Path -LiteralPath (Join-Path $DataDir 'no-auto-update.txt')) { exit 0 }      # switched off by the user
    if (-not $Force -and (Test-Path -LiteralPath $stampPath)) {
        $last = [datetime]::MinValue
        if ([datetime]::TryParse((Get-Content -LiteralPath $stampPath -TotalCount 1), [ref]$last) -and ((Get-Date) - $last).TotalHours -lt 20) { exit 0 }
    }
    $created = $false
    $mutex = New-Object System.Threading.Mutex($true, 'FluidWallpaperUpdate', [ref]$created)
    if (-not $created) { $mutex = $null; exit 0 }                                         # another check is running
    if ($DelaySec -gt 0) { Start-Sleep -Seconds $DelaySec }

    $verFile = Join-Path $InstallDir 'version.txt'
    if (-not (Test-Path -LiteralPath $verFile)) { Log "no version.txt in $InstallDir; nothing to update"; exit 0 }
    $mineText = (Get-Content -LiteralPath $verFile -TotalCount 1).Trim()
    $mine = ConvertTo-FwVersion $mineText
    if ($null -eq $mine) { Log "cannot read this copy's version ('$mineText')"; exit 0 }

    try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12 } catch { }
    $work = Join-Path $env:TEMP ('FluidWallpaperUpdate-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $sumsPath = Join-Path $work 'SHA256.txt'
    Get-FwFile 'SHA256.txt' $sumsPath
    $zipName = ''; $zipHash = ''; $theirs = $null
    foreach ($l in (Get-Content -LiteralPath $sumsPath)) {
        $m = [regex]::Match($l.Trim(), '^([0-9A-Fa-f]{64})\s+\*?(FluidWallpaper-(\d[\d.]*\d)\.zip)$')
        if (-not $m.Success) { continue }
        $v = ConvertTo-FwVersion $m.Groups[3].Value
        if ($null -ne $v -and ($null -eq $theirs -or $v -gt $theirs)) { $theirs = $v; $zipName = $m.Groups[2].Value; $zipHash = $m.Groups[1].Value }
    }
    if ($null -eq $theirs) { Log 'the published list names no package; nothing done'; exit 0 }
    # the check itself worked: not again for a day (also when there is nothing newer)
    Set-Content -LiteralPath $stampPath -Value (Get-Date -Format 's') -Encoding ASCII
    if ($theirs -le $mine) { Log "up to date ($mineText; published $theirs)"; exit 0 }
    Log "newer version published: $theirs (this copy: $mineText)"
    if ($DryRun) { Log 'dry run: stopping before the download'; exit 0 }
    if (-not $NoFullscreenCheck -and (Test-FwFullscreenInFront)) {
        Remove-Item -LiteralPath $stampPath -Force -ErrorAction SilentlyContinue           # try again at the next start
        Log 'a fullscreen window is in front: the update waits for the next start'; exit 0
    }

    $zipPath = Join-Path $work $zipName
    Get-FwFile $zipName $zipPath
    $got = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash
    if ($got -ne $zipHash.ToUpperInvariant()) { Log "the download does not match its published SHA-256 ($got); nothing installed"; exit 0 }
    $x = Join-Path $work 'x'
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($zipPath, $x)
    $inst = Join-Path $x 'install.ps1'
    if (-not (Test-Path -LiteralPath $inst) -or -not (Test-Path -LiteralPath (Join-Path $x 'program\FluidWallpaper.exe'))) {
        Log 'the package is incomplete; nothing installed'; exit 0
    }
    $wasRunning = @(Get-Process -Name FluidWallpaper -ErrorAction SilentlyContinue).Count -gt 0
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $inst + '"'), '-Quiet', '-KeepSettings')
    $relaunch = $wasRunning -and -not $NoLaunch                                             # an update never turns it ON
    if (-not $relaunch) { $a += '-NoLaunch' }
    if ($NoStop)      { $a += '-NoStop' }
    if ($NoRegistry)  { $a += '-NoRegistry' }
    if ($PSBoundParameters.ContainsKey('InstallDir')) { $a += @('-InstallDir', ('"' + $InstallDir + '"')) }
    if ($PSBoundParameters.ContainsKey('DataDir'))    { $a += @('-DataDir', ('"' + $DataDir + '"')) }
    if ($ShortcutDir) { $a += @('-ShortcutDir', ('"' + $ShortcutDir + '"')) }
    $psExe = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $p = Start-Process -FilePath $psExe -ArgumentList $a -WindowStyle Hidden -PassThru -Wait
    if ($p.ExitCode -eq 0) { Log "updated to $theirs (settings kept; wallpaper $(if ($relaunch) { 'was running: restarted' } else { 'not started' }))" }
    else { Log "the installer of $theirs ended with code $($p.ExitCode); see above. This copy may need the Setup run by hand." }
} catch {
    Log "update check failed: $($_.Exception.Message)"
} finally {
    if ($work -and (Test-Path -LiteralPath $work)) { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue }
    if ($mutex) { try { $mutex.ReleaseMutex() } catch { }; $mutex.Dispose() }
}
exit 0
