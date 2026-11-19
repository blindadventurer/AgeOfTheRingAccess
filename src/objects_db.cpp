// Reads object templates from the mod's INI files. Only the first top-level
// DisplayName, KindOf and EditorSorting of each "Object", "ChildObject" or
// "ObjectReskin" block are kept (module blocks come later in a template and have
// their own KindOf filters). ChildObject/ObjectReskin inherit what they lack.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <vector>
#include "log.h"
#include "objects_db.h"
#include "strings.h"

namespace {

struct Raw {
    std::string parent, displayKey, kindOf, sorting, buildCost;
};

std::map<std::string, Raw> g_raw;            // upper-case name -> raw fields
std::map<std::string, ObjectInfo> g_objects;  // upper-case name -> resolved
std::set<std::string> g_contained;            // upper-case names a horde or garrison spawns inside itself
std::map<std::string, std::string> g_defines;  // "#define GOBLIN_MINE_BUILDCOST 300" (gamedata.ini and others)
// upper-case command button -> AI type -> how many AISpecialPowerUpdate modules say so
std::map<std::string, std::map<std::string, int>> g_aiPower;

std::string Upper(std::string s) {
    for (auto& c : s) c = (char)toupper((unsigned char)c);
    return s;
}

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

void ParseFile(const std::wstring& path, int& objects) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    std::string line, current;
    bool haveDisplay = false, haveKind = false, haveSort = false, haveCost = false;
    // "Behavior = AISpecialPowerUpdate X" then CommandButtonName and SpecialPowerAIType, in any order.
    bool aiModule = false;
    std::string aiButton, aiType;
    auto aiFlush = [&] {
        if (aiModule && !aiButton.empty() && !aiType.empty()) ++g_aiPower[aiButton][aiType];
        aiModule = false;
        aiButton.clear();
        aiType.clear();
    };
    while (std::getline(f, line)) {
        // Both comment marks: commandset.ini has "//Main Menu" lines inside its blocks.
        size_t c = (std::min)(line.find(';'), line.find("//"));
        if (c != std::string::npos) line = line.substr(0, c);
        std::string t = Trim(line);
        if (t.empty()) continue;
        std::istringstream in(t);
        std::string word;
        in >> word;
        std::string uw = Upper(word);
        if (uw == "#DEFINE") {
            // The whole line: gamedata.ini's HOBBIT_KINDOF is a list of words.
            std::string name, value;
            in >> name;
            std::getline(in, value);
            if (!name.empty()) g_defines[Upper(name)] = Trim(value);
            continue;
        }
        if (uw == "OBJECT" || uw == "CHILDOBJECT" || uw == "OBJECTRESKIN") {
            std::string name, parent;
            in >> name >> parent;
            if (name.empty()) continue;
            current = Upper(name);
            Raw& r = g_raw[current];
            r = Raw();
            if (uw != "OBJECT") r.parent = Upper(parent);
            haveDisplay = haveKind = haveSort = haveCost = false;
            ++objects;
            continue;
        }
        if (current.empty()) continue;
        // Soldiers of a horde: "InitialPayload = MordorFighter 20", "RankInfo = ... UnitType:MordorFighter ..."
        if (uw == "INITIALPAYLOAD") {
            std::string eqSign, name;
            in >> eqSign >> name;
            if (eqSign == "=" && !name.empty()) g_contained.insert(Upper(name));
        } else if (uw == "RANKINFO") {
            for (size_t at = t.find("UnitType:"); at != std::string::npos; at = t.find("UnitType:", at + 9)) {
                std::istringstream u(t.substr(at + 9));
                std::string name;
                u >> name;
                if (!name.empty()) g_contained.insert(Upper(name));
            }
        }
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Upper(Trim(t.substr(0, eq))), val = Trim(t.substr(eq + 1));
        if (key == "BEHAVIOR") {
            aiFlush();
            aiModule = Upper(val).compare(0, 20, "AISPECIALPOWERUPDATE") == 0;
            continue;
        }
        if (aiModule && (key == "COMMANDBUTTONNAME" || key == "SPECIALPOWERAITYPE")) {
            // "AI_SPELLBOOK_ARMY_BREAKER;;,;;": the first word, stray marks cut off.
            std::string v = Upper(val.substr(0, val.find_first_of(" \t;,")));
            (key == "COMMANDBUTTONNAME" ? aiButton : aiType) = v;
            if (!aiButton.empty() && !aiType.empty()) aiFlush();
            continue;
        }
        Raw& r = g_raw[current];
        if (key == "DISPLAYNAME" && !haveDisplay) { r.displayKey = val; haveDisplay = true; }
        else if (key == "KINDOF" && !haveKind) { r.kindOf = " " + Upper(val) + " "; haveKind = true; }
        else if (key == "EDITORSORTING" && !haveSort) { r.sorting = Upper(val); haveSort = true; }
        else if (key == "BUILDCOST" && !haveCost) { r.buildCost = val; haveCost = true; }
    }
}

