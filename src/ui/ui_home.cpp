// ui3 HOME (see ui_home.h). One model per frame (BuildModel: derived from the app's state, nothing stored), then the
// drawing. Numbers are the design's px at 1280 x 800 (build2\shots\ui3\mocks\h.html, DESIGN.md); P() scales them.
#include <windows.h>
#include <shellapi.h>
#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "imgui.h"
#include "ui_home.h"
#include "ui_model.h"
#include "ui_cycle.h"
#include "../app_state.h"

using Microsoft::WRL::ComPtr;

namespace {

// ============================================================================================ colour
struct Rgb { float r = 0, g = 0, b = 0; };
Rgb Hex(unsigned h) { return { ((h >> 16) & 255) / 255.0f, ((h >> 8) & 255) / 255.0f, (h & 255) / 255.0f }; }
float Lin(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
float Lum(Rgb c) { return 0.2126f * Lin(c.r) + 0.7152f * Lin(c.g) + 0.0722f * Lin(c.b); }
float Contrast(Rgb a, Rgb b) {
    float la = Lum(a), lb = Lum(b);
    if (la < lb) std::swap(la, lb);
    return (la + 0.05f) / (lb + 0.05f);
}
Rgb Mix(Rgb a, Rgb b, float t) { return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t }; }
Rgb Mul(Rgb a, float k) { return { a.r * k, a.g * k, a.b * k }; }
ImU32 U(Rgb c, float a = 1.0f) {
    auto q = [](float v) { return (int)lroundf(fminf(fmaxf(v, 0.0f), 1.0f) * 255.0f); };
    return IM_COL32(q(c.r), q(c.g), q(c.b), q(a));
}
std::string HexText(Rgb c) {
    char b[16];
    snprintf(b, sizeof(b), "#%02X%02X%02X", (int)lroundf(c.r * 255), (int)lroundf(c.g * 255), (int)lroundf(c.b * 255));
    return b;
}
const Rgb kBlack = Hex(0x000000), kWhite = Hex(0xFFFFFF), kAsh = Hex(0x9A9A9A), kTrack = Hex(0x666666),
          kPip = Hex(0x383838), kLine = Hex(0x2E2E2E), kEdge = Hex(0x5C5C5C), kRaised = Hex(0x1C1C1C);

// The app's colour for a hue (the swatches' own colours, DESIGN.md 2 + 5.13; between two entries the colour is
// interpolated, so a pair the owner adds gets a colour too). Blue 215 = #2334F2, the owner-approved anchor.
struct HueEntry { float hue; unsigned rgb; };
const HueEntry kHues[] = {
    { 0, 0xFF3B30 }, { 15, 0xFF6B57 }, { 30, 0xFF7A1A }, { 40, 0xFFA51F }, { 58, 0xFFD60A }, { 75, 0xB6F23A },
    { 85, 0xA8F23A }, { 115, 0x34D058 }, { 145, 0x4DEBB0 }, { 185, 0x1FC6EE }, { 190, 0x2FD3C6 }, { 200, 0x2FA8FF },
    { 215, 0x2334F2 }, { 245, 0x4B3BFF }, { 275, 0x8A3DFF }, { 300, 0xC23DFF }, { 325, 0xFF2BB0 }, { 345, 0xFF4F8B },
};
float Wrap360(float h) { h = fmodf(h, 360.0f); return h < 0 ? h + 360.0f : h; }
float Wrap180Ui(float d) { return Wrap360(d + 180.0f) - 180.0f; }
Rgb HueColour(float hue) {
    hue = Wrap360(hue);
    const int n = (int)(sizeof(kHues) / sizeof(kHues[0]));
    for (int k = 0; k < n; k++) {
        const HueEntry& a = kHues[k];
        const HueEntry& b = kHues[(k + 1) % n];
        const float hb = k + 1 < n ? b.hue : b.hue + 360.0f;
        if (hue >= a.hue && hue <= hb) {
            const float t = hb > a.hue ? (hue - a.hue) / (hb - a.hue) : 0.0f;
            return Mix(Hex(a.rgb), Hex(b.rgb), t);
        }
    }
    return Hex(kHues[0].rgb);
}

// field / strong / quiet / mark / drain (DESIGN.md 2, with the coordinator's correction 2026-11-23: the FIELD is
// the pair's FIRST colour = the director's anchor hue; one rule for every pair, so an added pair works too)
struct Pal { Rgb field, strong, quiet, mark, drain; };
Pal FinishPal(Pal p) { p.drain = Mix(p.field, p.strong, 0.28f); return p; }
Pal PairPal(float anchorHue, bool darkroom) {
    const Rgb c = HueColour(anchorHue);
    Pal p;
    // the mark: the colour as it reads on black (the one thing there that will end)
    p.mark = c;
    for (float t = 0.0f; t <= 1.0f && Contrast(p.mark, kBlack) < 7.0f; t += 0.02f) p.mark = Mix(c, kWhite, t);
    if (!darkroom) {
        if (Lum(c) >= 0.30f) {                                  // light colour: black text
            p.field = c; p.strong = kBlack;
            for (float t = 0.0f; t <= 1.0f; t += 0.02f) { p.quiet = Mix(c, kBlack, t); if (Contrast(p.quiet, c) >= 5.0f) break; }
        } else {                                                // white text at 4.5:1 or better
            p.field = c;
            for (float k = 1.0f; k > 0.2f && Contrast(kWhite, p.field) < 4.5f; k -= 0.02f) p.field = Mul(c, k);
            p.strong = kWhite;
            p.quiet = kWhite;                                   // white mixed toward the field until just 5:1
            for (float t = 0.0f; t <= 1.0f; t += 0.02f) {
                const Rgb q = Mix(kWhite, p.field, t);
                if (Contrast(q, p.field) < 5.0f) break;
                p.quiet = q;
            }
        }
    } else {                                                    // the same hue, deep, with light tints as text
        p.field = c;
        for (float k = 1.0f; k > 0.05f && Contrast(p.field, kBlack) > 1.9f; k -= 0.01f) p.field = Mul(c, k);
        p.strong = Mix(c, kWhite, 0.75f);
        for (float t = 0.75f; t <= 1.0f && Contrast(p.strong, p.field) < 7.0f; t += 0.02f) p.strong = Mix(c, kWhite, t);
        p.quiet = c;
        for (float t = 0.0f; t <= 1.0f && Contrast(p.quiet, p.field) < 5.0f; t += 0.02f) p.quiet = Mix(c, kWhite, t);
    }
    return FinishPal(p);
}
Pal FixedPal(unsigned f, unsigned s, unsigned q, unsigned m) { return FinishPal({ Hex(f), Hex(s), Hex(q), Hex(m), {} }); }
Pal InkPal() { return FixedPal(0xC3CDD8, 0x000000, 0x343F4B, 0xC3CDD8); }
Pal FluidPal() { return FixedPal(0x2FB98A, 0x000000, 0x0A3B2B, 0x2FB98A); }

// ============================================================================================ state
struct Home {
    int screen = SCR_HOME;
    int fixedClockMin = -1;           // headless "clock HH:MM"
    double scriptT0 = 0;              // the clock (s of day) when the timed states below were scripted
    bool pausedScripted = false;
    std::string selName;              // the pair the scale sets, by NAME ("" = the playing one)
    std::string status;               // a refused action, shown under the scale
    ULONGLONG statusAt = 0;
    int gridRow = 0;                  // the swatch grid's first visible row
    bool gridToSel = true;            // scroll the selected tile into view on the next HOME frame (open, after a keep)
    float looksScroll = 0;            // ALL LOOKS: the black side's scroll (design px)
    std::string removeAsk;            // ALL LOOKS: the pair (by name) whose "Remove ... for good?" is asked in place
    std::wstring nowImagePath;
    ComPtr<ID3D11ShaderResourceView> nowImg;
    int imgW = 0, imgH = 0;
    bool live = false;
    bool acts = false;                // a headless script is pressing controls (click / drag): they act
};
Home s_h;

// ADD A PAIR (g-add.png): one session at a time; a draft made by "Show it next" lives in the director (ratings 0)
struct AddState {
    int from = SCR_HOME;              // where Cancel goes back to
    float h1 = 215.0f, h2 = 85.0f;    // handle 1 = the first colour (the anchor), handle 2 = the second colour
    char name[32] = {};               // the name field (display text; max 28 characters)
    bool typed = false;               // the owner typed: the name stops following the hues
    int draft = -1;                   // the director's index of this session's draft (-1 = not added yet)
    int grab = 0;                     // the handle being dragged (1 / 2), 0 = none
    int focus = 2;                    // the handle the arrow keys move
};
AddState s_add;

struct Fonts { ImFont* r = nullptr; ImFont* s = nullptr; ImFont* b = nullptr; float k = 1.0f; float asc = 0.8f; };
Fonts s_f;

// ============================================================================================ time
double NowSecOfDay() {
    if (s_h.fixedClockMin >= 0) return s_h.fixedClockMin * 60.0;
    SYSTEMTIME st;
    GetLocalTime(&st);
    return st.wHour * 3600.0 + st.wMinute * 60.0 + st.wSecond + st.wMilliseconds / 1000.0;
}
std::string Hhmm(double secOfDay) {
    long s = (long)floor(secOfDay);
    s = ((s % 86400) + 86400) % 86400;
    char b[16];
    snprintf(b, sizeof(b), "%02ld:%02ld", s / 3600, (s / 60) % 60);
    return b;
}
std::string AboutMin(float sec) {
    if (sec < 60.0f) return "in under a minute";
    char b[48];
    snprintf(b, sizeof(b), "in about %d min", (int)lroundf(sec / 60.0f));
    return b;
}

// ============================================================================================ names
std::string Cap(std::string s) { if (!s.empty()) s[0] = (char)toupper((unsigned char)s[0]); return s; }
std::vector<std::string> NameParts(int i) {
    std::vector<std::string> parts;
    std::string k = UiStillKey(i), cur;
    for (char c : k) { if (c == '_') { if (!cur.empty()) parts.push_back(Cap(cur)); cur.clear(); } else cur += c; }
    if (!cur.empty()) parts.push_back(Cap(cur));
    return parts;
}
std::string PairName(int i) {
    std::string o;
    for (const std::string& p : NameParts(i)) o += (o.empty() ? "" : " / ") + p;
    return o;
}
std::string KeyDisplay(const std::string& key) {   // a stored name as the window shows it ("sunset_glow" -> "Sunset / Glow")
    std::string o, cur;
    auto put = [&]() { if (!cur.empty()) o += (o.empty() ? "" : " / ") + Cap(cur); cur.clear(); };
    for (char c : key) { if (c == '_') put(); else cur += c; }
    put();
    return o;
}
std::string FaceName(int mode) { return mode ? "darkroom" : "vivid"; }
std::string RatedDisplay(int k) { return Cap(UiRatedName(k)); }
std::string StageDisplay(const std::wstring& name) {
    std::string s = UiNarrow(name);
    for (const char* pre : { "Look - ", "Scheme - " }) if (s.rfind(pre, 0) == 0) s.erase(0, strlen(pre));
    return s;
}
int PairMax(int i) { return std::max(UiStillRating(i, 0), UiStillRating(i, 1)); }

// ============================================================================================ the model
enum { LL_KEEP, LL_VISIT, LL_PAUSE, LL_PAIR, LL_RATED, LL_MORE };
struct ListLine { bool ring = false; std::string what, when; bool ends = false; int kind = 0, idx = -1; };
// a visit chip: a rated look (k), or Ink / Fluid (guest 0 / 1: a guest visit, inactive with the reason in `why` when
// its start preset is missing)
struct Chip { std::string label; int rated = -1; bool inactive = false; int guest = -1; std::string why; };
struct Model {
    bool live = false, autoOn = false, paused = false, director = false, darkroom = false, visiting = false;
    std::string visitName;            // the look visited
    float visitLeft = 0;
    int visitRated = -1;              // a rated look's visit (k), -1 = an Ink / Fluid one
    int visitGuest = -1;              // an Ink (0) / Fluid (1) visit (cycle.h CycleVisitGuest), -1 = none
    bool visitOwnColour = false;      // Ink / Fluid: their own field colour, no pair selected
    Pal pal;
    std::string title, state, railEnd, railWhy;
    bool railHasEnd = false;
    int playing = -1, sel = -1;
    bool keepOn = false;
    float keepLeft = 0;
    std::vector<Chip> chips;
    int chipOn = -1;
    std::vector<ListLine> lines;
    double now = 0;
};

Model BuildModel() {
    Model m;
    m.live = s_h.live;
    m.now = NowSecOfDay();
    const UiNowView nv = UiDirectorNow();
    const UiCycleStatusView cs = UiCycleStatus();
    m.director = nv.director && nv.combo >= 0 && nv.combo < UiStillCount();
    m.autoOn = cs.on || m.director;
    m.paused = s_h.live ? IsManualPaused() : s_h.pausedScripted;
    m.keepLeft = ceilf(UiKeepLeftSec() - 0.5f);   // stage 6: the real keep (cycle hold + the director's), whole s: a steady HH:MM
    m.keepOn = m.keepLeft > 0.0f;
    const UiVisitView vv = UiVisitNow();   // a real visit: a rated look (stage 7) or Ink / Fluid (a guest stage)
    if (vv.on && vv.guest >= 0) {
        m.visitGuest = vv.guest; m.visitName = vv.guest ? "Fluid" : "Ink"; m.visitLeft = ceilf(vv.leftSec - 0.5f);
    } else if (vv.on) {
        m.visitRated = vv.rated; m.visitName = RatedDisplay(vv.rated); m.visitLeft = ceilf(vv.leftSec - 0.5f);
    }
    const float visitLeft = m.visitLeft;
    m.visiting = visitLeft > 0.0f;
    if (m.visiting) { m.keepOn = false; m.keepLeft = 0.0f; }   // a visit replaces a keep (none is offered over it)
    m.playing = m.director ? nv.combo : -1;

    // ---- the colour field
    if (m.visiting) {
        m.title = m.visitName;
        const bool ink = m.visitGuest == 0, fluid = m.visitGuest == 1;
        m.visitOwnColour = ink || fluid;
        if (ink || fluid) m.playing = -1;            // no pair plays during an Ink / Fluid visit: no ring
        m.pal = ink ? InkPal() : fluid ? FluidPal() : m.director ? PairPal(UiStillAnchor(nv.combo), nv.mode == 1) : InkPal();
        m.state = "A visit. Back to automatic in " + std::to_string(std::max(1, (int)lroundf(visitLeft / 60.0f))) + " min.";
        m.railHasEnd = true;
        m.railEnd = Hhmm(m.now + visitLeft);
        m.railWhy = "visit ends, automatic carries on";
    } else if (m.director) {
        m.title = PairName(nv.combo);
        m.darkroom = nv.mode == 1;
        m.pal = PairPal(UiStillAnchor(nv.combo), m.darkroom);
        auto target = [](int c, int md) { return PairName(c) + ", " + FaceName(md); };
        if (nv.changing && !m.keepOn) {
            m.state = nv.toCombo >= 0 ? "Changing to " + target(nv.toCombo, nv.toMode) + "." : "Changing now.";
            m.railWhy = "a change is on screen";
        } else {
            const float left = m.keepOn ? m.keepLeft : nv.holdLeftSec >= 0 ? nv.holdLeftSec : cs.remainingSec;
            m.state = std::string(m.darkroom ? "Darkroom" : "Vivid") + ". Next change " + AboutMin(left) + ".";
            m.railHasEnd = true;
            m.railEnd = Hhmm(m.now + left);
            const std::string then = nv.nextCombo >= 0 ? "then " + target(nv.nextCombo, nv.nextMode) : std::string();
            if (m.keepOn) m.railWhy = then.empty() ? "keep ends, next pair picked at random" : "keep ends, " + then;
            else m.railWhy = then.empty() ? "next pair picked at random" : then;
        }
    } else if (cs.on) {
        const UiStageInfo st = UiCycleStage(cs.stage);
        m.title = StageDisplay(st.name);
        m.pal = st.look == LOOK_F ? FluidPal() : InkPal();
        m.state = "Next change " + AboutMin(cs.remainingSec) + ".";
        m.railHasEnd = true;
        m.railEnd = Hhmm(m.now + cs.remainingSec);
        m.railWhy = "next look picked at random";
    } else {
        const unsigned look = UiCurrentLook();
        m.title = UiLookName(look);
        m.pal = look == LOOK_F ? FluidPal() : InkPal();
        m.state = "Automatic is off. This look stays until you change it.";
    }
    if (m.paused) { m.state = "Paused. The wallpaper is dark until you resume."; m.railHasEnd = false; m.railWhy.clear(); }

    // ---- the visit chips (only while the automatic mode runs: a visit returns to it)
    if (m.autoOn) {
        for (int g = 0; g < 2; g++) {                // Ink / Fluid: a guest visit; off (40 %) when the preset is missing
            Chip c;
            c.label = g ? "Fluid" : "Ink";
            c.guest = g;
            c.inactive = !UiGuestAvailable(g, &c.why);
            m.chips.push_back(c);
        }
        for (int k = 0; k < UiRatedCount(); k++) m.chips.push_back({ RatedDisplay(k), k, false });
        for (int k = 0; k < (int)m.chips.size(); k++)
            if (m.visiting && (m.visitGuest >= 0 ? m.chips[k].guest == m.visitGuest : m.chips[k].rated == m.visitRated))
                m.chipOn = k;
    }

    // ---- the selected pair (by name; an Ink / Fluid visit selects none: no pair plays). Only a listed pair (not a
    // draft, not removed) can be selected: it is the one the scale sets.
    if (!s_h.selName.empty()) m.sel = UiStillFind(s_h.selName);
    if (m.sel >= 0 && !UiPairVisible(m.sel)) m.sel = -1;
    if (m.sel < 0 && !m.visitOwnColour) {
        m.sel = m.playing >= 0 && UiPairVisible(m.playing) ? m.playing : -1;
        for (int i = 0; m.sel < 0 && i < UiStillCount(); i++) if (UiPairVisible(i)) m.sel = i;
    }
    if (m.visitOwnColour && s_h.selName.empty()) m.sel = -1;

    // ---- "Different from automatic": derived from state, every frame (B4)
    if (m.keepOn) {
        ListLine l; l.ring = true; l.ends = true; l.kind = LL_KEEP;
        l.what = "Keeping " + (m.director ? PairName(nv.combo) : m.title);
        l.when = "back to automatic at " + Hhmm(m.now + m.keepLeft);
        m.lines.push_back(l);
    }
    if (m.visiting) {
        ListLine l; l.ring = true; l.ends = true; l.kind = LL_VISIT;
        l.what = "Visiting " + m.visitName;
        l.when = "back to automatic at " + Hhmm(m.now + visitLeft);
        m.lines.push_back(l);
    }
    if (m.paused) {
        ListLine l; l.ring = true; l.kind = LL_PAUSE;
        l.what = "Paused"; l.when = "until you resume";
        m.lines.push_back(l);
    }
    for (int i = 0; i < UiStillCount(); i++)
        if (UiPairVisible(i) && PairMax(i) == 0) { ListLine l; l.kind = LL_PAIR; l.idx = i; l.what = PairName(i); l.when = "never shown"; m.lines.push_back(l); }
    for (int k = 0; k < UiRatedCount(); k++)
        if (UiRatedRating(k) == 0) { ListLine l; l.kind = LL_RATED; l.idx = k; l.what = RatedDisplay(k); l.when = "never shown"; m.lines.push_back(l); }
    return m;
}

// ============================================================================================ actions
void Status(const std::string& s) { s_h.status = s; s_h.statusAt = GetTickCount64(); }
// Pause / Resume: the app's manual pause in a live window; a headless script toggles the scripted state
void DoPause() { if (s_h.live) TogglePause(); else s_h.pausedScripted = !s_h.pausedScripted; }
int OthersOn(int i) { int n = 0; for (int k = 0; k < UiStillCount(); k++) if (k != i && PairMax(k) > 0) n++; return n; }
bool PairNever(int i, int playing) {
    if (i < 0 || PairMax(i) == 0) return true;
    if (OthersOn(i) == 0) { Status("One colour pair has to stay on."); return false; }
    UiSetStillBeforeNever(i, UiStillRating(i, 0), UiStillRating(i, 1));
    UiSetStillRating(i, 0, 0);
    UiSetStillRating(i, 1, 0);
    if (i == playing) { UiKeep(0.0f); UiDirectorSkip(); }   // it is on screen: a keep goes, the next pair comes now
    return true;
}
void PairRestore(int i) {
    int w = 5, p = 5;
    UiStillBeforeNever(i, &w, &p);
    if (w <= 0 && p <= 0) w = p = 5;
    UiSetStillRating(i, 0, w);
    UiSetStillRating(i, 1, p);
    UiClearStillBeforeNever(i);
}
// the scale sets the pair as a whole: equal faces move together, different faces keep their split (UI3-PLAN)
void PairSet(int i, int v, int playing) {
    if (i < 0) return;
    const int r0 = UiStillRating(i, 0), r1 = UiStillRating(i, 1), mx = std::max(r0, r1);
    v = std::min(std::max(v, 0), 10);
    if (v == mx) return;
    if (v == 0) { PairNever(i, playing); return; }
    if (mx == 0) { UiSetStillRating(i, 0, v); UiSetStillRating(i, 1, v); UiClearStillBeforeNever(i); return; }
    if (r0 == r1) { UiSetStillRating(i, 0, v); UiSetStillRating(i, 1, v); return; }
    UiSetStillRating(i, 0, (int)lroundf(r0 * (float)v / (float)mx));
    UiSetStillRating(i, 1, (int)lroundf(r1 * (float)v / (float)mx));
}
void RatedRestore(int k) {
    int v = 2;                                   // a rated look's own default ([looks] missing = 2 of 10)
    UiRatedBeforeNever(k, &v);
    if (v <= 0) v = 2;
    UiSetRatedRating(k, v);
    UiClearRatedBeforeNever(k);
}

// ---- ADD A PAIR (the director side: director.h DirectorAddCombo ...)
std::string AddSuggest() { return UiHueName(s_add.h1) + " / " + UiHueName(s_add.h2); }
float AddOffset() { return Wrap360(s_add.h2 - s_add.h1); }   // DESIGN 5.13: the app keeps an offset
std::string AddDisplay() {                                   // the field, trimmed; empty = the suggestion
    std::string s = s_add.name;
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    return s.empty() ? AddSuggest() : s;
}
bool AddDraftAlive() { return s_add.draft >= 0 && UiPairIsDraft(s_add.draft); }
void AddReset() { const int from = s_add.from; s_add = AddState(); s_add.from = from; }
// open the screen; a draft of an earlier visit that was neither kept nor cancelled is picked up where it was left
void AddOpen(int from) {
    if (!AddDraftAlive()) AddReset();
    s_add.from = (from == SCR_LOOKS) ? SCR_LOOKS : SCR_HOME;
    s_add.grab = 0;
    s_h.status.clear();
    UiHomeSetScreen(SCR_ADD);
}
// the draft for this name and these hues: the session's own when it still matches or may be changed (not on screen,
// not on its way), else a new one (the old draft is cancelled: it finishes what is on screen and is never drawn again)
int AddEnsureDraft() {
    const std::string disp = AddDisplay();
    const float off = AddOffset();
    if (AddDraftAlive()) {
        const int i = s_add.draft;
        const bool same = fabsf(Wrap360(UiStillAnchor(i)) - Wrap360(s_add.h1)) < 0.01f &&
                          fabsf(Wrap360(UiStillOffset(i)) - off) < 0.01f && UiStillKey(i) == UiPairKeyFor(disp, i);
        if (same || UiPairUpdateDraft(i, disp, s_add.h1, off)) return i;
        UiPairCancel(i);
    }
    s_add.draft = UiPairAdd(disp, s_add.h1, off, true);
    return s_add.draft;
}
// where the session's draft is: 2 on screen / on its way, 1 queued (pos = its place), 0 neither
int AddDraftState(int* pos) {
    if (!AddDraftAlive()) return 0;
    const UiNowView nv = UiDirectorNow();
    if (nv.director && (nv.combo == s_add.draft || (nv.changing && nv.toCombo == s_add.draft))) return 2;
    int qi = 0, qm = 0;
    for (int k = 0; UiQueueAt(k, &qi, &qm); k++) if (qi == s_add.draft) { if (pos) *pos = k; return 1; }
    return 0;
}
void AddShowNext() {
    if (AddDraftState(nullptr) == 2) return;                 // it is playing already
    const int i = AddEnsureDraft();
    if (AddDraftState(nullptr) == 0) UiQueueStill(i, 0);     // its vivid face, once
}
void AddCancel() {
    if (AddDraftAlive()) UiPairCancel(s_add.draft);
    const int back = s_add.from;
    AddReset();
    UiHomeSetScreen(back);
}
void AddKeep() {
    const int i = AddEnsureDraft();
    UiPairKeep(i);                                            // commit (ratings 5) + combos.ini / [looks] (not headless)
    s_h.selName = UiStillKey(i);
    s_h.gridToSel = true;
    Status(PairName(i) + " joins the automatic mode at 5 of 10.");
    AddReset();
    UiHomeSetScreen(SCR_HOME);
}

// ============================================================================================ drawing primitives
float S = 1.0f, OX = 0.0f, OY = 0.0f;
ImDrawList* DL = nullptr;
ImVec2 P(float x, float y) { return ImVec2(OX + x * S, OY + y * S); }
ImFont* Fnt(int w) { ImFont* f = w >= 700 ? s_f.b : w >= 600 ? s_f.s : s_f.r; return f ? f : ImGui::GetFont(); }
float PxSize(float css) { return css * s_f.k * S; }
int Utf8Len(const char* c) {                          // bytes in the UTF-8 sequence that starts at c
    const unsigned char b = (unsigned char)*c;
    const int n = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 1;
    for (int k = 1; k < n; k++) if (!c[k]) return k;
    return n;
}
float TextW(int weight, float css, const std::string& t, float track = 0.0f) {
    ImFont* f = Fnt(weight);
    if (track == 0.0f) return f->CalcTextSizeA(PxSize(css), FLT_MAX, 0.0f, t.c_str()).x / S;
    float w = 0;
    for (const char* c = t.c_str(); *c;) {
        const int n = Utf8Len(c);
        w += f->CalcTextSizeA(PxSize(css), FLT_MAX, 0.0f, c, c + n).x / S + track;
        c += n;
    }
    return w;
}
// text in a CSS line box: top of the box, its line height; the glyphs' content area is centred in it
void Text(int weight, float css, float lh, float x, float top, ImU32 col, const std::string& t, float track = 0.0f) {
    ImFont* f = Fnt(weight);
    const float y = top + (lh - s_f.k * css) * 0.5f;
    if (track == 0.0f) { DL->AddText(f, PxSize(css), P(x, y), col, t.c_str()); return; }
    for (const char* c = t.c_str(); *c;) {
        const int n = Utf8Len(c);
        DL->AddText(f, PxSize(css), P(x, y), col, c, c + n);
        x += f->CalcTextSizeA(PxSize(css), FLT_MAX, 0.0f, c, c + n).x / S + track;
        c += n;
    }
}
void TextC(int weight, float css, float lh, float cx, float top, ImU32 col, const std::string& t) {
    Text(weight, css, lh, cx - TextW(weight, css, t) * 0.5f, top, col, t);
}
void TextR(int weight, float css, float lh, float rx, float top, ImU32 col, const std::string& t, float track = 0.0f) {
    Text(weight, css, lh, rx - TextW(weight, css, t, track), top, col, t, track);
}
void Rect(float x, float y, float w, float h, ImU32 c, float r = 0) {
    ImVec2 a = P(x, y), b = P(x + w, y + h);
    // a hairline stays one device pixel when the window is smaller than the design (no AA on filled rects)
    if (b.y - a.y < 1.0f) { a.y = floorf(a.y); b.y = a.y + 1.0f; }
    if (b.x - a.x < 1.0f) { a.x = floorf(a.x); b.x = a.x + 1.0f; }
    DL->AddRectFilled(a, b, c, r * S);
}
void RectLine(float x, float y, float w, float h, ImU32 c, float r, float th) {
    // the stroke inside the box, as CSS border-box draws it
    DL->AddRect(P(x + th * 0.5f, y + th * 0.5f), P(x + w - th * 0.5f, y + h - th * 0.5f), c, (r - th * 0.5f) * S, 0, th * S);
}
void Disc(float cx, float cy, float r, ImU32 c) { DL->AddCircleFilled(P(cx, cy), r * S, c, 0); }
void Ring(float cx, float cy, float r, ImU32 c, float th) { DL->AddCircle(P(cx, cy), r * S, c, 0, th * S); }
void HalfDisc(float cx, float cy, float r, ImU32 c, bool left) {
    const float pi = 3.14159265f;
    DL->PathArcTo(P(cx, cy), r * S, left ? pi * 0.5f : -pi * 0.5f, left ? pi * 1.5f : pi * 0.5f, 48);
    DL->PathFillConvex(c);
}
void Poly(float ox, float oy, std::initializer_list<ImVec2> pts, ImU32 c) {
    std::vector<ImVec2> v;
    for (const ImVec2& p : pts) v.push_back(P(ox + p.x, oy + p.y));
    DL->AddConvexPolyFilled(v.data(), (int)v.size(), c);
}
void Line(float x0, float y0, float x1, float y1, ImU32 c, float th) { DL->AddLine(P(x0, y0), P(x1, y1), c, th * S); }
void Chevron(float x, float y, ImU32 c, bool left = false) {    // 8 x 12, 2 px stroke
    ImVec2 p[3] = { P(x + (left ? 6.5f : 1.5f), y + 1.5f), P(x + (left ? 2.0f : 6.0f), y + 6.0f), P(x + (left ? 6.5f : 1.5f), y + 10.5f) };
    DL->AddPolyline(p, 3, c, ImDrawFlags_None, 2.0f * S);
}
void Cross(float cx, float cy, ImU32 c) {                       // 10 x 10, 1.6 stroke
    Line(cx - 4, cy - 4, cx + 4, cy + 4, c, 1.6f);
    Line(cx + 4, cy - 4, cx - 4, cy + 4, c, 1.6f);
}
// a hit area (design px); returns clicked; hovered / held out
bool Hit(const char* id, float x, float y, float w, float h, bool* hov = nullptr, bool* held = nullptr) {
    ImGui::SetCursorScreenPos(P(x, y));
    const bool c = ImGui::InvisibleButton(id, ImVec2(std::max(w * S, 1.0f), std::max(h * S, 1.0f)));
    const bool hv = ImGui::IsItemHovered();
    if (hv) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (hov) *hov = hv;
    if (held) *held = ImGui::IsItemActive();
    return c && (s_h.live || s_h.acts);
}
void ImageOf(ID3D11ShaderResourceView* srv, int iw, int ih, float x, float y, float w, float h, float r) {
    if (!srv || iw <= 0 || ih <= 0) return;
    const float ia = (float)iw / (float)ih, ba = w / h;
    ImVec2 uv0(0, 0), uv1(1, 1);
    if (ia > ba) { const float u = ba / ia; uv0.x = 0.5f - u * 0.5f; uv1.x = 0.5f + u * 0.5f; }   // cover: crop the sides
    else { const float v = ia / ba; uv0.y = 0.5f - v * 0.5f; uv1.y = 0.5f + v * 0.5f; }           // or top and bottom
    DL->AddImageRounded(ImTextureRef((ImTextureID)(intptr_t)srv), P(x, y), P(x + w, y + h), uv0, uv1,
                        IM_COL32_WHITE, r * S);
}
void Image(float x, float y, float w, float h, float r) { ImageOf(s_h.nowImg.Get(), s_h.imgW, s_h.imgH, x, y, w, h, r); }

// ============================================================================================ the picture of now (B5)
// While the window shows HOME or ALL LOOKS, the renderer is asked for a snapshot of the real wallpaper (fluid.h
// RequestSnapshot: the presented frame after post, boxed down to 592 x 333, sRGB) when the window opens and then
// every 10 s; it arrives a frame or two later (TakeSnapshot, on the frame's fence) and is drawn where the
// placeholder was. A paused wallpaper draws nothing: the request waits and the last picture stays. The last
// picture of each pair + face is kept in memory for the session (half size) for ALL LOOKS' "what is playing" tile.
// Headless (--ui-shot, --ui-live) there is no renderer: none of this runs, the --ui-now-image picture is used.
struct Tex2 { ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11ShaderResourceView> srv; int w = 0, h = 0; };
struct Pic { std::vector<uint8_t> rgba; int w = 0, h = 0; };
struct SnapState {
    ComPtr<ID3D11Device> dev;          // the window's D3D11 device (UiHomeDeviceCreated)
    Tex2 now;                          // the latest snapshot (HOME's still)
    Tex2 tile;                         // a kept pair + face picture (ALL LOOKS' tile; HOME until the first snapshot)
    Pic latest;                        // the latest snapshot on the CPU (re-uploaded after a UI device rebuild)
    bool haveLatest = false;           // a snapshot arrived since the window opened
    ULONGLONG askedAt = 0;             // 0 = ask on the next HOME / ALL LOOKS frame
    std::string askTag;                // the pair + face on screen when asked ("" = none, or a change on screen)
    struct Kept { Pic pic; unsigned serial = 0; };
    std::map<std::string, Kept> kept;  // "<pair>|<face>" -> its last picture, this session
    unsigned serial = 0;
    std::string tileKey;               // what `tile` holds
    unsigned tileSerial = 0;
};
SnapState s_snap;
const ULONGLONG kSnapEveryMs = 10000;

std::string PairFaceKey(int combo, int mode) { return UiStillKey(combo) + (mode ? "|darkroom" : "|vivid"); }
// the pair + face a snapshot taken now shows; "" when that is not one steady pair (no colour director, a change or a
// stage transition on screen, a visit to a rated look)
std::string SnapTag() {
    const UiNowView nv = UiDirectorNow();
    if (!nv.director || nv.changing || nv.combo < 0 || nv.combo >= UiStillCount()) return "";
    if (UiCycleStatus().transitioning || UiVisitNow().on) return "";
    return PairFaceKey(nv.combo, nv.mode);
}
bool UploadPic(Tex2& t, const Pic& p) {
    if (!s_snap.dev || p.w <= 0 || p.h <= 0 || p.rgba.size() < (size_t)p.w * p.h * 4) return false;
    if (t.tex && t.w == p.w && t.h == p.h) {   // made once at this size: only the pixels change
        ComPtr<ID3D11DeviceContext> ctx;
        s_snap.dev->GetImmediateContext(&ctx);
        ctx->UpdateSubresource(t.tex.Get(), 0, nullptr, p.rgba.data(), (UINT)p.w * 4, 0);
        return true;
    }
    t = Tex2();
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)p.w; td.Height = (UINT)p.h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;   // sRGB bytes, as LoadImageFile's PNG
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = { p.rgba.data(), (UINT)p.w * 4, 0 };
    if (FAILED(s_snap.dev->CreateTexture2D(&td, &sd, &t.tex)) ||
        FAILED(s_snap.dev->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv))) { t = Tex2(); return false; }
    t.w = p.w; t.h = p.h;
    return true;
}
// a kept picture at half size (2 x 2 box in linear light): a session of every pair and face stays a few MB
Pic HalfPic(const Pic& p) {
    static float dec[256];
    static bool made = false;
    if (!made) { for (int k = 0; k < 256; k++) dec[k] = Lin(k / 255.0f); made = true; }
    Pic o;
    o.w = std::max(1, p.w / 2); o.h = std::max(1, p.h / 2);
    o.rgba.resize((size_t)o.w * o.h * 4);
    for (int y = 0; y < o.h; y++)
        for (int x = 0; x < o.w; x++) {
            const int x0 = std::min(2 * x, p.w - 1), x1 = std::min(2 * x + 1, p.w - 1);
            const int y0 = std::min(2 * y, p.h - 1), y1 = std::min(2 * y + 1, p.h - 1);
            uint8_t* d = o.rgba.data() + ((size_t)y * o.w + x) * 4;
            for (int c = 0; c < 3; c++) {
                const float l = 0.25f * (dec[p.rgba[((size_t)y0 * p.w + x0) * 4 + c]] + dec[p.rgba[((size_t)y0 * p.w + x1) * 4 + c]] +
                                         dec[p.rgba[((size_t)y1 * p.w + x0) * 4 + c]] + dec[p.rgba[((size_t)y1 * p.w + x1) * 4 + c]]);
                const float s = l <= 0.0031308f ? l * 12.92f : 1.055f * powf(l, 1.0f / 2.4f) - 0.055f;
                d[c] = (uint8_t)std::min(255, std::max(0, (int)(s * 255.0f + 0.5f)));
            }
            d[3] = 255;
        }
    return o;
}
// once per HOME / ALL LOOKS frame of the live window: take a picture that landed, ask for the next one
void SnapTick() {
    if (!g_renderer || !s_h.live || !s_snap.dev) return;
    Pic p;
    if (g_renderer->TakeSnapshot(p.rgba, p.w, p.h)) {
        const std::string tag = SnapTag();
        s_snap.latest = std::move(p);
        s_snap.haveLatest = UploadPic(s_snap.now, s_snap.latest);
        // kept under its pair + face only when the same steady pair was on screen when asked and when it landed
        if (!tag.empty() && tag == s_snap.askTag) s_snap.kept[tag] = { HalfPic(s_snap.latest), ++s_snap.serial };
    }
    const ULONGLONG now = GetTickCount64();
    if (s_snap.askedAt == 0 || now - s_snap.askedAt >= kSnapEveryMs) {
        g_renderer->RequestSnapshot();   // idempotent while one is pending (a paused wallpaper: it waits)
        s_snap.askedAt = now;
        s_snap.askTag = SnapTag();
    }
}
// the kept picture of this pair + face, in `tile` (uploaded when it changed); null = none this session
const Tex2* KeptPic(const std::string& key) {
    if (key.empty()) return nullptr;
    const auto it = s_snap.kept.find(key);
    if (it == s_snap.kept.end()) return nullptr;
    if (s_snap.tileKey != key || s_snap.tileSerial != it->second.serial || !s_snap.tile.srv) {
        if (!UploadPic(s_snap.tile, it->second.pic)) { s_snap.tileKey.clear(); return nullptr; }
        s_snap.tileKey = key;
        s_snap.tileSerial = it->second.serial;
    }
    return &s_snap.tile;
}
void SnapReset() {   // window opened / closed: the next HOME frame asks at once; no picture of this opening yet
    s_snap.askedAt = 0;
    s_snap.askTag.clear();
    s_snap.haveLatest = false;
    s_snap.latest = Pic();
}
// what is playing as a kept-picture key: the director's pair + face ("" = no pair plays: another look, a visit)
std::string PlayingKey(const Model& m) {
    if (!m.director || m.visiting) return "";
    const UiNowView nv = UiDirectorNow();
    return PairFaceKey(nv.combo, nv.mode);
}
// HOME's still: the latest snapshot; until the first one of this opening, the kept one of the pair + face playing
const Tex2* HomePic(const Model& m) {
    if (s_snap.haveLatest && s_snap.now.srv) return &s_snap.now;
    return KeptPic(PlayingKey(m));
}
// ALL LOOKS' "what is playing" tile: the kept picture of the pair + face playing; no pair plays: the latest snapshot
const Tex2* TilePic(const Model& m) {
    const std::string k = PlayingKey(m);
    if (!k.empty()) return KeptPic(k);
    return s_snap.haveLatest && s_snap.now.srv ? &s_snap.now : nullptr;
}

