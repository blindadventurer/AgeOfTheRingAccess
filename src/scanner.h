#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

// Battle scanner: lists the objects around the selected unit by category, nearest
// first, with distance and map direction. Keys (battle only):
//   PageDown / PageUp            next / previous object
//   Ctrl+PageDown / Ctrl+PageUp  next / previous category
//   Home                         repeat the current object with a fresh distance
//   Shift+Home                   what the current object is for (its build button's tooltip)
//   End                          status: money and selection; while placing a building,
//                                its name, cost and what it is for
//   Ctrl+Home                    camera and mouse cursor to the current object
//   . (period, the Russian Ю key)  selects the current object without a click, also what
//                                stands behind a fortress
//   Ctrl+End                     camera to it and the selected units' order: a right click on
//                                an object, a plain move order (MSG_DO_MOVETO) to a place
//   Ctrl+Shift+End               route walk through the objective's guide places up to this one
//                                (on an objective: all its places); a hand-written map path
//                                (guides.ini path =) is walked at once, a found one (autopath =)
//                                when the move orders stall. Ctrl+Home/End on an objective use
//                                its guide place nearest to the selected unit. Legs over 80 m
//                                follow the map's roads (roads.txt) when they are not much
//                                longer; a stalled straight leg is tried once along the roads.
// Categories: мои, цели (labels.ini: quest markers, riddle answers), чужие, нейтральные,
// события (radar events), задания (shown objectives). Shift+PageDown/PageUp: subcategory.
// Also speaks the building picked for placement (a build hotkey) with its cost.
// modDir: rotwk\scripts\AotRAccess, where labels.ini names map objects by template.
void ScannerInit(HWND gameWindow, const std::wstring& modDir);
void ScannerTick();  // about every 100 ms: speaks selection changes
void ScannerKeys(bool gameFocused);
// Writes the current scan (every entry with id, name, owner, distance) into a dump.
void ScannerDump(FILE* f);
// Faction of the local player ("Mirkwood": player+0x58, same word as PlayerTemplate Side); empty outside a game.
std::string ScannerLocalFaction();

// Test aids: Ctrl+Shift+F11 writes a numbered mark with the mission state into the log;
// Ctrl+Shift+F10 twice within 3 s skips the mission as won.
void ScannerTestKeys(bool gameFocused);

// For the command panel mode (commands.cpp).
std::vector<uint32_t> ScannerSelection();  // selected objects (TheInGameUI selection list)
std::vector<std::string> ScannerObjectiveTags();  // the mission's objective tags, in map.ini order
std::wstring ScannerNameOf(uint32_t obj);
std::string ScannerTemplateName(uint32_t obj);
std::string ScannerCommandSetName(uint32_t obj);  // the template's CommandSet (template +0x70)
// First object of the local player whose template name contains part ("SpellBook").
uint32_t ScannerOwnObjectNamed(const char* part);
bool ScannerPosition(uint32_t obj, float& x, float& y, float& z);
constexpr int kTargetEnemy = 1, kTargetAlly = 2, kTargetNeutral = 4;
struct ScanTarget {
    uint32_t addr = 0;
    float x = 0, y = 0, z = 0, metres = 0;
    std::wstring name, direction;
    bool building = false;  // a STRUCTURE
    bool hidden = false;    // under the shroud for the local player
    bool mine = false;      // the local player's own
};
// Scanner objects of the given kinds, nearest to `from` (else the camera) first. Enemy:
// another side of the other camp (evil: Mordor, Isengard, Wild, Guldur, Angmar, Harad, Morgul,
// Rhun, Rhudaur), and neutral creatures that are not buildings; ally: own objects and other
// sides of the same camp.
std::vector<ScanTarget> ScannerTargets(int kinds, uint32_t from);
// For a builder's Extinguish Fire: the fires of the open objectives (guide places "потушить ...",
// guide objects "огонь ..."), nearest first; with none, the spot by the builder and own buildings.
std::vector<ScanTarget> ScannerFires(uint32_t from);
// For the training menu. The local player, 0 outside a game; ThePlayerList's address; every
// object of the local player, horde soldiers too (call on the game thread).
uint32_t ScannerLocalPlayer();
uintptr_t ScannerPlayerList();
void ScannerForEachOwnObject(const std::function<void(uint32_t)>& fn);
void ScannerForEachObject(const std::function<void(uint32_t)>& fn);
std::vector<uint32_t> ScannerVisibleEnemies();  // units of "чужие", with the soldiers of their hordes
// One scan for the tactics module: own objects (buildings too) and enemy units (other camp and
// wild creatures, no buildings, nothing the story filter hides). seen: in the clear of the local
// player's shroud now (status 0), so a report names only what a sighted player sees; all seen when
// the shroud cannot be read.
struct TacticalUnit {
    uint32_t addr = 0;
    float x = 0, y = 0;
    std::string tmpl;
    std::wstring name;
    bool enemy = false, seen = true, building = false;
};
std::vector<TacticalUnit> ScannerTactical();
std::wstring ScannerDirection(float dx, float dy);  // "северо-запад"
std::wstring ScannerMetres(float units);            // "158 метров"
bool ScannerObjectDead(uint32_t obj);
std::string ScannerPlayerId(uint32_t player);       // "PlyrCreeps", "Player_1"
uint32_t ScannerTeamOfSide(const char* side);       // the team of an object of that side, or 0
// The scanner's current entry, when it has a map position.
bool ScannerCurrentPlace(float& x, float& y, float& z, std::wstring& name);
// Camera onto the point, then a click at the window centre.
bool ScannerClickAt(float x, float y, float z, bool right);

// War of the Ring map (wotr.cpp): camera onto a point of the map, click at the window centre.
bool ScannerCameraTo(float x, float y);
void ScannerClickCentre(bool right);
// In battle: Enter is a left click and Backspace a right click at the window centre, the point
// the camera looks at. The game binds them to chat, which opens nothing outside multiplayer.
void ScannerClickKeys(bool gameFocused);
// = (the key with plus) camera as close as it goes, - as far as it goes, in battle and on the
// War of the Ring map. Sounds get louder and clearer close to the ground (player, 2026-11-05).
void CameraKeys(bool gameFocused);
// The camera to a point on the War of the Ring map, past the view's area limit.
bool ScannerCameraToFree(float x, float y);
