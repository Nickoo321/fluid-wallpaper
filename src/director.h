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
void  DirectorLoadLooks(const wchar_t* ini);     // reads [looks] of this ini (every config load calls it)
// One line for the rating log: phase, combo (-> next), the move, time in the phase.
std::string DirectorStatusLine();
// Snapshots take the stage's own values (as ClocksBaseCopy): every key the
// director drives that still holds its own output gets the value under it.
void  DirectorBaseCopy(FluidConfig& inout);
