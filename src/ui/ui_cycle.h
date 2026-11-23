// The settings window's ONE adapter onto the cycle / preset-folder machinery.
//
// It forwards to the cycle director (src/cycle.h, src/animators.h); the mood conductor it
// used to wrap is gone. Nothing else in src/ui/ (keys.inc included) may call the director's
// internals directly -- the richer model (CyclePause, CycleStageBase, freezes, in-cycle
// flags) is in src/animators.h for phase 1b.
#pragma once
#include <string>

bool*  UiCycleEnabledPtr();          // [cycle] enabled   (keys.inc row pointers)
float* UiCycleDwellPtr();            // [cycle] dwell     (s, default stage dwell, max 900)
float* UiCycleTransitionPtr();       // [cycle] lerp      (s, fluid -> fluid transition)
float* UiCycleJitterPtr();           // [cycle] jitter    (+- fraction of the dwell)
bool   UiCycleIsPtr(const void* p);  // one of the four above (pointer-exception list)
float  UiCycleDefault(const void* p);// code default for one of the four
void   UiCycleSetEnabled(bool on);   // persists + starts / lets go of the director
bool   UiCycleOn();                  // the director is running
void   UiCycleNoteUserPeak(float peak);    // peak / gamut edited by the user while cycling
void   UiCycleNoteUserGamut(int gamut);

// T8 looks (user 2026-11-18: "each is supposed to be rated ... Put it into the UI"): the stills' ratings, 0..10.
// mode 0 = a colour pair under white light, 1 = its darkroom pair. A set also writes settings.ini [looks].
int         UiStillCount();
std::string UiStillName(int i);
int         UiStillRating(int i, int mode);
void        UiSetStillRating(int i, int mode, int v);
bool        UiStillPoolOn();         // the running look draws its next rest by these ratings (director_dark_path 2)
// The rated look stages (the idea looks; cycle.h stage_N_rating; user 2026-11-23: "its jsut a low rating"): rows of
// the same table, one rating each (a shape, in whatever colours are running).
int         UiRatedCount();
std::string UiRatedName(int k);
int         UiRatedRating(int k);
void        UiSetRatedRating(int k, int v);
void        UiShowRated(int k);      // "show next": the cycle fades to it now
// "show next" (user 2026-11-21: "in the ui can i queue stuff?", "ok add that"): line a look up; the director takes
// the front of the line instead of drawing by rating. Not saved: a queue is for now.
void        UiQueueStill(int i, int mode);
int         UiQueueCount();
bool        UiQueueAt(int k, int* i, int* mode);   // the k-th entry of the queue (false = none)
std::string UiQueueText();           // "blue coral darkroom, then teal orange vivid"
void        UiQueueClear();
std::string UiNowLookText();         // what is on screen in plain words ("" = no colour director running)
void        UiQueueHeadless(const std::string& now);   // --ui-queue: no renderer, the block shown with this Now text

std::wstring UiPresetsDir();         // the one preset folder (%APPDATA%\FluidWallpaper\presets)
void   UiPresetsRescan();            // after a Save / Save as (no-op: nothing caches it)

// ---- phase 1b: the director + animators as the settings window sees them -------------------
// Everything below forwards to src/animators.h / src/cycle.h when the wallpaper is running.
// Headless (--ui-shot) there is no renderer: the stage list is loaded from the --ini file, and
// the status + live animator values can be SCRIPTED (UiCycleScript) so the proof shots can show
// "stage 3, 2:10 left" or "the rig is moving the focus" without a render.
#include <vector>
#include "../fluid.h"

