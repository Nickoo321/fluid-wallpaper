// director.h -- Oil3D L4: the COLOUR DIRECTOR (user 2026-10-30; brief
// reference/briefs/NEXT-oil3d-depth.md, "USER 2026-10-30 ~morning" + DESIGN v1).
// One slow movement between colour combos (reference/configs/combos.ini) with
// the tank's physics as the tools: dyes mixing in and washing out, lamp 1's
// window, and the palette's hue turn, which rests at each combo's own hue.
// [liquid_acid] colour_director=1 turns it on; 0 (the default) returns before
// touching anything: bit-identical to the app without it.
#pragma once
#include "fluid.h"
#include <string>

void  DirectorSetLogger(void (*fn)(const char*));
// The cycle director's seed (CycleSeed(): --cycle-seed, else --seed under
// --shot, else [cycle] seed; 0 = wall clock), on its own splitmix stream.
void  DirectorSeed(unsigned seed);
// --shot-time0 S: the director's CPU state is run forward S seconds the first
// time it engages, so a --shot-time0 series shows the journey at S.
void  DirectorFastForward(float seconds);
// Once per shown frame, right AFTER ClocksTick, in both loops.
void  DirectorTick(FluidRenderer& r, float dt);
bool  DirectorActive();
// NEXT (the "next" shortcut / tray / Settings button while the director holds the colours; user 2026-11-18: "Start
// the next shift now"): the hold of the still on screen ends and the next still is drawn among the OTHER pairs, so
// the user's slow shift starts at once. true = taken (pending or done); false = nothing to skip right now.
bool  DirectorSkip();
// T8 looks (user 2026-11-18: "each is supposed to be rated, and then normalized ... Put it into the UI"): every
// STILL = a combo's white pair (mode 0) or its darkroom pair (mode 1) has a rating 0..10, 5 until rated. With
// [liquid_acid] director_dark_path = 2 the next rest is drawn with chance rating / sum of ratings. The Settings
// window's Looks block sets them; they live in settings.ini [looks] <combo>_white / <combo>_pair (app-level: a
// stage / layout / scheme file without the section changes nothing).
int   DirectorLookCount();                       // the combos in the list (loads it on first use)
std::wstring DirectorLookName(int i);
float DirectorLookRating(int i, int mode);
void  DirectorSetLookRating(int i, int mode, float v);   // clamped 0..10; the caller persists
float DirectorLookRatingSum();                   // every still's rating, summed (the cycle's rated look stages draw against it)
void  DirectorLoadLooks(const wchar_t* ini);     // reads [looks] of this ini (every config load calls it)
// THE QUEUE (user 2026-11-21: "in the ui can i queue stuff?", "ok add that"): looks lined up by the Settings
// window's "show next" buttons; with director_dark_path = 2 the director takes the front of the line instead of
// drawing by rating. Empty = the draw of before. Not saved. mode 0 = the white pair, 1 = the darkroom pair.
void  DirectorQueueLook(int i, int mode);        // to the back of the line; alone + a hold on screen: starts at once
int   DirectorQueueCount();
bool  DirectorQueueAt(int k, int* i, int* mode);
void  DirectorQueueClear();
std::string DirectorNowText();                   // what is on screen, in plain words ("" = the director is off)
// One line for the rating log: phase, combo (-> next), the move, time in the phase.
std::string DirectorStatusLine();
// Snapshots take the stage's own values (as ClocksBaseCopy): every key the
// director drives that still holds its own output gets the value under it.
void  DirectorBaseCopy(FluidConfig& inout);