// ============================================================================================ HOME: the colour field
void DrawColourField(const Model& m) {
    const Pal& pl = m.pal;
    const ImU32 strong = U(pl.strong), quiet = U(pl.quiet);
    // the still: 592 x 333 (16:9), radius 22. The snapshot of the wallpaper (B5); until it lands: the --ui-now-image
    // picture headless, else a flat darker shade of the field
    const float stX = 48, stY = 48, stW = 592, stH = 333;
    if (const Tex2* sp = HomePic(m)) {
        Rect(stX, stY, stW, stH, IM_COL32(0, 0, 0, 255), 22);
        ImageOf(sp->srv.Get(), sp->w, sp->h, stX, stY, stW, stH, 22);
    } else if (s_h.nowImg) {
        Rect(stX, stY, stW, stH, IM_COL32(0, 0, 0, 255), 22);
        Image(stX, stY, stW, stH, 22);
    } else {
        Rect(stX, stY, stW, stH, U(Mul(pl.field, 0.55f)), 22);
    }
    if (m.paused) {
        Rect(stX, stY, stW, stH, IM_COL32(0, 0, 0, 190), 22);
        TextC(600, 19, 27, stX + stW * 0.5f, stY + stH * 0.5f - 13, U(kWhite), "Paused");
    }

    // the time rail (y 395, 20 high)
    const float ry = 395;
    const std::string t1 = "now " + Hhmm(m.now);
    Text(600, 15, 20, 48, ry, strong, t1);
    const float runX0 = 48 + TextW(600, 15, t1) + 12;
    if (m.railHasEnd) {
        const float wEnd = TextW(600, 15, m.railEnd), wWhy = m.railWhy.empty() ? 0.0f : TextW(400, 15, m.railWhy) + 8;
        const float runX1 = 640 - wWhy - wEnd - 12;
        if (runX1 > runX0 + 16) {
            Rect(runX0, ry + 9, runX1 - runX0, 2, strong);
            Disc(runX0 + 4, ry + 10, 4, strong);
            Rect(runX1 - 2, ry + 4, 2, 12, strong);
        }
        Text(600, 15, 20, 640 - wWhy - wEnd, ry, strong, m.railEnd);
        if (!m.railWhy.empty()) Text(400, 15, 20, 640 - wWhy + 8, ry, quiet, m.railWhy);
    } else if (!m.railWhy.empty()) {
        Text(400, 15, 20, runX0, ry, quiet, m.railWhy);
    }

    // the pair's name (72 / 72 bold, -2.5 tracking; shrinks to fit 592)
    float tSize = 72.0f;
    const float tw = TextW(700, tSize, m.title, -2.5f);
    if (tw > 592.0f) tSize = std::max(36.0f, tSize * 592.0f / tw);
    Text(700, tSize, 72, 48, 435, strong, m.title, -2.5f * tSize / 72.0f);
    Text(400, 19, 27, 48, 517, strong, m.state);

    // the ring key line (y 562): ring d 13, 2.5 stroke, 11 px before the sentence
    Ring(48 + 6.5f, 562 + 11, 5.25f, strong, 2.5f);
    Text(600, 17, 22, 72, 562, strong, "These end by themselves. Automatic carries on after.", -0.1f);

    // the round buttons (y 600, balls 64)
    const float by = 600, bc = by + 32;
    auto nameBlock = [&](float x, const std::string& name, const std::string& sub, bool drain, float drainFrac) {
        const float hName = 23, hSub = sub.empty() ? 0.0f : 3 + 18, hDrain = drain ? 7 + 6 : 0.0f;
        const float top = bc - (hName + hSub + hDrain) * 0.5f;
        Text(600, 21, 23, x, top, strong, name, -0.2f);
        float w = TextW(600, 21, name, -0.2f);
        if (!sub.empty()) { Text(400, 15, 18, x, top + 26, strong, sub); w = std::max(w, TextW(400, 15, sub)); }
        if (drain) {
            const float dy = top + hName + hSub + 7;
            Rect(x, dy, 116, 6, U(pl.drain), 3);
            Rect(x, dy, std::max(6.0f, 116 * drainFrac), 6, strong, 3);
            w = std::max(w, 116.0f);
        }
        return w;
    };
    bool hv = false;
    if (m.visiting) {
        // a visit (g-visit.png): "Back to automatic now" with the visit's draining bar, then Pause; no Next, no Keep
        const float bx = 48 + 7;                                    // room for the on-ring (7 px outside the ball)
        const std::string bName = "Back to automatic now", bSub = "the visit ends by itself at " + Hhmm(m.now + m.visitLeft);
        const float wB = std::max({ TextW(600, 21, bName, -0.2f), TextW(400, 15, bSub), 116.0f });
        if (Hit("##back", bx - 7, by, 64 + 14 + 7 + wB, 64, &hv)) UiVisitEnd();   // a rated look's visit or Ink / Fluid's
        Ring(bx + 32, bc, 37.5f, strong, 3.0f);
        Disc(bx + 32, bc, 32, hv ? U(kRaised) : IM_COL32(0, 0, 0, 255));
        // arc r 9 from the top clockwise 270 deg to the left, round caps, a triangle head at its start (DESIGN 5.1)
        DL->PathArcTo(P(bx + 32, bc + 0.5f), 9 * S, -3.14159265f * 0.5f, 3.14159265f, 40);
        DL->PathStroke(U(kWhite), ImDrawFlags_None, 3.4f * S);
        Disc(bx + 32 - 9, bc + 0.5f, 1.7f, U(kWhite));               // the round cap at the arc's end
        Poly(bx + 32, bc, { ImVec2(-6, -8.5f), ImVec2(1, -13.5f), ImVec2(1, -3.5f) }, U(kWhite));
        nameBlock(bx + 64 + 14, bName, bSub, true, m.visitLeft / 1800.0f);
        const float px = bx + 64 + 14 + wB + 36;
        const std::string pauseName = m.paused ? "Resume" : "Pause";
        if (Hit("##pause", px, by, 64 + 14 + TextW(600, 21, pauseName, -0.2f), 64, &hv)) DoPause();
        if (m.paused) Ring(px + 32, bc, 37.5f, strong, 3.0f);
        Disc(px + 32, bc, 32, hv ? U(kRaised) : IM_COL32(0, 0, 0, 255));
        if (m.paused) Poly(px + 32 - 8, bc - 11, { ImVec2(0, 0), ImVec2(20, 11), ImVec2(0, 22) }, U(kWhite));
        else {
            Rect(px + 32 - 10, bc - 11, 7, 22, U(kWhite), 1.5f);
            Rect(px + 32 + 3, bc - 11, 7, 22, U(kWhite), 1.5f);
        }
        nameBlock(px + 64 + 14, pauseName, "", false, 0);
    } else {
        // Next (or Start automatic while the automatic mode is off); Next ends a keep (CycleNext)
        const std::string nextName = m.autoOn ? "Next" : "Start automatic";
        const float wNext = TextW(600, 21, nextName, -0.2f);
        if (Hit("##next", 48, by, 64 + 14 + wNext, 64, &hv)) {
            if (m.autoOn) UiCycleNext(); else UiCycleSetEnabled(true);
        }
        Disc(80, bc, 32, hv ? U(kRaised) : IM_COL32(0, 0, 0, 255));
        Poly(80 - 13, bc - 11, { ImVec2(0, 0), ImVec2(12, 11), ImVec2(0, 22) }, U(kWhite));
        Poly(80 - 13, bc - 11, { ImVec2(13, 0), ImVec2(25, 11), ImVec2(13, 22) }, U(kWhite));
        nameBlock(48 + 64 + 14, nextName, "", false, 0);
        // Keep this: 30 min of what is on screen (the stage timer and the colour director wait); a second press lets go.
        // Not offered while the automatic mode is off (nothing would end to keep it from).
        const float kx = 48 + 64 + 14 + wNext + 36 + 6;
        const bool keepShown = m.autoOn || m.keepOn;
        const std::string keepName = m.keepOn ? "Keeping this" : "Keep this";
        const std::string keepSub = m.keepOn ? "until " + Hhmm(m.now + m.keepLeft) : "for 30 min";
        const float wKeep = std::max({ TextW(600, 21, keepName, -0.2f), TextW(400, 15, keepSub), m.keepOn ? 116.0f : 0.0f });
        if (keepShown) {
            if (Hit("##keep", kx, by, 64 + 14 + wKeep, 64, &hv)) UiKeep(m.keepOn ? 0.0f : 1800.0f);
            if (m.keepOn) Ring(kx + 32, bc, 37.5f, strong, 3.0f);
            Disc(kx + 32, bc, 32, hv ? U(kRaised) : IM_COL32(0, 0, 0, 255));
            Poly(kx + 32 - 10, bc - 13, { ImVec2(0, 0), ImVec2(20, 0), ImVec2(11.5f, 13), ImVec2(8.5f, 13) }, U(kWhite));
            Poly(kx + 32 - 10, bc - 13, { ImVec2(8.5f, 13), ImVec2(11.5f, 13), ImVec2(20, 26), ImVec2(0, 26) }, U(kWhite));
            nameBlock(kx + 64 + 14, keepName, keepSub, m.keepOn, m.keepLeft / 1800.0f);
        }
        // Pause / Resume
        const float px = keepShown ? kx + 64 + 14 + wKeep + 36 : 48 + 64 + 14 + wNext + 36;
        const std::string pauseName = m.paused ? "Resume" : "Pause";
        if (Hit("##pause", px, by, 64 + 14 + TextW(600, 21, pauseName, -0.2f), 64, &hv)) DoPause();
        if (m.paused) Ring(px + 32, bc, 37.5f, strong, 3.0f);
        Disc(px + 32, bc, 32, hv ? U(kRaised) : IM_COL32(0, 0, 0, 255));
        if (m.paused) Poly(px + 32 - 8, bc - 11, { ImVec2(0, 0), ImVec2(20, 11), ImVec2(0, 22) }, U(kWhite));
        else {
            Rect(px + 32 - 10, bc - 11, 7, 22, U(kWhite), 1.5f);
            Rect(px + 32 + 3, bc - 11, 7, 22, U(kWhite), 1.5f);
        }
        nameBlock(px + 64 + 14, pauseName, "", false, 0);
    }

    // visit another look (y 688): one row of chips; the ones that do not fit become a last chip "More"
    if (!m.chips.empty()) {
        Text(600, 15, 20, 48, 688, strong, m.chipOn >= 0 ? "Or visit another look instead, for 30 min" : "Or visit another look for 30 min");
        std::vector<float> w;
        for (const Chip& c : m.chips) w.push_back(TextW(600, 15, c.label) + 28);
        const float moreW = TextW(600, 15, "More") + 28;
        int shown = (int)m.chips.size();
        float total = 0;
        for (int k = 0; k < shown; k++) total += w[k] + (k ? 8 : 0);
        if (total > 592) {
            total = moreW;
            shown = 0;
            while (shown < (int)w.size() && total + w[shown] + 8 <= 592) { total += w[shown] + 8; shown++; }
        }
        float x = 48;
        // id 99 = More (opens All looks, where every look can be shown next). An inactive chip (Ink / Fluid whose start
        // preset is missing) is drawn at 40 % with no hit area, its reason in a tooltip; the chip being visited is filled.
        auto chip = [&](int id, const Chip& c, float cw, bool on) {
            bool h = false;
            char hid[32];
            snprintf(hid, sizeof(hid), "##chip%d", id);
            const bool inactive = c.inactive && !on;
            if (!inactive && Hit(hid, x, 716, cw, 36, &h)) {
                if (id == 99) UiHomeSetScreen(SCR_LOOKS);
                else if (c.rated >= 0 && !on) UiVisit(c.rated, 1800.0f);
                else if (c.guest >= 0 && !on) UiVisitGuest(c.guest, 1800.0f);   // Ink / Fluid: the guest visit
            }
            if (inactive && !c.why.empty() && ImGui::IsWindowHovered() &&
                ImGui::IsMouseHoveringRect(P(x, 716), P(x + cw, 716 + 36), false))
                ImGui::SetTooltip("%s", c.why.c_str());
            const float a = inactive ? 0.4f : 1.0f;
            if (on) Rect(x, 716, cw, 36, strong, 18);
            else if (h) Rect(x, 716, cw, 36, U(pl.strong, 0.14f), 18);
            if (!on) RectLine(x, 716, cw, 36, U(pl.strong, a), 18, 2);
            Text(600, 15, 20, x + 14, 724, on ? U(pl.field) : U(pl.strong, a), c.label);
            x += cw + 8;
        };
        for (int k = 0; k < shown; k++) chip(k, m.chips[k], w[k], k == m.chipOn);
        if (shown < (int)m.chips.size()) chip(99, Chip{ "More", -1, false }, moreW, false);
    }
}

