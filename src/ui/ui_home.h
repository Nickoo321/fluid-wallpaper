// ui3 (2026-11-23): the Settings window's new screens. HOME (the window opens here: the colour field = what ends by
// itself, the black field = what is kept for good), ALL LOOKS / DISPLAY AND SYSTEM / ADD A PAIR (placeholders in this
// build) and the switch to ADVANCED (today's whole window, drawn by ui_window.cpp). Everything here is drawn with
// ImDrawList; ImGui::InvisibleButton gives the hit areas. Design size 1280 x 800, scaled uniformly to the window.
// Specs: build2\shots\ui3\mocks\h.html + DESIGN.md; reference/briefs/NEXT-ui3.md, UI3-PLAN.md.
#pragma once
#include <string>

struct ID3D11Device;

enum UiScreen { SCR_HOME = 0, SCR_LOOKS, SCR_DISPLAY, SCR_ADVANCED, SCR_ADD };
int  UiHomeScreen();
void UiHomeSetScreen(int screen);
const char* UiHomeScreenName(int screen);
// the window was opened: HOME, the playing pair selected
void UiHomeOnOpen();
// the live window closed (WM_DESTROY): the renderer's snapshot readback is released (B5)
void UiHomeWindowClosed();
// after the default (Segoe) font is in the atlas: Schibsted Grotesk 400 / 600 / 700 from RCDATA 205-207
void UiHomeLoadFonts();
void UiHomeDeviceCreated(ID3D11Device* dev);   // the --ui-now-image texture
void UiHomeDeviceReleased();
// draws HOME / LOOKS / DISPLAY / ADD into a full-client ImGui window; live = a real window (clicks act)
void UiHomeDraw(float w, float h, bool live);
// headless script commands: screen | clock | now | keep | visit | paused | rate | never | pick.
// Returns "" when the command is not one of these, else "ok" or an error.
std::string UiHomeScript(const std::string& cmd);
void UiHomeSetNowImage(const std::wstring& path);   // --ui-now-image <jpg|png>
// headless --ui-script click / drag: the controls act while a script presses them (a headless shot otherwise only
// draws); and the window pixel of a design point (1280 x 800, scaled and centred as UiHomeDraw lays it out)
void UiHomeSetActs(bool on);
struct ImVec2;
ImVec2 UiHomeToWindow(float x, float y, float w, float h);
// ADD A PAIR, headless "type <text>": where the name field is, and a point with no control (design px)
ImVec2 UiHomeAddFieldPoint();
ImVec2 UiHomeBlankPoint();
// the "home" block for --ui-dump (one JSON member, no trailing comma)
std::string UiHomeDumpJson();
