#pragma once
#include <string>
#include <utility>
#include <vector>

// Game text: lotr.str (key -> displayed text) and the instance-name -> "$Key"
// pairs found in the APT .const files of the mod and the game.
void StringsLoad(const std::wstring& modFolder, const std::wstring& gameFolder);

// "APT:Skirmish" -> L"SKIRMISH"; empty when the key is unknown.
std::wstring LookupStr(const std::string& key);

// Text for an Apt instance name: "APT:<name>", then the name without common
// prefixes/suffixes ("tooltipsEAX3" -> EAX3, "SoloPlayNav" -> SoloPlay), then a
// unique key at most 3 letters longer. Empty when nothing fits.
std::wstring LookupAptName(const std::string& name);

// A map from the mod's maps\mapcache.ini, named as the map list shows it.
struct MapInfo {
    std::wstring name;         // display name (lotr.str "Map:...")
    std::wstring description;  // lotr.str "Map:.../Desc"
    int players = 0;
    bool adventure = false;    // folder "map sp ..." (single-player adventure)
    std::wstring replaced;     // the army to pick: an alternate-faction map's replaced faction, an adventure's player faction
    std::wstring role;         // who sits where on "Fortress:" and "Besiege:" maps, else empty
    std::vector<std::pair<float, float>> starts;  // Player_N_Start, mapcache.ini
    float width = 0, height = 0;                  // extentMax
};
// The map whose display name is one of the comma-separated cells of a list row.
const MapInfo* FindMapInRow(const std::wstring& rowText);

// Label key ("$Skirmish") that a movie assigns to an instance name, first in the
// given movie ("MainMenu"), then in any movie. Empty when none.
std::string ConstKeyFor(const std::string& movie, const std::string& instanceName);