// ============================================================================================ HOME: the black field
void DrawKeptField(const Model& m, float fieldX) {
    const ImU32 white = U(kWhite), ash = U(kAsh);
    const float x0 = fieldX + 48, cw = 496;
    Rect(x0, 54, 10, 10, white);
    Text(600, 17, 22, x0 + 21, 48, white, "These stay, on every look, until you change them.", -0.1f);
    Text(400, 15, 20, x0, 100, ash, "Colour pairs, and how often each comes up");

    // ---- the swatch grid: 4 x 115, gaps 12 / 6, 2 rows visible; more rows scroll (wheel) with a 4 px thumb. The
    // listed pairs only (a draft or a removed pair has no tile); the list may grow while the window is open.
    std::vector<int> vis;
    for (int i = 0; i < UiStillCount(); i++) if (UiPairVisible(i)) vis.push_back(i);
    const int n = (int)vis.size(), tiles = n + 1, cols = 4;
    const int rows = (tiles + cols - 1) / cols, visRows = 2;
    if (s_h.gridToSel) {                                        // the selected tile scrolled into view (open, a keep)
        for (int t = 0; t < n; t++)
            if (vis[t] == m.sel) {
                const int r = t / cols;
                if (r < s_h.gridRow) s_h.gridRow = r;
                if (r >= s_h.gridRow + visRows) s_h.gridRow = r - visRows + 1;
            }
        s_h.gridToSel = false;
    }
    s_h.gridRow = std::min(std::max(s_h.gridRow, 0), std::max(rows - visRows, 0));
    const float gy = 130;
    for (int t = 0; t < tiles; t++) {
        const int r = t / cols - s_h.gridRow, c = t % cols;
        if (r < 0 || r >= visRows) continue;
        const float tx = x0 + c * 127, ty = gy + r * 126;
        const float dcx = tx + 57.5f, dcy = ty + 36;
        bool hv = false;
        char id[32];
        snprintf(id, sizeof(id), "##sw%d", t);
        if (t == n) {                                           // the Add a pair tile: opens ADD A PAIR
            if (Hit(id, tx, ty, 115, 120, &hv)) AddOpen(SCR_HOME);
            if (hv) RectLine(tx, ty, 115, 120, U(kEdge), 18, 2);
            Ring(dcx, dcy, 21, U(kEdge), 2);
            Line(dcx - 7, dcy, dcx + 7, dcy, white, 2.2f);
            Line(dcx, dcy - 7, dcx, dcy + 7, white, 2.2f);
            TextC(600, 14, 17, dcx, ty + 68, white, "Add");
            TextC(600, 14, 17, dcx, ty + 85, white, "a pair");
            continue;
        }
        const int p = vis[t];                                   // the pair's index in the director's list
        if (Hit(id, tx, ty, 115, 120, &hv)) { s_h.selName = UiStillKey(p); s_h.status.clear(); }
        const bool sel = p == m.sel, never = PairMax(p) == 0;
        if (sel) RectLine(tx, ty, 115, 120, white, 18, 2);
        else if (hv) RectLine(tx, ty, 115, 120, U(kEdge), 18, 2);
        Rgb c1 = HueColour(UiStillAnchor(p)), c2 = HueColour(UiStillAnchor(p) + UiStillOffset(p));
        if (never) { c1 = Mul(c1, 0.25f); c2 = Mul(c2, 0.25f); }
        HalfDisc(dcx, dcy, 22, U(c1), true);
        HalfDisc(dcx, dcy, 22, U(c2), false);
        if (p == m.playing) Ring(dcx, dcy, 27.5f, U(m.pal.mark), 3);
        const std::vector<std::string> parts = NameParts(p);
        std::string l1 = parts.empty() ? "?" : parts[0], l2;
        for (size_t k = 1; k < parts.size(); k++) l2 += (l2.empty() ? "" : " / ") + parts[k];
        if (!l2.empty()) l1 += " /";
        TextC(600, 14, 17, dcx, ty + 68, white, l1);
        if (!l2.empty()) TextC(600, 14, 17, dcx, ty + 85, white, l2);
        if (never) TextC(400, 13, 15, dcx, ty + 104, ash, "never");
        else {
            const int v = PairMax(p);
            for (int k = 0; k < 10; k++) Rect(dcx - 43.5f + k * 9, ty + 108, 6, 6, k < v ? white : U(kPip), 1.5f);
        }
    }
    if (rows > visRows) {
        const float gh = visRows * 126 - 6;
        // the wheel anywhere over the grid (a hit area here would lose the hover to the tiles under it)
        if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(P(x0, gy), P(x0 + cw + 12, gy + gh), false) &&
            ImGui::GetIO().MouseWheel != 0)
            s_h.gridRow -= ImGui::GetIO().MouseWheel > 0 ? 1 : -1;
        s_h.gridRow = std::min(std::max(s_h.gridRow, 0), std::max(rows - visRows, 0));
        const float th = gh * visRows / rows, ty = gy + gh * s_h.gridRow / rows;
        Rect(x0 + cw + 8, ty, 4, th, U(kEdge), 2);
    }

    // ---- one scale for the selected pair (y 394 head, 438 slider)
    const float hy = 394;
    if (m.sel >= 0) {
        const std::string nm = PairName(m.sel);
        Text(400, 15, 20, x0, hy + 7, ash, "How often " + nm + " comes up");
        const bool never = PairMax(m.sel) == 0;
        const std::string pill = never ? "Show it again" : "Never show this";
        const float pw = TextW(600, 15, pill) + 36;
        bool hv = false;
        if (Hit("##never", x0 + cw - pw, hy, pw, 34, &hv)) {
            if (never) PairRestore(m.sel); else PairNever(m.sel, m.playing);
        }
        RectLine(x0 + cw - pw, hy, pw, 34, hv ? white : U(kEdge), 17, 2);
        Text(600, 15, 20, x0 + cw - pw + 18, hy + 7, white, pill);
        // Less  [track 340, 11 ticks, knob]  More  value
        const float sy = 438, wLess = TextW(400, 15, "Less"), tx = x0 + wLess + 16;
        Text(400, 15, 20, x0, sy + 2, white, "Less");
        bool held = false;
        Hit("##scale", tx - 12, sy, 340 + 24, 24, &hv, &held);
        if (held && (s_h.live || s_h.acts)) {
            const float mx = (ImGui::GetIO().MousePos.x - OX) / S;
            PairSet(m.sel, (int)lroundf((mx - tx) / 34.0f), m.playing);
        }
        Rect(tx, sy + 10, 340, 4, U(kTrack), 2);
        for (int k = 0; k <= 10; k++) Disc(tx + 34 * k, sy + 12, 3, U(kTrack));
        Disc(tx + 34 * PairMax(m.sel), sy + 12, 12, white);
        Text(400, 15, 20, tx + 340 + 16, sy + 2, white, "More");
        char vb[8];
        snprintf(vb, sizeof(vb), "%d", PairMax(m.sel));
        TextR(400, 15, 20, tx + 340 + 16 + TextW(400, 15, "More") + 16 + 20, sy + 2, ash, vb);   // right in a 20 px box
    } else if (m.visitOwnColour) {
        Text(400, 15, 20, x0, hy + 7, ash, "How often " + m.visitName + " comes up");
        Text(700, 17, 22, x0, hy + 32, white, "Only when you visit it.");
        Text(400, 15, 20, x0, hy + 58, ash, "Ink and Fluid are never part of the automatic mode. Pick a pair above to");
        Text(400, 15, 20, x0, hy + 78, ash, "set how often it comes up.");
    }
    if (!s_h.status.empty() && (!s_h.live || GetTickCount64() - s_h.statusAt < 5000))
        Text(400, 13, 16, x0, 468, U(m.pal.mark), s_h.status);

    // ---- "Different from automatic" (y 498), rows of 46 from 534, as many as fit above the links
    Text(700, 21, 26, x0, 498, white, "Different from automatic", -0.3f);
    Rect(x0, 534, cw, 1, U(kLine));
    const int fit = 3;                                          // 534 + 3 x 47 = 675 < the links at 712
    std::vector<ListLine> lines = m.lines;
    if ((int)lines.size() > fit) {
        ListLine more;
        more.kind = LL_MORE;
        more.what = "and " + std::to_string((int)lines.size() - (fit - 1)) + " more";
        lines.resize(fit - 1);
        lines.push_back(more);
    }
    if (lines.empty()) {
        Text(400, 15, 20, x0, 535 + 13, ash, "Nothing. It is all automatic.");
        Rect(x0, 535 + 46, cw, 1, U(kLine));
    }
    for (size_t k = 0; k < lines.size(); k++) {
        const ListLine& l = lines[k];
        const float ly = 535 + 47.0f * k, cy = ly + 23;
        if (l.kind == LL_MORE) {
            Text(400, 15, 20, x0, cy - 10, ash, l.what);
        } else {
            if (l.ring) Ring(x0 + 6.5f, cy, 5.25f, U(m.pal.mark), 2.5f);
            else Rect(x0 + 1.5f, cy - 5, 10, 10, white);
            Text(400, 15, 20, x0 + 27, cy - 10, white, l.what);
            bool hv = false;
            char id[32];
            snprintf(id, sizeof(id), "##x%d", (int)k);
            if (Hit(id, x0 + cw - 26, cy - 13, 26, 26, &hv)) {
                switch (l.kind) {
                case LL_KEEP:  UiKeep(0.0f); break;
                case LL_VISIT: UiVisitEnd(); break;
                case LL_PAUSE: DoPause(); break;
                case LL_PAIR:  PairRestore(l.idx); break;
                case LL_RATED: RatedRestore(l.idx); break;
                }
            }

            Ring(x0 + cw - 13, cy, 12.25f, hv ? white : U(kEdge), 1.5f);
            Cross(x0 + cw - 13, cy, white);
            if (l.ends) TextR(600, 15, 20, x0 + cw - 26 - 16, cy - 10, U(m.pal.mark), l.when);
            else TextR(400, 15, 20, x0 + cw - 26 - 16, cy - 10, ash, l.when);
        }
        Rect(x0, ly + 46, cw, 1, U(kLine));
    }

    // ---- three ways deeper (y 712, 40 high): flex 1 1 auto over 496 with 12 between
    const char* links[3] = { "All looks", "Display and system", "Advanced" };
    const int dest[3] = { SCR_LOOKS, SCR_DISPLAY, SCR_ADVANCED };
    float basis[3], sum = 0;
    for (int k = 0; k < 3; k++) { basis[k] = 2 + 18 + TextW(600, 15, links[k]) + 14 + 8 + 18 + 2; sum += basis[k]; }
    const float extra = std::max(0.0f, (cw - 24 - sum) / 3.0f);
    float lx = x0;
    for (int k = 0; k < 3; k++) {
        const float w = basis[k] + extra;
        bool hv = false;
        char id[32];
        snprintf(id, sizeof(id), "##link%d", k);
        if (Hit(id, lx, 712, w, 40, &hv)) UiHomeSetScreen(dest[k]);
        RectLine(lx, 712, w, 40, hv ? white : U(kEdge), 20, 2);
        Text(600, 15, 20, lx + 20, 722, white, links[k]);
        Chevron(lx + w - 20 - 8, 726, white);
        lx += w + 12;
    }
}

