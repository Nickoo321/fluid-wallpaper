// Adapter: settings window -> cycle director (see ui_cycle.h). The mood conductor is gone
// (absorbed into src/cycle.cpp, brief CYCLE-DIRECTOR.md SURVEY + DECISION 2026-09-25);
// the four keys.inc rows now point at the director's [cycle] values.
// Phase 1b: the playlist, the composed stage base, the pause-for-editing, the freezes and the
// derived animators' live values all go through here (animators.h), so ui_window.cpp and
// ui_model.cpp never touch the director or the renderer directly. Headless runs can script
// the director's status and the live values (UiCycleScript) for the proof shots.
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <utility>
#include "ui_cycle.h"
#include "ui_model.h"
#include "../cycle.h"
#include "../director.h"
#include "../animators.h"
#include "../journey.h"
#include "../clocks.h"
#include "../app_state.h"

bool*  UiCycleEnabledPtr()    { return CycleUiEnabledPtr(); }
float* UiCycleDwellPtr()      { return CycleUiDwellPtr(); }
float* UiCycleTransitionPtr() { return CycleUiLerpPtr(); }
float* UiCycleJitterPtr()     { return CycleUiJitterPtr(); }

bool UiCycleIsPtr(const void* p) {
    return p == CycleUiEnabledPtr() || p == CycleUiDwellPtr() ||
           p == CycleUiLerpPtr() || p == CycleUiJitterPtr();
}

float UiCycleDefault(const void* p) {
    static const CycleConfig d{};
    if (p == CycleUiDwellPtr())   return d.dwellSec;
    if (p == CycleUiLerpPtr())    return d.lerpSec;
    if (p == CycleUiJitterPtr())  return d.jitter;
    if (p == CycleUiEnabledPtr()) return d.enabled ? 1.0f : 0.0f;
    return 0.0f;
}

namespace {
bool              s_scripted = false;       // headless: the status below replaces CycleState()
UiCycleStatusView s_scriptStatus;
bool              s_liveScripted = false;   // headless: the live values below replace the getters
UiLiveValues      s_scriptLive;
UiLiveValues      s_prevLive;               // live mode: motion = changed within the last 2 s
bool              s_havePrev = false;
ULONGLONG         s_lastMove[UA_COUNT] = {};

unsigned LookOfStage(int cycleLook) {
    return cycleLook == CYCLE_LOOK_ACID ? LOOK_A : cycleLook == CYCLE_LOOK_INK ? LOOK_I : LOOK_F;
}
// the owner's stages only: a guest visit's stage (always the list's last entry, cycle.h CycleVisitGuest) is not one
bool Valid(int i) { return i >= 0 && i < CycleListedCount(); }
bool SamePath(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }
} // namespace

// persists [cycle] enabled; the director fades out to its first stage (or lands the
// current transition and lets go)
void UiCycleSetEnabled(bool on) {
    if (s_scripted && !on) { s_scripted = false; s_scriptStatus = UiCycleStatusView(); }
    CycleSetEnabled(on, true);
}
bool UiCycleOn() { return s_scripted ? s_scriptStatus.on : CycleState().phase != CYCLE_OFF; }
// a peak / gamut edit in the window while cycling is the user's own value (settings.ini,
// and the director's fallback for stages without their own [hdr] keys)
void UiCycleNoteUserPeak(float peak) { if (!s_scripted) CycleSetUserPeak(peak); }
void UiCycleNoteUserGamut(int gamut) { if (!s_scripted) CycleSetUserGamut(gamut); }

