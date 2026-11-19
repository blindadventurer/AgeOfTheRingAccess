#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>

void MenuInit(HWND gameWindow);
// About every 100 ms: rebuild the button list when the screen or its buttons change.
void MenuTick();
// Every loop pass: Up/Down/Home/End move, Space repeats with position, Enter clicks,
// Ctrl+F11 switches the movie-to-screen mapping (research).
void MenuKeys(bool gameFocused);
// True while the battle HUD is on screen (menu keys are off then).
bool MenuInBattle();
// True while a screen stands over the battle (pause menu, spell store): the game is paused.
bool MenuOverlay();
// While true the navigator says nothing (War of the Ring map: the keys belong to the map).
void MenuSetQuiet(bool quiet);
// On the War of the Ring map: the HUD's decoration is left out of the list, and a focused button
// is hovered with the mouse, so the tooltip the game shows for it is read (queued, then kept).
void MenuSetWorldMap(bool on);
// For ms milliseconds a rebuild neither moves the focus nor speaks (the mod itself changed the screen).
void MenuHoldFocus(DWORD ms);

// Buttons of the button list now (battle HUD included: "?.CommandButtons.0.Button.?",
// "?.SpellBookUI.Spell1.?", "?.SideCommandBar.ButtonSet.Button3.Button.Button.?"), with the
// centre in movie coordinates (1024 x 768, stretched to the window).
struct HudButton { std::string path; float x = 0, y = 0; };
int MenuHudButtons(std::vector<HudButton>& out);
// Clicks the first button whose path contains pathPart (left, or right: autocast toggle).
bool MenuClickHud(const std::string& pathPart, bool right);