// ============================================================================================ shared pieces (stage 9)
// word wrap at maxW design px (greedy, on spaces)
std::vector<std::string> Wrap(int weight, float css, const std::string& t, float maxW) {
    std::vector<std::string> out;
    std::string line, word;
    auto flush = [&]() {
        if (word.empty()) return;
        const std::string tryLine = line.empty() ? word : line + " " + word;
        if (!line.empty() && TextW(weight, css, tryLine) > maxW) { out.push_back(line); line = word; }
        else line = tryLine;
        word.clear();
    };
    for (char c : t) { if (c == ' ') flush(); else word += c; }
    flush();
    if (!line.empty()) out.push_back(line);
    return out;
}
float TextWrapped(int weight, float css, float lh, float x, float top, ImU32 col, const std::string& t, float maxW) {
    const std::vector<std::string> ls = Wrap(weight, css, t, maxW);
    for (size_t k = 0; k < ls.size(); k++) Text(weight, css, lh, x, top + lh * k, col, ls[k]);
    return lh * ls.size();
}
// the round "Home" button (44, chevron 9 x 14 at 2.4) + "Home" 17/600; ball oil on a colour field, raised on black
void HomeButton(float x, float y, bool onField, ImU32 textCol) {
    bool hv = false;
    const float w = 44 + 12 + TextW(600, 17, "Home");
    if (Hit("##home", x, y, w, 44, &hv)) UiHomeSetScreen(SCR_HOME);
    const float cx = x + 22, cy = y + 22;
    Disc(cx, cy, 22, onField ? (hv ? U(kRaised) : IM_COL32(0, 0, 0, 255)) : (hv ? U(Hex(0x2A2A2A)) : U(kRaised)));
    ImVec2 p[3] = { P(cx + 2.5f, cy - 5), P(cx - 2.5f, cy), P(cx + 2.5f, cy + 5) };
    DL->AddPolyline(p, 3, U(kWhite), ImDrawFlags_None, 2.4f * S);
    Text(600, 17, 22, x + 56, cy - 11, textCol, "Home", -0.1f);
}
// a pip meter (ALL LOOKS, DESIGN 5.5): 10 pips 7 x 16, gap 3; click or drag sets (pip n = n; a press on the lit first
// pip when the value is 1 sets 0); the wheel over it steps 1. Returns the new value, or -1 when nothing changed.
int PipMeter(const char* id, float x, float cy, int v) {
    for (int k = 0; k < 10; k++) Rect(x + k * 10, cy - 8, 7, 16, k < v ? U(kWhite) : U(kPip), 2);
    bool hv = false, held = false;
    ImGui::SetCursorScreenPos(P(x - 3, cy - 15));
    ImGui::InvisibleButton(id, ImVec2(103 * S, 30 * S));
    hv = ImGui::IsItemHovered();
    held = ImGui::IsItemActive();
    if (hv) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (!(s_h.live || s_h.acts)) return -1;
    const float mx = (ImGui::GetIO().MousePos.x - OX) / S;
    const int n = std::min(std::max((int)floorf((mx - x) / 10.0f) + 1, 0), 10);
    if (ImGui::IsItemActivated()) return (n == 1 && v == 1) ? 0 : (n != v ? n : -1);
    if (held && ImGui::IsMouseDragging(0, 2.0f) && n != v) return n;
    if (hv && ImGui::GetIO().MouseWheel != 0) {
        const int w = std::min(std::max(v + (ImGui::GetIO().MouseWheel > 0 ? 1 : -1), 0), 10);
        if (w != v) return w;
    }
    return -1;
}
// "Show next" pill (DESIGN 5.6), right edge at rx, top y; queued = filled mark, "Up next". Returns clicked.
bool ShowNextPill(const char* id, float rx, float y, bool queued, Rgb mark) {
    const std::string label = queued ? "Up next" : "Show next";
    const float w = TextW(600, 13, label) + 42, x = rx - w;
    bool hv = false;
    const bool c = Hit(id, x, y, w, 30, &hv);
    if (queued) Rect(x, y, w, 30, U(mark), 15);
    else {
        if (hv) Rect(x, y, w, 30, U(mark, 0.16f), 15);
        RectLine(x, y, w, 30, U(mark), 15, 2);
    }
    const ImU32 fg = queued ? IM_COL32(0, 0, 0, 255) : U(mark);
    Ring(x + 2 + 9 + 5.5f, y + 15, 4.5f, fg, 2);
    Text(600, 13, 16, x + 2 + 9 + 11 + 7, y + 7, fg, label);
    return c;
}
// "Visit" pill (ALL LOOKS, the other looks): the Show next pill's shape; on = filled, "Visiting"; inactive = 40 %, no hit
bool VisitPill(const char* id, float rx, float y, bool on, bool active, Rgb mark) {
    const std::string label = on ? "Visiting" : "Visit";
    const float w = TextW(600, 13, label) + 42, x = rx - w;
    bool hv = false;
    const bool c = active && Hit(id, x, y, w, 30, &hv);
    const float a = active ? 1.0f : 0.4f;
    if (on) Rect(x, y, w, 30, U(mark), 15);
    else {
        if (hv) Rect(x, y, w, 30, U(mark, 0.16f), 15);
        RectLine(x, y, w, 30, U(mark, a), 15, 2);
    }
    const ImU32 fg = on ? IM_COL32(0, 0, 0, 255) : U(mark, a);
    Ring(x + 2 + 9 + 5.5f, y + 15, 4.5f, fg, 2);
    Text(600, 13, 16, x + 2 + 9 + 11 + 7, y + 7, fg, label);
    return c;
}
std::string Pct(int v, int sum) {
    if (v <= 0 || sum <= 0) return "never";
    const int p = (int)lroundf(100.0f * v / sum);
    return p < 1 ? "<1%" : std::to_string(p) + "%";
}
int RatingSum() {
    int s = 0;
    for (int i = 0; i < UiStillCount(); i++) s += UiStillRating(i, 0) + UiStillRating(i, 1);
    for (int k = 0; k < UiRatedCount(); k++) s += UiRatedRating(k);
    return s;
}
bool Queued(int i, int mode) {
    int qi = 0, qm = 0;
    for (int k = 0; UiQueueAt(k, &qi, &qm); k++) if (qi == i && qm == mode) return true;
    return false;
}
// Setting a face to 0 when every other face of every pair is 0 already would make the draw even among all pairs
// (Never would stop meaning never): refused, as HOME's Never is.
bool PairFaceSet(int i, int mode, int v) {
    if (v == 0) {
        int others = 0;
        for (int k = 0; k < UiStillCount(); k++)
            for (int md = 0; md < 2; md++) if (!(k == i && md == mode)) others += UiStillRating(k, md);
        if (others == 0) { Status("One colour pair has to stay on."); return false; }
    }
    UiSetStillRating(i, mode, v);
    return true;
}

