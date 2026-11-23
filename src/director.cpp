// director.cpp -- Oil3D L4: the COLOUR DIRECTOR. See director.h and the brief
// (reference/briefs/NEXT-oil3d-depth.md, "USER 2026-10-30 ~morning": "you have
// 10 or whatever preset color combos. Then you use physical mixing + lamp +
// post processing hue shifting all as tools to move ... slow movement
// constantly ... post processing as a temporary thing, that generally returns
// to neutral most of the time, but you could do something odd, once in a while").
//
// COMBOS (reference/configs/combos.ini, else the compiled-in seed, found like
// dyes.ini): name = film anchor deg, patch offset deg, patch scale, patch load,
// exactly what the five two-colour Schemes differ by.
//
// REST at a combo (director_rest s, +-25 %): the film hue held at its anchor
// (NEUTRAL), and one MOVE at a time after a 20-50 s gap:
//   DASH     an analogous dye in the film (dye_a2 +-20..45 deg, 0.15..0.35):
//            the film's hue walks a little and comes back              40 %
//   REVEAL   a notch out of lamp 1's white window over the patches' hue:
//            they read as their other half (lamp1_level compensates; the
//            per-pixel cap keeps every pixel at or under today)          30 %
//   DETOUR   the whole palette turns +-15..40 deg and comes back; one in
//            seven is ODD: +-90 or 180 for a short hold                  25 %
//   DARKROOM a narrow lamp window, three variants in turn: the red/blue
//            SAFELIGHT (everything but the film dark), VIOLET (dim, the
//            patches leaking blue), MONO (all in the film's colour)       5 %
// TRIP to another combo. A look with patches POURS the next combo in (PourStep: the old patches
//   recede, the next film colour rises in from below as patch fluid until it fills the frame, the
//   film swaps underneath, the next patches pour in: "never mixing"). A one-colour look
//   (director_trip s, +-15 %), the user's recipe: the
//   whole-picture hue turn carries it (one smoothstep), the real colours land
//   underneath, the old pair is gone at the end. Optional garnish
//   (director_mix > 0, default 0): the next pair's hues come in as dyes and
//   swap places with the film during the turn (sin-shaped in and out), so the
//   middle is a physical mix (the first sheet: an olive in-between, darker).
// CHAPTERS (director_style 1, the default since 2026-10-30 ~19:30): lights + dyes, the user's "whole architecture";
//   the section above Step(). The rests / moves / trips above are director_style 0 and director_demo.
// Every glide is a smoothstep. The film hue goes through FluidRenderer::
// SetDirectorHue; the rest is written into the live config every frame with
// ADOPT-ON-CHANGE (as the clocks): a value that is not the one written last
// frame came from someone else (a stage apply, a slider) and becomes the
// BASE, which DirectorBaseCopy hands to snapshots and the release returns to.
// Turned off while on: an 8 s glide back to the base and the palette clock's
// own hue, then the hold is let go (another look = at once: it fades through
// black). The Settings window's hold (ClocksHeld) freezes it and lets the
// sliders write.
#include "director.h"
#include "clocks.h"
#include <windows.h>
#include <shlobj.h>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

double CycleHoldClock();   // cycle.cpp (ui3 Keep this): the wall clock, the wallpaper time under --shot

namespace {

// murk_k / motes (post-night T3 ruling): the water the combo brings, read only while director_water is on:
// murk_k x the stage's water_murk (1 = the stage's), motes = the mote count (-1 = the stage's own count)
struct Combo { std::wstring name; float anchor, offset, scale, load, murkK, motes; };
struct ComboSeed { const wchar_t* name; float anchor, offset, scale, load, murkK, motes; };
// = reference/configs/combos.ini: the live cycle's five two-colour Schemes + the
// inverted demo (Magenta Mint with every hue at its opposite: the roles swap).
// The water columns = what the Scheme files carried (Magenta Mint's water_murk 0.5, the
// user's 10-31 "just less"; Violet Amber's and Teal Orange's motes 20).
const ComboSeed kComboSeed[] = {
    { L"magenta_mint", 325.0f,  180.0f, 0.25f, 0.50f, 0.5f, -1.0f },
    { L"magenta_cyan", 325.0f, -140.0f, 0.25f, 0.50f, 1.0f, -1.0f },
    { L"blue_coral",   215.0f,  160.0f, 0.25f, 0.50f, 1.0f, -1.0f },
    { L"violet_amber", 275.0f,  125.0f, 0.40f, 0.25f, 1.0f, 20.0f },
    { L"teal_orange",  190.0f, -160.0f, 0.40f, 0.25f, 1.0f, 20.0f },
    { L"mint_magenta", 145.0f,  180.0f, 0.25f, 0.50f, 0.5f, -1.0f },
};

std::vector<Combo> s_combos;
bool s_loaded = false;
void (*s_log)(const char*) = nullptr;
// ui3 add / remove a pair (2026-11-23): per-combo flags, parallel to s_combos (missing = 0 = an ordinary pair, so a
// list nobody added to or removed from reads exactly as before). DRAFT = added by the Settings window and not kept
// yet (ratings 0: only the queue reaches it); HIDDEN = removed / cancelled this session (ratings 0, never drawn,
// erased at the next start because it is not in combos.ini). Never erased while running: indices stay valid.
enum { CF_DRAFT = 1, CF_HIDDEN = 2 };
std::vector<unsigned char> s_flags;
unsigned ComboFlags(int i) { return (i >= 0 && i < (int)s_flags.size()) ? s_flags[i] : 0u; }
bool Excluded(int i) { return (ComboFlags(i) & (CF_DRAFT | CF_HIDDEN)) != 0; }
std::wstring s_source;                     // the file the list came from ("" = the compiled-in seed)

void Log(const char* fmt, ...) {
    char buf[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (s_log) s_log(buf); else fputs(buf, stdout);
}

void LoadCombos() {
    if (s_loaded) return;
    s_loaded = true;
    wchar_t cand[3][MAX_PATH] = {};
    PWSTR appdata = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) {
        _snwprintf_s(cand[0], _TRUNCATE, L"%s\\FluidWallpaper\\combos.ini", appdata);
        CoTaskMemFree(appdata);
    }
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (wchar_t* sl = wcsrchr(exe, L'\\')) *sl = 0;
    _snwprintf_s(cand[1], _TRUNCATE, L"%s\\combos.ini", exe);
    _snwprintf_s(cand[2], _TRUNCATE, L"%s\\..\\reference\\configs\\combos.ini", exe);
    for (int c = 0; c < 3; c++) {
        if (!cand[c][0] || GetFileAttributesW(cand[c]) == INVALID_FILE_ATTRIBUTES) continue;
        std::vector<wchar_t> buf(32768);
        const DWORD n = GetPrivateProfileSectionW(L"combos", buf.data(), (DWORD)buf.size(), cand[c]);
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
            Combo k{ nm, 0.0f, 180.0f, 0.25f, 0.5f, 1.0f, -1.0f };
            if (nm.empty() || swscanf_s(line.c_str() + eq + 1, L" %f , %f , %f , %f , %f , %f",
                                        &k.anchor, &k.offset, &k.scale, &k.load, &k.murkK, &k.motes) < 2) continue;
            s_combos.push_back(k);
        }
        if (!s_combos.empty()) {
            Log("[director] %d combos from %ls\n", (int)s_combos.size(), cand[c]);
            s_source = cand[c];            // ui3: DirectorComboSource
            return;
        }
    }
    for (const ComboSeed& s : kComboSeed)
        s_combos.push_back({ s.name, s.anchor, s.offset, s.scale, s.load, s.murkK, s.motes });
    Log("[director] no combos.ini found (%%APPDATA%%\\FluidWallpaper, the exe dir, ..\\reference\\configs);"
        " using the compiled-in seed, %d combos\n", (int)s_combos.size());
}