struct UiStageInfo {
    std::wstring name, file, path, basePath;  // stem, as written, absolute, absolute base
    unsigned look = 1;                        // LOOK_F / LOOK_A / LOOK_I (ui_model.h bits)
    bool  overlay = false, ok = false;
    float dwellSec = 180.0f;                  // effective (stage's own, else [cycle] dwell)
    bool  ownDwell = false;
    float weight = 1.0f;                      // tier mode: the multiplier inside the tier
    int   tier = -1;                          // stage_N_tier: -1 none, 0 proven, 1 moderate, 2 wild
    bool  fluid = false;                      // a WE look stage (alternates; not drawn by tier)
    bool  rated = false;                      // a rated look stage (stage_N_rating): drawn by its LOOKS rating
};
struct UiCycleStatusView {
    bool  on = false;
    int   stage = -1, next = -1, count = 0;   // 0-based
    int   phase = 0;                          // CyclePhase
    float remainingSec = 0.0f;
    bool  transitioning = false, lerp = false;
    bool  paused = false;                     // dwell timer held (window open)
    float pauseLeftSec = 0.0f;
    bool  scripted = false;
};
UiCycleStatusView UiCycleStatus();
int          UiCycleStageCount();
UiStageInfo  UiCycleStage(int i);
int          UiCycleOrder();                  // 0 fixed, 1 alternate_random
void         UiCycleSetOrder(int order);
void         UiCycleNext();
void         UiCyclePrev();
void         UiCycleJump(int i);
void         UiCycleSetStageDwell(int i, float sec);
void         UiCycleSetStageWeight(int i, float w);   // tier mode: the within-tier multiplier
void         UiCycleSetStageTier(int i, int tier);    // stage_N_tier (-1 clears it)
bool         UiCycleTierMode();                       // any stage carries a tier
void         UiCycleMoveStage(int i, int dir);           // -1 up, +1 down
void         UiCycleRemoveStage(int i);
// append a stage for a preset file (absolute); base = its [meta] base (may be empty)
void         UiCycleAddStage(const std::wstring& path, const std::wstring& base, bool overlay, float dwellSec);
bool         UiCycleHasFile(const std::wstring& path);
void         UiCycleSetFileIncluded(const std::wstring& path, bool on);
void         UiCycleReplaceFile(const std::wstring& oldPath, const std::wstring& newPath);   // rename
// composed base of the current stage (what "dirty" is measured against), its file, revert
bool         UiCycleStageBase(FluidConfig& out);
std::wstring UiCycleStageFile();
// the running stage composes a stage base / oil_layout / scheme_hue_period its file does not
// name: "Save as, only my changes" saves self-contained instead (review 2026-10-04 R17)
bool         UiCycleStageComposesBeyondFile();
void         UiCycleRevertStage();
// window open => dwell timer paused (auto-resume after 10 min without input); close => resume
void         UiCyclePauseForEditing();
void         UiCycleResumeEditing();
bool         UiJourneyActive();
// brief CLOCKS (src/clocks.h): the window holds the slow key clocks while it is open
// (every factor glides to 1, pre-flight 7); a clocked row still gliding is locked
// (pre-flight 8); undo snapshots take the base (pre-flight 3).
void         UiClocksHold(bool on);
bool         UiClocksRowSettling(const std::string& sec, const std::string& key, float* base);
void         UiClocksBaseCopy(FluidConfig& cfg);

// freezes (animators.h Animator ids)
enum UiAnim { UA_PALETTE = 0, UA_HUE2, UA_RIG, UA_HUE_SHIFT, UA_TRANSITION, UA_CYCLE, UA_CLOCKS, UA_COUNT };
void         UiFreeze(int a);                 // 15 min auto-release
void         UiUnfreeze(int a);
void         UiUnfreezeAll();
bool         UiIsFrozen(int a);
float        UiFrozenLeftSec(int a);
std::string  UiAnimatorName(int a);

// live values of the DERIVED animators (the renderer's const getters) + which are moving now
struct UiLiveValues {
    bool  valid = false;
    float rig[8] = {};        // lampX lampY axisX axisY tiltDeg focus shiftX shiftY
    float hue2Deg = 0.0f;     // film_hue2 after the wobble
    float hueAngleDeg = 0.0f; // fluid hue-shift angle
    float paletteHueDeg = 0.0f;
    float sweepPos = -1.0f;
    unsigned moving = 0;      // bit (1 << UiAnim): changed within the last ~2 s
};
UiLiveValues UiLive();