// ============================================================================================ ALL LOOKS (g-looks.png)
void DrawLooksLeft(const Model& m, float h) {
    const Pal& pl = m.pal;
    const ImU32 strong = U(pl.strong), quiet = U(pl.quiet);
    HomeButton(40, 40, true, strong);
    Text(700, 44, 44, 40, 112, strong, "All looks", -1.3f);
    // what is playing: the kept snapshot of the pair + face playing (B5; headless: the --ui-now-image picture) and its line
    if (const Tex2* tp = TilePic(m)) {
        Rect(40, 184, 240, 135, IM_COL32(0, 0, 0, 255), 14);
        ImageOf(tp->srv.Get(), tp->w, tp->h, 40, 184, 240, 135, 14);
    } else if (s_h.nowImg) { Rect(40, 184, 240, 135, IM_COL32(0, 0, 0, 255), 14); Image(40, 184, 240, 135, 14); }
    else Rect(40, 184, 240, 135, U(Mul(pl.field, 0.55f)), 14);
    const UiNowView nv = UiDirectorNow();
    std::string what = m.title, sub;
    if (m.visiting) sub = "a visit, back to automatic at " + m.railEnd;
    else if (m.paused) sub = "paused";
    else if (!m.autoOn) sub = "automatic is off";
    else if (m.director) {
        what = PairName(nv.combo) + ", " + FaceName(nv.mode);
        if (nv.changing && !m.keepOn) sub = "changing now";
        else sub = (m.keepOn ? "kept, next change at " : "playing now, next change at ") + m.railEnd;
    } else sub = "playing now, next change at " + m.railEnd;
    float y = 331;
    y += TextWrapped(600, 17, 22, 40, y, strong, what, 244);
    Text(400, 15, 20, 40, y, quiet, sub);
    y += 20 + 30;
    Ring(40 + 6.5f, y + 10, 5.25f, strong, 2.5f);
    Text(600, 15, 20, 63, y, strong, "These end by themselves.");
    y += 20 + 16;
    Text(600, 15, 20, 40, y, strong, "Up next");
    y += 20 + 8;
    // the queue: its front in a box with an X (clears the queue), the rest counted
    int qi = -1, qm = 0, qn = 0;
    for (int a = 0, b = 0; UiQueueAt(qn, &a, &b); qn++) if (qn == 0) { qi = a; qm = b; }
    if (qi >= 0) {
        const std::vector<std::string> ls = Wrap(600, 15, PairName(qi) + ", " + FaceName(qm), 244 - 16 - 10 - 30 - 10);
        const std::string when = (m.railHasEnd ? "plays once at " + m.railEnd : std::string("plays once, next")) +
                                 (qn > 1 ? ", then " + std::to_string(qn - 1) + " more" : "");
        const float bh = 10 + 18.0f * ls.size() + 16 + 10;
        Rect(40, y, 244, bh, IM_COL32(0, 0, 0, 255), 18);
        for (size_t k = 0; k < ls.size(); k++) Text(600, 15, 18, 56, y + 10 + 18 * k, U(kWhite), ls[k]);
        Text(400, 13, 16, 56, y + 10 + 18 * ls.size(), U(pl.mark), when);
        bool hv = false;
        const float xc = 40 + 244 - 10 - 15, yc = y + bh * 0.5f;
        if (Hit("##qx", xc - 15, yc - 15, 30, 30, &hv)) UiQueueClear();
        Ring(xc, yc, 14.25f, hv ? U(kWhite) : U(kEdge), 1.5f);
        Cross(xc, yc, U(kWhite));
        y += bh;
    } else {
        Text(400, 15, 20, 40, y, quiet, "Nothing. Automatic picks.");
        y += 20;
    }
    y += 10;
    TextWrapped(400, 15, 20, 40, y, quiet, "Show next puts a look here. It plays once, then automatic carries on after.", 244);
    // the visit-only looks, at the bottom: 30 min of Ink / Fluid (a guest visit, HOME's chips' call). Only while the
    // automatic mode runs (a visit returns to it) and when the start preset is there (else 40 %, the reason as a tooltip);
    // the one being visited is filled.
    Text(600, 15, 20, 40, 646, strong, "Visit only, never automatic");
    float cx = 40;
    for (int g = 0; g < 2; g++) {
        const char* nm = g ? "Fluid" : "Ink";
        const float cw = TextW(600, 15, nm) + 32;
        std::string why;
        const bool on = m.visitGuest == g, have = UiGuestAvailable(g, &why);
        const bool active = on || (m.autoOn && have);
        if (!m.autoOn && why.empty()) why = "Only while the automatic mode runs: a visit goes back to it.";
        bool hv = false;
        char id[32];
        snprintf(id, sizeof(id), "##lguest%d", g);
        if (active && Hit(id, cx, 674, cw, 36, &hv) && !on) UiVisitGuest(g, 1800.0f);
        if (!active && ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(P(cx, 674), P(cx + cw, 674 + 36), false))
            ImGui::SetTooltip("%s", why.c_str());
        const float a = active ? 1.0f : 0.4f;
        if (on) Rect(cx, 674, cw, 36, strong, 18);
        else {
            if (hv) Rect(cx, 674, cw, 36, U(pl.strong, 0.14f), 18);
            RectLine(cx, 674, cw, 36, U(pl.strong, a), 18, 2);
        }
        Text(600, 15, 20, cx + 16, 682, on ? U(pl.field) : U(pl.strong, a), nm);
        cx += cw + 8;
    }
    TextWrapped(400, 15, 20, 40, 720, quiet, "For 30 min, then back to automatic.", 244);
    (void)h;
}

void DrawLooksRight(const Model& m, float w, float h) {
    const ImU32 white = U(kWhite), ash = U(kAsh);
    std::vector<int> vis;                                       // the listed pairs (no draft, no removed one)
    for (int i = 0; i < UiStillCount(); i++) if (UiPairVisible(i)) vis.push_back(i);
    const int n = (int)vis.size(), nr = UiRatedCount(), sum = RatingSum();
    const float x0 = 368, cw = 864;
    // the black side scrolls when the lists are taller than the window (more pairs or looks)
    const float listTop = 148, addY = listTop + 1 + 48.0f * n, othersY = addY + 48 + 24;
    const float gridTop = othersY + 26 + 8, rows = (float)((nr + 2) / 3);
    const float contentH = gridTop + 1 + 74 * rows + 40;
    const float maxScroll = std::max(0.0f, contentH - 800.0f);
    {   // the wheel over the black side (no hit area: it would take the hover from the controls under it)
        const ImGuiIO& io = ImGui::GetIO();
        if (io.MouseWheel != 0 && io.MousePos.x >= P(320, 0).x && ImGui::IsWindowHovered())
            s_h.looksScroll -= io.MouseWheel * 60.0f;
    }
    s_h.looksScroll = std::min(std::max(s_h.looksScroll, 0.0f), maxScroll);
    const float sy = s_h.looksScroll;
    ImGui::PushClipRect(P(320, 0), ImVec2(w, h), true);
    DL->PushClipRect(P(320, 0), ImVec2(w, h), true);

    Rect(x0, 40 + 6 - sy, 10, 10, white);
    Text(600, 17, 22, x0 + 21, 40 - sy, white, "Ratings stay until you change them. 0 is never.", -0.1f);
    Text(400, 15, 20, x0 + 21, 68 - sy, ash, "The chance a look comes up is its rating divided by all ratings together (" +
                                               std::to_string(sum) + " now).");
    Text(700, 21, 26, x0, 108 - sy, white, "Colour pairs", -0.3f);
    Text(400, 15, 20, 624, 114 - sy, ash, "Vivid: white light, both colours loud");
    Text(400, 15, 20, 936, 114 - sy, ash, "Darkroom: two dim lights, mostly black");
    Rect(x0, listTop - sy, cw, 1, U(kLine));
    for (int row = 0; row < n; row++) {
        const int i = vis[row];
        const float y0 = listTop + 1 + 48.0f * row - sy, cy = y0 + 23.5f;
        // the thumb: the pair's two colours (the vivid still per pair comes with the snapshot stage)
        const Rgb c1 = HueColour(UiStillAnchor(i)), c2 = HueColour(UiStillAnchor(i) + UiStillOffset(i));
        DL->AddRectFilled(P(x0, cy - 18), P(x0 + 32, cy + 18), U(c1), 8 * S, ImDrawFlags_RoundCornersLeft);
        DL->AddRectFilled(P(x0 + 32, cy - 18), P(x0 + 64, cy + 18), U(c2), 8 * S, ImDrawFlags_RoundCornersRight);
        if (PairMax(i) == 0) Rect(x0, cy - 18, 64, 36, IM_COL32(0, 0, 0, 158), 8);
        const bool added = !UiPairBuiltIn(i);
        const bool asking = added && _stricmp(s_h.removeAsk.c_str(), UiStillKey(i).c_str()) == 0;
        char id[40];
        if (added) {
            Text(600, 15, 19, 446, cy - 18, white, PairName(i));
            // "Remove this pair" (DESIGN 5.15): a text link under the name of a pair the owner added; it asks once
            const ImU32 rm = asking ? white : ash;
            const float ty = cy + 3;
            bool hv = false;
            snprintf(id, sizeof(id), "##rm%d", i);
            if (!asking && Hit(id, 444, ty - 2, 18 + TextW(600, 13, "Remove this pair"), 20, &hv)) {
                s_h.removeAsk = UiStillKey(i);
                s_h.status.clear();
            }
            const ImU32 rc = hv ? white : rm;
            Line(446.8f, ty + 2.6f, 456.2f, ty + 2.6f, rc, 1.4f);
            Line(448, ty + 2.8f, 448.7f, ty + 11.2f, rc, 1.4f);
            Line(448.7f, ty + 11.2f, 454.3f, ty + 11.2f, rc, 1.4f);
            Line(454.3f, ty + 11.2f, 455, ty + 2.8f, rc, 1.4f);
            Text(600, 13, 16, 462, ty, rc, "Remove this pair");
            Rect(462, ty + 15, TextW(600, 13, "Remove this pair"), 1, rc);
        } else {
            Text(600, 15, 19, 446, cy - 9.5f, white, PairName(i));
        }
        if (asking) {
            // asked once, in place of the two face cells: "Remove <pair> for good?  Remove / Keep it"
            const std::string q = "Remove " + PairName(i) + " for good?";
            Text(600, 15, 20, 624, cy - 10, white, q);
            const float kw = TextW(600, 15, "Keep it") + 40, rw = TextW(600, 15, "Remove") + 40;
            const float kx = 1232 - kw, rx = kx - 10 - rw;
            bool hv = false;
            snprintf(id, sizeof(id), "##rmyes%d", i);
            if (Hit(id, rx, cy - 17, rw, 34, &hv)) {
                const std::string nm = PairName(i), key = UiStillKey(i);
                UiPairRemove(i);
                s_h.removeAsk.clear();
                if (_stricmp(s_h.selName.c_str(), key.c_str()) == 0) s_h.selName.clear();
                Status(nm + " is removed.");
            }
            Rect(rx, cy - 17, rw, 34, hv ? U(Hex(0xE6E6E6)) : white, 17);
            Text(600, 15, 20, rx + 20, cy - 10, IM_COL32(0, 0, 0, 255), "Remove");
            snprintf(id, sizeof(id), "##rmno%d", i);
            if (Hit(id, kx, cy - 17, kw, 34, &hv)) s_h.removeAsk.clear();
            RectLine(kx, cy - 17, kw, 34, hv ? white : U(kEdge), 17, 2);
            Text(600, 15, 20, kx + 20, cy - 10, white, "Keep it");
            Rect(x0, y0 + 47, cw, 1, U(kLine));
            continue;
        }
        for (int md = 0; md < 2; md++) {
            const float cx = md ? 936.0f : 624.0f;
            const int v = UiStillRating(i, md);
            snprintf(id, sizeof(id), "##pip%d_%d", i, md);
            const int nv = PipMeter(id, cx, cy, v);
            if (nv >= 0) PairFaceSet(i, md, nv);
            const int vv = UiStillRating(i, md);
            TextR(400, 15, 20, cx + 97 + 8 + 18, cy - 10, ash, std::to_string(vv));
            if (vv > 0) TextR(700, 18, 22, cx + 97 + 8 + 18 + 4 + 50, cy - 11, white, Pct(vv, sum), -0.2f);
            else TextR(400, 15, 20, cx + 97 + 8 + 18 + 4 + 50, cy - 10, ash, "never");
            snprintf(id, sizeof(id), "##sn%d_%d", i, md);
            const bool q = Queued(i, md);
            if (ShowNextPill(id, cx + 296, cy - 15, q, m.pal.mark) && !q) UiQueueStill(i, md);
        }
        Rect(x0, y0 + 47, cw, 1, U(kLine));
    }
    {   // Add a pair: opens ADD A PAIR (Cancel comes back here)
        const float y0 = addY - sy, cy = y0 + 23.5f;
        bool hv = false;
        if (Hit("##addrow", x0, y0, 446 - x0 + TextW(600, 15, "Add a pair") + 12, 47, &hv)) AddOpen(SCR_LOOKS);
        RectLine(x0, cy - 18, 64, 36, hv ? white : U(kEdge), 8, 2);
        Line(x0 + 32, cy - 6, x0 + 32, cy + 6, white, 2);
        Line(x0 + 26, cy, x0 + 38, cy, white, 2);
        Text(600, 15, 20, 446, cy - 10, white, "Add a pair");
        Text(400, 15, 20, 446 + TextW(600, 15, "Add a pair") + 12, cy - 10, ash,
             "Pick two colours. Pairs you add can be removed; the six built in can only be set to 0.");
        Rect(x0, y0 + 47, cw, 1, U(kLine));
    }
    // the other looks: 3 columns, 28 apart, 74 high
    Text(700, 21, 26, x0, othersY - sy, white, "Other looks", -0.3f);
    Text(400, 15, 20, x0 + TextW(700, 21, "Other looks", -0.3f) + 10, othersY + 4 - sy, ash,
         "They play in whichever colour pair comes up.");
    Rect(x0, gridTop - sy, cw, 1, U(kLine));
    const float colW = (cw - 2 * 28) / 3.0f;
    for (int k = 0; k < nr; k++) {
        const float x = x0 + (k % 3) * (colW + 28), top = gridTop + 1 + 74.0f * (k / 3) - sy;
        Text(600, 16, 20, x, top + 10, white, RatedDisplay(k));
        const int v = UiRatedRating(k);
        if (v > 0) TextR(700, 18, 22, x + colW, top + 9, white, Pct(v, sum), -0.2f);
        else TextR(400, 15, 20, x + colW, top + 10, ash, "never");
        char id[40];
        snprintf(id, sizeof(id), "##rpip%d", k);
        const int nv = PipMeter(id, x, top + 41 + 15, v);
        if (nv >= 0) UiSetRatedRating(k, nv);
        TextR(400, 15, 20, x + 97 + 8 + 14, top + 41 + 5, ash, std::to_string(UiRatedRating(k)));
        // Visit (stage 7's call path, as the HOME chips): 30 min of that look, then the automatic mode carries on. Only
        // while the automatic mode runs (a visit returns to it); the look being visited reads "Visiting", filled.
        snprintf(id, sizeof(id), "##rvisit%d", k);
        const bool visiting = m.visitRated == k;
        if (VisitPill(id, x + colW, top + 41, visiting, m.autoOn || visiting, m.pal.mark) && !visiting) UiVisit(k, 1800.0f);
        Rect(x, top + 73, colW, 1, U(kLine));
    }
    if (maxScroll > 0) {                                        // a 4 px thumb at the right edge
        const float vh = 800.0f, th = vh * vh / contentH, ty = sy / maxScroll * (vh - th);
        Rect(1280 - 20, ty, 4, th, U(kEdge), 2);
    }
    if (!s_h.status.empty() && (!s_h.live || GetTickCount64() - s_h.statusAt < 5000)) {
        const float sw = TextW(400, 13, s_h.status);              // on its own black, over whatever scrolled under it
        Rect(1232 - sw - 12, 780 - 16 - 6, sw + 24, 28, IM_COL32(0, 0, 0, 255), 14);
        TextR(400, 13, 16, 1232, 780 - 16, U(m.pal.mark), s_h.status);
    }
    DL->PopClipRect();
    ImGui::PopClipRect();
}