// ---- its own splitmix stream (never the cycle's or the clocks') ------------
unsigned s_seed = 0;
bool     s_seeded = false;
uint64_t s_rng = 0;
void EnsureSeeded() {
    if (s_seeded) return;
    s_seeded = true;
    const uint64_t sd = s_seed ? (uint64_t)s_seed : (uint64_t)GetTickCount64();
    s_rng = sd * 0xD1B54A32D192ED03ull ^ 0x00C0105D1EC7ull;
}
uint64_t Next() {
    uint64_t z = (s_rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
float U01() { return (float)((Next() >> 40) * (1.0 / 16777216.0)); }
float U(float a, float b) { return a + (b - a) * U01(); }

float Wrap360(float d) { d = fmodf(d, 360.0f); return d < 0.0f ? d + 360.0f : d; }
float Wrap180(float d) { return Wrap360(d + 180.0f) - 180.0f; }
float Smooth(float x) { x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); return x * x * (3.0f - 2.0f * x); }
float Lerp(float a, float b, float t) { return a + (b - a) * t; }
uint32_t Bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

// ---- the keys it drives ------------------------------------------------------
enum { K_HUE2, K_SCALE, K_LOAD, K_A2H, K_A2S, K_A2K, K_A2A, K_B1H, K_B1S, K_B1K, K_B1A,
       K_LO, K_HI, K_LO2, K_HI2, K_LEVEL, K_COVER, K_H3, K_H3A, K_H3S, K_N };
bool IsAngle(int k) { return k == K_HUE2 || k == K_A2H || k == K_B1H || k == K_H3; }
float* Field(LiquidAcidConfig& a, int k) {
    switch (k) {
    case K_HUE2:  return &a.filmHue2;
    case K_SCALE: return &a.filmHue2Scale;
    case K_LOAD:  return &a.filmEqualLoadP;
    case K_A2H:   return &a.filmDyeHue[1];
    case K_A2S:   return &a.filmDyeSigma[1];
    case K_A2K:   return &a.filmDyeK[1];
    case K_A2A:   return &a.dyeA2Amount;
    case K_B1H:   return &a.filmDyeHue[2];
    case K_B1S:   return &a.filmDyeSigma[2];
    case K_B1K:   return &a.filmDyeK[2];
    case K_B1A:   return &a.dyeB1Amount;
    case K_LO:    return &a.lamp1Lo;
    case K_HI:    return &a.lamp1Hi;
    case K_LO2:   return &a.lamp1Lo2;
    case K_HI2:   return &a.lamp1Hi2;
    case K_COVER: return &a.filmHue2Cover;
    case K_H3:    return &a.filmHue3;            // T8 trip: the third hue = the incoming dye
    case K_H3A:   return &a.filmHue3Amt;
    case K_H3S:   return &a.filmHue3Share;
    default:      return &a.lamp1Level;
    }
}
float s_base[K_N] = {}, s_last[K_N] = {};
bool  s_have = false;          // s_last holds what it wrote (it owns the keys)

// What the director shows, in ABSOLUTE degrees (Write makes them relative).
struct Out {
    float hue = 0.0f, patch = 0.0f;          // the film's hue, the patches' base hue
    float scale = 0.25f, load = 0.5f;        // film_hue2_scale, film_equal_load_patches
    float a2 = 0.0f, a2Amt = 0.0f, a2Sig = 55.0f;   // dye A2 (in the film)
    float b1 = 0.0f, b1Amt = 0.0f;           // dye B1 (in the patches), the 'self' band
    float b1K = -1.0f;                       // its strength (-1 = the self dye's): the chapters' dense dye
    float notchC = 0.0f, notchW = 0.0f;      // lamp 1: a notch out of white (centre, width)
    float darkC = 0.0f, darkW = 360.0f;      // lamp 1: a narrow window (centre, width; 360 = white)
    float light2C = 0.0f, light2W = 0.0f;    // lamp 1's second range = the chapters' second light (0 = off)
    float level = 1.0f;                      // lamp1_level, the compensation
    bool  coverOn = false;                   // film_hue2_cover: the pour's bias (off = the stage's own)
    float cover = 0.0f;
    float shape = 0.0f;                      // T8 connections: the link's blobs (+ patch end, - film end; 0 = off)
    float shLo = 0.0f, shHi = 0.0f, hold = 0.0f;  // T8 shift trip: SetHue2Shapes' film-end / patch-end blobs, the hold
    float h3 = 0.0f, h3Amt = -1.0f, h3Share = 0.0f;   // T8 trip: film_hue3 (rel. deg), its amount, share (-1 = the stage's)
    float turn = 0.0f;                     // director_turn: the whole picture turned by this (deg; 0 = today)
    float murkK = 1.0f, motes = 0.0f;        // director_water: the combo's murk level and mote count (resolved)
};

enum Phase { P_OFF, P_ENTER, P_REST, P_TRIP, P_RELEASE, P_CHAP };
enum Move  { M_NONE, M_DASH, M_REVEAL, M_DETOUR, M_DARK };
const char* const kMoveName[] = { "none", "DASH", "REVEAL", "DETOUR", "DARKROOM" };
Phase s_phase = P_OFF;
float s_clock = 0.0f;          // the director's own time (s), for the log
float s_t = 0.0f, s_len = 0.0f;
int   s_cur = 0, s_next = -1, s_prev = -1;
Move  s_move = M_NONE;
float s_mt = 0.0f, s_mIn = 1.0f, s_mHold = 0.0f, s_mOut = 1.0f, s_mA = 0.0f, s_mB = 0.0f;
float s_gap = 0.0f;
int   s_darkTurn = 0;          // which darkroom variant comes next (0 safelight, 1 violet, 2 mono)
Out   s_out, s_from;
float s_ff = 0.0f;             // --shot-time0: run forward this much at the first engage
float s_relT = 0.0f, s_relHue = 0.0f, s_relFrom[K_N] = {};
enum PourStep { PO_RECEDE, PO_POUR, PO_SWAP, PO_DRAIN, PO_RETURN };
const char* const kPourName[] = { "RECEDE", "POUR", "SWAP", "DRAIN", "RETURN" };
bool  s_pourTrip = false;      // this trip pours (the look has patches), else the hue turn
float s_pourRampNow = 0.0f;    // T8 director_pour_ramp (s), set by Step: PourStep's POUR enters as growing shapes
int   s_pour = PO_RECEDE;
float s_pt = 0.0f;             // time in the pour step
float s_stageMotes = 0.0f;     // director_water: the stage's own mote count (a combo's motes -1 = this)
float s_relK = 1.0f, s_relMotes = 0.0f;   // director_water: where the release glides back from

Out RestOut(const Combo& c, float selfSig) {
    Out o;
    o.hue = Wrap360(c.anchor);
    o.patch = Wrap360(c.anchor + c.offset);
    o.scale = c.scale;
    o.load = c.load;
    o.a2 = o.hue;
    o.b1 = o.patch;
    o.a2Sig = selfSig;
    o.murkK = c.murkK;
    o.motes = c.motes >= 0.0f ? c.motes : s_stageMotes;
    return o;
}
// director_water: the water of A glided toward B's by v (the same smoothstep as the light it rides with)
void WaterGlide(Out& o, const Out& A, const Out& B, float v) {
    o.murkK = Lerp(A.murkK, B.murkK, v);
    o.motes = Lerp(A.motes, B.motes, v);
}
float RestLen(const LiquidAcidConfig& a) { return fmaxf(a.directorRest, 10.0f) * U(0.75f, 1.25f); }
// director_hold (the cadence): the base of a chapter hold, today's (R-derived) while the key is 0
float HoldBase(const LiquidAcidConfig& a, float todayBase) { return a.directorHold > 0.5f ? a.directorHold : todayBase; }

float MoveEnv() {
    if (s_mt < s_mIn) return Smooth(s_mt / s_mIn);
    if (s_mt < s_mIn + s_mHold) return 1.0f;
    return Smooth(1.0f - (s_mt - s_mIn - s_mHold) / s_mOut);
}
bool MoveDone() { return s_mt >= s_mIn + s_mHold + s_mOut; }

void StartMove(bool darkDemo) {
    const float r = darkDemo ? 1.0f : U01();      // director_demo 1: only the darkroom
    s_mt = 0.0f;
    if (r < 0.40f) {
        s_move = M_DASH;
        s_mA = (U01() < 0.5f ? -1.0f : 1.0f) * U(20.0f, 45.0f);   // offset from the film, deg
        s_mB = U(0.15f, 0.35f);                                  // amount
        s_mIn = U(15.0f, 25.0f); s_mHold = U(20.0f, 45.0f); s_mOut = U(15.0f, 25.0f);
    } else if (r < 0.70f) {
        s_move = M_REVEAL;
        s_mA = U(-30.0f, 30.0f);                                 // notch centre off the patch hue
        s_mB = U(60.0f, 110.0f);                                 // notch width
        s_mIn = U(20.0f, 30.0f); s_mHold = U(20.0f, 40.0f); s_mOut = U(20.0f, 30.0f);
    } else if (r < 0.95f) {
        s_move = M_DETOUR;
        if (U01() < 1.0f / 7.0f) {                               // the ODD one
            s_mA = U01() < 0.5f ? 180.0f : (U01() < 0.5f ? -90.0f : 90.0f);
            s_mIn = U(25.0f, 40.0f); s_mHold = U(8.0f, 15.0f); s_mOut = U(25.0f, 40.0f);
        } else {
            s_mA = (U01() < 0.5f ? -1.0f : 1.0f) * U(15.0f, 40.0f);
            s_mIn = U(30.0f, 50.0f); s_mHold = U(20.0f, 40.0f); s_mOut = U(30.0f, 50.0f);
        }
    } else {
        s_move = M_DARK;
        s_mA = (float)(s_darkTurn++ % 3);                        // the variant, in turn (no draw)
        s_mB = U(40.0f, 55.0f);                                  // window width
        s_mIn = U(15.0f, 25.0f); s_mHold = U(15.0f, 25.0f); s_mOut = U(15.0f, 25.0f);
    }
    s_mB = (s_move == M_DETOUR) ? 0.0f : s_mB;
    Log("[director] t=%.1f %ls: %s a=%+.1f b=%.2f in %.0f hold %.0f out %.0f s\n", s_clock,
        s_combos[s_cur].name.c_str(), kMoveName[s_move], s_mA, s_mB, s_mIn, s_mHold, s_mOut);
}

// The SAFELIGHT end for a film over its patches: windows live on the red..blue line with two-ended purples, so a
// purple film (240..360) gets the pure end farther from the patches (magenta over mint: red, the classic
// safelight); any other film a window on its own hue. The darkroom move and the chapters' light 1.
float SafelightEnd(float film, float patch) {
    const float fh = Wrap360(film), ph = Wrap360(patch);
    if (fh <= 240.0f) return fh;
    const float lp = ph > 240.0f ? (ph > 300.0f ? ph - 360.0f : 240.0f) : ph;
    return (fabsf(lp) >= fabsf(240.0f - lp)) ? 0.0f : 240.0f;
}

void ApplyMove(Out& o, float e) {
    switch (s_move) {
    case M_DASH:   o.a2 = Wrap360(o.hue + s_mA); o.a2Amt = s_mB * e; o.a2Sig = 45.0f; break;
    case M_REVEAL: o.notchC = Wrap360(o.patch + s_mA); o.notchW = s_mB * e; o.level = 1.0f + 1.5f * e; break;
    case M_DETOUR: o.hue = Wrap360(o.hue + s_mA * e); o.patch = Wrap360(o.patch + s_mA * e); break;
    case M_DARK: {
        // THREE darkroom variants, taken in turn (the user kept all three, "an
        // extra option that it goes to once in a while"):
        // 0 SAFELIGHT: ONE end of the spectrum. Windows live on the red..blue line
        //   with two-ended purples, so a window on a purple film's own hue passes
        //   both ends and the patches' share leaks. A purple film (240..360) gets
        //   the pure end farther from the patches (magenta over mint: red, the
        //   classic safelight); any other film a window on its own hue.
        // 1 VIOLET: 30 deg below the film's hue (the second sheet's "glitch": over
        //   a purple film a dim, blue-violet scene, the patches leaking blue).
        // 2 MONO: a window on the film's own hue, a stronger compensation (the
        //   first sheet: the whole frame in the film's colour).
        // The compensation lifts the film back toward today, capped.
        const float fh = Wrap360(o.hue);
        float dc = fh, lv = 1.5f;
        if (s_mA < 0.5f) {
            dc = SafelightEnd(o.hue, o.patch);
        } else if (s_mA < 1.5f) {
            dc = Wrap360(fh - 30.0f);
        } else {
            lv = 2.0f;
        }
        o.darkC = dc;
        o.darkW = 360.0f + (s_mB - 360.0f) * e;
        o.level = 1.0f + lv * e;
        break;
    }
    default: break;
    }
}

Out TripOut(const Combo& ci, const Combo& cj, float u, float mx, float selfSig) {
    // The user's recipe (2026-10-30, the mid-trip A/B: "probably c"): the whole-
    // picture hue turn CARRIES the trip, one smoothstep over its whole length, and
    // the real colours land underneath (anchor + patch offset + scale + load), so at
    // the end nothing is faked and the old pair is gone. The dyes are a garnish
    // (director_mix, default 0): the next pair's hues come in as dyes A2 / B1 and
    // swap places with the film while the turn runs, rising and falling with
    // sin(pi u), so the product holds a mixed colour in the middle.
    const Out A = RestOut(ci, selfSig), B = RestOut(cj, selfSig);
    const float dF = Wrap180(B.hue - A.hue), dP = Wrap180(B.patch - A.patch);
    const float uu = fminf(fmaxf(u, 0.0f), 1.0f);
    const float v = Smooth(uu), m = mx * sinf(3.14159265f * uu);
    Out o = A;
    o.hue = Wrap360(A.hue + dF * v);     o.a2 = Wrap360(B.hue - dF * v);
    o.patch = Wrap360(A.patch + dP * v); o.b1 = Wrap360(B.patch - dP * v);
    o.scale = Lerp(A.scale, B.scale, v); o.load = Lerp(A.load, B.load, v);
    o.a2Amt = m; o.b1Amt = m;
    WaterGlide(o, A, B, v);                  // director_water: with the turn
    return o;
}

void StartTrip(const LiquidAcidConfig& a) {
    const int n = (int)s_combos.size();
    s_t = 0.0f;
    if (n < 2) { s_len = RestLen(a); return; }   // one combo: it rests there
    int j = s_cur;
    for (int tries = 0; tries < 64; tries++) {
        j = (int)(U01() * (float)n) % n;
        if (j != s_cur && (j != s_prev || n < 3)) break;
    }
    if (j == s_cur) j = (s_cur + 1) % n;
    s_next = j;
    s_phase = P_TRIP;
    s_len = fmaxf(a.directorTrip, 10.0f) * U(0.85f, 1.15f);
    s_pourTrip = a.filmHue2Amt > 0.5f;       // a look with patches pours; a one-colour look turns
    s_pour = PO_RECEDE;
    s_pt = 0.0f;
    Log("[director] t=%.1f TRIP %ls -> %ls, %s (mix %.2f)\n", s_clock, s_combos[s_cur].name.c_str(),
        s_combos[j].name.c_str(), s_pourTrip ? "POUR" : "hue turn", fminf(fmaxf(a.directorMix, 0.0f), 1.0f));
}

// One step of a POUR trip (user 2026-10-30: "you just pour in the next fluid, never mixing", the way the film
// and the patches meet today). The hue2 patch field is made BELOW the bottom edge and only rises
// (StepHueField: a visible cell is pure advection), so film_hue2_cover changes what pours in from below while
// what is on screen rises out the top; every change of colour happens where nothing shows it:
//   RECEDE  cover -0.6: no new patch material; wait until no patch is on screen (field max < 0.55)
//   POUR    the patch colour becomes the NEXT film colour (nothing on screen carries it yet), the next patch
//           scale + load, cover +0.8: the next fluid rises in until it is the whole frame (field min > 0.7)
//   SWAP    under the full cover the film glides to the next film hue over 4 s (hidden; the patches keep
//           their colour, the offset compensated), so the frame does not change
//   DRAIN   cover -0.6: the patches, now the film's own colour, rise out and reveal the same colour
//   RETURN  the next combo's patch colour (off screen again), the stage's own cover: its patches pour in
// Each wait has a floor (8 s) and a ceiling (240 s); without the field (the --shot-time0 fast-forward) a
// nominal 75 s.
bool PourStep(float dt, const Combo& ci, const Combo& cj, float selfSig, const FluidRenderer* r, Out& o) {
    s_pt += dt;
    const Out A = RestOut(ci, selfSig), B = RestOut(cj, selfSig);
    float mn = 0.0f, mx = 1.0f;
    const bool have = r && r->Hue2FieldRange(mn, mx);
    auto waited = [&](bool cond) { return s_pt > 240.0f || (s_pt > 8.0f && (have ? cond : s_pt > 75.0f)); };
    auto next = [&](int p) {
        Log("[director] t=%.1f POUR %ls -> %ls: %s after %.0f s (field %.2f..%.2f)\n", s_clock, ci.name.c_str(),
            cj.name.c_str(), kPourName[p], s_pt, mn, mx);
        s_pour = p; s_pt = 0.0f;
    };
    o = A;
    o.coverOn = true;
    switch (s_pour) {
    case PO_RECEDE:
        o.cover = -0.6f;
        if (waited(mx < 0.55f)) next(PO_POUR);
        break;
    case PO_POUR:
        o.patch = B.hue; o.scale = B.scale; o.load = B.load; o.cover = 0.8f;
        if (s_pourRampNow > 0.0f) o.cover = Lerp(s_base[K_COVER], 0.8f, Smooth(s_pt / s_pourRampNow));   // T8 ramp
        if (waited(mn > 0.7f)) next(PO_SWAP);
        break;
    case PO_SWAP: {
        const float v = Smooth(s_pt / 4.0f);
        o.hue = Wrap360(A.hue + Wrap180(B.hue - A.hue) * v);
        o.patch = B.hue; o.scale = B.scale; o.load = B.load; o.cover = 0.8f;
        WaterGlide(o, A, B, v);              // director_water: with the hidden film swap
        if (s_pt >= 4.0f) next(PO_DRAIN);
        break;
    }
    case PO_DRAIN:
        o.hue = B.hue; o.patch = B.hue; o.scale = B.scale; o.load = B.load; o.cover = -0.6f;
        o.murkK = B.murkK; o.motes = B.motes;
        if (waited(mx < 0.55f)) next(PO_RETURN);
        break;
    default:                                 // PO_RETURN
        o = B;
        if (s_pt >= 45.0f) return true;
        break;
    }
    o.a2 = o.hue;
    o.b1 = o.patch;
    return false;
}

// ---- the LINK (director_dark_path 2's colour change; T8 connections, user 2026-11-18: "Figure out how to make every
// connection good ... litteraly think of every movement in between the 2 primary frames") ------------------------
// The pour above empties the tank to one colour twice (RECEDE, DRAIN: ~2.5 of its 5 minutes one flat colour) and
// fills it as a full-width slab ("Why not put it in the way all the other bubbles are?"). The tank has two fluids
// on one field (the film below the seam, the patches above it), and a fluid's colour may only change while none of
// it is on screen. So the link swaps ROLES instead of emptying: one fluid grows until it holds the whole frame (the
// cover rises slowly, so it arrives as the patch shapes the fluid carries in, bigger and bigger), the hidden fluid
// takes its new colour, then the cover turns round and the new colour comes back in as shapes among the old one;
// the second fluid swaps the same way. Two colours are on screen through the whole change except the two turns.
//   OUT    the cover glides toward one end (FILM FIRST: up, the patches take the frame; PATCH FIRST: down)
//   SWAP1  hidden: the fluid that is off screen takes its new colour (6 s)
//   BACK   the cover glides to the other end: the new colour rises in as shapes among the old fluid
//   SWAP2  hidden: the other fluid takes its new colour
//   HOME   the cover glides back to the stage's own: the landed pair, its shapes coming up from below
// The order is the one whose in-between pair (film first: the new film with the old patches; patch first: the old
// film with the new patches) is the farther apart in hue, so the middle never reads as one colour. Two pairs whose
// colours are both within kLkNear deg of each other (blue coral / teal orange, magenta mint / magenta cyan) GLIDE
// instead: the whole-picture hue turn's own move, nothing pours. Waits: 20 s floor, 300 s ceiling, a nominal 120 s
// without the field (the --shot-time0 fast-forward).
enum LinkStep { LK_GLIDE, LK_OUT, LK_SWAP1, LK_BACK, LK_SWAP2, LK_HOME };
const char* const kLinkName[] = { "GLIDE", "OUT", "SWAP1", "BACK", "SWAP2", "HOME" };
const float kLkNear = 45.0f;                      // both colours this close: a glide
// the blob mask's ramps (linear: no slow start, the first blobs show soon after a turn), seconds
const float kLkFull = 0.66f, kLkNone = 0.57f;     // the field past the seam band (0.62 +- 0.02 at film_hue2_seam 0.33)
const float kLkOut = 60.0f, kLkBack = 100.0f, kLkHome = 80.0f, kLkSwap = 6.0f;
// DARK (a darkroom pair to another combo's darkroom pair): the same steps with both lights kept narrow, so the
// darkroom never opens to white on the way. Each light follows its fluid: it closes and reopens on the new colour
// during that fluid's hidden swap (light 1 on the film's safelight end, light 2 on the patch dye).
const float kLkW1 = 50.0f, kLkW2 = 55.0f, kLkLv = 1.3f;   // = the chapters' kChW1, kChW2, kChLvPair
int   s_lk = LK_OUT;
float s_lkT = 0.0f, s_lkLen = 0.0f;
bool  s_lkFilmFirst = true, s_lkDark = false;
float SafelightEnd(float film, float patch);
void StartLink(const Combo& ci, const Combo& cj, float selfSig, float moveS, bool dark) {
    s_lkDark = dark;
    const Out A = RestOut(ci, selfSig), B = RestOut(cj, selfSig);
    const float dF = fabsf(Wrap180(B.hue - A.hue)), dP = fabsf(Wrap180(B.patch - A.patch));
    s_lkT = 0.0f;
    if (fmaxf(dF, dP) <= kLkNear) {
        s_lk = LK_GLIDE;
        s_lkLen = fmaxf(moveS, 60.0f) * fminf(fmaxf(fmaxf(dF, dP) / 30.0f, 1.0f), 1.5f);
    } else {
        s_lk = LK_OUT;
        s_lkFilmFirst = fabsf(Wrap180(B.hue - A.patch)) >= fabsf(Wrap180(A.hue - B.patch));
        s_lkLen = kLkOut;
    }
    Log("[director] t=%.1f LINK %ls -> %ls: %s%s (film %.0f -> %.0f, patches %.0f -> %.0f)\n", s_clock, ci.name.c_str(),
        cj.name.c_str(), s_lk == LK_GLIDE ? "GLIDE" : (s_lkFilmFirst ? "FILM FIRST" : "PATCH FIRST"),
        dark ? " in the darkroom" : "", A.hue, B.hue, A.patch, B.patch);
}
// the darkroom link's lights: light 1 at c1 (width x w1), light 2 at c2 (width x w2)
void LinkLights(Out& o, float c1, float w1, float c2, float w2) {
    o.darkC = c1; o.darkW = fmaxf(kLkW1 * w1, 1.0f); o.light2C = c2; o.light2W = kLkW2 * w2; o.level = kLkLv;
}
// one step; true = landed (o = B's rest, the cover back at the stage's own)
bool LinkStep(float dt, const Combo& ci, const Combo& cj, float selfSig, const FluidRenderer* r, Out& o) {
    s_lkT += dt;
    const Out A = RestOut(ci, selfSig), B = RestOut(cj, selfSig);
    float mn = 0.0f, mx = 1.0f;
    const bool have = r && r->Hue2FieldRange(mn, mx);
    const float base = s_base[K_COVER];
    const float s1 = s_lkFilmFirst ? 1.0f : -1.0f;  // OUT's fluid: + the patches, - the film (SetHue2Shapes)
    auto full1 = [&]() { return s_lkFilmFirst ? mn > kLkFull : mx < kLkNone; };   // OUT's fluid holds the frame
    auto full2 = [&]() { return s_lkFilmFirst ? mx < kLkNone : mn > kLkFull; };   // BACK's
    auto waited = [&](bool cond) { return s_lkT > 300.0f || (s_lkT > 20.0f && (have ? cond : s_lkT > 120.0f)); };
    auto next = [&](int p, float len) {
        Log("[director] t=%.1f LINK %ls -> %ls: %s after %.0f s (field %.2f..%.2f)\n", s_clock, ci.name.c_str(),
            cj.name.c_str(), kLinkName[p], s_lkT, mn, mx);
        s_lk = p; s_lkT = 0.0f; s_lkLen = len;
    };
    auto hue = [](float f, float t, float e) { return Wrap360(f + Wrap180(t - f) * e); };
    const float e = Smooth(s_lkT / fmaxf(s_lkLen, 1.0f));
    o = A;
    o.coverOn = true; o.cover = base;              // the refresh pours wait; the bias stays the stage's own
    const float L1A = SafelightEnd(A.hue, A.patch), L1B = SafelightEnd(B.hue, B.patch);
    const float shut = fabsf(1.0f - 2.0f * fminf(s_lkT / fmaxf(s_lkLen, 1.0f), 1.0f));   // a swap's light: 1 -> 0 -> 1
    const bool  late = s_lkT >= 0.5f * s_lkLen;                                        // ... on its new colour
    switch (s_lk) {
    case LK_GLIDE: {
        const float u = fminf(s_lkT / fmaxf(s_lkLen, 1.0f), 1.0f);
        o = TripOut(ci, cj, u, 0.0f, selfSig);
        if (s_lkDark) LinkLights(o, hue(L1A, L1B, Smooth(u)), 1.0f, o.patch, 1.0f);
        if (s_lkT >= s_lkLen) { o = B; if (s_lkDark) LinkLights(o, L1B, 1.0f, B.patch, 1.0f); return true; }
        return false;
    }
    case LK_OUT:
        o.shape = s1 * fminf(s_lkT / kLkOut, 1.0f);           // the blobs of the OUT fluid grow
        if (waited(full1())) next(LK_SWAP1, kLkSwap);
        break;
    case LK_SWAP1:
        o.shape = s1;
        if (s_lkFilmFirst) o.hue = hue(A.hue, B.hue, e); else o.patch = hue(A.patch, B.patch, e);
        if (s_lkT >= s_lkLen) next(LK_BACK, kLkBack);
        break;
    case LK_BACK:
        o.shape = s1 * fmaxf(1.0f - 2.0f * s_lkT / kLkBack, -1.0f);   // through 0: the other fluid
        if (s_lkFilmFirst) o.hue = B.hue; else o.patch = B.patch;
        WaterGlide(o, A, B, e);
        if (waited(full2())) next(LK_SWAP2, kLkSwap);
        break;
    case LK_SWAP2:
        o = B; o.coverOn = true; o.cover = base; o.shape = -s1;
        if (s_lkFilmFirst) o.patch = hue(A.patch, B.patch, e); else o.hue = hue(A.hue, B.hue, e);
        if (s_lkT >= s_lkLen) next(LK_HOME, kLkHome);
        break;
    default:                                       // LK_HOME
        o = B; o.coverOn = true; o.cover = base;
        o.shape = -s1 * fmaxf(1.0f - s_lkT / kLkHome, 0.0f);  // the blobs shrink to the stage's own field
        if (s_lkT >= s_lkLen) { o = B; if (s_lkDark) LinkLights(o, L1B, 1.0f, B.patch, 1.0f); return true; }
        break;
    }
    if (s_lkDark) {                                // the darkroom link: each light follows its fluid
        const bool f1 = s_lkFilmFirst;
        switch (s_lk) {
        case LK_OUT:   LinkLights(o, L1A, 1.0f, A.patch, 1.0f); break;
        case LK_SWAP1: if (f1) LinkLights(o, late ? L1B : L1A, shut, A.patch, 1.0f);
                       else    LinkLights(o, L1A, 1.0f, late ? B.patch : A.patch, shut); break;
        case LK_BACK:  if (f1) LinkLights(o, L1B, 1.0f, A.patch, 1.0f); else LinkLights(o, L1A, 1.0f, B.patch, 1.0f); break;
        case LK_SWAP2: if (f1) LinkLights(o, L1B, 1.0f, late ? B.patch : A.patch, shut);
                       else    LinkLights(o, late ? L1B : L1A, shut, B.patch, 1.0f); break;
        default:       LinkLights(o, L1B, 1.0f, B.patch, 1.0f); break;
        }
    }
    o.a2 = o.hue;
    o.b1 = o.patch;
    return false;
}

// ---- CHAPTERS (director_style 1; user 2026-10-30 ~19:15, the director's real job) -----------------------------
// "murk, with the darkroom effect, going thru the colors ... the entire thing I said about dyes in fluids ... a whole
// architecture". The user's chapter: "it's red. You add blue dye under red light, it looks cool cause it's like red
// and some black and white, then more blue comes, oh now it's like almost completely black and white, with red
// accents ... then you can turn on the blue light as well, and now its like primary and accent". And "that's not to
// the exclusion of these cool color combos and rectangular shapes": the vivid pair under white light (the Scheme as
// the cycle shows it) is where a chapter often ARRIVES. LIGHTS = lamp 1's two ranges (the spectral window model: a
// dye the light does not carry reads dark, nothing painted); DYES = the patch fluid, poured in from below through
// film_hue2_cover (nothing laid inside the frame, never blended). One chapter per combo:
//   WHITE   the vivid pair under white light, the DASH garnish now and then           1.0 x rest
//   DIM     the light narrows to LIGHT 1 alone (SafelightEnd: the film passes it, the patches do not), darker
//   RECEDE  no new patch fluid until none is on screen
//   FILL    the NEXT film dye pours in from below until it fills the tank (under the old light it may read
//           dark: the tank darkens as it fills)
//   SWAP    under the full cover the film becomes that dye (hidden) and light 1 glides to the next combo's
//           safelight end: the tank relights in the new colour
//   DRAIN   the cover recedes; the patch fluid, now the film's own dye, rises out
//   MONO    one light, one dye: the dark mono tank (murk in it)                     0.5 x rest
//   DYE     the chapter's patch dye (DENSE, kChDyeK, from here to PAIR) pours in under light 1, reads near-black
//   MORE    more of it until about half the frame ("almost completely black and white, with red accents")
//                                                                                    >= 0.35 x rest
//   LIGHT2  the second range opens on the patch dye: primary + accent
//   PAIR    held: the darkroom pair, the DASH garnish                                0.6 x rest
//   then 60 %: OPEN (both ranges widen to white) -> WHITE; else straight to DIM.
// Glides scale with rest / 180 (6 s floor); the pour waits are PourStep's (8 s floor, 240 s ceiling, a nominal
// 75 s in the fast-forward). The refresh pours (film_hue2_refresh) run whenever the cover is left alone.
// CADENCE (night-cadence, user 2026-11-02: "make all the transitions like 80% of the time ... like 45 seconds on a
// hold and 2 minutes of slow turning ... slow but moving"). Three keys, 0 = today bit for bit:
//   director_hold s  the base of every hold (WHITE, MONO, PAIR; today R x 1 / 0.5 / 0.6), same jitter draws; the
//                    WHITE / PAIR garnish then starts when the hold ends: hold -> the DASH walks out -> a short
//                    pause (its hold x director_hold / 45) -> walks back -> the next step
//   director_move s  every glide that long: DIM, SWAP (the film hue glides with the light instead of 4 s), LIGHT2,
//                    OPEN; the DASH's walk out and back each half of it (same draws, rescaled)
//   director_turn deg the TURN: the WHOLE picture (film, patches, dyes, lights together: the palette rotation's own
//                    move) turns on the same rhythm through every step, the darkroom stretch too (the first
//                    measure: ~8 min of one red tank between DIM and LIGHT2): hold (director_hold, else 45 s;
//                    x0.8..1.2), turn out over director_move (else 120 s; x0.85..1.15) to +-(0.5..1) x this, hold,
//                    turn back to 0, ... the sides alternate; it returns to neutral every other hold (the user's
//                    "returns to neutral most of the time"). Its clock waits while a chapter glide (DIM, SWAP,
//                    LIGHT2, OPEN) moves the colour, so the two never stack. Its own random stream.
// The chapter's draws are the same in number and order either way, so a seed walks the same combos.
enum ChapStep { CH_WHITE, CH_DIM, CH_RECEDE, CH_FILL, CH_SWAP, CH_DRAIN, CH_MONO, CH_DYE, CH_MORE, CH_LIGHT2,
                CH_PAIR, CH_OPEN, CH_TRIP, CH_CLOSE };
const char* const kChapName[] = { "WHITE", "DIM", "RECEDE", "FILL", "SWAP", "DRAIN", "MONO", "DYE", "MORE",
                                  "LIGHT2", "PAIR", "OPEN", "TRIP", "CLOSE" };
// T8 order (director_dark_share < 1; user 2026-11-17: "The mono color thing, can't be the 2nd thing I got after the
// first render 2 times in a row ... Maybe get rid of a mono cycle and do a mix it into everything else?"): at the
// end of a WHITE hold the chapter goes into the darkroom (DIM ...) with this probability, else it TRIPS under white
// light to the next combo by the pour (PourStep: recede, the next film rises in, the hidden swap, drain) and holds
// that combo's WHITE. Never the darkroom as the first move after the director engages. The draw is its own stream.
bool     s_firstWhite = false;
uint64_t s_dsRng = 0;
bool     s_dsSeeded = false;
float DsU() {
    if (!s_dsSeeded) { s_dsSeeded = true; s_dsRng = (s_seed ? (uint64_t)s_seed : (uint64_t)GetTickCount64()) * 0x9FB21C651E98DF25ull ^ 0xDA4Cull; }
    uint64_t z = (s_dsRng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return (float)((z >> 40) * (1.0 / 16777216.0));
}
// T8 looks (director_dark_path 2; user 2026-11-18 on the stills: "keep the middle and left column", then "each is
// supposed to be rated, and then normalized ... Put it into the UI ... 5/10 for now for everything"): every STILL = a
// combo's white pair or its darkroom pair has a rating 0..10 (5 until rated; the Settings window's Looks block,
// settings.ini [looks] <combo>_white / <combo>_pair). After each rest hold the next still is drawn from all the
// others with chance rating / sum of ratings (every candidate 0: equal). The first move after the director engages
// draws only among OTHER combos' stills (always a colour change first). The paths, none of them a rest: the same
// combo = CLOSE or OPEN; another combo = the white trip (from a PAIR: OPEN first; to a PAIR: CLOSE on landing).
// The draws use the T8 order stream (DsU), never the chapter's.
std::vector<float> s_rate[2];                      // [0] white pair, [1] darkroom pair; per combo
std::vector<std::wstring> s_looksIni;              // inis whose [looks] wait for the combo list (DirectorLoadLooks)
int   s_plCombo = -1, s_plMode = 0;                // the still the chapter is travelling to; -1 = none
float LookRate(int i, int m) {
    return (i >= 0 && i < (int)s_rate[m].size()) ? fminf(fmaxf(s_rate[m][i], 0.0f), 10.0f) : 5.0f;
}
void ApplyLooksIni(const wchar_t* ini) {           // a key that is not in the file keeps its value
    for (int i = 0; i < (int)s_combos.size(); i++) for (int m = 0; m < 2; m++) {
        if (Excluded(i)) continue;                 // ui3: a draft / removed pair's ratings are the Settings window's
        wchar_t key[160], buf[32] = {};
        _snwprintf_s(key, _TRUNCATE, L"%s_%s", s_combos[i].name.c_str(), m ? L"pair" : L"white");
        if (GetPrivateProfileStringW(L"looks", key, L"", buf, 32, ini) > 0)
            s_rate[m][i] = fminf(fmaxf((float)_wtof(buf), 0.0f), 10.0f);
    }
}
void EnsureRates() {                               // the combo list (loads on first use) + every waiting [looks]
    LoadCombos();
    // ui3: a list that GREW (DirectorAddCombo) keeps every rating it had, the new pair gets 5; the first load (from
    // empty) and any other size change are as before
    for (int m = 0; m < 2; m++) {
        if (s_rate[m].size() < s_combos.size()) s_rate[m].resize(s_combos.size(), 5.0f);
        else if (s_rate[m].size() != s_combos.size()) s_rate[m].assign(s_combos.size(), 5.0f);
    }
    for (const std::wstring& p : s_looksIni) ApplyLooksIni(p.c_str());
    s_looksIni.clear();
}
// THE QUEUE (user 2026-11-21: "in the ui can i queue stuff?", "ok add that"): the looks the user lined up with the
// Settings window's "show next" buttons, in order. While it holds anything PoolDraw takes its front instead of
// drawing by rating (an entry for the still that is already on screen is dropped); a queued pick uses no random
// draw, and an EMPTY queue is the code of before, draw for draw. Not saved anywhere: a queue is for now.
std::vector<std::pair<int, int>> s_queue;          // (combo, mode 0 white / 1 darkroom pair)
// the still in flight / on screen was the user's queued pick: a look queued now waits for that hold's full end
// (its own hold is not cut short); else a look queued into an empty line ends the hold at once (NEXT)
bool  s_qTaken = false;
bool  s_skipByQueue = false;                       // the pending NEXT came from the queue (a clear withdraws it)
void PoolDraw(int curMode, bool otherComboOnly) {
    EnsureRates();
    const int n = (int)s_combos.size();
    s_qTaken = false;
    while (!s_queue.empty()) {
        const int qi = s_queue.front().first, qm = s_queue.front().second;
        s_queue.erase(s_queue.begin());
        if (qi < 0 || qi >= n || (qi == s_cur && qm == curMode)) continue;
        s_plCombo = qi; s_plMode = qm; s_qTaken = true;
        Log("[director] QUEUE: the user's pick %ls %s (%d more in the queue)\n", s_combos[qi].name.c_str(),
            qm ? "pair" : "white", (int)s_queue.size());
        return;
    }
    auto cand = [&](int i, int m) { return Excluded(i) ? false : i != s_cur ? true : (!otherComboOnly && m != curMode); };
    float sum = 0.0f; int cnt = 0;
    for (int i = 0; i < n; i++) for (int m = 0; m < 2; m++) if (cand(i, m)) { sum += LookRate(i, m); cnt++; }
    s_plCombo = s_cur; s_plMode = 1 - curMode;     // no candidate at all (one combo, first move): its other mode
    if (cnt == 0) return;
    const float u = DsU();
    float acc = 0.0f;
    for (int i = 0; i < n; i++) for (int m = 0; m < 2; m++) {
        if (!cand(i, m)) continue;
        const float w = sum > 0.0f ? LookRate(i, m) / sum : 1.0f / (float)cnt;
        if (w <= 0.0f) continue;
        s_plCombo = i; s_plMode = m;               // the last one with a weight takes the rounding
        acc += w;
        if (u < acc) return;
    }
}
const float kChW1 = 50.0f, kChW2 = 55.0f;          // light 1 / light 2 widths, deg
// lamp1_level well under the full compensation (~2.5 at W50): darker than today (user: "more in the pure blue
// thing ... because it's darker"; the first render at 2.0 / 1.6 read too bright)
const float kChLvMono = 1.4f, kChLvPair = 1.3f;
// The chapter's own patch dye is DENSE (dye B1 at k = 2.5 x the self dye's, all of fluid B): under the other
// light it passes ~1 % instead of ~17 %, so it reads near-black ("red and some black and white"), under its own
// light it glows. It thins back to the self dye on the way to the white pair (OPEN), so WHITE is the Scheme.
const float kChDyeK = 2.5f;
int   s_ch = CH_WHITE;
float s_cT = 0.0f, s_cLen = 0.0f;                  // time in the step, its planned length
// NEXT (DirectorSkip; the "next" shortcut, the tray's Next stage, the Settings button). While the director runs, a
// cycle stage is only its base, so a press changed nothing on screen. The user's call 2026-11-18 on what a press
// should do: "Start the next shift now". The hold of the still on screen (WHITE or PAIR) ends at once and the draw is
// among the OTHER pairs' stills, so a colour change always follows (the same pair's other lighting is no answer to
// "next"). Pressed during a light glide (CLOSE / OPEN) it waits for the hold that follows; pressed while a change
// is already on screen (the trip, the darkroom walk) it is dropped. Never pressed = today, draw for draw.
// A garnish move in flight (the DASH: ~2.5 min on the live cadence, and every hold ends with one) is not walked to
// its end: it is wound down from where its envelope stands in at most kSkipOut seconds, then the hold ends.
bool  s_skip = false, s_skipWound = false;
const float kSkipOut = 10.0f;
float s_l1C = 0.0f, s_l1From = 0.0f, s_l1To = 0.0f;   // light 1's centre (absolute deg), SWAP's glide
bool  s_dimFromPair = false;
float s_keep = 0.0f;                               // T8 connections: the share keeper's cover offset
float s_redFix = 0.0f;                             // T8 connections: the turn's pull away from pure red (deg)
// the TURN cadence (director_turn): its own clock, legs and stream (never the chapter's draws)
float s_tnT = 0.0f, s_tnLen = -1.0f, s_tnFrom = 0.0f, s_tnTo = 0.0f, s_tnNow = 0.0f, s_tnSide = 1.0f;
bool  s_tnMoving = false;
uint64_t s_tnRng = 0;
float TnU(float a, float b) {
    uint64_t z = (s_tnRng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return a + (b - a) * (float)((z >> 40) * (1.0 / 16777216.0));
}
void TurnReset() { s_tnT = 0.0f; s_tnLen = -1.0f; s_tnFrom = s_tnTo = s_tnNow = 0.0f; s_tnSide = 1.0f; s_tnMoving = false; }
// one step of the turn; frozen while the chapter's own glide moves the colour
void TurnStep(float dt, const LiquidAcidConfig& a, bool chapterGlides) {
    if (!(a.directorTurn > 0.05f)) { TurnReset(); return; }
    const float H = a.directorHold > 0.5f ? a.directorHold : 45.0f, M = a.directorMove > 0.5f ? a.directorMove : 120.0f;
    if (s_tnLen < 0.0f) {                          // first call: seed the stream, start on a hold at neutral
        s_tnRng = (s_seed ? (uint64_t)s_seed : (uint64_t)GetTickCount64()) * 0x2545F4914F6CDD1Dull ^ 0x7E57ull;
        s_tnLen = H * TnU(0.8f, 1.2f);
    }
    if (chapterGlides) return;
    s_tnT += dt;
    if (!s_tnMoving) {
        if (s_tnT >= s_tnLen) {                    // the hold is over: the next leg
            s_tnMoving = true; s_tnT = 0.0f; s_tnLen = M * TnU(0.85f, 1.15f); s_tnFrom = s_tnNow;
            if (fabsf(s_tnNow) < 0.01f) { s_tnTo = s_tnSide * a.directorTurn * TnU(0.5f, 1.0f); s_tnSide = -s_tnSide; }
            else s_tnTo = 0.0f;                    // every other leg returns to neutral
            Log("[director] t=%.1f TURN %+.1f -> %+.1f deg over %.0f s\n", s_clock, s_tnFrom, s_tnTo, s_tnLen);
        }
    } else {
        s_tnNow = s_tnFrom + (s_tnTo - s_tnFrom) * Smooth(s_tnT / s_tnLen);
        if (s_tnT >= s_tnLen) { s_tnMoving = false; s_tnT = 0.0f; s_tnNow = s_tnTo; s_tnLen = H * TnU(0.8f, 1.2f); }
    }
}

// ---- the SHIFT trip (T8, the user's own design, 2026-11-18 ~14:35, on the first example's sheet: "Turn the blue to
// violet with a hue shift and add "blue" ... Don't change the geometry of anything ... Between 0 and 1. Hue shift to
// the end color ... Start adding a dye that would be blue, but before the hue shift makes it blue ... And then add
// the final step, the amber. Start adding it in small amounts whenever."). director_dark_path 2's white trip:
//   SHIFT  the film turns where it is (nothing replaces it) from the old film hue to the new one over kShLen, and the
//          whole-picture cadence turn glides to neutral with it (the first example's pink = violet under the frozen
//          turn). The old patches leave: SetHue2Shapes' hold, so no new patch fluid is made below the edge.
//          The INCOMING DYE is the third hue (film_hue3: the field's low end; share 0 = only the deepest film, none of
//          it on screen at the stage's own cover) at old film - new film, so the same shift carries it onto the old
//          film's colour; it rises in as blobs (SetHue2Shapes lo) from the first seconds on.
//   AMBER  once no old patch is on screen the patch colour becomes the new pair's (hidden); the new patches come in
//          through their own growing blobs (hi, up to kShAmber) while the dye's blobs shrink, then the hold lets go as shapes.
//   END    the new patches' blobs hand over to the stage's own field; landed once the third hue is off screen
//          (its amount back to the stage's, hidden) and the patch share is near the still's own (>= 0.2).
// Waits: 20 s floor, 300 s ceiling, a nominal 90 s without the field (the --shot-time0 fast-forward).
enum { SH_SHIFT, SH_AMBER, SH_END, SH_GLIDE };
const char* const kShName[] = { "SHIFT", "AMBER", "END", "GLIDE" };
const float kShLen = 150.0f, kShLo1 = 0.5f, kShLoMax = 0.62f, kShAmber = 0.3f;   // kShLoMax: past it the mask covers the row (a band)
const float kShH3Off = 0.25f;                     // field min above this: no cell in the third hue's band (0.11..0.21)
const float kShOpen = 75.0f, kShClose = 120.0f;   // pair -> pair: the lights open over the shift's start, narrow over SH_END
const float kShHold0 = 20.0f;                     // the old patches start leaving this late, so they overlap the first dye blobs
const float kShDye0 = 30.0f;                      // the dye's blob ramp starts this far in (it showed only at ~60 s)
int   s_sh = SH_SHIFT;
float s_shT = 0.0f, s_shAt = 0.0f, s_shTurn0 = 0.0f, s_shHi = 0.0f, s_dyeB = 0.0f;
// the SHARE KEEPER (T8 connections): new patch fluid through the patch-end blobs while the visible patch share is
// under kKeepShare (the share wanders on its own: the first sheets showed ~40 s of one colour with a sliver of the
// other inside a CLOSE and a glide). Shapes, never a cover step. Its own smoothed amount.
const float kKeepShare = 0.35f;
float s_kpHi = 0.0f;
float KeepHi(const FluidRenderer* r, float dt) {
    float share = 0.0f;
    const float want = (r && r->Hue2FieldShare(0.62f, share)) ? fminf(fmaxf((kKeepShare - share) * 2.5f, 0.0f), 0.5f) : 0.0f;
    s_kpHi += (want - s_kpHi) * (1.0f - expf(-dt / 8.0f));
    return s_kpHi;
}
void StartShift(const Combo& ci, const Combo& cj, bool dark) {
    s_sh = SH_SHIFT; s_shT = 0.0f; s_shAt = 0.0f; s_shTurn0 = s_tnNow; s_lkDark = dark;   // dark: pair -> pair
    // two pairs whose film AND patch colours are both within kLkNear deg (blue coral / teal orange, magenta mint /
    // magenta cyan): the whole picture glides instead (the turn's own move; nothing pours, the patches stay)
    if (fmaxf(fabsf(Wrap180(cj.anchor - ci.anchor)), fabsf(Wrap180(cj.anchor + cj.offset - ci.anchor - ci.offset))) <= kLkNear)
        s_sh = SH_GLIDE;
    Log("[director] t=%.1f SHIFT %ls -> %ls%s: film %.0f -> %.0f, incoming dye at %+.0f, patches %.0f -> %.0f, turn %+.1f -> 0\n",
        s_clock, ci.name.c_str(), cj.name.c_str(), s_sh == SH_GLIDE ? " (GLIDE)" : "", ci.anchor, cj.anchor, Wrap180(ci.anchor - cj.anchor),
        Wrap360(ci.anchor + ci.offset), Wrap360(cj.anchor + cj.offset), s_tnNow);
}
bool ShiftCore(float dt, const Combo& ci, const Combo& cj, float selfSig, const FluidRenderer* r, Out& o) {
    s_shT += dt;
    const Out A = RestOut(ci, selfSig), B = RestOut(cj, selfSig);
    float mn = 0.0f, mx = 1.0f;
    const bool have = r && r->Hue2FieldRange(mn, mx);
    const float base = s_base[K_COVER], tIn = s_shT - s_shAt;
    auto waited = [&](float t, bool cond) { return t > 300.0f || (t > 20.0f && (have ? cond : t > 90.0f)); };
    auto next = [&](int p) {
        Log("[director] t=%.1f SHIFT %ls -> %ls: %s after %.0f s (field %.2f..%.2f)\n", s_clock, ci.name.c_str(),
            cj.name.c_str(), kShName[p], s_shT, mn, mx);
        s_sh = p;
    };
    if (s_sh == SH_GLIDE) {                        // the near pair: kShLen of the whole-picture glide
        const float u = fminf(s_shT / kShLen, 1.0f);
        o = TripOut(ci, cj, u, 0.0f, selfSig);
        o.shHi = KeepHi(r, dt) * fminf(fminf(u * 4.0f, (1.0f - u) * 4.0f), 1.0f);   // the share keeper, eased in/out
        if (s_shT < kShLen) return false;
        o = B;
        Log("[director] t=%.1f SHIFT %ls -> %ls: GLIDE landed after %.0f s\n", s_clock, ci.name.c_str(), cj.name.c_str(), s_shT);
        return true;
    }
    const float v = Smooth(s_shT / kShLen);
    o = A;
    o.hue = Wrap360(A.hue + Wrap180(B.hue - A.hue) * v);
    o.scale = Lerp(A.scale, B.scale, v); o.load = Lerp(A.load, B.load, v);
    WaterGlide(o, A, B, v);
    o.coverOn = true;
    o.h3 = Wrap180(A.hue - B.hue); o.h3Amt = 1.0f; o.h3Share = 0.0f;
    s_tnNow = s_shTurn0 * (1.0f - v);              // the cadence turn rides the shift to neutral
    auto ramp = [](float t, float t0, float len) { return fminf(fmaxf((t - t0) / len, 0.0f), 1.0f); };
    // the two-colour floor: the visible share of the dye (field under 0.2) and the patches (over 0.62) together; while
    // it is under 0.3 the dye's blobs grow more (one sheet showed a single dye blob, then ~80 s of teal alone)
    auto dyeBoost = [&](float on) {
        float lo = 0.0f, hi = 0.0f;
        const bool got = r && r->Hue2FieldShare(0.2f, lo) && r->Hue2FieldShare(0.62f, hi);
        const float want = got ? fminf(fmaxf((0.3f - ((1.0f - lo) + hi)) * 1.5f, 0.0f), 0.35f) * on : 0.0f;
        s_dyeB += (want - s_dyeB) * (1.0f - expf(-dt / 25.0f));
        return s_dyeB;
    };
    if (s_sh == SH_SHIFT) {
        // the old patches keep their colour while they leave: the hold stops new patch fluid without lowering the
        // rest of the field (a cover drop pushed the whole bottom row into the third hue's band: a flat-topped band)
        o.patch = A.patch;
        o.hold = ramp(s_shT, kShHold0, 30.0f);   // (from 0 s the sheets showed 15-25 s of the film alone before the dye)
        // the dye: blobs from single peaks, at the same growth rate but kShDye0 earlier (the mask passes its first
        // peaks from the first second), so the first blobs are in view while the old patches are still there
        o.shLo = fminf(kShLo1 * ramp(s_shT, -kShDye0, 90.0f) + dyeBoost(s_shT > 45.0f ? 1.0f : 0.0f), kShLoMax);
        if (waited(s_shT, mx < kLkNone)) { s_shAt = s_shT; next(SH_AMBER); }
    } else if (s_sh == SH_AMBER) {
        // hidden: no old patch on screen. The new patches come in through their OWN growing blobs while the dye's
        // blobs shrink (no gap between the blue leaving and the amber being there); then the hold lets go (as shapes), so the
        // stage's own patch field returns under them
        o.patch = B.patch;
        o.shLo = fminf(kShLo1 + dyeBoost(1.0f - ramp(tIn, 80.0f, 60.0f)), kShLoMax) * (1.0f - ramp(tIn, 60.0f, 140.0f));   // stays while the amber grows
        o.shHi = fmaxf(kShAmber * ramp(tIn, 10.0f, 100.0f), KeepHi(r, dt) * ramp(tIn, 60.0f, 30.0f)); s_shHi = o.shHi;
        o.hold = 1.0f - ramp(tIn, 40.0f, 120.0f);                // the stage's own patches freed as growing shapes
        if (tIn >= 160.0f && v >= 1.0f) { s_shAt = s_shT; next(SH_END); }
    } else {
        // SH_END: the new patches' blobs hand over to the stage's own field, only while the patch share holds (>= 0.22,
        // so the hand-over never leaves one colour; a still with a thin field of its own lets go after 200 s anyway)
        o.patch = B.patch;
        float share = 0.0f;
        const bool haveS = r && r->Hue2FieldShare(0.62f, share);
        if (!haveS || share >= 0.22f || tIn > 200.0f) s_shHi = fmaxf(s_shHi - dt * kShAmber / 150.0f, 0.0f);
        o.shHi = fmaxf(s_shHi, KeepHi(r, dt));    // the keeper holds the share while the blobs hand over
        const bool closed = !s_lkDark || tIn >= kShClose;   // pair -> pair: the lights are narrow again (ShiftStep)
        if (closed && (have ? (s_shHi <= 0.0f && ((mn > kShH3Off && share >= 0.2f) || tIn > 400.0f)) : tIn > 150.0f)) {
            o = B;
            s_tnNow = 0.0f; s_tnMoving = false; s_tnT = 0.0f; s_tnFrom = s_tnTo = 0.0f; s_tnLen = 45.0f;
            Log("[director] t=%.1f SHIFT %ls -> %ls: landed after %.0f s (field %.2f..%.2f, patch share %.2f)\n", s_clock,
                ci.name.c_str(), cj.name.c_str(), s_shT, mn, mx, share);
            return true;
        }
    }
    (void)base;
    o.a2 = o.hue;
    o.b1 = o.patch;
    return false;
}
// the shift; between two darkroom pairs (s_lkDark) the lights open for it and narrow again as it lands: light 1's
// centre glides from the old pair's safelight end to the new one's with the film's shift, light 2 stays on the patch
// dye (it moves with the hidden patch swap, when no patch is on screen)
bool ShiftStep(float dt, const Combo& ci, const Combo& cj, float selfSig, const FluidRenderer* r, Out& o) {
    const bool done = ShiftCore(dt, ci, cj, selfSig, r, o);
    if (s_lkDark) {
        const float L1A = SafelightEnd(Wrap360(ci.anchor), Wrap360(ci.anchor + ci.offset));
        const float L1B = SafelightEnd(Wrap360(cj.anchor), Wrap360(cj.anchor + cj.offset));
        const float e = done ? 1.0f : Smooth(s_shT / kShLen);
        LinkLights(o, Wrap360(L1A + Wrap180(L1B - L1A) * e), 1.0f, o.patch, 1.0f);
        // the user's rule (2026-11-18): the colours change under WHITE light. The two ranges open with the start of
        // the shift (OPEN's own form) and narrow again over the hand-over (SH_END), so the incoming dye is lit while
        // it is on screen (kept narrow it read a dull grey-teal: no range of the lamp sits on the third hue)
        float g = Smooth(fminf(s_shT / kShOpen, 1.0f));
        if (s_sh == SH_END) g = fminf(g, 1.0f - Smooth(fminf((s_shT - s_shAt) / kShClose, 1.0f)));
        if (done) g = 0.0f;
        o.darkW = Lerp(kLkW1, 360.0f, g); o.light2W = Lerp(kLkW2, 360.0f, g); o.level = Lerp(kLkLv, 1.0f, g);
    }
    return done;
}

void StartDash(const LiquidAcidConfig& a) {         // StartMove's DASH alone (its draws, own order)
    s_move = M_DASH; s_mt = 0.0f;
    s_mA = (U01() < 0.5f ? -1.0f : 1.0f) * U(20.0f, 45.0f);
    s_mB = U(0.15f, 0.35f);
    s_mIn = U(15.0f, 25.0f); s_mHold = U(20.0f, 45.0f); s_mOut = U(15.0f, 25.0f);
    if (a.directorMove > 0.5f) {                   // cadence: out and back each half the move (mean), same draws
        const float k = 0.5f * a.directorMove / 20.0f;
        s_mIn *= k; s_mOut *= k;
    }
    if (a.directorHold > 0.5f) s_mHold *= a.directorHold / 45.0f;   // the pause at the walked hue
    Log("[director] t=%.1f %ls: DASH a=%+.1f b=%.2f in %.0f hold %.0f out %.0f s\n", s_clock,
        s_combos[s_cur].name.c_str(), s_mA, s_mB, s_mIn, s_mHold, s_mOut);
}

void PickNext() {                                  // StartTrip's choice: not this combo, not the last one
    const int n = (int)s_combos.size();
    int j = s_cur;
    for (int tries = 0; n > 1 && tries < 64; tries++) {
        j = (int)(U01() * (float)n) % n;
        if (j != s_cur && (j != s_prev || n < 3)) break;
    }
    if (n > 1 && j == s_cur) j = (s_cur + 1) % n;
    s_next = j;
}

// ui3 Keep this (DirectorKeep, 2026-11-23): the hold on screen does not end before this time on the cycle's hold clock.
// 0 = no keep: every line that reads it is skipped, the chapter step is today's.
double s_keepUntil = 0.0;
void KeepTick() {
    if (CycleHoldClock() < s_keepUntil) return;
    Log("[keep] director: the keep is over at t=%.1f (%s); the hold may end\n", s_clock, kChapName[s_ch]);
    s_keepUntil = 0.0;
}

void ChapterStep(float dt, const LiquidAcidConfig& a, float selfSig, const FluidRenderer* r) {
    const float R = fmaxf(a.directorRest, 10.0f), sR = R / 180.0f;
    const bool cadMove = a.directorMove > 0.5f, cadHold = a.directorHold > 0.5f;   // the cadence keys
    auto glide = [&](float s) { return cadMove ? fmaxf(a.directorMove, 6.0f) : fmaxf(s * sR, 6.0f); };
    float mn = 0.0f, mx = 1.0f;
    const bool have = r && r->Hue2FieldRange(mn, mx);
    float share = 0.0f;                            // the visible share past the patch seam
    const bool haveS = r && r->Hue2FieldShare(0.62f, share);
    s_cT += dt;
    if (s_keepUntil > 0.0) KeepTick();             // ui3 Keep this: its end, logged when it comes
    auto waited = [&](bool cond) { return s_cT > 240.0f || (s_cT > 8.0f && (have ? cond : s_cT > 75.0f)); };
    auto go = [&](int step, float len) {
        Log("[director] t=%.1f CHAPTER %ls: %s -> %s after %.0f s (field %.2f..%.2f, patch share %.2f)\n", s_clock,
            s_combos[s_cur].name.c_str(), kChapName[s_ch], kChapName[step], s_cT, mn, mx, share);
        s_ch = step; s_cT = 0.0f; s_cLen = len; s_keep = 0.0f;
    };
    Out o = RestOut(s_combos[s_cur], selfSig);
    auto light1 = [&](float c, float e) {          // e 0 = white .. 1 = light 1 alone
        o.darkC = c; o.darkW = 360.0f + (kChW1 - 360.0f) * e; o.level = 1.0f + (kChLvMono - 1.0f) * e;
    };
    auto pair = [&](float e2) {                    // light 1 + the second range on the patch dye, e2 0..1 open
        o.darkC = s_l1C; o.darkW = kChW1; o.light2C = o.patch; o.light2W = kChW2 * e2;
        o.level = Lerp(kChLvMono, kChLvPair, e2);
    };
    auto dense = [&](float e) {                    // the chapter's patch dye made dense (kChDyeK), e 0..1
        o.b1 = o.patch; o.b1Amt = e; o.b1K = kChDyeK * a.filmDyeK[0];
    };
    auto garnish = [&]() {                         // the DASH now and then (kept: "more aesthetic")
        if (s_move != M_NONE) {
            s_mt += dt;
            ApplyMove(o, MoveEnv());
            if (MoveDone()) { s_move = M_NONE; s_gap = U(30.0f, 70.0f); }
        } else if ((s_gap -= dt) <= 0.0f) {
            StartDash(a);
        }
    };
    // T8 connections: the SHARE KEEPER of the light glides (CLOSE / OPEN on director_dark_path 2): KeepHi's blobs,
    // eased in and out with the step (no cover step, so nothing enters as a flat front)
    auto keepShare = [&]() {
        const float u = fminf(s_cT / fmaxf(s_cLen, 1.0f), 1.0f);
        o.shHi = KeepHi(r, dt) * fminf(fminf(u * 4.0f, (1.0f - u) * 4.0f), 1.0f);
    };
    // cadence: the garnish starts when the hold ends (0.5 s early, so the step cannot end before it)
    auto dashAtHoldEnd = [&]() { if (cadHold) s_gap = fmaxf(s_cLen - 0.5f, 0.0f); };
    // T8 accent (director_accent deg, 0 = today; user 2026-11-18: "In the blue orange thing, if it was 2 different
    // shades of blue it looks fine"): the patches are KEPT through the one-light steps as a neighbouring shade of the
    // film. The patch dye glides to the film hue +- this many degrees (toward the combo's patch hue) as the light
    // narrows (DIM), RECEDE / DRAIN / MONO leave the stage's own cover (no emptying), so film + patch shapes stay on
    // screen in two shades of the one light; at FILL / DYE the patch dye glides on over 20 s to the next film / the
    // dark chapter dye, so DYE / MORE still read dark. RECEDE then waits a hold instead of an empty tank; DRAIN waits
    // until the full cover has broken up (field min < 0.3).
    const float accDeg = fmaxf(a.directorAccent, 0.0f);
    const bool  accOn = accDeg > 0.0f;
    const float restPatch = o.patch;               // this combo's patch hue
    const float shade = Wrap360(o.hue + (Wrap180(restPatch - o.hue) >= 0.0f ? accDeg : -accDeg));
    auto glideHue = [](float from, float to, float e) { return Wrap360(from + Wrap180(to - from) * e); };
    const float accHold = cadHold ? fmaxf(a.directorHold, 8.0f) : 45.0f;
    // T8 pour ramp (director_pour_ramp s, 0 = today): FILL's cover rises from the stage's own value to the pour's over
    // this many seconds, so the next dye enters as the ordinary patch shapes that grow and merge, full last
    const float pourRamp = fmaxf(a.directorPourRamp, 0.0f);
    // T8 blob (director_pour_k, 0 = today): the FILL pour made dense, thinned back over the SWAP relight
    const float pourK = a.directorPourK > 0.0f ? a.directorPourK * a.filmDyeK[0] : 0.0f;
    // T8 direct darkroom (director_dark_path 1) and, on it, the rated pool of stills (2)
    const bool direct = a.directorDarkPath >= 1, pool = a.directorDarkPath == 2;
    // NEXT: only a hold (or the light glide before one) can take it; anything else is already a change on screen
    // THE QUEUE: while it holds a look, a NEXT that meets a change on screen (the trip) is KEPT for the hold that
    // follows (that hold then ends at once and the queue is taken), so a "show next" never waits out a whole hold
    if (s_skip && s_ch != CH_WHITE && s_ch != CH_PAIR && s_ch != CH_CLOSE && s_ch != CH_OPEN && !(pool && !s_queue.empty())) {
        Log("[director] t=%.1f NEXT dropped: a change is already on screen (%s)\n", s_clock, kChapName[s_ch]);
        s_skip = false; s_skipWound = false; s_skipByQueue = false;
    }
    // the hold is over: its time is up, or NEXT was pressed (a DASH in flight finishes first, as at any hold's end).
    // Asked right AFTER garnish() in the two hold steps, where the hold's end has always been tested.
    bool over = false, skip = false;
    auto holdEnd = [&]() {
        over = (s_cT >= s_cLen || s_skip) && s_move == M_NONE;
        if (over && s_keepUntil > 0.0) over = false;   // ui3 Keep this: the hold waits (timer, queue, NEXT)
        skip = over && s_skip;
        if (skip) { s_skip = false; s_skipWound = false; s_skipByQueue = false; }
    };
    // NEXT with a garnish move in flight: the move's clock is put on a short way out that starts at the envelope's
    // present value (Smooth(u) = e, the smoothstep's inverse), so nothing jumps; asked right BEFORE garnish()
    auto windDown = [&]() {
        if (!s_skip || s_skipWound || s_move == M_NONE) return;
        if (s_keepUntil > 0.0) return;             // ui3 Keep this: a queued NEXT waits for the keep's end
        const float e = fminf(fmaxf(MoveEnv(), 0.0f), 1.0f);
        const float u = 0.5f - sinf(asinf(1.0f - 2.0f * e) / 3.0f);
        s_mIn = 0.0f; s_mHold = 0.0f; s_mOut = kSkipOut; s_mt = (1.0f - u) * kSkipOut;
        s_skipWound = true;
        Log("[director] t=%.1f NEXT: the %s is wound down from %.2f in %.1f s\n", s_clock, kMoveName[s_move], e, u * kSkipOut);
    };
    switch (s_ch) {
    case CH_WHITE:
        windDown();
        garnish();
        holdEnd();
        if (pool && over) {                        // T8 looks: the next still by rating
            const bool first = s_firstWhite;
            s_firstWhite = false;
            PoolDraw(0, first || skip);
            Log("[director] t=%.1f POOL %ls white -> %ls %s (rating %.0f%s)\n", s_clock, s_combos[s_cur].name.c_str(),
                s_combos[s_plCombo].name.c_str(), s_plMode ? "pair" : "white", LookRate(s_plCombo, s_plMode),
                first ? ", first move" : skip ? ", NEXT pressed" : "");
            if (s_plCombo == s_cur) {              // this combo's darkroom pair: the lights close
                s_plCombo = -1;
                s_l1C = SafelightEnd(o.hue, o.patch);
                s_dimFromPair = false;
                go(CH_CLOSE, glide(40.0f));
            } else {                               // another combo: the white trip (the LINK)
                s_next = s_plCombo;
                s_pour = PO_RECEDE; s_pt = 0.0f;
                StartShift(s_combos[s_cur], s_combos[s_next], false);
                go(CH_TRIP, 0.0f);
            }
            break;
        }
        if (over && (a.directorDarkShare < 1.0f || a.directorDarkPath == 1)) {
            // T8 order: the darkroom (today's walk, or with director_dark_path 1 the direct PAIR) or the white trip
            const bool first = s_firstWhite && a.directorDarkShare < 1.0f;
            s_firstWhite = false;
            const float u = DsU();
            if (first || skip || !(u < a.directorDarkShare)) {   // NEXT: the trip to another combo, never the darkroom
                PickNext();
                s_pour = PO_RECEDE; s_pt = 0.0f;
                Log("[director] t=%.1f CHAPTER %ls: TRIP under white to %ls (u %.3f, dark share %.2f%s)\n", s_clock,
                    s_combos[s_cur].name.c_str(), s_combos[s_next].name.c_str(), u, a.directorDarkShare,
                    first ? ", first move" : "");
                go(CH_TRIP, 0.0f);
                break;
            }
            if (a.directorDarkPath == 1) {         // T8 direct darkroom: both lights close from white together
                s_l1C = SafelightEnd(o.hue, o.patch);
                s_dimFromPair = false;
                go(CH_CLOSE, glide(40.0f));
                break;
            }
        }
        if (over) {
            s_l1C = SafelightEnd(o.hue, o.patch);
            s_dimFromPair = false;
            go(CH_DIM, glide(35.0f));
        }
        break;
    case CH_DIM: {
        const float e = Smooth(s_cT / s_cLen);
        if (s_dimFromPair) { pair(1.0f - e); dense(1.0f - e); } else light1(s_l1C, e);
        if (accOn) o.patch = glideHue(restPatch, shade, e);   // T8 accent: the patches turn to the shade
        if (s_cT >= s_cLen) { PickNext(); go(CH_RECEDE, 0.0f); }
        break;
    }
    case CH_RECEDE:
        light1(s_l1C, 1.0f); o.coverOn = true; o.cover = -0.6f;
        if (accOn) {                               // T8 accent: the shade patches stay (the stage's own cover)
            o.patch = shade; o.coverOn = false;
            if (s_cT >= accHold) go(CH_FILL, 0.0f);
        } else if (waited(mx < 0.55f)) go(CH_FILL, 0.0f);
        break;
    case CH_FILL: {
        const Out B = RestOut(s_combos[s_next], selfSig);
        light1(s_l1C, 1.0f); o.coverOn = true; o.cover = 0.8f;
        o.patch = B.hue; o.scale = B.scale; o.load = B.load;
        if (accOn) o.patch = glideHue(shade, B.hue, Smooth(s_cT / 20.0f));   // T8 accent: the shade becomes the pour
        if (pourK > 0.0f) { o.b1 = o.patch; o.b1Amt = 1.0f; o.b1K = pourK; }
        if (pourRamp > 0.0f) o.cover = Lerp(s_base[K_COVER], 0.8f, Smooth(s_cT / pourRamp));   // T8 pour ramp
        if (waited(mn > 0.7f)) {
            s_l1From = s_l1C; s_l1To = SafelightEnd(B.hue, B.patch); go(CH_SWAP, glide(25.0f));
            if (a.directorWater) {                 // director_water: the glide that rides this relight
                const Out A = RestOut(s_combos[s_cur], selfSig);
                Log("[director] t=%.1f WATER %ls -> %ls: murk_k %.2f -> %.2f, motes %.0f -> %.0f over %.0f s\n",
                    s_clock, s_combos[s_cur].name.c_str(), s_combos[s_next].name.c_str(), A.murkK, B.murkK,
                    A.motes, B.motes, fmaxf(s_cLen, 4.0f));
            }
        }
        break;
    }
    case CH_SWAP: {
        const Out B = RestOut(s_combos[s_next], selfSig);
        const float len = fmaxf(s_cLen, 4.0f);
        const float u = Smooth(s_cT / len), v = cadMove ? u : Smooth(s_cT / 4.0f);   // cadence: no 4 s hue swap
        const Out A = o;                           // this combo's rest (its water)
        o.hue = Wrap360(o.hue + Wrap180(B.hue - o.hue) * v);
        o.patch = B.hue; o.scale = B.scale; o.load = B.load; o.coverOn = true; o.cover = 0.8f;
        light1(Wrap360(s_l1From + Wrap180(s_l1To - s_l1From) * u), 1.0f);
        WaterGlide(o, A, B, u);                    // director_water: with the relight (light 1's glide)
        if (pourK > 0.0f) { o.b1 = o.patch; o.b1Amt = 1.0f - u; o.b1K = pourK; }   // T8: thins with the relight
        if (s_cT >= len) {
            s_l1C = s_l1To; s_prev = s_cur; s_cur = s_next; s_next = -1;
            go(CH_DRAIN, 0.0f);
        }
        break;
    }
    case CH_DRAIN:
        light1(s_l1C, 1.0f); o.patch = o.hue; o.coverOn = true; o.cover = -0.6f;
        if (accOn) {                               // T8 accent: the cover breaks up into film + shade patches
            o.patch = glideHue(o.hue, shade, Smooth(s_cT / 20.0f)); o.coverOn = false;
            if (waited(mn < 0.3f)) go(CH_MONO, HoldBase(a, R * 0.5f) * U(0.8f, 1.2f));
        } else if (waited(mx < 0.55f)) go(CH_MONO, HoldBase(a, R * 0.5f) * U(0.8f, 1.2f));
        break;
    case CH_MONO:
        light1(s_l1C, 1.0f); o.coverOn = true; o.cover = -0.6f;
        if (accOn) { o.patch = shade; o.coverOn = false; }   // T8 accent: two shades of the one light
        if (s_cT >= s_cLen) go(CH_DYE, 0.0f);
        break;
    case CH_DYE:                                   // the dye starts rising in (a few cells past the seam)
        if (accOn) o.patch = glideHue(shade, restPatch, Smooth(s_cT / 20.0f));   // T8 accent: the shade darkens
        light1(s_l1C, 1.0f); dense(1.0f); o.coverOn = true; o.cover = 0.15f;
        if (s_cT > 240.0f || (s_cT > fmaxf(0.25f * R, 8.0f) && (haveS ? share >= 0.08f : true)))
            go(CH_MORE, R * 0.35f * U(0.8f, 1.2f));
        break;
    case CH_MORE:                                  // until about half the frame is the dark dye
        light1(s_l1C, 1.0f); dense(1.0f); o.coverOn = true; o.cover = 0.45f;
        if (s_cT > 240.0f || (s_cT >= s_cLen && (haveS ? share >= 0.5f : true))) go(CH_LIGHT2, glide(30.0f));
        break;
    case CH_LIGHT2:
        pair(Smooth(s_cT / s_cLen)); dense(1.0f);
        if (s_cT >= s_cLen) { s_gap = U(20.0f, 40.0f); go(CH_PAIR, HoldBase(a, R * 0.6f) * U(0.8f, 1.2f)); dashAtHoldEnd(); }
        break;
    case CH_PAIR:
        pair(1.0f); if (!direct) dense(1.0f);      // T8 direct path: the tank keeps its own fluid
        windDown();
        garnish();
        holdEnd();
        if (over) {
            if (pool) {                            // T8 looks: the next still by rating; every way out is OPEN
                PoolDraw(1, skip);                 // NEXT: among the other combos' stills only
                Log("[director] t=%.1f POOL %ls pair -> %ls %s (rating %.0f%s)\n", s_clock, s_combos[s_cur].name.c_str(),
                    s_combos[s_plCombo].name.c_str(), s_plMode ? "pair" : "white", LookRate(s_plCombo, s_plMode),
                    skip ? ", NEXT pressed" : "");
                if (s_plCombo == s_cur) s_plCombo = -1;   // this combo's white pair: OPEN, then its hold
                if (s_plCombo >= 0 && s_plMode == 1) {    // another combo's darkroom pair: the link in the darkroom
                    s_next = s_plCombo; s_plCombo = -1;
                    StartShift(s_combos[s_cur], s_combos[s_next], true);   // the shift; the lights open for it, then narrow
                    go(CH_TRIP, 0.0f);
                    break;
                }
                go(CH_OPEN, glide(40.0f));
            }
            else if (direct) go(CH_OPEN, glide(40.0f));   // T8 direct path: back to WHITE
            else if (U01() < 0.6f) go(CH_OPEN, glide(40.0f));
            else { s_dimFromPair = true; go(CH_DIM, glide(35.0f)); }
        }
        break;
    case CH_TRIP:                                  // T8 order: the pour trip under white light
        if (pool ? ShiftStep(dt, s_combos[s_cur], s_combos[s_next], selfSig, r, o)               // T8: the shift trip

                 : (PourStep(dt, s_combos[s_cur], s_combos[s_next], selfSig, r, o), s_pour == PO_RETURN)) {
                                                   // landed: the next combo's WHITE (its patches pour in)
            s_prev = s_cur; s_cur = s_next; s_next = -1;
            if (pool && s_lkDark) {                // T8 connections: the darkroom link lands on the PAIR itself
                const Out B = RestOut(s_combos[s_cur], selfSig);
                s_lkDark = false; s_plCombo = -1;
                s_l1C = SafelightEnd(B.hue, B.patch);
                s_gap = U(20.0f, 40.0f); go(CH_PAIR, HoldBase(a, R * 0.6f) * U(0.8f, 1.2f)); dashAtHoldEnd();
                break;
            }
            if (pool && s_plCombo == s_cur && s_plMode == 1) {   // T8 looks: bound for this combo's darkroom pair,
                const Out B = RestOut(s_combos[s_cur], selfSig);   // so no white hold: the lights close at once
                s_plCombo = -1;
                s_l1C = SafelightEnd(B.hue, B.patch);
                s_dimFromPair = false;
                go(CH_CLOSE, glide(40.0f));
                break;
            }
            s_plCombo = -1;
            s_gap = U(30.0f, 70.0f); go(CH_WHITE, HoldBase(a, R) * U(0.75f, 1.25f)); dashAtHoldEnd();
        }
        break;
    case CH_CLOSE: {                               // T8 direct darkroom: OPEN backwards, no dense dye
        const float e = Smooth(s_cT / s_cLen);
        pair(1.0f);
        o.darkW = Lerp(360.0f, kChW1, e); o.light2W = Lerp(360.0f, kChW2, e); o.level = Lerp(1.0f, kChLvPair, e);
        if (pool) keepShare();
        if (s_cT >= s_cLen) { s_gap = U(20.0f, 40.0f); go(CH_PAIR, HoldBase(a, R * 0.6f) * U(0.8f, 1.2f)); dashAtHoldEnd(); }
        break;
    }
    default: {                                     // CH_OPEN: both ranges widen to white
        const float e = Smooth(s_cT / s_cLen);
        pair(1.0f); if (!direct) dense(1.0f - e);
        o.darkW = Lerp(kChW1, 360.0f, e); o.light2W = Lerp(kChW2, 360.0f, e); o.level = Lerp(kChLvPair, 1.0f, e);
        if (pool) keepShare();
        if (s_cT >= s_cLen && pool && s_plCombo >= 0 && s_plCombo != s_cur) {   // T8 looks: a pass, not a rest:
            s_next = s_plCombo;                    // straight on into the white trip to the drawn combo
            s_pour = PO_RECEDE; s_pt = 0.0f;
            StartShift(s_combos[s_cur], s_combos[s_next], false);
            go(CH_TRIP, 0.0f);
            break;
        }
        if (s_cT >= s_cLen) { s_gap = U(30.0f, 70.0f); go(CH_WHITE, HoldBase(a, R) * U(0.75f, 1.25f)); dashAtHoldEnd(); }
        break;
    }
    }
    s_out = o;
}

// director_turn_dark 0 (post-night T5): how much of the turn the chapter step lets through -- eased out over
// DIM (from WHITE or from PAIR), none from RECEDE to MORE (one light: the darkroom), eased back over LIGHT2
float TurnDarkMul() {
    const float e = s_cLen > 0.0f ? Smooth(s_cT / s_cLen) : 1.0f;
    switch (s_ch) {
    case CH_DIM:    return 1.0f - e;
    case CH_RECEDE: case CH_FILL: case CH_SWAP: case CH_DRAIN: case CH_MONO: case CH_DYE: case CH_MORE:
        return 0.0f;
    case CH_LIGHT2: return e;
    default:        return 1.0f;
    }
}

// T8 band (director_band_lo / _hi): where a shown hue h lands inside the band, pulled in by m at each edge
float BandTarget(float h, float lo, float hi, float m) {
    h = Wrap360(h);
    const float kReach = 120.0f;                   // one edge alone: how far past it a hue is pulled back
    if (lo > 0.0f && hi > 0.0f) {                  // both: lo -> up through the purples -> hi
        const float W = Wrap360(hi - lo), mm = fminf(m, 0.5f * W);
        const float l2 = lo + mm, W2 = W - 2.0f * mm;
        const float d = Wrap360(h - l2);
        if (d <= W2) return h;
        return (d - W2 < 0.5f * (360.0f - W2)) ? Wrap360(l2 + W2) : Wrap360(l2);   // the nearer edge
    }
    if (hi > 0.0f) { const float e = hi - m, p = Wrap180(h - e); return (p > 0.0f && p <= kReach) ? Wrap360(e) : h; }
    if (lo > 0.0f) { const float e = lo + m, p = Wrap180(e - h); return (p > 0.0f && p <= kReach) ? Wrap360(e) : h; }
    return h;
}
// a glide between two band points that stays inside the band (both edges set), else the short way
float BandGlide(float f, float t, float u, float lo, float hi) {
    if (lo > 0.0f && hi > 0.0f) {
        const float df = Wrap360(f - lo), dt = Wrap360(t - lo);
        return Wrap360(lo + df + (dt - df) * u);
    }
    return Wrap360(f + Wrap180(t - f) * u);
}
// the extra whole-picture turn (deg) that keeps light 1's shown hue (its centre + the turn already applied) inside
// the band; the envelope is TurnDarkMul's inverted (in over DIM, held RECEDE..MORE, out over LIGHT2)
float BandTurn(const LiquidAcidConfig& a, float turnNow) {
    const float lo = a.directorBandLo, hi = a.directorBandHi;
    const float env = 1.0f - TurnDarkMul();
    if (!(env > 0.0f)) return 0.0f;
    const float m = (a.directorTurnDark != 0 && a.directorTurn > 0.05f) ? a.directorTurn : 0.0f;
    float L, D;
    if (s_ch == CH_SWAP) {                         // light 1 glides to the next combo's end: the shown hue too
        const float u = Smooth(s_cT / fmaxf(s_cLen, 4.0f));
        L = Wrap360(s_l1From + Wrap180(s_l1To - s_l1From) * u);
        D = BandGlide(BandTarget(s_l1From, lo, hi, m), BandTarget(s_l1To, lo, hi, m), u, lo, hi);
    } else {
        L = s_l1C;
        D = BandTarget(L, lo, hi, m);
    }
    const float shown = Wrap360(D + turnNow);      // the turn wanders round the centre; the edge still holds
    const float fix = Wrap180(BandTarget(shown, lo, hi, 0.0f) - shown);
    return env * (Wrap180(D - L) + fix);
}

// Advance the state (CPU only: the fast-forward runs this alone, r = nullptr).
// ui3 (2026-11-23): the hidden pair (removed, or a cancelled draft) that last landed and was sent on (-1 = none)
int s_hidLanded = -1;
void Step(float dt, const LiquidAcidConfig& a, float selfSig, const FluidRenderer* r) {
    s_clock += dt;
    s_pourRampNow = fmaxf(a.directorPourRamp, 0.0f);   // T8: also the trip's pour (0 = today)
    s_t += dt;
    const float mx = fminf(fmaxf(a.directorMix, 0.0f), 1.0f);
    // ui3: a pair REMOVED (or a draft cancelled) while it was on its way still lands; the first tick it is the pair on
    // screen in a hold it leaves at once (NEXT, as DirectorSkip), once per landing, instead of holding its full time.
    // Inert while no pair is hidden: ComboFlags reads 0 for every pair then.
    if (ComboFlags(s_cur) & CF_HIDDEN) {
        const bool hold = (s_phase == P_CHAP && (s_ch == CH_WHITE || s_ch == CH_PAIR)) || s_phase == P_REST;
        if (hold && s_hidLanded != s_cur && !s_skip) {
            s_hidLanded = s_cur;
            Log("[director] t=%.1f %ls was removed while on its way: it has landed and leaves now (NEXT)\n", s_clock,
                s_combos[s_cur].name.c_str());
            DirectorSkip();
        }
    } else if (s_hidLanded >= 0) {
        s_hidLanded = -1;
    }
    switch (s_phase) {
    case P_ENTER: {
        const float v = Smooth(s_t / s_len);
        const Out B = RestOut(s_combos[s_cur], selfSig);
        Out o = B;
        o.hue = Wrap360(s_from.hue + Wrap180(B.hue - s_from.hue) * v);
        o.patch = Wrap360(s_from.patch + Wrap180(B.patch - s_from.patch) * v);
        o.scale = Lerp(s_from.scale, B.scale, v);
        o.load = Lerp(s_from.load, B.load, v);
        WaterGlide(o, s_from, B, v);               // director_water: from the stage's water
        s_out = o;
        if (s_t >= s_len && a.directorStyle == 1 && a.directorDemo != 1) {   // the chapters, from the white pair
            s_phase = P_CHAP; s_ch = CH_WHITE; s_cT = 0.0f; s_cLen = RestLen(a); s_move = M_NONE; s_gap = U(30.0f, 70.0f);
            s_firstWhite = true;                   // T8 order: the first move is never the darkroom
            s_skip = false; s_skipWound = false;   // a NEXT from before this engage is not carried over
            s_plCombo = -1;                        // T8 looks: no still in flight
            s_qTaken = false; s_skipByQueue = false;
            if (a.directorDarkPath == 2 && !s_queue.empty()) {   // THE QUEUE: a look queued while engaging is taken
                s_skip = s_skipByQueue = true;                   // at once (the first white hold ends)
                Log("[director] t=%.1f QUEUE waiting: the first white hold ends at once\n", s_clock);
            }
            if (a.directorHold > 0.5f) {           // cadence: the first WHITE holds director_hold, then the dash
                s_cLen *= a.directorHold / fmaxf(a.directorRest, 10.0f);
                s_gap = fmaxf(s_cLen - 0.5f, 0.0f);
            }
            Log("[director] t=%.1f CHAPTERS from %ls (white for %.0f s)\n", s_clock, s_combos[s_cur].name.c_str(), s_cLen);
        } else if (s_t >= s_len) {
            s_phase = P_REST; s_t = 0.0f; s_len = RestLen(a); s_gap = U(20.0f, 50.0f);
            Log("[director] t=%.1f REST %ls for %.0f s\n", s_clock, s_combos[s_cur].name.c_str(), s_len);
        }
        break;
    }
    case P_CHAP:
        ChapterStep(dt, a, selfSig, r);
        TurnStep(dt, a, s_ch == CH_DIM || s_ch == CH_SWAP || s_ch == CH_LIGHT2 || s_ch == CH_OPEN || s_ch == CH_TRIP
                        || s_ch == CH_CLOSE);
        s_out.turn = s_tnNow;                      // 0 unless director_turn (the cadence)
        if (a.directorTurnDark == 0 && s_tnNow != 0.0f) s_out.turn *= TurnDarkMul();   // post-night T5
        if (a.directorBandLo > 0.0f || a.directorBandHi > 0.0f) s_out.turn += BandTurn(a, s_out.turn);   // T8 band
        if (a.directorDarkPath == 2) {             // T8 connections: under white light the turn never makes the film
            // pure red (the first sheets: magenta + 30 deg = a red tank through a launch and a trip; the user: "This
            // pure red is too basic"). The film's shown hue keeps kRedGap from 0 on its own side; the darkroom pair
            // (a red safelight is its kept look) and the glides into it let go. Eased (20 s), as slow as the turn.
            const float kRedGap = 25.0f;
            const bool white = s_ch == CH_WHITE || (s_ch == CH_TRIP && !s_lkDark);
            const float env = white ? 1.0f : (s_ch == CH_OPEN || s_ch == CH_CLOSE)
                ? (s_ch == CH_OPEN ? Smooth(s_cT / fmaxf(s_cLen, 1.0f)) : 1.0f - Smooth(s_cT / fmaxf(s_cLen, 1.0f))) : 0.0f;
            const float own = Wrap180(s_combos[s_cur].anchor), f = Wrap180(own + s_out.turn);
            float want = 0.0f;
            if (fabsf(f) < kRedGap) want = (own < 0.0f ? -kRedGap : kRedGap) - f;
            s_redFix += (want * env - s_redFix) * (1.0f - expf(-dt / 20.0f));
            s_out.turn += s_redFix;
        }
        break;
    case P_REST: {
        Out o = RestOut(s_combos[s_cur], selfSig);
        const bool demo = a.directorDemo == 1;     // the darkroom tour: no trips, moves 4 s apart
        if (s_move != M_NONE) {
            s_mt += dt;
            ApplyMove(o, MoveEnv());
            if (MoveDone()) { s_move = M_NONE; s_gap = demo ? 4.0f : U(20.0f, 50.0f); }
        } else if (s_t < s_len || demo) {
            if (demo) s_gap = fminf(s_gap, 4.0f);
            s_gap -= dt;
            if (s_gap <= 0.0f) StartMove(demo);
        }
        s_out = o;
        if (!demo && s_t >= s_len && s_move == M_NONE) StartTrip(a);
        break;
    }
    case P_TRIP: {
        bool done;
        if (s_pourTrip) {
            done = PourStep(dt, s_combos[s_cur], s_combos[s_next], selfSig, r, s_out);
        } else {
            s_out = TripOut(s_combos[s_cur], s_combos[s_next], fminf(s_t / s_len, 1.0f), mx, selfSig);
            done = s_t >= s_len;
        }
        if (done) {
            s_prev = s_cur; s_cur = s_next; s_next = -1;
            s_phase = P_REST; s_t = 0.0f; s_len = RestLen(a); s_gap = U(20.0f, 50.0f);
            Log("[director] t=%.1f REST %ls for %.0f s\n", s_clock, s_combos[s_cur].name.c_str(), s_len);
        }
        break;
    }
    default: break;
    }
}

void Adopt(LiquidAcidConfig& a) {
    for (int k = 0; k < K_N; k++) {
        const float v = *Field(a, k);
        if (!s_have || Bits(v) != Bits(s_last[k])) s_base[k] = v;
    }
}

void Write(FluidRenderer& r, LiquidAcidConfig& a, const Out& o, float selfSig, float selfK) {
    float w[K_N];
    w[K_HUE2]  = Wrap180(o.patch - o.hue);
    w[K_SCALE] = o.scale;
    w[K_LOAD]  = o.load;
    w[K_A2H]   = Wrap180(o.a2 - o.hue);
    w[K_A2S]   = o.a2Sig;
    w[K_A2K]   = selfK;
    w[K_A2A]   = o.a2Amt;
    w[K_B1H]   = Wrap180(o.b1 - o.hue);
    w[K_B1S]   = selfSig;
    w[K_B1K]   = o.b1K > 0.0f ? o.b1K : selfK;
    w[K_B1A]   = o.b1Amt;
    if (o.darkW < 359.5f) {                  // a narrow window (the darkroom's safelight)
        w[K_LO] = Wrap180(o.darkC - o.hue) - 0.5f * o.darkW;
        w[K_HI] = w[K_LO] + o.darkW;
    } else if (o.notchW > 0.5f) {            // white with a notch: one window round the rest
        w[K_LO] = Wrap180(o.notchC + 0.5f * o.notchW - o.hue);
        w[K_HI] = w[K_LO] + 360.0f - o.notchW;
    } else {                                 // white = today's lamp
        w[K_LO] = 0.0f;
        w[K_HI] = 360.0f;
    }
    if (o.darkW < 359.5f && o.light2W > 0.5f) {    // the chapters' second light: lamp 1's second range
        w[K_LO2] = Wrap180(o.light2C - o.hue) - 0.5f * o.light2W;
        w[K_HI2] = w[K_LO2] + fminf(o.light2W, 360.0f);
    } else {
        w[K_LO2] = 0.0f;
        w[K_HI2] = 0.0f;
    }
    w[K_LEVEL] = o.level;
    w[K_COVER] = o.coverOn ? o.cover : s_base[K_COVER];
    w[K_H3]  = o.h3Amt >= 0.0f ? o.h3      : s_base[K_H3];    // T8 trip: the incoming dye (else the stage's own)
    w[K_H3A] = o.h3Amt >= 0.0f ? o.h3Amt   : s_base[K_H3A];
    w[K_H3S] = o.h3Amt >= 0.0f ? o.h3Share : s_base[K_H3S];
    for (int k = 0; k < K_N; k++) { *Field(a, k) = w[k]; s_last[k] = w[k]; }
    s_have = true;
    r.SetDirectorHue(o.turn != 0.0f ? Wrap360(o.hue + o.turn) : o.hue);   // director_turn: the whole picture
    r.SetHue2RefreshHold(o.coverOn);               // the refresh pours wait while it pours
    r.SetHue2Shapes(fmaxf(o.shLo, -o.shape), fmaxf(o.shHi, o.shape), o.hold);  // T8 connections: the blobs (0 = off)
    // director_water (post-night T3): the combo's murk level + mote count; key 0 = off = nothing changes
    r.SetDirectorWater(a.directorWater, o.murkK, o.motes);
    s_relK = o.murkK; s_relMotes = o.motes;
}

// Every key that still holds its own output gets the base back.
void RestoreBase(LiquidAcidConfig& a) {
    if (!s_have) return;
    for (int k = 0; k < K_N; k++) {
        float* p = Field(a, k);
        if (Bits(*p) == Bits(s_last[k])) *p = s_base[k];
    }
}

void Engage(FluidRenderer& r, const LiquidAcidConfig& a, float selfSig) {
    LoadCombos();
    EnsureSeeded();
    // the start: the combo nearest the stage's own (its named anchor, else the
    // palette's hue now, and its patch offset); the hue glides there over 20 s
    const float raw = r.DirectorHue() >= 0.0f ? r.DirectorHue() : r.PaletteHueRaw();
    const float h = a.paletteStartHue >= 0.0f ? a.paletteStartHue : raw;
    int best = 0;
    float bd = 1e9f;
    for (size_t i = 0; i < s_combos.size(); i++) {
        if (Excluded((int)i)) continue;           // ui3: never engage on a draft / removed pair
        const float d = fabsf(Wrap180(s_combos[i].anchor - h)) + 0.5f * fabsf(Wrap180(s_combos[i].offset - a.filmHue2));
        if (d < bd) { bd = d; best = (int)i; }
    }
    s_cur = best; s_prev = -1; s_next = -1;
    s_from = RestOut(s_combos[best], selfSig);
    s_from.hue = Wrap360(raw);
    s_from.patch = Wrap360(raw + a.filmHue2);
    s_from.scale = a.filmHue2Scale;
    s_from.load = a.filmEqualLoadP;
    s_from.murkK = 1.0f;                     // director_water: the entry glides from the stage's own water
    s_from.motes = s_stageMotes;
    r.RequestWaterPrewarm();                 // post-night T3 pre-warm of the water twins (live only, blobs only)
    s_phase = P_ENTER; s_t = 0.0f; s_len = 20.0f;
    TurnReset();                             // the turn cadence starts at neutral (no-op state while it is off)
    s_redFix = 0.0f; s_lkDark = false;       // T8 connections
    s_move = M_NONE;
    s_gap = U(20.0f, 50.0f);
    Log("[director] engaged at %ls (palette hue %.1f -> %.1f over 20 s), %d combos\n",
        s_combos[best].name.c_str(), raw, s_combos[best].anchor, (int)s_combos.size());
}

} // namespace

void DirectorSetLogger(void (*fn)(const char*)) { s_log = fn; }
void DirectorSeed(unsigned seed) { s_seed = seed; s_seeded = false; }
void DirectorFastForward(float seconds) { s_ff = seconds > 0.0f ? seconds : 0.0f; }
bool DirectorActive() { return s_phase != P_OFF; }
bool DirectorSkip() {                              // NEXT: see s_skip
    if (s_phase == P_CHAP) {
        s_skip = true;
        Log("[director] t=%.1f NEXT pressed (%s)\n", s_clock, kChapName[s_ch]);
        return true;
    }
    if (s_phase == P_REST) {                       // director_style 0: the rest ends, the trip follows
        s_t = fmaxf(s_t, s_len);
        Log("[director] t=%.1f NEXT pressed: the rest ends\n", s_clock);
        return true;
    }
    return false;                                  // engaging, mid trip or letting go: nothing to skip
}

// T8 looks: the stills' ratings (PoolDraw). mode 0 = the white pair, 1 = the darkroom pair.
int   DirectorLookCount() { EnsureRates(); return (int)s_combos.size(); }
std::wstring DirectorLookName(int i) {
    EnsureRates();
    return (i >= 0 && i < (int)s_combos.size()) ? s_combos[i].name : std::wstring();
}
// THE QUEUE (see s_queue): the Settings window's "show next". A look goes to the back of the line; when it is the
// only one and a hold is on screen the move starts at once (NEXT), otherwise it is taken at the next hold's end.
void  DirectorQueueLook(int i, int mode) {
    EnsureRates();
    if (i < 0 || i >= (int)s_combos.size() || s_queue.size() >= 12) return;
    const std::pair<int, int> q(i, mode ? 1 : 0);
    if (!s_queue.empty() && s_queue.back() == q) return;      // a double click is one entry
    if (s_queue.empty() && s_phase == P_CHAP) {                // the look already on screen or on its way: nothing to do
        int ti = s_cur, tm = 0;
        switch (s_ch) {
        case CH_WHITE: break;
        case CH_PAIR: case CH_CLOSE: tm = 1; break;
        case CH_OPEN: if (s_plCombo >= 0 && s_plCombo != s_cur) { ti = s_plCombo; tm = s_plMode; } break;
        case CH_TRIP: ti = s_next; tm = s_lkDark ? 1 : (s_plCombo == s_next ? s_plMode : 0); break;
        default: ti = -1; break;
        }
        if (q == std::pair<int, int>(ti, tm)) {
            Log("[director] t=%.1f QUEUE: %ls %s is already on screen or on its way, not queued\n", s_clock,
                s_combos[i].name.c_str(), q.second ? "pair" : "white");
            return;
        }
    }
    s_queue.push_back(q);
    Log("[director] t=%.1f QUEUE + %ls %s (%d in the queue)\n", s_clock, s_combos[i].name.c_str(),
        q.second ? "pair" : "white", (int)s_queue.size());
    // alone in the line: the hold on screen ends now (NEXT; a change on screen keeps it for the hold that follows),
    // unless the still on screen / on its way is itself a queued pick: that one keeps its whole hold
    if (s_queue.size() == 1 && !s_qTaken && DirectorSkip()) s_skipByQueue = true;
}
int   DirectorQueueCount() { return (int)s_queue.size(); }
bool  DirectorQueueAt(int k, int* i, int* mode) {
    if (k < 0 || k >= (int)s_queue.size()) return false;
    if (i) *i = s_queue[k].first;
    if (mode) *mode = s_queue[k].second;
    return true;
}
void  DirectorQueueClear() {                       // a NEXT the queue started and nobody took yet goes too
    s_queue.clear();
    if (s_skipByQueue) { s_skip = false; s_skipWound = false; s_skipByQueue = false; }
    Log("[director] t=%.1f QUEUE cleared\n", s_clock);
}
// What is on screen, in plain words (the Settings window's "Now" line); empty while the director is off.
std::string DirectorNowText() {
    if (s_phase == P_OFF) return std::string();
    auto nm = [](int i) {
        std::string o;
        if (i >= 0 && i < (int)s_combos.size()) for (wchar_t ch : s_combos[i].name) o += (ch == L'_') ? ' ' : (char)ch;
        return o;
    };
    if (s_phase != P_CHAP) return nm(s_cur);
    switch (s_ch) {
    case CH_WHITE: return nm(s_cur) + ", vivid (white light)";
    case CH_PAIR:  return nm(s_cur) + ", darkroom (two lights)";
    case CH_CLOSE: return nm(s_cur) + ": the lights are closing to its darkroom";
    case CH_OPEN:  return nm(s_cur) + ": the lights are opening to white";
    case CH_TRIP:  return "changing from " + nm(s_cur) + " to " + nm(s_next)
                        + ((s_lkDark || (s_plCombo >= 0 && s_plMode == 1)) ? " (darkroom)" : " (vivid)");
    default:       return nm(s_cur) + " (" + kChapName[s_ch] + ")";
    }
}
float DirectorLookRating(int i, int mode) { EnsureRates(); return LookRate(i, mode ? 1 : 0); }
float DirectorLookRatingSum() {
    EnsureRates();
    float s = 0.0f;
    for (int i = 0; i < (int)s_combos.size(); i++) s += LookRate(i, 0) + LookRate(i, 1);
    return s;
}
void  DirectorSetLookRating(int i, int mode, float v) {
    EnsureRates();
    if (i >= 0 && i < (int)s_combos.size()) s_rate[mode ? 1 : 0][i] = fminf(fmaxf(v, 0.0f), 10.0f);
}
void  DirectorLoadLooks(const wchar_t* ini) {
    if (!ini || !ini[0]) return;
    if (s_loaded) { EnsureRates(); ApplyLooksIni(ini); return; }
    if (s_looksIni.empty() || s_looksIni.back() != ini) s_looksIni.push_back(ini);   // until the list has loaded
}

std::string DirectorStatusLine() {
    if (s_phase == P_OFF) return "off";
    auto nm = [](int i) { std::string o; if (i >= 0 && i < (int)s_combos.size()) for (wchar_t ch : s_combos[i].name) o += (char)ch; return o; };
    static const char* const kPhase[] = { "off", "ENTER", "REST", "TRIP", "RELEASE", "CHAPTER" };
    char b[256];
    if (s_phase == P_CHAP) {
        snprintf(b, sizeof(b), "t=%.0f CHAPTER %s %s%s%s move %s (%.0f/%.0f s)", s_clock, nm(s_cur).c_str(),
                 kChapName[s_ch], s_next >= 0 ? " -> " : "", s_next >= 0 ? nm(s_next).c_str() : "",
                 s_move == M_NONE ? "none" : kMoveName[s_move], s_cT, s_cLen);
        return b;
    }
    snprintf(b, sizeof(b), "t=%.0f %s %s%s%s move %s (%.0f/%.0f s)", s_clock, kPhase[s_phase], nm(s_cur).c_str(),
             s_phase == P_TRIP ? " -> " : "", s_phase == P_TRIP ? nm(s_next).c_str() : "",
             s_move == M_NONE ? "none" : kMoveName[s_move], s_t, s_len);
    return b;
}

void DirectorBaseCopy(FluidConfig& inout) {
    if (!s_have) return;                     // never wrote: nothing of its own in any config
    for (int k = 0; k < K_N; k++) {
        float* p = Field(inout.acid, k);
        if (Bits(*p) == Bits(s_last[k])) *p = s_base[k];
    }
}

void DirectorTick(FluidRenderer& r, float dt) {
    LiquidAcidConfig& a = r.Config().acid;
    const bool want = a.enabled && a.colourDirector;
    if (!want && s_phase == P_OFF) return;   // off: nothing touched (bit-identical)
    if (!(dt > 0.0f)) dt = 0.0f;
    const float selfSig = a.filmDyeSigma[0], selfK = a.filmDyeK[0];
    s_stageMotes = (float)a.motes;           // director_water: the stage's count (it never writes motes)
    if (want) {
        if (s_phase == P_OFF || s_phase == P_RELEASE) {
            if (s_phase == P_OFF) Adopt(a);  // the stage's own values = the base
            Engage(r, a, selfSig);
            if (s_ff > 0.0f) {               // --shot-time0: the CPU state runs forward
                const float ff = s_ff;
                s_ff = 0.0f;
                for (float t = 0.0f; t < ff; t += 0.25f) Step(0.25f, a, selfSig, nullptr);
                Log("[director] fast-forward %.0f s (--shot-time0)\n", ff);
            }
        }
        Adopt(a);
        if (ClocksHeld()) return;            // the Settings window: frozen, the sliders write
        Step(dt, a, selfSig, &r);
        Write(r, a, s_out, selfSig, selfK);
        return;
    }
    // turned off while on
    r.SetHue2RefreshHold(false);             // off: the refresh pours are the stage's own again
    r.SetHue2Shapes(0.0f, 0.0f, 0.0f);
    if (!a.enabled) {                        // another look, through black: let go at once
        RestoreBase(a);
        r.SetDirectorHue(-1.0f);
        r.SetDirectorWater(false, 1.0f, 0.0f);
        s_phase = P_OFF; s_have = false;
        Log("[director] released (look change)\n");
        return;
    }
    if (s_phase != P_RELEASE) {
        Adopt(a);
        s_phase = P_RELEASE; s_relT = 0.0f;
        memcpy(s_relFrom, s_last, sizeof(s_relFrom));
        s_relHue = r.DirectorHue() >= 0.0f ? r.DirectorHue() : r.PaletteHueRaw();
        Log("[director] t=%.1f releasing over 8 s\n", s_clock);
    }
    Adopt(a);
    s_relT += dt;
    const float g = Smooth(s_relT / 8.0f);
    for (int k = 0; k < K_N; k++) {
        const float v = IsAngle(k) ? s_relFrom[k] + Wrap180(s_base[k] - s_relFrom[k]) * g
                                   : Lerp(s_relFrom[k], s_base[k], g);
        *Field(a, k) = v;
        s_last[k] = v;
    }
    const float raw = r.PaletteHueRaw();
    r.SetDirectorHue(Wrap360(s_relHue + Wrap180(raw - s_relHue) * g));
    // director_water: the water glides back to the stage's own with the rest, then lets go
    r.SetDirectorWater(a.directorWater, Lerp(s_relK, 1.0f, g), Lerp(s_relMotes, (float)a.motes, g));
    if (s_relT >= 8.0f) {
        for (int k = 0; k < K_N; k++) *Field(a, k) = s_base[k];
        r.SetDirectorHue(-1.0f);
        r.SetDirectorWater(false, 1.0f, 0.0f);
        s_phase = P_OFF; s_have = false;
        Log("[director] released\n");
    }
}

// ================================================================================================ ui3 (2026-11-23)
// Read-only views for the Settings window's HOME screen. Additions only: nothing above this line changed, and
// nothing below writes any director state (the pair list itself excepted: DirectorLoadCombosFrom, headless only,
// before the list has loaded).
float DirectorLookAnchor(int i) {
    LoadCombos();
    return (i >= 0 && i < (int)s_combos.size()) ? s_combos[i].anchor : 0.0f;
}
float DirectorLookOffset(int i) {
    LoadCombos();
    return (i >= 0 && i < (int)s_combos.size()) ? s_combos[i].offset : 180.0f;
}
DirectorView DirectorNow() {
    DirectorView v;
    if (s_phase == P_OFF) return v;
    const int n = (int)s_combos.size();
    auto ok = [n](int i) { return i >= 0 && i < n; };
    v.active = true;
    v.combo = ok(s_cur) ? s_cur : -1;
    if (s_phase == P_CHAP) {
        switch (s_ch) {
        case CH_WHITE: v.mode = 0; v.holdLeftSec = fmaxf(s_cLen - s_cT, 0.0f); break;
        case CH_PAIR:  v.mode = 1; v.holdLeftSec = fmaxf(s_cLen - s_cT, 0.0f); break;
        case CH_CLOSE: v.mode = 0; v.changing = true; v.toCombo = v.combo; v.toMode = 1; break;
        case CH_OPEN:
            v.mode = 1; v.changing = true;
            if (ok(s_plCombo) && s_plCombo != s_cur) { v.toCombo = s_plCombo; v.toMode = s_plMode; }
            else { v.toCombo = v.combo; v.toMode = 0; }
            break;
        case CH_TRIP:                              // a trip always leaves from white light (a PAIR opens first)
            v.mode = 0; v.changing = true;
            v.toCombo = ok(s_next) ? s_next : -1;
            v.toMode = (s_lkDark || (s_plCombo >= 0 && s_plCombo == s_next && s_plMode == 1)) ? 1 : 0;
            break;
        default:                                   // the old darkroom walk (DIM ... LIGHT2): one light or two
            v.mode = 1; v.changing = true; break;
        }
        if (s_skip && v.holdLeftSec > 0.0f) v.holdLeftSec = 0.0f;   // NEXT pressed: the hold ends now
    } else if (s_phase == P_REST) {
        v.holdLeftSec = fmaxf(s_len - s_t, 0.0f);
    } else if (s_phase == P_TRIP) {
        v.changing = true;
        v.toCombo = ok(s_next) ? s_next : -1;
    } else {                                       // ENTER / RELEASE
        v.changing = true;
    }
    if (!v.changing && !s_queue.empty() && ok(s_queue.front().first)) {
        v.nextCombo = s_queue.front().first;
        v.nextMode = s_queue.front().second;
    }
    v.keepLeftSec = DirectorKeepLeftSec();
    return v;
}
void DirectorKeep(float sec) {
    if (!(sec > 0.0f)) {
        if (s_keepUntil > 0.0) Log("[keep] director: let go at t=%.1f (%.0f s early)\n", s_clock, fmax(s_keepUntil - CycleHoldClock(), 0.0));
        s_keepUntil = 0.0;
        return;
    }
    s_keepUntil = fmax(CycleHoldClock() + sec, 1e-6);
    Log("[keep] director: the hold of %ls %s is kept %.0f s from t=%.1f (%s)\n",
        (s_cur >= 0 && s_cur < (int)s_combos.size()) ? s_combos[s_cur].name.c_str() : L"-",
        s_phase == P_CHAP ? kChapName[s_ch] : "-", sec, s_clock,
        (s_phase == P_CHAP && (s_ch == CH_WHITE || s_ch == CH_PAIR)) ? "this hold" : "the hold that follows");
}
float DirectorKeepLeftSec() { return s_keepUntil > 0.0 ? (float)fmax(s_keepUntil - CycleHoldClock(), 0.0) : 0.0f; }
bool DirectorLoadCombosFrom(const wchar_t* ini) {
    if (s_loaded || !ini || !ini[0] || GetFileAttributesW(ini) == INVALID_FILE_ATTRIBUTES) return false;
    std::vector<Combo> got;
    std::vector<wchar_t> buf(32768);
    const DWORD n = GetPrivateProfileSectionW(L"combos", buf.data(), (DWORD)buf.size(), ini);
    for (const wchar_t* e = buf.data(); e < buf.data() + n && *e; e += wcslen(e) + 1) {   // = LoadCombos' line rule
        std::wstring line(e);
        const size_t sc = line.find_first_of(L";#");
        if (sc == 0) continue;
        if (sc != std::wstring::npos) line.resize(sc);
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring nm = line.substr(0, eq);
        while (!nm.empty() && iswspace(nm.back())) nm.pop_back();
        while (!nm.empty() && iswspace(nm.front())) nm.erase(nm.begin());
        Combo k{ nm, 0.0f, 180.0f, 0.25f, 0.5f, 1.0f, -1.0f };
        if (nm.empty() || swscanf_s(line.c_str() + eq + 1, L" %f , %f , %f , %f , %f , %f",
                                    &k.anchor, &k.offset, &k.scale, &k.load, &k.murkK, &k.motes) < 2) continue;
        got.push_back(k);
    }
    if (got.empty()) return false;
    s_combos = got;
    s_loaded = true;
    s_source = ini;                                // ui3: DirectorComboSource
    Log("[director] %d combos from %ls (--ui-combos)\n", (int)s_combos.size(), ini);
    return true;
}

// ================================================================================================ ui3 add / remove
// (2026-11-23; reference/briefs/UI3-PLAN.md "Adding and removing a pair"). Additions only, each inert until its own
// call: the list only ever GROWS (indices held in s_cur / s_next / s_prev / s_plCombo / the queue / s_rate stay
// valid); a removed pair is HIDDEN (ratings 0, never drawn, its queue entries dropped) and erased at the next start.
namespace {
const wchar_t* const kSeedNames[] = { L"magenta_mint", L"magenta_cyan", L"blue_coral", L"violet_amber", L"teal_orange", L"mint_magenta" };
void EnsureFlags() { if (s_flags.size() < s_combos.size()) s_flags.resize(s_combos.size(), 0); }
bool ValidCombo(int i) { return i >= 0 && i < (int)s_combos.size(); }
// on screen or on its way (the chapter is travelling to it): only Hide's "ratings 0 + skip" may touch it
bool InFlight(int i) { return s_phase != P_OFF && (i == s_cur || i == s_next || i == s_plCombo); }
std::wstring FullPath(const std::wstring& p) {
    wchar_t b[MAX_PATH] = {};
    const DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH, b, nullptr);
    return (n > 0 && n < MAX_PATH) ? std::wstring(b) : p;
}
bool SameFile(const std::wstring& a, const std::wstring& b) { return !a.empty() && !b.empty() && _wcsicmp(FullPath(a).c_str(), FullPath(b).c_str()) == 0; }
std::string Narrow(const std::wstring& w) { std::string o; for (wchar_t c : w) o += (c < 128) ? (char)c : '?'; return o; }
std::wstring ComboValue(const Combo& k) {          // the combos.ini columns: anchor, offset, scale, load, murk_k, motes
    wchar_t b[160];
    _snwprintf_s(b, _TRUNCATE, L"%g, %g, %g, %g, %g, %g", k.anchor, k.offset, k.scale, k.load, k.murkK, k.motes);
    return b;
}
// a text file as UTF-8 (a UTF-16 file converted, a UTF-8 BOM dropped); false = unreadable
bool ReadTextUtf8(const std::wstring& path, std::string& out) {
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string raw;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0) raw.append(buf, got);
    CloseHandle(h);
    if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE) {
        const int nw = (int)((raw.size() - 2) / 2);
        const wchar_t* w = (const wchar_t*)(raw.data() + 2);
        const int n = WideCharToMultiByte(CP_UTF8, 0, w, nw, nullptr, 0, nullptr, nullptr);
        out.resize(n > 0 ? n : 0);
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, nw, &out[0], n, nullptr, nullptr);
        return true;
    }
    if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF) raw.erase(0, 3);
    out = raw;
    return true;
}
// the lines of a combos.ini before its first [section] line (its comment header), CRLF-ended
std::string HeaderOf(const std::string& text) {
    std::string o;
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(p, e - p);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t f = line.find_first_not_of(" \t");
        if (f != std::string::npos && line[f] == '[') break;
        o += line + "\r\n";
        p = e + 1;
    }
    return o;
}
// The %APPDATA% file replaces every other list (LoadCombos reads it first): if the list in use came from another file
// or from the compiled-in seed, the whole list (every pair that is neither a draft nor removed) is written there
// FIRST, under the source file's comment header; a file already there that was not the source is kept as .bak.
// Returns 0 = could not write, 1 = the list already came from that file, 2 = the whole list was copied there now.
int EnsureTarget(const std::wstring& target, std::string& what) {
    if (SameFile(s_source, target)) return 1;
    std::string text, header;
    if (!s_source.empty() && ReadTextUtf8(s_source, text)) header = HeaderOf(text);
    if (header.find_first_not_of(" \t\r\n") == std::string::npos)
        header = "; FluidWallpaper colour pairs (the Settings window wrote this list; it replaces the built-in six).\r\n"
                 "; name = film anchor deg, patch offset deg, patch scale, patch load, murk_k, motes\r\n";
    std::string body = header + "[combos]\r\n";
    int n = 0;
    for (int i = 0; i < (int)s_combos.size(); i++) {
        if (Excluded(i)) continue;
        body += Narrow(s_combos[i].name) + " = " + Narrow(ComboValue(s_combos[i])) + "\r\n";
        n++;
    }
    if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) CopyFileW(target.c_str(), (target + L".bak").c_str(), FALSE);
    HANDLE h = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { what += "cannot write " + Narrow(target) + "; "; return 0; }
    DWORD wr = 0;
    const BOOL ok = WriteFile(h, body.data(), (DWORD)body.size(), &wr, nullptr);
    CloseHandle(h);
    if (!ok || wr != (DWORD)body.size()) { what += "writing " + Narrow(target) + " failed; "; return 0; }
    what += "the whole list (" + std::to_string(n) + " pairs) copied from " +
            (s_source.empty() ? std::string("the built-in six") : Narrow(s_source)) + " to " + Narrow(target) + "; ";
    s_source = target;
    return 2;
}
} // namespace