// T8 looks: the stills' ratings live in the colour director; the window persists them in settings.ini [looks]
int UiStillCount() { return DirectorLookCount(); }
std::string UiStillName(int i) {
    std::string s = UiNarrow(DirectorLookName(i));
    for (char& c : s) if (c == '_') c = ' ';
    return s;
}
// headless: a scripted rating is memory only, and every config load re-reads the --ini's [looks] (main.cpp ->
// DirectorLoadLooks), which would undo it between the script and the shot; the window shows the scripted value
static std::map<std::pair<int, int>, int> s_roRating;
int UiStillRating(int i, int mode) {
    if (g_configReadOnly) {
        auto it = s_roRating.find({ i, mode ? 1 : 0 });
        if (it != s_roRating.end()) return it->second;
    }
    return (int)lroundf(DirectorLookRating(i, mode));
}
void UiSetStillRating(int i, int mode, int v) {
    DirectorSetLookRating(i, mode, (float)v);
    // confusion 33 (UI3-PLAN): a headless run (--ui-shot / --ui-dump) changes the rating in memory only
    if (g_configReadOnly) { s_roRating[{ i, mode ? 1 : 0 }] = (int)lroundf(DirectorLookRating(i, mode)); return; }
    if (!g_iniPath[0]) return;
    wchar_t key[160], val[16];
    _snwprintf_s(key, _TRUNCATE, L"%s_%s", DirectorLookName(i).c_str(), mode ? L"pair" : L"white");
    _snwprintf_s(val, _TRUNCATE, L"%d", UiStillRating(i, mode));
    WritePrivateProfileStringW(L"looks", key, val, g_iniPath);
}
// the rated look stages (cycle.h stage_N_rating): the cycle owns the ratings and persists them in [looks]
int UiRatedCount() { return CycleRatedCount(); }
std::string UiRatedName(int k) {
    const int j = CycleRatedStage(k);
    if (j < 0) return std::string();
    std::string s = UiNarrow(CycleGet().stages[j].name);
    if (s.rfind("Look - ", 0) == 0) s.erase(0, 7);              // the preset files are named "Look - <name>.ini"
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
int UiRatedRating(int k) { return (int)lroundf(CycleRatedRating(k)); }
void UiSetRatedRating(int k, int v) { CycleSetRatedRating(k, (float)v); }
void UiShowRated(int k) {
    const int j = CycleRatedStage(k);
    if (j >= 0) CycleJump(j);
}
// headless (--ui-queue, the layout proof of the "show next" block): the pool counts as on and the Now line is this text
static bool        s_headlessPool = false;
static std::string s_headlessNow;
void UiQueueHeadless(const std::string& now) { s_headlessPool = true; s_headlessNow = now; }
bool UiStillPoolOn() { return g_renderer ? g_renderer->Config().acid.directorDarkPath == 2 : s_headlessPool; }
// "show next": the queue lives in the colour director (DirectorQueueLook)
void UiQueueStill(int i, int mode) { DirectorQueueLook(i, mode); }
int  UiQueueCount() { return DirectorQueueCount(); }
bool UiQueueAt(int k, int* i, int* mode) { return DirectorQueueAt(k, i, mode); }
void UiQueueClear() { DirectorQueueClear(); }
std::string UiQueueText() {
    std::string s;
    int i = 0, m = 0;
    for (int k = 0; DirectorQueueAt(k, &i, &m); k++) {
        if (k) s += ", then ";
        s += UiStillName(i) + (m ? " darkroom" : " vivid");
    }
    return s;
}
std::string UiNowLookText() { return g_renderer ? DirectorNowText() : s_headlessNow; }

std::wstring UiPresetsDir() {
    wchar_t dir[MAX_PATH];
    GetPresetsDirectory(dir);   // %APPDATA%\FluidWallpaper\presets (the tray's list too)
    return dir;
}

void UiPresetsRescan() {}   // nothing caches the folder: the tray enumerates it on open

// ============================================================================ phase 1b
UiCycleStatusView UiCycleStatus() {
    if (s_scripted) return s_scriptStatus;
    UiCycleStatusView v;
    CycleStatus s = CycleState();
    v.on = s.phase != CYCLE_OFF;
    v.stage = s.stage;
    v.next = s.next;
    v.count = CycleListedCount();          // ui3: a guest visit's stage is not counted
    v.phase = s.phase;
    v.remainingSec = s.remainingSec;
    v.transitioning = s.transitioning;
    v.lerp = s.phase == CYCLE_LERP;
    v.paused = CyclePaused();
    v.pauseLeftSec = CyclePauseRemainingSec();
    return v;
}

int UiCycleStageCount() { return CycleListedCount(); }   // ui3: the playlist never shows a guest visit's stage

UiStageInfo UiCycleStage(int i) {
    UiStageInfo o;
    if (!Valid(i)) return o;
    const CycleConfig& c = CycleGet();
    const CycleStage& st = c.stages[i];
    o.name = st.name; o.file = st.file; o.path = st.path; o.basePath = st.basePath;
    o.look = LookOfStage(st.look);
    o.overlay = st.overlay; o.ok = st.ok;
    o.ownDwell = st.dwellSec > 0.0f;
    o.dwellSec = o.ownDwell ? st.dwellSec : c.dwellSec;
    o.weight = st.weight;
    o.tier = st.tier;
    o.fluid = !st.overlay && st.look == CYCLE_LOOK_FLUID;
    o.rated = !st.rateKey.empty() && !st.overlay && !st.photo;
    return o;
}
bool UiCycleTierMode() {
    for (auto& st : CycleGet().stages) if (st.tier >= 0) return true;
    return false;
}
void UiCycleSetStageTier(int i, int tier) {
    if (!Valid(i)) return;
    CycleConfig c = CycleGet();
    c.stages[i].tier = (tier >= CYCLE_TIER_PROVEN && tier <= CYCLE_TIER_WILD) ? tier : CYCLE_TIER_NONE;
    CycleSet(c);
}

int  UiCycleOrder() { return CycleGet().order; }
void UiCycleSetOrder(int order) {
    CycleConfig c = CycleGet();
    c.order = order == CYCLE_ORDER_FIXED ? CYCLE_ORDER_FIXED : CYCLE_ORDER_ALT_RANDOM;
    CycleSet(c);
}
void UiCycleNext() { CycleNext(); }        // no-ops headless (no renderer)
void UiCyclePrev() { CyclePrev(); }
void UiCycleJump(int i) { CycleJump(i); }

void UiCycleSetStageDwell(int i, float sec) {
    if (!Valid(i)) return;
    CycleConfig c = CycleGet();
    c.stages[i].dwellSec = fminf(fmaxf(sec, 10.0f), kCycleMaxDwellSec);
    CycleSet(c);
}
void UiCycleSetStageWeight(int i, float w) {
    if (!Valid(i)) return;
    CycleConfig c = CycleGet();
    c.stages[i].weight = fmaxf(w, 0.0f);
    CycleSet(c);
}
void UiCycleMoveStage(int i, int dir) {
    int j = i + dir;
    if (!Valid(i) || !Valid(j)) return;
    CycleConfig c = CycleGet();
    std::swap(c.stages[i], c.stages[j]);
    CycleSet(c);
}
void UiCycleRemoveStage(int i) {
    if (!Valid(i)) return;
    CycleConfig c = CycleGet();
    c.stages.erase(c.stages.begin() + i);
    CycleSet(c);
}
void UiCycleAddStage(const std::wstring& path, const std::wstring& base, bool overlay, float dwellSec) {
    CycleConfig c = CycleGet();
    CycleStage st;
    st.file = path;                       // absolute: resolves the same from any ini folder
    st.base = base;                       // a partial "Save as" names its base preset here
    st.overlay = overlay;
    st.dwellSec = fminf(dwellSec, kCycleMaxDwellSec);
    c.stages.push_back(st);
    CycleSet(c);
}
bool UiCycleHasFile(const std::wstring& path) {
    for (auto& st : CycleGet().stages) if (!st.guest && SamePath(st.path, path)) return true;
    return false;
}
void UiCycleSetFileIncluded(const std::wstring& path, bool on) { CycleSetStageIncluded(path, on, 180.0f); }
void UiCycleReplaceFile(const std::wstring& oldPath, const std::wstring& newPath) {
    CycleConfig c = CycleGet();
    bool changed = false;
    for (auto& st : c.stages)
        if (SamePath(st.path, oldPath)) { st.file = newPath; changed = true; }
    if (changed) CycleSet(c);
}

bool UiCycleStageBase(FluidConfig& out) {
    if (!s_scripted) return CycleStageBase(out);
    if (!s_scriptStatus.on || !Valid(s_scriptStatus.stage)) return false;
    const CycleStage& st = CycleGet().stages[s_scriptStatus.stage];
    out = FluidConfig{};                  // the director's Compose, minus the live shell keys
    (void)st;
    CycleComposeStageFiles(s_scriptStatus.stage, out);   // base -> oil_layout -> file, scheme_hue_period
    return true;
}
std::wstring UiCycleStageFile() {
    if (!s_scripted) return CycleStageFile();
    return (s_scriptStatus.on && Valid(s_scriptStatus.stage)) ? CycleGet().stages[s_scriptStatus.stage].path
                                                              : std::wstring();
}
bool UiCycleStageComposesBeyondFile() {
    // the stage UiCycleStageFile() names (review 2026-10-04 R17)
    if (UiCycleStageFile().empty()) return false;
    return CycleStageComposesBeyondFile(s_scripted ? s_scriptStatus.stage : CycleState().stage);
}
void UiCycleRevertStage() { if (!s_scripted) CycleRevertStage(); }

void UiCyclePauseForEditing() {
    if (s_scripted || !g_renderer) return;
    if (CycleState().phase != CYCLE_OFF) CyclePause(600.0f);   // re-armed on input
}
void UiCycleResumeEditing() {
    if (s_scripted || !g_renderer) return;
    if (CyclePaused()) CycleResume();
}
bool UiJourneyActive() { return !s_scripted && g_renderer && JourneyActive(); }
void UiClocksHold(bool on) { if (!s_scripted && g_renderer) ClocksHold(on); }
bool UiClocksRowSettling(const std::string& sec, const std::string& key, float* base) {
    return !s_scripted && g_renderer && ClocksRowSettling(sec.c_str(), key.c_str(), base);
}
void UiClocksBaseCopy(FluidConfig& cfg) { if (g_renderer) ClocksBaseCopy(cfg); }

void  UiFreeze(int a) { if (a >= 0 && a < UA_COUNT) Freeze((Animator)a, 900.0f); }
void  UiUnfreeze(int a) { if (a >= 0 && a < UA_COUNT) Unfreeze((Animator)a); }
void  UiUnfreezeAll() { UnfreezeAll(); }
bool  UiIsFrozen(int a) { return a >= 0 && a < UA_COUNT && IsFrozen((Animator)a); }
float UiFrozenLeftSec(int a) { return (a >= 0 && a < UA_COUNT) ? FrozenRemainingSec((Animator)a) : 0.0f; }
std::string UiAnimatorName(int a) { return UiNarrow(AnimatorName((Animator)a)); }

UiLiveValues UiLive() {
    if (s_liveScripted) {
        UiLiveValues v = s_scriptLive;
        for (int a = 0; a < UA_COUNT; a++) if (UiIsFrozen(a)) v.moving &= ~(1u << a);
        return v;
    }
    UiLiveValues v;
    if (!g_renderer) return v;
    const FluidRenderer& r = *g_renderer;
    v.valid = true;
    r.RigState(v.rig);
    v.hue2Deg = r.Hue2Deg();
    v.hueAngleDeg = r.HueAngleDeg();
    v.paletteHueDeg = r.PaletteHueDeg();
    v.sweepPos = r.PaletteSweepPos();
    ULONGLONG now = GetTickCount64();
    if (s_havePrev) {
        bool rig = false;
        for (int k = 0; k < 8; k++) if (fabsf(v.rig[k] - s_prevLive.rig[k]) > 1e-4f) rig = true;
        if (rig) s_lastMove[UA_RIG] = now;
        if (fabsf(v.hue2Deg - s_prevLive.hue2Deg) > 0.02f) s_lastMove[UA_HUE2] = now;
        if (fabsf(v.hueAngleDeg - s_prevLive.hueAngleDeg) > 0.02f) s_lastMove[UA_HUE_SHIFT] = now;
        if (fabsf(v.paletteHueDeg - s_prevLive.paletteHueDeg) > 0.005f ||
            fabsf(v.sweepPos - s_prevLive.sweepPos) > 1e-4f) s_lastMove[UA_PALETTE] = now;
    }
    s_prevLive = v;
    s_havePrev = true;
    for (int a = 0; a < UA_COUNT; a++)
        if (s_lastMove[a] && now - s_lastMove[a] < 2000 && !UiIsFrozen(a)) v.moving |= 1u << a;
    return v;
}

void UiCycleLoadHeadless(const wchar_t* ini) { CycleLoad(ini); }

std::string UiCycleScript(const std::string& cmdIn, FluidConfig& cfg) {
    char w[4][64] = {};
    int n = sscanf_s(cmdIn.c_str(), "%63s %63s %63s %63s", w[0], 64u, w[1], 64u, w[2], 64u, w[3], 64u);
    std::string c0 = n > 0 ? w[0] : "";
    auto animOf = [](const std::string& s) {
        return s == "palette" ? UA_PALETTE : s == "hue2" ? UA_HUE2 : s == "rig" ? UA_RIG
             : s == "hueshift" ? UA_HUE_SHIFT : s == "transition" ? UA_TRANSITION
             : s == "cycle" ? UA_CYCLE : s == "clocks" ? UA_CLOCKS : -1;
    };
    if (c0 == "cycle") {
        std::string a = n > 1 ? w[1] : "";
        if (a == "off") { s_scripted = false; s_scriptStatus = UiCycleStatusView(); return "ok"; }
        if (a != "stage" || n < 4) return "usage: cycle stage N SECONDS [paused] [lerp]";
        int st = atoi(w[2]) - 1;
        if (!Valid(st)) return "no such stage (the --ini file carries the [cycle] list)";
        s_scripted = true;
        s_scriptStatus = UiCycleStatusView();
        s_scriptStatus.on = true;
        s_scriptStatus.scripted = true;
        s_scriptStatus.stage = st;
        s_scriptStatus.count = CycleListedCount();
        s_scriptStatus.phase = CYCLE_DWELL;
        s_scriptStatus.remainingSec = (float)atof(w[3]);
        s_scriptStatus.paused = cmdIn.find("paused") != std::string::npos;
        s_scriptStatus.pauseLeftSec = s_scriptStatus.paused ? 600.0f : 0.0f;
        if (cmdIn.find("lerp") != std::string::npos) {
            s_scriptStatus.phase = CYCLE_LERP;
            s_scriptStatus.lerp = true;
            s_scriptStatus.transitioning = true;
        }
        // what the director does at a stage's black point: Config := the composed stage
        FluidConfig base;
        if (UiCycleStageBase(base)) {
            const FluidConfig shell = cfg;
            cfg = base;
            cfg.simRes = shell.simRes; cfg.dyeRes = shell.dyeRes;
            cfg.fpsLimit = shell.fpsLimit; cfg.mirrorSecond = shell.mirrorSecond;
            // the stage's shell globals, as the director's Compose applies them
            const CycleStage& cst = CycleGet().stages[st];
            std::vector<std::wstring> files = cst.baseChain;   // the whole chain, as Compose (R18)
            files.push_back(cst.path);
            for (const std::wstring& f : files) {
                if (f.empty()) continue;
                wchar_t b[64] = {};
                GetPrivateProfileStringW(L"hdr", L"peak_nits", L"", b, 64, f.c_str());
                if (b[0]) g_hdrPeakNits = (float)_wtof(b);
                int gm = (int)GetPrivateProfileIntW(L"hdr", L"gamut", g_gamutMode, f.c_str());
                if (gm >= 0 && gm <= 2) g_gamutMode = gm;
            }
        }
        return "ok";
    }
    if (c0 == "live") {
        if (n < 3) return "usage: live rig.focus|rig.tilt|rig.lampx|rig.lampy|hue2|hueangle|palette V [moving]";
        std::string what = w[1];
        float v = (float)atof(w[2]);
        bool moving = n > 3 && std::string(w[3]) == "moving";
        if (!s_liveScripted) {             // unscripted values sit at their bases
            s_scriptLive = UiLiveValues();
            s_scriptLive.rig[0] = cfg.post.lightX;
            s_scriptLive.rig[1] = cfg.post.lightY;
            s_scriptLive.rig[4] = cfg.post.focusTiltAngle;
            s_scriptLive.rig[5] = cfg.post.cameraFocus;
            s_scriptLive.hue2Deg = cfg.acid.filmHue2;
        }
        s_liveScripted = true;
        s_scriptLive.valid = true;
        int a = -1;
        if (what == "rig.focus")      { s_scriptLive.rig[5] = v; a = UA_RIG; }
        else if (what == "rig.tilt")  { s_scriptLive.rig[4] = v; a = UA_RIG; }
        else if (what == "rig.lampx") { s_scriptLive.rig[0] = v; a = UA_RIG; }
        else if (what == "rig.lampy") { s_scriptLive.rig[1] = v; a = UA_RIG; }
        else if (what == "hue2")      { s_scriptLive.hue2Deg = v; a = UA_HUE2; }
        else if (what == "hueangle")  { s_scriptLive.hueAngleDeg = v; a = UA_HUE_SHIFT; }
        else if (what == "palette")   { s_scriptLive.paletteHueDeg = v; a = UA_PALETTE; }
        else return "unknown live value";
        if (moving) s_scriptLive.moving |= 1u << a;
        else        s_scriptLive.moving &= ~(1u << a);
        return "ok";
    }
    if (c0 == "freeze" || c0 == "unfreeze") {
        int a = n > 1 ? animOf(w[1]) : -1;
        if (a < 0) return "unknown animator";
        if (c0 == "freeze") UiFreeze(a); else UiUnfreeze(a);
        return "ok";
    }
    return "unknown command";
}

// ============================================================================ ui3 HOME (2026-11-23)
namespace {
bool      s_nowScripted = false;
UiNowView s_nowScript;
// "Never"'s memory: ini key -> value; -1 = cleared (the ini copy is ignored from then on)
std::map<std::wstring, int> s_beforeNever;
const wchar_t* LooksReadIni() { return g_configIniPath[0] ? g_configIniPath : g_iniPath; }
bool BnGet(const std::wstring& key, int* v) {
    auto it = s_beforeNever.find(key);
    if (it != s_beforeNever.end()) { if (it->second < 0) return false; *v = it->second; return true; }
    wchar_t b[32] = {};
    if (GetPrivateProfileStringW(L"looks", key.c_str(), L"", b, 32, LooksReadIni()) <= 0) return false;
    *v = (int)lroundf(fminf(fmaxf((float)_wtof(b), 0.0f), 10.0f));
    return true;
}
void BnSet(const std::wstring& key, int v) {
    s_beforeNever[key] = v;
    if (g_configReadOnly || !g_iniPath[0]) return;
    wchar_t val[16];
    _snwprintf_s(val, _TRUNCATE, L"%d", v);
    WritePrivateProfileStringW(L"looks", key.c_str(), val, g_iniPath);
}
void BnClear(const std::wstring& key) {
    s_beforeNever[key] = -1;
    if (g_configReadOnly || !g_iniPath[0]) return;
    WritePrivateProfileStringW(L"looks", key.c_str(), nullptr, g_iniPath);   // the key goes
}
std::wstring StillBnKey(int i, int mode) { return DirectorLookName(i) + (mode ? L"_pair_before_never" : L"_white_before_never"); }
std::wstring RatedBnKey(int k) {
    const int j = CycleRatedStage(k);
    return j >= 0 ? CycleGet().stages[j].rateKey + L"_before_never" : std::wstring();
}
} // namespace

UiNowView UiDirectorNow() {
    if (s_nowScripted) return s_nowScript;
    UiNowView v;
    if (!g_renderer) return v;
    const DirectorView d = DirectorNow();
    v.director = d.active && d.combo >= 0;
    v.combo = d.combo; v.mode = d.mode; v.changing = d.changing;
    v.toCombo = d.toCombo; v.toMode = d.toMode;
    v.nextCombo = d.nextCombo; v.nextMode = d.nextMode;
    v.holdLeftSec = d.holdLeftSec; v.keepLeftSec = d.keepLeftSec;
    return v;
}
void UiDirectorNowScript(const UiNowView& v) { s_nowScripted = true; s_nowScript = v; s_nowScript.scripted = true; }
std::string UiStillKey(int i) { return UiNarrow(DirectorLookName(i)); }
int UiStillFind(const std::string& key) {
    int hit = -1;                                  // a listed pair first (a removed one may share its name)
    for (int i = 0; i < UiStillCount(); i++)
        if (_stricmp(UiStillKey(i).c_str(), key.c_str()) == 0) { if (UiPairVisible(i)) return i; if (hit < 0) hit = i; }
    return hit;
}
float UiStillAnchor(int i) { return DirectorLookAnchor(i); }
float UiStillOffset(int i) { return DirectorLookOffset(i); }
void  UiDirectorSkip() { if (!s_scripted && g_renderer) DirectorSkip(); }
bool UiStillBeforeNever(int i, int* white, int* pair) {
    int w = 0, p = 0;
    const bool hw = BnGet(StillBnKey(i, 0), &w), hp = BnGet(StillBnKey(i, 1), &p);
    if (!hw && !hp) return false;
    if (white) *white = hw ? w : 5;
    if (pair) *pair = hp ? p : 5;
    return true;
}
void UiSetStillBeforeNever(int i, int white, int pair) { BnSet(StillBnKey(i, 0), white); BnSet(StillBnKey(i, 1), pair); }
void UiClearStillBeforeNever(int i) { BnClear(StillBnKey(i, 0)); BnClear(StillBnKey(i, 1)); }
bool UiRatedBeforeNever(int k, int* v) { const std::wstring key = RatedBnKey(k); return !key.empty() && BnGet(key, v); }
void UiSetRatedBeforeNever(int k, int v) { const std::wstring key = RatedBnKey(k); if (!key.empty()) BnSet(key, v); }
void UiClearRatedBeforeNever(int k) { const std::wstring key = RatedBnKey(k); if (!key.empty()) BnClear(key); }
bool UiLoadCombosFrom(const std::wstring& ini) { return DirectorLoadCombosFrom(ini.c_str()); }

// ---- ui3 add / remove a pair -------------------------------------------------------------------------------------
namespace {
std::wstring s_appdataOverride;                    // --ui-appdata <dir> (headless only)
std::string  s_lastPairWrite;
// the two files this feature writes, and whether it may write them
void PairTargets(std::wstring& combos, std::wstring& settings, bool& readOnly) {
    if (!s_appdataOverride.empty()) {
        combos = s_appdataOverride + L"\\combos.ini";
        settings = s_appdataOverride + L"\\settings.ini";
        readOnly = false;                          // the scratch folder: these two writes only
        return;
    }
    settings = g_iniPath;                          // %APPDATA%\FluidWallpaper\settings.ini (main.cpp InitSettingsPath)
    combos.clear();
    if (g_iniPath[0]) {
        combos = g_iniPath;
        const size_t sl = combos.find_last_of(L"\\/");
        combos = (sl == std::wstring::npos ? std::wstring(L".") : combos.substr(0, sl)) + L"\\combos.ini";
    }
    readOnly = g_configReadOnly || combos.empty() || settings.empty();
}
void ForgetScriptedRating(int i) { s_roRating.erase({ i, 0 }); s_roRating.erase({ i, 1 }); }
} // namespace
bool UiPairVisible(int i) { return i >= 0 && i < UiStillCount() && !DirectorComboHidden(i) && !DirectorComboIsDraft(i); }
bool UiPairIsDraft(int i) { return DirectorComboIsDraft(i) && !DirectorComboHidden(i); }
bool UiPairBuiltIn(int i) { return DirectorComboIsBuiltIn(i); }
bool UiPairInFlight(int i) { return DirectorComboInFlight(i); }
std::string UiPairSource() { return UiNarrow(DirectorComboSource()); }
std::string UiHueName(float hue) { return DirectorHueName(hue); }
std::string UiPairKeyFor(const std::string& display, int ignore) { return UiNarrow(DirectorComboKeyFor(UiWide(display), ignore)); }
int UiPairAdd(const std::string& display, float anchor, float offset, bool draft) {
    const int i = DirectorAddCombo(UiWide(display), anchor, offset, draft);
    ForgetScriptedRating(i);
    return i;
}
bool UiPairUpdateDraft(int i, const std::string& display, float anchor, float offset) {
    return DirectorUpdateDraft(i, UiWide(display), anchor, offset);
}
void UiPairCancel(int i) {
    if (!DirectorComboIsDraft(i)) return;
    DirectorHideCombo(i);
    ForgetScriptedRating(i);
}
std::string UiPairKeep(int i) {
    DirectorCommitCombo(i);
    ForgetScriptedRating(i);
    std::wstring combos, settings;
    bool ro = true;
    PairTargets(combos, settings, ro);
    s_lastPairWrite = DirectorPersistKeep(i, combos.c_str(), settings.c_str(), ro);
    return s_lastPairWrite;
}
std::string UiPairRemove(int i) {
    if (i < 0 || i >= UiStillCount() || DirectorComboIsBuiltIn(i)) return "refused";
    const UiNowView nv = UiDirectorNow();
    const bool playing = nv.director && nv.combo == i;
    if (playing) UiKeep(0.0f);                     // a keep of it goes first, so the NEXT below is taken
    DirectorHideCombo(i);
    ForgetScriptedRating(i);
    s_beforeNever[StillBnKey(i, 0)] = -1;          // its Never memory goes with it
    s_beforeNever[StillBnKey(i, 1)] = -1;
    if (playing) UiDirectorSkip();                 // on screen: ratings 0 + skip (on its way: it lands, then leaves)
    std::wstring combos, settings;
    bool ro = true;
    PairTargets(combos, settings, ro);
    s_lastPairWrite = DirectorPersistRemove(i, combos.c_str(), settings.c_str(), ro);
    return s_lastPairWrite;
}
std::string UiPairLastWrite() { return s_lastPairWrite; }
float UiStillRatingDirector(int i, int mode) { return DirectorLookRating(i, mode); }
void UiSetAppdataOverride(const std::wstring& dir) { s_appdataOverride = dir; }

// ---- ui3 Keep this and visits ----------------------------------------------------------------------------------
// Live: the cycle's own calls (CycleKeepThis / CycleVisit decide what holds). Headless (no renderer, nothing ticks):
// the same hold state is set from the scripted view, so a click shows what the window would show; no jump happens.
void UiKeep(float sec) {
    if (g_renderer) { CycleKeepThis(sec); return; }
    if (!(sec > 0.0f)) {
        if (!CycleHoldIsVisit()) CycleHold(0.0f);
        DirectorKeep(0.0f);
        return;
    }
    if (UiCycleOn()) CycleHold(sec);
    if (UiDirectorNow().director) DirectorKeep(sec);
}
float UiKeepLeftSec() {
    const float c = CycleHoldIsVisit() ? 0.0f : CycleHoldLeftSec();
    return fmaxf(c, DirectorKeepLeftSec());
}
static const std::wstring& GuestPresetCached(int kind, std::string* why);   // below
UiVisitView UiVisitNow() {
    UiVisitView v;
    if (!CycleHoldIsVisit() || !(CycleHoldLeftSec() > 0.0f)) return v;
    const int g = CycleGuestStage();
    if (g >= 0 && CycleHoldStage() == g) {   // a visit to Ink / Fluid: which one, by its preset file
        const std::wstring f = CycleGuestFile();
        for (int kind = 0; kind < 2; kind++) {
            const std::wstring& p = GuestPresetCached(kind, nullptr);
            if (!p.empty() && SamePath(p, f)) { v.on = true; v.guest = kind; v.leftSec = CycleHoldLeftSec(); }
        }
        return v;
    }
    for (int k = 0; k < CycleRatedCount(); k++)
        if (CycleRatedStage(k) == CycleHoldStage()) { v.on = true; v.rated = k; v.leftSec = CycleHoldLeftSec(); }
    return v;
}
void UiVisit(int ratedK, float sec) {
    const int j = CycleRatedStage(ratedK);
    if (j < 0) return;
    if (g_renderer) { CycleVisit(j, sec); return; }
    DirectorKeep(0.0f);
    CycleHold(sec, true, j);
}
void UiVisitEnd() {
    CycleVisitEndNow();
    // headless (nothing ticks): a guest visit is let go at once, which takes the guest out of the list (cycle.cpp)
    if (!g_renderer && CycleGuestStage() >= 0) CycleHold(0.0f);
}
// ---- ui3 visits to Ink / Fluid ----------------------------------------------------------------------------------
// the start preset of Ink (0) / Fluid (1): the folder scan at most once a second per look (HOME asks every frame)
static const std::wstring& GuestPresetCached(int kind, std::string* why) {
    static ULONGLONG at[2] = {};
    static std::wstring path[2];
    static std::string reason[2];
    const ULONGLONG now = GetTickCount64();
    if (!at[kind] || now - at[kind] > 1000) {
        reason[kind].clear();
        path[kind] = CycleGuestPreset(kind, &reason[kind]);
        at[kind] = now;
    }
    if (why) *why = reason[kind];
    return path[kind];
}
bool UiGuestAvailable(int kind, std::string* why) {
    if (kind < 0 || kind > 1) return false;
    return !GuestPresetCached(kind, why).empty();
}
void UiVisitGuest(int kind, float sec) {
    std::string why;
    const std::wstring p = CycleGuestPreset(kind, &why);
    if (p.empty()) return;
    CycleVisitGuest(p, sec);                 // live: the jump and the hold; headless: appended and held (no jump)
}
int UiGuestStage() { return CycleGuestStage(); }
std::string UiGuestFile() {
    const std::wstring f = CycleGuestFile();
    const size_t sl = f.find_last_of(L"\\/");
    return UiNarrow(sl == std::wstring::npos ? f : f.substr(sl + 1));
}
int UiCycleStageCountAll() { return (int)CycleGet().stages.size(); }