// ============================================================================================ DISPLAY AND SYSTEM
// The five settings that belong to this PC (today's right-column System rows), set through the same rows and
// UiSetValue as there, so they save exactly as today; plus the HDR analyzer, the presets folder and Exit.
int SysRow(const char* sec, const char* key) { return UiFindRow(sec, key); }
void SetRowValue(int row, float v) {
    if (row < 0) return;
    const float old = UiValue(row);
    UiSetValue(row, v);
    UiPushKeyUndo(row, old, UiValue(row));
}
void Toggle(const char* id, float rx, float cy, int row) {
    if (row < 0) return;
    const bool on = UiValue(row) > 0.5f;
    const float x = rx - 52, y = cy - 15;
    bool hv = false;
    if (Hit(id, x - 14 - 28, y, 52 + 14 + 28, 30, &hv)) SetRowValue(row, on ? 0.0f : 1.0f);
    TextR(400, 15, 20, x - 14, cy - 10, U(kAsh), on ? "On" : "Off");
    if (on) { Rect(x, y, 52, 30, U(kWhite), 15); Disc(x + 52 - 4 - 11, cy, 11, IM_COL32(0, 0, 0, 255)); }
    else {
        RectLine(x, y, 52, 30, hv ? U(kWhite) : U(kEdge), 15, 2);
        Disc(x + 4 + 9 + 0.0f, cy, 9, U(kAsh));
    }
}
void Pill(const char* id, float x, float y, const std::string& label, int icon, bool strong, std::function<void()> act) {
    // icon 0 none, 1 chevron (goes to a screen), 2 arrow (opens outside the app)
    const float iw = icon == 1 ? 8.0f : icon == 2 ? 12.0f : 0.0f;
    const float w = 2 + 18 + TextW(600, 15, label) + (icon ? 14 + iw : 0) + 18 + 2;
    bool hv = false;
    if (Hit(id, x, y, w, 40, &hv)) act();
    RectLine(x, y, w, 40, (strong || hv) ? U(kWhite) : U(kEdge), 20, 2);
    Text(600, 15, 20, x + 20, y + 10, U(kWhite), label);
    const float ix = x + 20 + TextW(600, 15, label) + 14;
    if (icon == 1) Chevron(ix, y + 14, U(kWhite));
    if (icon == 2) {
        ImVec2 p[3] = { P(ix + 4.5f, y + 14 + 1.5f), P(ix + 10.5f, y + 14 + 1.5f), P(ix + 10.5f, y + 14 + 7.5f) };
        DL->AddPolyline(p, 3, U(kWhite), ImDrawFlags_None, 2.0f * S);
        Line(ix + 10.5f, y + 14 + 1.5f, ix + 1.5f, y + 14 + 10.5f, U(kWhite), 2.0f);
    }
}
float PillW(const std::string& label, int icon) {
    const float iw = icon == 1 ? 8.0f : icon == 2 ? 12.0f : 0.0f;
    return 2 + 18 + TextW(600, 15, label) + (icon ? 14 + iw : 0) + 18 + 2;
}

void DrawDisplayScreen() {
    const ImU32 white = U(kWhite), ash = U(kAsh);
    HomeButton(48, 40, false, white);
    Text(700, 44, 48, 48, 112, white, "Display and", -1.3f);
    Text(700, 44, 48, 48, 160, white, "system", -1.3f);
    Rect(48, 232 + 6, 10, 10, white);
    Text(600, 17, 22, 69, 232, white, "These stay until you change them.", -0.1f);
    Text(400, 15, 20, 69, 262, ash, "They belong to this PC, not to a look.");

    const float x0 = 520, rx = 1232, top = 112;
    Rect(x0, top, 712, 1, U(kLine));
    struct Item { const char* label; const char* sub; const char* sec; const char* key; };
    const Item items[5] = {
        { "FPS limit", "Frames drawn each second", "general", "fps_limit" },
        { "Pause on fullscreen app", "A game or a film fills the screen", "general", "pause_on_fullscreen" },
        { "Pause on maximized app", "Any window covers the desktop", "general", "pause_on_maximized" },
        { "Mirror on second monitor", "The same wallpaper on both screens", "general", "mirror_second" },
        { "Start with Windows", "The wallpaper comes on when you sign in", "system", "autostart" },
    };
    for (int k = 0; k < 5; k++) {
        const float y0 = top + 1 + 76.0f * k, cy = y0 + 37.5f;
        Text(600, 17, 22, x0, cy - 22, white, items[k].label, -0.1f);
        Text(400, 15, 20, x0, cy, ash, items[k].sub);
        const int row = SysRow(items[k].sec, items[k].key);
        char id[32];
        snprintf(id, sizeof(id), "##sys%d", k);
        if (k == 0 && row >= 0) {
            // the segmented choice (DESIGN 5.12): 60, 120, 144, 240, No limit (= 260, the row's top: the cap is
            // skipped at or above the monitor's refresh); a stored value not in the list shows as its own segment
            const float cur = UiValue(row);
            std::vector<std::pair<std::string, float>> segs = { { "60", 60.0f }, { "120", 120.0f }, { "144", 144.0f }, { "240", 240.0f } };
            const bool noLimit = cur >= 245.0f;
            bool listed = noLimit;
            for (auto& sg : segs) if (fabsf(sg.second - cur) < 0.5f) listed = true;
            if (!listed) {
                segs.push_back({ std::to_string((int)lroundf(cur)), cur });
                std::sort(segs.begin(), segs.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
            }
            segs.push_back({ "No limit", 260.0f });
            float tw = 0;
            std::vector<float> sw;
            for (auto& sg : segs) { sw.push_back(std::max(56.0f, TextW(600, 15, sg.first) + 24)); tw += sw.back(); }
            const float bx = rx - (tw + 8), by = cy - 19;
            RectLine(bx, by, tw + 8, 38, U(kEdge), 19, 2);
            float sx = bx + 4;
            for (size_t q = 0; q < segs.size(); q++) {
                const bool on = segs[q].second >= 245.0f ? noLimit : (!noLimit && fabsf(segs[q].second - cur) < 0.5f);
                char sid[32];
                snprintf(sid, sizeof(sid), "##fps%d", (int)q);
                bool hv = false;
                if (Hit(sid, sx, by + 4, sw[q], 30, &hv) && !on) SetRowValue(row, segs[q].second);
                if (on) Rect(sx, by + 4, sw[q], 30, white, 15);
                else if (hv) Rect(sx, by + 4, sw[q], 30, U(kRaised), 15);
                TextC(600, 15, 20, sx + sw[q] * 0.5f, cy - 10, on ? IM_COL32(0, 0, 0, 255) : ash, segs[q].first);
                sx += sw[q];
            }
        } else {
            Toggle(id, rx, cy, row);
        }
        Rect(x0, y0 + 75, 712, 1, U(kLine));
    }
    // tools
    const float ty = top + 1 + 76 * 5 + 40;
    Text(400, 15, 20, x0, ty, ash, "Tools");
    Pill("##hdr", x0, ty + 32, "HDR analyzer", 1, false, []() {
        if (s_h.live) ShowAnalyzerWindow(); else Status("HDR analyzer: opens the analyzer window (a live window only)");
    });
    Pill("##presets", x0 + PillW("HDR analyzer", 1) + 12, ty + 32, "Presets folder", 2, false, []() {
        if (s_h.live) ShellExecuteW(nullptr, L"open", UiPresetsDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        else Status("Presets folder: opens " + UiNarrow(UiPresetsDir()) + " (a live window only)");
    });
    // exit
    Rect(x0, 672, 712, 1, U(kLine));
    Text(400, 15, 20, x0, 707, ash, "Exit stops the wallpaper until you start it again.");
    Pill("##exit", rx - PillW("Exit wallpaper", 0), 697, "Exit wallpaper", 0, true, []() {
        if (s_h.live) RequestExit(); else Status("Exit wallpaper: stops the wallpaper (a live window only)");
    });
    if (!s_h.status.empty() && (!s_h.live || GetTickCount64() - s_h.statusAt < 5000))
        Text(400, 13, 16, x0, 760, U(kAsh), s_h.status);
}

// ============================================================================================ ADD A PAIR (g-add.png)
// geometry (design px): the ring's centre and radii (DESIGN 5.13: outer d 312, band 28, handles d 38 at r 142)
const float kRingCX = 736 + 156, kRingCY = 220 + 156, kRingR0 = 128, kRingR1 = 156, kHandleR = 142;
const float kFieldX = 736, kFieldY = 130, kFieldW = 496, kFieldH = 50;
ImVec2 RingPoint(float hue, float r) {                       // 0 deg at the top, clockwise
    const float a = hue * 3.14159265f / 180.0f;
    return ImVec2(kRingCX + sinf(a) * r, kRingCY - cosf(a) * r);
}
float HueAt(float x, float y) {                              // the hue under a design point (whole degrees)
    const float a = atan2f(x - kRingCX, -(y - kRingCY)) * 180.0f / 3.14159265f;
    return Wrap360(roundf(a));
}
void DrawHueRing() {
    // 360 one-degree segments, each in the APP's colour for its hue (HueColour = the swatches' colours), the colour
    // interpolated across a segment; the two edges smoothed with a thin black anti-aliased circle (both sides are oil)
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    DL->PrimReserve(360 * 6, 360 * 4);
    for (int d = 0; d < 360; d++) {
        const ImU32 ca = U(HueColour((float)d)), cb = U(HueColour((float)(d + 1)));
        const ImVec2 o0 = RingPoint((float)d, kRingR1), i0 = RingPoint((float)d, kRingR0);
        const ImVec2 o1 = RingPoint((float)(d + 1), kRingR1), i1 = RingPoint((float)(d + 1), kRingR0);
        const ImDrawIdx b = (ImDrawIdx)DL->_VtxCurrentIdx;
        DL->PrimWriteVtx(P(o0.x, o0.y), uv, ca);
        DL->PrimWriteVtx(P(i0.x, i0.y), uv, ca);
        DL->PrimWriteVtx(P(i1.x, i1.y), uv, cb);
        DL->PrimWriteVtx(P(o1.x, o1.y), uv, cb);
        DL->PrimWriteIdx(b); DL->PrimWriteIdx((ImDrawIdx)(b + 1)); DL->PrimWriteIdx((ImDrawIdx)(b + 2));
        DL->PrimWriteIdx(b); DL->PrimWriteIdx((ImDrawIdx)(b + 2)); DL->PrimWriteIdx((ImDrawIdx)(b + 3));
    }
    DL->AddCircle(P(kRingCX, kRingCY), kRingR1 * S + 0.5f, IM_COL32(0, 0, 0, 255), 360, 1.5f);
    DL->AddCircle(P(kRingCX, kRingCY), kRingR0 * S - 0.5f, IM_COL32(0, 0, 0, 255), 360, 1.5f);
}
void DrawHandle(int n, float hue) {
    const ImVec2 c = RingPoint(hue, kHandleR);
    const Rgb fill = HueColour(hue);
    Disc(c.x, c.y, 21, IM_COL32(0, 0, 0, 255));              // the 2 px oil outer ring
    Disc(c.x, c.y, 19, U(kWhite));                           // the 3 px white border
    Disc(c.x, c.y, 16, U(fill));
    const std::string t = std::to_string(n);
    const ImU32 fg = Lum(fill) >= 0.30f ? IM_COL32(0, 0, 0, 255) : U(kWhite);
    Text(700, 15, 20, c.x - TextW(700, 15, t) * 0.5f, c.y - 10, fg, t);
}
// the name field: an ImGui InputText laid into the design's box (19/600 white on black, 2 px stroke, the hint inside)
void DrawNameField() {
    if (!s_add.typed) snprintf(s_add.name, sizeof(s_add.name), "%s", AddSuggest().c_str());
    // the hint: where the name came from; a typed name the window will show differently says how ("shows as ...")
    std::string hint = "made from the two colours";
    if (s_add.typed) {
        const std::string shown = KeyDisplay(UiPairKeyFor(AddDisplay(), AddDraftAlive() ? s_add.draft : -1));
        hint = shown == AddDisplay() ? std::string() : "shows as " + shown;
    }
    const float hintW = hint.empty() ? 0.0f : TextW(500, 13, hint) + 16;
    const ImGuiStyle& st = ImGui::GetStyle();
    const float fs = PxSize(19) / std::max(st.FontScaleMain * st.FontScaleDpi, 0.01f);
    ImGui::PushFont(Fnt(600), fs);
    const float lineH = ImGui::GetFontSize();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14 * S, std::max(0.0f, (46 * S - lineH) * 0.5f)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, U(kEdge));
    ImGui::SetCursorScreenPos(P(kFieldX + 2, kFieldY + 2));
    ImGui::SetNextItemWidth((kFieldW - 4 - hintW) * S);
    char before[32];
    memcpy(before, s_add.name, sizeof(before));
    ImGui::InputText("##pairname", s_add.name, 29, ImGuiInputTextFlags_AutoSelectAll);   // max 28 characters
    const bool active = ImGui::IsItemActive();
    if (strcmp(before, s_add.name) != 0) s_add.typed = true;  // the owner typed: it stays as typed
    if (!active && s_add.typed && AddDisplay() == AddSuggest() && s_add.name[0] == 0) s_add.typed = false;
    if (active) s_add.grab = 0;
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(3);
    ImGui::PopFont();
    RectLine(kFieldX, kFieldY, kFieldW, kFieldH, active ? U(kWhite) : U(kEdge), 14, 2);
    if (!hint.empty()) TextR(500, 13, 16, kFieldX + kFieldW - 16, kFieldY + 17, U(kAsh), hint);
}
void DrawAddScreen() {
    if (!s_add.typed) snprintf(s_add.name, sizeof(s_add.name), "%s", AddSuggest().c_str());   // follows the hues
    const Pal pl = PairPal(s_add.h1, false);                 // the new pair's first colour (decision 8)
    const ImU32 strong = U(pl.strong), quiet = U(pl.quiet), white = U(kWhite), ash = U(kAsh);
    const Rgb c1 = HueColour(s_add.h1), c2 = HueColour(s_add.h2);
    const std::string title = KeyDisplay(UiPairKeyFor(AddDisplay(), AddDraftAlive() ? s_add.draft : -1));   // as it will show

    // ---- the colour field: the pair as two flat areas (no drawn sketch), its name, Show it next
    // an oil (black) panel, as the mock-up's sketch sits on black, with the two colours as two flat areas in it: the
    // first colour (the film) the larger one, the second (the patches) beside it; the black keeps the first colour
    // apart from the field, which is the same colour
    const float stX = 48, stY = 48, stW = 592, stH = 333, cut = stX + stW * 0.60f;
    Rect(stX, stY, stW, stH, IM_COL32(0, 0, 0, 255), 22);
    Rect(stX + 16, stY + 16, cut - 8 - (stX + 16), stH - 32, U(c1), 12);
    Rect(cut + 8, stY + 16, stX + stW - 16 - (cut + 8), stH - 32, U(c2), 12);
    Text(400, 15, 20, 48, 395, quiet, "The two colours, flat. The real picture shows up once the pair has played.");
    float tSize = 72.0f;
    const float tw = TextW(700, tSize, title, -2.5f);
    if (tw > 592.0f) tSize = std::max(36.0f, tSize * 592.0f / tw);
    Text(700, tSize, 72, 48, 435, strong, title, -2.5f * tSize / 72.0f);
    Text(400, 19, 27, 48, 517, strong, "New. Not in the automatic mode yet.");
    Ring(48 + 6.5f, 562 + 11, 5.25f, strong, 2.5f);
    Text(600, 17, 22, 72, 562, strong, "This ends by itself. Automatic carries on after.", -0.1f);
    // Show it next: adds the pair as a draft (ratings 0: only the queue reaches it) and queues its vivid face once
    int qpos = 0;
    const int ds = AddDraftState(&qpos);
    const UiNowView nv = UiDirectorNow();
    const float keepLeft = UiKeepLeftSec();
    std::string name = "Show it next", sub;
    if (ds == 2) { name = "Playing now"; sub = "plays once, then automatic carries on"; }
    else if (ds == 1) {
        name = "Up next";
        if (qpos > 0) sub = "plays once, after " + std::to_string(qpos) + " more in the line";
        else if (keepLeft > 0) sub = "plays once at " + Hhmm(NowSecOfDay() + keepLeft) + ", when the keep ends";
        else if (nv.director && !nv.changing && nv.holdLeftSec > 0) sub = "plays once at " + Hhmm(NowSecOfDay() + nv.holdLeftSec);
        else sub = "plays once, after the change on screen";
    } else {
        int qn = 0;
        for (int a = 0, b = 0; UiQueueAt(qn, &a, &b); qn++) {}
        if (qn > 0) sub = "plays once, after the " + std::to_string(qn) + " lined up";
        else if (keepLeft > 0) sub = "plays once at " + Hhmm(NowSecOfDay() + keepLeft) + ", when the keep ends";
        else sub = "the change starts now; it plays once";
    }
    const float by = 600, bc = by + 32;
    const float wName = std::max(TextW(600, 21, name, -0.2f), TextW(500, 15, sub));
    bool hv = false;
    if (Hit("##shownext", 48, by, 64 + 14 + wName, 64, &hv)) AddShowNext();
    if (ds) Ring(80, bc, 37.5f, strong, 3.0f);
    Disc(80, bc, 32, hv ? U(kRaised) : IM_COL32(0, 0, 0, 255));
    Poly(80 - 13, bc - 11, { ImVec2(0, 0), ImVec2(12, 11), ImVec2(0, 22) }, white);
    Poly(80 - 13, bc - 11, { ImVec2(13, 0), ImVec2(25, 11), ImVec2(13, 22) }, white);
    const float top = bc - (23 + 3 + 18) * 0.5f;
    Text(600, 21, 23, 48 + 64 + 14, top, strong, name, -0.2f);
    Text(500, 15, 18, 48 + 64 + 14, top + 26, strong, sub);
    Text(400, 15, 20, 48, 700, quiet, "Trying it does not keep it.");

    // ---- the black side: the pair you would keep
    Rect(736, 48 + 6, 10, 10, white);
    Text(600, 17, 22, 757, 48, white, "Keep this pair, and it stays in the automatic mode.", -0.1f);
    Text(400, 15, 20, 736, 102, ash, "Name");
    DrawNameField();

    // the hue ring: drag a handle round it (a press on the band +- 10 px grabs the nearer handle)
    DrawHueRing();
    HalfDisc(kRingCX, kRingCY, 60, U(c1), true);
    HalfDisc(kRingCX, kRingCY, 60, U(c2), false);
    {
        ImGui::SetCursorScreenPos(P(kRingCX - kRingR1 - 10, kRingCY - kRingR1 - 10));
        ImGui::InvisibleButton("##ring", ImVec2((kRingR1 + 10) * 2 * S, (kRingR1 + 10) * 2 * S));
        const bool acts = s_h.live || s_h.acts;
        const float mx = (ImGui::GetIO().MousePos.x - OX) / S, my = (ImGui::GetIO().MousePos.y - OY) / S;
        const float dist = sqrtf((mx - kRingCX) * (mx - kRingCX) + (my - kRingCY) * (my - kRingCY));
        const bool onBand = dist >= kRingR0 - 10 && dist <= kRingR1 + 10;
        if (ImGui::IsItemHovered() && onBand) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (acts && ImGui::IsItemActivated() && onBand) {
            const float h = HueAt(mx, my);
            s_add.grab = fabsf(Wrap180Ui(h - s_add.h1)) <= fabsf(Wrap180Ui(h - s_add.h2)) ? 1 : 2;
            s_add.focus = s_add.grab;
        }
        if (!ImGui::IsItemActive()) s_add.grab = 0;
        if (acts && s_add.grab) (s_add.grab == 1 ? s_add.h1 : s_add.h2) = HueAt(mx, my);
        // the arrow keys move the handle last used 5 deg (not while the name is being typed)
        if (acts && !ImGui::GetIO().WantTextInput) {
            float& h = s_add.focus == 1 ? s_add.h1 : s_add.h2;
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow)) h = Wrap360(h + 5);
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow)) h = Wrap360(h - 5);
        }
    }
    if (s_add.grab == 1) { DrawHandle(2, s_add.h2); DrawHandle(1, s_add.h1); }   // the one in hand on top
    else { DrawHandle(1, s_add.h1); DrawHandle(2, s_add.h2); }

    // the legend: the two colours by name
    const float lx = 1076;
    for (int k = 0; k < 2; k++) {
        const float y = 252 + 74.0f * k;
        const Rgb c = k ? c2 : c1;
        Text(400, 15, 20, lx, y, ash, k ? "Second colour" : "First colour");
        Disc(lx + 13, y + 24 + 14, 13, U(kWhite));
        Disc(lx + 13, y + 24 + 14, 11, U(c));
        const std::string num = k ? "2" : "1";
        Text(700, 13, 16, lx + 13 - TextW(700, 13, num) * 0.5f, y + 24 + 6, Lum(c) >= 0.30f ? IM_COL32(0, 0, 0, 255) : white, num);
        Text(700, 24, 28, lx + 36, y + 24, white, UiHueName(k ? s_add.h2 : s_add.h1), -0.4f);
    }
    TextWrapped(400, 15, 20, lx, 400, ash, "Drag the two handles round the ring. Every colour is a vivid one; the look sets how bright.", 156);

    // the way out: Cancel (a draft is hidden) / Keep this pair (kept: 5 of 10, written to combos.ini)
    Text(400, 15, 20, 736, 674, ash, "It joins the automatic mode at 5 of 10, vivid and darkroom.");
    const float kw = 2 + 20 + 10 + 10 + TextW(600, 16, "Keep this pair") + 20 + 2, cwid = 2 + 20 + TextW(600, 16, "Cancel") + 20 + 2;
    const float kx = 1232 - kw, cx = kx - 12 - cwid, py = 708;
    if (Hit("##addkeep", kx, py, kw, 44, &hv)) AddKeep();
    Rect(kx, py, kw, 44, hv ? U(Hex(0xE6E6E6)) : white, 22);
    Rect(kx + 22, py + 17, 10, 10, IM_COL32(0, 0, 0, 255));
    Text(600, 16, 20, kx + 22 + 10 + 10, py + 12, IM_COL32(0, 0, 0, 255), "Keep this pair");
    if (s_h.screen != SCR_ADD) return;                       // kept: HOME draws from the next frame
    if (Hit("##addcancel", cx, py, cwid, 44, &hv)) { AddCancel(); return; }
    RectLine(cx, py, cwid, 44, hv ? white : U(kEdge), 22, 2);
    Text(600, 16, 20, cx + 22, py + 12, white, "Cancel");
}

