#pragma once
#include <map>
#include <string>
#include <vector>

// Spell store (SpellStore.apt, opened from the palantir's PlayerMagic button): its buttons
// Spell1..SpellN are the slots of the faction's purchase command set (playertemplate.ini
// PurchaseScienceCommandSet in the campaign, PurchaseScienceCommandSetMP in skirmish).
// Each slot is a PURCHASE_SCIENCE command button (commandbutton.ini: TextLabel, DescriptLabel,
// Science); the science (science.ini) has the cost in power points and its prerequisites.
// Slot order = button number: PROVEN by disassembly (AptSpellStore::OnBttnSpell 0x8235A5 takes the
// number after "Spell" minus 1 as the index into the store's buttons, filled in command set order).
void SpellsLoad(const std::wstring& modFolder);
// Label ("Rallying Call, 5 очков") and details (tooltip, prerequisites) of SpellStore button
// number slot (1-based) for the local player's faction. skirmish selects the MP set.
bool SpellStoreSlot(int slot, bool skirmish, std::wstring& label, std::wstring& details);
// The same words for a PURCHASE_SCIENCE command button named by the store itself (the campaign's
// Fellowship side has no playertemplate set the mod can find: log 2026-11-14 23:17, "Spell2" spoken).
bool SpellStoreButtonWords(const std::string& button, bool skirmish, std::wstring& label, std::wstring& details);

// Command buttons and command sets of the mod's INI (commandbutton.ini and *button*.inc,
// commandset.ini), used by the spell store and the command panel mode (commands.cpp).
struct CommandButtonInfo {
    std::string textKey, descriptKey, science;
    std::string command;   // "SPECIAL_POWER", "UNIT_BUILD", "OBJECT_UPGRADE", "SPELL_BOOK", ... (upper-case)
    std::string options;   // " NEED_TARGET_ENEMY_OBJECT NEED_TARGET_POS ... " (upper-case, spaces around)
    std::string object;    // UNIT_BUILD / *_CONSTRUCT: the template built
    std::string upgrade;   // *_UPGRADE: the upgrade bought
    bool autoAbility = false;  // AutoAbility = Yes: a right click on the button toggles autocast
    int rangeStart = -1, rangeCount = 0;  // PUSH_VISIBLE_COMMAND_RANGE: its sub-menu, counted from 0
    bool Has(const char* option) const { return options.find(std::string(" ") + option + " ") != std::string::npos; }
};
// Picks the mission whose map.ini lists one of these objective tags: its own command sets and
// buttons are then found first. Returns the map folder, empty when none matches.
std::string CommandsUseMission(const std::vector<std::string>& objectiveTags);
// A map that defines this command set for itself (alternate-faction maps have no objective tags
// to tell them by) becomes the current one. False when no map has it.
bool CommandsUseMapWith(const std::string& set);
// Slot number -> CommandButton name; nullptr when the set is not in commandset.ini.
const std::map<int, std::string>* CommandSetFind(const std::string& name);
const CommandButtonInfo* CommandButtonFind(const std::string& name);
// The button's label without its "(&C)" part, and the hotkey letter after '&' (0: none).
std::wstring CommandButtonLabel(const std::string& name, const CommandButtonInfo& b, wchar_t& hotkey);
std::wstring CommandButtonDescription(const CommandButtonInfo& b);
// The hero a REVIVE button of the player's fortress hires: "CreateAHero", the ring hero, or the
// Nth of playertemplate.ini BuildableHeroesMP without CreateAHero for Command_GenericReviveSlotN
// (HYPOTHESIS: the game fills the slots in that order). Empty when unknown.
// position: the button's place among the set's revive buttons, ring hero left out, the first
// generic slot being 1. "CreateAHero", or empty when no hero has this button.
std::string ReviveHero(const std::string& button, int position);
// BuildCost of an upgrade (upgrade.ini, #defines resolved); -1 when unknown.
int UpgradeCost(const std::string& upgrade);
