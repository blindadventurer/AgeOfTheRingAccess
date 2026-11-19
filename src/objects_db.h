#pragma once
#include <string>

// Object templates read from the mod's INI (aotr\data\ini\object\**\*.ini):
// display name (resolved through lotr.str), KindOf words and EditorSorting.
struct ObjectInfo {
    std::wstring displayName;   // "Orc Warriors"; empty when unknown
    std::string kindOf;         // " SELECTABLE HORDE INFANTRY " (spaces around every word)
    std::string editorSorting;  // "UNIT", "STRUCTURE", ...
    int buildCost = -1;         // BuildCost, a #define resolved; -1 when none
    std::wstring description;   // tooltip of the button that builds it (lotr.str), "\n" as pauses
    wchar_t hotkey = 0;         // letter after '&' in that button's label ("Build Greenwood Vineyard (&R)")
    bool contained = false;     // named by an InitialPayload or RankInfo UnitType: a horde's soldier
    bool Has(const char* kind) const { return kindOf.find(std::string(" ") + kind + " ") != std::string::npos; }
};

void ObjectsDbLoad(const std::wstring& modFolder);
// nullptr when the template is not in the mod's INI.
const ObjectInfo* ObjectsDbFind(const std::string& templateName);
// "5", or a #define of the mod's INI ("GOOD_RANK_1_COST") resolved to a number; -1 when unknown.
int ObjectsDbNumber(const std::string& value);
// How the game's own AI uses a command button: the SpecialPowerAIType of the AISpecialPowerUpdate
// modules naming it ("AI_SPECIAL_POWER_HEAL_AOE", "AI_SPELLBOOK_ARMY_BREAKER"; 6369 such lines in
// the object INI of AotR 10.0.2). The type most modules give wins. Empty when no module names it.
std::string ObjectsDbAiPowerType(const std::string& commandButton);