// ============================================================================================ fonts
// ImGui sizes a font by its line height (hhea ascent - descent); CSS sizes it by the em. s_f.k converts (DESIGN.md's
// sizes are CSS px). Read from the TTF's own head / hhea tables.
void FontMetrics(const unsigned char* d, size_t n) {
    auto u16 = [&](size_t o) { return o + 1 < n ? (unsigned)(d[o] << 8 | d[o + 1]) : 0u; };
    auto s16 = [&](size_t o) { int v = (int)u16(o); return v >= 32768 ? v - 65536 : v; };
    auto u32 = [&](size_t o) { return (size_t)u16(o) << 16 | u16(o + 2); };
    unsigned upem = 0;
    int asc = 0, desc = 0;
    const unsigned tables = u16(4);
    for (unsigned t = 0; t < tables; t++) {
        const size_t r = 12 + 16 * (size_t)t;
        if (r + 16 > n) break;
        const size_t off = u32(r + 8);
        if (!memcmp(d + r, "head", 4)) upem = u16(off + 18);
        if (!memcmp(d + r, "hhea", 4)) { asc = s16(off + 4); desc = s16(off + 6); }
    }
    if (upem && asc > desc) { s_f.k = (float)(asc - desc) / (float)upem; s_f.asc = (float)asc / (float)upem; }
}
ImFont* AddResFont(int id, bool metrics) {
    HRSRC rs = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL rg = rs ? LoadResource(nullptr, rs) : nullptr;
    void* data = rg ? LockResource(rg) : nullptr;
    const DWORD size = rs ? SizeofResource(nullptr, rs) : 0;
    if (!data || !size) return nullptr;
    if (metrics) FontMetrics((const unsigned char*)data, size);
    ImFontConfig fc;
    fc.FontDataOwnedByAtlas = false;            // the exe's own resource memory: never freed
    fc.OversampleH = 2;
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, (int)size, 16.0f, &fc);
}

// ============================================================================================ the now image (WIC)
bool LoadImageFile(ID3D11Device* dev, const std::wstring& path) {
    s_h.nowImg.Reset();
    if (!dev || path.empty()) return false;
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> fr;
    ComPtr<IWICFormatConverter> cv;
    if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &fr)) || FAILED(f->CreateFormatConverter(&cv)) ||
        FAILED(cv->Initialize(fr.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return false;
    UINT w = 0, h = 0;
    cv->GetSize(&w, &h);
    std::vector<uint8_t> px((size_t)w * h * 4);
    if (FAILED(cv->CopyPixels(nullptr, w * 4, (UINT)px.size(), px.data()))) return false;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = { px.data(), w * 4, 0 };
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(dev->CreateTexture2D(&td, &sd, &tex))) return false;
    if (FAILED(dev->CreateShaderResourceView(tex.Get(), nullptr, &s_h.nowImg))) return false;
    s_h.imgW = (int)w; s_h.imgH = (int)h;
    return true;
}

std::string J(const std::string& s) {
    std::string o = "\"";
    for (unsigned char ch : s) {
        if (ch == '"' || ch == '\\') { o += '\\'; o += (char)ch; }
        else if (ch < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", ch); o += b; }
        else o += (char)ch;
    }
    return o + "\"";
}

} // namespace

// ============================================================================================ public
int  UiHomeScreen() { return s_h.screen; }
void UiHomeSetScreen(int screen) {
    if (screen < SCR_HOME || screen > SCR_ADD) return;
    if (screen != s_h.screen && !(screen == SCR_HOME && s_h.screen == SCR_ADD)) s_h.status.clear();   // a keep's line stays
    if (screen != SCR_LOOKS) s_h.removeAsk.clear();
    s_h.screen = screen;
}
const char* UiHomeScreenName(int screen) {
    static const char* const k[] = { "home", "looks", "display", "advanced", "add" };
    return screen >= 0 && screen <= SCR_ADD ? k[screen] : "?";
}
void UiHomeOnOpen() {
    s_h.screen = SCR_HOME; s_h.selName.clear(); s_h.status.clear(); s_h.gridRow = 0; s_h.looksScroll = 0;
    s_h.gridToSel = true; s_h.removeAsk.clear();
    SnapReset();                          // B5: the first HOME frame asks the renderer for a picture
}
void UiHomeWindowClosed() {
    SnapReset();
    if (g_renderer) g_renderer->ReleaseSnapshot();   // B5: its readback buffer exists only while the window is open
}
ImVec2 UiHomeAddFieldPoint() { return ImVec2(kFieldX + 120, kFieldY + kFieldH * 0.5f); }
ImVec2 UiHomeBlankPoint() { return ImVec2(1150, 600); }   // on ADD A PAIR: black, no control (a click ends typing)

void UiHomeLoadFonts() {
    s_f = Fonts();
    s_f.r = AddResFont(205, true);
    s_f.s = AddResFont(206, false);
    s_f.b = AddResFont(207, false);
    if (!s_f.r && !s_f.s && !s_f.b) s_f.k = 1.0f;   // no bundled font: ImGui's own, sized as it is
}
void UiHomeDeviceCreated(ID3D11Device* dev) {
    if (!s_h.nowImagePath.empty()) LoadImageFile(dev, s_h.nowImagePath);
    s_snap.dev = dev;                     // B5: the snapshot textures live on this device
    if (s_snap.haveLatest) s_snap.haveLatest = UploadPic(s_snap.now, s_snap.latest);   // a rebuilt device keeps the picture
}
void UiHomeDeviceReleased() {
    s_h.nowImg.Reset();
    s_snap.now = Tex2();
    s_snap.tile = Tex2();
    s_snap.tileKey.clear();
    s_snap.dev.Reset();
}
void UiHomeSetNowImage(const std::wstring& path) { s_h.nowImagePath = path; }
void UiHomeSetActs(bool on) { s_h.acts = on; }
ImVec2 UiHomeToWindow(float x, float y, float w, float h) {
    const float s = std::min(w / 1280.0f, h / 800.0f);
    return ImVec2(floorf((w - 1280.0f * s) * 0.5f) + x * s, floorf((h - 800.0f * s) * 0.5f) + y * s);
}

