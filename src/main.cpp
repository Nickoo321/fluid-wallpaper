// FluidWallpaper — app shell: wallpaper window (WorkerW), tray icon,
// fullscreen pause, live HDR detection. Rendering lives in fluid.cpp.
//
// GUI app. Flags:
//   --console          open a diagnostics console
//   --gradient         render the M1 HDR test gradient instead of the fluid
//   --stats            print dye-field readback stats every 5 s
//   --simres N         override simulation resolution (default 256)
//   --dyeres N         override dye resolution (default 1024)
//   --force-render     ignore fullscreen pause (automated testing)
//   --test-suspend     force one renderer suspend ~3 s in, resume ~8 s (test)
//
// Headless capture (never shows anything, never touches the live config):
//   --shot <out.png>   render OFFSCREEN and write <out>.png + <out>-hdr.png
//                      (8-bit views) plus <out>.jxr (lossless JPEG XR, 64bpp
//                      RGBA half, linear scRGB -- real HDR in Photos) and
//                      <out>-pq.png (16-bit Rec.2020/ST 2084 with cICP)
//   --shot-size WxH    capture size (default 2560x1440)
//   --shot-fps N       simulated steps per second, 30..240 (default 144 = every recorded md5); 60 renders a
//                      given delay in 60/144 of the frames (user 2026-11-16: no reason to go above 60)
//   --shot-delay N     seconds of wallpaper time to simulate first (default 40)
//   --shot-series N:S  N captures, S seconds apart, named by elapsed seconds
//                      (<out>-NNN.png; a sub-second delay or interval names
//                      them in TENTHS as <out>-NNNN.png, e.g. 0600/0605/0610)
//   --shot-pour X,Y,S,D  hold LMB at (X,Y) px from S for D seconds
//   --shot-drop X,Y,T[,VY]  one ink drop at (X,Y) px at wallpaper time T
//                      (VY = downward impulse; default [drops] speed)
//   --shot-mouse X,Y,DX,DY,T[,DUR]  one synthetic pointer STROKE from (X,Y) px
//                      to (X+DX,Y+DY) px, starting at wallpaper time T over
//                      DUR seconds (default 0.5). Pointer only -- never a
//                      click -- so it drives [liquid_acid] mouse_oil_mode and
//                      nothing else.
//   --shot-preset <ini> AT <sec>  apply that preset at wallpaper time <sec>,
//                      through the tray's own ApplyPreset (look switches too)
//   --seed N           seed every rand() behavior (default 1234) -> determinism
//   --ini <path>       read config from this file instead of the live ini
//   --hdr on|off       set the HDR state instead of querying the display
//   --sdr-white <nits> SDR-content brightness for HDR mode (default 240)
//   --panel-max <nits> stands in for the DXGI-reported max (peak_nits=-1 only)
//   --sdr-gamut 0|1|2  [hdr] sdr_gamut for this run (HDR off only: 0 plain sRGB, 1 match HDR on the
//                      panel, 2 the HDR stretch); --panel-primaries rx,ry,gx,gy,bx,by stands in for
//                      the DXGI-reported primaries (without it mode 1 uses the P3 stand-in)
//   --mouse-none       no mouse splats (the default in shot mode)
//   --shot-png-only    write only <out>.png (the md5 file; stats still logged)
//   --cycle            force the [cycle] director on (it reads [cycle] from --ini)
//   --cycle-dwell S / --cycle-jitter J / --cycle-order fixed|alternate_random /
//   --cycle-seed N (default --seed) / --cycle-stage N (1-based start)
//   --cycle-next-at T[,T..]  Next stage at wallpaper time T (the tray's call)
//   --cycle-pause-at T1,T2   CyclePause at T1, CycleResume at T2 (animators.h)
//   --shot-queue C:M@T[,..]  the Settings window's "show next": queue combo C (kComboSeed name), mode M
//                      (0 vivid / 1 darkroom) at wallpaper time T (the colour director's queue)
//   --cycle-keep-at T,SEC    ui3 Keep this at wallpaper time T for SEC s ([keep] lines: start, end)
//   --cycle-visit-at T,N,SEC ui3 a visit to stage N (1-based, a rated look) at T for SEC s ([visit] lines);
//                      N = ink | fluid: a GUEST visit to that look's start preset (CycleVisitGuest)
//   --shot-hide C@T    ui3 the Settings window's Remove of combo C at T (DirectorHideCombo, memory only)
//   --shot-freeze NAME,T1,T2 Freeze(palette|hue2|rig|hueshift|transition|clocks)
//   --clocks-dryrun HOURS    brief CLOCKS: the clock scheduler alone (CPU, 10 Hz
//                      virtual time), one line per draw + a summary, then exit
//   --clocks-force G=F|mode|tail[,..]  pin clock groups (all = every group)
//   --clocks-hold-at T1,T2   ClocksHold(true) at T1, (false) at T2 (the Settings window's calls)
//   --shot-fade F      hold the display fade at F (fade-scaling proof)
//   --cover-sweep a,b,.. also step a CPU-only shadow hue2 mix field per listed
//                      film_hue2_cover bias and log its [cover] line (every 5 s
//                      and at each capture) beside the live field's; never drawn.
//                      An entry b@s also sets that field's film_hue2_scale s.
//   --cover-seeds N    run every --cover-sweep entry over N noise seeds (the
//                      value-noise lattice; seed 0 = the live field's noise)
//   --photos <dir>     brief BY: the folder an EMPTY [photo] folder= means in
//                      this shot run (without it a --shot run has no default
//                      photos folder, so identity never depends on the user's
//                      photos). A photo stage's decode is JOINED at the black
//                      point under --shot (frame-deterministic).
//   --shot-time0 S     start the master clock at S seconds of uptime (what a look does after
//                      hours: time-driven effects, float precision); default 0
//   --test-device-lost T  self-test of the GPU device-loss recovery (review 2026-10-04 R3):
//                      ID3D12Device5::RemoveDevice() at wallpaper time T, then the live
//                      loop's rebuild (Shutdown + soft init, same config), rendering on to
//                      the capture; [devlost] lines in the shot log. Without it a real
//                      device loss in a --shot run stays fatal.
//   --snapshot-at T <png>  ui3 B5 test: the Settings window's snapshot (RequestSnapshot) asked
//                      in the frame that completes T s of wallpaper time (T = --shot-delay:
//                      the shot's own frame), written as an 8-bit PNG when TakeSnapshot
//                      delivers; a [snapshot] line in the shot log. The shot is unchanged.
//   --test-murk-async  post-night T1 gate: the murk bakes on the live wallpaper's worker path
//                      (the volume lands frames later) instead of synchronously in the frame
//
// Cycle control of a RUNNING instance (posts WM_COMMAND and exits; never
// starts a wallpaper): --cycle-next | --cycle-prev | --cycle-on | --cycle-off
// [--cycle-target shot] (shot = a cycling headless run instead of the tray).
// Normal launch: --cycle-stage N starts the cycle on stage N.

#include <windows.h>
#include <cstdarg>
#include <shellapi.h>
#include <shlobj.h>
#include <dxgi1_6.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <share.h>
#include <vector>
#include <algorithm>
#include <string>
#include <csignal>
#include <exception>
#include <tlhelp32.h>
#include "fluid.h"
#include "app_state.h"
#include "cycle.h"
#include "clocks.h"
#include "director.h"
#include "photo.h"
#include "animators.h"
#include "ui/ui_model.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

void WpLog(const char* fmt, ...);            // defined below; used by Fail()
static void ExitLog(const char* fmt, ...);   // post-night T2: an "exit:" line (below)
static HWND g_wallpaperWnds[4] = {};          // our WorkerW children (main + mirror), for Fail()
void ForegroundDesc(char* out, size_t cap);

void Fail(const char* what, HRESULT hr) {
    char buf[512];
    _snprintf_s(buf, _TRUNCATE, "%s failed (hr=0x%08lX)", what, (unsigned long)hr);
    fprintf(stderr, "FATAL: %s\n", buf);
    // Record it BEFORE the dialog. A fatal box the user dismisses -- or
    // never sees, because a fullscreen game is covering it -- used to leave
    // no evidence at all, which is exactly the position the second of the
    // two crashes left us in. Now every Fail() is in the log with its
    // HRESULT and with whatever owned the foreground.
    char fgd[256];
    ForegroundDesc(fgd, sizeof(fgd));
    WpLog("FATAL %s  %s", buf, fgd);
    // The box below can sit unseen for hours (a fullscreen game covers it) while the last
    // presented frame stays frozen on the OLED. Hide our windows first, so the shell's own
    // wallpaper shows instead (review 2026-10-04 R3). A GPU device loss in the live wallpaper
    // never gets here any more: the renderer flags it and the main loop rebuilds the device.
    for (HWND w : g_wallpaperWnds)
        if (w && IsWindow(w)) ShowWindow(w, SW_HIDE);
    MessageBoxA(nullptr, buf, "Fluid Wallpaper - fatal error", MB_ICONERROR);
    ExitLog("exit: fatal error (%s), after its box", buf);
    ExitProcess(1);
}

// ---------------------------------------------------------------------------
// App state + settings (persisted to %APPDATA%\FluidWallpaper\settings.ini)
// ---------------------------------------------------------------------------

static bool     g_running = true;
static bool     g_manualPause = false;
bool            g_pauseOnFullscreen = true;   // shared with settings.cpp
bool            g_pauseOnMaximized = true;
// [general] startup_grace_s (user 2026-10-28: "once I open it, it is forced to run even if I'm in game for 30
// seconds"): the fullscreen/maximized pause is ignored for this long after launch. 0 = off.
static ULONGLONG g_startupGraceMs = 30000;
static ULONGLONG g_graceDrawnMs = 0;      // frame time drawn since start: the startup grace's clock
// [general] startup_grace_fps (user 2026-10-31: "the unblocked 30 seconds are rendered at 30 fps, like if I'm in
// game"): the frame cap while the grace renders THROUGH a fullscreen / maximized app; the grace clock counts drawn
// time at 1/30 s per frame, so the warm-up still lasts the full startup_grace_s. 0 = no cap (the fps_limit).
static float    g_startupGraceFps = 30.0f;
static bool     g_graceThrottled = false;  // the grace is running with such an app in front (the 500 ms check)
// [general] fs_suspend_mode: what a long fullscreen pause does after kSuspendAfterMs. 1 (default) = tear the
// renderer down only while its VRAM is over the OS budget (a paused wallpaper only HOLDS memory, which is fine
// until a game needs it); 0 = always (the pre-2026-10-28 behaviour); 2 = never.
static int       g_fsSuspendMode = 1;
// [general] fs_vram_pct (user 2026-10-30: "why doesnt ur wallpaper turn off if its over like 80% usage", "have it
// check like every 3 mins"): under mode 1, a long fullscreen pause ALSO tears down when the WHOLE card's dedicated
// VRAM (FluidRenderer::VramCardUsage: the game + the desktop + every app) is at or over this percent, checked
// kSuspendAfterMs into the pause and then every 3 minutes while it lasts. 0 = off (only the per-process budget).
static int       g_fsVramPct = 80;
float           g_hdrPeakNits = -1.0f;        // -1 = panel max (auto), 0 = off, else nits
int             g_gamutMode = 2;              // 0 sRGB, 1 P3, 2 BT.2020
// [hdr] sdr_gamut: what the gamut becomes while Windows HDR is OFF (FluidRenderer::SetSdrGamut):
// 0 plain sRGB, 1 match HDR on this panel (gamut -> the panel's primaries; default), 2 the HDR stretch.
int             g_sdrGamutMode = 1;
static float    g_panelPrim[6] = {};          // xy of R, G, B of the wallpaper's monitor (DXGI / EDID)
static bool     g_panelPrimValid = false;     // last GOOD read: a missed lookup never clears it
static bool     g_sdrManaged = false;         // Windows colour-manages the SDR desktop (ACM / WCG)
static float    g_mirrorPrim[6] = {};         // the same three for the second-monitor mirror
static bool     g_mirrorPrimValid = false;
static bool     g_mirrorSdrManaged = false;
static bool     g_mirrorStale = false;        // the mirror's monitor went away, came back or moved
FluidRenderer*  g_renderer = nullptr;
static bool     g_forceRender = false;
static bool     g_testSuspend = false;   // --test-suspend: scripted suspend/resume cycle
static float    g_fpsOverride = 0.0f;   // --fps test flag; 0 = use settings
float           g_currentFps = 0.0f;    // smoothed achieved framerate

// U42 (review 2026-10-04): WHO holds the manual pause. The scripts used to share one bool, so
// the game watcher's CMD_PAUSE_OFF un-paused an away-pause (and the reverse) and nothing could
// tell. Now every source holds its own bit, the pause stays on while ANY bit is set, and
// g_manualPause is the derived "held by anyone" flag the render loop and the UI read.
enum PauseHolder : UINT {
    HOLD_USER = 1,     // tray / Settings toggle, and CMD_PAUSE_ON with lParam 0 (pre-U42 callers)
    HOLD_AWAY = 2,     // tools\away-pause.ps1
    HOLD_GAME = 4,     // tools\game-pause-live.ps1
    HOLD_ALL  = 7,     // the user's own resume, and CMD_PAUSE_OFF with lParam 0, clear them all
};
static UINT     g_pauseHolders = 0;

static void PauseHolderNames(UINT h, char* out, size_t cap) {
    static const char* const kNames[3] = { "user", "away", "game" };
    if (!h) { strcpy_s(out, cap, "none"); return; }
    out[0] = 0;
    for (int i = 0; i < 3; i++)
        if (h & (1u << i)) {
            if (out[0]) strcat_s(out, cap, "+");
            strcat_s(out, cap, kNames[i]);
        }
}

// The ONE writer of the manual pause. on: `holder` takes the pause; off: `holder` lets go.
// Every call lands in FluidWallpaper.log with its source and the resulting state; an exact
// repeat that changes nothing is logged once per streak, so an older watcher re-sending
// CMD_PAUSE_ON every poll cannot roll the log.
static void SetManualPause(bool on, UINT holder, const char* source) {
    const UINT before = g_pauseHolders;
    if (on) g_pauseHolders |= holder;
    else    g_pauseHolders &= ~holder;
    g_manualPause = g_pauseHolders != 0;
    char hn[32], held[32], line[192];
    PauseHolderNames(holder, hn, sizeof(hn));
    PauseHolderNames(g_pauseHolders, held, sizeof(held));
    const char* note = "";
    if ((before != 0) == g_manualPause)
        note = before == g_pauseHolders ? " (already)" : (on ? " (already paused)" : " (still held)");
    _snprintf_s(line, _TRUNCATE, "pause: %s source=%s holder=%s -> %s (held: %s)%s",
                on ? "ON" : "OFF", source, hn, g_manualPause ? "paused" : "running", held, note);
    printf("%s\n", line);
    static char lastRepeat[192] = "";
    if (before == g_pauseHolders) {
        if (strcmp(lastRepeat, line) == 0) return;
        strcpy_s(lastRepeat, line);
    } else {
        lastRepeat[0] = 0;
    }
    WpLog("%s", line);
}

// post-night T2: why the main loop ended, for its "exit:" line (every way out of the process logs one)
static const char* g_exitReason = nullptr;
void RequestExit()   { g_running = false; g_exitReason = "the Settings window's Exit button"; }
// the Settings window's Pause/Resume button: a resume is the user's own and clears every holder
void TogglePause()   { SetManualPause(!g_manualPause, g_manualPause ? HOLD_ALL : HOLD_USER, "settings"); }
bool IsManualPaused() { return g_manualPause; }
static bool     g_fsPaused = false;
// renderer fully torn down (fullscreen suspend, device lost, refused startup). File scope, not a
// wWinMain local, so the tray's pause-state query (U42) can report it.
static bool     g_suspended = false;
static HMONITOR g_monitor = nullptr;
wchar_t         g_iniPath[MAX_PATH] = {};
wchar_t         g_configIniPath[MAX_PATH] = {};   // == g_iniPath unless --ini
bool            g_configReadOnly = false;         // --shot: never write config

// ---------------------------------------------------------------------------
// Rolling diagnostic log, %APPDATA%\FluidWallpaper\FluidWallpaper.log.
// Deliberately tiny and append-only. It exists because a swap-chain creation
// was once DENIED on a resume while a fullscreen game owned the output
// (E_ACCESSDENIED), and all the evidence there was of it was one fatal
// message box and a dead process. Now every attempt, its HRESULT and what was
// in the foreground land here. Nobody creates this file by hand -- the first
// WpLog() call does. Skipped entirely under --shot (g_configReadOnly), so a
// headless render still writes nothing anywhere.
// ---------------------------------------------------------------------------
wchar_t         g_logPath[MAX_PATH] = {};
void WpLog(const char* fmt, ...) {
    if (g_configReadOnly || !g_logPath[0]) return;
    char line[1024];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int n = _snprintf_s(line, _TRUNCATE, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] ",
                        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                        st.wSecond, st.wMilliseconds);
    if (n < 0) return;
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line + n, sizeof(line) - n, _TRUNCATE, fmt, ap);
    va_end(ap);
    strcat_s(line, "\r\n");
    // write-data access, not append-only: SetEndOfFile below needs it (with
    // FILE_APPEND_DATA alone the truncate was refused and the log never rolled)
    HANDLE h = CreateFileW(g_logPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz = {};
    // Roll at 256 KB by truncating: a wallpaper must never fill a disk, and
    // the interesting entries are always the last ones.
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 256 * 1024) {
        SetFilePointer(h, 0, nullptr, FILE_BEGIN);
        SetEndOfFile(h);
    }
    SetFilePointer(h, 0, nullptr, FILE_END);
    DWORD wrote = 0;
    WriteFile(h, line, (DWORD)strlen(line), &wrote, nullptr);
    CloseHandle(h);
}

// The exe name (no folder) of the process that owns a window, for the log lines below. Never the window
// TITLE: a title carries document names, page titles and chat names, and this log is the file people
// send along with a problem report. The folder is left out too (it holds the Windows user name).
static void WindowExeName(HWND w, wchar_t* out, size_t cap) {
    wcsncpy_s(out, cap, L"?", _TRUNCATE);
    DWORD pid = 0;
    if (!w || !GetWindowThreadProcessId(w, &pid) || !pid) return;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return;
    wchar_t path[MAX_PATH];
    DWORD n = MAX_PATH;
    if (QueryFullProcessImageNameW(h, 0, path, &n)) {
        const wchar_t* base = wcsrchr(path, L'\\');
        wcsncpy_s(out, cap, base ? base + 1 : path, _TRUNCATE);
    }
    CloseHandle(h);
}

// Foreground window class + owning exe for those log lines: when a swap chain is
// refused, WHICH app owns the screen is the whole diagnosis.
void ForegroundDesc(char* out, size_t cap) {
    HWND fg = GetForegroundWindow();
    wchar_t cls[96] = L"?", exe[96] = L"?";
    if (fg) {
        GetClassNameW(fg, cls, 96);
        WindowExeName(fg, exe, 96);
    }
    _snprintf_s(out, cap, _TRUNCATE, "fg='%ls' exe='%ls'%s", cls, exe,
                (fg && IsZoomed(fg)) ? " maximized" : "");
}

// ---------------------------------------------------------------------------
// post-night T2 (2026-10-31 17:45: the live wallpaper was gone a minute after a manual pause, with no
// log line and no WER event): every way out of the process now leaves an "exit: <reason>" line. The
// ordinary ways say so where they happen (the main loop's end, Fail(), a shader that does not compile,
// the session ending); the hooks below catch the rest: a crash on any thread (logged, then handed on to
// the filter that was there, so Windows treats it as before), abort() / std::terminate, an exit() from
// anywhere, the console closing (--console). A kill from OUTSIDE (TerminateProcess, Stop-Process, the job
// object of a launching shell closing) runs no code of ours at all: the "startup:" line that names the
// parent process and a kill-on-close job is the evidence for that one.
static volatile LONG g_exitLogged = 0;
static void ExitLog(const char* fmt, ...) {
    char line[700];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    InterlockedExchange(&g_exitLogged, 1);
    WpLog("%s", line);
}
static LPTOP_LEVEL_EXCEPTION_FILTER g_prevCrashFilter = nullptr;
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    static char mod[MAX_PATH];                 // static: a stack overflow leaves little stack
    const EXCEPTION_RECORD* er = ep ? ep->ExceptionRecord : nullptr;
    void* addr = er ? er->ExceptionAddress : nullptr;
    HMODULE hm = nullptr;
    mod[0] = 0;
    if (addr && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)addr, &hm))
        GetModuleFileNameA(hm, mod, MAX_PATH);
    const char* name = strrchr(mod, '\\');
    name = name ? name + 1 : (mod[0] ? mod : "?");
    const DWORD code = er ? er->ExceptionCode : 0;
    static char avd[64];
    avd[0] = 0;
    if (code == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        _snprintf_s(avd, _TRUNCATE, ", %s %p", er->ExceptionInformation[0] == 8 ? "DEP at" :
                    er->ExceptionInformation[0] ? "writing" : "reading", (void*)er->ExceptionInformation[1]);
    ExitLog("exit: CRASH exception 0x%08lX%s at %p (%s+0x%llX), thread %lu%s",
            (unsigned long)code, code == EXCEPTION_STACK_OVERFLOW ? " (stack overflow)" :
            code == 0xE06D7363u ? " (uncaught C++ exception)" : "", addr, name,
            (unsigned long long)(hm ? (const char*)addr - (const char*)hm : 0), GetCurrentThreadId(), avd);
    return g_prevCrashFilter ? g_prevCrashFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}
static void __cdecl OnAbortSignal(int) {
    ExitLog("exit: abort() (SIGABRT), thread %lu", GetCurrentThreadId());
    signal(SIGABRT, SIG_DFL);                  // abort() goes on as it would have
}
static void __cdecl OnTerminate() {
    ExitLog("exit: std::terminate (an exception nobody caught), thread %lu", GetCurrentThreadId());
    abort();
}
static void __cdecl OnCrtExit() {
    if (!g_exitLogged) WpLog("exit: exit() with no reason logged before it");
}
static BOOL WINAPI OnConsoleCtrl(DWORD ev) {
    ExitLog("exit: console event %lu (0 Ctrl+C, 1 Ctrl+Break, 2 the console closed, 5 logoff, 6 shutdown)",
            (unsigned long)ev);
    return FALSE;                              // the default handler ends the process, as before
}
// The process this one was started by, and whether it sits in a job that dies with its owner: a wallpaper
// launched from a tool shell's job goes down silently when that shell's job is closed.
static void LogStartupOrigin() {
    const DWORD self = GetCurrentProcessId();
    DWORD parent = 0;
    wchar_t pname[MAX_PATH] = L"?";
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = { sizeof(pe) };
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
            if (pe.th32ProcessID == self) { parent = pe.th32ParentProcessID; break; }
        pe.dwSize = sizeof(pe);
        for (BOOL ok = parent && Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
            if (pe.th32ProcessID == parent) { wcscpy_s(pname, pe.szExeFile); break; }
        CloseHandle(snap);
    }
    BOOL inJob = FALSE;
    IsProcessInJob(GetCurrentProcess(), nullptr, &inJob);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
    const bool haveLi = inJob && QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &li,
                                                            sizeof(li), nullptr);
    const DWORD lf = haveLi ? li.BasicLimitInformation.LimitFlags : 0;
    WpLog("startup: pid %lu, parent pid %lu (%ls)%s%s%s", (unsigned long)self, (unsigned long)parent, pname,
          inJob ? ", in a job object" : ", no job object",
          (lf & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) ? " that KILLS ON CLOSE (this wallpaper dies with its launcher)" : "",
          (lf & (JOB_OBJECT_LIMIT_BREAKAWAY_OK | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK)) ? " (breakaway allowed)" : "");
}
static void InstallExitLogging() {
    g_prevCrashFilter = SetUnhandledExceptionFilter(CrashFilter);
    signal(SIGABRT, OnAbortSignal);
    std::set_terminate(OnTerminate);
    atexit(OnCrtExit);
    SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);
}

static void InitSettingsPath() {
    wchar_t* appdata = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) {
        wchar_t dir[MAX_PATH];
        _snwprintf_s(dir, _TRUNCATE, L"%s\\FluidWallpaper", appdata);
        CoTaskMemFree(appdata);
        CreateDirectoryW(dir, nullptr);
        _snwprintf_s(g_iniPath, _TRUNCATE, L"%s\\settings.ini", dir);
        _snwprintf_s(g_logPath, _TRUNCATE, L"%s\\FluidWallpaper.log", dir);
    }
    wcscpy_s(g_configIniPath, MAX_PATH, g_iniPath);
}

static void LoadSettings() {
    if (!g_configIniPath[0]) return;
    g_pauseOnFullscreen = GetPrivateProfileIntW(L"general", L"pause_on_fullscreen", 1, g_configIniPath) != 0;
    {
        const int grace = (int)GetPrivateProfileIntW(L"general", L"startup_grace_s", 30, g_configIniPath);
        g_startupGraceMs = (ULONGLONG)(grace < 0 ? 0 : (grace > 600 ? 600 : grace)) * 1000ull;
        const int gfps = (int)GetPrivateProfileIntW(L"general", L"startup_grace_fps", 30, g_configIniPath);
        g_startupGraceFps = (float)(gfps < 0 ? 0 : (gfps > 240 ? 240 : gfps));
        const int mode = (int)GetPrivateProfileIntW(L"general", L"fs_suspend_mode", 1, g_configIniPath);
        g_fsSuspendMode = (mode < 0 || mode > 2) ? 1 : mode;
        const int pct = (int)GetPrivateProfileIntW(L"general", L"fs_vram_pct", 80, g_configIniPath);
        g_fsVramPct = (pct < 0 || pct > 100) ? 80 : pct;
    }
    g_pauseOnMaximized = GetPrivateProfileIntW(L"general", L"pause_on_maximized", 1, g_configIniPath) != 0;
    wchar_t buf[64] = {};
    GetPrivateProfileStringW(L"hdr", L"peak_nits", L"-1", buf, 64, g_configIniPath);
    g_hdrPeakNits = (float)_wtof(buf);
    g_gamutMode = (int)GetPrivateProfileIntW(L"hdr", L"gamut", 2, g_configIniPath);
    if (g_gamutMode < 0 || g_gamutMode > 2) g_gamutMode = 2;
    g_sdrGamutMode = (int)GetPrivateProfileIntW(L"hdr", L"sdr_gamut", 1, g_configIniPath);
    if (g_sdrGamutMode < 0 || g_sdrGamutMode > 2) g_sdrGamutMode = 1;
}

static void SaveSettings() {
    if (!g_iniPath[0] || g_configReadOnly) return;
    WritePrivateProfileStringW(L"general", L"pause_on_fullscreen",
                               g_pauseOnFullscreen ? L"1" : L"0", g_iniPath);
    WritePrivateProfileStringW(L"general", L"pause_on_maximized",
                               g_pauseOnMaximized ? L"1" : L"0", g_iniPath);
    // cycling: the globals hold the current STAGE's peak / gamut; the file keeps the user's own
    float peak = g_hdrPeakNits;
    int gamut = g_gamutMode;
    CycleUserShell(peak, gamut);
    wchar_t buf[32];
    swprintf_s(buf, L"%.0f", peak);
    WritePrivateProfileStringW(L"hdr", L"peak_nits", buf, g_iniPath);
    swprintf_s(buf, L"%d", gamut);
    WritePrivateProfileStringW(L"hdr", L"gamut", buf, g_iniPath);
    swprintf_s(buf, L"%d", g_sdrGamutMode);
    WritePrivateProfileStringW(L"hdr", L"sdr_gamut", buf, g_iniPath);
}

// ---- Oil3D colour stage L1: the curated DYE PALETTE (dyes.ini) -------------
// Data, not shader: [dyes] name = hue, sigma, muted -- hue in degrees RELATIVE
// to the anchor (the film's own hue), sigma = the dye's band width in degrees
// (broad = desaturated), muted 0|1 (a pale dye: half the strength). The film's
// own dye A1 is the entry 'self'. Read once, first file found of:
// %APPDATA%\FluidWallpaper\dyes.ini (the user's), <exe dir>\dyes.ini,
// <exe dir>\..\reference\configs\dyes.ini (the repo's, from build\ or build2\).
// Oil3D L2 (folded in): when none of the three exists, the COMPILED-IN seed
// below is the palette, with one log line: an exe run from a folder with no
// repo beside it (a live swap, the branch the live exe was built from) resolves
// every name exactly as with the file.
struct DyeEntry { std::wstring name; float hue, sigma; bool muted; };
static std::vector<DyeEntry> g_dyes;
static bool g_dyesLoaded = false;
// A copy of reference\configs\dyes.ini's [dyes] seed (2026-10-16, 15 entries; L3 +2 broad = 17),
// same order, same values: every one is an integer, so the swscanf of the file
// and these literals give the same floats bit for bit. Keep the two in step
// when the palette changes (L3 extends it).
struct DyeSeed { const wchar_t* name; float hue, sigma; int muted; };
static const DyeSeed kDyeSeed[] = {
    { L"self",          0.0f, 55.0f, 0 },
    { L"dash_20",      20.0f, 45.0f, 0 },
    { L"dash_m20",    -20.0f, 45.0f, 0 },
    { L"analog_45",    45.0f, 50.0f, 0 },
    { L"pair_m140",  -140.0f, 50.0f, 0 },
    { L"pair_125",    125.0f, 50.0f, 0 },
    { L"pair_160",    160.0f, 50.0f, 0 },
    { L"pair_180",    180.0f, 50.0f, 0 },
    { L"pair_m150",  -150.0f, 50.0f, 0 },
    { L"square_90",    90.0f, 50.0f, 0 },
    { L"triad_120",   120.0f, 50.0f, 0 },
    { L"triad_m120", -120.0f, 50.0f, 0 },
    { L"muted_60",     60.0f, 80.0f, 1 },
    { L"muted_180",   180.0f, 80.0f, 1 },
    { L"muted_m90",   -90.0f, 80.0f, 1 },
    { L"broad_0",       0.0f, 100.0f, 0 },   // Oil3D L3: the broad, paint-like pair
    { L"broad_180",   180.0f, 140.0f, 0 },
};
static const float kDyeK = 1.8f, kDyeKMuted = 0.9f;   // absorbance off-band at amount 1
static void DyeLog(const char* fmt, ...) {               // the shot log (stdout) + WpLog
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(b, _TRUNCATE, fmt, ap);
    va_end(ap);
    printf("%s\n", b);
    WpLog("%s", b);
}
static void LoadDyePalette() {
    if (g_dyesLoaded) return;
    g_dyesLoaded = true;
    wchar_t cand[4][MAX_PATH] = {};
    PWSTR appdata = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) {
        _snwprintf_s(cand[0], _TRUNCATE, L"%s\\FluidWallpaper\\dyes.ini", appdata);
        CoTaskMemFree(appdata);
    }
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (wchar_t* sl = wcsrchr(exe, L'\\')) *sl = 0;
    _snwprintf_s(cand[1], _TRUNCATE, L"%s\\dyes.ini", exe);
    _snwprintf_s(cand[2], _TRUNCATE, L"%s\\..\\reference\\configs\\dyes.ini", exe);
    for (int c = 0; c < 3; c++) {
        if (!cand[c][0] || GetFileAttributesW(cand[c]) == INVALID_FILE_ATTRIBUTES) continue;
        std::vector<wchar_t> buf(32768);
        const DWORD n = GetPrivateProfileSectionW(L"dyes", buf.data(), (DWORD)buf.size(), cand[c]);
        for (const wchar_t* e = buf.data(); e < buf.data() + n && *e; e += wcslen(e) + 1) {
            std::wstring line(e);
            const size_t sc = line.find_first_of(L";#");
            if (sc == 0) continue;
            if (sc != std::wstring::npos) line.resize(sc);
            const size_t eq = line.find(L'=');
            if (eq == std::wstring::npos) continue;
            std::wstring nm = line.substr(0, eq);
            while (!nm.empty() && iswspace(nm.back())) nm.pop_back();
            while (!nm.empty() && iswspace(nm.front())) nm.erase(nm.begin());
            float hue = 0.0f, sig = 50.0f; int muted = 0;
            if (nm.empty() || swscanf_s(line.c_str() + eq + 1, L" %f , %f , %d", &hue, &sig, &muted) < 2) continue;
            g_dyes.push_back({ nm, hue, sig, muted != 0 });
        }
        DyeLog("dyes: %d palette entries from %ls", (int)g_dyes.size(), cand[c]);
        return;
    }
    for (const DyeSeed& s : kDyeSeed) g_dyes.push_back({ s.name, s.hue, s.sigma, s.muted != 0 });
    DyeLog("dyes: no dyes.ini found (%%APPDATA%%\\FluidWallpaper, the exe dir, ..\\reference\\configs);"
           " using the compiled-in seed palette, %d entries", (int)g_dyes.size());
}
// A dye key's value: a palette name, or a plain number (degrees relative to the
// anchor, the 'self' band). "" / unknown = no dye (k 0).
// An unknown name is logged ONCE per name (every ini load resolves the dyes
// again: a stage cycle used to repeat the line on every stage).
static std::vector<std::wstring> g_dyesWarned;
static void ResolveDye(const wchar_t* v, float& hue, float& sigma, float& k) {
    LoadDyePalette();
    hue = 0.0f; sigma = 50.0f; k = 0.0f;
    if (!v || !v[0]) return;
    for (const DyeEntry& d : g_dyes) {
        if (_wcsicmp(d.name.c_str(), v) == 0) {
            hue = d.hue; sigma = d.sigma; k = d.muted ? kDyeKMuted : kDyeK; return;
        }
    }
    wchar_t* end = nullptr;
    const float h = (float)wcstod(v, &end);
    if (end && end != v) {
        float hs = 0.0f, ss = 55.0f, ks = 0.0f;
        ResolveDye(L"self", hs, ss, ks);
        hue = h; sigma = ks > 0.0f ? ss : 55.0f; k = kDyeK; return;
    }
    for (const std::wstring& w : g_dyesWarned)
        if (_wcsicmp(w.c_str(), v) == 0) return;
    g_dyesWarned.push_back(v);
    DyeLog("dyes: '%ls' is not in the palette (no dye; logged once)", v);
}

// Full config from an ini file — every value the settings window writes.
// Absent keys keep whatever is already in cfg (lets presets be partial).
static void LoadConfigFromIni(const wchar_t* ini, FluidConfig& cfg) {
    if (!ini || !ini[0]) return;
    auto getF = [ini](const wchar_t* sec, const wchar_t* key, float def) {
        wchar_t buf[64] = {};
        GetPrivateProfileStringW(sec, key, L"", buf, 64, ini);
        return buf[0] ? (float)_wtof(buf) : def;
    };
    auto getB = [ini](const wchar_t* sec, const wchar_t* key, bool def) {
        return GetPrivateProfileIntW(sec, key, def ? 1 : 0, ini) != 0;
    };
    auto getI = [ini](const wchar_t* sec, const wchar_t* key, int def) {
        return (int)GetPrivateProfileIntW(sec, key, def, ini);
    };
    cfg.densityDissipation  = getF(L"sim", L"density_diffusion", cfg.densityDissipation);
    cfg.velocityDissipation = getF(L"sim", L"velocity_diffusion", cfg.velocityDissipation);
    cfg.pressureDissipation = getF(L"sim", L"pressure_diffusion", cfg.pressureDissipation);
    cfg.decayFast           = getF(L"sim", L"decay_fast", cfg.decayFast);
    cfg.decayThreshold      = getF(L"sim", L"decay_threshold", cfg.decayThreshold);
    cfg.satRestore          = getF(L"sim", L"saturation_restore", cfg.satRestore);
    cfg.maxBrightness       = getF(L"sim", L"max_brightness", cfg.maxBrightness);
    cfg.curl                = getF(L"sim", L"vorticity", cfg.curl);
    cfg.baroclinic          = getF(L"sim", L"baroclinic", cfg.baroclinic);
    cfg.flowSpeed           = getF(L"sim", L"flow_speed", cfg.flowSpeed);
    cfg.splatRadius         = getF(L"sim", L"splat_radius", cfg.splatRadius);
    cfg.shading             = getB(L"sim", L"shading", cfg.shading);
    cfg.simRes              = getI(L"sim", L"sim_res", cfg.simRes);
    cfg.dyeRes              = getI(L"sim", L"dye_res", cfg.dyeRes);
    cfg.pressureIterations  = getI(L"sim", L"pressure_iterations", cfg.pressureIterations);
    cfg.hdrKnee             = getF(L"hdr", L"knee", cfg.hdrKnee);
    cfg.hdrSaturation       = getF(L"hdr", L"saturation", cfg.hdrSaturation);
    cfg.hdrBrightness       = getF(L"hdr", L"brightness", cfg.hdrBrightness);
    cfg.hdrContrast         = getF(L"hdr", L"contrast", cfg.hdrContrast);
    cfg.hdrCompensation     = getB(L"hdr", L"compensation", cfg.hdrCompensation);
    cfg.colorCyclePeriod    = getF(L"behavior", L"color_cycle_period", cfg.colorCyclePeriod);
    cfg.wanderers           = getB(L"behavior", L"wanderers", cfg.wanderers);
    cfg.wandererCount       = getI(L"behavior", L"wanderer_count", cfg.wandererCount);
    cfg.wandererMode        = getI(L"behavior", L"wanderer_mode", cfg.wandererMode);
    cfg.wandererSpeed       = getF(L"behavior", L"wanderer_speed", cfg.wandererSpeed);
    cfg.wandererBrightness  = getF(L"behavior", L"wanderer_brightness", cfg.wandererBrightness);
    cfg.wandererScale       = getF(L"behavior", L"wanderer_scale", cfg.wandererScale);
    cfg.wandererResumeDelay = getF(L"behavior", L"wanderer_resume_delay", cfg.wandererResumeDelay);
    cfg.autoPause           = getB(L"behavior", L"auto_pause", cfg.autoPause);
    cfg.darkFloor           = getF(L"behavior", L"dark_floor", cfg.darkFloor);
    cfg.darkLevel           = getF(L"behavior", L"dark_level", cfg.darkLevel);
    cfg.survDarkFloor       = getF(L"behavior", L"surv_dark_floor", cfg.survDarkFloor);
    cfg.contrastReq         = getF(L"behavior", L"contrast_req", cfg.contrastReq);
    cfg.dartEnabled         = getB(L"behavior", L"dart_enabled", cfg.dartEnabled);
    cfg.dartInterval        = getF(L"behavior", L"dart_interval", cfg.dartInterval);
    cfg.dartSpeed           = getF(L"behavior", L"dart_speed", cfg.dartSpeed);
    cfg.hsEnabled           = getB(L"behavior", L"hueshift_enabled", cfg.hsEnabled);
    cfg.hsStep              = getF(L"behavior", L"hueshift_step", cfg.hsStep);
    cfg.hsLinger            = getF(L"behavior", L"hueshift_linger", cfg.hsLinger);
    cfg.hsGlide             = getF(L"behavior", L"hueshift_glide", cfg.hsGlide);
    cfg.hsBurstSteps        = getI(L"behavior", L"hueshift_burst_steps", cfg.hsBurstSteps);
    cfg.hsOffTime           = getF(L"behavior", L"hueshift_off_time", cfg.hsOffTime);
    cfg.idleSplats          = getB(L"behavior", L"idle_splats", cfg.idleSplats);
    cfg.idleInterval        = getF(L"behavior", L"idle_interval", cfg.idleInterval);
    cfg.idleAmount          = getI(L"behavior", L"idle_amount", cfg.idleAmount);
    cfg.idleBrightness      = getF(L"behavior", L"idle_brightness", cfg.idleBrightness);
    cfg.holdToSplat         = getB(L"behavior", L"hold_to_splat", cfg.holdToSplat);
    cfg.splatOnClick        = getB(L"behavior", L"splat_on_click", cfg.splatOnClick);
    cfg.showMouse           = getB(L"behavior", L"show_mouse", cfg.showMouse);
    cfg.fpsLimit            = getF(L"general", L"fps_limit", cfg.fpsLimit);
    cfg.mirrorSecond        = getB(L"general", L"mirror_second", cfg.mirrorSecond);
    cfg.colorful            = getB(L"color", L"colorful", cfg.colorful);
    cfg.moreColors          = getB(L"color", L"more_colors", cfg.moreColors);
    cfg.postSaturation      = getF(L"color", L"post_saturation", cfg.postSaturation);
    cfg.postContrast        = getF(L"color", L"post_contrast", cfg.postContrast);
    cfg.postBrightness      = getF(L"color", L"post_brightness", cfg.postBrightness);
    cfg.postHue             = getF(L"color", L"post_hue", cfg.postHue);
    cfg.hueCenter           = getF(L"color", L"hue_center", cfg.hueCenter);
    cfg.hueRange            = getF(L"color", L"hue_range", cfg.hueRange);
    cfg.hueLinger           = getF(L"color", L"hue_linger", cfg.hueLinger);
    cfg.dyeDiffusion        = getF(L"sim", L"dye_diffusion", cfg.dyeDiffusion);
    cfg.curveEnabled        = getB(L"color", L"curve_enabled", cfg.curveEnabled);
    cfg.curveCenter         = getF(L"color", L"curve_center", cfg.curveCenter);
    cfg.curveWidth          = getF(L"color", L"curve_width", cfg.curveWidth);
    cfg.curveHeight         = getF(L"color", L"curve_height", cfg.curveHeight);
    cfg.shadowFloor         = getF(L"color", L"shadow_floor", cfg.shadowFloor);
    cfg.shadowKnee          = getF(L"color", L"shadow_knee", cfg.shadowKnee);
    cfg.gravity             = getF(L"sim", L"gravity", cfg.gravity);
    cfg.gravityPow          = getF(L"sim", L"gravity_pow", cfg.gravityPow);
    cfg.gravityBlur         = getF(L"sim", L"gravity_blur", cfg.gravityBlur);
    // ---- render look selector + "Liquid Acid" parameters -------------------
    // [look] style = fluid | liquid_acid | ink   (default fluid).
    // The settings-window checkboxes write the equivalent int keys
    // [look] liquid_acid = 0|1 / [look] ink = 0|1; a key that is actually
    // PRESENT in the file overrides the string form (so the checkbox still
    // works on an ini that also carries style=), and ink wins ties.
    {
        wchar_t style[32] = {};
        GetPrivateProfileStringW(L"look", L"style", L"", style, 32, ini);
        if (style[0]) {
            cfg.acid.enabled = (_wcsicmp(style, L"liquid_acid") == 0);
            cfg.ink.enabled  = (_wcsicmp(style, L"ink") == 0);
        }
        cfg.acid.enabled = getB(L"look", L"liquid_acid", cfg.acid.enabled);
        cfg.ink.enabled  = getB(L"look", L"ink", cfg.ink.enabled);
        if (cfg.ink.enabled) cfg.acid.enabled = false;
    }
    // ---- "Ink in water" render parameters ([ink]) --------------------------
    {
        InkConfig& k = cfg.ink;
        const wchar_t* S = L"ink";
        k.inverted     = getB(S, L"inverted", k.inverted);
        k.density      = getF(S, L"density", k.density);
        k.chroma       = getF(S, L"chroma", k.chroma);
        k.edgeStrength = getF(S, L"edge_strength", k.edgeStrength);
        k.edgeLo       = getF(S, L"edge_lo", k.edgeLo);
        k.edgeHi       = getF(S, L"edge_hi", k.edgeHi);
        k.edgeScale    = getF(S, L"edge_scale", k.edgeScale);
        k.vignette     = getF(S, L"vignette", k.vignette);
        k.coreKnee     = getF(S, L"core_knee", k.coreKnee);
        k.hdrCore      = getF(S, L"hdr_core", k.hdrCore);
        k.motionLo     = getF(S, L"motion_lo", k.motionLo);
        k.motionHi     = getF(S, L"motion_hi", k.motionHi);
        k.motionOpacity= getF(S, L"motion_opacity", k.motionOpacity);
        k.veilFloor    = getF(S, L"veil_floor", k.veilFloor);
        k.tintMidDip   = getF(S, L"tint_mid_dip", k.tintMidDip);
        k.pairSweepPeriod = getF(S, L"pair_sweep_period", k.pairSweepPeriod);
        k.parallax     = getF(S, L"parallax", k.parallax);
        k.parallaxScale= getF(S, L"parallax_scale", k.parallaxScale);
        k.parallaxDrift= getF(S, L"parallax_drift", k.parallaxDrift);
        k.tonemap      = getI(S, L"tonemap", k.tonemap);
        k.whiteNits    = getF(S, L"white_nits", k.whiteNits);
        k.blackNits    = getF(S, L"black_nits", k.blackNits);
        k.toneKnee     = getF(S, L"tone_knee", k.toneKnee);
        k.toneChroma   = getF(S, L"tone_chroma", k.toneChroma);
        k.tintHueBlend = getF(S, L"tint_hue_blend", k.tintHueBlend);
        k.blackOnly    = getI(S, L"black_only", k.blackOnly);       // test keys (ag-ink)
        k.filmColour   = getF(S, L"film_colour", k.filmColour);
        struct { const wchar_t* key; float* dst; } triples[] = {
            { L"paper_color", k.paper }, { L"tint_thin", k.tintThin },
            { L"tint_thick",  k.tintThick },
        };
        for (auto& t : triples) {
            wchar_t buf[64] = {}; float r, g, b;
            GetPrivateProfileStringW(S, t.key, L"", buf, 64, ini);
            if (swscanf_s(buf, L"%f %f %f", &r, &g, &b) == 3) {
                t.dst[0] = r; t.dst[1] = g; t.dst[2] = b;
            }
        }
    }
    // ---- screen mirroring / kaleidoscope ([mirror]) -------------------------
    // Display-only and look-agnostic, so this is the one block that is worth
    // shipping as a PARTIAL preset: an ini carrying nothing but [mirror] folds
    // whatever look happens to be running.
    {
        MirrorConfig& mr = cfg.mirror;
        const wchar_t* S = L"mirror";
        mr.mode         = getI(S, L"mode", mr.mode);
        mr.segments     = getI(S, L"segments", mr.segments);
        mr.source       = getI(S, L"source", mr.source);
        mr.rotatePeriod = getF(S, L"rotate_period", mr.rotatePeriod);
        mr.drift        = getF(S, L"drift", mr.drift);
        mr.soft         = getF(S, L"soft", mr.soft);
        // `center = "x y"` is the hand-written form; the settings sliders
        // write the two singles, and a single that is actually PRESENT wins
        // (same rule as [look] style vs the per-look int keys).
        wchar_t buf[64] = {}; float cx, cy;
        GetPrivateProfileStringW(S, L"center", L"", buf, 64, ini);
        if (swscanf_s(buf, L"%f %f", &cx, &cy) == 2) {
            mr.centerX = cx; mr.centerY = cy;
        }
        mr.centerX = getF(S, L"center_x", mr.centerX);
        mr.centerY = getF(S, L"center_y", mr.centerY);
        mr.camFov    = getF(S, L"cam_fov", mr.camFov);         // mode 5, the fish-eye camera (test)
        mr.camPitch  = getF(S, L"cam_pitch", mr.camPitch);
        mr.camHeight = getF(S, L"cam_height", mr.camHeight);
        mr.camFade   = getF(S, L"cam_fade", mr.camFade);
        mr.camRelief = getF(S, L"cam_relief", mr.camRelief);
    }
    // ---- [post]: film grain + aberration vignette, EVERY look --------------
    // The last thing that happens to the composite. Both amounts default to 0
    // and the shader branches the whole block out then, so an ini that does
    // not name these keys renders exactly as it did.
    {
        PostConfig& po = cfg.post;
        const wchar_t* S = L"post";
        po.filmGrain      = getF(S, L"film_grain", po.filmGrain);
        po.filmGrainSize  = getF(S, L"film_grain_size", po.filmGrainSize);
        po.filmGrainSpeed = getF(S, L"film_grain_speed", po.filmGrainSpeed);
        po.filmGrainFps   = getF(S, L"film_grain_fps", po.filmGrainFps);
        po.filmGrainColor = getF(S, L"film_grain_color", po.filmGrainColor);
        po.filmGrainChroma  = getF(S, L"film_grain_chroma",  po.filmGrainChroma);
        po.filmGrainDensity = getF(S, L"film_grain_density", po.filmGrainDensity);
        po.aberration     = getF(S, L"aberration", po.aberration);
        po.aberrationPx   = getF(S, L"aberration_px", po.aberrationPx);
        po.aberrationField= getF(S, L"aberration_field", po.aberrationField);
        po.aberrationCoc  = getF(S, L"aberration_coc", po.aberrationCoc);
        po.vignette       = getF(S, L"vignette", po.vignette);
        po.softness       = getF(S, L"softness", po.softness);
        po.halo           = getF(S, L"halo", po.halo);
        po.haloPx         = getF(S, L"halo_px", po.haloPx);
        po.bandMin        = getF(S, L"band_min", po.bandMin);
        po.postBlurPx     = getF(S, L"post_blur_px", po.postBlurPx);
        po.postGlow       = getF(S, L"post_glow", po.postGlow);
        po.postGlowPx     = getF(S, L"post_glow_px", po.postGlowPx);
        po.postGlowDark   = getF(S, L"post_glow_dark", po.postGlowDark);
        po.filmDust       = getF(S, L"film_dust", po.filmDust);
        po.filmHairs      = getF(S, L"film_hairs", po.filmHairs);
        po.filmScratches  = getF(S, L"film_scratches", po.filmScratches);
        po.filmLeak       = getF(S, L"film_leak", po.filmLeak);
        po.filmArtefactRate = getF(S, L"film_artefact_rate", po.filmArtefactRate);
        po.filmNoise      = getF(S, L"film_noise", po.filmNoise);
        po.filmNoiseSize  = getF(S, L"film_noise_size", po.filmNoiseSize);
        po.filmStock      = getF(S, L"film_stock", po.filmStock);
        po.fog            = getF(S, L"fog", po.fog);
        po.fogPx          = getF(S, L"fog_px", po.fogPx);
        po.fogMassGate    = getF(S, L"fog_mass_gate", po.fogMassGate);
        po.artefactLumGate = getF(S, L"artefact_lum_gate", po.artefactLumGate);
        // brief BN
        po.cornerWarp        = getF(S, L"corner_warp", po.cornerWarp);
        po.cornerWarpR       = getF(S, L"corner_warp_r", po.cornerWarpR);
        po.bloomWarmth       = getF(S, L"bloom_warmth", po.bloomWarmth);
        po.glassStreaks      = getF(S, L"glass_streaks", po.glassStreaks);
        po.halationThreshold = getF(S, L"halation_threshold", po.halationThreshold);
        po.bloom          = getF(S, L"bloom", po.bloom);
        po.bloomPx        = getF(S, L"bloom_px", po.bloomPx);
        po.lightX         = getF(S, L"light_x", po.lightX);
        po.lightY         = getF(S, L"light_y", po.lightY);
        po.lightDrift     = getF(S, L"light_drift", po.lightDrift);
        po.lightZ         = getF(S, L"light_z",     po.lightZ);
        po.cameraFov      = getF(S, L"camera_fov", po.cameraFov);
        po.cameraFocus    = getF(S, L"camera_focus", po.cameraFocus);
        po.cameraFieldCurve = getF(S, L"camera_field_curve", po.cameraFieldCurve);
        po.cameraAxisX    = getF(S, L"camera_axis_x", po.cameraAxisX);
        po.cameraAxisY    = getF(S, L"camera_axis_y", po.cameraAxisY);
        po.focusTilt      = getF(S, L"focus_tilt", po.focusTilt);
        po.focusTiltAngle = getF(S, L"focus_tilt_angle", po.focusTiltAngle);
        po.focusBandPx    = getF(S, L"focus_band_px", po.focusBandPx);
        po.focusTiltPeriod= getF(S, L"focus_tilt_period", po.focusTiltPeriod);
        po.focusTiltMoveS = getF(S, L"focus_tilt_move_s", po.focusTiltMoveS);
        po.dofMaxPx       = getF(S, L"dof_max_px", po.dofMaxPx);
        po.psfPx          = getF(S, L"psf_px", po.psfPx);
        po.dither         = getF(S, L"dither", po.dither);
        po.ditherAuto     = getB(S, L"dither_auto", po.ditherAuto);
        po.massBlack      = getF(S, L"mass_black", po.massBlack);
        po.massBlackSoft  = getB(S, L"mass_black_soft", po.massBlackSoft);   // post-night T5
        po.bloomAmbient   = getB(S, L"bloom_ambient", po.bloomAmbient);      // T7
        po.sdrShoulder    = getF(S, L"sdr_shoulder", po.sdrShoulder);
        po.halation       = getF(S, L"halation", po.halation);
        po.halationPx     = getF(S, L"halation_px", po.halationPx);
        po.halationWarmth = getF(S, L"halation_warmth", po.halationWarmth);
        // --- THE LID (task V2) ---
        po.lid            = getF(S, L"lid",              po.lid);
        po.lidGhost       = getF(S, L"lid_ghost",        po.lidGhost);
        po.lidGhostSpread = getF(S, L"lid_ghost_spread", po.lidGhostSpread);
        po.lidRings       = getF(S, L"lid_rings",        po.lidRings);
        po.lidSheen       = getF(S, L"lid_sheen",        po.lidSheen);
        po.lidSheenPx     = getF(S, L"lid_sheen_px",     po.lidSheenPx);
        po.lidGlint       = getF(S, L"lid_glint",        po.lidGlint);
        po.lidIris        = getF(S, L"lid_iris",         po.lidIris);
        po.lidRefractPx   = getF(S, L"lid_refract_px",   po.lidRefractPx);
        po.lidScratch        = getF(S, L"lid_scratch",         po.lidScratch);
        po.lidScratchDensity = getF(S, L"lid_scratch_density", po.lidScratchDensity);
        po.lidScratchLen     = getF(S, L"lid_scratch_len",     po.lidScratchLen);
        po.lidScratchCorner  = getF(S, L"lid_scratch_corner",  po.lidScratchCorner);
        po.lidScratchSoft    = getF(S, L"lid_scratch_soft",    po.lidScratchSoft);
        po.lidScratchTint    = getF(S, L"lid_scratch_tint",    po.lidScratchTint);
        po.shimmer        = getF(S, L"shimmer", po.shimmer);
        po.shimmerPx      = getF(S, L"shimmer_px", po.shimmerPx);
        po.vignetteWander = getF(S, L"vignette_wander", po.vignetteWander);
        po.pixelShiftPx   = getF(S, L"pixel_shift_px", po.pixelShiftPx);
        po.rigReadjust    = getF(S, L"rig_readjust", po.rigReadjust);
    }
    // ---- ink drops ([drops]); usable with ANY look --------------------------
    {
        DropConfig& d = cfg.drops;
        const wchar_t* S = L"drops";
        d.enabled       = getB(S, L"drops", d.enabled);
        d.interval      = getF(S, L"interval", d.interval);
        d.xMin          = getF(S, L"x_min", d.xMin);
        d.xMax          = getF(S, L"x_max", d.xMax);
        d.yMin          = getF(S, L"y_min", d.yMin);
        d.yMax          = getF(S, L"y_max", d.yMax);
        d.speed         = getF(S, L"speed", d.speed);
        d.radius        = getF(S, L"radius", d.radius);
        d.density       = getF(S, L"density", d.density);
        d.tailSec       = getF(S, L"tail_sec", d.tailSec);
        d.tailDensity   = getF(S, L"tail_density", d.tailDensity);
        d.spatter       = getI(S, L"spatter", d.spatter);
        d.spatterRadius = getF(S, L"spatter_radius", d.spatterRadius);
        d.spatterSpeed  = getF(S, L"spatter_speed", d.spatterSpeed);
        d.spatterSpread = getF(S, L"spatter_spread", d.spatterSpread);
        d.colorMode     = getI(S, L"color_mode", d.colorMode);
        d.obeyGovernor  = getB(S, L"obey_governor", d.obeyGovernor);
        d.inBlack       = getF(S, L"in_black", d.inBlack);
        d.inkRise       = getF(S, L"ink_rise", d.inkRise);   // test keys (ag-ink)
        d.pushFilm      = getB(S, L"push_film", d.pushFilm);
        d.impulse       = getF(S, L"impulse", d.impulse);      // test keys (round 3)
        d.inkSwirl      = getF(S, L"ink_swirl", d.inkSwirl);
        d.asymmetry    = getF(S, L"asymmetry", d.asymmetry);
        d.tailRadiusFrac= getF(S, L"tail_radius_frac", d.tailRadiusFrac);
        d.tailSpeed     = getF(S, L"tail_speed", d.tailSpeed);
        d.impulseSpread = getF(S, L"impulse_spread", d.impulseSpread);
        wchar_t buf[64] = {}; float r, g, b;
        GetPrivateProfileStringW(S, L"color", L"", buf, 64, ini);
        if (swscanf_s(buf, L"%f %f %f", &r, &g, &b) == 3) {
            d.color[0] = r; d.color[1] = g; d.color[2] = b;
        }
    }
    {
        LiquidAcidConfig& a = cfg.acid;
        const wchar_t* S = L"liquid_acid";
        a.blobCount    = getI(S, L"blob_count", a.blobCount);
        a.discFrac     = getF(S, L"disc_frac", a.discFrac);
        a.webFrac      = getF(S, L"web_frac", a.webFrac);
        a.bubbleFrac   = getF(S, L"bubble_frac", a.bubbleFrac);
        a.discMin      = getF(S, L"disc_min", a.discMin);
        a.discMax      = getF(S, L"disc_max", a.discMax);
        a.webMin       = getF(S, L"web_min", a.webMin);
        a.webMax       = getF(S, L"web_max", a.webMax);
        a.bubbleMin    = getF(S, L"bubble_min", a.bubbleMin);
        a.bubbleMax    = getF(S, L"bubble_max", a.bubbleMax);
        a.holeMin      = getF(S, L"hole_min", a.holeMin);
        a.holeMax      = getF(S, L"hole_max", a.holeMax);
        a.sizeBias     = getF(S, L"size_bias", a.sizeBias);
        a.bigBias      = getF(S, L"big_bias", a.bigBias);
        a.holeWeight   = getF(S, L"hole_weight", a.holeWeight);
        a.threshold    = getF(S, L"threshold", a.threshold);
        a.supportScale = getF(S, L"support_scale", a.supportScale);
        a.aaScale      = getF(S, L"aa_scale", a.aaScale);
        a.flowGain     = getF(S, L"flow_gain", a.flowGain);
        a.curlDrift    = getF(S, L"curl_drift", a.curlDrift);
        a.repulsion    = getF(S, L"repulsion", a.repulsion);
        a.buoyancy     = getF(S, L"buoyancy", a.buoyancy);
        a.damping      = getF(S, L"damping", a.damping);
        a.breathAmt    = getF(S, L"breath", a.breathAmt);
        a.wrapMargin   = getF(S, L"wrap_margin", a.wrapMargin);
        a.riseSpeed    = getF(S, L"rise_speed", a.riseSpeed);
        a.riseWobble   = getF(S, L"rise_wobble", a.riseWobble);
        a.riseRespawn  = getB(S, L"rise_respawn", a.riseRespawn);
        a.riseStretch  = getF(S, L"rise_stretch", a.riseStretch);
        a.riseBottomLight = getF(S, L"rise_bottom_light", a.riseBottomLight);
        a.riseParallax    = getF(S, L"rise_parallax", a.riseParallax);
        a.riseParallaxDim = getF(S, L"rise_parallax_dim", a.riseParallaxDim);
        a.oilDrag      = getF(S, L"oil_drag", a.oilDrag);
        a.oilDyeBlock  = getF(S, L"oil_dye_block", a.oilDyeBlock);
        a.oilViscosity = getF(S, L"oil_viscosity", a.oilViscosity);
        a.mouseOilMode = getI(S, L"mouse_oil_mode", a.mouseOilMode);
        a.mouseOilRadius = getF(S, L"mouse_oil_radius", a.mouseOilRadius);
        a.mouseOilGain   = getF(S, L"mouse_oil_gain", a.mouseOilGain);
        a.rimWidth     = getF(S, L"rim_width", a.rimWidth);
        a.rimInset     = getF(S, L"rim_inset", a.rimInset);
        a.rimDark      = getF(S, L"rim_dark", a.rimDark);
        a.rimVary      = getF(S, L"rim_vary", a.rimVary);
        a.rimInkFollow = getF(S, L"rim_ink_follow", a.rimInkFollow);
        a.rimOrder     = getB(S, L"rim_order", a.rimOrder);
        a.meniscus     = getF(S, L"meniscus", a.meniscus);
        a.meniscusW    = getF(S, L"meniscus_width", a.meniscusW);
        a.meniscusOff  = getF(S, L"meniscus_offset", a.meniscusOff);
        a.refraction   = getF(S, L"refraction", a.refraction);
        a.translucency = getF(S, L"translucency", a.translucency);
        a.oilTexture   = getF(S, L"oil_texture", a.oilTexture);
        a.inkShading   = getF(S, L"ink_shading", a.inkShading);
        a.oilHdr       = getF(S, L"oil_hdr", a.oilHdr);
        a.rimHdr       = getF(S, L"rim_hdr", a.rimHdr);
        a.oilThinEdge  = getF(S, L"oil_thin_edge", a.oilThinEdge);
        a.oilEdgeFrac  = getF(S, L"oil_edge_frac", a.oilEdgeFrac);
        a.oilSpecular  = getF(S, L"oil_specular", a.oilSpecular);
        a.oilIrid      = getF(S, L"oil_iridescence", a.oilIrid);
        a.swarmLens    = getF(S, L"swarm_lens", a.swarmLens);
        a.meniscusFromInk = getF(S, L"meniscus_from_ink", a.meniscusFromInk);
        a.meniscusFilmMix = getF(S, L"meniscus_film_mix", a.meniscusFilmMix);
        a.oilGlow      = getF(S, L"oil_glow", a.oilGlow);
        a.refractionWidth = getF(S, L"refraction_width", a.refractionWidth);
        a.oilTransparency = getF(S, L"oil_transparency", a.oilTransparency);
        a.oilAbsorb    = getF(S, L"oil_absorb", a.oilAbsorb);
        a.oilFilmBump  = getF(S, L"oil_film_bump", a.oilFilmBump);
        a.oilRefractBody = getF(S, L"oil_refract_body", a.oilRefractBody);
        a.oilInkBlur   = getF(S, L"oil_ink_blur", a.oilInkBlur);
        a.inkLevels    = getF(S, L"ink_levels", a.inkLevels);
        a.inkSoft      = getF(S, L"ink_soft", a.inkSoft);
        a.inkMix       = getF(S, L"ink_mix", a.inkMix);
        a.inkHueVary   = getF(S, L"ink_hue_vary", a.inkHueVary);
        a.inkComplementLock = getB(S, L"ink_complement_lock", a.inkComplementLock);
        a.inkComplementSpan = getF(S, L"ink_complement_span", a.inkComplementSpan);
        a.hueSweepPeriod    = getF(S, L"hue_sweep_period", a.hueSweepPeriod);
        a.hueRotatePeriod   = getF(S, L"hue_rotate_period", a.hueRotatePeriod);
        a.hueAnchorWeight   = getF(S, L"hue_anchor_weight", a.hueAnchorWeight);
        a.paletteStartHue   = getF(S, L"palette_start_hue", a.paletteStartHue);
        a.oilSaturation     = getF(S, L"oil_saturation", a.oilSaturation);
        a.sweepCount        = getI(S, L"sweep_count", a.sweepCount);
        if (a.sweepCount < 1) a.sweepCount = 1;
        if (a.sweepCount > LiquidAcidConfig::kSweepMax)
            a.sweepCount = LiquidAcidConfig::kSweepMax;
        a.droplets     = getI(S, L"droplets", a.droplets);
        a.dropletSpawn = getF(S, L"droplet_spawn_rate", a.dropletSpawn);
        a.dropletRMin  = getF(S, L"droplet_r_min", a.dropletRMin);
        a.dropletRMax  = getF(S, L"droplet_r_max", a.dropletRMax);
        a.dropletBias  = getF(S, L"droplet_bias", a.dropletBias);
        a.dropletSupport = getF(S, L"droplet_support", a.dropletSupport);
        a.dropletWeight  = getF(S, L"droplet_weight", a.dropletWeight);
        a.dropletOilW    = getF(S, L"droplet_oil_weight", a.dropletOilW);
        a.dropletInkFrac = getF(S, L"droplet_ink_frac", a.dropletInkFrac);
        a.dropletLife    = getF(S, L"droplet_life", a.dropletLife);
        a.dropletDamping = getF(S, L"droplet_damping", a.dropletDamping);
        a.dropletJitter  = getF(S, L"droplet_jitter", a.dropletJitter);
        a.dropletAttract = getF(S, L"droplet_attract", a.dropletAttract);
        a.dropletMerge   = getF(S, L"droplet_merge", a.dropletMerge);
        a.dropletRise    = getF(S, L"droplet_rise", a.dropletRise);
        a.accentMode     = getI(S, L"accent_mode", a.accentMode);
        a.accentMaxR     = getF(S, L"accent_max_r", a.accentMaxR);
        a.accentFrac     = getF(S, L"accent_frac", a.accentFrac);
        a.oilEdgeMode    = getI(S, L"oil_edge_mode", a.oilEdgeMode);
        a.oilEdgeCurve   = getF(S, L"oil_edge_curve", a.oilEdgeCurve);
        a.oilPenumbra    = getF(S, L"oil_penumbra", a.oilPenumbra);
        a.oilPenumbraPx  = getF(S, L"oil_penumbra_px", a.oilPenumbraPx);
        a.oilPenumbraHue = getF(S, L"oil_penumbra_hue", a.oilPenumbraHue);
        a.oilPenumbraDark= getF(S, L"oil_penumbra_dark", a.oilPenumbraDark);
        a.cellulose      = getF(S, L"cellulose", a.cellulose);
        a.celluloseInk   = getF(S, L"cellulose_ink", a.celluloseInk);
        a.celluloseOil   = getF(S, L"cellulose_oil", a.celluloseOil);
        a.celluloseScale = getF(S, L"cellulose_scale", a.celluloseScale);
        a.celluloseDrift = getF(S, L"cellulose_drift", a.celluloseDrift);
        a.dropletRingFrac  = getF(S, L"droplet_ring_frac", a.dropletRingFrac);
        a.dropletRingWidth = getF(S, L"droplet_ring_width", a.dropletRingWidth);
        a.dropletRingLift  = getF(S, L"droplet_ring_lift", a.dropletRingLift);
        a.dropletRingClump = getF(S, L"droplet_ring_clump", a.dropletRingClump);
        // --- conservation of mass (brief S) ---
        a.weather            = getF(S, L"weather",          a.weather);
        a.weatherPeriodS     = getF(S, L"weather_period_s", a.weatherPeriodS);
        a.filmHue2           = getF(S, L"film_hue2",        a.filmHue2);
        a.filmHue2Amt        = getF(S, L"film_hue2_amt",    a.filmHue2Amt);
        a.filmHue2Scale      = getF(S, L"film_hue2_scale",  a.filmHue2Scale);
        a.filmHue2Drift      = getF(S, L"film_hue2_drift",  a.filmHue2Drift);
        a.filmHue2Decay      = getF(S, L"film_hue2_decay",  a.filmHue2Decay);
        a.filmHue3           = getF(S, L"film_hue3",        a.filmHue3);
        a.filmHue3Amt        = getF(S, L"film_hue3_amt",    a.filmHue3Amt);
        a.filmHue3Share      = getF(S, L"film_hue3_share",  a.filmHue3Share);
        a.filmHue2Cover      = getF(S, L"film_hue2_cover",  a.filmHue2Cover);
        a.filmEqualLoad      = getF(S, L"film_equal_load",  a.filmEqualLoad);
        a.filmEqualLoadP     = getF(S, L"film_equal_load_patches", a.filmEqualLoadP);
        a.filmHue2Seam       = getF(S, L"film_hue2_seam",   a.filmHue2Seam);
        a.filmHue2SeamStretch = getF(S, L"film_hue2_seam_stretch", a.filmHue2SeamStretch);   // T8b
        a.filmHue2Leak       = getF(S, L"film_hue2_leak",   a.filmHue2Leak);
        a.filmHue2LeakReach  = getF(S, L"film_hue2_leak_reach", a.filmHue2LeakReach);
        a.filmHue2LeakCore   = getF(S, L"film_hue2_leak_core", a.filmHue2LeakCore);
        a.filmHue2LeakIn     = getF(S, L"film_hue2_leak_in", a.filmHue2LeakIn);
        a.crustHueMix        = getF(S, L"crust_hue_mix",    a.crustHueMix);
        a.filmHue2Wobble     = getF(S, L"film_hue2_wobble",        a.filmHue2Wobble);
        a.filmHue2SeedRows   = getF(S, L"film_hue2_seed_rows",     a.filmHue2SeedRows);
        a.filmHue2Rise       = getF(S, L"film_hue2_rise",          a.filmHue2Rise);
        a.filmHue2Refresh    = getF(S, L"film_hue2_refresh",       a.filmHue2Refresh);
        a.filmHue2Lobes      = getF(S, L"film_hue2_lobes",         a.filmHue2Lobes);   // T8
        a.filmHue2WobbleP    = getF(S, L"film_hue2_wobble_period", a.filmHue2WobbleP);
        a.boundaryReflectR   = getF(S, L"boundary_reflect_r",      a.boundaryReflectR);
        a.boundaryReflectAmt = getF(S, L"boundary_reflect_amt",    a.boundaryReflectAmt);
        a.dropletMassBias    = getF(S, L"droplet_mass_bias",     a.dropletMassBias);
        a.dropletCrustDens   = getF(S, L"droplet_crust_density", a.dropletCrustDens);
        a.dropletCrustR      = getF(S, L"droplet_crust_r",       a.dropletCrustR);
        a.massRim            = getF(S, L"mass_rim",              a.massRim);
        a.shadowAmt          = getF(S, L"shadow_amt",            a.shadowAmt);
        a.shadowLen          = getF(S, L"shadow_len",            a.shadowLen);
        a.shadowSoft         = getF(S, L"shadow_soft",           a.shadowSoft);
        a.dropletRacerFrac   = getF(S, L"droplet_racer_frac",   a.dropletRacerFrac);
        a.dropletRacerSpeed  = getF(S, L"droplet_racer_speed",  a.dropletRacerSpeed);
        a.dropletRacerWobble = getF(S, L"droplet_racer_wobble", a.dropletRacerWobble);
        a.dropletRacerRMax   = getF(S, L"droplet_racer_r_max",  a.dropletRacerRMax);
        a.dropletRingRMul    = getF(S, L"droplet_ring_r_mul",    a.dropletRingRMul);
        a.dropletRingBigFrac = getF(S, L"droplet_ring_big_frac", a.dropletRingBigFrac);
        a.dropletBigFrac     = getF(S, L"droplet_big_frac",      a.dropletBigFrac);
        a.dropletBigRMul     = getF(S, L"droplet_big_r_mul",     a.dropletBigRMul);
        a.dropletCoalesce  = getF(S, L"droplet_coalesce",   a.dropletCoalesce);
        a.dropletCoalesceS = getF(S, L"droplet_coalesce_s", a.dropletCoalesceS);
        a.conserveMass     = getF(S, L"conserve_mass",  a.conserveMass);
        a.spawnGrowS       = getF(S, L"spawn_grow_s",   a.spawnGrowS);
        a.dissolveS        = getF(S, L"dissolve_s",     a.dissolveS);
        a.blobTurnoverS    = getF(S, L"blob_turnover_s", a.blobTurnoverS);   // brief BZ
        a.dropletRingWobble= getF(S, L"droplet_ring_wobble", a.dropletRingWobble);
        a.dropletDepth     = getF(S, L"droplet_depth", a.dropletDepth);
        a.depthRise        = getF(S, L"depth_rise", a.depthRise);
        a.depthAmount      = getF(S, L"depth_amount", a.depthAmount);
        a.diffraction      = getF(S, L"diffraction", a.diffraction);
        a.diffractionPx    = getF(S, L"diffraction_px", a.diffractionPx);
        a.dropletLens      = getF(S, L"droplet_lens", a.dropletLens);
        a.dropletLensCentre= getF(S, L"droplet_lens_centre", a.dropletLensCentre);
        a.dropletLensBand  = getF(S, L"droplet_lens_band", a.dropletLensBand);
        a.dropletSpec      = getF(S, L"droplet_spec", a.dropletSpec);
        a.dropletShadow    = getF(S, L"droplet_shadow", a.dropletShadow);
        a.dropletWobble    = getF(S, L"droplet_wobble", a.dropletWobble);
        a.dropletCore      = getF(S, L"droplet_core", a.dropletCore);
        a.dropletShadowLamp = getI(S, L"droplet_shadow_lamp", a.dropletShadowLamp);   // post-night T4
        a.dropletHazeRim   = getF(S, L"droplet_haze_rim", a.dropletHazeRim);
        a.oilSpecTint      = getI(S, L"oil_spec_tint", a.oilSpecTint);       // T7
        a.dyeDepth         = getF(S, L"dye_depth", a.dyeDepth);
        a.dyeDepthTilt     = getF(S, L"dye_depth_tilt", a.dyeDepthTilt);
        a.dyeDepthW        = getF(S, L"dye_depth_w", a.dyeDepthW);
        a.dyeHue           = getF(S, L"dye_hue", a.dyeHue);
        a.dyeSat           = getF(S, L"dye_sat", a.dyeSat);
        a.dyeLum           = getF(S, L"dye_lum", a.dyeLum);
        a.dyeHueFollow     = getI(S, L"dye_hue_follow", a.dyeHueFollow);
        a.dyeDroplets      = getF(S, L"dye_droplets", a.dyeDroplets);
        a.dyeMasses        = getF(S, L"dye_masses", a.dyeMasses);
        a.dyeDropHue       = getF(S, L"dye_droplet_hue", a.dyeDropHue);
        a.dyeDropSat       = getF(S, L"dye_droplet_sat", a.dyeDropSat);
        a.dyeDropLum       = getF(S, L"dye_droplet_lum", a.dyeDropLum);
        a.dyeSmoke         = getF(S, L"dye_smoke", a.dyeSmoke);
        a.filmLevel        = getF(S, L"film_level", a.filmLevel);
        a.oilFluor         = getF(S, L"oil_fluor", a.oilFluor);
        a.oilFluorReach    = getF(S, L"oil_fluor_reach", a.oilFluorReach);
        a.dyeLampFollow    = getF(S, L"dye_lamp_follow", a.dyeLampFollow);
        a.darkSat          = getF(S, L"dark_sat", a.darkSat);
        a.dyeLumVary       = getF(S, L"dye_lum_vary", a.dyeLumVary);
        a.dyeHueVary       = getF(S, L"dye_hue_vary", a.dyeHueVary);
        a.dyeThickHue      = getF(S, L"dye_thick_hue", a.dyeThickHue);
        a.dyeCore          = getF(S, L"dye_core", a.dyeCore);
        a.oilHeight        = getI(S, L"oil_height", a.oilHeight);          // Oil3D Stage A
        a.oilHeightPx      = getF(S, L"oil_height_px", a.oilHeightPx);
        a.oil3dAbsorb      = getF(S, L"oil3d_absorb", a.oil3dAbsorb);      // Oil3D Stage B
        a.tankDepth        = getF(S, L"tank_depth", a.tankDepth);           // Oil3D C1
        a.lightElev        = getF(S, L"light_elev", a.lightElev);
        a.lightAzim        = getF(S, L"light_azim", a.lightAzim);
        a.lightAzimDrift   = getF(S, L"light_azim_drift", a.lightAzimDrift);
        a.lightSize        = getF(S, L"light_size", a.lightSize);
        a.shadowOnFilm     = getF(S, L"shadow_on_film", a.shadowOnFilm);
        a.waterScatter     = getF(S, L"water_scatter", a.waterScatter);     // Oil3D C2
        a.waterSigma       = getF(S, L"water_sigma", a.waterSigma);
        a.waterMurk        = getF(S, L"water_murk", a.waterMurk);           // Oil3D W1 (murk)
        a.murkClouds       = getF(S, L"murk_clouds", a.murkClouds);
        a.murkWaves        = getF(S, L"murk_waves", a.murkWaves);
        a.murkStrands      = getF(S, L"murk_strands", a.murkStrands);
        a.murkClumps       = getF(S, L"murk_clumps", a.murkClumps);
        a.murkShape        = getF(S, L"murk_shape", a.murkShape);
        a.murkWarp         = getF(S, L"murk_warp", a.murkWarp);
        a.murkScale        = getF(S, L"murk_scale", a.murkScale);
        a.murkDrift        = getF(S, L"murk_drift", a.murkDrift);
        a.murkShadow       = getF(S, L"murk_shadow", a.murkShadow);
        a.murkScatter      = getF(S, L"murk_scatter", a.murkScatter);
        a.murkLight        = getF(S, L"murk_light", a.murkLight);
        a.murkGrain        = getF(S, L"murk_grain", a.murkGrain);             // Oil3D W3 (fine silt + haze)
        a.murkGrainPx      = getF(S, L"murk_grain_px", a.murkGrainPx);
        a.murkGrainSlices  = getI(S, L"murk_grain_slices", a.murkGrainSlices);
        a.murkHaze         = getF(S, L"murk_haze", a.murkHaze);
        a.murkHazeLift     = getF(S, L"murk_haze_lift", a.murkHazeLift);
        a.murkCloudShare   = getF(S, L"murk_cloud_share", a.murkCloudShare);
        a.motes            = getI(S, L"motes", a.motes);                      // Oil3D C3
        a.motePx           = getF(S, L"mote_px", a.motePx);
        a.moteBright       = getF(S, L"mote_bright", a.moteBright);
        a.moteDepthBias    = getF(S, L"mote_depth_bias", a.moteDepthBias);
        a.moteGlint        = getF(S, L"mote_glint", a.moteGlint);
        a.waterTint        = getF(S, L"water_tint", a.waterTint);           // Oil3D W2 (water colour)
        a.waterHue         = getF(S, L"water_hue", a.waterHue);
        a.siltTint         = getF(S, L"silt_tint", a.siltTint);
        a.siltHue          = getF(S, L"silt_hue", a.siltHue);
        a.colourDirector   = getB(S, L"colour_director", a.colourDirector);   // Oil3D L4
        a.directorRest     = getF(S, L"director_rest", a.directorRest);
        a.directorTrip     = getF(S, L"director_trip", a.directorTrip);
        a.directorMix      = getF(S, L"director_mix", a.directorMix);
        a.directorDemo     = getI(S, L"director_demo", a.directorDemo);
        a.directorStyle    = getI(S, L"director_style", a.directorStyle);
        a.directorHold     = getF(S, L"director_hold", a.directorHold);   // night-cadence
        a.directorMove     = getF(S, L"director_move", a.directorMove);
        a.directorTurn     = getF(S, L"director_turn", a.directorTurn);
        a.directorWater    = getB(S, L"director_water", a.directorWater);   // post-night T3
        a.directorTurnDark = getI(S, L"director_turn_dark", a.directorTurnDark);   // post-night T5
        a.directorBandLo   = getF(S, L"director_band_lo", a.directorBandLo);       // T8 band
        a.directorBandHi   = getF(S, L"director_band_hi", a.directorBandHi);
        a.directorPourK    = getF(S, L"director_pour_k", a.directorPourK);         // T8 blob
        a.directorAccent   = getF(S, L"director_accent", a.directorAccent);        // T8 accent
        a.directorPourRamp = getF(S, L"director_pour_ramp", a.directorPourRamp);   // T8 pour
        a.directorDarkShare = getF(S, L"director_dark_share", a.directorDarkShare);   // T8 order
        a.directorDarkPath = getI(S, L"director_dark_path", a.directorDarkPath);      // T8 direct darkroom
        DirectorLoadLooks(ini);   // T8 looks: [looks] ratings, app-level (a file without the section changes nothing)
        // (shaft_hdr: dropped in Oil3D L1b; a leftover line is simply not read)
        // Oil3D colour stage L1: lamp 1 as a spectral window + the film dyes
        a.lamp1Lo          = getF(S, L"lamp1_lo", a.lamp1Lo);
        a.lamp1Hi          = getF(S, L"lamp1_hi", a.lamp1Hi);
        a.lamp1Lo2         = getF(S, L"lamp1_lo2", a.lamp1Lo2);
        a.lamp1Hi2         = getF(S, L"lamp1_hi2", a.lamp1Hi2);
        a.lamp1Feather     = getF(S, L"lamp1_feather", a.lamp1Feather);
        a.lamp1Level       = getF(S, L"lamp1_level", a.lamp1Level);
        // Oil3D L2: lamp 2, a second window from its own direction (level 0 = off)
        a.lamp2Lo          = getF(S, L"lamp2_lo", a.lamp2Lo);
        a.lamp2Hi          = getF(S, L"lamp2_hi", a.lamp2Hi);
        a.lamp2Lo2         = getF(S, L"lamp2_lo2", a.lamp2Lo2);
        a.lamp2Hi2         = getF(S, L"lamp2_hi2", a.lamp2Hi2);
        a.lamp2Feather     = getF(S, L"lamp2_feather", a.lamp2Feather);
        a.lamp2Level       = getF(S, L"lamp2_level", a.lamp2Level);
        a.lamp2Elev        = getF(S, L"lamp2_elev", a.lamp2Elev);
        a.lamp2Azim        = getF(S, L"lamp2_azim", a.lamp2Azim);
        a.lamp2Size        = getF(S, L"lamp2_size", a.lamp2Size);
        a.dyeA2Amount      = getF(S, L"dye_a2_amount", a.dyeA2Amount);
        a.dyeB1Amount      = getF(S, L"dye_b1_amount", a.dyeB1Amount);
        a.dyeB2Amount      = getF(S, L"dye_b2_amount", a.dyeB2Amount);
        a.oilDye           = getF(S, L"oil_dye", a.oilDye);              // Oil3D L3: the dark-dye oil
        a.oilDyeAmount     = getF(S, L"oil_dye_amount", a.oilDyeAmount);
        {
            // names are kept for the round trip; absent keys keep the live dye
            wchar_t* const names[3] = { a.dyeA2Name, a.dyeB1Name, a.dyeB2Name };
            const wchar_t* const keys[3] = { L"dye_a2", L"dye_b1", L"dye_b2" };
            for (int d = 0; d < 3; d++) {
                wchar_t buf[64] = {};
                GetPrivateProfileStringW(S, keys[d], L"\x01", buf, 64, ini);
                if (buf[0] == L'\x01') continue;
                if (wchar_t* sc = wcspbrk(buf, L";#")) *sc = 0;   // inline comment
                size_t n = wcslen(buf);
                while (n > 0 && iswspace(buf[n - 1])) buf[--n] = 0;
                wcsncpy_s(names[d], 32, buf, _TRUNCATE);
            }
            float h, sg, k;
            ResolveDye(L"self", h, sg, k);
            a.filmDyeHue[0] = 0.0f; a.filmDyeSigma[0] = k > 0.0f ? sg : 55.0f; a.filmDyeK[0] = k > 0.0f ? k : kDyeK;
            for (int d = 0; d < 3; d++) {
                ResolveDye(names[d], h, sg, k);
                a.filmDyeHue[d + 1] = h; a.filmDyeSigma[d + 1] = sg; a.filmDyeK[d + 1] = k;
            }
        }
        a.lampGrey         = getF(S, L"lamp_grey", a.lampGrey);
        a.lampGreySize     = getF(S, L"lamp_grey_size", a.lampGreySize);
        a.lampGreyCool     = getF(S, L"lamp_grey_cool", a.lampGreyCool);
        a.lampGreyHeart     = getF(S, L"lamp_grey_heart", a.lampGreyHeart);
        a.lampGreyHeartSize = getF(S, L"lamp_grey_heart_size", a.lampGreyHeartSize);
        a.lampGreyHeartLift = getF(S, L"lamp_grey_heart_lift", a.lampGreyHeartLift);
        a.shadowTone       = getF(S, L"shadow_tone", a.shadowTone);
        a.shadowToneLift   = getF(S, L"shadow_tone_lift", a.shadowToneLift);
        a.shadowToneSat    = getF(S, L"shadow_tone_sat", a.shadowToneSat);
        a.shadowTonePeriod = getF(S, L"shadow_tone_period", a.shadowTonePeriod);
        a.shadowToneFade   = getF(S, L"shadow_tone_fade", a.shadowToneFade);
        a.shadowToneHue    = getF(S, L"shadow_tone_hue", a.shadowToneHue);
        a.highlightToneHue   = getF(S, L"highlight_tone_hue", a.highlightToneHue);
        a.highlightToneAmt   = getF(S, L"highlight_tone_amt", a.highlightToneAmt);
        a.highlightToneSat   = getF(S, L"highlight_tone_sat", a.highlightToneSat);
        a.highlightToneDesat = getF(S, L"highlight_tone_desat", a.highlightToneDesat);
        a.toneBalance        = getF(S, L"tone_balance", a.toneBalance);
        a.postChroma   = getF(S, L"post_chroma", a.postChroma);
        a.postLift     = getF(S, L"post_lift", a.postLift);
        {   // ink_mode = bands | water (string wins); int form ink_water=0|1
            wchar_t mode[32] = {};
            GetPrivateProfileStringW(S, L"ink_mode", L"", mode, 32, ini);
            if (mode[0]) a.inkMode = (_wcsicmp(mode, L"water") == 0) ? 1 : 0;
            a.inkMode = getB(S, L"ink_water", a.inkMode != 0) ? 1 : 0;
        }
        a.inkGain      = getF(S, L"ink_gain", a.inkGain);
        a.inkBias      = getF(S, L"ink_bias", a.inkBias);
        a.seamStrength = getF(S, L"seam_strength", a.seamStrength);
        a.seamLo       = getF(S, L"seam_lo", a.seamLo);
        a.seamHi       = getF(S, L"seam_hi", a.seamHi);
        a.seamScale    = getF(S, L"seam_scale", a.seamScale);
        a.grainAmt     = getF(S, L"grain", a.grainAmt);
        a.grainScale   = getF(S, L"grain_scale", a.grainScale);
        a.grainShadowW = getF(S, L"grain_shadow_weight", a.grainShadowW);
        a.toeTint      = getF(S, L"toe_tint", a.toeTint);
        a.speckle      = getF(S, L"speckle", a.speckle);
        a.speckScale   = getF(S, L"speckle_scale", a.speckScale);
        a.swarmHoles   = getF(S, L"swarm_holes", a.swarmHoles);
        a.swarmDrops   = getF(S, L"swarm_drops", a.swarmDrops);
        a.swarmDensity = getF(S, L"swarm_density", a.swarmDensity);
        a.swarmScaleA  = getF(S, L"swarm_scale_holes", a.swarmScaleA);
        a.swarmScaleB  = getF(S, L"swarm_scale_drops", a.swarmScaleB);
        a.swarmRMin    = getF(S, L"swarm_r_min", a.swarmRMin);
        a.swarmRMax    = getF(S, L"swarm_r_max", a.swarmRMax);
        a.swarmRimDark = getF(S, L"swarm_rim_dark", a.swarmRimDark);
        a.swarmDrift   = getF(S, L"swarm_drift", a.swarmDrift);
        a.swarmClump   = getF(S, L"swarm_clump", a.swarmClump);
        a.swarmDark    = getF(S, L"swarm_dark", a.swarmDark);
        {   // meniscus halo colour
            wchar_t buf[64] = {}; float r, g, b;
            GetPrivateProfileStringW(S, L"meniscus_color", L"", buf, 64, ini);
            if (swscanf_s(buf, L"%f %f %f", &r, &g, &b) == 3) {
                a.meniscusCol[0] = r; a.meniscusCol[1] = g; a.meniscusCol[2] = b;
            }
        }
        // The curated vivid list. Two spellings per entry, both accepted:
        //   sweep_pair_N_oil / sweep_pair_N_ink   (the shipped names)
        //   sweep_oil_N      / sweep_ink_N        (the short form, N up to 12)
        // An OIL value may be three floats (one anchor; the other three oil
        // shades are derived from it as before) or TWELVE (the four shades
        // verbatim -- how the tile9 palettes are carried without being
        // re-derived and toned down).
        for (int ci = 0; ci < LiquidAcidConfig::kSweepMax; ci++) {
            wchar_t key[40], buf[160] = {};
            float f[12];
            auto readKey = [&](const wchar_t* fmt) {
                swprintf_s(key, fmt, ci + 1);
                buf[0] = 0;
                GetPrivateProfileStringW(S, key, L"", buf, 160, ini);
                return (int)wcslen(buf);
            };
            if (!readKey(L"sweep_pair_%d_oil")) readKey(L"sweep_oil_%d");
            int n = swscanf_s(buf, L"%f %f %f %f %f %f %f %f %f %f %f %f",
                              &f[0], &f[1], &f[2], &f[3], &f[4], &f[5],
                              &f[6], &f[7], &f[8], &f[9], &f[10], &f[11]);
            if (n >= 3) {
                for (int k = 0; k < 3; k++) a.sweepOil[ci * 3 + k] = f[k];
                a.sweepOilFullSet[ci] = (n >= 12);
                if (n >= 12)
                    for (int k = 0; k < 12; k++) a.sweepOilFull[ci * 12 + k] = f[k];
            }
            if (!readKey(L"sweep_pair_%d_ink")) readKey(L"sweep_ink_%d");
            if (swscanf_s(buf, L"%f %f %f", &f[0], &f[1], &f[2]) == 3)
                for (int k = 0; k < 3; k++) a.sweepInk[ci * 3 + k] = f[k];
        }
        // oil_color_1..4 and ink_stop_1..4, "r g b" floats like splat_color_N
        for (int ci = 0; ci < 4; ci++) {
            wchar_t key[32], buf[64] = {};
            float r, g, b;
            swprintf_s(key, L"oil_color_%d", ci + 1);
            GetPrivateProfileStringW(S, key, L"", buf, 64, ini);
            if (swscanf_s(buf, L"%f %f %f", &r, &g, &b) == 3) {
                a.oilColors[ci * 3 + 0] = r; a.oilColors[ci * 3 + 1] = g; a.oilColors[ci * 3 + 2] = b;
            }
            buf[0] = 0;
            swprintf_s(key, L"ink_stop_%d", ci + 1);
            GetPrivateProfileStringW(S, key, L"", buf, 64, ini);
            if (swscanf_s(buf, L"%f %f %f", &r, &g, &b) == 3) {
                a.inkRamp[ci * 3 + 0] = r; a.inkRamp[ci * 3 + 1] = g; a.inkRamp[ci * 3 + 2] = b;
            }
        }
    }
    for (int ci = 0; ci < 5; ci++) {
        wchar_t key[32], buf[64] = {};
        swprintf_s(key, L"splat_color_%d", ci + 1);
        GetPrivateProfileStringW(L"color", key, L"", buf, 64, ini);
        float r, g, b;
        if (swscanf_s(buf, L"%f %f %f", &r, &g, &b) == 3) {
            cfg.splatColors[ci * 3 + 0] = r;
            cfg.splatColors[ci * 3 + 1] = g;
            cfg.splatColors[ci * 3 + 2] = b;
        }
    }
    printf("config loaded: density=%.3f decay_fast=%.2f vorticity=%.0f sim=%d dye=%d look=%s\n",
           cfg.densityDissipation, cfg.decayFast, cfg.curl, cfg.simRes, cfg.dyeRes,
           cfg.ink.enabled ? (cfg.ink.inverted ? "ink (inverted)" : "ink (paper)")
                           : (cfg.acid.enabled
                              ? (cfg.acid.inkMode == 1 ? "liquid_acid (ink_mode=water)"
                                                       : "liquid_acid")
                              : "fluid"));
}

static void LoadFullConfig(FluidConfig& cfg) { LoadConfigFromIni(g_configIniPath, cfg); }

void LoadConfigFromFile(const wchar_t* ini, FluidConfig& cfg) { LoadConfigFromIni(ini, cfg); }

// ---------------------------------------------------------------------------
// WorkerW: the layer behind the desktop icons
// ---------------------------------------------------------------------------

static HWND FindWallpaperHost() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) return nullptr;

    DWORD_PTR result = 0;
    SendMessageTimeoutW(progman, 0x052C, 0xD, 0x1, SMTO_NORMAL, 1000, &result);
    SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, &result);

    struct Ctx { HWND wallpaper = nullptr; } ctx;
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        if (FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr)) {
            c->wallpaper = FindWindowExW(nullptr, hwnd, L"WorkerW", nullptr);
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.wallpaper) {
        printf("WorkerW layout: classic (sibling WorkerW)\n");
        return ctx.wallpaper;
    }

    if (FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr)) {
        HWND workerw = FindWindowExW(progman, nullptr, L"WorkerW", nullptr);
        if (workerw) {
            printf("WorkerW layout: Win11 24H2 (WorkerW child of Progman)\n");
            return workerw;
        }
        printf("WorkerW layout: 24H2 without WorkerW child, using Progman\n");
        return progman;
    }

    printf("WorkerW layout: unknown, falling back to Progman\n");
    return progman;
}

static bool g_shuttingDown = false;
static bool g_wallpaperLost = false;
static bool g_destroyingMirror = false;   // intentional teardown, not shell loss
static HMONITOR g_monitor2 = nullptr;
static RECT g_monitor2Rect = {};

static BOOL CALLBACK FindSecondMonitor(HMONITOR mon, HDC, LPRECT rc, LPARAM) {
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    if (!(mi.dwFlags & MONITORINFOF_PRIMARY)) {
        g_monitor2 = mon;
        g_monitor2Rect = *rc;
    }
    return TRUE;
}

static LRESULT CALLBACK WallpaperWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE:   DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        if (g_shuttingDown) {
            PostQuitMessage(0);
        } else if (g_destroyingMirror) {
            // deliberate mirror teardown — not a shell restart
        } else {
            // Explorer restart tore down the WorkerW hierarchy (and us with
            // it). Don't quit — the main loop re-hooks into the new shell.
            printf("wallpaper window destroyed externally (Explorer restart?) - will reattach\n");
            g_wallpaperLost = true;
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND CreateWallpaperWindowAt(HWND host, int screenX, int screenY,
                                    int width, int height) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WallpaperWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FluidWallpaperWnd";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);   // fails harmlessly after the first call

    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        wc.lpszClassName, L"Fluid Wallpaper",
        WS_POPUP, 0, 0, width, height,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) Fail("CreateWindowExW", HRESULT_FROM_WIN32(GetLastError()));
    for (HWND& w : g_wallpaperWnds)              // remembered for Fail(); dead slots are reused
        if (!w || !IsWindow(w)) { w = hwnd; break; }

    SetParent(hwnd, host);
    POINT origin = { screenX, screenY };
    ScreenToClient(host, &origin);
    SetWindowPos(hwnd, HWND_TOP, origin.x, origin.y, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return hwnd;
}

static HWND CreateWallpaperWindow(HWND host, int width, int height) {
    return CreateWallpaperWindowAt(host, 0, 0, width, height);
}

// ---------------------------------------------------------------------------
// Fullscreen detection
// ---------------------------------------------------------------------------

static bool IsShellOrOwnWindow(HWND hwnd) {
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    return wcscmp(cls, L"Progman") == 0 ||
           wcscmp(cls, L"WorkerW") == 0 ||
           wcscmp(cls, L"Shell_TrayWnd") == 0 ||
           wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
           wcscmp(cls, L"FluidWallpaperWnd") == 0 ||
           wcscmp(cls, L"FluidWallpaperTray") == 0 ||
           wcscmp(cls, L"XamlExplorerHostIslandWindow") == 0 ||
           wcscmp(cls, L"Windows.UI.Core.CoreWindow") == 0;
}

static bool MonitorIsOurs(HMONITOR m) {
    if (m == g_monitor) return true;
    return g_monitor2 && m == g_monitor2 &&
           g_renderer && g_renderer->MirrorActive();
}

static bool FullscreenAppActive() {
    HWND fg = GetForegroundWindow();
    if (!fg || IsShellOrOwnWindow(fg)) return false;
    HMONITOR fgMon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    if (!MonitorIsOurs(fgMon)) return false;

    // Maximized windows are not fullscreen — including borderless
    // custom-titlebar apps (Electron, Windows Terminal); pause_on_maximized
    // owns those. Fullscreen is any NON-maximized window whose CLIENT area
    // covers the whole monitor. The window STYLE is deliberately not consulted:
    // "windowed fullscreen" games (DRAGON BALL FighterZ, 2026-09-18: a
    // WS_CAPTION window at -8,-31..2568,1448 with its frame hanging off-screen)
    // keep a caption style and were never detected when the style was tested.
    if (IsZoomed(fg)) return false;

    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(fgMon, &mi)) return false;
    RECT c;
    if (!GetClientRect(fg, &c)) return false;
    POINT tl = { c.left, c.top }, br = { c.right, c.bottom };
    if (!ClientToScreen(fg, &tl) || !ClientToScreen(fg, &br)) return false;
    return tl.x <= mi.rcMonitor.left && tl.y <= mi.rcMonitor.top &&
           br.x >= mi.rcMonitor.right && br.y >= mi.rcMonitor.bottom;
}

static bool MaximizedAppActive() {
    HWND fg = GetForegroundWindow();
    if (!fg || IsShellOrOwnWindow(fg)) return false;
    if (!MonitorIsOurs(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST))) return false;
    return IsZoomed(fg) != FALSE;
}

// ---------------------------------------------------------------------------
// Live HDR state
// ---------------------------------------------------------------------------

static bool  g_hdrActive = false;
float        g_maxNits = 0.0f;    // shared with the settings window (ui/)
bool AppHdrActive() { return g_hdrActive; }
static float g_sdrWhiteNits = 80.0f;

// The user's "SDR content brightness" slider (HDR mode only). SDR content —
// which is what the reference wallpaper was — renders at this level, so we
// match it for parity. 1000 units == 80 nits.
// *ok (optional): false when the lookup missed and the 80 is only the fallback.
static float GetSdrWhiteNits(HMONITOR mon, bool* ok = nullptr) {
    if (ok) *ok = false;
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return 80.0f;

    UINT32 numPath = 0, numMode = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &numPath, &numMode) != ERROR_SUCCESS)
        return 80.0f;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(numPath);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(numMode);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &numPath, paths.data(),
                           &numMode, modes.data(), nullptr) != ERROR_SUCCESS)
        return 80.0f;

    for (UINT32 i = 0; i < numPath; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
        if (wcscmp(src.viewGdiDeviceName, mi.szDevice) != 0) continue;

        DISPLAYCONFIG_SDR_WHITE_LEVEL wl = {};
        wl.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        wl.header.size = sizeof(wl);
        wl.header.adapterId = paths[i].targetInfo.adapterId;
        wl.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&wl.header) != ERROR_SUCCESS) break;
        if (ok) *ok = true;
        return wl.SDRWhiteLevel / 1000.0f * 80.0f;
    }
    return 80.0f;
}

// Windows' active colour mode for the monitor: 0 SDR, 1 WCG (an SDR desktop under auto
// colour management: DWM colour-manages it like HDR), 2 HDR; -1 unknown.
// DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 (Windows 11 24H2; declared locally so older SDKs
// build), else the older info's advancedColorEnabled bit.
static int QueryColorMode(HMONITOR mon) {
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return -1;
    UINT32 numPath = 0, numMode = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &numPath, &numMode) != ERROR_SUCCESS) return -1;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(numPath);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(numMode);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &numPath, paths.data(),
                           &numMode, modes.data(), nullptr) != ERROR_SUCCESS)
        return -1;
    for (UINT32 i = 0; i < numPath; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
        if (wcscmp(src.viewGdiDeviceName, mi.szDevice) != 0) continue;
        struct { DISPLAYCONFIG_DEVICE_INFO_HEADER header; UINT32 value, colorEncoding, bitsPerColorChannel,
                 activeColorMode; } i2 = {};
        i2.header.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)15;   // GET_ADVANCED_COLOR_INFO_2
        i2.header.size = sizeof(i2);
        i2.header.adapterId = paths[i].targetInfo.adapterId;
        i2.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&i2.header) == ERROR_SUCCESS && i2.activeColorMode <= 2)
            return (int)i2.activeColorMode;
        struct { DISPLAYCONFIG_DEVICE_INFO_HEADER header; UINT32 value, colorEncoding, bitsPerColorChannel; } i1 = {};
        i1.header.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)9;    // GET_ADVANCED_COLOR_INFO
        i1.header.size = sizeof(i1);
        i1.header.adapterId = paths[i].targetInfo.adapterId;
        i1.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&i1.header) != ERROR_SUCCESS) return -1;
        return (i1.value & 2) ? 1 : 0;      // advancedColorEnabled: colour-managed (HDR is told apart by DXGI)
    }
    return -1;
}

// primXY (optional): xy of the output's R, G, B primaries as DXGI reports them (EDID);
// *primValid false when the output was not found.
static bool QueryHDR(HMONITOR mon, float* maxNits, float* primXY = nullptr, bool* primValid = nullptr) {
    if (primValid) *primValid = false;
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return g_hdrActive;
    for (UINT ai = 0; ; ai++) {
        ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(factory->EnumAdapters1(ai, &adapter))) break;
        for (UINT oi = 0; ; oi++) {
            ComPtr<IDXGIOutput> output;
            if (FAILED(adapter->EnumOutputs(oi, &output))) break;
            ComPtr<IDXGIOutput6> output6;
            DXGI_OUTPUT_DESC1 desc = {};
            if (SUCCEEDED(output.As(&output6)) && SUCCEEDED(output6->GetDesc1(&desc)) &&
                desc.Monitor == mon) {
                if (maxNits) *maxNits = desc.MaxLuminance;
                if (primXY) {
                    primXY[0] = desc.RedPrimary[0];   primXY[1] = desc.RedPrimary[1];
                    primXY[2] = desc.GreenPrimary[0]; primXY[3] = desc.GreenPrimary[1];
                    primXY[4] = desc.BluePrimary[0];  primXY[5] = desc.BluePrimary[1];
                    if (primValid) *primValid = true;
                }
                return desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            }
        }
    }
    return g_hdrActive;
}

// ---------------------------------------------------------------------------
// Tray icon
// ---------------------------------------------------------------------------

static const UINT WM_TRAYICON = WM_APP + 1;
enum TrayCmd : UINT {
    CMD_PAUSE = 1, CMD_FSPAUSE = 2, CMD_EXIT = 3, CMD_MAXPAUSE = 4, CMD_SETTINGS = 5,
    CMD_ANALYZER = 6, CMD_SCENES = 7,
    // Explicit, NON-toggling pause commands for scripts (tools/away-pause.ps1):
    // a toggle forces the caller to track state, and one missed message leaves
    // the wallpaper running while the script thinks it is paused. CMD_PAUSE
    // stays the toggle, because that is what the tray menu item wants.
    CMD_PAUSE_ON = 8, CMD_PAUSE_OFF = 9,
    CMD_PRESET_SAVE = 30, CMD_PRESET_FOLDER = 31,
    CMD_PHOTO_FOLDER = 32,    // brief BY: "Open photos folder" (33/34 stay retired)
    CMD_PRESET_BASE = 2000,   // up to kMaxTrayPresets entries (no 50 cap any more)
    // Cycle director. All non-toggling except the tray's On/Off item (a
    // checkbox, like CMD_PAUSE): scripts and --cycle-next use ON/OFF/NEXT/PREV
    // so they never have to track state. (33/34/800 were the retired mood
    // conductor's ids; left unused.)
    CMD_CYCLE_TOGGLE = 40, CMD_CYCLE_ON = 41, CMD_CYCLE_OFF = 42,
    CMD_CYCLE_NEXT = 43, CMD_CYCLE_PREV = 44, CMD_CYCLE_STAGE_BASE = 900,
    CMD_PEAK_OFF = 10, CMD_PEAK_AUTO = 11, CMD_PEAK_300 = 12, CMD_PEAK_600 = 13,
    CMD_PEAK_800 = 15, CMD_PEAK_1000 = 14,
    CMD_GAMUT_SRGB = 20, CMD_GAMUT_P3 = 21, CMD_GAMUT_2020 = 22,
    CMD_SDRGAMUT_PLAIN = 23, CMD_SDRGAMUT_MATCH = 24, CMD_SDRGAMUT_STRETCH = 25,   // [hdr] sdr_gamut 0 / 1 / 2
};

// U42 pause-state READBACK for the watcher scripts (tools\pause-readback.ps1). Read-only: sent to
// the tray window with SendMessageTimeout (wParam = lParam = 0), it changes nothing and answers
//   0x4657'0000 ('FW', so a pre-U42 exe's DefWindowProc 0 means "no readback")
//   | 0x1000 (protocol v1, bits 12-15)
//   | the holders of the manual pause << 8 (0x100 user, 0x200 away, 0x400 game)
//   | the flags below.
static const UINT WM_FW_PAUSE_QUERY = WM_APP + 0x42;
enum PauseStateFlag : UINT {
    PSF_MANUAL     = 0x001,   // manual pause on (held by anyone): one black frame, then no presents
    PSF_FULLSCREEN = 0x002,   // fullscreen / maximized app in front: not rendering (no black frame)
    PSF_SUSPENDED  = 0x004,   // renderer torn down (fullscreen > 20 s, device lost, refused startup)
    PSF_CYCLE      = 0x008,   // cycle director paused (Settings window open)
    PSF_LOST       = 0x010,   // wallpaper window lost (Explorer restart), waiting for the shell
};
static LRESULT PauseStateWord() {
    UINT f = 0x46570000u | 0x1000u | ((g_pauseHolders & HOLD_ALL) << 8);
    if (g_manualPause)   f |= PSF_MANUAL;
    if (g_fsPaused)      f |= PSF_FULLSCREEN;
    if (g_suspended)     f |= PSF_SUSPENDED;
    if (CyclePaused())   f |= PSF_CYCLE;
    if (g_wallpaperLost) f |= PSF_LOST;
    return (LRESULT)f;
}

static NOTIFYICONDATAW g_nid = {};

// presets (implementations further down; the menu needs them declared)
static std::vector<std::wstring> g_presetPaths;
static void GetPresetsDir(wchar_t out[MAX_PATH]);
static void GetPhotosDir(wchar_t out[MAX_PATH]);
static void ApplyPreset(const std::wstring& path);
static void SaveCurrentAsPreset();
static void RateNow(int rating);   // Ctrl+Alt+Up / Down (the rating hotkeys)
static const int kHotRateUp = 0x5201, kHotRateDown = 0x5202;
static void ShowTrayMenuBody(HWND hwnd, HMENU presets);

static const size_t kMaxTrayPresets = 2000;

// Which look a preset file carries: its [look] section (style= string, or the int forms
// the old checkboxes wrote). No [look] section at all = a partial overlay (Mirror - *).
// [meta] base= of a preset (FINAL-CYCLE B.4, the same rule as the settings
// window's UiPresetBase): relative to the preset's own folder, or absolute.
// Empty when the preset has none or the file is missing.
static std::wstring PresetMetaBase(const std::wstring& path) {
    wchar_t b[MAX_PATH] = {};
    GetPrivateProfileStringW(L"meta", L"base", L"", b, MAX_PATH, path.c_str());
    if (!b[0]) return L"";
    std::wstring base = b;
    while (!base.empty() && (base.back() == L' ' || base.back() == L'\t')) base.pop_back();
    size_t a = 0;
    while (a < base.size() && (base[a] == L' ' || base[a] == L'\t')) a++;
    base = base.substr(a);
    const bool absolute = base.size() > 2 && (base[1] == L':' || (base[0] == L'\\' && base[1] == L'\\'));
    if (!absolute) {
        size_t sl = path.find_last_of(L"\\/");
        base = (sl == std::wstring::npos ? std::wstring(L".") : path.substr(0, sl)) + L"\\" + base;
    }
    wchar_t full[MAX_PATH * 2] = {};
    if (GetFullPathNameW(base.c_str(), MAX_PATH * 2, full, nullptr)) base = full;
    if (GetFileAttributesW(base.c_str()) == INVALID_FILE_ATTRIBUTES) return L"";
    return base;
}

// The [meta] base chain UNDER a preset, root first (the file itself not included), up to
// maxDepth files -- the same walk and the same limit (4) as MergePresetChain. The ONE
// resolver the cycle (ResolveStage) and the Settings reset target (RecomputeTarget) share,
// so a "Save as" on top of a chained preset composes the same everywhere: they followed
// one level while the tray apply followed four (review 2026-10-04 R18). A missing file
// ends the walk, as in PresetMetaBase; *missing (optional) is set when it was a dangling
// base= that ended it, so a cycle stage can refuse it the way it refuses a missing base.
std::vector<std::wstring> PresetBaseChain(const std::wstring& path, int maxDepth, bool* missing) {
    std::vector<std::wstring> chain;
    if (missing) *missing = false;
    std::wstring cur = path;
    for (int d = 0; d < maxDepth; d++) {
        const std::wstring base = PresetMetaBase(cur);
        if (base.empty()) {
            wchar_t b[MAX_PATH] = {};
            GetPrivateProfileStringW(L"meta", L"base", L"", b, MAX_PATH, cur.c_str());
            if (b[0] && missing) *missing = true;
            break;
        }
        chain.insert(chain.begin(), base);
        cur = base;
    }
    return chain;
}

static int PresetLookGroup(const wchar_t* path, int depth = 0) {
    wchar_t sec[512] = {};
    if (GetPrivateProfileSectionW(L"look", sec, 512, path) == 0) {
        // no [look]: a partial. [meta] base= carries the look it sits on;
        // [meta] look= names it outright; else an overlay (the old rule)
        if (depth < 4) {
            std::wstring base = PresetMetaBase(path);
            if (!base.empty()) return PresetLookGroup(base.c_str(), depth + 1);
        }
        wchar_t ml[32] = {};
        GetPrivateProfileStringW(L"meta", L"look", L"", ml, 32, path);
        if (!_wcsicmp(ml, L"liquid_acid")) return 1;
        if (!_wcsicmp(ml, L"ink")) return 2;
        if (!_wcsicmp(ml, L"fluid")) return 0;
        return 3;   // overlay
    }
    // same rule as LoadConfigFromIni: style= string first, a PRESENT int key overrides it,
    // ink wins ties (reading just [look] keeps the menu instant with 60+ files)
    wchar_t style[32] = {};
    GetPrivateProfileStringW(L"look", L"style", L"", style, 32, path);
    bool acid = _wcsicmp(style, L"liquid_acid") == 0;
    bool ink = _wcsicmp(style, L"ink") == 0;
    acid = GetPrivateProfileIntW(L"look", L"liquid_acid", acid ? 1 : 0, path) != 0;
    ink = GetPrivateProfileIntW(L"look", L"ink", ink ? 1 : 0, path) != 0;
    return ink ? 2 : (acid ? 1 : 0);
}

static void ShowTrayMenu(HWND hwnd) {
    // enumerate preset files fresh each time the menu opens (the presets folder; the
    // moods folder is retired with the conductor -- its files stay on disk untouched,
    // nothing scans them any more), sorted and grouped into submenus by the look
    // they carry (UI-REHAUL 1a: no 50 cap)
    g_presetPaths.clear();
    HMENU presets = CreatePopupMenu();
    {
        wchar_t dir[MAX_PATH], pattern[MAX_PATH];
        GetPresetsDir(dir);
        swprintf_s(pattern, L"%s\\*.ini", dir);
        std::vector<std::wstring> names;
        WIN32_FIND_DATAW fd;
        HANDLE find = FindFirstFileW(pattern, &fd);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.push_back(fd.cFileName);
            } while (FindNextFileW(find, &fd));
            FindClose(find);
        }
        std::sort(names.begin(), names.end(), [](const std::wstring& x, const std::wstring& y) {
            return _wcsicmp(x.c_str(), y.c_str()) < 0;
        });
        static const wchar_t* kGroup[4] = { L"Fluid (WE)", L"Liquid Acid", L"Ink", L"Overlays" };
        HMENU sub[4] = { CreatePopupMenu(), CreatePopupMenu(), CreatePopupMenu(), CreatePopupMenu() };
        int count[4] = {};
        for (const std::wstring& file : names) {
            if (g_presetPaths.size() >= kMaxTrayPresets) break;
            std::wstring full = std::wstring(dir) + L"\\" + file;
            std::wstring name = file;
            size_t dot = name.rfind(L".ini");
            if (dot != std::wstring::npos) name.resize(dot);
            int g = PresetLookGroup(full.c_str());
            AppendMenuW(sub[g], MF_STRING, CMD_PRESET_BASE + g_presetPaths.size(), name.c_str());
            g_presetPaths.push_back(full);
            count[g]++;
        }
        for (int g = 0; g < 4; g++) {
            if (count[g] == 0) { DestroyMenu(sub[g]); continue; }
            wchar_t lbl[64];
            swprintf_s(lbl, L"%s (%d)", kGroup[g], count[g]);
            AppendMenuW(presets, MF_POPUP, (UINT_PTR)sub[g], lbl);
        }
        if (g_presetPaths.empty())
            AppendMenuW(presets, MF_STRING | MF_GRAYED, 0, L"(no presets yet)");
        AppendMenuW(presets, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(presets, MF_STRING, CMD_PRESET_SAVE, L"Save current as new preset");
        AppendMenuW(presets, MF_STRING, CMD_PRESET_FOLDER, L"Open presets folder");
        AppendMenuW(presets, MF_STRING, CMD_PHOTO_FOLDER, L"Open photos folder");
    }
    ShowTrayMenuBody(hwnd, presets);
}

static void ShowTrayMenuBody(HWND hwnd, HMENU presets) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (g_manualPause ? MF_CHECKED : 0), CMD_PAUSE, L"Pause");
    AppendMenuW(menu, MF_STRING | (g_pauseOnFullscreen ? MF_CHECKED : 0), CMD_FSPAUSE,
                L"Pause when a fullscreen app is running");
    AppendMenuW(menu, MF_STRING | (g_pauseOnMaximized ? MF_CHECKED : 0), CMD_MAXPAUSE,
                L"Pause when an app is maximized");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // HDR peak brightness (hot-spot target; only active while Windows HDR is on)
    HMENU peak = CreatePopupMenu();
    wchar_t autoLabel[64];
    swprintf_s(autoLabel, L"Windows-reported max (%.0f nits)", g_maxNits);
    AppendMenuW(peak, MF_STRING | (g_hdrPeakNits == 0.0f ? MF_CHECKED : 0), CMD_PEAK_OFF,
                L"Off — match SDR brightness");
    AppendMenuW(peak, MF_STRING | (g_hdrPeakNits < 0.0f ? MF_CHECKED : 0), CMD_PEAK_AUTO, autoLabel);
    AppendMenuW(peak, MF_STRING | (g_hdrPeakNits == 300.0f ? MF_CHECKED : 0), CMD_PEAK_300, L"300 nits");
    AppendMenuW(peak, MF_STRING | (g_hdrPeakNits == 600.0f ? MF_CHECKED : 0), CMD_PEAK_600, L"600 nits");
    AppendMenuW(peak, MF_STRING | (g_hdrPeakNits == 800.0f ? MF_CHECKED : 0), CMD_PEAK_800, L"800 nits");
    AppendMenuW(peak, MF_STRING | (g_hdrPeakNits == 1000.0f ? MF_CHECKED : 0), CMD_PEAK_1000,
                L"1000 nits (QD-OLED small window)");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)peak, L"HDR peak brightness");

    HMENU gamut = CreatePopupMenu();
    AppendMenuW(gamut, MF_STRING | (g_gamutMode == 0 ? MF_CHECKED : 0), CMD_GAMUT_SRGB, L"sRGB (standard)");
    AppendMenuW(gamut, MF_STRING | (g_gamutMode == 1 ? MF_CHECKED : 0), CMD_GAMUT_P3, L"Display-P3 (like the WE original)");
    AppendMenuW(gamut, MF_STRING | (g_gamutMode == 2 ? MF_CHECKED : 0), CMD_GAMUT_2020, L"BT.2020 (full QD-OLED)");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)gamut, L"Color gamut");

    HMENU sdrg = CreatePopupMenu();
    AppendMenuW(sdrg, MF_STRING | (g_sdrGamutMode == 1 ? MF_CHECKED : 0), CMD_SDRGAMUT_MATCH,
                L"Match HDR on this panel (default)");
    AppendMenuW(sdrg, MF_STRING | (g_sdrGamutMode == 0 ? MF_CHECKED : 0), CMD_SDRGAMUT_PLAIN, L"Plain sRGB");
    AppendMenuW(sdrg, MF_STRING | (g_sdrGamutMode == 2 ? MF_CHECKED : 0), CMD_SDRGAMUT_STRETCH,
                L"Full stretch (monitor in sRGB mode)");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)sdrg, L"SDR colours (HDR off)");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // Cycle director: On/Off, Next, Previous, the stage list (current checked)
    HMENU cycle = CreatePopupMenu();
    {
        const CycleConfig& cc = CycleGet();
        const CycleStatus cs = CycleState();
        const bool running = cs.phase != CYCLE_OFF;
        const UINT none = cc.stages.empty() ? MF_GRAYED : 0;
        AppendMenuW(cycle, MF_STRING | none | (running ? MF_CHECKED : 0), CMD_CYCLE_TOGGLE,
                    L"Cycle looks");
        AppendMenuW(cycle, MF_STRING | (running ? 0 : MF_GRAYED), CMD_CYCLE_NEXT, L"Next stage");
        AppendMenuW(cycle, MF_STRING | (running ? 0 : MF_GRAYED), CMD_CYCLE_PREV, L"Previous stage");
        AppendMenuW(cycle, MF_SEPARATOR, 0, nullptr);
        for (int i = 0; i < (int)cc.stages.size() && i < 99; i++) {
            if (cc.stages[i].guest) continue;    // ui3: a guest visit (Ink / Fluid) is not a stage of the playlist
            const std::wstring label = CycleStageLabel(i);
            AppendMenuW(cycle, MF_STRING | (cc.stages[i].ok ? 0 : MF_GRAYED) |
                               (running && i == cs.stage ? MF_CHECKED : 0),
                        CMD_CYCLE_STAGE_BASE + i, label.c_str());
        }
        if (cc.stages.empty())
            AppendMenuW(cycle, MF_STRING | MF_GRAYED, 0, L"(no [cycle] stages in settings.ini)");
    }
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)cycle, L"Cycle");

    AppendMenuW(menu, MF_POPUP, (UINT_PTR)presets, L"Presets");
    AppendMenuW(menu, MF_STRING, CMD_SETTINGS, L"Settings…");
    AppendMenuW(menu, MF_STRING, CMD_ANALYZER, L"HDR analyzer (nits heat-map)…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CMD_EXIT, L"Exit");

    SetForegroundWindow(hwnd);
    POINT pt;
    GetCursorPos(&pt);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

static UINT g_taskbarCreatedMsg = 0;

// menu font for the owner-drawn tray items, cached for the process lifetime
static HFONT TrayMenuFont() {
    static HFONT s_menuFont = nullptr;
    if (!s_menuFont) {
        NONCLIENTMETRICSW ncm = { sizeof(ncm) };
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
            s_menuFont = CreateFontIndirectW(&ncm.lfMenuFont);
    }
    return s_menuFont;
}

// A tray pick of peak / gamut is the USER's own value: live now, the director's
// fallback for stages without their own [hdr] keys, and what settings.ini keeps.
static void SetUserPeak(float nits) { g_hdrPeakNits = nits; CycleSetUserPeak(nits); SaveSettings(); }
static void SetUserGamut(int mode)  { g_gamutMode = mode;   CycleSetUserGamut(mode); SaveSettings(); }

static LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Explorer restarted: the tray was rebuilt, our icon is gone — re-add it.
    if (g_taskbarCreatedMsg && msg == g_taskbarCreatedMsg) {
        Shell_NotifyIconW(NIM_ADD, &g_nid);
        return 0;
    }
    switch (msg) {
    case WM_HOTKEY:   // the rating hotkeys (RegisterRateHotkeys)
        if (wp == kHotRateUp) RateNow(+1);
        else if (wp == kHotRateDown) RateNow(-1);
        return 0;
    case WM_TRAYICON:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP ||
            LOWORD(lp) == WM_CONTEXTMENU)
            ShowTrayMenu(hwnd);
        return 0;
    case WM_FW_PAUSE_QUERY:   // U42 readback; read-only
        return PauseStateWord();
    case WM_ENDSESSION:       // post-night T2: Windows ends the process right after this returns
        if (wp) ExitLog("exit: the Windows session is ending (lParam 0x%08lX)", (unsigned long)lp);
        return 0;
    // owner-drawn (skipped) mood items: gray text, but still clickable
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT* mi = (MEASUREITEMSTRUCT*)lp;
        if (mi->CtlType == ODT_MENU && mi->itemData) {
            const wchar_t* text = (const wchar_t*)mi->itemData;
            HDC dc = GetDC(hwnd);
            HFONT oldFont = nullptr;
            if (HFONT f = TrayMenuFont()) oldFont = (HFONT)SelectObject(dc, f);
            SIZE sz = {};
            GetTextExtentPoint32W(dc, text, lstrlenW(text), &sz);
            if (oldFont) SelectObject(dc, oldFont);
            ReleaseDC(hwnd, dc);
            mi->itemWidth = sz.cx + 34;   // check gutter + padding
            mi->itemHeight = sz.cy + 8 > 22 ? sz.cy + 8 : 22;
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* di = (DRAWITEMSTRUCT*)lp;
        if (di->CtlType == ODT_MENU && di->itemData) {
            const wchar_t* text = (const wchar_t*)di->itemData;
            const bool sel = (di->itemState & ODS_SELECTED) != 0;
            FillRect(di->hDC, &di->rcItem,
                     GetSysColorBrush(sel ? COLOR_MENUHILIGHT : COLOR_MENU));
            SetBkMode(di->hDC, TRANSPARENT);
            const COLORREF fg = GetSysColor(sel ? COLOR_HIGHLIGHTTEXT : COLOR_GRAYTEXT);
            SetTextColor(di->hDC, fg);
            HFONT oldFont = nullptr;
            if (HFONT f = TrayMenuFont()) oldFont = (HFONT)SelectObject(di->hDC, f);
            RECT tr = di->rcItem;
            tr.left += 26;   // leave the check gutter empty
            if (di->itemState & ODS_CHECKED) {
                // owner-draw items get no stock checkmark — draw one
                HPEN pen = CreatePen(PS_SOLID, 1, fg);
                HPEN oldPen = (HPEN)SelectObject(di->hDC, pen);
                const int cx = di->rcItem.left + 12, cy = (tr.top + tr.bottom) / 2;
                MoveToEx(di->hDC, cx - 5, cy, nullptr);
                LineTo(di->hDC, cx - 1, cy + 4);
                LineTo(di->hDC, cx + 6, cy - 5);
                SelectObject(di->hDC, oldPen);
                DeleteObject(pen);
            }
            DrawTextW(di->hDC, text, -1, &tr, DT_SINGLELINE | DT_VCENTER);
            if (oldFont) SelectObject(di->hDC, oldFont);
            return TRUE;
        }
        break;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case CMD_PAUSE:   // the tray's toggle: a resume is the user's own and clears every holder
            SetManualPause(!g_manualPause, g_manualPause ? HOLD_ALL : HOLD_USER, "tray");
            break;
        case CMD_PAUSE_ON:
        case CMD_PAUSE_OFF: {
            // lParam = the caller's holder bit (2 away-pause, 4 game-pause). 0 = a pre-U42 caller:
            // ON is a user hold, OFF clears every holder (what OFF always meant).
            const bool on = LOWORD(wp) == CMD_PAUSE_ON;
            UINT holder = (UINT)lp & HOLD_ALL;
            if (!holder) holder = on ? HOLD_USER : HOLD_ALL;
            SetManualPause(on, holder, on ? "cmd8" : "cmd9");
            break;
        }
        case CMD_FSPAUSE:
            g_pauseOnFullscreen = !g_pauseOnFullscreen;
            SaveSettings();
            printf("pause on fullscreen: %s\n", g_pauseOnFullscreen ? "on" : "off");
            break;
        case CMD_MAXPAUSE:
            g_pauseOnMaximized = !g_pauseOnMaximized;
            SaveSettings();
            printf("pause on maximized: %s\n", g_pauseOnMaximized ? "on" : "off");
            break;
        case CMD_PEAK_OFF:  SetUserPeak(0.0f);    break;
        case CMD_PEAK_AUTO: SetUserPeak(-1.0f);   break;
        case CMD_PEAK_300:  SetUserPeak(300.0f);  break;
        case CMD_PEAK_600:  SetUserPeak(600.0f);  break;
        case CMD_PEAK_800:  SetUserPeak(800.0f);  break;
        case CMD_PEAK_1000: SetUserPeak(1000.0f); break;
        case CMD_SETTINGS:
            WpLog("tray: CMD_SETTINGS");
            ShowSettingsWindow();
            break;
        case CMD_ANALYZER:
            ShowAnalyzerWindow();
            break;
        case CMD_PRESET_SAVE:
            SaveCurrentAsPreset();
            break;
        case CMD_CYCLE_TOGGLE:
            CycleSetEnabled(CycleState().phase == CYCLE_OFF, true);
            break;
        case CMD_CYCLE_ON:
        case CMD_CYCLE_OFF:
            CycleSetEnabled(LOWORD(wp) == CMD_CYCLE_ON, true);
            break;
        case CMD_CYCLE_NEXT:
            CycleNext();
            break;
        case CMD_CYCLE_PREV:
            CyclePrev();
            break;
        case CMD_PRESET_FOLDER: {
            wchar_t dir[MAX_PATH];
            GetPresetsDir(dir);
            CreateDirectoryW(dir, nullptr);
            ShellExecuteW(nullptr, L"open", dir, nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        case CMD_PHOTO_FOLDER: {             // brief BY: the photo stage's default folder
            wchar_t dir[MAX_PATH];
            GetPhotosDir(dir);
            CreateDirectoryW(dir, nullptr);
            ShellExecuteW(nullptr, L"open", dir, nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        default:
            if (LOWORD(wp) >= CMD_PRESET_BASE && LOWORD(wp) < CMD_PRESET_BASE + kMaxTrayPresets &&
                LOWORD(wp) - CMD_PRESET_BASE < g_presetPaths.size()) {
                ApplyPreset(g_presetPaths[LOWORD(wp) - CMD_PRESET_BASE]);
            } else if (LOWORD(wp) >= CMD_CYCLE_STAGE_BASE && LOWORD(wp) < CMD_CYCLE_STAGE_BASE + 99) {
                CycleJump(LOWORD(wp) - CMD_CYCLE_STAGE_BASE);
            }
            break;
        case CMD_GAMUT_SRGB: SetUserGamut(0); break;
        case CMD_GAMUT_P3:   SetUserGamut(1); break;
        case CMD_GAMUT_2020: SetUserGamut(2); break;
        case CMD_SDRGAMUT_PLAIN:   g_sdrGamutMode = 0; SaveSettings(); WpLog("tray: sdr_gamut 0 (plain sRGB)"); break;
        case CMD_SDRGAMUT_MATCH:   g_sdrGamutMode = 1; SaveSettings(); WpLog("tray: sdr_gamut 1 (match HDR)"); break;
        case CMD_SDRGAMUT_STRETCH: g_sdrGamutMode = 2; SaveSettings(); WpLog("tray: sdr_gamut 2 (full stretch)"); break;
        case CMD_EXIT:
            printf("tray: exit clicked\n");
            g_running = false;
            g_exitReason = "CMD_EXIT (the tray's Exit, or a script's WM_COMMAND 3)";
            break;
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND CreateTrayWindow() {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FluidWallpaperTray";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Fluid Wallpaper Tray",
                                0, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) Fail("CreateTrayWindow", HRESULT_FROM_WIN32(GetLastError()));

    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_INFO;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));   // fluid vortex
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"Fluid Wallpaper");
    // startup balloon so the icon is discoverable (Win11 hides new tray icons
    // behind the ^ overflow by default)
    wcscpy_s(g_nid.szInfoTitle, L"Fluid Wallpaper is running");
    wcscpy_s(g_nid.szInfo,
             L"Right-click this icon for settings and pause. "
             L"Tip: double-clicking FluidWallpaper.exe again also opens Settings.");
    g_nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;   // balloon only once
    return hwnd;
}

static void RemoveTrayIcon() {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

static void UpdateTrayTip() {
    if (!g_nid.hWnd) return;
    if (g_hdrActive)
        _snwprintf_s(g_nid.szTip, _TRUNCATE, L"Fluid Wallpaper — HDR on (%.0f nits)", g_maxNits);
    else
        _snwprintf_s(g_nid.szTip, _TRUNCATE, L"Fluid Wallpaper — HDR off (SDR)");
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void ShowTrayBalloon(const wchar_t* title, const wchar_t* msg, bool quiet = false) {
    if (!g_nid.hWnd) return;
    g_nid.uFlags |= NIF_INFO;
    wcscpy_s(g_nid.szInfoTitle, title);
    wcscpy_s(g_nid.szInfo, msg);
    g_nid.dwInfoFlags = NIIF_INFO | (quiet ? NIIF_NOSOUND : 0);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
}

// ---------------------------------------------------------------------------
// Presets: full settings.ini snapshots in %APPDATA%\FluidWallpaper\presets
// ---------------------------------------------------------------------------

static void GetPresetsDir(wchar_t out[MAX_PATH]) {
    wcscpy_s(out, MAX_PATH, g_iniPath);
    wchar_t* slash = wcsrchr(out, L'\\');
    if (slash) *slash = 0;
    wcscat_s(out, MAX_PATH, L"\\presets");
}

// brief BY pre-flight 21: the photo stage's default folder, the same
// derivation as the presets folder: dirname(settings.ini)\photos.
static void GetPhotosDir(wchar_t out[MAX_PATH]) {
    wcscpy_s(out, MAX_PATH, g_iniPath);
    wchar_t* slash = wcsrchr(out, L'\\');
    if (slash) *slash = 0;
    wcscat_s(out, MAX_PATH, L"\\photos");
}

static void WriteTextFileUtf16(const wchar_t* path, const wchar_t* text) {
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WORD bom = 0xFEFF;
    WriteFile(f, &bom, 2, &written, nullptr);
    WriteFile(f, text, (DWORD)(wcslen(text) * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(f);
}

// The user's original Wallpaper Engine preset: project.json values + the WE
// right-panel color filter they ran it with. peak off / P3 = how WE looked.
static const wchar_t* kWEOriginalIni =
// True WE parity (rebuilt 2026-09-16): every key the look depends on, from WE
// config.json (wproperties + wec_* panel: sat 57 / con 67 / brs 53 / hue 54).
// sim_res/dye_res intentionally absent - presets never touch them.
L"[moods]\r\nenabled=0\r\n"
L"[hdr]\r\npeak_nits=700\r\ncompensation=1\r\nknee=0.70\r\nsaturation=1.20\r\nbrightness=1.08\r\ncontrast=1.00\r\ngamut=1\r\n"
L"[sim]\r\nvorticity=48\r\nsplat_radius=0.64\r\ndensity_diffusion=0.999\r\nvelocity_diffusion=0.999\r\n"
L"pressure_diffusion=0.85\r\npressure_iterations=20\r\ndecay_fast=1.000\r\ndecay_threshold=0.290\r\n"
L"saturation_restore=0.93\r\nmax_brightness=1.35\r\nshading=1\r\ndye_diffusion=0.000\r\nbaroclinic=0.000\r\nflow_speed=1.00\r\n"
L"[behavior]\r\ncolor_cycle_period=19\r\nwanderers=1\r\nwanderer_count=2\r\nwanderer_mode=0\r\n"
L"wanderer_speed=246\r\nwanderer_brightness=0.10\r\nwanderer_scale=0.10\r\nwanderer_resume_delay=4.5\r\n"
L"auto_pause=1\r\ndark_floor=9\r\ndark_level=0.070\r\nsurv_dark_floor=8\r\ncontrast_req=30\r\n"
L"dart_enabled=1\r\ndart_interval=7\r\ndart_speed=967\r\n"
L"hueshift_enabled=1\r\nhueshift_step=83\r\nhueshift_linger=6.5\r\nhueshift_glide=7.2\r\n"
L"hueshift_burst_steps=2\r\nhueshift_off_time=10\r\n"
L"idle_splats=1\r\nidle_interval=9.6\r\nidle_amount=8\r\nidle_brightness=1.50\r\n"
L"hold_to_splat=1\r\nsplat_on_click=1\r\nshow_mouse=1\r\n"
L"[color]\r\ncolorful=1\r\nmore_colors=1\r\npost_saturation=1.14\r\npost_contrast=1.34\r\n"
L"post_brightness=1.06\r\npost_hue=14.4\r\nhue_center=0\r\nhue_range=180\r\nhue_linger=0\r\n"
L"curve_enabled=0\r\nshadow_floor=0.100\r\nshadow_knee=0.15\r\n"
L"[general]\r\npause_on_fullscreen=1\r\npause_on_maximized=1\r\n";

// Themed presets (partial inis — unspecified keys inherit the user's current
// settings). Sunny Embers: the orange-majority mood the user loves, pinned via
// the hue band so it can't drift away. Ember Depths: dark field, small red
// dots + blue accents (palette mode). Verdant: greens/teals.
static const wchar_t* kSunnyEmbersIni =
L"[color]\r\ncolorful=1\r\nhue_center=25\r\nhue_range=28\r\n"
L"post_saturation=1.15\r\npost_contrast=1.30\r\npost_brightness=1.05\r\npost_hue=0\r\n"
L"[sim]\r\ndensity_diffusion=0.9995\r\ndecay_fast=1.000\r\ndecay_threshold=0.290\r\n"
L"saturation_restore=0.93\r\nmax_brightness=0.90\r\nsplat_radius=0.64\r\nvorticity=48\r\n"
L"[behavior]\r\ncolor_cycle_period=45\r\nwanderer_brightness=0.10\r\ndark_floor=18\r\n"
L"idle_amount=4\r\nidle_interval=12\r\n";

static const wchar_t* kEmberDepthsIni =
L"[color]\r\ncolorful=0\r\nmore_colors=1\r\n"
L"splat_color_1=1.0000 0.0500 0.0500\r\nsplat_color_2=0.8000 0.0000 0.1000\r\n"
L"splat_color_3=0.1000 0.2000 1.0000\r\nsplat_color_4=1.0000 0.2000 0.0000\r\n"
L"splat_color_5=0.0000 0.5000 1.0000\r\n"
L"post_saturation=1.10\r\npost_contrast=1.40\r\npost_brightness=1.00\r\npost_hue=0\r\n"
L"[sim]\r\nsplat_radius=0.15\r\nmax_brightness=0.70\r\ndensity_diffusion=0.9990\r\n"
L"decay_fast=0.985\r\ndecay_threshold=0.200\r\nsaturation_restore=0.90\r\nvorticity=45\r\n"
L"[behavior]\r\nwanderer_count=3\r\nwanderer_brightness=0.12\r\ndark_floor=30\r\n"
L"idle_amount=3\r\nidle_interval=6\r\ncolor_cycle_period=19\r\n";

static const wchar_t* kVerdantIni =
L"[color]\r\ncolorful=1\r\nhue_center=120\r\nhue_range=35\r\n"
L"post_saturation=1.15\r\npost_contrast=1.30\r\npost_brightness=1.05\r\npost_hue=0\r\n"
L"[sim]\r\ndensity_diffusion=0.9992\r\ndecay_fast=1.000\r\ndecay_threshold=0.290\r\n"
L"saturation_restore=0.93\r\nmax_brightness=0.85\r\nsplat_radius=0.55\r\nvorticity=48\r\n"
L"[behavior]\r\ncolor_cycle_period=30\r\nwanderer_brightness=0.10\r\ndark_floor=20\r\n"
L"idle_amount=4\r\nidle_interval=10\r\n";

// User-requested "super weird" scene: huge ember-colored bubbles pushed
// through the response-curve hump, so mid-brightness rims glow at max while
// splat cores drop back — bright outlines of every blob. Soft fronts via a
// touch of dye diffusion and a near-zero decay threshold.
static const wchar_t* kEmberRimsIni =
L"[color]\r\ncolorful=1\r\nhue_center=22\r\nhue_range=16\r\n"
L"curve_enabled=1\r\ncurve_center=0.30\r\ncurve_width=0.10\r\ncurve_height=1.30\r\n"
L"post_saturation=1.10\r\npost_contrast=1.15\r\npost_brightness=1.00\r\npost_hue=0\r\n"
L"[sim]\r\nsplat_radius=0.900\r\nmax_brightness=1.00\r\ndensity_diffusion=0.9990\r\n"
L"decay_fast=0.995\r\ndecay_threshold=0.020\r\nsaturation_restore=0.700\r\n"
L"vorticity=42\r\ndye_diffusion=0.050\r\n"
L"[behavior]\r\nwanderer_count=2\r\nwanderer_brightness=0.15\r\ndark_floor=25\r\n"
L"idle_amount=3\r\nidle_interval=8\r\ncolor_cycle_period=40\r\n";

static void EnsureBuiltinPresets() {
    if (!g_iniPath[0] || g_configReadOnly) return;
    wchar_t dir[MAX_PATH];
    // brief BY: the (empty) photos folder next to presets\, so the user can
    // find it; read-only runs (--shot) never create it
    GetPhotosDir(dir);
    CreateDirectoryW(dir, nullptr);
    GetPresetsDir(dir);
    CreateDirectoryW(dir, nullptr);

    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\WE Original.ini", dir);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
        WriteTextFileUtf16(path, kWEOriginalIni);
    swprintf_s(path, L"%s\\Sunny Embers.ini", dir);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
        WriteTextFileUtf16(path, kSunnyEmbersIni);
    swprintf_s(path, L"%s\\Ember Depths.ini", dir);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
        WriteTextFileUtf16(path, kEmberDepthsIni);
    swprintf_s(path, L"%s\\Verdant.ini", dir);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
        WriteTextFileUtf16(path, kVerdantIni);
    swprintf_s(path, L"%s\\Ember Rims.ini", dir);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
        WriteTextFileUtf16(path, kEmberRimsIni);

    // migrate the earlier checkpoint into the presets folder (never deleted)
    swprintf_s(path, L"%s\\Deep Clouds.ini", dir);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        wchar_t old[MAX_PATH];
        wcscpy_s(old, g_iniPath);
        wchar_t* slash = wcsrchr(old, L'\\');
        if (slash) *slash = 0;
        wcscat_s(old, L"\\preset-deep-clouds.ini");
        if (GetFileAttributesW(old) != INVALID_FILE_ATTRIBUTES)
            CopyFileW(old, path, TRUE);
    }
}

// Persist the complete merged config to an ini file (presets can be partial,
// so after applying one, the resolved state must be written out in full).
// includeShell=false skips the machine/shell keys (sim_res, dye_res,
// fps_limit, mirror_second) — used for mood files, which never touch them.
// "%.*f" at the key's usual decimals, unless that loses the value: then the shortest %g that
// reads back as the same float. A full dump is a preset now (Save as self-contained), and a
// value such as 0.0125 written at 3 decimals did not reproduce the look (review 2026-10-04 R15).
static void FormatLossless(wchar_t* b, size_t n, float v, int dec) {
    swprintf_s(b, n, L"%.*f", dec, v);
    for (int p = 6; p <= 9 && (float)_wtof(b) != v; p++) swprintf_s(b, n, L"%.*g", p, v);
}

static void WriteConfigKeys(const wchar_t* path, const FluidConfig& c, bool includeShell);
void WriteConfigToIni(const wchar_t* path, const FluidConfig& c, bool includeShell) {
    if (!path || !path[0] || g_configReadOnly) return;
    WriteConfigKeys(path, c, includeShell);
}
// The Settings window's self-contained "Save as": the full look dump, never the machine keys.
// Its own MayWrite() guards the path (the headless test folder is writable under --ui-shot,
// where g_configReadOnly blocks WriteConfigToIni) (review 2026-10-04 R15).
void WriteLookToIni(const wchar_t* path, const FluidConfig& c) {
    if (path && path[0]) WriteConfigKeys(path, c, false);
}

static void WriteConfigKeys(const wchar_t* path, const FluidConfig& c, bool includeShell) {
    auto putF = [path](const wchar_t* sec, const wchar_t* key, float v, int dec) {
        wchar_t b[48];
        FormatLossless(b, 48, v, dec);
        WritePrivateProfileStringW(sec, key, b, path);
    };
    auto putI = [path](const wchar_t* sec, const wchar_t* key, int v) {
        wchar_t b[32];
        swprintf_s(b, L"%d", v);
        WritePrivateProfileStringW(sec, key, b, path);
    };
    putF(L"sim", L"density_diffusion", c.densityDissipation, 4);
    putF(L"sim", L"velocity_diffusion", c.velocityDissipation, 4);
    putF(L"sim", L"pressure_diffusion", c.pressureDissipation, 3);
    putI(L"sim", L"pressure_iterations", c.pressureIterations);
    putF(L"sim", L"decay_fast", c.decayFast, 3);
    putF(L"sim", L"decay_threshold", c.decayThreshold, 3);
    putF(L"sim", L"saturation_restore", c.satRestore, 3);
    putF(L"sim", L"max_brightness", c.maxBrightness, 2);
    putF(L"sim", L"vorticity", c.curl, 1);
    putF(L"sim", L"baroclinic", c.baroclinic, 1);
    putF(L"sim", L"flow_speed", c.flowSpeed, 2);
    putF(L"sim", L"splat_radius", c.splatRadius, 3);
    putI(L"sim", L"shading", c.shading);
    if (includeShell) {
        putI(L"sim", L"sim_res", c.simRes);
        putI(L"sim", L"dye_res", c.dyeRes);
    }
    putF(L"hdr", L"knee", c.hdrKnee, 2);
    putF(L"hdr", L"saturation", c.hdrSaturation, 2);
    putF(L"hdr", L"brightness", c.hdrBrightness, 2);
    putF(L"hdr", L"contrast", c.hdrContrast, 2);
    putI(L"hdr", L"compensation", c.hdrCompensation);
    putF(L"behavior", L"color_cycle_period", c.colorCyclePeriod, 0);
    putI(L"behavior", L"wanderers", c.wanderers);
    putI(L"behavior", L"wanderer_count", c.wandererCount);
    putI(L"behavior", L"wanderer_mode", c.wandererMode);
    putF(L"behavior", L"wanderer_speed", c.wandererSpeed, 0);
    putF(L"behavior", L"wanderer_brightness", c.wandererBrightness, 2);
    putF(L"behavior", L"wanderer_scale", c.wandererScale, 2);
    putF(L"behavior", L"wanderer_resume_delay", c.wandererResumeDelay, 1);
    putI(L"behavior", L"auto_pause", c.autoPause);
    putF(L"behavior", L"dark_floor", c.darkFloor, 0);
    putF(L"behavior", L"dark_level", c.darkLevel, 3);
    putF(L"behavior", L"surv_dark_floor", c.survDarkFloor, 0);
    putF(L"behavior", L"contrast_req", c.contrastReq, 0);
    putI(L"behavior", L"dart_enabled", c.dartEnabled);
    putF(L"behavior", L"dart_interval", c.dartInterval, 0);
    putF(L"behavior", L"dart_speed", c.dartSpeed, 0);
    putI(L"behavior", L"hueshift_enabled", c.hsEnabled);
    putF(L"behavior", L"hueshift_step", c.hsStep, 0);
    putF(L"behavior", L"hueshift_linger", c.hsLinger, 1);
    putF(L"behavior", L"hueshift_glide", c.hsGlide, 1);
    putI(L"behavior", L"hueshift_burst_steps", c.hsBurstSteps);
    putF(L"behavior", L"hueshift_off_time", c.hsOffTime, 0);
    putI(L"behavior", L"idle_splats", c.idleSplats);
    putF(L"behavior", L"idle_interval", c.idleInterval, 1);
    putI(L"behavior", L"idle_amount", c.idleAmount);
    putF(L"behavior", L"idle_brightness", c.idleBrightness, 2);
    putI(L"behavior", L"hold_to_splat", c.holdToSplat);
    putI(L"behavior", L"splat_on_click", c.splatOnClick);
    putI(L"behavior", L"show_mouse", c.showMouse);
    if (includeShell) {
        putF(L"general", L"fps_limit", c.fpsLimit, 0);
        putI(L"general", L"mirror_second", c.mirrorSecond);
    }
    putI(L"color", L"colorful", c.colorful);
    putI(L"color", L"more_colors", c.moreColors);
    putF(L"color", L"post_saturation", c.postSaturation, 2);
    putF(L"color", L"post_contrast", c.postContrast, 2);
    putF(L"color", L"post_brightness", c.postBrightness, 2);
    putF(L"color", L"post_hue", c.postHue, 0);
    putF(L"color", L"hue_center", c.hueCenter, 0);
    putF(L"color", L"hue_range", c.hueRange, 0);
    putF(L"color", L"hue_linger", c.hueLinger, 2);
    putF(L"sim", L"dye_diffusion", c.dyeDiffusion, 3);
    putI(L"color", L"curve_enabled", c.curveEnabled);
    putF(L"color", L"curve_center", c.curveCenter, 2);
    putF(L"color", L"curve_width", c.curveWidth, 2);
    putF(L"color", L"curve_height", c.curveHeight, 2);
    putF(L"color", L"shadow_floor", c.shadowFloor, 3);
    putF(L"color", L"shadow_knee", c.shadowKnee, 2);
    putF(L"sim", L"gravity", c.gravity, 3);
    putF(L"sim", L"gravity_pow", c.gravityPow, 3);
    putF(L"sim", L"gravity_blur", c.gravityBlur, 3);
    // colour triples / the 12-float palettes: each number lossless as well (R15)
    auto fmtList = [](const float* v, int n) {
        std::wstring s;
        for (int k = 0; k < n; k++) {
            wchar_t b[64];   // %.4f of a float up to FLT_MAX is 46 chars: 32 could abort in swprintf_s
            FormatLossless(b, 64, v[k], 4);
            if (k) s += L' ';
            s += b;
        }
        return s;
    };
    for (int ci = 0; ci < 5; ci++) {
        wchar_t key[32];
        swprintf_s(key, L"splat_color_%d", ci + 1);
        WritePrivateProfileStringW(L"color", key, fmtList(&c.splatColors[ci * 3], 3).c_str(), path);
    }
    // ---- the render LOOK and its three sections ---------------------------
    // Without these a saved preset silently dropped the look: it round-tripped
    // the fluid keys, then re-applied as style=fluid with the ink/acid tuning
    // gone. [look] style is written in the string form the inis use (and the
    // int forms the checkboxes write are cleared, so they cannot contradict
    // it); the [ink]/[drops]/[liquid_acid] blocks below are written in full,
    // because a preset that carries a look must carry the tuning that makes
    // that look look like anything.
    auto putRgb = [path, &fmtList](const wchar_t* sec, const wchar_t* key, const float* c3) {
        WritePrivateProfileStringW(sec, key, fmtList(c3, 3).c_str(), path);
    };
    WritePrivateProfileStringW(L"look", L"style",
                               c.ink.enabled ? L"ink" : (c.acid.enabled ? L"liquid_acid"
                                                                        : L"fluid"), path);
    WritePrivateProfileStringW(L"look", L"ink", nullptr, path);          // delete
    WritePrivateProfileStringW(L"look", L"liquid_acid", nullptr, path);  // delete
    {
        const InkConfig& k = c.ink;
        const wchar_t* S = L"ink";
        putI(S, L"inverted", k.inverted);
        putF(S, L"density", k.density, 3);
        putF(S, L"chroma", k.chroma, 3);
        putF(S, L"edge_strength", k.edgeStrength, 3);
        putF(S, L"edge_lo", k.edgeLo, 4);
        putF(S, L"edge_hi", k.edgeHi, 4);
        putF(S, L"edge_scale", k.edgeScale, 2);
        putF(S, L"vignette", k.vignette, 3);
        putF(S, L"core_knee", k.coreKnee, 3);
        putF(S, L"hdr_core", k.hdrCore, 3);
        putF(S, L"motion_lo", k.motionLo, 1);
        putF(S, L"motion_hi", k.motionHi, 1);
        putF(S, L"motion_opacity", k.motionOpacity, 3);
        putF(S, L"veil_floor", k.veilFloor, 3);
        putF(S, L"tint_mid_dip", k.tintMidDip, 3);
        putF(S, L"pair_sweep_period", k.pairSweepPeriod, 1);
        putF(S, L"parallax", k.parallax, 3);
        putF(S, L"parallax_scale", k.parallaxScale, 3);
        putF(S, L"parallax_drift", k.parallaxDrift, 4);
        putI(S, L"tonemap", k.tonemap);
        putF(S, L"white_nits", k.whiteNits, 1);
        putF(S, L"black_nits", k.blackNits, 2);
        putF(S, L"tone_knee", k.toneKnee, 3);
        putF(S, L"tone_chroma", k.toneChroma, 3);
        putF(S, L"tint_hue_blend", k.tintHueBlend, 3);
        if (k.blackOnly) putI(S, L"black_only", k.blackOnly);              // test keys: written only while on
        if (k.blackOnly && k.filmColour > 0.0f) putF(S, L"film_colour", k.filmColour, 2);
        putRgb(S, L"paper_color", k.paper);
        putRgb(S, L"tint_thin", k.tintThin);
        putRgb(S, L"tint_thick", k.tintThick);
    }
    {
        const MirrorConfig& mr = c.mirror;
        const wchar_t* S = L"mirror";
        putI(S, L"mode", mr.mode);
        putI(S, L"segments", mr.segments);
        putI(S, L"source", mr.source);
        putF(S, L"rotate_period", mr.rotatePeriod, 1);
        putF(S, L"drift", mr.drift, 2);
        putF(S, L"soft", mr.soft, 4);
        putF(S, L"center_x", mr.centerX, 4);
        putF(S, L"center_y", mr.centerY, 4);
        if (mr.mode == 5) {                // the fish-eye camera's keys: written only while it is on
            putF(S, L"cam_fov", mr.camFov, 1);
            putF(S, L"cam_pitch", mr.camPitch, 1);
            putF(S, L"cam_height", mr.camHeight, 3);
            putF(S, L"cam_fade", mr.camFade, 3);
            putF(S, L"cam_relief", mr.camRelief, 2);
        }
        WritePrivateProfileStringW(S, L"center", nullptr, path);   // delete the pair form
    }
    {
        const PostConfig& po = c.post;
        const wchar_t* S = L"post";
        putF(S, L"film_grain", po.filmGrain, 3);
        putF(S, L"film_grain_size", po.filmGrainSize, 2);
        putF(S, L"film_grain_speed", po.filmGrainSpeed, 2);
        putF(S, L"film_grain_fps", po.filmGrainFps, 1);
        putF(S, L"film_grain_color", po.filmGrainColor, 3);
        putF(S, L"film_grain_chroma", po.filmGrainChroma, 3);
        putF(S, L"film_grain_density", po.filmGrainDensity, 3);
        putF(S, L"aberration", po.aberration, 3);
        putF(S, L"aberration_px", po.aberrationPx, 2);
        putF(S, L"aberration_field", po.aberrationField, 2);
        putF(S, L"aberration_coc", po.aberrationCoc, 3);
        putF(S, L"vignette", po.vignette, 3);
        putF(S, L"softness", po.softness, 2);
        putF(S, L"halo", po.halo, 3);
        putF(S, L"halo_px", po.haloPx, 1);
        putF(S, L"band_min", po.bandMin, 2);
        putF(S, L"post_blur_px", po.postBlurPx, 2);
        putF(S, L"post_glow", po.postGlow, 3);
        putF(S, L"post_glow_px", po.postGlowPx, 1);
        putF(S, L"post_glow_dark", po.postGlowDark, 2);
        putF(S, L"film_dust", po.filmDust, 3);
        putF(S, L"film_hairs", po.filmHairs, 3);
        putF(S, L"film_scratches", po.filmScratches, 3);
        putF(S, L"film_leak", po.filmLeak, 3);
        putF(S, L"film_artefact_rate", po.filmArtefactRate, 1);
        putF(S, L"film_noise", po.filmNoise, 3);
        putF(S, L"film_noise_size", po.filmNoiseSize, 2);
        putF(S, L"film_stock", po.filmStock, 3);
        putF(S, L"fog", po.fog, 3);
        putF(S, L"fog_px", po.fogPx, 1);
        putF(S, L"fog_mass_gate", po.fogMassGate, 3);
        putF(S, L"artefact_lum_gate", po.artefactLumGate, 3);
        putF(S, L"corner_warp", po.cornerWarp, 3);
        putF(S, L"corner_warp_r", po.cornerWarpR, 3);
        putF(S, L"bloom_warmth", po.bloomWarmth, 3);
        putF(S, L"glass_streaks", po.glassStreaks, 3);
        putF(S, L"halation_threshold", po.halationThreshold, 3);
        putF(S, L"bloom", po.bloom, 3);
        putF(S, L"bloom_px", po.bloomPx, 1);
        putF(S, L"light_x", po.lightX, 3);
        putF(S, L"light_y", po.lightY, 3);
        putF(S, L"light_drift", po.lightDrift, 3);
        putF(S, L"light_z", po.lightZ, 3);
        putF(S, L"camera_fov", po.cameraFov, 2);
        putF(S, L"camera_focus", po.cameraFocus, 3);
        putF(S, L"camera_field_curve", po.cameraFieldCurve, 3);
        putF(S, L"camera_axis_x", po.cameraAxisX, 3);
        putF(S, L"camera_axis_y", po.cameraAxisY, 3);
        putF(S, L"focus_tilt", po.focusTilt, 3);
        putF(S, L"focus_tilt_angle", po.focusTiltAngle, 1);
        putF(S, L"focus_band_px", po.focusBandPx, 0);
        putF(S, L"focus_tilt_period", po.focusTiltPeriod, 1);
        putF(S, L"focus_tilt_move_s", po.focusTiltMoveS, 2);
        putF(S, L"dof_max_px", po.dofMaxPx, 2);
        putF(S, L"psf_px", po.psfPx, 2);
        putF(S, L"dither", po.dither, 2);
        putI(S, L"dither_auto", po.ditherAuto);
        putF(S, L"mass_black", po.massBlack, 2);
        putI(S, L"mass_black_soft", po.massBlackSoft);                    // post-night T5
        putI(S, L"bloom_ambient", po.bloomAmbient);                       // T7
        putF(S, L"sdr_shoulder", po.sdrShoulder, 2);
        putF(S, L"halation", po.halation, 3);
        putF(S, L"halation_px", po.halationPx, 1);
        putF(S, L"halation_warmth", po.halationWarmth, 2);
        putF(S, L"lid", po.lid, 3);
        putF(S, L"lid_ghost", po.lidGhost, 3);
        putF(S, L"lid_ghost_spread", po.lidGhostSpread, 3);
        putF(S, L"lid_rings", po.lidRings, 3);
        putF(S, L"lid_sheen", po.lidSheen, 3);
        putF(S, L"lid_sheen_px", po.lidSheenPx, 1);
        putF(S, L"lid_glint", po.lidGlint, 3);
        putF(S, L"lid_iris", po.lidIris, 3);
        putF(S, L"lid_refract_px", po.lidRefractPx, 2);
        putF(S, L"lid_scratch", po.lidScratch, 3);
        putF(S, L"lid_scratch_density", po.lidScratchDensity, 3);
        putF(S, L"lid_scratch_len", po.lidScratchLen, 3);
        putF(S, L"lid_scratch_corner", po.lidScratchCorner, 3);
        putF(S, L"lid_scratch_soft", po.lidScratchSoft, 3);
        putF(S, L"lid_scratch_tint", po.lidScratchTint, 3);
        putF(S, L"shimmer", po.shimmer, 3);
        putF(S, L"shimmer_px", po.shimmerPx, 2);
        putF(S, L"vignette_wander", po.vignetteWander, 2);
        putF(S, L"pixel_shift_px", po.pixelShiftPx, 2);
        putF(S, L"rig_readjust", po.rigReadjust, 2);
    }
    {
        const DropConfig& d = c.drops;
        const wchar_t* S = L"drops";
        putI(S, L"drops", d.enabled);
        putF(S, L"interval", d.interval, 2);
        putF(S, L"x_min", d.xMin, 3);   putF(S, L"x_max", d.xMax, 3);
        putF(S, L"y_min", d.yMin, 3);   putF(S, L"y_max", d.yMax, 3);
        putF(S, L"speed", d.speed, 1);
        putF(S, L"radius", d.radius, 3);
        putF(S, L"density", d.density, 3);
        putF(S, L"tail_sec", d.tailSec, 2);
        putF(S, L"tail_density", d.tailDensity, 3);
        putF(S, L"tail_radius_frac", d.tailRadiusFrac, 3);
        putF(S, L"tail_speed", d.tailSpeed, 2);
        putF(S, L"impulse_spread", d.impulseSpread, 3);
        putF(S, L"asymmetry", d.asymmetry, 3);
        putI(S, L"spatter", d.spatter);
        putF(S, L"spatter_radius", d.spatterRadius, 3);
        putF(S, L"spatter_speed", d.spatterSpeed, 2);
        putF(S, L"spatter_spread", d.spatterSpread, 3);
        putI(S, L"color_mode", d.colorMode);
        putI(S, L"obey_governor", d.obeyGovernor);
        if (d.inBlack > 0.0f) putF(S, L"in_black", d.inBlack, 3);   // test key: written only while on
        if (d.inkRise > 0.0f) putF(S, L"ink_rise", d.inkRise, 2);   // test keys (ag-ink): written only while on
        if (!d.pushFilm) putI(S, L"push_film", 0);
        if (d.impulse != 1.0f) putF(S, L"impulse", d.impulse, 2);      // test keys (round 3): only while set
        if (d.inkSwirl > 0.0f) putF(S, L"ink_swirl", d.inkSwirl, 4);
        putRgb(S, L"color", d.color);
    }
    {
        const LiquidAcidConfig& a = c.acid;
        const wchar_t* S = L"liquid_acid";
        putI(S, L"blob_count", a.blobCount);
        putF(S, L"disc_frac", a.discFrac, 3);
        putF(S, L"web_frac", a.webFrac, 3);
        putF(S, L"bubble_frac", a.bubbleFrac, 3);
        putF(S, L"disc_min", a.discMin, 3);   putF(S, L"disc_max", a.discMax, 3);
        putF(S, L"web_min", a.webMin, 3);     putF(S, L"web_max", a.webMax, 3);
        putF(S, L"bubble_min", a.bubbleMin, 3); putF(S, L"bubble_max", a.bubbleMax, 3);
        putF(S, L"hole_min", a.holeMin, 3);   putF(S, L"hole_max", a.holeMax, 3);
        putF(S, L"size_bias", a.sizeBias, 3);
        putF(S, L"big_bias", a.bigBias, 3);
        putF(S, L"hole_weight", a.holeWeight, 3);
        putF(S, L"threshold", a.threshold, 3);
        putF(S, L"support_scale", a.supportScale, 3);
        putF(S, L"aa_scale", a.aaScale, 3);
        putF(S, L"flow_gain", a.flowGain, 3);
        putF(S, L"curl_drift", a.curlDrift, 4);
        putF(S, L"repulsion", a.repulsion, 4);
        putF(S, L"buoyancy", a.buoyancy, 4);
        putF(S, L"damping", a.damping, 3);
        putF(S, L"breath", a.breathAmt, 3);
        putF(S, L"wrap_margin", a.wrapMargin, 3);
        putF(S, L"rise_speed", a.riseSpeed, 4);
        putF(S, L"rise_wobble", a.riseWobble, 3);
        putI(S, L"rise_respawn", a.riseRespawn ? 1 : 0);
        putF(S, L"rise_stretch", a.riseStretch, 3);
        putF(S, L"rise_bottom_light", a.riseBottomLight, 3);
        putF(S, L"rise_parallax", a.riseParallax, 3);
        putF(S, L"rise_parallax_dim", a.riseParallaxDim, 3);
        putF(S, L"oil_drag", a.oilDrag, 3);
        putF(S, L"oil_dye_block", a.oilDyeBlock, 3);
        putF(S, L"oil_viscosity", a.oilViscosity, 3);
        putI(S, L"mouse_oil_mode", a.mouseOilMode);
        putF(S, L"mouse_oil_radius", a.mouseOilRadius, 3);
        putF(S, L"mouse_oil_gain", a.mouseOilGain, 3);
        putF(S, L"rim_width", a.rimWidth, 4);
        putF(S, L"rim_inset", a.rimInset, 4);
        putF(S, L"rim_dark", a.rimDark, 3);
        putF(S, L"rim_vary", a.rimVary, 3);
        putF(S, L"rim_ink_follow", a.rimInkFollow, 3);
        putI(S, L"rim_order", a.rimOrder ? 1 : 0);
        putF(S, L"meniscus", a.meniscus, 3);
        putF(S, L"meniscus_width", a.meniscusW, 4);
        putF(S, L"meniscus_offset", a.meniscusOff, 4);
        putF(S, L"refraction", a.refraction, 4);
        putF(S, L"translucency", a.translucency, 3);
        putF(S, L"oil_texture", a.oilTexture, 3);
        putF(S, L"ink_shading", a.inkShading, 3);
        putF(S, L"oil_hdr", a.oilHdr, 3);
        putF(S, L"rim_hdr", a.rimHdr, 3);
        putF(S, L"oil_thin_edge", a.oilThinEdge, 3);
        putF(S, L"oil_edge_frac", a.oilEdgeFrac, 3);
        putF(S, L"oil_specular", a.oilSpecular, 3);
        putF(S, L"oil_iridescence", a.oilIrid, 3);
        putF(S, L"swarm_lens", a.swarmLens, 3);
        putF(S, L"meniscus_from_ink", a.meniscusFromInk, 3);
        putF(S, L"meniscus_film_mix", a.meniscusFilmMix, 3);
        putF(S, L"oil_glow", a.oilGlow, 3);
        putF(S, L"refraction_width", a.refractionWidth, 2);
        putF(S, L"oil_transparency", a.oilTransparency, 3);
        putF(S, L"oil_absorb", a.oilAbsorb, 2);
        putF(S, L"oil_film_bump", a.oilFilmBump, 3);
        putF(S, L"oil_refract_body", a.oilRefractBody, 4);
        putF(S, L"oil_ink_blur", a.oilInkBlur, 3);
        putF(S, L"ink_levels", a.inkLevels, 1);
        putF(S, L"ink_soft", a.inkSoft, 3);
        putF(S, L"ink_mix", a.inkMix, 3);
        putF(S, L"ink_hue_vary", a.inkHueVary, 1);
        putI(S, L"ink_complement_lock", a.inkComplementLock);
        putF(S, L"ink_complement_span", a.inkComplementSpan, 1);
        putF(S, L"hue_sweep_period", a.hueSweepPeriod, 1);
        putF(S, L"hue_rotate_period", a.hueRotatePeriod, 1);
        putF(S, L"hue_anchor_weight", a.hueAnchorWeight, 3);
        putF(S, L"palette_start_hue", a.paletteStartHue, 1);
        putF(S, L"oil_saturation", a.oilSaturation, 3);
        putI(S, L"sweep_count", a.sweepCount);
        putI(S, L"droplets", a.droplets);
        putF(S, L"droplet_spawn_rate", a.dropletSpawn, 1);
        putF(S, L"droplet_r_min", a.dropletRMin, 4);
        putF(S, L"droplet_r_max", a.dropletRMax, 4);
        putF(S, L"droplet_bias", a.dropletBias, 2);
        putF(S, L"droplet_support", a.dropletSupport, 2);
        putF(S, L"droplet_weight", a.dropletWeight, 3);
        putF(S, L"droplet_oil_weight", a.dropletOilW, 3);
        putF(S, L"droplet_ink_frac", a.dropletInkFrac, 3);
        putF(S, L"droplet_life", a.dropletLife, 1);
        putF(S, L"droplet_damping", a.dropletDamping, 2);
        putF(S, L"droplet_jitter", a.dropletJitter, 4);
        putF(S, L"droplet_attract", a.dropletAttract, 3);
        putF(S, L"droplet_merge", a.dropletMerge, 3);
        putF(S, L"droplet_rise", a.dropletRise, 3);
        putI(S, L"accent_mode", a.accentMode);
        putF(S, L"accent_max_r", a.accentMaxR, 3);
        putF(S, L"accent_frac", a.accentFrac, 3);
        putI(S, L"oil_edge_mode", a.oilEdgeMode);
        putF(S, L"oil_edge_curve", a.oilEdgeCurve, 3);
        putF(S, L"oil_penumbra", a.oilPenumbra, 3);
        putF(S, L"oil_penumbra_px", a.oilPenumbraPx, 1);
        putF(S, L"oil_penumbra_hue", a.oilPenumbraHue, 1);
        putF(S, L"oil_penumbra_dark", a.oilPenumbraDark, 3);
        putF(S, L"cellulose", a.cellulose, 3);
        putF(S, L"cellulose_ink", a.celluloseInk, 3);
        putF(S, L"cellulose_oil", a.celluloseOil, 3);
        putF(S, L"cellulose_scale", a.celluloseScale, 1);
        putF(S, L"cellulose_drift", a.celluloseDrift, 3);
        putF(S, L"droplet_ring_frac", a.dropletRingFrac, 3);
        putF(S, L"droplet_ring_width", a.dropletRingWidth, 3);
        putF(S, L"droplet_ring_lift", a.dropletRingLift, 3);
        putF(S, L"droplet_ring_clump", a.dropletRingClump, 3);
        putF(S, L"weather", a.weather, 3);
        putF(S, L"weather_period_s", a.weatherPeriodS, 1);
        putF(S, L"film_hue2", a.filmHue2, 1);
        putF(S, L"film_hue2_amt", a.filmHue2Amt, 3);
        putF(S, L"film_hue2_scale", a.filmHue2Scale, 3);
        putF(S, L"film_hue2_drift", a.filmHue2Drift, 3);
        putF(S, L"film_hue2_decay", a.filmHue2Decay, 3);
        putF(S, L"film_hue3", a.filmHue3, 1);
        putF(S, L"film_hue3_amt", a.filmHue3Amt, 3);
        putF(S, L"film_hue3_share", a.filmHue3Share, 3);
        putF(S, L"film_hue2_cover", a.filmHue2Cover, 3);
        putF(S, L"film_equal_load", a.filmEqualLoad, 3);
        putF(S, L"film_equal_load_patches", a.filmEqualLoadP, 3);
        putF(S, L"film_hue2_seam", a.filmHue2Seam, 3);
        putF(S, L"film_hue2_seam_stretch", a.filmHue2SeamStretch, 2);    // T8b
        putF(S, L"film_hue2_leak", a.filmHue2Leak, 2);
        putF(S, L"film_hue2_leak_reach", a.filmHue2LeakReach, 2);
        putF(S, L"film_hue2_leak_core", a.filmHue2LeakCore, 2);
        putF(S, L"film_hue2_leak_in", a.filmHue2LeakIn, 2);
        putF(S, L"crust_hue_mix", a.crustHueMix, 3);
        putF(S, L"film_hue2_wobble", a.filmHue2Wobble, 2);
        putF(S, L"film_hue2_seed_rows", a.filmHue2SeedRows, 0);
        putF(S, L"film_hue2_rise", a.filmHue2Rise, 3);
        putF(S, L"film_hue2_refresh", a.filmHue2Refresh, 0);
        putF(S, L"film_hue2_lobes", a.filmHue2Lobes, 2);                // T8
        putF(S, L"film_hue2_wobble_period", a.filmHue2WobbleP, 1);
        putF(S, L"boundary_reflect_r", a.boundaryReflectR, 3);
        putF(S, L"boundary_reflect_amt", a.boundaryReflectAmt, 3);
        putF(S, L"droplet_mass_bias", a.dropletMassBias, 3);
        putF(S, L"droplet_crust_density", a.dropletCrustDens, 3);
        putF(S, L"droplet_crust_r", a.dropletCrustR, 3);
        putF(S, L"mass_rim", a.massRim, 3);
        putF(S, L"shadow_amt", a.shadowAmt, 3);
        putF(S, L"shadow_len", a.shadowLen, 3);
        putF(S, L"shadow_soft", a.shadowSoft, 3);
        putF(S, L"droplet_racer_frac", a.dropletRacerFrac, 3);
        putF(S, L"droplet_racer_speed", a.dropletRacerSpeed, 3);
        putF(S, L"droplet_racer_wobble", a.dropletRacerWobble, 3);
        putF(S, L"droplet_racer_r_max", a.dropletRacerRMax, 4);
        putF(S, L"droplet_ring_r_mul", a.dropletRingRMul, 3);
        putF(S, L"droplet_ring_big_frac", a.dropletRingBigFrac, 3);
        if (a.dropletBigFrac > 0.0f) {     // test keys: written only while on, so a saved ini stays as it was
            putF(S, L"droplet_big_frac", a.dropletBigFrac, 3);
            putF(S, L"droplet_big_r_mul", a.dropletBigRMul, 2);
        }
        putF(S, L"droplet_coalesce", a.dropletCoalesce, 3);
        putF(S, L"droplet_coalesce_s", a.dropletCoalesceS, 3);
        putF(S, L"conserve_mass", a.conserveMass, 3);
        putF(S, L"spawn_grow_s", a.spawnGrowS, 3);
        putF(S, L"dissolve_s", a.dissolveS, 3);
        putF(S, L"blob_turnover_s", a.blobTurnoverS, 3);   // brief BZ
        putF(S, L"droplet_ring_wobble", a.dropletRingWobble, 3);
        putF(S, L"droplet_depth", a.dropletDepth, 3);
        putF(S, L"depth_rise", a.depthRise, 3);
        putF(S, L"depth_amount", a.depthAmount, 2);
        putF(S, L"diffraction", a.diffraction, 3);
        putF(S, L"diffraction_px", a.diffractionPx, 2);
        putF(S, L"droplet_lens", a.dropletLens, 3);
        putF(S, L"droplet_lens_centre", a.dropletLensCentre, 3);
        putF(S, L"droplet_lens_band", a.dropletLensBand, 2);
        putF(S, L"droplet_spec", a.dropletSpec, 3);
        putF(S, L"droplet_shadow", a.dropletShadow, 3);
        putF(S, L"droplet_wobble", a.dropletWobble, 3);
        putF(S, L"droplet_core", a.dropletCore, 3);
        putI(S, L"droplet_shadow_lamp", a.dropletShadowLamp);            // post-night T4
        putF(S, L"droplet_haze_rim", a.dropletHazeRim, 3);
        putI(S, L"oil_spec_tint", a.oilSpecTint);                        // T7
        putF(S, L"dye_depth", a.dyeDepth, 3);
        putF(S, L"dye_depth_tilt", a.dyeDepthTilt, 3);
        putF(S, L"dye_depth_w", a.dyeDepthW, 3);
        putF(S, L"dye_hue", a.dyeHue, 1);
        putF(S, L"dye_sat", a.dyeSat, 3);
        putF(S, L"dye_lum", a.dyeLum, 3);
        putI(S, L"dye_hue_follow", a.dyeHueFollow);
        putF(S, L"dye_droplets", a.dyeDroplets, 3);
        putF(S, L"dye_masses", a.dyeMasses, 3);
        putF(S, L"dye_droplet_hue", a.dyeDropHue, 1);
        putF(S, L"dye_droplet_sat", a.dyeDropSat, 3);
        putF(S, L"dye_droplet_lum", a.dyeDropLum, 3);
        putF(S, L"dye_smoke", a.dyeSmoke, 3);
        putF(S, L"film_level", a.filmLevel, 3);
        putF(S, L"oil_fluor", a.oilFluor, 3);
        putF(S, L"oil_fluor_reach", a.oilFluorReach, 3);
        putF(S, L"dye_lamp_follow", a.dyeLampFollow, 3);
        putF(S, L"dark_sat", a.darkSat, 3);
        putF(S, L"dye_lum_vary", a.dyeLumVary, 3);
        putF(S, L"dye_hue_vary", a.dyeHueVary, 1);
        putF(S, L"dye_thick_hue", a.dyeThickHue, 1);
        putF(S, L"dye_core", a.dyeCore, 3);
        putI(S, L"oil_height", a.oilHeight);                             // Oil3D Stage A
        putF(S, L"oil_height_px", a.oilHeightPx, 1);
        putF(S, L"oil3d_absorb", a.oil3dAbsorb, 3);                      // Oil3D Stage B
        putF(S, L"tank_depth", a.tankDepth, 3);                          // Oil3D C1
        putF(S, L"light_elev", a.lightElev, 1);
        putF(S, L"light_azim", a.lightAzim, 1);
        putF(S, L"light_azim_drift", a.lightAzimDrift, 2);
        putF(S, L"light_size", a.lightSize, 1);
        putF(S, L"shadow_on_film", a.shadowOnFilm, 3);
        putF(S, L"water_scatter", a.waterScatter, 3);                    // Oil3D C2
        putF(S, L"water_sigma", a.waterSigma, 3);
        putF(S, L"water_murk", a.waterMurk, 3);                          // Oil3D W1 (murk)
        putF(S, L"murk_clouds", a.murkClouds, 3);
        putF(S, L"murk_waves", a.murkWaves, 3);
        putF(S, L"murk_strands", a.murkStrands, 3);
        putF(S, L"murk_clumps", a.murkClumps, 3);
        putF(S, L"murk_shape", a.murkShape, 3);
        putF(S, L"murk_warp", a.murkWarp, 3);
        putF(S, L"murk_scale", a.murkScale, 3);
        putF(S, L"murk_drift", a.murkDrift, 3);
        putF(S, L"murk_shadow", a.murkShadow, 3);
        putF(S, L"murk_scatter", a.murkScatter, 3);
        putF(S, L"murk_light", a.murkLight, 3);
        putF(S, L"murk_grain", a.murkGrain, 3);                          // Oil3D W3 (fine silt + haze)
        putF(S, L"murk_grain_px", a.murkGrainPx, 2);
        putI(S, L"murk_grain_slices", a.murkGrainSlices);
        putF(S, L"murk_haze", a.murkHaze, 3);
        putF(S, L"murk_haze_lift", a.murkHazeLift, 3);
        putF(S, L"murk_cloud_share", a.murkCloudShare, 3);
        putI(S, L"motes", a.motes);                                      // Oil3D C3
        putF(S, L"mote_px", a.motePx, 3);
        putF(S, L"mote_bright", a.moteBright, 3);
        putF(S, L"mote_depth_bias", a.moteDepthBias, 3);
        putF(S, L"mote_glint", a.moteGlint, 3);
        putF(S, L"water_tint", a.waterTint, 3);                          // Oil3D W2
        putF(S, L"water_hue", a.waterHue, 1);
        putF(S, L"silt_tint", a.siltTint, 3);
        putF(S, L"silt_hue", a.siltHue, 1);
        putI(S, L"colour_director", a.colourDirector);                   // Oil3D L4
        putF(S, L"director_rest", a.directorRest, 1);
        putF(S, L"director_trip", a.directorTrip, 1);
        putF(S, L"director_mix", a.directorMix, 3);
        putI(S, L"director_demo", a.directorDemo);
        putI(S, L"director_style", a.directorStyle);
        putF(S, L"director_hold", a.directorHold, 1);                    // night-cadence
        putF(S, L"director_move", a.directorMove, 1);
        putF(S, L"director_turn", a.directorTurn, 1);
        putI(S, L"director_water", a.directorWater);                     // post-night T3
        putI(S, L"director_turn_dark", a.directorTurnDark);              // post-night T5
        putF(S, L"director_band_lo", a.directorBandLo, 1);               // T8 band
        putF(S, L"director_band_hi", a.directorBandHi, 1);
        putF(S, L"director_pour_k", a.directorPourK, 2);                 // T8 blob
        putF(S, L"director_accent", a.directorAccent, 1);                // T8 accent (deg)
        putF(S, L"director_pour_ramp", a.directorPourRamp, 1);           // T8 pour
        putF(S, L"director_dark_share", a.directorDarkShare, 2);       // T8 order
        putI(S, L"director_dark_path", a.directorDarkPath);            // T8 direct darkroom
        putF(S, L"lamp1_lo", a.lamp1Lo, 1);                              // Oil3D L1
        putF(S, L"lamp1_hi", a.lamp1Hi, 1);
        putF(S, L"lamp1_lo2", a.lamp1Lo2, 1);
        putF(S, L"lamp1_hi2", a.lamp1Hi2, 1);
        putF(S, L"lamp1_feather", a.lamp1Feather, 1);
        putF(S, L"lamp1_level", a.lamp1Level, 3);
        putF(S, L"lamp2_lo", a.lamp2Lo, 1);                              // Oil3D L2
        putF(S, L"lamp2_hi", a.lamp2Hi, 1);
        putF(S, L"lamp2_lo2", a.lamp2Lo2, 1);
        putF(S, L"lamp2_hi2", a.lamp2Hi2, 1);
        putF(S, L"lamp2_feather", a.lamp2Feather, 1);
        putF(S, L"lamp2_level", a.lamp2Level, 3);
        putF(S, L"lamp2_elev", a.lamp2Elev, 1);
        putF(S, L"lamp2_azim", a.lamp2Azim, 1);
        putF(S, L"lamp2_size", a.lamp2Size, 1);
        WritePrivateProfileStringW(S, L"dye_a2", a.dyeA2Name, path);
        WritePrivateProfileStringW(S, L"dye_b1", a.dyeB1Name, path);
        WritePrivateProfileStringW(S, L"dye_b2", a.dyeB2Name, path);
        putF(S, L"dye_a2_amount", a.dyeA2Amount, 3);
        putF(S, L"dye_b1_amount", a.dyeB1Amount, 3);
        putF(S, L"dye_b2_amount", a.dyeB2Amount, 3);
        putF(S, L"oil_dye", a.oilDye, 1);
        putF(S, L"oil_dye_amount", a.oilDyeAmount, 3);
        putF(S, L"lamp_grey", a.lampGrey, 3);
        putF(S, L"lamp_grey_size", a.lampGreySize, 3);
        putF(S, L"lamp_grey_cool", a.lampGreyCool, 3);
        putF(S, L"lamp_grey_heart", a.lampGreyHeart, 3);
        putF(S, L"lamp_grey_heart_size", a.lampGreyHeartSize, 3);
        putF(S, L"lamp_grey_heart_lift", a.lampGreyHeartLift, 3);
        putF(S, L"shadow_tone", a.shadowTone, 3);
        putF(S, L"shadow_tone_lift", a.shadowToneLift, 3);
        putF(S, L"shadow_tone_sat", a.shadowToneSat, 3);
        putF(S, L"shadow_tone_period", a.shadowTonePeriod, 0);
        putF(S, L"shadow_tone_fade", a.shadowToneFade, 0);
        putF(S, L"shadow_tone_hue", a.shadowToneHue, 0);
        putF(S, L"highlight_tone_hue", a.highlightToneHue, 0);
        putF(S, L"highlight_tone_amt", a.highlightToneAmt, 3);
        putF(S, L"highlight_tone_sat", a.highlightToneSat, 3);
        putF(S, L"highlight_tone_desat", a.highlightToneDesat, 3);
        putF(S, L"tone_balance", a.toneBalance, 3);
        putF(S, L"post_chroma", a.postChroma, 3);
        putF(S, L"post_lift", a.postLift, 3);
        WritePrivateProfileStringW(S, L"ink_mode", a.inkMode == 1 ? L"water" : L"bands", path);
        WritePrivateProfileStringW(S, L"ink_water", nullptr, path);   // delete the int form
        putF(S, L"ink_gain", a.inkGain, 3);
        putF(S, L"ink_bias", a.inkBias, 3);
        putF(S, L"seam_strength", a.seamStrength, 3);
        putF(S, L"seam_lo", a.seamLo, 3);
        putF(S, L"seam_hi", a.seamHi, 3);
        putF(S, L"seam_scale", a.seamScale, 2);
        putF(S, L"grain", a.grainAmt, 4);
        putF(S, L"grain_scale", a.grainScale, 2);
        putF(S, L"grain_shadow_weight", a.grainShadowW, 3);
        putF(S, L"toe_tint", a.toeTint, 3);
        putF(S, L"speckle", a.speckle, 3);
        putF(S, L"speckle_scale", a.speckScale, 1);
        putF(S, L"swarm_holes", a.swarmHoles, 3);
        putF(S, L"swarm_drops", a.swarmDrops, 3);
        putF(S, L"swarm_density", a.swarmDensity, 3);
        putF(S, L"swarm_scale_holes", a.swarmScaleA, 1);
        putF(S, L"swarm_scale_drops", a.swarmScaleB, 1);
        putF(S, L"swarm_r_min", a.swarmRMin, 3);
        putF(S, L"swarm_r_max", a.swarmRMax, 3);
        putF(S, L"swarm_rim_dark", a.swarmRimDark, 3);
        putF(S, L"swarm_drift", a.swarmDrift, 4);
        putF(S, L"swarm_clump", a.swarmClump, 3);
        putF(S, L"swarm_dark", a.swarmDark, 3);
        putRgb(S, L"meniscus_color", a.meniscusCol);
        // Canonical spelling on write is the short form; the legacy
        // sweep_pair_N_* keys are deleted so a 12-float palette can never be
        // shadowed by a stale 3-float anchor left behind in the same file.
        {
            int nw = a.sweepCount > LiquidAcidConfig::kSweepPairs
                   ? a.sweepCount : LiquidAcidConfig::kSweepPairs;
            if (nw > LiquidAcidConfig::kSweepMax) nw = LiquidAcidConfig::kSweepMax;
            for (int ci = 0; ci < LiquidAcidConfig::kSweepMax; ci++) {
                wchar_t key[40];
                swprintf_s(key, L"sweep_pair_%d_oil", ci + 1);
                WritePrivateProfileStringW(S, key, nullptr, path);
                swprintf_s(key, L"sweep_pair_%d_ink", ci + 1);
                WritePrivateProfileStringW(S, key, nullptr, path);
                swprintf_s(key, L"sweep_oil_%d", ci + 1);
                if (ci >= nw) { WritePrivateProfileStringW(S, key, nullptr, path); }
                else if (a.sweepOilFullSet[ci]) {
                    WritePrivateProfileStringW(S, key, fmtList(&a.sweepOilFull[ci * 12], 12).c_str(), path);
                } else {
                    putRgb(S, key, &a.sweepOil[ci * 3]);
                }
                swprintf_s(key, L"sweep_ink_%d", ci + 1);
                if (ci >= nw) WritePrivateProfileStringW(S, key, nullptr, path);
                else          putRgb(S, key, &a.sweepInk[ci * 3]);
            }
        }
        for (int ci = 0; ci < 4; ci++) {
            wchar_t key[32];
            swprintf_s(key, L"oil_color_%d", ci + 1);
            putRgb(S, key, &a.oilColors[ci * 3]);
            swprintf_s(key, L"ink_stop_%d", ci + 1);
            putRgb(S, key, &a.inkRamp[ci * 3]);
        }
    }
}

static void SaveFullConfig(const FluidConfig& c) {
    if (!g_iniPath[0]) return;
    WriteConfigToIni(g_iniPath, c, true);
    SaveSettings();   // shell globals: pauses, peak, gamut, cycle config
}

// ---- RATINGS (user 2026-10-30: "a shortcut that I can press that's like kind of a rating system or maybe it
// just feeds you data and then you manually do it"). Ctrl+Alt+Up = like, Ctrl+Alt+Down = dislike, while the
// wallpaper runs. Each press appends ONE line to ratings.log beside settings.ini (what is on screen: the cycle
// stage, the colour director's combo and move, the palette hue, the colour / lamp / water keys) and writes the
// LIVE config as ratings\<time>-like|dislike.ini (the colour director off, the palette starting at the hue on
// screen), so the moment can be re-rendered headless (--shot --ini <that file>). Nothing changes by itself; a
// quiet tray balloon confirms the press.
static void RegisterRateHotkeys(HWND hwnd) {
    const bool up = RegisterHotKey(hwnd, kHotRateUp, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_UP) != 0;
    const bool dn = RegisterHotKey(hwnd, kHotRateDown, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_DOWN) != 0;
    WpLog("ratings: Ctrl+Alt+Up %s, Ctrl+Alt+Down %s", up ? "registered" : "TAKEN by another app",
          dn ? "registered" : "TAKEN by another app");
}
static void RateNow(int rating) {
    if (!g_renderer || !g_iniPath[0]) return;
    const FluidConfig& c = g_renderer->Config();
    const LiquidAcidConfig& a = c.acid;
    wchar_t dir[MAX_PATH], logp[MAX_PATH], snap[MAX_PATH], stamp[32];
    wcscpy_s(dir, g_iniPath);
    if (wchar_t* sl = wcsrchr(dir, L'\\')) *sl = 0;
    _snwprintf_s(logp, _TRUNCATE, L"%s\\ratings.log", dir);
    wcscat_s(dir, L"\\ratings");
    CreateDirectoryW(dir, nullptr);
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snwprintf_s(stamp, _TRUNCATE, L"%04d%02d%02d-%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    _snwprintf_s(snap, _TRUNCATE, L"%s\\%s-%s.ini", dir, stamp, rating > 0 ? L"like" : L"dislike");
    const float hue = g_renderer->DirectorHue() >= 0.0f ? g_renderer->DirectorHue() : g_renderer->PaletteHueRaw();
    const CycleStatus cs = CycleState();
    const CycleConfig& cc = CycleGet();
    std::wstring stage = (cs.stage >= 0 && cs.stage < (int)cc.stages.size()) ? cc.stages[cs.stage].file : L"-";
    const char* look = c.ink.enabled ? "ink" : (a.enabled ? "oil" : "fluid");
    char line[1400];
    snprintf(line, sizeof(line),
             "%04d-%02d-%02d %02d:%02d:%02d %s look=%s stage=%d %ls | hue %.1f hue2 %+.0f x%.2f | dyes a2 %.2f@%+.0f b1 %.2f@%+.0f"
             " | lamp1 %.0f..%.0f x%.2f lamp2 %.2f | water tint %.2f@%+.0f silt %.2f@%+.0f murk %.2f motes %d"
             " | director %s | %ls\r\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, rating > 0 ? "LIKE   " : "DISLIKE",
             look, cs.stage + 1, stage.c_str(), hue, a.filmHue2, a.filmHue2Amt, a.dyeA2Amount, a.filmDyeHue[1],
             a.dyeB1Amount, a.filmDyeHue[2], a.lamp1Lo, a.lamp1Hi, a.lamp1Level, a.lamp2Level, a.waterTint, a.waterHue,
             a.siltTint, a.siltHue, a.waterMurk, a.motes, DirectorStatusLine().c_str(), snap);
    if (FILE* f = _wfsopen(logp, L"ab", _SH_DENYNO)) { fputs(line, f); fclose(f); }
    // the moment as a full ini: the director's live keys as they are, the director off, the palette
    // starting at the hue on screen (its own rotation runs on from there)
    FluidConfig sc = c;
    sc.acid.colourDirector = false;
    if (a.enabled) sc.acid.paletteStartHue = fmodf(hue + 360.0f, 360.0f);
    WriteConfigToIni(snap, sc, false);
    line[strcspn(line, "\r\n")] = 0;
    WpLog("ratings: %s", line);
    ShowTrayBalloon(rating > 0 ? L"Rated: like" : L"Rated: dislike", L"Saved what is on screen (ratings.log)", true);
}

// A preset's [look] keys, or those of the [meta] base chain under it.
static bool PresetCarriesLook(const std::wstring& path, int depth = 0) {
    for (const wchar_t* k : { L"style", L"liquid_acid", L"ink" }) {
        wchar_t lk[32] = {};
        GetPrivateProfileStringW(L"look", k, L"", lk, 32, path.c_str());
        if (lk[0]) return true;
    }
    if (depth >= 4) return false;
    const std::wstring base = PresetMetaBase(path);
    return !base.empty() && PresetCarriesLook(base, depth + 1);
}

// Merge a preset onto cfg: its [meta] base chain first (FINAL-CYCLE B.4, the
// way the settings window applies a partial "Save as"), then the file.
static void MergePresetChain(const std::wstring& path, FluidConfig& cfg, int depth = 0) {
    if (depth < 4) {
        const std::wstring base = PresetMetaBase(path);
        if (!base.empty()) {
            MergePresetChain(base, cfg, depth + 1);
            printf("preset base applied: %ls\n", base.c_str());
        }
    }
    LoadConfigFromIni(path.c_str(), cfg);
}

// [hdr] peak_nits / gamut along the same chain (file over base).
static void ApplyPresetShellChain(const std::wstring& path, int depth = 0) {
    if (depth < 4) {
        const std::wstring base = PresetMetaBase(path);
        if (!base.empty()) ApplyPresetShellChain(base, depth + 1);
    }
    wchar_t buf[64] = {};
    GetPrivateProfileStringW(L"hdr", L"peak_nits", L"", buf, 64, path.c_str());
    if (buf[0]) g_hdrPeakNits = (float)_wtof(buf);
    int gm = (int)GetPrivateProfileIntW(L"hdr", L"gamut", g_gamutMode, path.c_str());
    if (gm >= 0 && gm <= 2) g_gamutMode = gm;
}

static void ApplyPreset(const std::wstring& path) {
    if (!g_renderer) return;
    // A LOOK preset ([look] keys) picked by hand while cycling: the user took
    // the wheel, the director lets go for this session ([cycle] enabled is
    // untouched: the cycle resumes on the next start, or from the tray). A
    // partial overlay without [look] (the mirror overlays) folds the current
    // stage instead and lasts until the next stage switch; nothing is written
    // to settings.ini for it while the director owns the look.
    // (a partial whose [meta] base carries a [look] is a look preset too)
    const bool lookPreset = PresetCarriesLook(path);
    if (lookPreset || !g_cycleActive) {
        CycleManualOverride("look preset applied by hand");
        g_renderer->SetFade(1.0f);
        g_renderer->SetBlackOut(false);
    }
    // The settings window stays open (UI-REHAUL R1: it reads every value live each frame,
    // so there is nothing to rebuild). The UI model snapshots the config for one undo entry.
    // --shot (read-only) never involves the UI model; neither does an overlay applied while
    // the director owns the look (no settings.ini write while cycling).
    const bool uiTrack = !g_configReadOnly && !g_cycleActive;
    if (uiTrack) UiBeginWholeChange();

    // merge the (possibly partial) preset over the current state: its [meta]
    // base chain first, then the file (B.4)
    FluidConfig fresh = g_renderer->Config();
    MergePresetChain(path, fresh);
    // LIVE: the machine keys never come from a preset (AGENTS.md hard constraint:
    // sim_res / dye_res stay at the live 256 / 4096; the cycle's Compose pins the
    // same four). Old user presets still carry them (review 2026-10-04 R5: two with
    // sim_res=512). --shot keeps taking them, so the identity baselines do not move.
    if (!g_configReadOnly) {
        const FluidConfig& live = g_renderer->Config();
        fresh.simRes = live.simRes;
        fresh.dyeRes = live.dyeRes;
        fresh.fpsLimit = live.fpsLimit;
        fresh.mirrorSecond = live.mirrorSecond;
    }

    // shell globals: take the preset's value only when it specifies one
    ApplyPresetShellChain(path);

    int oldSim = g_renderer->Config().simRes;
    int oldDye = g_renderer->Config().dyeRes;
    g_renderer->Config() = fresh;
    if (fresh.simRes != oldSim || fresh.dyeRes != oldDye)
        g_renderer->SetResolutions(fresh.simRes, fresh.dyeRes);
    else
        g_renderer->ReinitWanderers();
    // A preset may carry [look] style=ink / liquid_acid. The display PSO for
    // that look is only built at device creation for the look that was on
    // then, so build it now or the look would silently render as fluid.
    g_renderer->EnsureLookResources();

    if (!g_cycleActive) SaveFullConfig(fresh);   // cycling: in memory only
    UpdateTrayTip();
    if (uiTrack) UiEndWholeChange(path);   // [ui] active_preset / overlays + undo entry

    const wchar_t* name = wcsrchr(path.c_str(), L'\\');
    ShowTrayBalloon(L"Preset applied", name ? name + 1 : path.c_str());
    printf("preset applied: %ls\n", path.c_str());
}

static void SaveCurrentAsPreset() {
    // new snapshots go to the presets folder (no shell keys)
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    GetPresetsDir(dir);
    CreateDirectoryW(dir, nullptr);
    // CLOCKS pre-flight 3: a snapshot takes the BASE, never base x F
    FluidConfig snap;
    if (g_renderer) { snap = g_renderer->Config(); ClocksBaseCopy(snap); }
    // ini = live state (the clocks' base). Cycling: in memory only, as in ApplyPreset --
    // the composed stage is not the user's own base (review 2026-10-04 R4).
    if (g_renderer && !g_cycleActive) SaveFullConfig(snap);
    wchar_t name[64] = {};
    for (int n = 1; n < 100 && !name[0]; n++) {
        swprintf_s(path, L"%s\\Preset %d.ini", dir, n);
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) swprintf_s(name, L"Preset %d", n);
    }
    // Preset 1..99 all taken: a timestamped name, so a save always lands
    // (review 2026-10-04 U15).
    if (!name[0]) {
        SYSTEMTIME st; GetLocalTime(&st);
        swprintf_s(name, L"Preset %04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay,
                   st.wHour, st.wMinute, st.wSecond);
        swprintf_s(path, L"%s\\%s.ini", dir, name);
    }
    if (g_renderer) WriteConfigToIni(path, snap, false);
    if (!g_configReadOnly && UiModelReady()) UiNotifyPresetSaved(path);
    wchar_t msg[160];
    swprintf_s(msg, L"Saved as \"%s\" — rename the file in the presets folder if you like.", name);
    ShowTrayBalloon(L"Preset saved", msg);
}

// wrappers for the scenes window (the statics above stay file-local)
void ApplyPresetPath(const std::wstring& path) { ApplyPreset(path); }
void SaveCurrentAsPresetFile() { SaveCurrentAsPreset(); }
void GetPresetsDirectory(wchar_t out[MAX_PATH]) { GetPresetsDir(out); }
void GetPhotosDirectory(wchar_t out[MAX_PATH]) { GetPhotosDir(out); }
void PersistShellSettings() { SaveSettings(); }
void PersistFullConfigNow() {
    if (!g_renderer) return;
    FluidConfig snap = g_renderer->Config();   // CLOCKS pre-flight 3: the base, never base x F
    ClocksBaseCopy(snap);
    SaveFullConfig(snap);
}

// ---------------------------------------------------------------------------
// --shot: headless deterministic capture
//
// Renders the wallpaper offscreen (no window, no WorkerW, no tray, no focus
// change, no writes to the live config) and dumps the display pass to PNG.
// The renderer runs on a FIXED timestep with no vsync and no sleeping, so
// "40 seconds of wallpaper" costs a few seconds of wall clock.
// ---------------------------------------------------------------------------

struct ShotOpts {
    std::wstring out;
    std::wstring ini;
    int      width = 2560, height = 1440;
    float    delaySec = 40.0f;
    int      seriesCount = 1;       // --shot-series N:interval
    float    seriesInterval = 0.0f;
    int      shotFps = 144;         // --shot-fps N: simulated steps per second (30..240), 144 = before
    unsigned seed = 1234;
    bool     hdrOn = false;
    float    sdrWhiteNits = 240.0f;
    float    panelMaxNits = 1000.0f;   // stands in for the DXGI-reported max
    int      sdrGamut = -1;            // --sdr-gamut 0|1|2 (default: the ini's [hdr] sdr_gamut)
    float    panelPrim[6] = {};        // --panel-primaries rx,ry,gx,gy,bx,by (stands in for DXGI)
    bool     panelPrimSet = false;
    bool     sdrArgBad = false;        // --sdr-gamut / --panel-primaries did not parse: refuse the run
    bool     mouseNone = true;
    int      yieldMs = 2;              // --shot-yield ms: sleep per simulated frame
    int      gameFps = 0;              // --shot-game-fps N: cap at N fps while a game is in front
    std::wstring gameLog;              // --shot-game-log <path>: tools\game-pause-live.ps1's log
    double   time0 = 0.0;             // --shot-time0: the master clock's start (seconds of uptime)
    int      oilDebug = 0;             // --shot-debug height|normal|contour (Oil3D Stage A)
    bool     lamp2Capped = false;      // --shot-lamp2-capped: L2's capped two-lamp reference (Oil3D L3 A/B)
    bool     timings = false;          // --timings: GPU ms per pass, logged at every shot
    std::wstring dumpPath;             // --shot-dump <json>: the UE OilTank dump (DUMP-SCHEMA v1)
    std::vector<int> stageTaps;        // --shot-stage-tap <list>: Oil3D L3b stage taps (-1 = the lamps map)
    std::wstring iniChain;             // --shot-ini-chain "a|b|c": recorded in the dump only
    // --test-device-lost T (review 2026-10-04 R3): remove the device at wallpaper time T
    float    devLostAt = -1.0f;
    bool     murkAsync = false;        // --test-murk-async (post-night T1)
    bool     devLostRemoved = false;
    bool     devLostAwaitFirst = false;   // re-init done, the next good frame is logged
    // --snapshot-at T <png> (ui3 B5): the renderer's snapshot of the frame at T, as the Settings window gets it
    float    snapAt = -1.0f;
    std::wstring snapPng;
    bool     snapAsked = false, snapRecorded = false, snapDone = false;
    long long snapAskFrame = -1, snapRecFrame = -1;
    // --shot-pour X,Y,START,DUR : hold LMB at (X,Y) px from START for DUR seconds,
    // with a slow circular drift (radius 40 px, 0.5 rev/s) like a resting hand.
    bool     pour = false;
    float    pourX = 1280, pourY = 720, pourStart = 0, pourDur = 0;
    // --shot-drop X,Y,T[,VY] : one ink drop at (X,Y) px at wallpaper time T,
    // with downward impulse VY (default [drops] speed). The reproducible
    // single-drop test for the ink look.
    bool     drop = false;
    float    dropX = 1280, dropY = 200, dropAt = 0, dropVy = -1.0f;
    bool     dropFired = false;
    // --shot-mouse X,Y,DX,DY,T[,DUR] : one synthetic pointer STROKE, from
    // (X,Y) px to (X+DX, Y+DY) px, starting at wallpaper time T and taking DUR
    // seconds (default 0.5). The reproducible test for [liquid_acid]
    // mouse_oil_mode -- a single one-frame sample would carry almost no
    // pointer velocity, and a stroke is what a comb or a push actually is.
    // It only ever sets FrameInput's pointer fields, so with show_mouse /
    // hold_to_splat / splat_on_click at 0 (every rise preset) it cannot put a
    // single splat of dye into the ink.
    bool     mouse = false;
    float    mouseSx = 1280, mouseSy = 720, mouseDx = 0, mouseDy = 0;
    float    mouseAt = 0, mouseDur = 0.5f;
    // --shot-preset <ini> AT <sec> : apply that preset at wallpaper time <sec>
    // through the SAME ApplyPreset() the tray menu uses. This is how a look
    // SWITCH is verified headlessly: start on one ini, switch mid-run, capture
    // after. (The shot process is config-read-only, so ApplyPreset's write-back
    // is a no-op and nothing on disk is touched.)
    std::wstring presetIni;
    float    presetAt = 0.0f;
    bool     presetApplied = false;
    // Cycle director under --shot. [cycle] is read from the --ini file; these
    // override it: --cycle (force on), --cycle-dwell S, --cycle-jitter J,
    // --cycle-order fixed|alternate_random, --cycle-seed N (default: --seed),
    // --cycle-stage N (1-based start stage).
    bool     cycleForce = false;
    float    cycleDwell = -1.0f, cycleJitter = -1.0f;
    int      cycleOrder = -1;
    unsigned cycleSeed = 0;
    int      cycleStage = -1;
    // --cycle-next-at T[,T2...]: CycleNext() at wallpaper time T, in process
    // (the same call the tray's "Next stage" makes). --cycle-pause-at T1,T2:
    // CyclePause() at T1, CycleResume() at T2 (animators.h, headless).
    // --shot-freeze NAME,T1,T2: Freeze(animator) at T1, Unfreeze at T2
    // (NAME = palette|hue2|rig|hueshift|transition).
    std::vector<float> cycleNextAt;
    size_t   cycleNextDone = 0;
    // --cycle-burst-at T[,T..]: CycleBurst() at wallpaper time T (the tamed
    // burst on the current oil stage, forced). --cycle-draw-test N: the draw
    // statistics over N tiered draws (no renderer, no GPU), then exit.
    std::vector<float> cycleBurstAt;
    size_t   cycleBurstDone = 0;
    // --shot-queue "<combo>:<mode>@<sec>[,...]": DirectorQueueLook(combo, mode) at wallpaper time sec (the Settings
    // window's "show next"; mode 0 vivid / 1 darkroom; combo = a kComboSeed name such as blue_coral)
    struct QueueAt { std::wstring combo; int mode; float t; };
    std::vector<QueueAt> shotQueue;
    size_t   shotQueueDone = 0;
    // ui3 --cycle-keep-at T,SEC: the Settings window's Keep this (CycleKeepThis) at wallpaper time T for SEC s.
    // --cycle-visit-at T,STAGE,SEC: a visit to stage STAGE (1-based; a rated look) at T for SEC s (CycleVisit).
    // Either one makes the wallpaper time the hold clock (CycleHoldSetClock), so the [keep] / [visit] lines are exact.
    float    keepAt = -1.0f, keepSec = 0.0f;
    float    visitAt = -1.0f, visitSec = 0.0f;
    int      visitStage = 0;
    int      visitGuest = -1;          // ui3 --cycle-visit-at T,ink|fluid,SEC: 0 Ink / 1 Fluid (a guest visit), -1 = a stage
    // ui3 --shot-hide C@T: the Settings window's Remove (DirectorHideCombo) of combo C at wallpaper time T (memory only)
    std::wstring hideCombo;
    float    hideAt = -1.0f;
    int      cycleDrawTest = 0;
    float    pauseAt = -1.0f, resumeAt = -1.0f;
    int      freezeAnim = -1;
    float    freezeAt = -1.0f, unfreezeAt = -1.0f;
    // brief CLOCKS: --clocks-dryrun HOURS (the scheduler alone on the CPU, no
    // renderer, then exit) and --clocks-force <group>=<F>|mode|tail[,...] (pins
    // groups for a proof render; ticks with the cycle off).
    double   clocksDryrun = 0.0;
    std::wstring clocksForce;
    // --clocks-hold-at T1,T2: ClocksHold(true) at T1, ClocksHold(false) at T2 -- the
    // Settings window's calls, headless (proof 5: glide to 1, settle, release)
    float    clocksHoldAt = -1.0f, clocksReleaseAt = -1.0f;
    // --shot-png-only: write <out>.png only (stats still logged). Saves ~35 MB
    // per capture on a long series; the md5 file is the same .png as always.
    bool     pngOnly = false;
    // --shot-fade F: hold the display fade at F on every frame (the cycle
    // director's fade scalar, forced). Same seed + same time with different F
    // = the same sim state, so mean_lum(F) / mean_lum(1) measures exactly how
    // the fade scales the finished frame. Test hook; -1 = not set.
    float    fixedFade = -1.0f;
    // --cover-sweep a,b,..: the FINAL-CYCLE C coverage logger also steps one
    // shadow hue2 mix field per listed film_hue2_cover bias (CPU only, never
    // drawn) and logs each one's [cover] line beside the live field's.
    std::vector<float> coverSweep;
    // brief BW: --cover-sweep entries may carry a scale (b@s), and
    // --cover-seeds N replicates every entry over N noise seeds (0..N-1).
    std::vector<float> coverScale;
    int      coverSeeds = 1;
    // brief BY: --photos <dir> (what an empty [photo] folder= means here)
    std::wstring photosDir;
};
static bool g_shotPngOnly = false;

static void ShotLog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[1024];
    _vsnprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
    // Mirror to a log file: this is a /SUBSYSTEM:WINDOWS exe, so stdout only
    // lands somewhere when the launching shell gave us a pipe or a file.
    wchar_t path[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, path);
    wcscat_s(path, L"FluidWallpaper-shot.log");
    if (FILE* f = _wfsopen(path, L"a", _SH_DENYNO)) {
        fputs(buf, f);
        fclose(f);
    }
}

// --shot-game-log: is a game in front? The game watcher (tools\game-pause-live.ps1) appends PAUSE /
// RESUME lines; like the render scripts' gate, anything but a RESUME last line counts as a game. A
// missing or unreadable log = no watcher = no game.
static bool ShotGameInFront(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[512];
    DWORD got = 0;
    LARGE_INTEGER sz = {}, off = {};
    if (GetFileSizeEx(h, &sz)) {
        off.QuadPart = sz.QuadPart > (LONGLONG)sizeof(buf) - 1 ? sz.QuadPart - ((LONGLONG)sizeof(buf) - 1) : 0;
        if (!SetFilePointerEx(h, off, nullptr, FILE_BEGIN) || !ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr))
            got = 0;
    }
    CloseHandle(h);
    while (got > 0 && (buf[got - 1] == '\n' || buf[got - 1] == '\r' || buf[got - 1] == ' ')) got--;
    if (got == 0) return false;
    buf[got] = 0;
    const char* line = buf;
    for (DWORD i = got; i > 0; i--)
        if (buf[i - 1] == '\n') { line = buf + i; break; }
    return strstr(line, "RESUME") == nullptr;
}

static float LinearToSrgb(float c) {
    if (c <= 0.0f) return 0.0f;
    if (c >= 1.0f) return 1.0f;
    return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}
static uint8_t ToByte(float srgb) {
    int v = (int)(srgb * 255.0f + 0.5f);
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

// WIC PNG writer (24bpp BGR).
// --shot-dump: one channel, 16 bits, native-endian samples (WIC writes the
// PNG's big-endian itself).
static bool WritePng16Gray(const wchar_t* path, const std::vector<uint16_t>& g, int w, int h) {
    ComPtr<IWICImagingFactory> fac;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&fac)))) return false;
    ComPtr<IWICStream> stream;
    if (FAILED(fac->CreateStream(&stream))) return false;
    if (FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE))) return false;
    ComPtr<IWICBitmapEncoder> enc;
    if (FAILED(fac->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc))) return false;
    if (FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(enc->CreateNewFrame(&frame, &props))) return false;
    if (FAILED(frame->Initialize(props.Get()))) return false;
    if (FAILED(frame->SetSize((UINT)w, (UINT)h))) return false;
    WICPixelFormatGUID want = GUID_WICPixelFormat16bppGray, fmt = want;
    if (FAILED(frame->SetPixelFormat(&fmt))) return false;
    if (memcmp(&fmt, &want, sizeof(fmt)) != 0) return false;
    const UINT stride = (UINT)w * 2;
    if (FAILED(frame->WritePixels((UINT)h, stride, stride * (UINT)h,
                                  const_cast<BYTE*>((const BYTE*)g.data())))) return false;
    if (FAILED(frame->Commit())) return false;
    return SUCCEEDED(enc->Commit());
}
static bool WritePng(const wchar_t* path, const std::vector<uint8_t>& bgr, int w, int h) {
    ComPtr<IWICImagingFactory> fac;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&fac)))) return false;
    ComPtr<IWICStream> stream;
    if (FAILED(fac->CreateStream(&stream))) return false;
    if (FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE))) return false;
    ComPtr<IWICBitmapEncoder> enc;
    if (FAILED(fac->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc))) return false;
    if (FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(enc->CreateNewFrame(&frame, &props))) return false;
    if (FAILED(frame->Initialize(props.Get()))) return false;
    if (FAILED(frame->SetSize((UINT)w, (UINT)h))) return false;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->SetPixelFormat(&fmt))) return false;
    const UINT stride = (UINT)w * 3;
    if (FAILED(frame->WritePixels((UINT)h, stride, stride * (UINT)h,
                                  const_cast<BYTE*>(bgr.data())))) return false;
    if (FAILED(frame->Commit())) return false;
    return SUCCEEDED(enc->Commit());
}

// ---------------------------------------------------------------------------
// True-HDR shot output. The two 8-bit PNGs above are both lossy views of the
// frame -- one clips at SDR white, the other tone-maps -- so a subtle post
// effect (lid sheen, grain, banding in a slow ramp) cannot be judged in
// either. These two carry the captured floats instead:
//
//   <stem>.jxr     64bpp RGBA half, LINEAR scRGB (1.0 = 80 nits), lossless.
//                  Same convention as Windows Game Bar's HDR screenshots, so
//                  the Photos app shows it in real HDR on an HDR display.
//   <stem>-pq.png  48bpp RGB, Rec.2020 primaries, ST 2084 (PQ) transfer, with
//                  a cICP chunk so a PQ-aware viewer doesn't have to guess.
//                  Absolute nits, viewer-independent, no container quirks.
// ---------------------------------------------------------------------------

// IEEE binary32 -> binary16, round-to-nearest, sign preserved (scRGB values
// outside the sRGB gamut are legitimately negative, so this must not clamp).
static uint16_t FloatToHalf(float f) {
    uint32_t x;
    memcpy(&x, &f, sizeof(x));
    const uint32_t sign = (x >> 16) & 0x8000u;
    const uint32_t rawExp = (x >> 23) & 0xFFu;
    uint32_t mant = x & 0x7FFFFFu;
    if (rawExp == 0xFFu)                                   // Inf / NaN
        return (uint16_t)(sign | 0x7C00u | (mant ? 0x200u : 0u));
    const int32_t exp = (int32_t)rawExp - 127 + 15;
    if (exp >= 31) return (uint16_t)(sign | 0x7C00u);      // overflow -> Inf
    if (exp <= 0) {                                        // subnormal / zero
        if (exp < -10) return (uint16_t)sign;
        mant |= 0x800000u;
        const uint32_t shift = (uint32_t)(14 - exp);
        uint32_t h = mant >> shift;
        if ((mant >> (shift - 1)) & 1u) h++;               // round to nearest
        return (uint16_t)(sign | h);
    }
    uint32_t h = ((uint32_t)exp << 10) | (mant >> 13);
    if ((mant >> 12) & 1u) h++;                            // may carry into exp
    return (uint16_t)(sign | h);
}

// JPEG XR (WIC's WMPhoto codec), 64bpp RGBA half, lossless.
static bool WriteJxrHalf(const wchar_t* path, const std::vector<uint16_t>& rgbaHalf,
                         int w, int h) {
    ComPtr<IWICImagingFactory> fac;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&fac)))) return false;
    ComPtr<IWICStream> stream;
    if (FAILED(fac->CreateStream(&stream))) return false;
    if (FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE))) return false;
    ComPtr<IWICBitmapEncoder> enc;
    if (FAILED(fac->CreateEncoder(GUID_ContainerFormatWmp, nullptr, &enc))) return false;
    if (FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(enc->CreateNewFrame(&frame, &props))) return false;
    if (props) {
        // Lossless only takes effect with UseCodecOptions; ImageQuality 1.0 is
        // the fallback for any codec build that ignores the pair. A failed
        // Write here is not fatal -- it just costs quality, and we'd rather
        // ship a slightly-compressed .jxr than none.
        PROPBAG2 opt = {};
        VARIANT v;
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VariantInit(&v); v.vt = VT_R4;   v.fltVal  = 1.0f;          props->Write(1, &opt, &v);
        opt.pstrName = const_cast<LPOLESTR>(L"UseCodecOptions");
        VariantInit(&v); v.vt = VT_BOOL; v.boolVal = VARIANT_TRUE;  props->Write(1, &opt, &v);
        opt.pstrName = const_cast<LPOLESTR>(L"Lossless");
        VariantInit(&v); v.vt = VT_BOOL; v.boolVal = VARIANT_TRUE;  props->Write(1, &opt, &v);
    }
    if (FAILED(frame->Initialize(props.Get()))) return false;
    if (FAILED(frame->SetSize((UINT)w, (UINT)h))) return false;
    WICPixelFormatGUID want = GUID_WICPixelFormat64bppRGBAHalf, fmt = want;
    if (FAILED(frame->SetPixelFormat(&fmt))) return false;
    if (memcmp(&fmt, &want, sizeof(fmt)) != 0) {
        // The encoder negotiated something else; a silent 8-bit fallback is
        // exactly the failure this file exists to avoid, so say so and stop.
        ShotLog("[shot] jxr: encoder refused 64bppRGBAHalf\n");
        return false;
    }
    const UINT stride = (UINT)w * 8;
    if (FAILED(frame->WritePixels((UINT)h, stride, stride * (UINT)h,
                                  const_cast<BYTE*>((const BYTE*)rgbaHalf.data())))) return false;
    if (FAILED(frame->Commit())) return false;
    return SUCCEEDED(enc->Commit());
}

// WIC PNG writer, 48bpp RGB (16 bits per channel).
static bool WritePng48(const wchar_t* path, const std::vector<uint16_t>& rgb, int w, int h) {
    ComPtr<IWICImagingFactory> fac;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&fac)))) return false;
    ComPtr<IWICStream> stream;
    if (FAILED(fac->CreateStream(&stream))) return false;
    if (FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE))) return false;
    ComPtr<IWICBitmapEncoder> enc;
    if (FAILED(fac->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc))) return false;
    if (FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(enc->CreateNewFrame(&frame, &props))) return false;
    if (FAILED(frame->Initialize(props.Get()))) return false;
    if (FAILED(frame->SetSize((UINT)w, (UINT)h))) return false;
    WICPixelFormatGUID want = GUID_WICPixelFormat48bppRGB, fmt = want;
    if (FAILED(frame->SetPixelFormat(&fmt))) return false;
    if (memcmp(&fmt, &want, sizeof(fmt)) != 0) {
        ShotLog("[shot] pq png: encoder refused 48bppRGB\n");
        return false;
    }
    const UINT stride = (UINT)w * 6;
    if (FAILED(frame->WritePixels((UINT)h, stride, stride * (UINT)h,
                                  const_cast<BYTE*>((const BYTE*)rgb.data())))) return false;
    if (FAILED(frame->Commit())) return false;
    return SUCCEEDED(enc->Commit());
}

static uint32_t Crc32Png(const uint8_t* p, size_t n) {
    static uint32_t tab[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            tab[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = tab[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// Rewrite a just-written PNG: drop any sRGB/gAMA/cHRM/iCCP the encoder added
// and insert cICP right after IHDR -- BT.2020 primaries (9), ST 2084 transfer
// (16), RGB matrix (0), full range (1). Without it a PQ PNG is read as sRGB
// and looks like fog.
static bool PngSetCicpPq(const wchar_t* path) {
    std::vector<uint8_t> in;
    if (FILE* f = _wfsopen(path, L"rb", _SH_DENYNO)) {
        fseek(f, 0, SEEK_END);
        const long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 8) {
            in.resize((size_t)sz);
            if (fread(in.data(), 1, in.size(), f) != in.size()) in.clear();
        }
        fclose(f);
    }
    if (in.size() < 8) return false;

    std::vector<uint8_t> out;
    out.reserve(in.size() + 16);
    out.insert(out.end(), in.begin(), in.begin() + 8);          // signature
    size_t i = 8;
    bool inserted = false;
    while (i + 12 <= in.size()) {
        const uint32_t len = ((uint32_t)in[i] << 24) | ((uint32_t)in[i + 1] << 16) |
                             ((uint32_t)in[i + 2] << 8) | (uint32_t)in[i + 3];
        if (len > in.size() || i + 12 + (size_t)len > in.size()) return false;
        const char* t = (const char*)&in[i + 4];
        const bool drop = !memcmp(t, "sRGB", 4) || !memcmp(t, "gAMA", 4) ||
                          !memcmp(t, "cHRM", 4) || !memcmp(t, "iCCP", 4) ||
                          !memcmp(t, "cICP", 4);
        if (!drop) out.insert(out.end(), in.begin() + i, in.begin() + i + 12 + len);
        if (!inserted && !memcmp(t, "IHDR", 4)) {
            uint8_t c[12] = { 0, 0, 0, 4, 'c', 'I', 'C', 'P', 9, 16, 0, 1 };
            const uint32_t crc = Crc32Png(c + 4, 8);
            out.insert(out.end(), c, c + 12);
            out.push_back((uint8_t)(crc >> 24)); out.push_back((uint8_t)(crc >> 16));
            out.push_back((uint8_t)(crc >> 8));  out.push_back((uint8_t)crc);
            inserted = true;
        }
        i += 12 + (size_t)len;
    }
    if (!inserted) return false;
    FILE* f = _wfsopen(path, L"wb", _SH_DENYNO);
    if (!f) return false;
    const bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

// Absolute nits -> ST 2084 (PQ) code value in 0..1.
static float PqFromNits(float nits) {
    const float m1 = 0.1593017578125f, m2 = 78.84375f;
    const float c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    float y = nits * (1.0f / 10000.0f);
    if (!(y > 0.0f)) return 0.0f;
    if (y > 1.0f) y = 1.0f;
    const float yp = powf(y, m1);
    return powf((c1 + c2 * yp) / (1.0f + c3 * yp), m2);
}
static uint16_t ToU16(float v) {
    int x = (int)(v * 65535.0f + 0.5f);
    return (uint16_t)(x < 0 ? 0 : (x > 65535 ? 65535 : x));
}

static void EnsureParentDir(const wchar_t* path) {
    wchar_t dir[MAX_PATH];
    wcscpy_s(dir, MAX_PATH, path);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (!slash) return;
    *slash = 0;
    // create the chain (one level of nesting is the common case here)
    wchar_t parent[MAX_PATH];
    wcscpy_s(parent, MAX_PATH, dir);
    if (wchar_t* s2 = wcsrchr(parent, L'\\')) { *s2 = 0; CreateDirectoryW(parent, nullptr); }
    CreateDirectoryW(dir, nullptr);
}

// Encode the captured linear-scRGB frame four ways and report what's in it.
//   <stem>.png      display-referred SDR: /sdrScale, clip at 1.0, sRGB encode
//   <stem>-hdr.png  highlight-preserving: hue-preserving Reinhard on the max
//                   channel, so anything above SDR white stays visible
//   <stem>.jxr      the floats themselves: linear scRGB half, lossless JPEG XR
//   <stem>-pq.png   the floats in absolute nits: Rec.2020 + ST 2084, 16-bit
static void WriteShotPair(const std::wstring& stem, const std::vector<float>& rgba,
                          int w, int h, float sdrScale, float elapsed) {
    const size_t n = (size_t)w * h;
    std::vector<uint8_t> sdr(n * 3), hdr(n * 3);
    std::vector<uint16_t> half(n * 4), pq(n * 3);

    double lumSum = 0.0;
    size_t aboveWhite = 0, negative = 0;
    float maxScrgb = 0.0f;
    // 10-bin NITS histogram of the max channel (scRGB 1.0 = 80 nits by
    // definition). Fixed edges so any two runs are directly comparable; the
    // point of it is to see how many distinct brightness STEPS a look uses,
    // which the mean and the max between them cannot show.
    static const float kNitEdge[9] = { 1, 5, 15, 40, 80, 160, 240, 400, 600 };
    size_t nitHist[10] = {};
    float minNits = 1e9f;

    for (size_t p = 0; p < n; p++) {
        float r = rgba[p * 4 + 0], g = rgba[p * 4 + 1], b = rgba[p * 4 + 2];
        if (r != r) r = 0; if (g != g) g = 0; if (b != b) b = 0;   // NaN guard
        if (r < 0.0f || g < 0.0f || b < 0.0f) negative++;

        // scene-referred, untouched: linear scRGB half (negatives kept)
        half[p * 4 + 0] = FloatToHalf(r);
        half[p * 4 + 1] = FloatToHalf(g);
        half[p * 4 + 2] = FloatToHalf(b);
        half[p * 4 + 3] = 0x3C00;                                  // alpha 1.0
        // absolute nits: Rec.709 -> Rec.2020 (BT.2087), scRGB 1.0 = 80 nits,
        // then PQ. Out-of-Rec.2020 negatives clip; the .jxr keeps them.
        {
            const float r20 = 0.627404f * r + 0.329283f * g + 0.043313f * b;
            const float g20 = 0.069097f * r + 0.919540f * g + 0.011362f * b;
            const float b20 = 0.016391f * r + 0.088013f * g + 0.895595f * b;
            pq[p * 3 + 0] = ToU16(PqFromNits(fmaxf(0.0f, r20) * 80.0f));
            pq[p * 3 + 1] = ToU16(PqFromNits(fmaxf(0.0f, g20) * 80.0f));
            pq[p * 3 + 2] = ToU16(PqFromNits(fmaxf(0.0f, b20) * 80.0f));
        }
        maxScrgb = fmaxf(maxScrgb, fmaxf(r, fmaxf(g, b)));
        {
            const float nits = fmaxf(r, fmaxf(g, b)) * 80.0f;
            minNits = fminf(minNits, nits);
            int bin = 0;
            while (bin < 9 && nits >= kNitEdge[bin]) bin++;
            nitHist[bin]++;
        }
        // Rec.709 luminance of the scRGB value (1.0 = 80 nits)
        lumSum += 0.2126 * r + 0.7152 * g + 0.0722 * b;
        if (fmaxf(r, fmaxf(g, b)) > sdrScale) aboveWhite++;

        // display-referred: SDR white -> 1.0, negatives (wide gamut) clip to 0
        float dr = fmaxf(0.0f, r / sdrScale);
        float dg = fmaxf(0.0f, g / sdrScale);
        float db = fmaxf(0.0f, b / sdrScale);
        sdr[p * 3 + 0] = ToByte(LinearToSrgb(fminf(db, 1.0f)));   // B
        sdr[p * 3 + 1] = ToByte(LinearToSrgb(fminf(dg, 1.0f)));   // G
        sdr[p * 3 + 2] = ToByte(LinearToSrgb(fminf(dr, 1.0f)));   // R

        // highlight-preserving: Reinhard on the max channel, ratios preserved
        float m = fmaxf(dr, fmaxf(dg, db));
        float k = m > 1e-6f ? (m / (1.0f + m)) / m : 0.0f;
        hdr[p * 3 + 0] = ToByte(LinearToSrgb(db * k));
        hdr[p * 3 + 1] = ToByte(LinearToSrgb(dg * k));
        hdr[p * 3 + 2] = ToByte(LinearToSrgb(dr * k));
    }

    std::wstring sdrPath = stem + L".png";
    std::wstring hdrPath = stem + L"-hdr.png";
    std::wstring jxrPath = stem + L".jxr";
    std::wstring pqPath  = stem + L"-pq.png";
    EnsureParentDir(sdrPath.c_str());
    bool okS = WritePng(sdrPath.c_str(), sdr, w, h);
    bool okH = g_shotPngOnly || WritePng(hdrPath.c_str(), hdr, w, h);
    bool okJ = g_shotPngOnly || WriteJxrHalf(jxrPath.c_str(), half, w, h);
    bool okP = g_shotPngOnly || (WritePng48(pqPath.c_str(), pq, w, h) && PngSetCicpPq(pqPath.c_str()));

    const double meanLum = lumSum / (double)n;
    ShotLog("[shot] t=%.1fs %ls %dx%d  mean_lum=%.4f scRGB (%.1f nits)  "
            "above_sdr_white=%.2f%%  max_scRGB=%.3f (%.0f nits)  negative_px=%.2f%%  "
            "sdr=%s hdr=%s jxr=%s pq=%s\n",
            elapsed, sdrPath.c_str(), w, h, meanLum, meanLum * 80.0,
            100.0 * aboveWhite / (double)n, maxScrgb, maxScrgb * 80.0f,
            100.0 * negative / (double)n,
            okS ? "ok" : "FAILED", okH ? "ok" : "FAILED",
            okJ ? "ok" : "FAILED", okP ? "ok" : "FAILED");
    ShotLog("[shot] nits min=%.1f mean=%.1f max=%.0f  hist%% "
            "[<1]%.2f [1-5]%.2f [5-15]%.2f [15-40]%.2f [40-80]%.2f "
            "[80-160]%.2f [160-240]%.2f [240-400]%.2f [400-600]%.2f [600+]%.2f\n",
            minNits, meanLum * 80.0, maxScrgb * 80.0f,
            100.0 * nitHist[0] / (double)n, 100.0 * nitHist[1] / (double)n,
            100.0 * nitHist[2] / (double)n, 100.0 * nitHist[3] / (double)n,
            100.0 * nitHist[4] / (double)n, 100.0 * nitHist[5] / (double)n,
            100.0 * nitHist[6] / (double)n, 100.0 * nitHist[7] / (double)n,
            100.0 * nitHist[8] / (double)n, 100.0 * nitHist[9] / (double)n);
}

// Refresh rate of the monitor the wallpaper lives on, cached (a display-mode
// change is rare and a stale value for a second is harmless). 0 -> 60 so the
// software FPS cap keeps its old behaviour if the query ever fails.
static float MonitorRefreshHz() {
    static float hz = 0.0f;
    static ULONGLONG checked = 0;
    const ULONGLONG now = GetTickCount64();
    if (hz > 0.0f && now - checked < 2000) return hz;
    checked = now;
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    if (g_monitor && GetMonitorInfoW(g_monitor, &mi) &&
        EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) &&
        dm.dmDisplayFrequency > 1) {
        hz = (float)dm.dmDisplayFrequency;
    } else if (hz <= 0.0f) {
        hz = 60.0f;
    }
    return hz;
}

// Cheap pre-scan so the normal launch path below stays untouched.
static bool ShotModeRequested() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool found = false;
    for (int i = 1; i < argc && !found; i++)
        if (wcscmp(argv[i], L"--shot") == 0) found = true;
    LocalFree(argv);
    return found;
}

// --shot + cycling: the message-only control window's procedure (see RunShotMode)
static LRESULT CALLBACK ShotCycleWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_COMMAND) {
        ShotLog("[cycle] WM_COMMAND %u from another process\n", (unsigned)LOWORD(w));
        switch (LOWORD(w)) {
        case CMD_CYCLE_NEXT: CycleNext(); break;
        case CMD_CYCLE_PREV: CyclePrev(); break;
        case CMD_CYCLE_ON:   CycleSetEnabled(true, false); break;
        case CMD_CYCLE_OFF:  CycleSetEnabled(false, false); break;
        default:
            if (LOWORD(w) >= CMD_CYCLE_STAGE_BASE && LOWORD(w) < CMD_CYCLE_STAGE_BASE + 99)
                CycleJump(LOWORD(w) - CMD_CYCLE_STAGE_BASE);
            break;
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// Settings-window model on the renderer's config; hooks = the calls the old window made.
// Called by BOTH the live path (wWinMain) and RunShotMode once g_renderer is set: without it
// UiModelReady() is false and ShowSettingsWindow() returns silently (tray Settings / the
// second-launch shortcut did nothing on every live build since UI 1a, 2026-09-24).
static void InitSettingsModel(FluidRenderer& renderer) {
    UiModelInit(renderer.Config());
    UiLoadActivePreset();
    UiHooks h;
    h.reinitWanderers = [] { if (g_renderer) g_renderer->ReinitWanderers(); };
    h.ensureLook = [] { if (g_renderer) g_renderer->EnsureLookResources(); };
    h.setResolutions = [](int simRes, int dyeRes) { if (g_renderer) g_renderer->SetResolutions(simRes, dyeRes); };
    h.applyPreset = [](const std::wstring& p) { ApplyPreset(p); };
    UiSetHooks(h);
}

// ---- --shot-stage-tap (Oil3D L3b, shot-only debug) -----------------------------
// After the shot is written: the frame re-drawn once per tap (CaptureStageTap /
// the lamps map) and each read back to <stem>-tap<NNN>.f16 (tap000 = the shot
// itself, tapL = the lamps map): 'FWT1', int32 w, h, channels (3), then w x h x
// 3 IEEE half (rgb, linear; the target is FP16, so this is lossless), after
// a float32 sdrScale (the lamps map is SRGBToLinear(v) x sdrScale).
static bool WriteTapF16(const std::wstring& path, const std::vector<float>& rgba, int w, int h, float sdrScale) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    const int32_t hd[4] = { 0x31545746, w, h, 3 };   // 'FWT1'
    fwrite(hd, sizeof(hd), 1, f);
    fwrite(&sdrScale, sizeof(float), 1, f);          // then the frame's sdrScale
    std::vector<uint16_t> row((size_t)w * 3);
    for (int y = 0; y < h; y++) {
        const float* q = &rgba[(size_t)y * w * 4];
        for (int x = 0; x < w; x++)
            for (int c = 0; c < 3; c++) row[(size_t)x * 3 + c] = FloatToHalf(q[(size_t)x * 4 + c]);
        fwrite(row.data(), sizeof(uint16_t), row.size(), f);
    }
    fclose(f);
    return true;
}
static void WriteStageTaps(FluidRenderer& r, const std::wstring& stem, const ShotOpts& o,
                           const std::vector<float>& shot, float sdrScale) {
    const int w = o.width, h = o.height;
    auto meanY = [&](const std::vector<float>& p) {
        double s = 0.0;
        for (size_t i = 0; i < (size_t)w * h; i++)
            s += 0.2126 * p[i * 4] + 0.7152 * p[i * 4 + 1] + 0.0722 * p[i * 4 + 2];
        return s / ((double)w * h);
    };
    WriteTapF16(stem + L"-tap000.f16", shot, w, h, sdrScale);
    ShotLog("[tap] 0 (the shot) mean Y %.6f\n", meanY(shot));
    std::vector<float> px;
    for (int t : o.stageTaps) {
        wchar_t sfx[24];
        if (t < 0) swprintf_s(sfx, L"-tapL.f16");
        else       swprintf_s(sfx, L"-tap%03d.f16", t);
        const bool ok = (t < 0) ? r.CaptureDebugFrame(-1, px) : r.CaptureStageTap(t, px);
        if (!ok || px.size() < (size_t)w * h * 4) { ShotLog("[tap] %d: capture FAILED\n", t); continue; }
        WriteTapF16(stem + sfx, px, w, h, sdrScale);
        double dmax = 0.0;
        if (t == 299)
            for (size_t i = 0; i < (size_t)w * h * 4; i++) dmax = fmax(dmax, fabs((double)px[i] - shot[i]));
        if (t == 299) ShotLog("[tap] 299 (post pass re-drawn, no tap) mean Y %.6f, max |d| vs the shot %.3g\n", meanY(px), dmax);
        else          ShotLog("[tap] %d mean Y %.6f\n", t, meanY(px));
    }
}

// ---- --shot-dump (Oil3D -> UE OilTank, ue5/OilTank/DUMP-SCHEMA.md v1) ------
static std::string JsonPathEsc(const std::wstring& w) {
    std::string u;
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n > 1) { u.resize((size_t)n - 1); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &u[0], n, nullptr, nullptr); }
    std::string o;
    for (char c : u) {
        if (c == '\\' || c == '"') o += '\\';
        o += c;
    }
    return o;
}
static float SrgbToLin1(float c) {
    c = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}
// film_rgb / accent_rgb from the film frame (display gamma): a luma-weighted
// hue histogram of the saturated pixels; the film is the strongest hue, the
// accent the strongest one >= 60 deg away (film again if there is none).
static void FilmColours(const std::vector<float>& f, int w, int h, float film[3], float acc[3]) {
    const int NB = 36;
    double wsum[NB] = {}, rs[NB] = {}, gs[NB] = {}, bs[NB] = {};
    for (int y = 0; y < h; y += 2)
        for (int x = 0; x < w; x += 2) {
            const float* q = &f[((size_t)y * w + x) * 4];
            const float r = SrgbToLin1(q[0]), g = SrgbToLin1(q[1]), b = SrgbToLin1(q[2]);
            const float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b));
            if (mx < 0.02f || (mx - mn) < 0.25f * mx) continue;
            float hue = atan2f(0.8660254f * (g - b), r - 0.5f * (g + b)) * 57.29578f;
            if (hue < 0.0f) hue += 360.0f;
            const int bi = ((int)(hue / 10.0f)) % NB;
            const double wt = mx - mn;
            wsum[bi] += wt; rs[bi] += r * wt; gs[bi] += g * wt; bs[bi] += b * wt;
        }
    int b0 = 0;
    for (int i = 1; i < NB; i++) if (wsum[i] > wsum[b0]) b0 = i;
    int b1 = -1;
    for (int i = 0; i < NB; i++) {
        int d = abs(i - b0); d = d > NB / 2 ? NB - d : d;
        if (d < 6) continue;
        if (b1 < 0 || wsum[i] > wsum[b1]) b1 = i;
    }
    double tot = 0.0;
    for (int i = 0; i < NB; i++) tot += wsum[i];
    auto mean = [&](int c, float o[3]) {
        double W = 0, R = 0, G = 0, B = 0;
        for (int k = -1; k <= 1; k++) {
            const int i = (c + k + NB) % NB;
            W += wsum[i]; R += rs[i]; G += gs[i]; B += bs[i];
        }
        if (W <= 0.0) { o[0] = o[1] = o[2] = 0.0f; return; }
        o[0] = (float)(R / W); o[1] = (float)(G / W); o[2] = (float)(B / W);
    };
    mean(b0, film);
    if (b1 >= 0 && tot > 0.0 && wsum[b1] > 0.02 * tot) mean(b1, acc);
    else { acc[0] = film[0]; acc[1] = film[1]; acc[2] = film[2]; }
}
static bool WriteShotDump(FluidRenderer& r, const std::wstring& json, const ShotOpts& o) {
    const int w = o.width, h = o.height;
    std::wstring stem = json;
    if (stem.size() > 5 && _wcsicmp(stem.c_str() + stem.size() - 5, L".json") == 0) stem.resize(stem.size() - 5);
    std::wstring dir, base = stem;
    const size_t sl = stem.find_last_of(L"\\/");
    if (sl != std::wstring::npos) { dir = stem.substr(0, sl + 1); base = stem.substr(sl + 1); }
    const std::wstring hName = base + L"-height.png", fName = base + L"-film.png";
    // h (debug 4): r + g / 1024, ~21 bits through the half-float target
    std::vector<float> px;
    if (!r.CaptureDebugFrame(4, px) || px.size() < (size_t)w * h * 4) {
        ShotLog("[dump] ERROR: height capture failed (acid look + OIL_HEIGHT PSO needed)\n");
        return false;
    }
    std::vector<uint16_t> hg((size_t)w * h);
    double hMax = 0.0, hMean = 0.0;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        double v = (double)px[i * 4 + 0] + (double)px[i * 4 + 1] / 1024.0;
        v = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
        hMax = v > hMax ? v : hMax; hMean += v;
        hg[i] = (uint16_t)lround(v * 65535.0);
    }
    hMean /= (double)w * h;
    // the split's wiring proof: the surface PRE-PASS buffer's h (debug 6, read
    // by the display pass through t5) against the in-register h above
    {
        std::vector<float> sv;
        if (r.CaptureDebugFrame(6, sv) && sv.size() >= (size_t)w * h * 4) {
            double dMax = 0.0, dSum = 0.0, oMax = 0.0;
            for (size_t i = 0; i < (size_t)w * h; i++) {
                const double hr = (double)hg[i] / 65535.0;
                const double d = fabs((double)sv[i * 4 + 0] - hr);
                dMax = d > dMax ? d : dMax; dSum += d;
                oMax = sv[i * 4 + 1] > oMax ? sv[i * 4 + 1] : oMax;
            }
            ShotLog("[dump] surface pre-pass check: |h(buffer) - h(display)| max %.6f mean %.7f (half-float step at 1 = 0.00049); h_oil max %.4f\n",
                    dMax, dSum / ((double)w * h), oMax);
        } else {
            ShotLog("[dump] surface pre-pass check: capture failed\n");
        }
    }
    EnsureParentDir(json.c_str());
    const bool okH = WritePng16Gray((dir + hName).c_str(), hg, w, h);
    // the film without the oil (debug 5), display gamma -> 8-bit sRGB
    bool okF = false;
    float film[3] = { 0, 0, 0 }, acc[3] = { 0, 0, 0 };
    if (r.CaptureDebugFrame(5, px) && px.size() >= (size_t)w * h * 4) {
        std::vector<uint8_t> bgr((size_t)w * h * 3);
        for (size_t i = 0; i < (size_t)w * h; i++)
            for (int c = 0; c < 3; c++) {
                float v = px[i * 4 + c];
                v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                bgr[i * 3 + (2 - c)] = (uint8_t)lroundf(v * 255.0f);
            }
        okF = WritePng((dir + fName).c_str(), bgr, w, h);
        FilmColours(px, w, h, film, acc);
    }
    std::vector<float> dr;
    r.DumpDroplets(dr);
    FILE* f = nullptr;
    if (_wfopen_s(&f, json.c_str(), L"wb") != 0 || !f) { ShotLog("[dump] ERROR: cannot write the json\n"); return false; }
    fprintf(f, "{\n  \"version\": 1,\n  \"t\": %.4f,\n  \"seed\": %u,\n  \"width\": %d,\n  \"height\": %d,\n",
            r.ClockSeconds(), o.seed, w, h);
    fprintf(f, "  \"film_rgb\": [%.6f, %.6f, %.6f],\n  \"accent_rgb\": [%.6f, %.6f, %.6f],\n",
            film[0], film[1], film[2], acc[0], acc[1], acc[2]);
    fprintf(f, "  \"height_png\": \"%s\",\n  \"film_png\": \"%s\",\n",
            okH ? JsonPathEsc(hName).c_str() : "", okF ? JsonPathEsc(fName).c_str() : "");
    fprintf(f, "  \"ini_chain\": [");
    {
        std::vector<std::wstring> chain;
        std::wstring cur;
        const std::wstring src = o.iniChain.empty() ? o.ini : o.iniChain;
        for (wchar_t c : src) { if (c == L'|') { if (!cur.empty()) chain.push_back(cur); cur.clear(); } else cur += c; }
        if (!cur.empty()) chain.push_back(cur);
        for (size_t k = 0; k < chain.size(); k++)
            fprintf(f, "%s\"%s\"", k ? ", " : "", JsonPathEsc(chain[k]).c_str());
    }
    fprintf(f, "],\n  \"droplets\": [");
    for (size_t k = 0; k + 2 < dr.size(); k += 3)
        fprintf(f, "%s\n    { \"x\": %.6f, \"y\": %.6f, \"r\": %.6f, \"depth\": 0.0 }",
                k ? "," : "", dr[k], dr[k + 1], dr[k + 2]);
    fprintf(f, "%s]\n}\n", dr.empty() ? "" : "\n  ");
    fclose(f);
    ShotLog("[dump] %ls: height %s (max %.4f mean %.4f), film %s, film_rgb %.4f %.4f %.4f, accent %.4f %.4f %.4f, %zu droplets\n",
            json.c_str(), okH ? "ok" : "FAILED", hMax, hMean, okF ? "ok" : "FAILED",
            film[0], film[1], film[2], acc[0], acc[1], acc[2], dr.size() / 3);
    return okH;
}

static int RunShotMode() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    ShotOpts o;
    {
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; i < argc; i++) {
            auto next = [&](void) -> const wchar_t* { return i + 1 < argc ? argv[++i] : nullptr; };
            if (wcscmp(argv[i], L"--shot") == 0) {
                if (const wchar_t* v = next()) o.out = v;
            } else if (wcscmp(argv[i], L"--shot-size") == 0) {
                if (const wchar_t* v = next()) {
                    int w = 0, h = 0;
                    if (swscanf_s(v, L"%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                        o.width = w; o.height = h;
                    }
                }
            } else if (wcscmp(argv[i], L"--shot-pour") == 0 && i + 1 < argc) {
                if (swscanf_s(argv[++i], L"%f,%f,%f,%f", &o.pourX, &o.pourY, &o.pourStart, &o.pourDur) == 4)
                    o.pour = true;
            } else if (wcscmp(argv[i], L"--shot-drop") == 0 && i + 1 < argc) {
                const wchar_t* v = argv[++i];
                int got = swscanf_s(v, L"%f,%f,%f,%f", &o.dropX, &o.dropY, &o.dropAt, &o.dropVy);
                if (got >= 3) { o.drop = true; if (got == 3) o.dropVy = -1.0f; }
            } else if (wcscmp(argv[i], L"--shot-mouse") == 0 && i + 1 < argc) {
                const wchar_t* v = argv[++i];
                int got = swscanf_s(v, L"%f,%f,%f,%f,%f,%f", &o.mouseSx, &o.mouseSy,
                                    &o.mouseDx, &o.mouseDy, &o.mouseAt, &o.mouseDur);
                if (got >= 5) { o.mouse = true; if (got == 5) o.mouseDur = 0.5f; }
            } else if (wcscmp(argv[i], L"--shot-preset") == 0 && i + 1 < argc) {
                // --shot-preset <ini> AT <sec>   (also accepts <ini> <sec>)
                o.presetIni = argv[++i];
                if (i + 1 < argc && _wcsicmp(argv[i + 1], L"AT") == 0) i++;
                if (i + 1 < argc) o.presetAt = (float)_wtof(argv[++i]);
            } else if (wcscmp(argv[i], L"--shot-yield") == 0 && i + 1 < argc) {
                o.yieldMs = _wtoi(argv[++i]);
            } else if (wcscmp(argv[i], L"--shot-game-fps") == 0 && i + 1 < argc) {
                o.gameFps = _wtoi(argv[++i]);
            } else if (wcscmp(argv[i], L"--shot-game-log") == 0 && i + 1 < argc) {
                o.gameLog = argv[++i];
            } else if (wcscmp(argv[i], L"--shot-delay") == 0) {
                if (const wchar_t* v = next()) o.delaySec = (float)_wtof(v);
            } else if (wcscmp(argv[i], L"--shot-series") == 0) {
                if (const wchar_t* v = next()) {
                    int n = 0; float iv = 0;
                    if (swscanf_s(v, L"%d:%f", &n, &iv) == 2 && n > 0) {
                        o.seriesCount = n; o.seriesInterval = iv;
                    }
                }
            } else if (wcscmp(argv[i], L"--seed") == 0) {
                if (const wchar_t* v = next()) o.seed = (unsigned)_wtoi(v);
            } else if (wcscmp(argv[i], L"--ini") == 0) {
                if (const wchar_t* v = next()) o.ini = v;
            } else if (wcscmp(argv[i], L"--hdr") == 0) {
                if (const wchar_t* v = next()) o.hdrOn = (_wcsicmp(v, L"on") == 0);
            } else if (wcscmp(argv[i], L"--sdr-white") == 0) {
                if (const wchar_t* v = next()) o.sdrWhiteNits = (float)_wtof(v);
            } else if (wcscmp(argv[i], L"--panel-max") == 0) {
                if (const wchar_t* v = next()) o.panelMaxNits = (float)_wtof(v);
            } else if (wcscmp(argv[i], L"--sdr-gamut") == 0) {
                // a typo must not render a different mode under the label that was asked for
                const wchar_t* v = next();
                if (v && v[0] >= L'0' && v[0] <= L'2' && v[1] == 0) o.sdrGamut = v[0] - L'0';
                else o.sdrArgBad = true;
            } else if (wcscmp(argv[i], L"--panel-primaries") == 0) {
                const wchar_t* v = next();
                o.panelPrimSet = v && swscanf_s(v, L"%f,%f,%f,%f,%f,%f", &o.panelPrim[0], &o.panelPrim[1],
                                                &o.panelPrim[2], &o.panelPrim[3], &o.panelPrim[4], &o.panelPrim[5]) == 6;
                if (!o.panelPrimSet) o.sdrArgBad = true;
            } else if (wcscmp(argv[i], L"--mouse-none") == 0) {
                o.mouseNone = true;
            } else if (wcscmp(argv[i], L"--cycle") == 0) {
                o.cycleForce = true;
            } else if (wcscmp(argv[i], L"--cycle-dwell") == 0 && i + 1 < argc) {
                o.cycleDwell = (float)_wtof(argv[++i]);
            } else if (wcscmp(argv[i], L"--cycle-jitter") == 0 && i + 1 < argc) {
                o.cycleJitter = (float)_wtof(argv[++i]);
            } else if (wcscmp(argv[i], L"--cycle-order") == 0 && i + 1 < argc) {
                o.cycleOrder = (_wcsicmp(argv[++i], L"fixed") == 0) ? CYCLE_ORDER_FIXED
                                                                     : CYCLE_ORDER_ALT_RANDOM;
            } else if (wcscmp(argv[i], L"--cycle-seed") == 0 && i + 1 < argc) {
                o.cycleSeed = (unsigned)_wtoi(argv[++i]);
            } else if (wcscmp(argv[i], L"--cycle-stage") == 0 && i + 1 < argc) {
                o.cycleStage = _wtoi(argv[++i]);
            } else if (wcscmp(argv[i], L"--cycle-next-at") == 0 && i + 1 < argc) {
                const wchar_t* v = argv[++i];
                while (v && *v) {
                    o.cycleNextAt.push_back((float)_wtof(v));
                    v = wcschr(v, L',');
                    if (v) v++;
                }
            } else if (wcscmp(argv[i], L"--shot-queue") == 0 && i + 1 < argc) {
                std::wstring v = argv[++i];
                size_t pos = 0;
                while (pos < v.size()) {
                    size_t e = v.find(L',', pos);
                    if (e == std::wstring::npos) e = v.size();
                    const std::wstring tok = v.substr(pos, e - pos);
                    const size_t c = tok.find(L':'), at = tok.find(L'@');
                    if (c != std::wstring::npos && at != std::wstring::npos && at > c)
                        o.shotQueue.push_back({ tok.substr(0, c), _wtoi(tok.substr(c + 1, at - c - 1).c_str()),
                                                (float)_wtof(tok.substr(at + 1).c_str()) });
                    pos = e + 1;
                }
            } else if (wcscmp(argv[i], L"--shot-hide") == 0 && i + 1 < argc) {   // ui3: C@T (DirectorHideCombo)
                const std::wstring v = argv[++i];
                const size_t at = v.find(L'@');
                if (at != std::wstring::npos) { o.hideCombo = v.substr(0, at); o.hideAt = (float)_wtof(v.substr(at + 1).c_str()); }
            } else if (wcscmp(argv[i], L"--cycle-keep-at") == 0 && i + 1 < argc) {
                swscanf_s(argv[++i], L"%f,%f", &o.keepAt, &o.keepSec);
            } else if (wcscmp(argv[i], L"--cycle-visit-at") == 0 && i + 1 < argc) {
                // ui3: STAGE may be ink or fluid (a guest visit, CycleVisitGuest); a number = the stage visit as before
                const std::wstring v = argv[i + 1];
                const size_t c1 = v.find(L','), c2 = c1 == std::wstring::npos ? c1 : v.find(L',', c1 + 1);
                const std::wstring mid = c2 == std::wstring::npos ? std::wstring() : v.substr(c1 + 1, c2 - c1 - 1);
                if (_wcsicmp(mid.c_str(), L"ink") == 0 || _wcsicmp(mid.c_str(), L"fluid") == 0) {
                    o.visitGuest = _wcsicmp(mid.c_str(), L"ink") == 0 ? 0 : 1;
                    o.visitAt = (float)_wtof(v.substr(0, c1).c_str());
                    o.visitSec = (float)_wtof(v.substr(c2 + 1).c_str());
                    ++i;
                } else
                swscanf_s(argv[++i], L"%f,%d,%f", &o.visitAt, &o.visitStage, &o.visitSec);
            } else if (wcscmp(argv[i], L"--cycle-burst-at") == 0 && i + 1 < argc) {
                const wchar_t* v = argv[++i];
                while (v && *v) {
                    o.cycleBurstAt.push_back((float)_wtof(v));
                    v = wcschr(v, L',');
                    if (v) v++;
                }
            } else if (wcscmp(argv[i], L"--cycle-draw-test") == 0 && i + 1 < argc) {
                o.cycleDrawTest = _wtoi(argv[++i]);
            } else if (wcscmp(argv[i], L"--clocks-dryrun") == 0 && i + 1 < argc) {
                o.clocksDryrun = _wtof(argv[++i]);
            } else if (wcscmp(argv[i], L"--clocks-hold-at") == 0 && i + 1 < argc) {
                swscanf_s(argv[++i], L"%f,%f", &o.clocksHoldAt, &o.clocksReleaseAt);
            } else if (wcscmp(argv[i], L"--clocks-force") == 0 && i + 1 < argc) {
                if (!o.clocksForce.empty()) o.clocksForce += L",";
                o.clocksForce += argv[++i];
            } else if (wcscmp(argv[i], L"--cycle-pause-at") == 0 && i + 1 < argc) {
                swscanf_s(argv[++i], L"%f,%f", &o.pauseAt, &o.resumeAt);
            } else if (wcscmp(argv[i], L"--shot-freeze") == 0 && i + 1 < argc) {
                wchar_t name[32] = {};
                if (swscanf_s(argv[++i], L"%31[^,],%f,%f", name, 32, &o.freezeAt, &o.unfreezeAt) >= 2) {
                    if (_wcsicmp(name, L"palette") == 0)         o.freezeAnim = ANIM_PALETTE;
                    else if (_wcsicmp(name, L"hue2") == 0)       o.freezeAnim = ANIM_HUE2;
                    else if (_wcsicmp(name, L"rig") == 0)        o.freezeAnim = ANIM_RIG;
                    else if (_wcsicmp(name, L"hueshift") == 0)   o.freezeAnim = ANIM_HUE_SHIFT;
                    else if (_wcsicmp(name, L"transition") == 0) o.freezeAnim = ANIM_TRANSITION;
                    else if (_wcsicmp(name, L"clocks") == 0)     o.freezeAnim = ANIM_CLOCKS;
                }
            } else if (wcscmp(argv[i], L"--shot-lamp2-capped") == 0) {
                o.lamp2Capped = true;
            } else if (wcscmp(argv[i], L"--shot-png-only") == 0) {
                o.pngOnly = true;
            } else if (wcscmp(argv[i], L"--shot-debug") == 0 && i + 1 < argc) {
                const wchar_t* v = argv[++i];
                if (_wcsicmp(v, L"height") == 0)       o.oilDebug = 1;
                else if (_wcsicmp(v, L"normal") == 0)  o.oilDebug = 2;
                else if (_wcsicmp(v, L"contour") == 0) o.oilDebug = 3;
                else if (_wcsicmp(v, L"surface") == 0) o.oilDebug = 6;   // pre-pass raw: r h, g h_oil, b wax
                else if (_wcsicmp(v, L"column") == 0)  o.oilDebug = 7;   // Oil3D C2: r lit column, g mass cov (water_scatter > 0)
                // Oil3D L2 (lamp2_level > 0): r lamp 2's share of the film's light, g lamp 1's
                // occlusion, b lamp 2's. NEGATIVE so no older debug branch (all > 0.5) takes it,
                // and it does not force the OIL_HEIGHT PSO (the tank's own keys do).
                else if (_wcsicmp(v, L"lamps") == 0)   o.oilDebug = -1;
            } else if (wcscmp(argv[i], L"--timings") == 0) {
                o.timings = true;
            } else if (wcscmp(argv[i], L"--shot-dump") == 0 && i + 1 < argc) {
                o.dumpPath = argv[++i];
            } else if (wcscmp(argv[i], L"--shot-stage-tap") == 0 && i + 1 < argc) {
                // comma list of taps / ranges a-b; L = the --shot-debug lamps map
                const wchar_t* v = argv[++i];
                while (*v) {
                    if (*v == L'L' || *v == L'l') { o.stageTaps.push_back(-1); v++; }
                    else {
                        wchar_t* e = nullptr;
                        const long a0 = wcstol(v, &e, 10);
                        long a1 = a0;
                        if (e && *e == L'-') a1 = wcstol(e + 1, &e, 10);
                        for (long t = a0; t <= a1 && t - a0 < 400; t++) o.stageTaps.push_back((int)t);
                        v = (e && e != v) ? e : v + 1;
                    }
                    while (*v == L',' || *v == L' ') v++;
                }
            } else if (wcscmp(argv[i], L"--shot-ini-chain") == 0 && i + 1 < argc) {
                o.iniChain = argv[++i];
            } else if (wcscmp(argv[i], L"--shot-time0") == 0 && i + 1 < argc) {
                o.time0 = _wtof(argv[++i]);
            } else if (wcscmp(argv[i], L"--shot-fps") == 0 && i + 1 < argc) {
                const int f = _wtoi(argv[++i]);
                o.shotFps = f < 30 ? 30 : (f > 240 ? 240 : f);
            } else if (wcscmp(argv[i], L"--test-device-lost") == 0 && i + 1 < argc) {
                o.devLostAt = (float)_wtof(argv[++i]);
            } else if (wcscmp(argv[i], L"--snapshot-at") == 0 && i + 2 < argc) {
                o.snapAt = (float)_wtof(argv[++i]);
                o.snapPng = argv[++i];
            } else if (wcscmp(argv[i], L"--test-murk-async") == 0) {
                o.murkAsync = true;
            } else if (wcscmp(argv[i], L"--photos") == 0 && i + 1 < argc) {
                o.photosDir = argv[++i];
            } else if (wcscmp(argv[i], L"--shot-fade") == 0 && i + 1 < argc) {
                o.fixedFade = (float)_wtof(argv[++i]);
            } else if (wcscmp(argv[i], L"--cover-seeds") == 0 && i + 1 < argc) {
                o.coverSeeds = _wtoi(argv[++i]);
                if (o.coverSeeds < 1) o.coverSeeds = 1;
                if (o.coverSeeds > 64) o.coverSeeds = 64;
            } else if (wcscmp(argv[i], L"--cover-sweep") == 0 && i + 1 < argc) {
                const wchar_t* p = argv[++i];
                while (*p) {
                    wchar_t* end = nullptr;
                    const float v = (float)wcstod(p, &end);
                    if (end == p) break;
                    o.coverSweep.push_back(v);
                    p = end;
                    float sc = -1.0f;
                    if (*p == L'@') {
                        const wchar_t* q = p + 1;
                        const float s = (float)wcstod(q, &end);
                        if (end != q) { sc = s; p = end; }
                    }
                    o.coverScale.push_back(sc);
                    while (*p == L',' || *p == L' ') p++;
                }
            }
        }
        LocalFree(argv);
    }
    g_shotPngOnly = o.pngOnly;
    if (o.out.empty()) {
        ShotLog("[shot] ERROR: --shot needs an output .png path\n");
        return 2;
    }
    if (o.sdrArgBad) {
        ShotLog("[shot] ERROR: --sdr-gamut needs 0, 1 or 2; --panel-primaries needs rx,ry,gx,gy,bx,by\n");
        return 2;
    }
    if (o.sdrWhiteNits < 1.0f) o.sdrWhiteNits = 80.0f;

    // Config: read-only for the whole run. Nothing below may write an ini, a
    // mood, a journey, or an autostart key.
    g_configReadOnly = true;
    InitSettingsPath();
    if (!o.ini.empty()) {
        if (GetFileAttributesW(o.ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
            ShotLog("[shot] ERROR: --ini file not found: %ls\n", o.ini.c_str());
            return 2;
        }
        wcscpy_s(g_configIniPath, MAX_PATH, o.ini.c_str());
    }

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        ShotLog("[shot] ERROR: CoInitializeEx failed\n");
        return 3;
    }

    ShotLog("[shot] FluidWallpaper offscreen capture\n");
    ShotLog("[shot] ini=%ls\n", g_configIniPath);
    char stepsNote[32] = "";   // --shot-fps: only when it is not the default 144
    if (o.shotFps != 144) _snprintf_s(stepsNote, _TRUNCATE, " steps=%d/s", o.shotFps);
    ShotLog("[shot] size=%dx%d seed=%u delay=%.1fs series=%d:%.1fs hdr=%s sdr_white=%.0f nits%s\n",
            o.width, o.height, o.seed, o.delaySec, o.seriesCount, o.seriesInterval,
            o.hdrOn ? "on" : "off", o.sdrWhiteNits, stepsNote);

    FluidConfig cfg;
    LoadSettings();          // shell globals: peak_nits, gamut (from the shot ini)
    LoadFullConfig(cfg);     // full look config
    EnsureBuiltinPresets();  // no-op while read-only
    // Cycle director: [cycle] from the --ini file. Off (the default) = not a
    // single call below changes the render path.
    CycleSetLogger([](const char* s) { ShotLog("%s", s); });
    // brief BY photo stage: its log, --photos, and the black-point join
    PhotoSetLogger([](const char* s) { ShotLog("%s", s); });
    PhotoSetCliDir(o.photosDir.c_str());
    PhotoSetSynchronous(true);
    CycleLoad(g_configIniPath);
    CycleOverride(o.cycleDwell, o.cycleOrder, o.cycleSeed ? o.cycleSeed : o.seed,
                  o.cycleStage, o.cycleForce, o.cycleJitter);
    if (o.cycleDrawTest > 0) {               // statistics only: no renderer, no GPU
        CycleDrawTest(o.cycleDrawTest);
        CoUninitialize();
        return 0;
    }
    // brief CLOCKS: [clocks] from the --ini file; the director's seed (pre-flight 24)
    ClocksSetLogger([](const char* s) { ShotLog("%s", s); });
    ClocksLoad(g_configIniPath);
    ClocksSeed(CycleSeed());
    DirectorSetLogger([](const char* s) { ShotLog("%s", s); });   // Oil3D L4: the colour director
    DirectorSeed(CycleSeed());
    if (o.clocksDryrun > 0.0) {              // scheduler only: no renderer, no GPU (pre-flight 25)
        ClocksDryRun(o.clocksDryrun);
        CoUninitialize();
        return 0;
    }
    if (!o.clocksForce.empty() && !ClocksForce(o.clocksForce.c_str()))
        ShotLog("[clocks] --clocks-force: could not parse all of '%ls'\n", o.clocksForce.c_str());
    const bool cycling = CycleBoot(cfg);

    // HDR state exactly as the shell would resolve it, without querying the
    // real display (see the main loop: sdrScale / peak / SetHdrOptions).
    const bool  hdrActive = o.hdrOn;
    g_maxNits = o.panelMaxNits;
    const float sdrScale = hdrActive ? (o.sdrWhiteNits / 80.0f) : 1.0f;
    ShotLog("[shot] HDR %s, sdrScale=%.4f, peak_nits=%.0f (ini), gamut=%d\n",
            hdrActive ? "ON" : "OFF", sdrScale, g_hdrPeakNits, g_gamutMode);

    FluidRenderer::SetRandomSeed(o.seed);
    FluidRenderer renderer;
    // --test-device-lost: survive the loss like the live wallpaper; otherwise it stays fatal
    if (o.devLostAt >= 0.0f) renderer.SetSurviveDeviceLoss(true);
    if (o.murkAsync) {
        renderer.SetTestMurkAsync(true);
        ShotLog("[shot] --test-murk-async: the murk bakes on the live worker path\n");
    }
    renderer.InitOffscreen(o.width, o.height, cfg);
    if (o.oilDebug > 0) {
        renderer.SetOilHeightDebug(o.oilDebug);
        ShotLog("[shot] --shot-debug %s: OIL_HEIGHT PSO, post pass off (acid look only)\n",
                o.oilDebug == 1 ? "height" : o.oilDebug == 2 ? "normal" : o.oilDebug == 6 ? "surface"
                : o.oilDebug == 7 ? "column" : "contour");
    } else if (o.oilDebug < 0) {
        renderer.SetOilHeightDebug(o.oilDebug);
        ShotLog("[shot] --shot-debug lamps: the OIL_LAMP2 PSO's map (r lamp 2's share, g lamp 1's"
                " occlusion, b lamp 2's), post pass off; needs lamp2_level > 0\n");
    }
    if (o.lamp2Capped) {
        renderer.SetLamp2Capped(true);
        ShotLog("[shot] --shot-lamp2-capped: lamp 2 under L2's CAPPED reference (lamp 1's white track),"
                " not the filled two-lamp default\n");
    }
    if (o.timings) renderer.EnableTimings(true);
    if (o.time0 > 0.0) {
        renderer.SetClock(o.time0);
        DirectorFastForward((float)o.time0);   // Oil3D L4: its CPU state runs forward too
        ShotLog("[shot] master clock starts at %.0f s (%.2f h of uptime)\n", o.time0, o.time0 / 3600.0);
    }
    g_renderer = &renderer;
    // [hdr] sdr_gamut headless: the ini's mode (or --sdr-gamut); the panel
    // primaries only from --panel-primaries (no display is queried), so without
    // it mode 1 uses its documented stand-in (the P3 stretch for a BT.2020 config).
    if (o.sdrGamut >= 0 && o.sdrGamut <= 2) g_sdrGamutMode = o.sdrGamut;
    renderer.SetSdrGamut(g_sdrGamutMode, o.panelPrim, o.panelPrimSet, false);
    {
        float m[9];
        const bool ok = FluidRenderer::SdrMatchMatrix(g_gamutMode, o.panelPrimSet ? o.panelPrim : nullptr, m);
        ShotLog("[shot] sdr_gamut=%d (used only with --hdr off) panel primaries %s", g_sdrGamutMode,
                o.panelPrimSet ? "given" : "not given");
        if (ok) ShotLog(": matrix [%.4f %.4f %.4f | %.4f %.4f %.4f | %.4f %.4f %.4f]",
                        m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]);
        ShotLog("\n");
    }
    // what DXGI reports for the real outputs right now (diagnostic only; nothing is applied)
    {
        ComPtr<IDXGIFactory6> f6;
        if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f6)))) {
            for (UINT ai = 0; ; ai++) {
                ComPtr<IDXGIAdapter1> ad;
                if (FAILED(f6->EnumAdapters1(ai, &ad))) break;
                for (UINT oi = 0; ; oi++) {
                    ComPtr<IDXGIOutput> out;
                    if (FAILED(ad->EnumOutputs(oi, &out))) break;
                    ComPtr<IDXGIOutput6> out6;
                    DXGI_OUTPUT_DESC1 d = {};
                    if (SUCCEEDED(out.As(&out6)) && SUCCEEDED(out6->GetDesc1(&d)))
                        ShotLog("[display] %ls colorspace=%d R(%.4f,%.4f) G(%.4f,%.4f) B(%.4f,%.4f) W(%.4f,%.4f) max=%.0f\n",
                                d.DeviceName, (int)d.ColorSpace, d.RedPrimary[0], d.RedPrimary[1], d.GreenPrimary[0],
                                d.GreenPrimary[1], d.BluePrimary[0], d.BluePrimary[1], d.WhitePoint[0], d.WhitePoint[1],
                                d.MaxLuminance);
                }
            }
        }
    }
    renderer.SetCoverageWanted(CycleCoverageWanted());
    if (!o.coverSweep.empty()) {
        std::vector<float> b, s;
        std::vector<int> z;
        for (size_t k = 0; k < o.coverSweep.size(); k++)
            for (int sd = 0; sd < o.coverSeeds; sd++) {
                b.push_back(o.coverSweep[k]);
                s.push_back(k < o.coverScale.size() ? o.coverScale[k] : -1.0f);
                z.push_back(sd);
            }
        renderer.SetCoverSweep(b, s, z);
    }
    // FINAL-CYCLE C coverage logger: the hue2 mix field's visible-cell shares,
    // CPU only (no render needed to read them). live = the field the frame
    // draws; sweep rows = shadow fields at other film_hue2_cover biases.
    auto coverLog = [&](double t) {
        const LiquidAcidConfig& la = renderer.Config().acid;
        if (!la.enabled || (la.filmHue2Amt <= 0.0005f && la.filmHue3Amt <= 0.0005f)) return;
        float c[10];
        for (int i = -1; i < renderer.CoverSweepCount(); i++) {
            if (!renderer.MixCoverage(i, c)) continue;
            const float b = (i < 0) ? la.filmHue2Cover : renderer.CoverSweepBias(i);
            float sc = (i < 0) ? la.filmHue2Scale : renderer.CoverSweepScale(i);
            if (sc <= 0.0f) sc = la.filmHue2Scale;
            const int sd = (i < 0) ? 0 : renderer.CoverSweepSeed(i);
            ShotLog("[cover] t=%.1f %s bias=%+.3f scale=%.3f nseed=%d hi62=%.3f full68=%.3f band=%.3f "
                    "lo32=%.3f lo28=%.3f lo24=%.3f lo20=%.3f mean=%.3f patches=%.0f big=%.3f\n",
                    t, i < 0 ? "live " : "sweep", b, sc, sd, c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7],
                    c[8], c[9]);
        }
    };
    InitSettingsModel(renderer);
    // --cycle-next from ANOTHER process reaches a headless run through this
    // message-only window (class FluidWallpaperShotCycle, never the live
    // tray's class, so no script can mistake one for the other). Only while
    // cycling: a plain capture creates nothing.
    HWND shotCtl = nullptr;
    if (cycling) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = ShotCycleWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"FluidWallpaperShotCycle";
        RegisterClassW(&wc);
        shotCtl = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                  HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    }

    // --shot-fps N (sheen T7, user 2026-11-16 "no reason to do above 60"): the simulated steps per second; every
    // frame count below follows it. 144 (the default) = the same float / double arithmetic as the literal 144 before.
    const int    shotHz = o.shotFps;
    const float  hzF = (float)shotHz;
    const double hzD = (double)shotHz;
    const float dt = 1.0f / hzF;      // fixed timestep (--shot-fps, default 1/144), no vsync, no sleeping
    FrameInput fin;                   // --mouse-none: all-zero, no user input
    (void)o.mouseNone;

    long long frames = 0;
    long long nextLog = 0;
    const DWORD wallStart = GetTickCount();
    bool  inGame = false;             // --shot-game-fps: the watcher's last word, re-read every 2 s
    DWORD gameChecked = 0;
    bool  gameKnown = false;

    // strip a trailing .png so series/-hdr names hang off a clean stem
    std::wstring stem = o.out;
    if (stem.size() > 4 && _wcsicmp(stem.c_str() + stem.size() - 4, L".png") == 0)
        stem.resize(stem.size() - 4);

    // brief BY pre-flight 34: QPC time of CycleTick..Frame (the Sleep excluded)
    // per frame; the max over each photo LOAD window (request .. upload
    // recorded) against the max over the 5 s of frames before it. Reported at
    // the [state] site. Nothing is timed-dependent: the image is unchanged.
    LARGE_INTEGER qpcF;
    QueryPerformanceFrequency(&qpcF);
    std::vector<float> ftRing(shotHz * 5, 0.0f);   // the last 5 s of frame times
    size_t ftPos = 0, ftFill = 0;
    bool   ftLoading = false;
    float  ftBase = 0.0f, ftLoad = 0.0f;
    long long ftLoadFrames = 0;
    double ftLoadT0 = 0.0;
    std::vector<std::string> ftReports;

    // --snapshot-at (ui3 B5): after each frame, note the frame the request was recorded in and write the
    // picture once TakeSnapshot delivers it (its frame's fence done). Reads only; the frames are untouched.
    // lastFrame = the index of the frame rendered last (it ends at t = (lastFrame + 1) / fps)
    auto snapPoll = [&](long long lastFrame) {
        if (!o.snapAsked || o.snapDone) return;
        if (!o.snapRecorded && !renderer.SnapshotRequested()) { o.snapRecorded = true; o.snapRecFrame = lastFrame; }
        std::vector<uint8_t> px;
        int sw = 0, sh = 0;
        if (!renderer.TakeSnapshot(px, sw, sh)) return;
        o.snapDone = true;
        std::vector<uint8_t> bgr((size_t)sw * sh * 3);
        for (size_t p = 0; p < (size_t)sw * sh; p++) {
            bgr[p * 3 + 0] = px[p * 4 + 2];
            bgr[p * 3 + 1] = px[p * 4 + 1];
            bgr[p * 3 + 2] = px[p * 4 + 0];
        }
        EnsureParentDir(o.snapPng.c_str());
        const bool ok = WritePng(o.snapPng.c_str(), bgr, sw, sh);
        ShotLog("[snapshot] t=%.3fs %dx%d %s %ls (asked in frame %lld, recorded in frame %lld = t %.3fs, "
                "delivered after frame %lld; CPU %.1f ms)\n",
                (o.snapRecFrame + 1) / hzD, sw, sh, ok ? "written" : "NOT written (WIC failed)", o.snapPng.c_str(),
                o.snapAskFrame, o.snapRecFrame, (o.snapRecFrame + 1) / hzD, lastFrame, renderer.SnapshotCpuMs());
    };

    std::vector<float> pixels;
    for (int s = 0; s < o.seriesCount; s++) {
        const double target = o.delaySec + (double)s * o.seriesInterval;
        const long long want = (long long)llround(target * hzD);
        while (frames < want) {
            fin = FrameInput{};
            if (o.pour) {
                const float t = frames / hzF;
                if (t >= o.pourStart && t < o.pourStart + o.pourDur) {
                    const float ang = (t - o.pourStart) * 3.14159265f;   // 0.5 rev/s
                    const float px = o.pourX + 40.0f * cosf(ang);
                    const float py = o.pourY + 40.0f * sinf(ang);
                    static float lx = 0, ly = 0; static bool have = false;
                    fin.mouseX = px; fin.mouseY = py;
                    if (have) { fin.mouseMoved = true; fin.mouseDx = (px - lx) * 5.0f; fin.mouseDy = (py - ly) * 5.0f; }
                    lx = px; ly = py; have = true;
                    fin.mouseDown = true; fin.userInteracted = true;
                }
            }
            // --shot-mouse: one synthetic pointer stroke for the oil modes.
            // Pointer fields only -- never mouseDown, so no splat path can
            // fire even if an ini did have hold_to_splat on.
            if (o.mouse) {
                const float tm = frames / hzF;
                if (tm >= o.mouseAt && tm <= o.mouseAt + o.mouseDur) {
                    const float u = (o.mouseDur > 1e-4f)
                                  ? (tm - o.mouseAt) / o.mouseDur : 1.0f;
                    const float px = o.mouseSx + o.mouseDx * u;
                    const float py = o.mouseSy + o.mouseDy * u;
                    static float mlx = 0, mly = 0; static bool mhave = false;
                    fin.mouseX = px; fin.mouseY = py;
                    if (mhave) {
                        fin.mouseMoved = true;
                        fin.mouseDx = (px - mlx) * 5.0f;
                        fin.mouseDy = (py - mly) * 5.0f;
                    }
                    mlx = px; mly = py; mhave = true;
                }
            }
            // --shot-drop: one ink drop, fired on the first frame at or past
            // its wallpaper time, so the whole run stays reproducible.
            // --shot-preset: swap the whole look mid-run, through the tray's
            // own code path (merge over live state, resolutions, look PSOs).
            if (!o.presetIni.empty() && !o.presetApplied &&
                frames / hzF >= o.presetAt) {
                o.presetApplied = true;
                ShotLog("[shot] applying preset %ls at t=%.2fs\n",
                        o.presetIni.c_str(), frames / hzF);
                ApplyPreset(o.presetIni);
                ShotLog("[shot] look now: %s\n",
                        renderer.Config().ink.enabled ? "ink"
                        : (renderer.Config().acid.enabled ? "liquid_acid" : "fluid"));
            }
            // --test-device-lost: the removal; the next GPU call in the frame path sees it
            if (o.devLostAt >= 0.0f && !o.devLostRemoved && frames / hzF >= o.devLostAt) {
                o.devLostRemoved = true;
                const bool removed = renderer.SimulateDeviceRemoved();
                ShotLog("[devlost] removed at t=%.2fs (frame %lld): %s\n", frames / hzF, frames,
                        removed ? "ID3D12Device5::RemoveDevice()" : "FAILED, no ID3D12Device5");
            }
            if (o.snapAt >= 0.0f && !o.snapAsked && (frames + 1) / hzD >= (double)o.snapAt) {   // --snapshot-at (ui3 B5)
                renderer.RequestSnapshot();
                o.snapAsked = true;
                o.snapAskFrame = frames;
                ShotLog("[snapshot] --snapshot-at %.3f: asked before frame %lld (it ends at t=%.3fs)\n",
                        o.snapAt, frames, (frames + 1) / hzD);
            }
            if (o.drop && !o.dropFired && frames / hzF >= o.dropAt) {
                renderer.QueueDrop(o.dropX, o.dropY, o.dropVy);
                o.dropFired = true;
                ShotLog("[shot] drop injected at t=%.2fs (%.0f,%.0f) vy=%.0f\n",
                        frames / hzF, o.dropX, o.dropY, o.dropVy);
            }
            while (o.shotQueueDone < o.shotQueue.size() && frames / hzF >= o.shotQueue[o.shotQueueDone].t) {
                const auto& q = o.shotQueue[o.shotQueueDone++];
                int qi = -1;
                for (int k = 0; k < DirectorLookCount(); k++) if (_wcsicmp(DirectorLookName(k).c_str(), q.combo.c_str()) == 0) qi = k;
                ShotLog("[shot] --shot-queue %.2f: show next %ls %s%s\n", q.t, q.combo.c_str(), q.mode ? "darkroom" : "vivid",
                        qi < 0 ? " (NO SUCH COMBO)" : "");
                if (qi >= 0) DirectorQueueLook(qi, q.mode);
            }
            if (o.hideAt >= 0.0f && frames / hzF >= o.hideAt) {   // ui3 --shot-hide: inert without the flag
                int hi = -1;
                for (int k = 0; k < DirectorLookCount(); k++) if (_wcsicmp(DirectorLookName(k).c_str(), o.hideCombo.c_str()) == 0) hi = k;
                ShotLog("[shot] --shot-hide %.2f: remove %ls%s (%s)\n", o.hideAt, o.hideCombo.c_str(), hi < 0 ? " (NO SUCH COMBO)" : "",
                        DirectorStatusLine().c_str());
                if (hi >= 0) DirectorHideCombo(hi);
                o.hideAt = -1.0f;
            }
            if (o.keepAt >= 0.0f || o.visitAt >= 0.0f || o.keepSec > 0.0f || o.visitSec > 0.0f) {   // ui3, inert without the flags
                const float tNow = frames / hzF;
                CycleHoldSetClock(tNow);
                if (o.keepAt >= 0.0f && tNow >= o.keepAt) {
                    ShotLog("[keep] --cycle-keep-at %.2f: Keep this for %.0f s (cycle %s, colour director %s)\n", o.keepAt,
                            o.keepSec, CycleState().phase != CYCLE_OFF ? "on" : "off", DirectorActive() ? "on" : "off");
                    CycleKeepThis(o.keepSec);
                    o.keepAt = -1.0f;
                }
                if (o.visitGuest >= 0 && o.visitAt >= 0.0f && tNow >= o.visitAt) {   // ui3: the guest visit (Ink / Fluid)
                    std::string why;
                    const std::wstring preset = CycleGuestPreset(o.visitGuest, &why);
                    ShotLog("[visit] --cycle-visit-at %.2f: visit %s for %.0f s: %d stages before (%d listed), preset %ls\n",
                            o.visitAt, o.visitGuest == 0 ? "Ink" : "Fluid", o.visitSec, (int)CycleGet().stages.size(),
                            CycleListedCount(), preset.empty() ? L"(none)" : preset.c_str());
                    if (preset.empty()) ShotLog("[visit] --cycle-visit-at: no preset (%s)\n", why.c_str());
                    else if (!CycleVisitGuest(preset, o.visitSec)) ShotLog("[visit] --cycle-visit-at: not taken\n");
                    else ShotLog("[visit] --cycle-visit-at: guest stage %d, %d stages now\n", CycleGuestStage() + 1,
                                 (int)CycleGet().stages.size());
                    o.visitAt = -1.0f;
                }
                if (o.visitAt >= 0.0f && tNow >= o.visitAt) {
                    ShotLog("[visit] --cycle-visit-at %.2f: visit stage %d for %.0f s\n", o.visitAt, o.visitStage, o.visitSec);
                    if (!CycleVisit(o.visitStage - 1, o.visitSec)) ShotLog("[visit] --cycle-visit-at: not taken\n");
                    o.visitAt = -1.0f;
                }
            }
            if (cycling) {
                if (shotCtl) {
                    MSG m;
                    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&m);
                        DispatchMessageW(&m);
                    }
                }
                const float tNow = frames / hzF;
                while (o.cycleNextDone < o.cycleNextAt.size() &&
                       tNow >= o.cycleNextAt[o.cycleNextDone]) {
                    ShotLog("[cycle] --cycle-next-at %.2f: Next stage (the tray's call)\n",
                            o.cycleNextAt[o.cycleNextDone]);
                    CycleNext();
                    o.cycleNextDone++;
                }
                while (o.cycleBurstDone < o.cycleBurstAt.size() &&
                       tNow >= o.cycleBurstAt[o.cycleBurstDone]) {
                    ShotLog("[cycle] --cycle-burst-at %.2f\n", o.cycleBurstAt[o.cycleBurstDone]);
                    CycleBurst();
                    o.cycleBurstDone++;
                }
                if (o.pauseAt >= 0.0f && tNow >= o.pauseAt) { CyclePause(); o.pauseAt = -1.0f; }
                if (o.resumeAt >= 0.0f && tNow >= o.resumeAt) { CycleResume(); o.resumeAt = -1.0f; }
            }
            if (o.freezeAnim >= 0) {
                const float tNow = frames / hzF;
                if (o.freezeAt >= 0.0f && tNow >= o.freezeAt) {
                    Freeze((Animator)o.freezeAnim);
                    o.freezeAt = -1.0f;
                }
                if (o.unfreezeAt >= 0.0f && tNow >= o.unfreezeAt) {
                    Unfreeze((Animator)o.freezeAnim);
                    o.unfreezeAt = -1.0f;
                }
            }
            // The director's frame: fade / black / warm-up sub-steps. With the
            // cycle off this is fade 1.0, no black, no steps -- today's frame.
            LARGE_INTEGER qpc0;
            QueryPerformanceCounter(&qpc0);
            const CycleFrame cf = CycleTick(renderer, dt);
            {
                const float tNow = frames / hzF;
                if (o.clocksHoldAt >= 0.0f && tNow >= o.clocksHoldAt) {
                    ShotLog("[clocks] --clocks-hold-at %.2f: hold (the Settings window's call)\n", tNow);
                    ClocksHold(true);
                    o.clocksHoldAt = -1.0f;
                }
                if (o.clocksReleaseAt >= 0.0f && tNow >= o.clocksReleaseAt) {
                    ShotLog("[clocks] --clocks-hold-at %.2f: release (WM_DESTROY's call)\n", tNow);
                    ClocksHold(false);
                    o.clocksReleaseAt = -1.0f;
                }
            }
            ClocksTick(renderer, dt);        // brief CLOCKS: after the director, before the sim
            DirectorTick(renderer, dt);      // Oil3D L4: the colour director, after the clocks
            float peak = g_hdrPeakNits < 0.0f ? g_maxNits : g_hdrPeakNits;   // -1 = panel max
            renderer.SetHdrOptions(peak, g_gamutMode);
            for (int k = 0; k < cf.extraSteps; k++) renderer.SimOnlyStep(cf.stepDt);
            renderer.SetFade(o.fixedFade >= 0.0f ? o.fixedFade : cf.fade);
            renderer.SetBlackOut(cf.black);
            renderer.Frame(dt, sdrScale, hdrActive, fin);
            // --test-device-lost: the renderer-level half of the live loop's recovery
            // (suspendRenderer + resumeRenderer): Shutdown, then a soft init with the same
            // config and the same backoff, and on with the run (review 2026-10-04 R3)
            if (o.devLostAt >= 0.0f && renderer.DeviceLost()) {
                ShotLog("[devlost] detected hr=0x%08lX at t=%.2fs (frame %lld)\n",
                        (unsigned long)renderer.DeviceLostHr(), frames / hzF, frames);
                const FluidConfig keep = renderer.Config();
                renderer.Shutdown();
                ShotLog("[devlost] shutdown ok\n");
                bool ok = false;
                for (int attempt = 1; attempt <= 8 && !ok; attempt++) {
                    ok = renderer.TryInitOffscreen(o.width, o.height, keep);
                    if (!ok) {
                        const DWORD back = 250u << (attempt - 1);
                        ShotLog("[devlost] re-init attempt %d failed, retrying in %lu ms\n",
                                attempt, (unsigned long)back);
                        Sleep(back);
                    }
                }
                if (!ok) {
                    ShotLog("[devlost] ERROR: re-init failed 8 times\n");
                    CoUninitialize();
                    return 5;
                }
                renderer.SetCoverageWanted(CycleCoverageWanted());   // as resumeRenderer does
                ShotLog("[devlost] re-init ok\n");
                o.devLostAwaitFirst = true;
            } else if (o.devLostAwaitFirst) {
                o.devLostAwaitFirst = false;
                ShotLog("[devlost] first frame after recovery ok (t=%.2fs, frame %lld)\n",
                        frames / hzF, frames);
            }
            {   // brief BY pre-flight 34 (see ftRing above)
                LARGE_INTEGER qpc1;
                QueryPerformanceCounter(&qpc1);
                const float ms = (float)(1000.0 * (double)(qpc1.QuadPart - qpc0.QuadPart) / (double)qpcF.QuadPart);
                const bool loading = renderer.PhotoLoading();
                if (loading && !ftLoading) {
                    ftBase = 0.0f;
                    for (size_t k = 0; k < ftFill; k++) ftBase = fmaxf(ftBase, ftRing[k]);
                    ftLoad = 0.0f;
                    ftLoadFrames = 0;
                    ftLoadT0 = frames / hzD;
                }
                if (loading || ftLoading) { ftLoad = fmaxf(ftLoad, ms); ftLoadFrames++; }
                if (!loading && ftLoading) {
                    char b[256];
                    _snprintf_s(b, _TRUNCATE, "[photo] max frame %.2f ms over the load window (%lld frames, "
                                "t %.3f..%.3f) vs %.2f ms over the preceding 5 s baseline (%zu frames)\n",
                                ftLoad, ftLoadFrames, ftLoadT0, (frames + 1) / hzD, ftBase, ftFill);
                    ftReports.push_back(b);
                }
                ftLoading = loading;
                ftRing[ftPos] = ms;
                ftPos = (ftPos + 1) % ftRing.size();
                if (ftFill < ftRing.size()) ftFill++;
            }
            snapPoll(frames);                // --snapshot-at (ui3 B5); nothing without the flag
            frames++;
            // --timings: only the last 10 s before each capture are averaged
            // (the population and the droplets have settled by then)
            if (o.timings && frames == want - 10 * shotHz) {
                double a0, a1, a2, a3, a4; int an;
                renderer.TakeTimings(a0, a1, a2, a3, a4, an);
            }
            // Leave the GPU some air: an unthrottled full-res sim starves the
            // compositor and Wallpaper Engine (the OLED went grey once when
            // two of these ran at once). Never run two shot processes together.
            // --shot-game-fps N (2026-10-29: renders at a low frame rate while a game runs): while
            // a game is in front the loop runs at N frames a second instead of being suspended. Pacing
            // only: the sim steps a fixed 1/shot_fps per frame, so the image is the same at any speed.
            if (o.gameFps > 0 && !o.gameLog.empty()) {
                const DWORD now = GetTickCount();
                if (!gameKnown || now - gameChecked >= 2000) {
                    const bool g = ShotGameInFront(o.gameLog);
                    if (g && (!gameKnown || !inGame))
                        ShotLog("[game] game in front: capped at %d fps (frame %lld)\n", o.gameFps, frames);
                    else if (!g && (!gameKnown || inGame))
                        ShotLog("[game] no game: full speed (frame %lld)\n", frames);
                    inGame = g;
                    gameKnown = true;
                    gameChecked = now;
                }
            }
            if (inGame) {
                LARGE_INTEGER qpc2;
                QueryPerformanceCounter(&qpc2);
                const double ms = 1000.0 * (double)(qpc2.QuadPart - qpc0.QuadPart) / (double)qpcF.QuadPart;
                const double period = 1000.0 / (double)o.gameFps;
                Sleep((DWORD)(ms < period ? period - ms : 1.0));
            } else if (o.yieldMs > 0) {
                Sleep((DWORD)o.yieldMs);
            }
            if (frames >= nextLog) {
                ShotLog("[shot] simulated %.1f s (%lld frames, %.1f s wall)\n",
                        frames / hzD, frames, (GetTickCount() - wallStart) / 1000.0);
                coverLog(frames / hzD);
                nextLog = frames + shotHz * 5;
            }
        }
        if (o.timings) {
            double tSim = 0, tDisp = 0, tPost = 0, tAll = 0, tMax = 0;
            int tN = 0;
            if (renderer.TakeTimings(tSim, tDisp, tPost, tAll, tMax, tN))
                ShotLog("[timings] %d frames (GPU, mean ms): sim %.3f  surface %.3f  display %.3f (max %.3f)  post %.3f  frame %.3f  [oil_height=%d debug=%d %dx%d]\n",
                        tN, tSim, renderer.TakeSurfMs(), tDisp, tMax, tPost, tAll, renderer.Config().acid.oilHeight, o.oilDebug,
                        o.width, o.height);
            {   // P1: per-bracket detail, one ShotLog per line (its buffer is 1 KB)
                const std::string& d = renderer.TimingsDetail();
                size_t a = 0;
                while (a < d.size()) {
                    size_t e = d.find('\n', a);
                    if (e == std::string::npos) e = d.size();
                    ShotLog("%s\n", d.substr(a, e - a).c_str());
                    a = e + 1;
                }
            }
        }
        if (!renderer.CaptureOffscreen(pixels)) {
            ShotLog("[shot] ERROR: readback failed\n");
            renderer.Shutdown();
            CoUninitialize();
            return 4;
        }
        snapPoll(frames - 1);   // --snapshot-at: CaptureOffscreen waited for the GPU, so a copy of the last frame is done
        std::wstring shotStem = stem;
        if (o.seriesCount > 1) {
            wchar_t suffix[32];
            // Whole-second series keep the 3-digit form every sheet script
            // already globs for (reference/configs/ab.py wants prefix-NNN.png).
            // A sub-second series could not use it: 60 / 60.5 / 61 all round
            // to 060 / 061 / 061, so one capture silently overwrote another.
            // Those get TENTHS in four digits instead -- 0600 / 0605 / 0610 --
            // which still sorts in time order. The choice is made once per
            // series, from the delay and the interval, so one run never mixes
            // the two widths.
            const double iv = (double)o.seriesInterval;
            const bool wholeSec = fabs((double)o.delaySec - llround((double)o.delaySec)) < 1e-4
                               && fabs(iv - llround(iv)) < 1e-4;
            // ...and a FRAME-step series (interval well under a tenth, used by
            // the pop detector, which needs consecutive frames) needs finer
            // still: six digits of MILLISECONDS.
            const bool wholeTenth = fabs((double)o.delaySec * 10.0 - llround((double)o.delaySec * 10.0)) < 1e-3
                                 && fabs(iv * 10.0 - llround(iv * 10.0)) < 1e-3;
            if (wholeSec)         swprintf_s(suffix, L"-%03d", (int)llround(target));
            else if (wholeTenth)  swprintf_s(suffix, L"-%04d", (int)llround(target * 10.0));
            else                  swprintf_s(suffix, L"-%06d", (int)llround(target * 1000.0));
            shotStem += suffix;
        }
        WriteShotPair(shotStem, pixels, o.width, o.height, sdrScale, (float)(frames / hzD));
        // --shot-dump: AFTER the shot is written (it re-draws this frame with
        // debug modes and leaves the frame ring as the capture left it). A
        // series gets _NNNN numbering, one json per capture.
        if (!o.dumpPath.empty()) {
            std::wstring dj = o.dumpPath;
            if (o.seriesCount > 1) {
                if (dj.size() > 5 && _wcsicmp(dj.c_str() + dj.size() - 5, L".json") == 0) dj.resize(dj.size() - 5);
                wchar_t sfx[24];
                swprintf_s(sfx, L"_%04d.json", s);
                dj += sfx;
            }
            WriteShotDump(renderer, dj, o);
        }
        if (!o.stageTaps.empty()) WriteStageTaps(renderer, shotStem, o, pixels, sdrScale);
        coverLog(frames / hzD);
        if (cycling || renderer.Config().acid.enabled) {
            char cyc[256] = "cycle=off";
            if (cycling) CycleDescribe(cyc, sizeof(cyc));
            // dir_hue (night-cadence): the film hue the colour director sets, only while it holds it (palette_abs
            // is the clock it overrides); with the director off the line is unchanged
            char dirHue[32] = "";
            if (renderer.DirectorHue() >= 0.0f) snprintf(dirHue, sizeof(dirHue), " dir_hue=%.1f", renderer.DirectorHue());
            ShotLog("[state] t=%.3f %s blobs=%d droplets=%d hue_angle=%.1f palette_hue=%.1f "
                    "palette_abs=%.1f hue2=%.1f fade=%.4f black=%d%s\n",
                    frames / hzD, cyc, renderer.AcidBlobCount(), renderer.AcidDropletCount(),
                    renderer.HueAngleDeg(), renderer.PaletteHueDeg(),
                    fmodf(renderer.PaletteBaseHueDeg() + renderer.PaletteHueDeg(), 360.0f),
                    renderer.Hue2Deg(), renderer.Fade(), renderer.BlackOut() ? 1 : 0, dirHue);
        }
        if (ClocksActive()) ClocksReport();  // brief CLOCKS proof 3e: max |delta| per frame
        for (const std::string& r : ftReports) ShotLog("%s", r.c_str());   // brief BY pre-flight 34
        ftReports.clear();
        // Where the camera rig is at this instant. Every [post] effect hangs
        // off these, and all of them are supposed to be moving, so a series of
        // shots at different t is the only honest test of the motion.
        {
            float rg[8];
            renderer.RigState(rg);
            ShotLog("[rig] lamp %.4f,%.4f  lens %.4f,%.4f  tilt %+.1fdeg  "
                    "focus %.4f  shift %+.3f,%+.3f px\n",
                    rg[0], rg[1], rg[2], rg[3], rg[4], rg[5], rg[6], rg[7]);
            // brief BU: where the lamp-grey region is and what the split tone
            // adds, so a series can be checked against the frames.
            const LiquidAcidConfig& la = renderer.Config().acid;
            if (la.enabled && (la.lampGrey > 1e-4f || la.shadowTone > 1e-4f)) {
                float bu[7];
                renderer.BuState(bu);
                ShotLog("[bu] grey centre %.4f,%.4f corner %d sliding %d  tone %.5f,%.5f,%.5f\n",
                        bu[0], bu[1], (int)bu[2], (int)bu[3], bu[4], bu[5], bu[6]);
            }
            // brief BU-b: the highlight tint, the shared gate and its origin.
            if (la.enabled && (la.shadowTone > 1e-4f || la.highlightToneAmt > 1e-4f ||
                               la.highlightToneDesat > 1e-4f)) {
                float bb[5];
                renderer.BubState(bb);
                ShotLog("[bub] hi %.5f,%.5f,%.5f  gate %.4f  t0 %.2f\n",
                        bb[0], bb[1], bb[2], bb[3], bb[4]);
            }
        }
        // DIAGNOSTIC ONLY (brief A): FW_ACID_DUMP=1 writes a CSV of the acid
        // sim's CPU state beside each capture. Off unless the env var is set.
        {
            wchar_t dumpEnv[8] = {};
            if (GetEnvironmentVariableW(L"FW_ACID_DUMP", dumpEnv, 8) > 0
                && dumpEnv[0] != L'0') {
                std::wstring csv = shotStem + L".csv";
                renderer.DumpAcidCsv(csv.c_str());
            }
        }
    }

    if (o.snapAt >= 0.0f && !o.snapDone)
        ShotLog("[snapshot] --snapshot-at %.3f: NOT written (%s)\n", o.snapAt,
                !o.snapAsked ? "t is past the run's end" : !o.snapRecorded ? "no frame drew the display after the ask"
                                                                          : "the copy never landed");
    ShotLog("[shot] done: %lld frames simulated in %.1f s wall\n",
            frames, (GetTickCount() - wallStart) / 1000.0);
    g_renderer = nullptr;
    renderer.Shutdown();
    CoUninitialize();
    return 0;
}

// ---------------------------------------------------------------------------
// --ui-shot / --ui-dump: the settings window rendered headless on WARP (UI-REHAUL 1a).
// Like --shot: returns before the single-instance mutex, no WorkerW, no tray, no renderer,
// config read-only (auditor pre-flight 7). Flags: --ini <ini> (the config the window edits;
// treated as the applied preset when it carries a [look] section), --hdr on|off,
// --panel-max <nits>, plus the --ui-* flags parsed in ui/ui_window.cpp.
// ---------------------------------------------------------------------------
static bool UiShotModeRequested() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool found = false;
    for (int i = 1; i < argc && !found; i++)
        if (wcscmp(argv[i], L"--ui-shot") == 0 || wcscmp(argv[i], L"--ui-dump") == 0) found = true;
    // ui3 --ui-live: the real Settings window on the same scripted, read-only state (no renderer, no WorkerW, no
    // tray, no mutex, nothing written); it exits when the window is closed
    for (int i = 1; i < argc && !found; i++)
        if (wcscmp(argv[i], L"--ui-live") == 0) found = true;
    LocalFree(argv);
    return found;
}

static int RunUiShotMode() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring ini;
    bool hdrOn = false;
    float panelMax = 1000.0f;
    bool uiLive = false;               // ui3 --ui-live: a real window, so per-monitor DPI as the app has it
    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--ini") == 0 && i + 1 < argc) ini = argv[++i];
        else if (wcscmp(argv[i], L"--hdr") == 0 && i + 1 < argc) hdrOn = _wcsicmp(argv[++i], L"on") == 0;
        else if (wcscmp(argv[i], L"--panel-max") == 0 && i + 1 < argc) panelMax = (float)_wtof(argv[++i]);
        else if (wcscmp(argv[i], L"--ui-live") == 0) uiLive = true;
    }
    g_configReadOnly = true;
    InitSettingsPath();
    if (!ini.empty()) {
        wchar_t full[MAX_PATH];
        if (!GetFullPathNameW(ini.c_str(), MAX_PATH, full, nullptr) ||
            GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) {
            fprintf(stderr, "[ui] --ini file not found: %ls\n", ini.c_str());
            LocalFree(argv);
            return 2;
        }
        wcscpy_s(g_configIniPath, MAX_PATH, full);
        ini = full;
    }
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        LocalFree(argv);
        return 3;
    }
    if (uiLive) SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_hdrActive = hdrOn;
    g_maxNits = panelMax;
    FluidConfig cfg;
    LoadSettings();
    LoadFullConfig(cfg);
    srand(1234);
    UiModelInit(cfg);
    UiLoadActivePreset();
    if (UiActivePresetPath().empty() && !ini.empty() && UiFileHasLookSection(ini))
        UiSetActivePresetPath(ini);    // a preset ini opened directly = that preset applied
    int rc = UiRunHeadless(cfg, argc, argv);
    LocalFree(argv);
    CoUninitialize();
    return rc;
}

// ---------------------------------------------------------------------------

// --cycle-next / --cycle-prev / --cycle-on / --cycle-off: message the RUNNING
// instance (the tray window, like CMD_PAUSE_ON/OFF for away-pause.ps1) and
// exit; never starts a wallpaper. --cycle-target shot sends to a headless
// --shot run that is cycling instead (its own window class). Returns -1 when
// the command line carries none of these, else the process exit code.
static int CycleCliCommand() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    UINT cmd = 0;
    bool shot = false;
    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--cycle-next") == 0)      cmd = CMD_CYCLE_NEXT;
        else if (wcscmp(argv[i], L"--cycle-prev") == 0) cmd = CMD_CYCLE_PREV;
        else if (wcscmp(argv[i], L"--cycle-on") == 0)   cmd = CMD_CYCLE_ON;
        else if (wcscmp(argv[i], L"--cycle-off") == 0)  cmd = CMD_CYCLE_OFF;
        else if (wcscmp(argv[i], L"--cycle-target") == 0 && i + 1 < argc)
            shot = _wcsicmp(argv[++i], L"shot") == 0;
    }
    LocalFree(argv);
    if (!cmd) return -1;
    HWND target = shot ? FindWindowExW(HWND_MESSAGE, nullptr, L"FluidWallpaperShotCycle", nullptr)
                       : FindWindowW(L"FluidWallpaperTray", nullptr);
    if (!target) return 1;
    PostMessageW(target, WM_COMMAND, cmd, 0);
    return 0;
}

// ---------------------------------------------------------------------------
// --demo: a TEST look, live, in its own full-screen window (user 2026-11-21:
// "can you make a demo function maybe? So i get an app popupp and it shows
// the thing on my screen?"). What a screenshot cannot show: the true size,
// the motion and the real HDR brightness of a look that is not live.
//
//   FluidWallpaper.exe --demo --ini <file> [--demo-label "text"] [--seed N]
//                      [--shot-time0 S]
//
// Like --shot it returns before the single-instance mutex and never disturbs a
// running wallpaper: the config is read-only (no ini, mood or autostart write,
// and WpLog is silent, so the live log stays the live copy's), there is no
// tray window (the watchers find the LIVE tray by its class), no WorkerW, no
// hotkey. Unlike --shot it has a real swap chain on a borderless window over
// the primary monitor, runs in real time and reads the real display's HDR
// state. The window's class is not one IsShellOrOwnWindow() knows, so the live
// wallpaper sees an ordinary full-screen app in front and pauses itself: there
// is still one render at a time. The demo stops drawing while it is not the
// foreground window, for the same reason.
// Keys: Esc / Q = close (exit code 0); 1 / 2 = close with exit code 11 / 12
// (the launcher, band\tools\demo.ps1, starts the other config); H = the label.
// The game guard and gpu.lock are the launcher's job.
// ---------------------------------------------------------------------------
static bool DemoModeRequested() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool found = false;
    for (int i = 1; i < argc && !found; i++)
        if (wcscmp(argv[i], L"--demo") == 0) found = true;
    LocalFree(argv);
    return found;
}

static std::wstring g_demoLabel;
static HFONT        g_demoFont = nullptr;
static bool         g_demoQuit = false;
static int          g_demoExit = 0;
static bool         g_demoLabelToggle = false;
static bool         g_demoCamToggle = false;                 // F
static float        g_demoCamPitch = 0.0f, g_demoCamFov = 0.0f, g_demoCamHeight = 0.0f;   // pending key steps
static bool         g_demoCamReliefToggle = false;           // R
static float        g_demoCamRelief = 0.0f;                  // Home / End

static LRESULT CALLBACK DemoLabelWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        HGDIOBJ old = g_demoFont ? SelectObject(dc, g_demoFont) : nullptr;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        InflateRect(&rc, -18, -12);
        DrawTextW(dc, g_demoLabel.c_str(), -1, &rc, DT_LEFT | DT_TOP | DT_NOPREFIX);
        if (old) SelectObject(dc, old);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, m, w, l);
}

static LRESULT CALLBACK DemoWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_KEYDOWN:
        // never DestroyWindow here: the renderer's swap chain is on this
        // window, so the loop ends first, shuts the renderer down, then destroys
        if (w == VK_ESCAPE || w == 'Q')        { g_demoExit = 0;  g_demoQuit = true; }
        else if (w == '1' || w == VK_NUMPAD1)  { g_demoExit = 11; g_demoQuit = true; }
        else if (w == '2' || w == VK_NUMPAD2)  { g_demoExit = 12; g_demoQuit = true; }
        else if (w == 'H')                     g_demoLabelToggle = true;
        // the fish-eye camera, live (no restart, the same picture): F on / off, arrows = tilt and
        // lens width, PgUp / PgDn = height over the surface. Applied in the loop.
        else if (w == 'F')                     g_demoCamToggle = true;
        else if (w == VK_UP)                   g_demoCamPitch += 2.0f;
        else if (w == VK_DOWN)                 g_demoCamPitch -= 2.0f;
        else if (w == VK_RIGHT)                g_demoCamFov += 5.0f;
        else if (w == VK_LEFT)                 g_demoCamFov -= 5.0f;
        else if (w == VK_PRIOR)                g_demoCamHeight += 1.0f;
        else if (w == VK_NEXT)                 g_demoCamHeight -= 1.0f;
        else if (w == 'R')                     g_demoCamReliefToggle = true;   // the domes on / off (flat v1)
        else if (w == VK_HOME)                 g_demoCamRelief += 0.1f;        // taller / lower domes
        else if (w == VK_END)                  g_demoCamRelief -= 0.1f;
        return 0;
    case WM_CLOSE:
        g_demoExit = 0; g_demoQuit = true;
        return 0;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(h, m, w, l);
}

static int RunDemoMode() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::wstring ini;
    unsigned seed = 1234;          // the same start picture for every config (0 = a new one each run)
    double time0 = 0.0;
    float  prewarm = 0.0f;         // --demo-prewarm S: the first S seconds of the picture run fast-forward
    {
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; i < argc; i++) {
            if (wcscmp(argv[i], L"--ini") == 0 && i + 1 < argc)              ini = argv[++i];
            else if (wcscmp(argv[i], L"--demo-label") == 0 && i + 1 < argc)  g_demoLabel = argv[++i];
            else if (wcscmp(argv[i], L"--seed") == 0 && i + 1 < argc)        seed = (unsigned)_wtoi(argv[++i]);
            else if (wcscmp(argv[i], L"--shot-time0") == 0 && i + 1 < argc)  time0 = _wtof(argv[++i]);
            else if (wcscmp(argv[i], L"--demo-prewarm") == 0 && i + 1 < argc) prewarm = (float)_wtof(argv[++i]);
        }
        LocalFree(argv);
    }
    if (ini.empty() || GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        ShotLog("[demo] ERROR: --demo needs --ini <an existing file>\n");
        MessageBoxW(nullptr, L"--demo needs --ini <an existing config file>.", L"Fluid Wallpaper demo", MB_ICONERROR);
        return 2;
    }
    for (size_t k = 0; (k = g_demoLabel.find(L"\\n", k)) != std::wstring::npos; ) g_demoLabel.replace(k, 2, L"\n");

    // Config: read-only for the whole run, exactly as --shot.
    g_configReadOnly = true;
    InitSettingsPath();
    wcscpy_s(g_configIniPath, MAX_PATH, ini.c_str());
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        ShotLog("[demo] ERROR: CoInitializeEx failed\n");
        return 3;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ShotLog("[demo] ini=%ls seed=%u time0=%.0f\n", g_configIniPath, seed, time0);

    FluidConfig cfg;
    LoadSettings();          // shell globals: peak_nits, gamut (from the demo ini)
    LoadFullConfig(cfg);
    EnsureBuiltinPresets();  // no-op while read-only
    CycleLoad(g_configIniPath);
    CycleOverride(-1.0f, -1, seed, -1, false);
    ClocksLoad(g_configIniPath);
    ClocksSeed(CycleSeed());
    DirectorSeed(CycleSeed());
    CycleBoot(cfg);

    const int width  = GetSystemMetrics(SM_CXSCREEN);
    const int height = GetSystemMetrics(SM_CYSCREEN);
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DemoWndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"FluidWallpaperDemo";      // never a class IsShellOrOwnWindow() or a watcher knows
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"Fluid Wallpaper demo",
                                WS_POPUP, 0, 0, width, height, nullptr, nullptr, inst, nullptr);
    if (!hwnd) {
        ShotLog("[demo] ERROR: CreateWindowExW failed (%lu)\n", GetLastError());
        CoUninitialize();
        return 3;
    }
    ShowWindow(hwnd, SW_SHOW);
    if (!SetForegroundWindow(hwnd) || GetForegroundWindow() != hwnd) {
        // Windows refuses the foreground to a process that did not get the last input (a launcher
        // script started us). The user asked for this window: an Alt tap lifts the lock.
        keybd_event(VK_MENU, 0, 0, 0);
        keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
        SetForegroundWindow(hwnd);
    }
    g_monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);

    if (seed) FluidRenderer::SetRandomSeed(seed);
    FluidRenderer renderer;
    renderer.SetSurviveDeviceLoss(true);           // a lost device ends the demo below instead of a fatal box
    if (!renderer.TryInit(hwnd, width, height, cfg)) {
        ShotLog("[demo] ERROR: the swap chain was refused (is a full-screen app holding the screen?)\n");
        DestroyWindow(hwnd);
        MessageBoxW(nullptr, L"The demo could not get the screen (is a full-screen app in front?).",
                    L"Fluid Wallpaper demo", MB_ICONERROR);
        CoUninitialize();
        return 4;
    }
    if (time0 > 0.0) {
        renderer.SetClock(time0);
        DirectorFastForward((float)time0);
    }
    g_renderer = &renderer;
    renderer.SetCoverageWanted(CycleCoverageWanted());
    InitSettingsModel(renderer);

    // the real display's HDR state, as the live path resolves it at startup
    g_hdrActive = QueryHDR(g_monitor, &g_maxNits, g_panelPrim, &g_panelPrimValid);
    g_sdrWhiteNits = GetSdrWhiteNits(g_monitor);
    g_sdrManaged = !g_hdrActive && QueryColorMode(g_monitor) == 1;
    renderer.SetSdrGamut(g_sdrGamutMode, g_panelPrim, g_panelPrimValid, g_sdrManaged);
    ShotLog("[demo] %dx%d, HDR %s (max %.0f nits, SDR white %.0f nits), peak_nits=%.0f, gamut=%d\n", width, height,
            g_hdrActive ? "ON" : "OFF", g_maxNits, g_sdrWhiteNits, g_hdrPeakNits, g_gamutMode);

    // the label: a small always-on-top strip (GDI cannot draw on a flip swap chain's window)
    HWND labelWnd = nullptr;
    if (!g_demoLabel.empty()) {
        const UINT dpi = GetDpiForWindow(hwnd);
        g_demoFont = CreateFontW(-MulDiv(15, (int)dpi, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        RECT tr = { 0, 0, width / 2, 0 };
        HDC sdc = GetDC(nullptr);
        HGDIOBJ old = SelectObject(sdc, g_demoFont);
        DrawTextW(sdc, g_demoLabel.c_str(), -1, &tr, DT_LEFT | DT_TOP | DT_NOPREFIX | DT_CALCRECT);
        SelectObject(sdc, old);
        ReleaseDC(nullptr, sdc);
        WNDCLASSW lc = {};
        lc.lpfnWndProc = DemoLabelWndProc;
        lc.hInstance = inst;
        lc.lpszClassName = L"FluidWallpaperDemoLabel";
        RegisterClassW(&lc);
        labelWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
                                   lc.lpszClassName, L"", WS_POPUP, 28, 28, (tr.right - tr.left) + 36,
                                   (tr.bottom - tr.top) + 24, hwnd, nullptr, inst, nullptr);
        if (labelWnd) {
            SetLayeredWindowAttributes(labelWnd, 0, 215, LWA_ALPHA);
            ShowWindow(labelWnd, SW_SHOWNOACTIVATE);
        }
    }
    bool labelShown = labelWnd != nullptr;
    ULONGLONG labelUntil = GetTickCount64() + 12000;     // then it gets out of the picture's way; H brings it back

    LARGE_INTEGER freq, prev, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);
    const char* why = "closed by the user";
    MSG msg = {};
    bool run = true;
    int  demoModeBefore = 0;                                   // what F goes back to
    float demoReliefBefore = 0.70f;                            // what R goes back to
    bool demoCamPlaced = (renderer.Config().mirror.mode == 5); // the ini placed the camera itself
    // --demo-prewarm S (the user on the first demo, which flipped after ~8 s a view: "doesnt really look
    // different"; a cold start has no ink in the water, no settled droplets, nothing to compare yet): the
    // first S seconds of the picture run as fast as the GPU goes, sim-only steps of 1/60 s with every 12th
    // one a shown frame, so it is watched fast-forwarding instead of a black wait. The keys work meanwhile.
    if (prewarm > 0.5f) {
        const float pdt = 1.0f / 60.0f;
        const int steps = (int)(prewarm * 60.0f);
        const ULONGLONG w0 = GetTickCount64();
        int done = 0;
        for (; done < steps && run && !g_demoQuit; done++) {
            if ((done % 12) == 0) {
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    if (msg.message == WM_QUIT) run = false;
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                if (renderer.DeviceLost() || renderer.PresentBroken()) break;
            }
            const CycleFrame cf = CycleTick(renderer, pdt);
            ClocksTick(renderer, pdt);
            DirectorTick(renderer, pdt);
            if ((done % 12) == 11) {
                const float peak = g_hdrPeakNits < 0.0f ? g_maxNits : g_hdrPeakNits;
                renderer.SetHdrOptions(peak, g_gamutMode);
                renderer.SetFade(cf.fade);
                renderer.SetBlackOut(cf.black);
                renderer.Frame(pdt, g_hdrActive ? (g_sdrWhiteNits / 80.0f) : 1.0f, g_hdrActive, FrameInput{});
            } else {
                renderer.SimOnlyStep(pdt);
            }
        }
        ShotLog("[demo] prewarm: %.0f s of picture (%d steps) in %.1f s\n", done / 60.0f, done,
                (GetTickCount64() - w0) / 1000.0);
        labelUntil = GetTickCount64() + 12000;
        QueryPerformanceCounter(&prev);
    }
    while (run) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) run = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!run || g_demoQuit) break;
        if (renderer.DeviceLost())    { why = "the GPU device was lost"; break; }
        if (renderer.PresentBroken()) { why = "the window was lost"; break; }
        if (labelWnd) {
            if (g_demoLabelToggle) {
                g_demoLabelToggle = false;
                labelShown = !labelShown;
                labelUntil = labelShown ? GetTickCount64() + 12000 : 0;
                ShowWindow(labelWnd, labelShown ? SW_SHOWNOACTIVATE : SW_HIDE);
            } else if (labelShown && GetTickCount64() > labelUntil) {
                labelShown = false;
                ShowWindow(labelWnd, SW_HIDE);
            }
        }
        // the fish-eye camera's keys ([mirror] mode 5): uniforms only, so they take effect on the next frame
        if (g_demoCamToggle || g_demoCamPitch != 0.0f || g_demoCamFov != 0.0f || g_demoCamHeight != 0.0f
            || g_demoCamReliefToggle || g_demoCamRelief != 0.0f) {
            MirrorConfig& mr = renderer.Config().mirror;
            if (g_demoCamToggle) {
                if (mr.mode == 5) mr.mode = demoModeBefore;
                else {
                    demoModeBefore = mr.mode;
                    mr.mode = 5;
                    if (!demoCamPlaced) { mr.centerX = 0.5f; mr.centerY = 1.0f; demoCamPlaced = true; }
                }
            }
            if (mr.mode == 5) {
                mr.camPitch  = fminf(fmaxf(mr.camPitch + g_demoCamPitch, 5.0f), 90.0f);
                mr.camFov    = fminf(fmaxf(mr.camFov + g_demoCamFov, 40.0f), 178.0f);
                mr.camHeight = fminf(fmaxf(mr.camHeight * powf(1.08f, g_demoCamHeight), 0.03f), 3.0f);
                if (g_demoCamReliefToggle) {
                    if (mr.camRelief > 0.001f) { demoReliefBefore = mr.camRelief; mr.camRelief = 0.0f; }
                    else mr.camRelief = demoReliefBefore;
                }
                if (g_demoCamRelief != 0.0f && mr.camRelief > 0.001f)
                    mr.camRelief = fminf(fmaxf(mr.camRelief + g_demoCamRelief, 0.1f), 1.5f);
            }
            g_demoCamReliefToggle = false; g_demoCamRelief = 0.0f;
            g_demoCamToggle = false;
            g_demoCamPitch = g_demoCamFov = g_demoCamHeight = 0.0f;
        }
        // not in front = a trickle of 2 frames a second (the window keeps a picture instead of going
        // black): the wallpaper behind is drawing again then, and two full renders at once are what
        // the GPU lock exists to prevent
        if (GetForegroundWindow() != hwnd) {
            QueryPerformanceCounter(&now);
            if ((double)(now.QuadPart - prev.QuadPart) / (double)freq.QuadPart < 0.5) {
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
                continue;
            }
        }
        QueryPerformanceCounter(&now);
        const float dt = (float)((double)(now.QuadPart - prev.QuadPart) / (double)freq.QuadPart);
        const float kMaxStep = 1.0f / 30.0f;             // a stall must not become a jump (as the live loop)
        const float simDt = (dt < kMaxStep) ? dt : kMaxStep;
        float fpsLim = renderer.Config().fpsLimit;
        if (fpsLim < 10.0f) fpsLim = 10.0f;
        const float refreshHz = MonitorRefreshHz();
        if (fpsLim < refreshHz - 1.0f) {
            if (dt < 1.0f / (fpsLim + 2.0f)) {
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 2, QS_ALLINPUT);
                continue;
            }
        } else if (dt < 1.0f / (refreshHz * 3.0f)) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 1, QS_ALLINPUT);
            continue;
        }
        prev = now;

        FrameInput fin;                                  // the pointer stirs the picture as on the desktop
        {
            static POINT prevCursor = { LONG_MIN, LONG_MIN };
            POINT cp;
            GetCursorPos(&cp);
            if (prevCursor.x != LONG_MIN && (cp.x != prevCursor.x || cp.y != prevCursor.y)) {
                fin.mouseMoved = true;
                fin.mouseDx = (float)(cp.x - prevCursor.x) * 5.0f;
                fin.mouseDy = (float)(cp.y - prevCursor.y) * 5.0f;
                fin.userInteracted = true;
            }
            if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) { fin.mouseDown = true; fin.userInteracted = true; }
            fin.mouseX = (float)cp.x;
            fin.mouseY = (float)cp.y;
            prevCursor = cp;
        }
        const float sdrScale = g_hdrActive ? (g_sdrWhiteNits / 80.0f) : 1.0f;
        const CycleFrame cf = CycleTick(renderer, simDt);
        ClocksTick(renderer, simDt);
        DirectorTick(renderer, simDt);
        const float peak = g_hdrPeakNits < 0.0f ? g_maxNits : g_hdrPeakNits;
        renderer.SetHdrOptions(peak, g_gamutMode);
        for (int k = 0; k < cf.extraSteps; k++) renderer.SimOnlyStep(cf.stepDt);
        renderer.SetFade(cf.fade);
        renderer.SetBlackOut(cf.black);
        renderer.Frame(simDt, sdrScale, g_hdrActive, fin);
    }
    {
        const MirrorConfig& mr = renderer.Config().mirror;
        ShotLog("[demo] camera at the end: mode %d, cam_pitch %.1f, cam_fov %.1f, cam_height %.3f, cam_relief %.2f, centre %.2f %.2f\n",
                mr.mode, mr.camPitch, mr.camFov, mr.camHeight, mr.camRelief, mr.centerX, mr.centerY);
    }
    ShotLog("[demo] end: %s, exit code %d\n", why, g_demoExit);
    g_renderer = nullptr;
    renderer.Shutdown();
    if (labelWnd) DestroyWindow(labelWnd);
    DestroyWindow(hwnd);
    if (g_demoFont) DeleteObject(g_demoFont);
    CoUninitialize();
    return g_demoExit;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // Headless capture: no window, no WorkerW, no tray, no single-instance
    // handshake (a running wallpaper must not be disturbed), no config writes.
    if (ShotModeRequested()) return RunShotMode();
    // --demo: a test look live in its own window; the same guarantees (see RunDemoMode)
    if (DemoModeRequested()) return RunDemoMode();
    {
        const int rc = CycleCliCommand();
        if (rc >= 0) return rc;
    }
    if (UiShotModeRequested()) return RunUiShotMode();

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"FluidWallpaper_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Second launch = "open the settings of the running instance".
        HWND tray = FindWindowW(L"FluidWallpaperTray", nullptr);
        if (tray) {
            // let the running instance take the foreground for the window it opens
            DWORD pid = 0;
            GetWindowThreadProcessId(tray, &pid);
            if (pid) AllowSetForegroundWindow(pid);
            PostMessageW(tray, WM_COMMAND, CMD_SETTINGS, 0);
        } else {
            MessageBoxW(nullptr,
                        L"Fluid Wallpaper is already running but not responding.\n"
                        L"Check Task Manager for FluidWallpaper.exe.",
                        L"Fluid Wallpaper", MB_ICONINFORMATION);
        }
        return 0;
    }

    FluidConfig cfg;
    int cycleStartStage = -1;   // --cycle-stage N (1-based)
    {
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; i < argc; i++) {
            if (wcscmp(argv[i], L"--cycle-stage") == 0 && i + 1 < argc) {
                cycleStartStage = _wtoi(argv[++i]);
            } else if (wcscmp(argv[i], L"--console") == 0) {
                AllocConsole();
                FILE* f;
                freopen_s(&f, "CONOUT$", "w", stdout);
                freopen_s(&f, "CONOUT$", "w", stderr);
            } else if (wcscmp(argv[i], L"--gradient") == 0) {
                cfg.gradientMode = true;
            } else if (wcscmp(argv[i], L"--calibrate") == 0 && i + 1 < argc) {
                cfg.gradientMode = true;   // skips sim resources; pattern path
                cfg.calibratePage = _wtoi(argv[++i]);
            } else if (wcscmp(argv[i], L"--stats") == 0) {
                cfg.stats = true;
            } else if (wcscmp(argv[i], L"--force-render") == 0) {
                g_forceRender = true;
            } else if (wcscmp(argv[i], L"--test-suspend") == 0) {
                g_testSuspend = true;
            } else if (wcscmp(argv[i], L"--simres") == 0 && i + 1 < argc) {
                cfg.simRes = _wtoi(argv[++i]);
            } else if (wcscmp(argv[i], L"--dyeres") == 0 && i + 1 < argc) {
                cfg.dyeRes = _wtoi(argv[++i]);
            } else if (wcscmp(argv[i], L"--fps") == 0 && i + 1 < argc) {
                g_fpsOverride = (float)_wtof(argv[++i]);
            }
        }
        LocalFree(argv);
    }
    setvbuf(stdout, nullptr, _IONBF, 0);

    // Opt the process into dark-mode common controls (same mechanism apps like
    // Notepad++ use: uxtheme ordinal 135 = SetPreferredAppMode(AllowDark)).
    // Makes the DarkMode_Explorer-themed buttons/checkboxes render dark faces.
    if (HMODULE ux = LoadLibraryW(L"uxtheme.dll")) {
        typedef int(WINAPI* FnSetPreferredAppMode)(int);
        if (auto setMode = (FnSetPreferredAppMode)GetProcAddress(ux, MAKEINTRESOURCEA(135)))
            setMode(1);   // AllowDark
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    InitSettingsPath();
    InstallExitLogging();                    // post-night T2: every way out logs an "exit:" line
    LogStartupOrigin();
    LoadSettings();

    LoadFullConfig(cfg);
    EnsureBuiltinPresets();
    // Cycle director ([cycle] in settings.ini). Enabled: the start stage is
    // composed into cfg now and the first frames are its black warm-up.
    CycleLoad(g_configIniPath);
    CycleOverride(-1.0f, -1, 0, cycleStartStage, false);
    CycleBoot(cfg);
    ClocksLoad(g_configIniPath);             // brief CLOCKS: [clocks] from settings.ini
    ClocksSeed(CycleSeed());
    DirectorSeed(CycleSeed());               // Oil3D L4: the colour director

    printf("FluidWallpaper - fluid simulation behind desktop icons\n");

    HWND host = FindWallpaperHost();
    if (!host) {
        MessageBoxW(nullptr, L"Could not find the desktop wallpaper layer (is Explorer running?)",
                    L"Fluid Wallpaper", MB_ICONERROR);
        ExitLog("exit: no desktop wallpaper layer at startup (Explorer not running?)");
        return 1;
    }

    const int width  = GetSystemMetrics(SM_CXSCREEN);
    const int height = GetSystemMetrics(SM_CYSCREEN);
    printf("Primary monitor: %dx%d\n", width, height);

    HWND hwnd = CreateWallpaperWindow(host, width, height);
    g_monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    EnumDisplayMonitors(nullptr, nullptr, FindSecondMonitor, 0);
    if (g_monitor2)
        printf("second monitor found: (%ld,%ld)-(%ld,%ld)\n",
               g_monitor2Rect.left, g_monitor2Rect.top,
               g_monitor2Rect.right, g_monitor2Rect.bottom);

    FluidRenderer renderer;
    // A removed / reset GPU device (driver update or crash) is survived, not fatal: the
    // loop below rebuilds it (review 2026-10-04 R3). --shot keeps it fatal.
    renderer.SetSurviveDeviceLoss(true);
    // STARTUP uses TryInit for the same reason the resume does. Both fatal
    // dialogs the user hit were a swap chain refused while a game owned the
    // output -- once on a resume, once on a launch. A wallpaper that cannot
    // have the screen yet should WAIT for it, not put a message box over the
    // game and quit. A refusal here simply starts the process in the same
    // suspended state a fullscreen pause produces, and the backoff loop in
    // the message loop brings it up as soon as the output is free.
    bool startupOk = renderer.TryInit(hwnd, width, height, cfg);
    if (!startupOk) {
        char fgd[256];
        ForegroundDesc(fgd, sizeof(fgd));
        WpLog("startup Init refused; starting suspended and retrying  %s", fgd);
        printf("startup: the swap chain was refused; waiting for the output\n");
    }
    g_renderer = &renderer;
    renderer.SetCoverageWanted(CycleCoverageWanted());
    InitSettingsModel(renderer);   // the Settings window needs the model on the LIVE path too

    CreateTrayWindow();
    RegisterRateHotkeys(g_nid.hWnd);   // Ctrl+Alt+Up / Down: the ratings

    g_hdrActive = QueryHDR(g_monitor, &g_maxNits, g_panelPrim, &g_panelPrimValid);
    g_sdrWhiteNits = GetSdrWhiteNits(g_monitor);
    g_sdrManaged = !g_hdrActive && QueryColorMode(g_monitor) == 1;
    renderer.SetSdrGamut(g_sdrGamutMode, g_panelPrim, g_panelPrimValid, g_sdrManaged);
    {
        float m[9];
        const bool ok = FluidRenderer::SdrMatchMatrix(g_gamutMode, g_panelPrimValid ? g_panelPrim : nullptr, m);
        WpLog("display: HDR %s%s, primaries %s R(%.4f,%.4f) G(%.4f,%.4f) B(%.4f,%.4f); sdr_gamut=%d, gamut=%d, "
              "match matrix %s [%.4f %.4f %.4f | %.4f %.4f %.4f | %.4f %.4f %.4f]",
              g_hdrActive ? "on" : "off", g_sdrManaged ? " (SDR desktop colour-managed by Windows: HDR stretch used)" : "",
              g_panelPrimValid ? "from DXGI" : "UNKNOWN", g_panelPrim[0], g_panelPrim[1],
              g_panelPrim[2], g_panelPrim[3], g_panelPrim[4], g_panelPrim[5], g_sdrGamutMode, g_gamutMode,
              ok ? "ok" : "not usable (stand-in)", ok ? m[0] : 0.f, ok ? m[1] : 0.f, ok ? m[2] : 0.f, ok ? m[3] : 0.f,
              ok ? m[4] : 0.f, ok ? m[5] : 0.f, ok ? m[6] : 0.f, ok ? m[7] : 0.f, ok ? m[8] : 0.f);
    }
    UpdateTrayTip();
    printf("HDR live state: %s (max %.0f nits, SDR white %.0f nits)\n",
           g_hdrActive ? "ON" : "OFF", g_maxNits, g_sdrWhiteNits);

    LARGE_INTEGER freq, prev, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);

    // Fullscreen-pause suspend: after 20 s of continuous fullscreen/maximized
    // pause, tear the renderer down completely so the foreground app (game)
    // gets all RAM/VRAM back; rebuild transparently when the pause clears.
    // Manual pause alone never suspends — its resume should stay instant.
    static const ULONGLONG kSuspendAfterMs = 20000;
    static ULONGLONG fsPausedSince = 0;   // 0 = not fs-paused right now
    static ULONGLONG fsCardCheckAt = 0;   // fs_vram_pct: the next whole-card check (0 = at kSuspendAfterMs)
    static const ULONGLONG kCardCheckEveryMs = 180000;   // user: "3 minutes i meant"
    bool blackFramePending = true;        // one black frame per manual pause
    // g_suspended (renderer fully torn down) is file scope: the tray's pause query reads it
    static bool fsSuspended = false;      // suspension came from the fs trigger
    // Resume retry state. resumeWanted stays true from the moment something
    // asked for a resume until one actually succeeds, so a refusal is a delay
    // and never a permanently black desktop.
    static bool       resumeWanted = false;
    static int        resumeFails = 0;
    static ULONGLONG  nextResumeTry = 0;
    static FluidConfig savedCfg;          // live config snapshot for the resume
    if (!startupOk) {
        // The startup Init was refused: come up in the SUSPENDED state and let
        // the backoff loop below do exactly what it does after a game exits.
        // Nothing else has to know: from here on this is an ordinary suspend.
        savedCfg = cfg;
        g_suspended = true;
        resumeWanted = true;
        nextResumeTry = 0;
    }
    const ULONGLONG bootTick = GetTickCount64();   // --test-suspend clock

    auto suspendRenderer = [&](const char* why) {
        savedCfg = renderer.Config();
        renderer.Shutdown();
        g_suspended = true;
        SYSTEMTIME st;
        GetLocalTime(&st);
        printf("[%02d:%02d:%02d.%03d] renderer suspended (%s)\n",
               st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, why);
        WpLog("renderer suspended (%s)", why);   // pairs with "renderer resumed" (U42 audit trail)
    };
    auto resumeRenderer = [&]() -> bool {
        // same Init call as startup, then the startup state restore: resolved
        // HDR options (like the per-frame call site does), coverage override,
        // scRGB color space.
        //
        // TryInit, not Init: DXGI refuses a swap chain with E_ACCESSDENIED
        // while another app still holds the output exclusively, and this
        // resume fires at exactly the moment a fullscreen game is handing the
        // output back. That used to reach Fail() -- a fatal dialog over a
        // black desktop, which is how the wallpaper died once. Now a refusal
        // leaves the renderer torn down and we come back later.
        if (!renderer.TryInit(hwnd, width, height, savedCfg)) {
            resumeFails++;
            // 250 ms, 500, 1 s, 2, 4, 8, 16, then every 30 s
            ULONGLONG back = 250ull << (resumeFails - 1 < 6 ? resumeFails - 1 : 6);
            if (back > 30000ull) back = 30000ull;
            nextResumeTry = GetTickCount64() + back;
            char fg[256];
            ForegroundDesc(fg, sizeof(fg));
            WpLog("resume attempt %d failed, retrying in %llu ms  %s",
                  resumeFails, (unsigned long long)back, fg);
            printf("resume failed (attempt %d), retrying in %llu ms\n",
                   resumeFails, (unsigned long long)back);
            // After a couple of minutes of refusals the window itself is the
            // likeliest suspect (Explorer restarted while we were suspended
            // and this hwnd is parented to a WorkerW that no longer exists),
            // so rebuild it before the next try.
            if (resumeFails >= 8) {
                HWND host2 = FindWallpaperHost();
                if (host2) {
                    HWND fresh = CreateWallpaperWindow(host2, width, height);
                    if (fresh) {
                        if (hwnd && fresh != hwnd) DestroyWindow(hwnd);
                        hwnd = fresh;
                        g_monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
                        WpLog("resume: rebuilt the wallpaper window (hwnd=%p)", (void*)hwnd);
                    }
                }
            }
            return false;
        }
        resumeFails = 0;
        nextResumeTry = 0;
        float peak = g_hdrPeakNits < 0.0f ? g_maxNits : g_hdrPeakNits;
        renderer.SetHdrOptions(peak, g_gamutMode);
        renderer.SetCoverageWanted(CycleCoverageWanted());
        renderer.ReassertColorSpace();
        g_suspended = false;
        // Mood/journey state lives in the config, so it survives via savedCfg;
        // purely time-based phases (hue wheel position, transition progress)
        // jump across the suspend — accepted, serializing conductor state
        // isn't worth it for a wallpaper that was invisible anyway.
        QueryPerformanceCounter(&prev);   // don't integrate the suspended gap
        SYSTEMTIME st;
        GetLocalTime(&st);
        printf("[%02d:%02d:%02d.%03d] renderer resumed\n",
               st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        WpLog("renderer resumed");
        return true;
    };

    ULONGLONG lastFsCheck = 0, lastHdrCheck = 0;
    bool wasPaused = false;

    MSG msg = {};
    while (g_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_running = false; if (!g_exitReason) g_exitReason = "WM_QUIT"; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_running) break;

        // GPU device removed / reset (driver update, driver crash / TDR, Win+Ctrl+Shift+B):
        // the renderer has stopped touching the GPU and raised DeviceLost(). Tear it down
        // exactly like a long fullscreen pause and let the resume backoff below build a new
        // device on the same window; the cycle director and the config wait in memory, the
        // mirror comes back through its lifecycle. This used to be a fatal box + exit
        // (review 2026-10-04 R3). Checked before PresentBroken: a dead device is not a lost
        // window, and a reattach onto it could only fail.
        if (!g_suspended && renderer.DeviceLost()) {
            WpLog("device lost (hr=0x%08lX): renderer torn down, rebuilding through the resume backoff",
                  (unsigned long)renderer.DeviceLostHr());
            suspendRenderer("GPU device lost");
            fsPausedSince = 0;
            resumeFails = 0;
            if (g_fsPaused) {
                // a game in front (its own crash, likely): leave it the GPU and come back when
                // it exits, the way the fullscreen suspend does
                fsSuspended = true;
                resumeWanted = false;
            } else {
                fsSuspended = false;
                resumeWanted = true;
                nextResumeTry = 0;
            }
        }

        // Survive Explorer restarts: wait for the new shell, then re-hook.
        // PresentBroken covers the race where the window died mid-frame
        // before its WM_DESTROY reached us.
        if (!g_wallpaperLost && renderer.PresentBroken()) {
            if (IsWindow(hwnd)) DestroyWindow(hwnd);   // triggers the lost flag
            else g_wallpaperLost = true;
        }
        if (g_wallpaperLost) {
            fsPausedSince = 0;   // don't bank suspend time across a shell restart
            static ULONGLONG lastTry = 0;
            ULONGLONG now2 = GetTickCount64();
            if (now2 - lastTry >= 1000) {
                lastTry = now2;
                HWND newHost = FindWallpaperHost();
                if (newHost) {
                    // the window of a refused try is still there: without this each 1 s
                    // retry left one more shown window behind (review 2026-10-04 R10).
                    // Its WM_DESTROY only sets g_wallpaperLost, which is already set.
                    if (IsWindow(hwnd)) DestroyWindow(hwnd);
                    hwnd = CreateWallpaperWindow(newHost, width, height);
                    g_monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
                    // suspended: skip the swapchain-only reattach — the resume
                    // does a full Init() onto this new window instead.
                    // TryReattach: this one creates a swap chain too, and it
                    // can be refused for the same reasons. Staying "lost" just
                    // means this 1 s loop asks again.
                    if (!g_suspended && !renderer.TryReattach(hwnd)) {
                        WpLog("reattach refused, staying lost (hwnd=%p)", (void*)hwnd);
                    } else {
                        g_wallpaperLost = false;
                        QueryPerformanceCounter(&prev);
                    }
                }
            }
            if (g_wallpaperLost) {
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 200, QS_ALLINPUT);
                continue;
            }
        }

        // Second-monitor mirror lifecycle (off by default; settings checkbox)
        {
            static HWND mirrorWnd = nullptr;
            static ULONGLONG lastMirrorTry = 0;
            // suspended: the mirror chain went down with the device — keep
            // wanting it off here; the resume Init leaves MirrorActive()
            // false, so it re-enables on the next iteration
            bool want = !g_suspended &&
                        renderer.Config().mirrorSecond && g_monitor2 != nullptr;

            if (renderer.MirrorActive() && (renderer.MirrorBroken() || g_mirrorStale)) {
                g_mirrorStale = false;
                renderer.DisableMirror();
                g_destroyingMirror = true;
                if (mirrorWnd && IsWindow(mirrorWnd)) DestroyWindow(mirrorWnd);
                g_destroyingMirror = false;
                mirrorWnd = nullptr;
            }
            if (want && !renderer.MirrorActive()) {
                ULONGLONG now3 = GetTickCount64();
                if (now3 - lastMirrorTry >= 2000) {
                    lastMirrorTry = now3;
                    EnumDisplayMonitors(nullptr, nullptr, FindSecondMonitor, 0);
                    HWND host2 = FindWallpaperHost();
                    if (g_monitor2 && host2) {
                        // a suspend (Shutdown) drops the mirror chain but not its window:
                        // without this each resume leaked one window (review U16a)
                        if (mirrorWnd && IsWindow(mirrorWnd)) {
                            g_destroyingMirror = true;
                            DestroyWindow(mirrorWnd);
                            g_destroyingMirror = false;
                        }
                        mirrorWnd = nullptr;
                        int w2 = g_monitor2Rect.right - g_monitor2Rect.left;
                        int h2 = g_monitor2Rect.bottom - g_monitor2Rect.top;
                        mirrorWnd = CreateWallpaperWindowAt(host2, g_monitor2Rect.left,
                                                           g_monitor2Rect.top, w2, h2);
                        renderer.EnableMirror(mirrorWnd, w2, h2);
                        if (!renderer.MirrorActive()) {      // refused: no window left behind per retry
                            g_destroyingMirror = true;
                            DestroyWindow(mirrorWnd);
                            g_destroyingMirror = false;
                            mirrorWnd = nullptr;
                        }
                        float max2 = 0.0f;
                        // a fresh mirror starts from what its monitor reports NOW (nothing kept
                        // from a previous mirror monitor); unknown counts as not colour-managed
                        bool hdr2 = QueryHDR(g_monitor2, &max2, g_mirrorPrim, &g_mirrorPrimValid);
                        float sdrW2 = GetSdrWhiteNits(g_monitor2);
                        float peak2 = hdr2 ? (g_hdrPeakNits < 0.0f ? max2 : g_hdrPeakNits) : 0.0f;
                        renderer.SetMirrorHdr(hdr2 ? sdrW2 / 80.0f : 1.0f, peak2, hdr2);
                        g_mirrorSdrManaged = !hdr2 && QueryColorMode(g_monitor2) == 1;
                        renderer.SetMirrorPrimaries(g_mirrorPrim, g_mirrorPrimValid, g_mirrorSdrManaged);
                    }
                }
            } else if (!want && renderer.MirrorActive()) {
                renderer.DisableMirror();
                g_destroyingMirror = true;
                if (mirrorWnd && IsWindow(mirrorWnd)) DestroyWindow(mirrorWnd);
                g_destroyingMirror = false;
                mirrorWnd = nullptr;
            }
        }

        ULONGLONG tick = GetTickCount64();
        if (tick - lastFsCheck >= 500) {
            lastFsCheck = tick;
            const bool fsWanted = (g_pauseOnFullscreen && FullscreenAppActive()) ||
                                  (g_pauseOnMaximized && MaximizedAppActive());
            // startup_grace_s counts DRAWN time, not uptime (user 2026-10-30: "do 30 seconds of actual rendering"):
            // the shader builds of the first frames (murk / director variants, up to ~30 s) used to eat the grace
            const bool inGrace = g_graceDrawnMs < g_startupGraceMs;
            static bool graceLogged = false;
            if (inGrace && fsWanted && !graceLogged) {
                graceLogged = true;
                WpLog("startup grace: rendering through a fullscreen app for the first %llu s (capped at %d fps)",
                      (unsigned long long)(g_startupGraceMs / 1000), (int)g_startupGraceFps);
            }
            g_fsPaused = !g_forceRender && !inGrace && fsWanted;
            g_graceThrottled = inGrace && fsWanted;
        }
        if (tick - lastHdrCheck >= 2000) {
            lastHdrCheck = tick;
            // a display topology change (monitor re-plugged, wake from deep sleep) can hand
            // out a new HMONITOR; with the cached one every lookup below fails silently
            // (review 2026-10-04 R9: HDR toggles unseen, SDR white read as 80, no game pause)
            if (HMONITOR monNow = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY)) {
                if (monNow != g_monitor) {
                    WpLog("display poll: monitor handle changed %p -> %p", (void*)g_monitor, (void*)monNow);
                    g_monitor = monNow;
                }
            }
            float primNow[6] = {};
            bool primNowValid = false;
            bool hdr = QueryHDR(g_monitor, &g_maxNits, primNow, &primNowValid);
            if (primNowValid) {              // a missed lookup (mode change, wake) keeps the last good read
                memcpy(g_panelPrim, primNow, sizeof(g_panelPrim));
                g_panelPrimValid = true;
            }
            // a missed lookup keeps the last value (the 80-nit fallback dimmed an HDR desktop
            // to a third until the next poll)
            bool sdrWhiteOk = false;
            float sdrWhite = GetSdrWhiteNits(g_monitor, &sdrWhiteOk);
            if (sdrWhiteOk && sdrWhite != g_sdrWhiteNits) g_sdrWhiteNits = sdrWhite;
            if (hdr) g_sdrManaged = false;       // not read while HDR is on; not queried either
            else {
                const int colorMode = QueryColorMode(g_monitor);
                if (colorMode >= 0) g_sdrManaged = colorMode == 1;   // -1 (lookup missed): keep the last value
            }
            renderer.SetSdrGamut(g_sdrGamutMode, g_panelPrim, g_panelPrimValid, g_sdrManaged);   // + tray edits
            // the second monitor: re-scan every poll, so one that was unplugged, re-plugged or
            // moved is noticed (g_monitor2 was never cleared: the mirror kept rendering to a
            // monitor that was gone). A changed one gets the mirror rebuilt for what is there now.
            {
                const HMONITOR was2 = g_monitor2;
                const RECT wasRect2 = g_monitor2Rect;
                g_monitor2 = nullptr;
                EnumDisplayMonitors(nullptr, nullptr, FindSecondMonitor, 0);
                if (renderer.MirrorActive() && (g_monitor2 != was2 || !EqualRect(&wasRect2, &g_monitor2Rect))) {
                    WpLog("display poll: second monitor changed, mirror rebuilt");
                    g_mirrorStale = true;
                }
            }
            // the second-monitor mirror follows ITS monitor's HDR state (review 2026-10-04:
            // it was read once, when the mirror came up, and its gamut came from the primary)
            if (renderer.MirrorActive() && g_monitor2 && !g_mirrorStale) {
                float max2 = 0.0f;
                float prim2[6] = {};
                bool prim2Valid = false;
                const bool hdr2 = QueryHDR(g_monitor2, &max2, prim2, &prim2Valid);
                const float sdrW2 = GetSdrWhiteNits(g_monitor2);
                const float peak2 = hdr2 ? (g_hdrPeakNits < 0.0f ? max2 : g_hdrPeakNits) : 0.0f;
                renderer.SetMirrorHdr(hdr2 ? sdrW2 / 80.0f : 1.0f, peak2, hdr2);
                // a missed lookup keeps the mirror's last good primaries and colour-managed flag
                if (prim2Valid) {
                    memcpy(g_mirrorPrim, prim2, sizeof(g_mirrorPrim));
                    g_mirrorPrimValid = true;
                }
                if (hdr2) g_mirrorSdrManaged = false;
                else {
                    const int cm2 = QueryColorMode(g_monitor2);
                    if (cm2 >= 0) g_mirrorSdrManaged = cm2 == 1;
                }
                renderer.SetMirrorPrimaries(g_mirrorPrim, g_mirrorPrimValid, g_mirrorSdrManaged);
            }
            if (hdr != g_hdrActive) {
                g_hdrActive = hdr;
                UpdateTrayTip();
                renderer.ReassertColorSpace();   // HDR transitions can drop it
                SYSTEMTIME st;
                GetLocalTime(&st);
                printf("[%02d:%02d:%02d.%03d] Windows HDR turned %s (max %.0f nits, SDR white %.0f nits)\n",
                       st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                       hdr ? "ON" : "OFF", g_maxNits, g_sdrWhiteNits);
            }
        }

        bool paused = g_manualPause || g_fsPaused;
        if (paused != wasPaused) {
            wasPaused = paused;
            SYSTEMTIME st;
            GetLocalTime(&st);
            printf("[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
            if (paused && !g_manualPause) {
                wchar_t cls[64] = L"?", exe[64] = L"?";
                HWND fg = GetForegroundWindow();
                if (fg) { GetClassNameW(fg, cls, 64); WindowExeName(fg, exe, 64); }
                printf("paused (%s: class='%ls' exe='%ls')\n",
                       IsZoomed(fg) ? "maximized app" : "fullscreen app", cls, exe);
            } else {
                printf(paused ? "paused (manual)\n" : "resumed\n");
            }
        }

        // Suspend/resume the renderer around long fullscreen pauses. Only
        // g_fsPaused drives this — manual pause alone never suspends.
        if (!g_suspended) {
            if (g_fsPaused && !g_wallpaperLost) {
                if (fsPausedSince == 0) fsPausedSince = tick;
                if (tick - fsPausedSince >= kSuspendAfterMs && g_fsSuspendMode != 2) {
                    // fs_suspend_mode 1: a paused wallpaper only holds VRAM; give it back only when the
                    // OS budget says the foreground app needs it (checked every loop while paused)
                    unsigned long long use = 0, bud = 0;
                    const bool over = renderer.VramOverBudget(&use, &bud);
                    // fs_vram_pct: the WHOLE card (the game + the desktop + every app), every 3 minutes
                    bool cardOver = false;
                    unsigned long long cardUse = 0, cardTotal = 0;
                    if (g_fsSuspendMode == 1 && !over && g_fsVramPct > 0 && tick >= fsCardCheckAt) {
                        fsCardCheckAt = tick + kCardCheckEveryMs;
                        if (renderer.VramCardUsage(&cardUse, &cardTotal) && cardTotal)
                            cardOver = cardUse * 100ull >= cardTotal * (unsigned long long)g_fsVramPct;
                    }
                    if (g_fsSuspendMode == 0 || over || cardOver) {
                        if (over) WpLog("fullscreen pause: VRAM over budget (%.2f of %.2f GB): tearing down",
                                        use / 1073741824.0, bud / 1073741824.0);
                        if (cardOver) WpLog("fullscreen pause: the card is %.0f%% full (%.1f of %.1f GB, fs_vram_pct %d): "
                                            "tearing down", 100.0 * cardUse / cardTotal, cardUse / 1073741824.0,
                                            cardTotal / 1073741824.0, g_fsVramPct);
                        suspendRenderer("fullscreen app, freeing GPU/RAM");
                        fsSuspended = true;
                    }
                }
            } else {
                fsPausedSince = 0;
                fsCardCheckAt = 0;
            }
        } else if (fsSuspended && !g_fsPaused) {
            // fullscreen app exited: rebuild the renderer like startup did
            fsSuspended = false;
            fsPausedSince = 0;
            resumeWanted = true;
            nextResumeTry = 0;
        }

        // Resume (or re-try a refused resume) on its own backoff clock. The
        // game may still own the output for a while after it loses the
        // foreground, so the first attempt is quite likely to be refused --
        // that is normal and costs one log line, not the process.
        if (g_suspended && resumeWanted && !g_wallpaperLost &&
            tick >= nextResumeTry) {
            if (resumeRenderer()) resumeWanted = false;
        }

        // hidden test hook: force one suspend ~3 s after start, resume ~8 s
        if (g_testSuspend) {
            const double el = (double)(tick - bootTick) / 1000.0;
            if (!g_suspended && el >= 3.0 && el < 8.0) {
                printf("[test-suspend] forcing suspend at %.1f s\n", el);
                suspendRenderer("test hook, freeing GPU/RAM");
            } else if (g_suspended && !fsSuspended && !resumeWanted && el >= 8.0) {
                printf("[test-suspend] forcing resume at %.1f s\n", el);
                resumeWanted = true;
                nextResumeTry = 0;
            }
        }

        if (paused || g_suspended) {
            // A MANUAL pause is the user walking away from the desk, so the
            // panel should go dark instead of holding the last frame on an
            // OLED for however long they are gone. Present exactly ONE black
            // frame on the transition into the pause and then stop presenting
            // as before: no repaint loop, no extra GPU work while paused.
            // (A fullscreen-app pause deliberately does NOT do this -- that
            // wallpaper is hidden behind the game anyway, and clearing it
            // would flash black the moment the game exits.)
            if (blackFramePending && !g_suspended && g_manualPause) {
                blackFramePending = false;
                renderer.PresentBlack();
                printf("paused (manual): black frame presented\n");
            }
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 200, QS_ALLINPUT);
            QueryPerformanceCounter(&prev);   // don't integrate the paused gap
            continue;
        }
        blackFramePending = true;   // armed again for the next manual pause

        QueryPerformanceCounter(&now);
        float dt = (float)((double)(now.QuadPart - prev.QuadPart) / (double)freq.QuadPart);
        // A STALL MUST NOT BECOME A JUMP. If something else takes the GPU for a
        // moment (another process building or rendering, a driver hiccup, a
        // resume), the next dt arrives as a tenth of a second or more and the
        // sim integrates all of it in one step -- the whole picture lurches,
        // which is exactly what the user sees as a frame jump. Clamp what the
        // sim and the mood conductor INTEGRATE to a few frames' worth; the
        // measured dt is kept below for the fps stat and the frame cap, so
        // neither the pacing nor fps_limit changes. The --shot path steps a
        // fixed 1/144 s and never reaches this loop, so captures are
        // bit-identical.
        const float kMaxStep = 1.0f / 30.0f;
        const float simDt = (dt < kMaxStep) ? dt : kMaxStep;

        // FPS cap from settings. Present(1, 0) is ALSO vsynced, and the two
        // caps fight each other at high refresh: the wait below has a ~2 ms
        // floor, so on a 240 Hz panel (4.17 ms vblank interval) a loop asking
        // for 240 fps sleeps straight past the vblank it was aiming at, and
        // vsync then rounds the miss up to a whole extra refresh -- a hard
        // 120 fps, which is exactly what the analyzer measured on the ink
        // look. So: when the requested cap is at or above the monitor's own
        // refresh rate, there is nothing for the software cap to do and it is
        // skipped entirely; vsync paces the loop on its own.
        float fpsLim = g_fpsOverride > 0.0f ? g_fpsOverride : renderer.Config().fpsLimit;
        if (fpsLim < 10.0f) fpsLim = 10.0f;
        // startup_grace_fps: the grace drawing through a game runs at a trickle, not the full fps_limit
        if (g_graceThrottled && g_startupGraceFps > 0.0f && fpsLim > g_startupGraceFps)
            fpsLim = g_startupGraceFps < 5.0f ? 5.0f : g_startupGraceFps;
        const float refreshHz = MonitorRefreshHz();
        if (fpsLim < refreshHz - 1.0f) {
            if (dt < 1.0f / (fpsLim + 2.0f)) {
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 2, QS_ALLINPUT);
                continue;
            }
        } else if (dt < 1.0f / (refreshHz * 3.0f)) {
            // Vsync is the pacer now. This floor never trips on a presenting
            // window (a vblank is 3x further apart than it); it exists so that
            // a Present that STOPS blocking -- an occluded wallpaper behind a
            // fullscreen app -- cannot turn the loop into a busy spin.
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 1, QS_ALLINPUT);
            continue;
        }
        prev = now;
        if (dt > 0.0001f)
            g_currentFps = g_currentFps * 0.95f + (1.0f / dt) * 0.05f;

        // Mouse input, Wallpaper Engine style: cursor movement reaches the
        // wallpaper regardless of focus; clicks only when the desktop itself
        // is focused. Coordinates are primary-monitor px (our window is at 0,0).
        FrameInput fin;
        {
            static POINT prevCursor = { LONG_MIN, LONG_MIN };
            POINT cp;
            GetCursorPos(&cp);
            bool onMonitor = MonitorFromPoint(cp, MONITOR_DEFAULTTONULL) == g_monitor;
            HWND fg = GetForegroundWindow();
            wchar_t cls[64] = {};
            if (fg) GetClassNameW(fg, cls, 64);
            bool desktopFocused = wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0;

            if (prevCursor.x != LONG_MIN && (cp.x != prevCursor.x || cp.y != prevCursor.y) && onMonitor) {
                fin.mouseMoved = true;
                fin.mouseDx = (float)(cp.x - prevCursor.x) * 5.0f;
                fin.mouseDy = (float)(cp.y - prevCursor.y) * 5.0f;
                fin.userInteracted = true;
            }
            if (desktopFocused && onMonitor && (GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
                fin.mouseDown = true;
                fin.userInteracted = true;
            }
            fin.mouseX = (float)cp.x;
            fin.mouseY = (float)cp.y;
            prevCursor = cp;
        }

        // SDR mode: DWM maps scRGB 1.0 to panel white, so scale 1. HDR mode:
        // match the user's SDR-content brightness, like the reference did.
        float sdrScale = g_hdrActive ? (g_sdrWhiteNits / 80.0f) : 1.0f;
        // Cycle director (it absorbed the mood conductor): stage dwell, the
        // fluid->fluid lerp, fades, and the black warm-up -- N sim-only steps
        // per shown frame while the target is cleared to black. Off: fade
        // 1.0, no black, no steps, i.e. exactly today's frame.
        const CycleFrame cf = CycleTick(renderer, simDt);
        ClocksTick(renderer, simDt);         // brief CLOCKS: after the director, before the sim
        DirectorTick(renderer, simDt);       // Oil3D L4: the colour director, after the clocks
        float peak = g_hdrPeakNits < 0.0f ? g_maxNits : g_hdrPeakNits;   // -1 = panel max
        renderer.SetHdrOptions(peak, g_gamutMode);
        for (int k = 0; k < cf.extraSteps; k++) renderer.SimOnlyStep(cf.stepDt);
        renderer.SetFade(cf.fade);
        renderer.SetBlackOut(cf.black);
        renderer.Frame(simDt, sdrScale, g_hdrActive, fin);
        // the grace clock: simDt is clamped to 1/30 s, so a frame stalled on a shader build adds 33 ms, not its wait
        if (g_graceDrawnMs < g_startupGraceMs) g_graceDrawnMs += (ULONGLONG)(simDt * 1000.0f);
    }

    printf("Shutting down...\n");
    ExitLog("exit: the main loop ended: %s (manual pause %s, fullscreen pause %s, suspended %s)",
            g_exitReason ? g_exitReason : "g_running cleared with no reason recorded",
            g_manualPause ? "on" : "off", g_fsPaused ? "on" : "off", g_suspended ? "yes" : "no");
    g_shuttingDown = true;
    UnregisterHotKey(g_nid.hWnd, kHotRateUp);
    UnregisterHotKey(g_nid.hWnd, kHotRateDown);
    RemoveTrayIcon();
    renderer.Shutdown();
    if (!g_wallpaperLost) DestroyWindow(hwnd);
    ReleaseMutex(mutex);

    SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, nullptr, SPIF_SENDCHANGE);
    return 0;
}
