#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Quick start of War of the Ring on the setup screen (Skirmish.apt with the scenario combo).
// A small spoken wizard: scenario, your army, how many other players, their armies and sides,
// the computer's difficulty; then the mod sets the setup screen's own combos (Player N, Army N,
// Team N) through the game's list code and, if asked, presses PLAY.
// The scenarios' rules (players, allowed armies, fixed armies and teams of the historical ones)
// come from the mod's own files: data\ini\campaigns\riskcampaign.ini and its scenario includes.
void WotrSetupInit(const std::wstring& modFolder);
// Opens the wizard. scenarioCombo: the setup screen's scenario combo window.
void WotrSetupOpen(uintptr_t scenarioCombo);
bool WotrSetupIsOpen();
// Closes it without changing anything (the setup screen went away).
void WotrSetupClose();
// The wizard's keys while it is open: Up/Down rows, Enter choose, Backspace or apostrophe back,
// Left/Right enemy or ally (other players' step), Space repeat, Shift+Space description,
// a letter jumps to the next row starting with it.
void WotrSetupKeys();
// Players and rules of a scenario as the setup screen names it ("Northern Conquest"), for the
// scenario choice; empty when unknown.
std::wstring WotrScenarioSummary(const std::wstring& shownName);

// Quick start of a skirmish: the same wizard over the skirmish setup screen, a map from the
// game's map list instead of a scenario. A map row as menu.cpp reads it from that list.
struct SetupMap {
    std::wstring name, details;
    std::wstring army;  // the army this map is played with (alternate faction maps), else empty
    int row = 0;        // row in the game's map list
    int kind = 1;       // 1 ordinary, 2 fortress, 3 besiege, 4 alternate faction, 5 adventure
    int players = 0;    // most players the map holds, 0 unknown
};
void SkirmishSetupOpen(uintptr_t mapList, const std::vector<SetupMap>& maps);