// ---- ui3 HOME (2026-11-23): the colour director as the HOME screen reads it ---------------------------------
struct UiNowView {
    bool  director = false;        // a colour pair is on screen (the colour director runs)
    int   combo = -1, mode = 0;    // the pair on screen; 0 vivid, 1 darkroom
    bool  changing = false;        // a change is on screen (no hold)
    int   toCombo = -1, toMode = 0;      // where that change goes (-1 unknown)
    int   nextCombo = -1, nextMode = 0;  // during a hold: the front of the queue (-1 = drawn at random)
    float holdLeftSec = -1.0f;     // the hold on screen, seconds left (-1 = not a hold)
    float keepLeftSec = 0.0f;
    bool  scripted = false;
};
UiNowView   UiDirectorNow();
void        UiDirectorNowScript(const UiNowView& v);   // headless "now ...": replaces the director's view
std::string UiStillKey(int i);       // the combo's own name ("blue_coral")
int         UiStillFind(const std::string& key);   // index of a combo name, -1 = none
float       UiStillAnchor(int i);    // the film hue (deg)
float       UiStillOffset(int i);    // the second colour = anchor + offset
void        UiDirectorSkip();        // NEXT for the director alone (Never on the pair that is playing)
// "Never": the ratings a pair had before, kept in settings.ini [looks] <combo>_white_before_never /
// <combo>_pair_before_never (the director reads only the exact _white / _pair keys). Memory first; written to
// settings.ini only when the config is writable (never headless).
bool        UiStillBeforeNever(int i, int* white, int* pair);
void        UiSetStillBeforeNever(int i, int white, int pair);
void        UiClearStillBeforeNever(int i);
bool        UiRatedBeforeNever(int k, int* v);    // <rateKey>_before_never
void        UiSetRatedBeforeNever(int k, int v);
void        UiClearRatedBeforeNever(int k);
// --ui-combos <ini>: the pair list for a headless shot (before anything loads the list)
bool        UiLoadCombosFrom(const std::wstring& ini);
// ---- ui3 add / remove a pair (director.h DirectorAddCombo ...). Indices are the director's; a pair the window
// lists is a VISIBLE one (neither a draft nor removed). Writes go to %APPDATA%\FluidWallpaper\combos.ini and
// settings.ini [looks], never while the config is read-only (headless), except under --ui-appdata <dir> (headless
// only: both files of this feature go to that folder and these two writes are allowed).
bool        UiPairVisible(int i);
bool        UiPairIsDraft(int i);
bool        UiPairBuiltIn(int i);
bool        UiPairInFlight(int i);
std::string UiPairSource();
std::string UiHueName(float hue);
std::string UiPairKeyFor(const std::string& display, int ignore);   // the name the pair would be stored under
int         UiPairAdd(const std::string& display, float anchor, float offset, bool draft);
bool        UiPairUpdateDraft(int i, const std::string& display, float anchor, float offset);
void        UiPairCancel(int i);                  // a draft: hidden (memory only; it was never written)
std::string UiPairKeep(int i);                    // commit (ratings 5) + persist; the line says what was written
std::string UiPairRemove(int i);                  // hide (+ a keep let go and NEXT if it plays) + persist
std::string UiPairLastWrite();                    // the last Keep / Remove line (for --ui-dump)
float       UiStillRatingDirector(int i, int mode);   // the director's own value (--ui-dump; no headless cache)
void        UiSetAppdataOverride(const std::wstring& dir);
// ---- ui3 Keep this and visits (cycle.h CycleKeepThis / CycleVisit; headless: the same hold state, no jump) --------
void        UiKeep(float sec);       // Keep this for sec s (the stage timer + the colour director's hold); <= 0 lets go
float       UiKeepLeftSec();         // 0 = no keep
// guest = 0 Ink / 1 Fluid: a visit to a look that is not a stage (cycle.h CycleVisitGuest); rated = -1 then
struct UiVisitView { bool on = false; int rated = -1; float leftSec = 0.0f; int guest = -1; };
UiVisitView UiVisitNow();            // a visit to a rated look (k = 0 .. UiRatedCount()-1) or to Ink / Fluid, and its time left
void        UiVisit(int ratedK, float sec);
void        UiVisitEnd();            // "Back to automatic now" / its X (headless: a guest visit is let go at once)
// ---- ui3 visits to Ink / Fluid (kind 0 / 1): the start preset (CycleGuestPreset) and the guest visit ------------
bool        UiGuestAvailable(int kind, std::string* why);   // false + why: the preset file is nowhere (the chip is off)
void        UiVisitGuest(int kind, float sec);              // live: the cycle fades to it; headless: appended + held
int         UiGuestStage();          // the guest stage's 0-based index in the director's list, -1 = none
std::string UiGuestFile();           // its preset file name ("" = none)
int         UiCycleStageCountAll();  // every stage the director holds, a guest included (--ui-dump)

// headless only
void         UiCycleLoadHeadless(const wchar_t* ini);
// "cycle stage N SECONDS [paused] [lerp]" | "cycle off" | "live rig.focus V [moving]" |
// "live rig.tilt V" | "live hue2 V [moving]" | "live hueangle V [moving]" | "live palette V [moving]"
// "freeze palette|hue2|rig|hueshift|transition" | "unfreeze ..."; returns "ok" or an error.
std::string  UiCycleScript(const std::string& cmd, FluidConfig& cfg);