// "#define" lines of the INI files outside the object folder (gamedata.ini holds the costs).
void ReadDefines(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream in(line);
        std::string word, name, value;
        in >> word >> name;
        std::getline(in, value);
        value = Trim(value.substr(0, (std::min)(value.find(';'), value.find("//"))));
        if (Upper(word) == "#DEFINE" && !name.empty() && !value.empty()) g_defines[Upper(name)] = value;
    }
}

void WalkDefines(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        std::wstring p = dir + L"\\" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (_wcsicmp(n.c_str(), L"object") != 0) WalkDefines(p);
        } else if (n.size() > 4 && (_wcsicmp(n.c_str() + n.size() - 4, L".ini") == 0 ||
                                    _wcsicmp(n.c_str() + n.size() - 4, L".inc") == 0)) {
            ReadDefines(p);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// "600", or a #define name resolved through up to 4 levels; -1 when unknown.
int CostValue(const std::string& v) {
    std::string cur = v;
    for (int i = 0; i < 5 && !cur.empty(); ++i) {
        if (isdigit((unsigned char)cur[0])) return atoi(cur.c_str());
        auto it = g_defines.find(Upper(cur));
        if (it == g_defines.end()) return -1;
        cur = it->second.substr(0, it->second.find_first_of(" 	"));
    }
    return -1;
}

// Command buttons that build an object ("Object = MirkwoodVineyard"): the tooltip
// (DescriptLabel) says what it is for, the TextLabel's "&R" is its hotkey. The first
// button naming an object wins.
struct Button {
    std::string descriptKey, textKey;
};
std::map<std::string, Button> g_buttons;  // upper-case object name -> button

void ParseButtons(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    std::string line, object;
    Button cur;
    auto flush = [&] {
        if (!object.empty() && !cur.descriptKey.empty() && !g_buttons.count(object)) g_buttons[object] = cur;
        object.clear();
        cur = Button();
    };
    while (std::getline(f, line)) {
        // Both comment marks: commandset.ini has "//Main Menu" lines inside its blocks.
        size_t c = (std::min)(line.find(';'), line.find("//"));
        if (c != std::string::npos) line = line.substr(0, c);
        std::string t = Trim(line);
        if (t.empty()) continue;
        if (Upper(t.substr(0, 13)) == "COMMANDBUTTON") { flush(); continue; }
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Upper(Trim(t.substr(0, eq))), val = Trim(t.substr(eq + 1));
        if (key == "OBJECT") object = Upper(val.substr(0, val.find_first_of(" \t")));
        else if (key == "DESCRIPTLABEL") cur.descriptKey = val;
        else if (key == "TEXTLABEL") cur.textKey = val;
    }
    flush();
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
                   (n.size() > 4 && _wcsicmp(n.c_str() + n.size() - 4, L".inc") == 0 &&
                    n.find(L"button") != std::wstring::npos)) {
            ParseButtons(p);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// lotr.str keeps "\n" as two characters; speak them as a pause.
std::wstring Spoken(std::wstring s) {
    for (size_t p; (p = s.find(L"\\n")) != std::wstring::npos;) s.replace(p, 2, L". ");
    return s;
}

void Walk(const std::wstring& dir, int& files, int& objects) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        std::wstring p = dir + L"\\" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            Walk(p, files, objects);
        } else if (n.size() > 4 && _wcsicmp(n.c_str() + n.size() - 4, L".ini") == 0) {
            ++files;
            ParseFile(p, objects);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Words of a KindOf line into a set. A name from a #define stands for its list (HOBBIT_KINDOF,
// ARNOR_RANGER_KINDOF); "+X" and "-X" change the set.
void AddKinds(const std::string& list, std::set<std::string>& kinds, int depth = 0) {
    std::istringstream in(list);
    for (std::string w; in >> w;) {
        char sign = w[0] == '+' || w[0] == '-' ? w[0] : 0;
        if (sign) w.erase(0, 1);
        auto macro = g_defines.find(Upper(w));
        if (macro != g_defines.end() && depth < 4) AddKinds(macro->second, kinds, depth + 1);
        else if (sign == '-') kinds.erase(Upper(w));
        else kinds.insert(Upper(w));
    }
}

// Fills missing fields from the parent chain (ChildObject, ObjectReskin). "KindOf = +SUMMONED"
// on a child changes its parent's list instead of replacing it: 858 templates, among them every
// _AI, _WOTR and _SUMMONED horde, lost SELECTABLE that way (census 2026-11-05).
Raw Resolve(const std::string& name, int depth = 0) {
    auto it = g_raw.find(name);
    if (it == g_raw.end()) return {};
    Raw r = it->second;
    bool relative = r.kindOf.size() > 1 && (r.kindOf[1] == '+' || r.kindOf[1] == '-');
    std::string parentKinds;
    if (!r.parent.empty() && depth < 8 &&
        (relative || r.displayKey.empty() || r.kindOf.empty() || r.sorting.empty() || r.buildCost.empty())) {
        Raw p = Resolve(r.parent, depth + 1);
        if (r.buildCost.empty()) r.buildCost = p.buildCost;
        if (r.displayKey.empty()) r.displayKey = p.displayKey;
        if (r.kindOf.empty()) r.kindOf = p.kindOf;
        if (r.sorting.empty()) r.sorting = p.sorting;
        parentKinds = p.kindOf;
    }
    std::set<std::string> kinds;
    if (relative) AddKinds(parentKinds, kinds);
    AddKinds(r.kindOf, kinds);
    if (!kinds.empty()) {
        r.kindOf = " ";
        for (const std::string& k : kinds) r.kindOf += k + " ";
    }
    return r;
}

}  // namespace

void ObjectsDbLoad(const std::wstring& modFolder) {
    int files = 0, objects = 0;
    WalkDefines(modFolder + L"\\data\\ini");
    Walk(modFolder + L"\\data\\ini\\object", files, objects);
    // Pick-ups (athelas, treasures, palantir shards...) are defined outside the object folder.
    ParseFile(modFolder + L"\\data\\ini\\crate.ini", objects);
    WalkButtons(modFolder + L"\\data\\ini");
    int named = 0, costs = 0, described = 0;
    for (const auto& kv : g_raw) {
        Raw r = Resolve(kv.first);
        ObjectInfo info;
        if (!r.displayKey.empty()) info.displayName = LookupStr(r.displayKey);
        // "Building Rubble \\n Will rebuild the structure over time": the name is the first line.
        size_t line = info.displayName.find(L"\\n");
        if (line != std::wstring::npos) {
            info.displayName.resize(line);
            while (!info.displayName.empty() && info.displayName.back() == L' ') info.displayName.pop_back();
        }
        named += !info.displayName.empty();
        info.kindOf = r.kindOf;
        info.editorSorting = r.sorting;
        info.contained = g_contained.count(kv.first) > 0;
        info.buildCost = r.buildCost.empty() ? -1 : CostValue(r.buildCost);
        costs += info.buildCost >= 0;
        auto b = g_buttons.find(kv.first);
        if (b == g_buttons.end() && !r.parent.empty()) b = g_buttons.find(r.parent);
        if (b != g_buttons.end()) {
            info.description = Spoken(LookupStr(b->second.descriptKey));
            std::wstring label = LookupStr(b->second.textKey);
            size_t amp = label.find(L'&');
            if (amp != std::wstring::npos && amp + 1 < label.size()) info.hotkey = towupper(label[amp + 1]);
        }
        // Objects no button builds (lairs, plots): lotr.str often has "<DisplayName key>Description"
        // ("OBJECT:HillTrollLairDescription" = "Allows training of Hill Trolls").
        if (info.description.empty() && !r.displayKey.empty())
            info.description = Spoken(LookupStr(r.displayKey + "Description"));
        described += !info.description.empty();
        g_objects[kv.first] = info;
    }
    LogLine(L"objects: " + std::to_wstring(objects) + L" templates from " + std::to_wstring(files) + L" INI files, " +
            std::to_wstring(named) + L" with a display name, " + std::to_wstring(g_contained.size()) +
            L" spawned inside hordes or garrisons, " + std::to_wstring(costs) + L" with a cost, " +
            std::to_wstring(described) + L" with a button tooltip (" + std::to_wstring(g_buttons.size()) + L" buttons), " +
            std::to_wstring(g_defines.size()) + L" #defines, " + std::to_wstring(g_aiPower.size()) +
            L" buttons with an AI power type");
}

int ObjectsDbNumber(const std::string& value) { return CostValue(value); }

std::string ObjectsDbAiPowerType(const std::string& commandButton) {
    auto it = g_aiPower.find(Upper(commandButton));
    if (it == g_aiPower.end()) return {};
    std::string best;
    int most = 0;
    for (const auto& kv : it->second)
        if (kv.second > most) { most = kv.second; best = kv.first; }
    return best;
}

const ObjectInfo* ObjectsDbFind(const std::string& templateName) {
    auto it = g_objects.find(Upper(templateName));
    return it == g_objects.end() ? nullptr : &it->second;
}