void UiHomeDraw(float w, float h, bool live) {
    s_h.live = live;
    // uniform scale of the 1280 x 800 design, centred; the two fields run to the window's edges
    S = std::min(w / 1280.0f, h / 800.0f);
    OX = floorf((w - 1280.0f * S) * 0.5f);
    OY = floorf((h - 800.0f * S) * 0.5f);
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));
    ImGui::Begin("##home", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    DL = ImGui::GetWindowDrawList();
    if (s_h.screen == SCR_HOME) {
        UiModelTick();
        SnapTick();                       // B5: the live window only (a renderer); nothing headless
        const Model m = BuildModel();
        const float split = OX + 688.0f * S;
        DL->AddRectFilled(ImVec2(0, 0), ImVec2(split, h), U(m.pal.field));
        DL->AddRectFilled(ImVec2(split, 0), ImVec2(w, h), IM_COL32(0, 0, 0, 255));
        DrawColourField(m);
        DrawKeptField(m, 688);
    } else if (s_h.screen == SCR_LOOKS) {
        UiModelTick();
        SnapTick();
        const Model m = BuildModel();
        const float split = OX + 320.0f * S;
        DL->AddRectFilled(ImVec2(0, 0), ImVec2(split, h), U(m.pal.field));
        DL->AddRectFilled(ImVec2(split, 0), ImVec2(w, h), IM_COL32(0, 0, 0, 255));
        DrawLooksLeft(m, h);
        DrawLooksRight(m, w, h);
    } else if (s_h.screen == SCR_DISPLAY) {
        DrawDisplayScreen();
    } else {
        UiModelTick();
        const float split = OX + 688.0f * S;
        DL->AddRectFilled(ImVec2(0, 0), ImVec2(split, h), U(PairPal(s_add.h1, false).field));
        DL->AddRectFilled(ImVec2(split, 0), ImVec2(w, h), IM_COL32(0, 0, 0, 255));
        DrawAddScreen();
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

std::string UiHomeScript(const std::string& cmd) {
    char w[8][64] = {};
    const int n = sscanf_s(cmd.c_str(), "%63s %63s %63s %63s %63s %63s %63s %63s", w[0], 64u, w[1], 64u, w[2], 64u,
                           w[3], 64u, w[4], 64u, w[5], 64u, w[6], 64u, w[7], 64u);
    const std::string c0 = n > 0 ? w[0] : "";
    auto combo = [](const char* nm) { return UiStillFind(nm); };
    if (c0 == "screen") {
        const std::string s = n > 1 ? w[1] : "";
        if (s == "add") { AddOpen(s_h.screen == SCR_LOOKS ? SCR_LOOKS : SCR_HOME); return "ok"; }   // as its tile does
        for (int k = SCR_HOME; k <= SCR_ADD; k++) if (s == UiHomeScreenName(k)) { s_h.screen = k; return "ok"; }
        return "usage: screen home|looks|display|advanced|add";
    }
    if (c0 == "addpair") {   // addpair <name> <hue> <offset> [draft]: a pair appended as "Keep this pair" / a draft does
        if (n < 4) return "usage: addpair <name> <hue> <offset> [draft]";
        const bool draft = n >= 5 && std::string(w[4]) == "draft";
        const int i = UiPairAdd(w[1], (float)atof(w[2]), (float)atof(w[3]), true);
        if (!draft) UiPairKeep(i);
        return "ok: " + UiStillKey(i) + " index " + std::to_string(i) + (draft ? " (draft)" : " (kept)");
    }
    if (c0 == "hue") {       // hue 1|2 <deg>: a handle on ADD A PAIR (opens the screen if it is not open)
        if (n < 3 || (std::string(w[1]) != "1" && std::string(w[1]) != "2")) return "usage: hue 1|2 <deg>";
        if (s_h.screen != SCR_ADD) AddOpen(s_h.screen);
        (std::string(w[1]) == "1" ? s_add.h1 : s_add.h2) = Wrap360(roundf((float)atof(w[2])));
        return "ok";
    }
    if (c0 == "type") {      // type <text>: the name field (ui_window types into the real field when it can press)
        const size_t at = cmd.find("type ");
        const std::string t = at == std::string::npos ? std::string() : cmd.substr(at + 5);
        if (s_h.screen != SCR_ADD) AddOpen(s_h.screen);
        snprintf(s_add.name, 29, "%s", t.c_str());
        s_add.typed = !t.empty();
        return "ok (set directly)";
    }
    if (c0 == "clock") {
        int hh = 0, mm = 0;
        if (n < 2 || sscanf_s(w[1], "%d:%d", &hh, &mm) != 2) return "usage: clock HH:MM";
        s_h.fixedClockMin = hh * 60 + mm;
        s_h.scriptT0 = NowSecOfDay();
        return "ok";
    }
    if (c0 == "now") {   // now <combo> vivid|darkroom [left S] [changing] [then <combo> <mode>]
        if (n < 3) return "usage: now <combo> vivid|darkroom [left S] [changing] [then <combo> vivid|darkroom]";
        UiNowView v;
        v.combo = combo(w[1]);
        if (v.combo < 0) return "no such combo";
        v.director = true;
        v.mode = std::string(w[2]) == "darkroom" ? 1 : 0;
        v.holdLeftSec = 360.0f;
        for (int k = 3; k < n; k++) {
            const std::string a = w[k];
            if (a == "left" && k + 1 < n) v.holdLeftSec = (float)atof(w[++k]);
            else if (a == "changing") v.changing = true;
            else if (a == "then" && k + 2 < n) {
                const int c = combo(w[k + 1]);
                const int md = std::string(w[k + 2]) == "darkroom" ? 1 : 0;
                k += 2;
                if (c < 0) return "no such combo after then";
                v.nextCombo = c; v.nextMode = md; v.toCombo = c; v.toMode = md;
            } else return "unknown word: " + a;
        }
        if (v.changing) { v.holdLeftSec = -1.0f; v.nextCombo = -1; }
        UiDirectorNowScript(v);
        return "ok";
    }
    if (c0 == "keep") {   // keep SECONDS: the real keep (UiKeep), as the button sets it; keep 0 lets go
        if (n < 2) return "usage: keep SECONDS";
        UiKeep((float)atof(w[1]));
        return "ok";
    }
    if (c0 == "next") {   // next: the tray's / the window's Next (CycleNext): ends a keep or a visit first
        UiCycleNext();
        return "ok";
    }
    if (c0 == "visit") {  // visit <name words> SECONDS: a rated look (UiVisit) or Ink / Fluid (UiVisitGuest), the real call
        if (n < 3) return "usage: visit <look> SECONDS";
        std::string nm;
        for (int k = 1; k < n - 1; k++) nm += (nm.empty() ? "" : " ") + std::string(w[k]);
        for (int k = 0; k < UiRatedCount(); k++)
            if (_stricmp(RatedDisplay(k).c_str(), nm.c_str()) == 0) { UiVisit(k, (float)atof(w[n - 1])); return "ok"; }
        const int g = _stricmp(nm.c_str(), "ink") == 0 ? 0 : _stricmp(nm.c_str(), "fluid") == 0 ? 1 : -1;
        if (g < 0) return "no such look: " + nm;
        std::string why;
        if (!UiGuestAvailable(g, &why)) return "refused: " + why;
        UiVisitGuest(g, (float)atof(w[n - 1]));
        return UiGuestStage() >= 0 ? "ok" : "refused: the cycle did not take it";
    }
    if (c0 == "paused") { s_h.pausedScripted = true; return "ok"; }
    if (c0 == "rate") {   // rate <combo> [white|pair] V  (memory only: g_configReadOnly)
        if (n < 3) return "usage: rate <combo> [white|pair] V";
        const int i = combo(w[1]);
        if (i < 0) return "no such combo";
        const std::string face = w[2];
        if (face == "white" || face == "pair") {
            if (n < 4) return "usage: rate <combo> [white|pair] V";
            UiSetStillRating(i, face == "pair" ? 1 : 0, atoi(w[3]));
        } else {
            UiSetStillRating(i, 0, atoi(w[2]));
            UiSetStillRating(i, 1, atoi(w[2]));
        }
        return "ok";
    }
    if (c0 == "never") {  // never <combo> [prev V]
        if (n < 2) return "usage: never <combo> [prev V]";
        const int i = combo(w[1]);
        if (i < 0) return "no such combo";
        if (n >= 4 && std::string(w[2]) == "prev") { UiSetStillRating(i, 0, atoi(w[3])); UiSetStillRating(i, 1, atoi(w[3])); }
        if (!PairNever(i, -1)) return "refused: " + s_h.status;
        return "ok";
    }
    if (c0 == "pick") {
        if (n < 2) return "usage: pick <combo>";
        if (combo(w[1]) < 0) return "no such combo";
        s_h.selName = UiStillKey(combo(w[1]));
        return "ok";
    }
    return "";
}

std::string UiHomeDumpJson() {
    const Model m = BuildModel();
    std::string o = "\"home\": {\"screen\": " + J(UiHomeScreenName(s_h.screen)) + ", \"title\": " + J(m.title) +
                    ", \"state\": " + J(m.state) + ", \"rail\": " + J(m.railHasEnd ? m.railEnd + " " + m.railWhy : m.railWhy) +
                    ", \"automatic\": " + (m.autoOn ? "true" : "false") + ", \"paused\": " + (m.paused ? "true" : "false") +
                    ", \"field\": " + J(HexText(m.pal.field)) + ", \"strong\": " + J(HexText(m.pal.strong)) +
                    ", \"quiet\": " + J(HexText(m.pal.quiet)) + ", \"mark\": " + J(HexText(m.pal.mark)) +
                    ", \"playing\": " + J(m.playing >= 0 ? UiStillKey(m.playing) : "") +
                    ", \"selected\": " + J(m.sel >= 0 ? UiStillKey(m.sel) : "") + ", \"pairs\": [";
    for (int i = 0; i < UiStillCount(); i++) {
        char b[300];
        snprintf(b, sizeof(b), "{\"name\": \"%s\", \"anchor\": %g, \"offset\": %g, \"white\": %d, \"pair\": %d, \"director\": \"%g/%g\", \"state\": \"%s\"}",
                 UiStillKey(i).c_str(), UiStillAnchor(i), UiStillOffset(i), UiStillRating(i, 0), UiStillRating(i, 1),
                 UiStillRatingDirector(i, 0), UiStillRatingDirector(i, 1),
                 UiPairVisible(i) ? "listed" : UiPairIsDraft(i) ? "draft" : "removed");
        o += std::string(i ? ", " : "") + b;
    }
    {   // the swatch grid as drawn: the listed pairs + the Add tile, 4 columns, 2 rows visible from grid_row
        int tiles = 1;
        o += "], \"grid\": {\"tiles\": [";
        for (int i = 0; i < UiStillCount(); i++) if (UiPairVisible(i)) { o += J(UiStillKey(i)) + ", "; tiles++; }
        o += "\"Add a pair\"], \"rows\": " + std::to_string((tiles + 3) / 4) + ", \"first_visible_row\": " +
             std::to_string(s_h.gridRow) + ", \"scrolls\": " + ((tiles + 3) / 4 > 2 ? "true" : "false") + "}";
        o += ", \"pairs_source\": " + J(UiPairSource()) + ", \"pairs_last_write\": " + J(UiPairLastWrite());
    }
    char kb[256];
    snprintf(kb, sizeof(kb), ", \"keep_left_s\": %.0f, \"keep_until\": %s, \"visiting\": %s, \"visit_left_s\": %.0f",
             m.keepLeft, J(m.keepOn ? Hhmm(m.now + m.keepLeft) : "").c_str(), J(m.visiting ? m.visitName : "").c_str(), m.visitLeft);
    o += kb;
    // ui3 guest visit: the guest stage the director holds (1-based, 0 = none), its file, every stage it holds against
    // the playlist the window shows (a guest is never in it)
    snprintf(kb, sizeof(kb), ", \"guest\": {\"stage\": %d, \"file\": %s, \"stages_held\": %d, \"playlist_count\": %d}",
             UiGuestStage() + 1, J(UiGuestFile()).c_str(), UiCycleStageCountAll(), UiCycleStageCount());
    o += kb;
    o += ", \"buttons\": " + J(m.visiting ? std::string("Back to automatic now | ") + (m.paused ? "Resume" : "Pause")
                                            : std::string(m.autoOn ? "Next" : "Start automatic") +
                                                  (m.keepOn ? " | Keeping this" : m.autoOn ? " | Keep this" : "") +
                                                  (m.paused ? " | Resume" : " | Pause"));
    o += ", \"chips\": [";
    for (size_t k = 0; k < m.chips.size(); k++)
        o += (k ? ", " : "") + J(m.chips[k].label + (m.chips[k].inactive ? " (inactive: " + m.chips[k].why + ")" : "") +
                                 ((int)k == m.chipOn ? " (on)" : ""));
    o += "], \"different_from_automatic\": [";
    for (size_t k = 0; k < m.lines.size(); k++) o += (k ? ", " : "") + J(m.lines[k].what + " | " + m.lines[k].when);
    o += "], \"status\": " + J(s_h.status) + "}";
    // ALL LOOKS: every rating and its chance from the live ratings (decision 9), the queue
    const int sum = RatingSum();
    o += ",\n  \"looks\": {\"sum\": " + std::to_string(sum) + ", \"pairs\": [";
    bool firstRow = true;
    for (int i = 0; i < UiStillCount(); i++) {                  // the rows as ALL LOOKS lists them (listed pairs)
        if (!UiPairVisible(i)) continue;
        const int v0 = UiStillRating(i, 0), v1 = UiStillRating(i, 1);
        const bool added = !UiPairBuiltIn(i);
        o += std::string(firstRow ? "" : ", ") + "{\"name\": " + J(UiStillKey(i)) + ", \"vivid\": " + std::to_string(v0) +
             ", \"vivid_pct\": " + J(Pct(v0, sum)) + ", \"darkroom\": " + std::to_string(v1) + ", \"darkroom_pct\": " +
             J(Pct(v1, sum)) + ", \"show_next\": " + J(std::string(Queued(i, 0) ? "vivid up next" : "") + (Queued(i, 1) ? "darkroom up next" : "")) +
             ", \"built_in\": " + (added ? "false" : "true") + ", \"remove_link\": " + (added ? "true" : "false") + "}";
        firstRow = false;
    }
    o += "], \"remove_asking\": " + J(s_h.removeAsk.empty() ? "" : "Remove " + PairName(UiStillFind(s_h.removeAsk)) + " for good?  Remove / Keep it");
    o += ", \"other_looks\": [";
    for (int k = 0; k < UiRatedCount(); k++)
        o += std::string(k ? ", " : "") + "{\"name\": " + J(RatedDisplay(k)) + ", \"rating\": " + std::to_string(UiRatedRating(k)) +
             ", \"pct\": " + J(Pct(UiRatedRating(k), sum)) + ", \"button\": " +
             J(m.visitRated == k ? "Visiting" : (m.autoOn ? "Visit" : "Visit (inactive: automatic is off)")) + "}";
    o += "], \"visit_only\": [";                   // the Ink / Fluid pills at the bottom of the coloured side
    for (int g = 0; g < 2; g++) {
        std::string why;
        const bool have = UiGuestAvailable(g, &why);
        o += std::string(g ? ", " : "") + J(std::string(g ? "Fluid" : "Ink") + ": " +
             (m.visitGuest == g ? "visiting" : !have ? "inactive (" + why + ")" : m.autoOn ? "active" : "inactive (automatic is off)"));
    }
    o += "], \"queue\": " + J(UiQueueText()) + "}";
    // ADD A PAIR: the session (hues, colours, the name and what it would be stored under, the draft and its state)
    {
        int qpos = 0;
        const int ds = AddDraftState(&qpos);
        char b[512];
        snprintf(b, sizeof(b), ",\n  \"add\": {\"hue1\": %g, \"hue2\": %g, \"offset\": %g, \"colour1\": \"%s %s\", \"colour2\": \"%s %s\", "
                 "\"field\": \"%s\", \"from\": \"%s\", \"typed\": %s, ",
                 s_add.h1, s_add.h2, AddOffset(), UiHueName(s_add.h1).c_str(), HexText(HueColour(s_add.h1)).c_str(),
                 UiHueName(s_add.h2).c_str(), HexText(HueColour(s_add.h2)).c_str(), HexText(PairPal(s_add.h1, false).field).c_str(),
                 UiHomeScreenName(s_add.from), s_add.typed ? "true" : "false");
        o += b;
        o += "\"name\": " + J(AddDisplay()) + ", \"stored_as\": " + J(UiPairKeyFor(AddDisplay(), AddDraftAlive() ? s_add.draft : -1)) +
             ", \"draft\": " + (AddDraftAlive() ? "{\"index\": " + std::to_string(s_add.draft) + ", \"name\": " + J(UiStillKey(s_add.draft)) +
                                  ", \"vivid\": " + std::to_string(UiStillRating(s_add.draft, 0)) + ", \"darkroom\": " +
                                  std::to_string(UiStillRating(s_add.draft, 1)) + ", \"where\": " +
                                  J(ds == 2 ? "on screen" : ds == 1 ? "queued, place " + std::to_string(qpos + 1) : "added, not queued") + "}"
                               : std::string("null")) + "}";
    }
    // DISPLAY AND SYSTEM: the five rows' values as the app holds them now
    o += ",\n  \"display\": {";
    const char* const keys[5][2] = { { "general", "fps_limit" }, { "general", "pause_on_fullscreen" }, { "general", "pause_on_maximized" },
                                     { "general", "mirror_second" }, { "system", "autostart" } };
    for (int k = 0; k < 5; k++) {
        const int row = UiFindRow(keys[k][0], keys[k][1]);
        char b[96];
        snprintf(b, sizeof(b), "%s\"%s\": %g", k ? ", " : "", keys[k][1], row >= 0 ? UiValue(row) : -1.0f);
        o += b;
    }
    o += "}";
    return o;
}