// ---- read-only views for the Settings window's HOME screen (ui3, 2026-11-23) ----------------------------------
// A pair's two hues: the film is `anchor`, the patches `anchor + offset` (degrees, the combos.ini columns).
float DirectorLookAnchor(int i);
float DirectorLookOffset(int i);
// What the director is doing right now, as the HOME screen reads it. Nothing here changes any state.
struct DirectorView {
    bool  active = false;        // the director runs (a colour pair is on screen)
    int   combo = -1;            // the pair on screen (while changing: the one being left)
    int   mode = 0;              // 0 vivid (CH_WHITE, white light), 1 darkroom (CH_PAIR, two lights)
    bool  changing = false;      // not a hold: TRIP / CLOSE / OPEN (or another step between holds)
    int   toCombo = -1;          // while changing: where the change goes (-1 = not known yet)
    int   toMode = 0;
    int   nextCombo = -1;        // during a hold: the front of the queue (-1 = drawn by rating at the hold's end)
    int   nextMode = 0;
    float holdLeftSec = -1.0f;   // seconds left in the hold on screen (-1 = not a hold)
    float keepLeftSec = 0.0f;    // Keep this: seconds left (0 = none)
};
DirectorView DirectorNow();
// ui3 Keep this (2026-11-23): the hold of the still on screen (WHITE / PAIR) does not end before this many seconds
// on the cycle's hold clock (CycleHoldClock: the wall clock, the wallpaper time under --shot): its timer, a queued
// "show next" and DirectorSkip all wait; the small garnish moves carry on. Pressed during a change it holds the hold
// that follows. sec <= 0 lets go. No keep = every path as before.
void  DirectorKeep(float sec);
float DirectorKeepLeftSec();
// Headless (--ui-combos <ini>): read the pair list from this file instead of the usual search. Only before the list
// has loaded; false = it had loaded already or the file has no [combos] lines (the usual search then runs as before).
bool  DirectorLoadCombosFrom(const wchar_t* ini);

// ---- ui3 add / remove a pair (2026-11-23; UI3-PLAN "Adding and removing a pair") ---------------------------------
// The list only GROWS while running (every index held anywhere stays valid); a removed pair is HIDDEN (ratings 0,
// never drawn, never engaged on, its queue entries dropped) and is gone at the next start (not in combos.ini). None of
// these is called by the app on its own: a list nobody adds to or removes from behaves exactly as before.
// Appends a pair (default scale 0.25, load 0.50, murk_k 1, motes -1) under DirectorComboKeyFor(name); draft = ratings 0
// (only the queue reaches it), else ratings 5. Returns its index.
int   DirectorAddCombo(const std::wstring& name, float anchorHue, float offset, bool draft);
// a draft that is neither on screen nor on its way: new name / hues (false = not allowed: add a new one instead)
bool  DirectorUpdateDraft(int i, const std::wstring& name, float anchorHue, float offset);
void  DirectorCommitCombo(int i);                 // "Keep this pair": not a draft any more, ratings 5 on both faces
void  DirectorHideCombo(int i);                   // ratings 0, hidden, its queue entries dropped (never erased)
bool  DirectorComboHidden(int i);
bool  DirectorComboIsDraft(int i);
bool  DirectorComboInFlight(int i);               // on screen or on its way (the director is travelling to it)
bool  DirectorComboIsBuiltIn(int i);              // one of the six seed names (never removed, only set to 0)
std::wstring DirectorComboSource();               // the file the list came from, or "built in"
// The name rule: lower-case ASCII words joined by "_" ("Blue / Lime" -> blue_lime), not starting with look_, not
// ending in _white / _pair, unique among the pairs that are not removed (a clash gets _2, _3 ...); `ignore` = the
// index being renamed (-1 none).
std::wstring DirectorComboKeyFor(const std::wstring& wanted, int ignore);
const char* DirectorHueName(float hue);           // the nearest colour name, only to SUGGEST a name ("Blue", "Lime")
// Persistence ("Keep this pair" / "Remove this pair"). combosIni = the %APPDATA% combos.ini (that file replaces every
// other list: if the list in use came from elsewhere, the whole list is copied there first, under the source's
// comment header); settingsIni = settings.ini ([looks] <name>_white / _pair). readOnly = nothing is written (the
// returned line says so). Returns what was done, in words.
std::string DirectorPersistKeep(int i, const wchar_t* combosIni, const wchar_t* settingsIni, bool readOnly);
std::string DirectorPersistRemove(int i, const wchar_t* combosIni, const wchar_t* settingsIni, bool readOnly);
