#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fstream>
#include <map>
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <set>
#include <sstream>
#include <vector>
#include "log.h"
#include "objects_db.h"
#include "scanner.h"
#include "spells.h"
#include "strings.h"

namespace {

struct Faction {
    std::string spSet, mpSet, ringHero;
    std::vector<std::string> heroes;
};
struct Science { std::string cost, costMp, prereq; };
using Button = CommandButtonInfo;

std::map<std::string, Faction> g_factions;                 // upper-case Side -> purchase sets
std::map<std::string, std::map<int, std::string>> g_sets;  // upper-case CommandSet -> slot -> button
std::map<std::string, Button> g_buttons;                   // upper-case CommandButton
std::map<std::string, Science> g_sciences;                 // upper-case Science
std::map<std::string, std::string> g_upgradeCosts;         // upper-case Upgrade -> BuildCost value

std::string Upper(std::string s) {
    for (auto& c : s) c = (char)toupper((unsigned char)c);
    return s;
}

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// Calls block(kind, name) on each "Kind Name" line and field(key, value) on each "key = value"
// line inside a block; blocks end at "End".
// '#include "\includes\x.inc"' lines are followed, relative to the including file's folder
// (commandset.ini lines 37-39 pull in the foundation, inn and high pass sets; upgrade.ini lines
// 7, 8 and 2981 pull in upgrade.inc, oldupgrade.inc and CreateAHeroUpgrades.inc).
template <class B, class F>
void ParseIni(const std::wstring& path, B block, F field, int depth = 0) {
    std::ifstream f(path, std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        std::string raw = Trim(line);
        if (raw.size() > 8 && _strnicmp(raw.c_str(), "#include", 8) == 0) {
            std::string inc = Trim(raw.substr(8));
            inc.erase(std::remove(inc.begin(), inc.end(), '"'), inc.end());
            while (!inc.empty() && (inc[0] == '\\' || inc[0] == '/')) inc.erase(0, 1);
            size_t slash = path.find_last_of(L"\\/");
            std::wstring dir = slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1);
            if (!inc.empty() && depth < 4) ParseIni(dir + Widen(inc.c_str()), block, field, depth + 1);
            continue;
        }
        // Both comment marks: commandset.ini has "//Main Menu" lines inside its blocks.
        size_t c = (std::min)(line.find(';'), line.find("//"));
        if (c != std::string::npos) line = line.substr(0, c);
        std::string t = Trim(line);
        if (t.empty()) continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) {
            std::istringstream in(t);
            std::string kind, name;
            in >> kind >> name;
            block(Upper(kind), name);
        } else {
            field(Upper(Trim(t.substr(0, eq))), Trim(t.substr(eq + 1)));
        }
    }
}

// Command sets and buttons a mission's map.ini defines for itself (Lord of the Rings: Sauron's
// dialogue answers; Council of Elrond: Bilbo's Ring), with the map's objective tags, by which
// the mod tells which mission runs. They win over the global ones while that mission runs.
struct MapCommands {
    std::set<std::string> tags;  // upper-case MissionObjectiveTag / BonusMissionObjectiveTag
    std::map<std::string, std::map<int, std::string>> sets;
    std::map<std::string, Button> buttons;
};
std::map<std::string, MapCommands> g_maps;  // map folder -> its own commands
const MapCommands* g_curMap = nullptr;

void SetField(Button& b, const std::string& key, const std::string& value) {
    if (key == "TEXTLABEL") b.textKey = value;
    else if (key == "DESCRIPTLABEL") b.descriptKey = value;
    else if (key == "SCIENCE") b.science = value;
    else if (key == "COMMAND") b.command = Upper(value);
    else if (key == "OPTIONS") b.options = " " + Upper(value) + " ";
    else if (key == "OBJECT") b.object = value;
    else if (key == "UPGRADE") b.upgrade = value;
    else if (key == "AUTOABILITY") b.autoAbility = Upper(value) == "YES";
    else if (key == "COMMANDRANGESTART") b.rangeStart = atoi(value.c_str());
    else if (key == "COMMANDRANGECOUNT") b.rangeCount = atoi(value.c_str());
}