const char* DirectorHueName(float hue) {
    struct HN { float h; const char* n; };
    // DESIGN.md 5.13 + UI3-PLAN (Teal 190): the nearest name; used only to SUGGEST a pair's name
    static const HN k[] = { { 0, "Red" }, { 15, "Coral" }, { 30, "Orange" }, { 40, "Amber" }, { 58, "Yellow" },
        { 75, "Chartreuse" }, { 85, "Lime" }, { 115, "Green" }, { 145, "Mint" }, { 185, "Cyan" }, { 190, "Teal" },
        { 200, "Sky" }, { 215, "Blue" }, { 245, "Indigo" }, { 275, "Violet" }, { 300, "Purple" }, { 325, "Magenta" },
        { 345, "Pink" } };
    hue = Wrap360(hue);
    const char* best = k[0].n;
    float bd = 1e9f;
    for (const HN& e : k) {
        const float d = fabsf(Wrap180(hue - e.h));
        if (d < bd) { bd = d; best = e.n; }
    }
    return best;
}
std::wstring DirectorComboKeyFor(const std::wstring& wanted, int ignore) {
    LoadCombos();
    std::wstring k;
    bool gap = false;
    for (wchar_t c : wanted) {
        const bool word = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9');
        if (!word) { gap = true; continue; }
        if (gap && !k.empty()) k += L'_';
        gap = false;
        k += (c >= L'A' && c <= L'Z') ? (wchar_t)(c - L'A' + L'a') : c;
    }
    auto ends = [&](const wchar_t* s) { const size_t n = wcslen(s); return k.size() >= n && k.compare(k.size() - n, n, s) == 0; };
    for (bool again = true; again;) {               // look_* is a rated look's [looks] key; *_white / *_pair a face's
        again = false;
        if (k.rfind(L"look_", 0) == 0) { k.erase(0, 5); again = true; }
        if (ends(L"_white")) { k.resize(k.size() - 6); again = true; }
        if (ends(L"_pair")) { k.resize(k.size() - 5); again = true; }
    }
    if (k.size() > 40) { k.resize(40); while (!k.empty() && k.back() == L'_') k.pop_back(); }
    if (k.empty()) k = L"pair";
    auto taken = [&](const std::wstring& nm) {
        for (int i = 0; i < (int)s_combos.size(); i++)
            if (i != ignore && !(ComboFlags(i) & CF_HIDDEN) && _wcsicmp(s_combos[i].name.c_str(), nm.c_str()) == 0) return true;
        return false;
    };
    const std::wstring base = k;
    for (int n = 2; taken(k); n++) k = base + L"_" + std::to_wstring(n);
    return k;
}
int DirectorAddCombo(const std::wstring& name, float anchorHue, float offset, bool draft) {
    EnsureRates();                                 // the list + every waiting [looks], before it grows
    EnsureFlags();
    const std::wstring nm = DirectorComboKeyFor(name, -1);
    s_combos.push_back({ nm, Wrap360(anchorHue), Wrap360(offset), 0.25f, 0.50f, 1.0f, -1.0f });
    s_flags.push_back(draft ? CF_DRAFT : 0);
    EnsureRates();                                 // grows the ratings (5 for the new pair), keeps every other one
    const int i = (int)s_combos.size() - 1;
    s_rate[0][i] = s_rate[1][i] = draft ? 0.0f : 5.0f;
    Log("[director] ADD %ls = %ls (%s, index %d of %d)\n", nm.c_str(), ComboValue(s_combos[i]).c_str(),
        draft ? "a draft: ratings 0, only the queue reaches it" : "kept: ratings 5", i, (int)s_combos.size());
    return i;
}
bool DirectorUpdateDraft(int i, const std::wstring& name, float anchorHue, float offset) {
    if (!ValidCombo(i) || ComboFlags(i) != CF_DRAFT || InFlight(i)) return false;
    s_combos[i].name = DirectorComboKeyFor(name, i);
    s_combos[i].anchor = Wrap360(anchorHue);
    s_combos[i].offset = Wrap360(offset);
    Log("[director] DRAFT %d now %ls = %ls\n", i, s_combos[i].name.c_str(), ComboValue(s_combos[i]).c_str());
    return true;
}
void DirectorCommitCombo(int i) {
    EnsureRates();
    EnsureFlags();
    if (!ValidCombo(i)) return;
    s_flags[i] = 0;
    s_rate[0][i] = s_rate[1][i] = 5.0f;
    Log("[director] KEEP %ls: in the automatic mode, vivid and darkroom 5 of 10\n", s_combos[i].name.c_str());
}
void DirectorHideCombo(int i) {
    EnsureRates();
    EnsureFlags();
    if (!ValidCombo(i)) return;
    s_flags[i] = CF_HIDDEN;
    s_rate[0][i] = s_rate[1][i] = 0.0f;
    const size_t before = s_queue.size();
    for (size_t k = 0; k < s_queue.size();) if (s_queue[k].first == i) s_queue.erase(s_queue.begin() + k); else k++;
    if (s_queue.empty() && before > 0 && s_skipByQueue) { s_skip = false; s_skipWound = false; s_skipByQueue = false; }
    Log("[director] HIDE %ls: ratings 0, %d queue entr%s dropped%s; erased at the next start\n", s_combos[i].name.c_str(),
        (int)(before - s_queue.size()), before - s_queue.size() == 1 ? "y" : "ies", InFlight(i) ? ", on screen / on its way" : "");
}
bool DirectorComboHidden(int i) { return (ComboFlags(i) & CF_HIDDEN) != 0; }
bool DirectorComboIsDraft(int i) { return (ComboFlags(i) & CF_DRAFT) != 0; }
bool DirectorComboInFlight(int i) { return ValidCombo(i) && InFlight(i); }
bool DirectorComboIsBuiltIn(int i) {
    LoadCombos();
    if (!ValidCombo(i)) return false;
    for (const wchar_t* n : kSeedNames) if (_wcsicmp(n, s_combos[i].name.c_str()) == 0) return true;
    return false;
}
std::wstring DirectorComboSource() { LoadCombos(); return s_source.empty() ? std::wstring(L"built in") : s_source; }
std::string DirectorPersistKeep(int i, const wchar_t* combosIni, const wchar_t* settingsIni, bool readOnly) {
    if (!ValidCombo(i) || Excluded(i)) return "not a kept pair";
    const Combo& k = s_combos[i];
    std::string what = "keep " + Narrow(k.name) + ": ";
    if (readOnly || !combosIni || !combosIni[0] || !settingsIni || !settingsIni[0]) {
        what += "in memory only, write skipped (read-only config) [would write " + Narrow(k.name) + " = " +
                Narrow(ComboValue(k)) + " to combos.ini, [looks] " + Narrow(k.name) + "_white = 5 and _pair = 5]";
        Log("[combos] %s\n", what.c_str());
        return what;
    }
    const int t = EnsureTarget(combosIni, what);
    if (t == 1) {                                  // the file in use: its line, the owner's comments kept
        WritePrivateProfileStringW(L"combos", k.name.c_str(), ComboValue(k).c_str(), combosIni);
        what += "line " + Narrow(k.name) + " = " + Narrow(ComboValue(k)) + " written to " + Narrow(combosIni) + "; ";
    } else if (t == 2) {
        what += "(its line " + Narrow(k.name) + " = " + Narrow(ComboValue(k)) + " is in that copy); ";
    }
    for (int m = 0; m < 2; m++) {
        const std::wstring key = k.name + (m ? L"_pair" : L"_white");
        WritePrivateProfileStringW(L"looks", key.c_str(), L"5", settingsIni);
    }
    what += "[looks] " + Narrow(k.name) + "_white = 5, _pair = 5 in " + Narrow(settingsIni);
    Log("[combos] %s\n", what.c_str());
    return what;
}
std::string DirectorPersistRemove(int i, const wchar_t* combosIni, const wchar_t* settingsIni, bool readOnly) {
    if (!ValidCombo(i)) return "no such pair";
    if (DirectorComboIsBuiltIn(i)) return "a built-in pair is never removed";
    const Combo& k = s_combos[i];
    std::string what = "remove " + Narrow(k.name) + ": ";
    if (readOnly || !combosIni || !combosIni[0] || !settingsIni || !settingsIni[0]) {
        what += "in memory only, write skipped (read-only config) [would delete its combos.ini line and "
                "[looks] _white, _pair, _white_before_never, _pair_before_never]";
        Log("[combos] %s\n", what.c_str());
        return what;
    }
    const int t = EnsureTarget(combosIni, what);
    if (t == 1) {
        WritePrivateProfileStringW(L"combos", k.name.c_str(), nullptr, combosIni);
        what += "line " + Narrow(k.name) + " deleted from " + Narrow(combosIni) + "; ";
    } else if (t == 2) {
        what += "(the copy leaves " + Narrow(k.name) + " out); ";
    }
    for (const wchar_t* sfx : { L"_white", L"_pair", L"_white_before_never", L"_pair_before_never" })
        WritePrivateProfileStringW(L"looks", (k.name + sfx).c_str(), nullptr, settingsIni);
    what += "[looks] " + Narrow(k.name) + "_white, _pair, _white_before_never, _pair_before_never deleted from " + Narrow(settingsIni);
    Log("[combos] %s\n", what.c_str());
    return what;
}