void LoadMapCommands(const std::wstring& mapsDir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((mapsDir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    int sets = 0, buttons = 0;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L".." || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        MapCommands m;
        std::string kindNow, cur;
        ParseIni(mapsDir + L"\\" + n + L"\\map.ini", [&](const std::string& kind, const std::string& name) {
            kindNow = kind;
            cur = kind == "COMMANDBUTTON" || kind == "COMMANDSET" ? Upper(name) : std::string();
        }, [&](const std::string& key, const std::string& value) {
            if (key == "MISSIONOBJECTIVETAG" || key == "BONUSMISSIONOBJECTIVETAG") m.tags.insert(Upper(value));
            if (cur.empty()) return;
            if (kindNow == "COMMANDBUTTON") SetField(m.buttons[cur], key, value);
            else if (!key.empty() && isdigit((unsigned char)key[0])) m.sets[cur][atoi(key.c_str())] = value;
        });
        if (m.sets.empty() && m.buttons.empty()) continue;
        sets += (int)m.sets.size();
        buttons += (int)m.buttons.size();
        g_maps[Utf8(n)] = std::move(m);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    LogLine(L"spells: " + std::to_wstring(g_maps.size()) + L" missions define their own commands: " +
            std::to_wstring(sets) + L" command sets, " + std::to_wstring(buttons) + L" buttons");
}

void LoadButtons(const std::wstring& path) {
    std::string cur;
    ParseIni(path, [&](const std::string& kind, const std::string& name) {
        cur = kind == "COMMANDBUTTON" ? Upper(name) : std::string();
    }, [&](const std::string& key, const std::string& value) {
        if (!cur.empty()) SetField(g_buttons[cur], key, value);
    });
}

void LoadUpgrades(const std::wstring& path) {
    std::string cur;
    ParseIni(path, [&](const std::string& kind, const std::string& name) {
        cur = kind == "UPGRADE" ? Upper(name) : std::string();
    }, [&](const std::string& key, const std::string& value) {
        if (!cur.empty() && key == "BUILDCOST") g_upgradeCosts[cur] = value;
    });
}

void WalkButtons(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        std::wstring p = dir + L"\\" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (_wcsicmp(n.c_str(), L"object") != 0) WalkButtons(p);
        } else if (_wcsicmp(n.c_str(), L"commandbutton.ini") == 0 ||
                   (n.size() > 4 && _wcsicmp(n.c_str() + n.size() - 4, L".inc") == 0 && n.find(L"button") != std::wstring::npos)) {
            LoadButtons(p);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// "Rallying Call (&R)" -> "Rallying Call"; "\n" -> pause.
std::wstring Clean(std::wstring t) {
    size_t a = t.find(L"(&");
    if (a != std::wstring::npos) {
        size_t b = t.find(L')', a);
        t.erase(a, b == std::wstring::npos ? std::wstring::npos : b - a + 1);
    }
    for (size_t p; (p = t.find(L'&')) != std::wstring::npos;) t.erase(p, 1);
    for (size_t p; (p = t.find(L"\\n")) != std::wstring::npos;) t.replace(p, 2, L". ");
    while (!t.empty() && t.back() == L' ') t.pop_back();
    return t;
}

// Readable name of a spell science: the label of the button that buys it.
std::wstring ScienceName(const std::string& upperScience) {
    for (const auto& b : g_buttons)
        if (Upper(b.second.science) == upperScience && !b.second.textKey.empty()) {
            std::wstring n = Clean(LookupStr(b.second.textKey));
            if (!n.empty()) return n;
        }
    return {};
}

// "SCIENCE_GOOD OR SCIENCE_MIRKWOOD SCIENCE_MirkwoodLights OR SCIENCE_MIRKWOOD SCIENCE_MirkwoodArrows":
// alternatives split by OR, all words of one alternative needed. Faction gates (SCIENCE_GOOD,
// SCIENCE_MIRKWOOD: no button buys them) are left out, so this says "Lights или Arrows".
std::wstring Prerequisites(const std::string& prereq) {
    std::istringstream in(prereq);
    std::string w;
    std::vector<std::wstring> alternatives;
    std::wstring cur;
    auto flush = [&] {
        if (!cur.empty() && std::find(alternatives.begin(), alternatives.end(), cur) == alternatives.end())
            alternatives.push_back(cur);
        cur.clear();
    };
    while (in >> w) {
        std::string u = Upper(w);
        if (u == "OR") { flush(); continue; }
        std::wstring name = ScienceName(u);
        if (name.empty()) continue;
        if (!cur.empty()) cur += L" и ";
        cur += name;
    }
    flush();
    std::wstring out;
    for (const auto& alt : alternatives) out += (out.empty() ? L"" : L" или ") + alt;
    return out;
}

// "Command_PurchaseSpellCampaignSunflare" -> "Campaign Sunflare" when lotr.str has no label.
std::wstring NameFromButton(std::string n) {
    for (const char* p : {"Command_PurchaseSpell", "Command_Purchase", "Command_"})
        if (n.compare(0, strlen(p), p) == 0) { n = n.substr(strlen(p)); break; }
    std::string out;
    for (size_t i = 0; i < n.size(); ++i) {
        if (i && isupper((unsigned char)n[i]) && islower((unsigned char)n[i - 1])) out += ' ';
        out += n[i] == '_' ? ' ' : n[i];
    }
    return Widen(out.c_str());
}

}  // namespace

void SpellsLoad(const std::wstring& modFolder) {
    std::wstring ini = modFolder + L"\\data\\ini";
    // Side comes before the purchase sets in every PlayerTemplate block.
    std::string cur, side;
    ParseIni(ini + L"\\playertemplate.ini", [&](const std::string& kind, const std::string&) {
        if (kind == "PLAYERTEMPLATE") side.clear();
    }, [&](const std::string& key, const std::string& value) {
        if (key == "SIDE") side = Upper(value);
        else if (!side.empty() && key == "PURCHASESCIENCECOMMANDSET") g_factions[side].spSet = Upper(value);
        else if (!side.empty() && key == "PURCHASESCIENCECOMMANDSETMP") g_factions[side].mpSet = Upper(value);
        else if (!side.empty() && key == "BUILDABLEHEROESMP") {
            std::istringstream in(value);
            g_factions[side].heroes.clear();
            for (std::string h; in >> h;)
                if (h != "CreateAHero") g_factions[side].heroes.push_back(h);
        } else if (!side.empty() && key == "BUILDABLERINGHEROESMP") {
            std::istringstream in(value);
            in >> g_factions[side].ringHero;
        }
    });
    ParseIni(ini + L"\\commandset.ini", [&](const std::string& kind, const std::string& name) {
        cur = kind == "COMMANDSET" ? Upper(name) : std::string();
    }, [&](const std::string& key, const std::string& value) {
        if (!cur.empty() && !key.empty() && isdigit((unsigned char)key[0])) g_sets[cur][atoi(key.c_str())] = value;
    });
    WalkButtons(ini);
    ParseIni(ini + L"\\science.ini", [&](const std::string& kind, const std::string& name) {
        cur = kind == "SCIENCE" ? Upper(name) : std::string();
    }, [&](const std::string& key, const std::string& value) {
        if (cur.empty()) return;
        Science& s = g_sciences[cur];
        if (key == "SCIENCEPURCHASEPOINTCOST") s.cost = value;
        else if (key == "SCIENCEPURCHASEPOINTCOSTMP") s.costMp = value;
        else if (key == "PREREQUISITESCIENCES") s.prereq = value;
    });
    LoadUpgrades(ini + L"\\upgrade.ini");  // its #include lines bring the .inc files
    LoadMapCommands(modFolder + L"\\maps");
    LogLine(L"spells: " + std::to_wstring(g_factions.size()) + L" factions with a spell store, " +
            std::to_wstring(g_sets.size()) + L" command sets, " + std::to_wstring(g_buttons.size()) + L" buttons, " +
            std::to_wstring(g_sciences.size()) + L" sciences, " + std::to_wstring(g_upgradeCosts.size()) + L" upgrades");
}

std::string CommandsUseMission(const std::vector<std::string>& objectiveTags) {
    g_curMap = nullptr;
    for (const auto& kv : g_maps)
        for (const std::string& t : objectiveTags)
            if (kv.second.tags.count(Upper(t))) {
                g_curMap = &kv.second;
                return kv.first;
            }
    return {};
}

bool CommandsUseMapWith(const std::string& set) {
    std::string u = Upper(set);
    for (const auto& kv : g_maps)
        if (kv.second.sets.count(u)) {
            if (g_curMap != &kv.second) LogLine(L"spells: " + Widen(set.c_str()) + L" comes from " + Widen(kv.first.c_str()));
            g_curMap = &kv.second;
            return true;
        }
    return false;
}

const std::map<int, std::string>* CommandSetFind(const std::string& name) {
    std::string u = Upper(name);
    if (g_curMap) {
        auto m = g_curMap->sets.find(u);
        if (m != g_curMap->sets.end()) return &m->second;
    }
    auto it = g_sets.find(u);
    return it == g_sets.end() ? nullptr : &it->second;
}

const CommandButtonInfo* CommandButtonFind(const std::string& name) {
    std::string u = Upper(name);
    if (g_curMap) {
        auto m = g_curMap->buttons.find(u);
        if (m != g_curMap->buttons.end()) return &m->second;
    }
    auto it = g_buttons.find(u);
    return it == g_buttons.end() ? nullptr : &it->second;
}

std::wstring CommandButtonLabel(const std::string& name, const CommandButtonInfo& b, wchar_t& hotkey) {
    // A toggle names several labels ("CONTROLBAR:ToggleStanceHoldGround CONTROLBAR:..."): the first.
    std::string key = b.textKey.substr(0, b.textKey.find(' '));
    std::wstring raw = key.empty() ? std::wstring() : LookupStr(key);
    hotkey = 0;
    size_t amp = raw.find(L'&');
    if (amp != std::wstring::npos && amp + 1 < raw.size() && iswalnum(raw[amp + 1])) hotkey = towupper(raw[amp + 1]);
    std::wstring label = Clean(raw);
    return label.empty() ? NameFromButton(name) : label;
}

std::string ReviveHero(const std::string& button, int position) {
    if (button.find("CreateAHero") != std::string::npos) return "CreateAHero";
    auto f = g_factions.find(Upper(ScannerLocalFaction()));
    if (f == g_factions.end()) return {};
    if (button.find("RingHero") != std::string::npos) return f->second.ringHero;
    return position >= 1 && (size_t)position <= f->second.heroes.size() ? f->second.heroes[position - 1] : std::string();
}

std::wstring CommandButtonDescription(const CommandButtonInfo& b) {
    return b.descriptKey.empty() ? std::wstring() : Clean(LookupStr(b.descriptKey));
}

int UpgradeCost(const std::string& upgrade) {
    auto it = g_upgradeCosts.find(Upper(upgrade));
    return it == g_upgradeCosts.end() ? -1 : ObjectsDbNumber(it->second);
}

bool SpellStoreSlot(int slot, bool skirmish, std::wstring& label, std::wstring& details) {
    std::string faction = Upper(ScannerLocalFaction());
    auto f = g_factions.find(faction);
    if (f == g_factions.end()) return false;
    const std::string& setName = skirmish && !f->second.mpSet.empty() ? f->second.mpSet : f->second.spSet;
    auto set = g_sets.find(setName);
    if (set == g_sets.end()) return false;
    auto slotIt = set->second.find(slot);
    if (slotIt == set->second.end()) return false;
    if (!SpellStoreButtonWords(slotIt->second, skirmish, label, details)) return false;
    static std::string logged;
    std::string key = setName + "#" + std::to_string(slot);
    if (logged.find("|" + key + "|") == std::string::npos) {
        logged += "|" + key + "|";
        LogLine(L"spells: " + Widen(faction.c_str()) + L" " + Widen(setName.c_str()) + L" slot " +
                std::to_wstring(slot) + L" = " + Widen(slotIt->second.c_str()) + L" -> " + label);
    }
    return true;
}

bool SpellStoreButtonWords(const std::string& button, bool skirmish, std::wstring& label, std::wstring& details) {
    auto b = g_buttons.find(Upper(button));
    if (b == g_buttons.end()) return false;
    label = Clean(LookupStr(b->second.textKey));
    if (label.empty()) label = NameFromButton(button);
    auto s = g_sciences.find(Upper(b->second.science));
    if (s != g_sciences.end()) {
        int cost = ObjectsDbNumber(skirmish && !s->second.costMp.empty() ? s->second.costMp : s->second.cost);
        if (cost >= 0) label += L", " + std::to_wstring(cost) + L" очк.";
        std::wstring pre = Prerequisites(s->second.prereq);
        details = Clean(LookupStr(b->second.descriptKey));
        if (!pre.empty()) details += L" Сначала нужно: " + pre + L".";
    } else {
        details = Clean(LookupStr(b->second.descriptKey));
    }
    return true;
}
