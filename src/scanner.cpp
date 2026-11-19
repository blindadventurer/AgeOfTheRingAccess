// Battle scanner. Read only (camera and clicks go through the game's own functions).
// Layouts proven by the dumps and tests of 2026-11-01 (NOTEBOOK.md "Scanner research"):
//   TheGameLogic [0xDE412C]: object list head +0xAC; Object: vtable 0x00C123D8,
//   template +0x04 (name AsciiString at template+0x64, side at +0x6C), position
//   +0x14/+0x24/+0x34, id +0x74, next +0x8C, Drawable +0x84 (object back at drawable+0xFC).
//   ThePlayerList [0xDE4928]: local player +0x10 (vtable 0x00C141BC).
//   Player: display name UnicodeString +0x38, faction +0x58, Money at +0x90 (vtable
//   0x00BF7BD8) with the amount at +0x94.
//   Owner: obj+0x31C -> +0x30 -> +0x08 = Player (team -> prototype -> player).
//   TheInGameUI [0xDE4830]: selected drawables std::list, head node pointer at +0x20
//   (nodes {next, prev, Drawable*}), selected count at +0x568, build placement +0x53C.
//   TheRadar [0xDE4AB8]: radar events from +0x2C, 0x50 bytes each (type +0, active
//   byte +4, create frame +8, die frame +0xC, world x/y/z floats +0x34/+0x38/+0x3C);
//   type 3 = under attack, unused slots hold type 11 or 0 (dumps of 2026-11-01 04:08-04:13).
//   TheMissionObjectiveTracker [0xDE8C94]: +0x10 -> list whose +4 points to 8-byte
//   entries {AsciiString tag "SCRIPT:LAObjective00", byte +4 ?, byte +5 shown, byte +6
//   completed} (Council of Elrond log 2026-11-01 04:29: "Answer Gollum's riddles" went
//   0 -> 0x100 when the riddles began; finished stories read 0x010100).
//   Targets: objects named in labels.ini (riddle answer pictures, the mod's quest
//   markers AotRDialogueDots / AotRQuestMarker) are listed even when not selectable.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "game.h"
#include "gamethread.h"
#include "hints.h"
#include "log.h"
#include "memory.h"
#include "menu.h"
#include "nocatch.h"
#include "objects_db.h"
#include "commands.h"
#include "roads.h"
#include "scanner.h"
#include "speech.h"
#include "wotr.h"
#include "strings.h"

bool EvilSide(const std::string& side);

namespace {

constexpr uintptr_t kTheGameLogic = 0xDE412C, kThePlayerList = 0xDE4928, kTheInGameUI = 0xDE4830;
constexpr uintptr_t kTheRadar = 0xDE4AB8, kTheObjectiveTracker = 0xDE8C94;
constexpr uint32_t kObjectVtable = 0x00C123D8, kPlayerVtable = 0x00C141BC, kMoneyVtable = 0x00BF7BD8;
constexpr uint32_t kListHead = 0xAC, kObjNext = 0x8C, kObjTemplate = 0x04, kObjId = 0x74, kObjTeam = 0x31C;
constexpr uint32_t kObjX = 0x14, kObjY = 0x24, kObjZ = 0x34, kDrawableObject = 0xFC;
constexpr uint32_t kTemplateName = 0x64, kTemplateSide = 0x6C;
constexpr uint32_t kPlayerName = 0x38, kPlayerId = 0x4C, kPlayerFaction = 0x58, kPlayerMoneyVt = 0x90, kPlayerMoney = 0x94;
constexpr uint32_t kUiSelection = 0x20, kUiSelectedCount = 0x568;
constexpr uint32_t kRadarEvents = 0x2C, kRadarEventSize = 0x50;
constexpr int kRadarEventCount = 64;
// World units per metre: Tauriel's GeometryHeight is 19.2 (tauriel.ini), a person
// about 1.9 m tall; Tauriel next to a builder read 2 m in the test of 2026-11-01.
constexpr float kUnitsPerMetre = 10.f;
// A reference that moves this far (or changes) re-sorts the list nearest first.
constexpr float kResortDistance = 300.f;

enum Category { kMine, kTargets, kOthers, kAllies, kNeutral, kEvents, kObjectives, kCategories };
const wchar_t* kCategoryNames[kCategories] = {L"мои", L"цели", L"чужие", L"союзники", L"нейтральные", L"события", L"задания"};
enum Kind { kAll, kHeroes, kUnits, kBuildings, kKinds };
const wchar_t* kKindNames[kKinds] = {L"все", L"герои", L"войска", L"постройки"};
bool HasKinds(int cat) { return cat == kMine || cat == kOthers || cat == kAllies || cat == kNeutral; }

// Ids: objects use their game id; events and objectives get ids in their own ranges.
constexpr uint32_t kEventId = 0x80000000u, kObjectiveId = 0xC0000000u;

struct Entry {
    uint32_t id = 0, addr = 0, owner = 0;  // addr 0: not an object (event, objective)
    std::string tmpl, side;
    std::wstring name;
    const char* nameSource = "";
    int category = 0, kind = kUnits, soldiers = 0;
    float x = 0, y = 0, z = 0;
    bool keep = false, foundation = false, placed = true;  // placed: has a map position
    bool autoWalk = false;  // labels.ini "| auto"
    bool pickup = false;    // taken by walking over it: gets a plain move order, not a right click
    bool hidden = false;    // under the shroud on a story map: in no list, AutoWalk still sees it
    bool autoPlace = false; // a place from guides-auto.ini: the scripts' chain may reach another story
    bool attack = false;    // guide's "attack =": Ctrl with the right click (a cocoon of the player's own)
    bool hide = false;      // guide's "hide =": Ctrl+End sends the selected units into it
    bool ping = false;     // event of type 0: a mission's script marked this point on the radar
    bool blocked = false;  // a target a way the mission keeps shut cuts off (gates.txt): no route goes there
    bool gateStep = false; // "сначала: ...": the place to go so that such a way opens (gates.txt "go")
    uint32_t order = 0;  // events: creation frame (newest first); objectives: list index
    std::string group;   // targets: the open objective it serves (upper-case tag), else empty
    const char* shelf = "";  // targets: the subcategory for things of no objective
};

// A stealth mission's threat near the player's unit.
struct Threat {
    float x = 0, y = 0;
    std::wstring name;
};

struct Scan {
    std::vector<Entry> entries;
    uint32_t local = 0;
    int walked = 0, notInIni = 0, notSelectable = 0, soldiers = 0, civilians = 0, badOwner = 0, foundations = 0;
    int dead = 0, outside = 0;   // dying objects; objects beyond the active map border
    int rubble = 0;              // destroyed gates and walls kept as rubble (listed, "разрушено")
    bool shroudRead = false, shroudOn = false;
    int shroudW = 0, shroudH = 0, shroudPlayer = -1, ownClear = 0, ownAll = 0, shrouded = 0;
    float maxX = 0, maxY = 0;    // active border in world units (0: none known)
    std::vector<Threat> threats; // while an objective with "danger =" is open
    float danger = 0;            // its detection radius in world units
    bool idsUnique = true;
    double ms = 0;
};

uint32_t U32(uintptr_t a) {
    uint32_t v = 0;
    SafeReadU32(a, v);
    return v;
}

bool Ptr(uint32_t v) { return v >= 0x10000 && v < 0x7FFF0000; }

bool IsObject(uint32_t p) { return Ptr(p) && U32(p) == kObjectVtable; }

float F32(uintptr_t a) {
    float v = 0;
    SafeRead(a, &v, 4);
    return std::isfinite(v) ? v : 0.f;
}

// UnicodeString data: {u16 refcount, u16 ?, u16 length, u16 alloc, UTF-16 chars}.
std::wstring ReadUnicodeString(uint32_t data) {
    if (!Ptr(data)) return {};
    uint16_t len = 0;
    if (!SafeRead(data + 4, &len, 2) || len == 0 || len > 64) return {};
    wchar_t buf[65];
    if (!SafeRead(data + 8, buf, len * 2)) return {};
    return std::wstring(buf, len);
}

// "WildHillTroll_Slaved" -> "Wild Hill Troll Slaved"
std::wstring CleanName(const std::string& s) {
    std::wstring out;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '_') { if (!out.empty() && out.back() != L' ') out += L' '; continue; }
        if (i > 0 && !out.empty() && isupper((unsigned char)c) && islower((unsigned char)s[i - 1]) && out.back() != L' ') out += L' ';
        out += (wchar_t)(unsigned char)c;
    }
    return out;
}

uint32_t OwnerOf(uint32_t obj) {
    uint32_t team = U32(obj + kObjTeam);
    if (!Ptr(team)) return 0;
    uint32_t proto = U32(team + 0x30);
    if (!Ptr(proto)) return 0;
    uint32_t player = U32(proto + 8);
    return Ptr(player) && U32(player) == kPlayerVtable ? player : 0;
}

void ReadPosition(uint32_t obj, float& x, float& y, float& z) {
    x = F32(obj + kObjX);
    y = F32(obj + kObjY);
    z = F32(obj + kObjZ);
}

uint32_t LocalPlayer() {
    uint32_t list = U32(kThePlayerList);
    uint32_t p = Ptr(list) ? U32(list + 0x10) : 0;
    return Ptr(p) && U32(p) == kPlayerVtable ? p : 0;
}

// Templates never move or change during a game: their names are read once.
struct TemplateInfo {
    std::string name, side;
    const ObjectInfo* info = nullptr;
};
std::map<uint32_t, TemplateInfo> g_templates;

const TemplateInfo& TemplateOf(uint32_t tmpl) {
    auto it = g_templates.find(tmpl);
    if (it != g_templates.end()) return it->second;
    TemplateInfo t;
    t.name = game::ReadAsciiString(U32(tmpl + kTemplateName));
    t.side = game::ReadAsciiString(U32(tmpl + kTemplateSide));
    t.info = ObjectsDbFind(t.name);
    return g_templates[tmpl] = t;
}

// labels.ini (next to prism.dll): "Template = words" or "Template = words | auto".
// "auto": as soon as such an object appears, the mod walks the player's hero onto it
// (the riddles in the dark give a few seconds to stand on the right picture).
struct Label {
    std::wstring words;
    bool autoWalk = false;
};
std::map<std::string, Label> g_labels;

std::string UpperA(std::string v) {
    for (auto& c : v) c = (char)toupper((unsigned char)c);
    return v;
}

void LoadLabels(const std::wstring& modDir) {
    std::ifstream f(modDir + L"\\labels.ini", std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (line.empty() || line[0] == ';' || eq == std::string::npos) continue;
        auto trim = [](std::string v) {
            size_t a = v.find_first_not_of(" \t"), b = v.find_last_not_of(" \t");
            return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
        };
        std::string key = trim(line.substr(0, eq)), val = trim(line.substr(eq + 1));
        if (key.size() > 3 && (unsigned char)key[0] == 0xEF) key = key.substr(3);  // UTF-8 BOM
        Label l;
        size_t bar = val.find('|');
        if (bar != std::string::npos) {
            l.autoWalk = trim(val.substr(bar + 1)) == "auto";
            val = trim(val.substr(0, bar));
        }
        l.words = Widen(val.c_str());
        if (!key.empty() && !val.empty()) g_labels[UpperA(key)] = l;
    }
    LogLine(L"scanner: " + std::to_wstring(g_labels.size()) + L" labels from labels.ini");
}

// guides.ini: help for objectives. [<tag>], "say = words", "place = words | x y",
// "path = <waypoint path label of the map>" (Ctrl+Shift+End walks it exactly).
struct GuidePlace {
    std::wstring words;
    float x = 0, y = 0;
    std::vector<std::string> scripts;  // upper-case: listed while one of them can still fire
    bool hand = false;  // guides.ini, stealth.ini: a person chose it, no filter by the shroud
    bool visit = false;  // "visit = words | x y": leaves the list once an own unit stood there
    bool step = false;   // "step = words | x y": a visit listed only when the steps before it are done
    // "step = words | x y | gone Template x y": done when no object of Template stands within 10 m of that
    // point (a wall of fire deleted, a crate picked up), not by a visit: a save loaded midway skips it.
    std::string goneTmpl;
    float goneX = 0, goneY = 0;
    // "place = words | x y | while Template": listed only while an object of Template stands within 10 m
    // of the place (Grey Company's travel circles wait off the map until a script brings them in, and
    // are deleted once their region is done).
    std::string whileTmpl;
};
// "mark = x y | words": a game's light at that point belongs to a later stage. It leaves the top of
// "цели" for the story shelf under these words (Lothlorien: Galadriel's light shines from the first
// minute, and the walk to it ends at a wall, 2026-11-07 01:23).
struct GuideMark {
    float x = 0, y = 0;
    std::wstring words;
};
struct GuideObject {
    std::string tmpl;  // upper-case template: every object of it serves the objective
    std::wstring words;
    bool attack = false;  // "attack = Template | words": the player's own object, freed by a force attack
    bool hide = false;    // "hide = Template | words": a hiding place, Ctrl+End sends the game's enter order
    // "attack = Template | words | x y; x y; ...": the objects stand at these points and must go in this
    // order (walls open one passage each). Only the first one still standing is listed, as "words N из M".
    std::vector<std::pair<float, float>> order;
};
// "name = Template | x y | words": the object of the guide's template standing within 10 m of that point
// is called so (one bush of fifteen is the one the story waits for).
struct GuideName {
    std::string tmpl;
    float x = 0, y = 0;
    std::wstring words;
};
struct Guide {
    std::wstring say;
    std::vector<GuideName> names;
    std::vector<GuidePlace> places;
    std::string after;     // [HINT:...] guides: also active once this objective is done
    std::string path;      // hand-written (guides.ini): walked at once
    std::string autoPath;  // found by tools\make_guides.py: walked when move orders stall
    float autoX = 0, autoY = 0;  // the place it was found for ("autopath = label | x y")
    bool autoPlaced = false;
    std::vector<GuideObject> objects;  // "object = Template | words": characters, pick-ups
    std::vector<GuideMark> marks;      // lights of a later stage
    float danger = 0;                  // "danger = metres": enemies notice within this distance
    std::set<std::string> threats;     // "threat = Template": enemies the sides do not show
};
std::map<std::string, Guide> g_guides;  // upper-case objective tag -> guide
std::set<std::string> g_visited;         // "visit" places an own unit has stood at, by "x,y"
std::set<uint32_t> g_pingsSeen;          // radar pings of type 0 already looked at
std::map<uint32_t, DWORD> g_pingTick;    // ping -> when it was first seen, not the ones of a loaded game

// guides.ini is written by hand; guides-auto.ini is generated from the maps' scripts
// (tools\make_guides.py) and only adds objectives guides.ini does not cover.
void LoadGuides(const std::wstring& path, bool onlyNew) {
    std::ifstream f(path, std::ios::binary);
    std::string line;
    Guide* cur = nullptr;
    std::set<std::string> before;
    for (const auto& g : g_guides) before.insert(g.first);
    Guide skipped;
    auto trim = [](std::string v) {
        size_t a = v.find_first_not_of(" \t\r"), b = v.find_last_not_of(" \t\r");
        return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
    };
    int places = 0;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.size() >= 3 && (unsigned char)line[0] == 0xEF) line = trim(line.substr(3));  // UTF-8 BOM
        if (line.empty() || line[0] == ';') continue;
        if (line[0] == '[' && line.back() == ']') {
            std::string tag = UpperA(trim(line.substr(1, line.size() - 2)));
            cur = onlyNew && before.count(tag) ? &skipped : &g_guides[tag];
            continue;
        }
        size_t eq = line.find('=');
        if (!cur || eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq)), val = trim(line.substr(eq + 1));
        if (key == "say") {
            cur->say = Widen(val.c_str());
        } else if (key == "path") {
            cur->path = val;
        } else if (key == "autopath") {
            size_t bar = val.find('|');
            cur->autoPath = trim(val.substr(0, bar));
            cur->autoPlaced = bar != std::string::npos &&
                              sscanf_s(val.c_str() + bar + 1, "%f %f", &cur->autoX, &cur->autoY) == 2;
        } else if (key == "after") {
            cur->after = UpperA(val);
        } else if (key == "mark") {
            size_t bar = val.find('|');
            GuideMark gm;
            if (bar == std::string::npos || sscanf_s(val.c_str(), "%f %f", &gm.x, &gm.y) != 2) continue;
            gm.words = Widen(trim(val.substr(bar + 1)).c_str());
            cur->marks.push_back(gm);
        } else if (key == "place" || key == "visit" || key == "step") {
            size_t bar = val.find('|');
            if (bar == std::string::npos) continue;
            GuidePlace gp;
            gp.hand = !onlyNew;
            gp.visit = key == "visit" || key == "step";
            gp.step = key == "step";
            gp.words = Widen(trim(val.substr(0, bar)).c_str());
            size_t bar2 = val.find('|', bar + 1);
            char goneT[128] = "";
            if (gp.step && bar2 != std::string::npos &&
                sscanf_s(val.c_str() + bar2 + 1, " gone %127s %f %f", goneT, (unsigned)sizeof goneT, &gp.goneX, &gp.goneY) == 3) {
                gp.goneTmpl = UpperA(goneT);
                bar2 = std::string::npos;
            }
            if (bar2 != std::string::npos) {
                std::stringstream names(val.substr(bar2 + 1));
                for (std::string n; std::getline(names, n, ';');) {
                    n = trim(n);
                    if (n.compare(0, 6, "while ") == 0) gp.whileTmpl = UpperA(trim(n.substr(6)));
                    else if (!n.empty()) gp.scripts.push_back(UpperA(n));
                }
            }
            // A step with a script is done when the script fires, not when a unit stands there: the
            // Barrows lever stands 9 m from its tomb, so the fight at the tomb would have passed it.
            if (gp.step && !gp.scripts.empty()) gp.visit = false;
            if (sscanf_s(val.c_str() + bar + 1, "%f %f", &gp.x, &gp.y) == 2) { cur->places.push_back(gp); ++places; }
        } else if (key == "object" || key == "attack" || key == "hide") {
            size_t bar = val.find('|');
            GuideObject go;
            go.attack = key == "attack";
            go.hide = key == "hide";
            go.tmpl = UpperA(trim(val.substr(0, bar)));
            size_t bar2 = bar == std::string::npos ? bar : val.find('|', bar + 1);
            if (bar != std::string::npos)
                go.words = Widen(trim(val.substr(bar + 1, bar2 == std::string::npos ? bar2 : bar2 - bar - 1)).c_str());
            if (bar2 != std::string::npos) {
                std::stringstream points(val.substr(bar2 + 1));
                for (std::string pt; std::getline(points, pt, ';');) {
                    float x = 0, y = 0;
                    if (sscanf_s(pt.c_str(), "%f %f", &x, &y) == 2) go.order.push_back({x, y});
                }
            }
            cur->objects.push_back(go);
        } else if (key == "name") {
            size_t bar = val.find('|'), bar2 = bar == std::string::npos ? bar : val.find('|', bar + 1);
            GuideName gn;
            if (bar2 == std::string::npos || sscanf_s(val.c_str() + bar + 1, "%f %f", &gn.x, &gn.y) != 2) continue;
            gn.tmpl = UpperA(trim(val.substr(0, bar)));
            gn.words = Widen(trim(val.substr(bar2 + 1)).c_str());
            cur->names.push_back(gn);
        } else if (key == "danger") {
            cur->danger = (float)atof(val.c_str());
        } else if (key == "threat") {
            cur->threats.insert(UpperA(val));
        }
    }
    LogLine(L"scanner: " + path + L": " + std::to_wstring(places) + L" places; " + std::to_wstring(g_guides.size()) +
            L" objective guides in all");
}

// Mission objectives from the tracker (the map.ini list, in its order).
struct Objective {
    int index = 0;
    std::string tag;
    uint32_t state = 0;
    bool shown = false, done = false;
};

std::vector<Objective> ReadObjectives() {
    std::vector<Objective> out;
    uint32_t tracker = U32(kTheObjectiveTracker);
    uint32_t list = Ptr(tracker) ? U32(tracker + 0x10) : 0;
    uint32_t begin = Ptr(list) ? U32(list + 4) : 0, end = Ptr(list) ? U32(list + 8) : 0;
    int count = Ptr(begin) && end > begin && end - begin <= 64 * 8 ? (int)((end - begin) / 8) : 0;
    for (int i = 0; i < count; ++i) {
        Objective o;
        o.index = i;
        o.tag = game::ReadAsciiString(U32(begin + i * 8));
        o.state = U32(begin + i * 8 + 4);
        o.shown = ((o.state >> 8) & 0xFF) != 0;
        o.done = ((o.state >> 16) & 0xFF) != 0;
        if (!o.tag.empty()) out.push_back(o);
    }
    return out;
}

std::wstring ObjectiveText(const std::string& tag) {
    std::wstring t = LookupStr(tag);
    return t.empty() ? Widen(tag.c_str()) : t;
}

// The game's own script table: a one-shot script clears its active byte when it fires, so a
// trigger that has done its work drops out of the lists by itself. Disassembly: TheSidesList
// [0xDE77A0] +0x3C side count, side i = list + 0x40 + i * 0x60 (0x602F20), its ScriptList at
// side + 8 (0x604986). Scripts: findScript 0x7B72EC, table object at list +0x2C, entries begin
// list +0x38, end +0x3C, stride 0x14; entry +8 AsciiString key, +0x10 value, Script = value + 4,
// active byte Script +0x40 (ENABLE_SCRIPT 0x604ADD). The side table is PROVEN by nocatch's log
// (538 scripts, the map's count, 2026-11-07); until 2026-11-08 this read the GROUP table (list
// +0x18/+0x1C, active +0xC) and saw 47 names, so nearly every place counted as live.
// A script inactive at the start is often enabled later by the story (429 of 1214 place scripts
// in the maps, scratch placeflags.py), so a place goes only once its script was seen active and
// then inactive in this battle: it fired.
constexpr uintptr_t kTheSidesList = 0xDE77A0;
std::map<std::string, uint32_t> g_scriptTable;  // upper-case name -> Script
std::set<uint32_t> g_scriptSeenOn;              // Scripts seen active in this battle
bool g_scriptTableRead = false;
uint32_t g_scriptStamp = 0;

uint32_t ScriptTablesStamp() {
    uint32_t sides = U32(kTheSidesList), stamp = 0;
    int count = Ptr(sides) ? (int)U32(sides + 0x3C) : 0;
    for (int i = 0; i < count && i < 32; ++i) stamp = stamp * 31 + U32(sides + 0x40 + i * 0x60 + 8 + 0x38);
    return stamp;
}

void ReadScriptTable() {
    g_scriptTableRead = true;
    g_scriptTable.clear();
    g_scriptSeenOn.clear();
    g_scriptStamp = ScriptTablesStamp();
    uint32_t sides = U32(kTheSidesList);
    int count = Ptr(sides) ? (int)U32(sides + 0x3C) : 0;
    int active = 0, entries = 0, noValue = 0, unread = 0, noName = 0, twice = 0;
    std::wstring sample;
    for (int i = 0; i < count && i < 32; ++i) {
        uint32_t list = sides + 0x40 + i * 0x60 + 8;
        uint32_t begin = U32(list + 0x38), end = U32(list + 0x3C);
        if (!Ptr(begin) || end <= begin || (end - begin) / 0x14 > 20000) continue;
        for (uint32_t e = begin; e < end; e += 0x14) {
            ++entries;
            uint32_t value = U32(e + 0x10);
            uint8_t on = 0;
            if (!Ptr(value)) { ++noValue; continue; }
            if (!SafeRead(value + 4 + 0x40, &on, 1)) { ++unread; continue; }
            std::string name = game::ReadAsciiString(U32(e + 8));
            if (name.empty()) {
                // Diagnosis (2026-11-14): nocatch counts ~1000 entries where this kept ~100 names.
                if (++noName <= 3) {
                    wchar_t s[90];
                    swprintf_s(s, L" [%08X: %08X %08X %08X %08X %08X]", e, U32(e), U32(e + 4), U32(e + 8), U32(e + 12),
                               U32(e + 16));
                    sample += s;
                }
                continue;
            }
            twice += g_scriptTable.count(UpperA(name)) ? 1 : 0;
            g_scriptTable[UpperA(name)] = value + 4;
            if (on) ++active, g_scriptSeenOn.insert(value + 4);
        }
    }
    wchar_t b[220];
    swprintf_s(b, L"scripts: %d sides, %zu scripts, %d active; %d entries: %d no value, %d unread, %d no name, %d names twice",
               count, g_scriptTable.size(), active, entries, noValue, unread, noName, twice);
    LogLine(b + sample);
}

// False only when every known script of the place was seen active in this battle and is off now.
bool AnyScriptLive(const std::vector<std::string>& names) {
    if (names.empty()) return true;
    if (!g_scriptTableRead || ScriptTablesStamp() != g_scriptStamp) ReadScriptTable();
    bool known = false;
    for (const std::string& n : names) {
        auto it = g_scriptTable.find(n);
        if (it == g_scriptTable.end()) continue;
        known = true;
        uint8_t on = 0;
        if (!SafeRead(it->second + 0x40, &on, 1)) return true;
        if (on) { g_scriptSeenOn.insert(it->second); return true; }
        if (!g_scriptSeenOn.count(it->second)) return true;  // not enabled yet, or fired before we looked
    }
    if (known) {
        static std::set<std::string> said;
        if (said.insert(names[0]).second) LogLine(L"scripts: place done, its script fired: " + Widen(names[0].c_str()));
    }
    return !known;
}

// Markers that stand over someone: the scanner adds the nearest named thing.
bool IsMarker(const std::string& tmpl) {
    std::string u = UpperA(tmpl);
    return u == "AOTRDIALOGUEDOTS" || u == "AOTRQUESTMARKER" || u == "AOTRQUESTHANDIN" || u.rfind("INGAMEMARKER", 0) == 0;
}

// Soldiers of a horde: the object field that points to the horde holding it
// (Zero Hour's Object::m_containedBy). Its offset is found in game, once per battle:
// the field of the units' memory that most often points to a HORDE object. The INI
// test (InitialPayload names) missed MordorFighter1/MordorFighter2 in the Last
// Alliance mission (1771 "others", 2026-11-01 04:06), so this one comes first.
int g_containOffset = -1;
DWORD g_containTried = 0;

void CalibrateContainment(const std::vector<std::pair<uint32_t, const TemplateInfo*>>& all,
                          const std::set<uint32_t>& hordes) {
    if (hordes.empty() || GetTickCount() - g_containTried < 10000) return;
    g_containTried = GetTickCount();
    std::map<int, int> hits;
    int candidates = 0;
    static uint32_t buf[0x500 / 4];
    for (const auto& o : all) {
        const ObjectInfo* info = o.second->info;
        if (!info || !info->Has("SELECTABLE") || info->Has("HORDE") || info->Has("HERO") || info->Has("STRUCTURE"))
            continue;
        if (++candidates > 1500) break;
        if (!SafeRead(o.first + 0x40, buf, 0x4C0)) continue;
        for (int i = 0; i < 0x4C0 / 4; ++i)
            if (hordes.count(buf[i])) hits[0x40 + i * 4]++;
    }
    int best = -1, bestHits = 0, second = 0;
    for (const auto& h : hits) {
        if (h.second > bestHits) { second = bestHits; bestHits = h.second; best = h.first; }
        else if (h.second > second) second = h.second;
    }
    wchar_t b[200];
    swprintf_s(b, L"scanner: horde membership field: %d candidates, %zu hordes, best obj+0x%X with %d hits, next %d",
               candidates, hordes.size(), best, bestHits, second);
    LogLine(b);
    if (best >= 0 && bestHits >= 5 && bestHits >= 2 * second) g_containOffset = best;
}

// The active map border. Campaign maps hold several stories; scripts switch the playable
// area with MAP_SWITCH_BORDER (handler 0x7BCE69 -> TerrainLogic::setActiveBoundary 0x680E15,
// disassembly 2026-11-01): TheTerrainLogic [0xDE4690] +0x30/+0x34 vector of {int x, int y}
// sizes in cells (8 bytes each), +0x3C the active index. The area is (0, 0)-(x * 10, y * 10):
// Council of Elrond's borders match the map file (border 2 = 199 x 162 for Bilbo's cave,
// border 3 = 543 x 203 for Gladden Fields, Rivendell lies beyond both).
constexpr uintptr_t kTheTerrainLogic = 0xDE4690;
constexpr uint32_t kBoundsBegin = 0x30, kBoundsEnd = 0x34, kBoundsActive = 0x3C;

bool ActiveBorder(float& maxX, float& maxY) {
    uint32_t tl = U32(kTheTerrainLogic);
    if (!Ptr(tl)) return false;
    uint32_t b = U32(tl + kBoundsBegin), e = U32(tl + kBoundsEnd);
    int32_t i = (int32_t)U32(tl + kBoundsActive);
    if (!Ptr(b) || e < b || (e - b) / 8 > 64 || i < 0 || (uint32_t)i >= (e - b) / 8) return false;
    int32_t xy[2] = {};
    if (!SafeRead(b + 8 * i, xy, sizeof xy) || xy[0] <= 0 || xy[1] <= 0) return false;
    maxX = xy[0] * 10.f;
    maxY = xy[1] * 10.f;
    return true;
}

// Object::isEffectivelyDead: bit 0 of the byte at obj+0x458 (ScriptConditions'
// NAMED_DYING check 0x7E4A31, disassembly 2026-11-01). Dying units stay in the object list
// until their death animation ends.
constexpr uint32_t kObjPrivateStatus = 0x458;

bool Dead(uint32_t obj) {
    uint8_t b = 0;
    return SafeRead(obj + kObjPrivateStatus, &b, 1) && (b & 1);
}

// The map's black cover. The active border is a rectangle from the map's corner, so it holds other
// stories' parts of the map (Gladden Fields' border also holds Gollum's cave). What separates
// episodes is the shroud: scripts call MAP_SHROUD_ALL on 52 of 62 campaign maps at a story's start.
// TheShroudManager [0xDE4358] (ctor 0xB4DA80), its implementation at +0x10; Worldbuilder's
// ShroudManagerImpl::GetShroudStatusForPlayer, the same offsets in game.dat 0xB4E390, 0xB4E420:
// +4, +8 map origin, +0x20 cells per unit, +0x24, +0x28 cells across and down, +0x2C cells of 0xA8
// bytes. ShroudManagerCell::GetShroudStatusForPlayer 0xB4E2E0: word at cell + 4 + player * 8:
// 0xFFFF never seen or shrouded again, 0 fogged, else clear.
constexpr uintptr_t kTheShroudManager = 0xDE4358;
constexpr float kThisStoryReach = 1000.f;  // 100 m
struct ShroudMap {
    float ox = 0, oy = 0, inv = 0;
    int32_t w = 0, h = 0;
    uint32_t cells = 0;
    int player = -1;
};

bool ReadShroud(uint32_t local, ShroudMap& m) {
    uint32_t mgr = U32(kTheShroudManager), list = U32(kThePlayerList);
    if (!Ptr(mgr) || !Ptr(list) || !local) return false;
    for (int i = 0; i < 20; ++i)
        if (U32(list + 0x18 + i * 4) == local) m.player = i;
    if (m.player < 0) return false;
    for (uint32_t impl : {U32(mgr + 0x10), mgr}) {
        if (!Ptr(impl)) continue;
        float size = F32(impl + 0x1C);
        m.inv = F32(impl + 0x20);
        m.w = (int32_t)U32(impl + 0x24);
        m.h = (int32_t)U32(impl + 0x28);
        m.cells = U32(impl + 0x2C);
        if (size <= 0 || std::fabs(size * m.inv - 1.f) > 0.01f || m.w <= 0 || m.h <= 0 || m.w > 5000 || m.h > 5000 || !Ptr(m.cells))
            continue;
        m.ox = F32(impl + 4);
        m.oy = F32(impl + 8);
        return true;
    }
    return false;
}

// 0 clear, 1 fogged, 2 shrouded, -1 off the table.
int ShroudAt(const ShroudMap& m, float x, float y) {
    int cx = (int)std::floor((x - m.ox) * m.inv), cy = (int)std::floor((y - m.oy) * m.inv);
    if (cx < 0 || cy < 0 || cx >= m.w || cy >= m.h) return -1;
    uint16_t w = 0;
    if (!SafeRead(m.cells + (uint32_t)(cy * m.w + cx) * 0xA8 + 4 + m.player * 8, &w, 2)) return -1;
    return w == 0xFFFF ? 2 : w == 0 ? 1 : 0;
}

// Log line for checking the two filters: every boundary with the active index, and the
// byte obj+0x458 counted over SELECTABLE objects (the first live test, 2026-11-01 08:14,
// read the border as 6000 x 6000 and counted 1675 of 2672 objects as dying).
void LogFilterDiagnostics();
void MarkShutWays(Scan& s, const std::vector<Objective>& objectives);

Scan RunScan() {
    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceCounter(&t0);
    Scan s;
    if (!ActiveBorder(s.maxX, s.maxY)) s.maxX = s.maxY = 0;
    auto outside = [&s](float x, float y) {
        return s.maxX > 0 && (x < -50.f || y < -50.f || x > s.maxX + 50.f || y > s.maxY + 50.f);
    };
    s.local = LocalPlayer();
    std::vector<Objective> objectives = ReadObjectives();
    // Objects the open objectives name, and the threats of a stealth objective.
    std::map<std::string, std::pair<std::string, const GuideObject*>> wanted;
    std::set<std::string> forced;
    std::set<std::string> threatTemplates;
    for (const Objective& o : objectives) {
        if (!o.shown || o.done) continue;
        auto g = g_guides.find(UpperA(o.tag));
        if (g == g_guides.end()) continue;
        for (const GuideObject& go : g->second.objects) {
            wanted.insert({go.tmpl, {g->first, &go}});
            if (go.attack) forced.insert(go.tmpl);
        }
        s.danger = (std::max)(s.danger, g->second.danger * kUnitsPerMetre);
        threatTemplates.insert(g->second.threats.begin(), g->second.threats.end());
    }
    uint32_t localEvil = EvilSide(game::ReadAsciiString(s.local ? U32(s.local + kPlayerFaction) : 0));
    // Mission scripts keep friendly towns on a side of their own: Rivendell's towers belong to
    // PlyrCivilian (map sp good rivendell), enemies to PlyrCreeps. Skirmish sides are Player_N.
    std::map<uint32_t, bool> scriptedSide;
    auto scripted = [&scriptedSide](uint32_t owner) {
        auto it = scriptedSide.find(owner);
        if (it != scriptedSide.end()) return it->second;
        std::string name = owner ? game::ReadAsciiString(U32(owner + kPlayerId)) : std::string();
        bool yes = name.rfind("Plyr", 0) == 0 && name != "PlyrCreeps";
        scriptedSide[owner] = yes;
        return yes;
    };
    uint32_t gl = U32(kTheGameLogic);
    std::vector<std::pair<uint32_t, const TemplateInfo*>> all;
    std::set<uint32_t> hordes;
    for (uint32_t p = Ptr(gl) ? U32(gl + kListHead) : 0; IsObject(p) && s.walked < 20000; p = U32(p + kObjNext)) {
        ++s.walked;
        uint32_t tmpl = U32(p + kObjTemplate);
        if (!Ptr(tmpl)) continue;
        const TemplateInfo& ti = TemplateOf(tmpl);
        all.push_back({p, &ti});
        if (ti.info && ti.info->Has("HORDE")) hordes.insert(p);
    }
    if (g_containOffset < 0) CalibrateContainment(all, hordes);
    std::map<uint32_t, int> squad;  // horde -> soldiers inside
    std::set<uint32_t> ids;
    for (const auto& o : all) {
        uint32_t p = o.first;
        const TemplateInfo& ti = *o.second;
        const ObjectInfo* info = ti.info;
        // Labelled objects are checked first: the dead bit is set on 1001 of 1348 objects in
        // the Shire, scenery and (HYPOTHESIS) the quest markers, which have no real body.
        auto label = g_labels.find(UpperA(ti.name));
        std::string upper = UpperA(ti.name);
        auto want = wanted.find(upper);
        // A gate lever: DwarfWaystone is INERT scenery in the base INI (TDHdwarfprops.ini), but the
        // map.ini of Grey Company, Paths of the Dead and Dale makes it SELECTABLE with
        // CastleGateCommandSet; a script hands it to Player_1 by the lever (NAMED_TRANSFER_OWNERSHIP_PLAYER).
        // The scanner reads the base INI only, so it dropped the lever as unselectable (log 2026-11-19
        // 01:11:53: Ю at the lever selected Elladan). Kept while the local player owns it.
        bool lever = upper == "DWARFWAYSTONE" && s.local && OwnerOf(p) == s.local;
        if (s.danger > 0 && !Dead(p)) {
            bool enemy = threatTemplates.count(upper) != 0;
            if (!enemy && info && info->Has("SELECTABLE") && !info->Has("STRUCTURE") && OwnerOf(p) != s.local)
                enemy = (uint32_t)EvilSide(ti.side) != localEvil;
            if (enemy) {
                Threat t;
                float z;
                ReadPosition(p, t.x, t.y, z);
                t.name = info && !info->displayName.empty() ? info->displayName : Widen(ti.name.c_str());
                s.threats.push_back(t);
            }
        }
        // Pick-ups wait in a store off the map until a script brings them (Party's notes, Shelob's
        // torches, Durin's axe): they are not there yet.
        bool crate = info && info->Has("CRATE");
        bool stored = false;
        // map.ini crates are not in the object list: the guide's words say what they are.
        if (crate || (want != wanted.end() && want->second.second->words.rfind(L"предмет", 0) == 0)) {
            float x, y, z;
            ReadPosition(p, x, y, z);
            stored = outside(x, y);
        }
        // Fires are invisible helper objects, maybe with the dead bit of scenery (HYPOTHESIS, none
        // was read yet); a fire put out leaves the object list.
        if (want != wanted.end() && !stored && (!Dead(p) || upper.find("PARTICLESYSPROXY") != std::string::npos)) {
            // A second entry: the unit stays in its own category too.
            Entry e;
            e.addr = p;
            e.tmpl = ti.name;
            e.side = ti.side;
            e.id = U32(p + kObjId) | 0x40000000u;
            e.owner = OwnerOf(p);
            e.name = !want->second.second->words.empty() ? want->second.second->words
                     : info && !info->displayName.empty() ? info->displayName : Widen(ti.name.c_str());
            if (crate && want->second.second->words.empty()) e.name = L"предмет: " + e.name;
            e.pickup = crate || e.name.rfind(L"предмет", 0) == 0;  // map.ini crates are not in the object list
            e.attack = forced.count(upper) != 0;
            e.hide = want->second.second->hide;
            e.nameSource = "guides.ini";
            e.category = kTargets;
            e.kind = kAll;
            e.group = want->second.first;
            ReadPosition(p, e.x, e.y, e.z);
            s.entries.push_back(e);
        }
        // A destroyed gate or wall keeps its object as rubble (KeepObjectDie "IWantRubble",
        // SelfRepairFromRubbleLoop: RBHelmsDeepGateDoorBig and 36 other gates of the mod's INI),
        // dead bit set, and can be rebuilt: it stays in the lists (player 2026-11-15: a destroyed
        // gate left the scanner and could not be repaired).
        bool rubble = Dead(p) && info && info->Has("STRUCTURE") && info->Has("SELECTABLE");
        if (label == g_labels.end() && Dead(p) && !rubble && !lever) { ++s.dead; continue; }
        {
            float x, y, z;
            ReadPosition(p, x, y, z);
            if (outside(x, y)) { ++s.outside; continue; }
        }
        if (label != g_labels.end()) {
            Entry e;
            e.addr = p;
            e.tmpl = ti.name;
            e.side = ti.side;
            e.id = U32(p + kObjId);
            e.owner = OwnerOf(p);
            e.name = label->second.words;
            e.autoWalk = label->second.autoWalk;
            e.nameSource = "labels.ini";
            e.category = kTargets;
            e.kind = kAll;
            e.shelf = upper == "AOTRDIALOGUEDOTS" ? "#T" : upper.rfind("INGAMEMARKER", 0) == 0 ? "#G" : "#M";
            ReadPosition(p, e.x, e.y, e.z);
            s.entries.push_back(e);
            continue;
        }
        if (!info) { ++s.notInIni; continue; }
        // Pick-ups (crate.ini: athelas, treasures, palantir shards, Balin's axe, brandy...):
        // side Civilian and often found only by eye.
        if (info->Has("CRATE")) {
            if (want != wanted.end()) continue;  // already listed under its objective
            Entry e;
            e.pickup = true;
            e.addr = p;
            e.tmpl = ti.name;
            e.side = ti.side;
            e.id = U32(p + kObjId);
            e.name = L"предмет: " + (info->displayName.empty() ? CleanName(ti.name) : info->displayName);
            e.nameSource = "crate";
            e.shelf = "#I";
            e.category = kTargets;
            e.kind = kAll;
            ReadPosition(p, e.x, e.y, e.z);
            s.entries.push_back(e);
            continue;
        }
        if (!info->Has("SELECTABLE") && !lever) { ++s.notSelectable; continue; }
        bool hero = info->Has("HERO");
        uint32_t holder = g_containOffset >= 0 ? U32(p + g_containOffset) : 0;
        if (!hero && holder && hordes.count(holder)) { ++s.soldiers; ++squad[holder]; continue; }
        if (info->contained && !hero) { ++s.soldiers; continue; }
        // The player's own objects stay: Farmer Maggot's cart has no side of its own.
        if (ti.side == "Civilian" && !(s.local && OwnerOf(p) == s.local)) { ++s.civilians; continue; }
        Entry e;
        e.addr = p;
        e.tmpl = ti.name;
        e.side = ti.side;
        e.id = U32(p + kObjId);
        if (!ids.insert(e.id).second) s.idsUnique = false;
        e.owner = OwnerOf(p);
        if (!e.owner) ++s.badOwner;
        if (!info->displayName.empty()) { e.name = info->displayName; e.nameSource = "ini"; }
        else { e.name = CleanName(e.tmpl); e.nameSource = "template"; }
        if (rubble) { e.name += L", разрушено"; ++s.rubble; }
        if (lever) { e.name = L"рычаг ворот"; e.nameSource = "lever"; }
        e.kind = info->Has("STRUCTURE") || lever ? kBuildings : hero ? kHeroes : kUnits;
        if (s.local && e.owner == s.local) e.category = kMine;
        else if (e.side == "Neutral") e.category = kNeutral;
        else if (scripted(e.owner) && (uint32_t)EvilSide(e.side) == localEvil) e.category = kAllies;
        else e.category = kOthers;
        ReadPosition(p, e.x, e.y, e.z);
        e.keep = info->Has("CASTLE_KEEP");
        e.foundation = info->Has("BASE_FOUNDATION");
        s.entries.push_back(e);
    }
    for (Entry& e : s.entries) {
        auto it = squad.find(e.addr);
        if (it != squad.end()) e.soldiers = it->second;
    }
    // A building stands on its foundation: a citadel on its fortress foundation (both "Fortress",
    // ids 502 and 484 at 861, 3202, 2026-11-01), an expansion on a fortress pad (Prison of the
    // Damned Expansion and Fortress Expansion Plot both at (597, 3382), log 2026-11-05 00:38:21),
    // a Farm on a Rohan Build Plot. Every pad and plot carries KindOf BASE_FOUNDATION
    // (AngmarFortressExpansionPadCorner, RohanBuildingFoundation, EconomyPlotFlag). The plot is
    // left out while a building of any owner stands within 3 m, and comes back once it is gone.
    std::vector<Entry> kept;
    for (const Entry& e : s.entries) {
        bool covered = false;
        if (e.foundation)
            for (const Entry& k : s.entries)
                covered |= &k != &e && !k.foundation && (k.keep || k.kind == kBuildings) &&
                           std::hypot(k.x - e.x, k.y - e.y) < 30.f;
        if (covered) ++s.foundations;
        else kept.push_back(e);
    }
    s.entries.swap(kept);

    // A marker names the nearest character or object with a display name (civilians
    // and unselectable ones too: "Dots - Lindir" stands over a townsman).
    bool markers = false;
    for (const Entry& e : s.entries) markers |= e.category == kTargets && IsMarker(e.tmpl);
    if (markers) {
        struct Named { float x, y; std::wstring name; };
        std::vector<Named> named;
        for (const auto& o : all) {
            const ObjectInfo* info = o.second->info;
            // A name of one character: the waypoint marker's "." (log "метка игры: .").
            if (!info || info->displayName.size() < 2 || IsMarker(o.second->name) || info->Has("INERT")) continue;
            float x, y, z;
            ReadPosition(o.first, x, y, z);
            named.push_back({x, y, info->displayName});
        }
        for (Entry& e : s.entries) {
            if (e.category != kTargets || !IsMarker(e.tmpl)) continue;
            float best = 80.f;  // 8 m
            const Named* who = nullptr;
            for (const Named& n : named) {
                float d = std::hypot(n.x - e.x, n.y - e.y);
                if (d < best) best = d, who = &n;
            }
            if (who) e.name += L": " + who->name;
        }
    }

    // Radar events: what Space jumps to (attacks on the player's units and buildings).
    uint32_t radar = U32(kTheRadar);
    for (int i = 0; Ptr(radar) && i < kRadarEventCount; ++i) {
        uint32_t ev = radar + kRadarEvents + i * kRadarEventSize;
        uint32_t type = U32(ev), created = U32(ev + 8);
        if (type == 11 || created == 0) continue;
        Entry e;
        // Several pings of one script share a frame (Lothlorien's five goblin caves).
        e.id = kEventId | ((uint32_t)i << 24) | (created & 0xFFFFFF);
        e.category = kEvents;
        e.order = created;
        // RadarEventType in the Zero Hour source (Radar.h); 3 is proven here, the rest HYPOTHESIS.
        // Type 0: a mission's script points the minimap at something (OBJECT_ and TEAM_CREATE_RADAR_EVENT
        // with type 0, 484 of 511 in the campaign maps); "Rescue the Fellowship" has nothing but this.
        static const wchar_t* const kEventWords[] = {L"точка на карте", L"постройка", L"улучшение", L"нападение", L"сообщение", L"маяк",
                                                     L"проникновение", L"план боя", L"найден скрытый враг", L"раскрыт ваш скрытый отряд"};
        e.ping = type == 0;
        e.name = type <= 9 ? kEventWords[type] : L"событие " + std::to_wstring(type);
        e.nameSource = "radar";
        e.x = F32(ev + 0x34);
        e.y = F32(ev + 0x38);
        e.z = F32(ev + 0x3C);
        if ((U32(ev + 4) & 0xFF) != 0) e.name += L", сейчас";
        if (outside(e.x, e.y)) continue;
        s.entries.push_back(e);
    }

    // Shown open objectives, and the guide places of each (guides.ini). Completed ones are
    // left out (the player, 2026-11-01: finished objectives cluttered the list).
    constexpr uint32_t kGuideId = 0xA0000000u;
    // "visit": gone for good once an own unit stood there; the scripts cannot tell (those in groups
    // read as live), and the found group of hidden elves kept the first line of the objective.
    auto present = [&all](const std::string& tmpl, float x, float y) {
        for (const auto& o : all) {
            if (UpperA(o.second->name) != tmpl) continue;
            float ox, oy, oz;
            ReadPosition(o.first, ox, oy, oz);
            if (std::hypot(ox - x, oy - y) < 100.f) return true;
        }
        return false;
    };
    auto visited = [&s, &present](const GuidePlace& gp) {
        if (!gp.whileTmpl.empty() && !present(gp.whileTmpl, gp.x, gp.y)) return true;
        if (!gp.goneTmpl.empty()) return !present(gp.goneTmpl, gp.goneX, gp.goneY);
        if (!gp.visit) return false;
        std::string key = std::to_string((int)gp.x) + "," + std::to_string((int)gp.y);
        if (g_visited.count(key)) return true;
        for (const Entry& m : s.entries)
            if (m.addr && m.category == kMine && std::hypot(m.x - gp.x, m.y - gp.y) < 100.f) {
                g_visited.insert(key);
                return true;
            }
        return false;
    };
    // A build plot place goes once an own building stands on it: Fangorn's "Village 01 Plot 02"
    // stayed in "цели" and in a route with a Farm at 0 m (log 2026-11-08 03:49:35, Farm at
    // (979, 842), place at (977, 844)); its script "... Plot 01 Built" can have fired before a
    // saved game was loaded, so the scripts cannot tell.
    auto plotBuilt = [&s](const GuidePlace& gp) {
        if (gp.words.find(L"Plot") == std::wstring::npos) return false;
        for (const Entry& m : s.entries) {
            if (!m.addr || m.category != kMine || m.kind != kBuildings) continue;
            std::string t = UpperA(m.tmpl);
            if (t.find("PLOT") != std::string::npos || t.find("FOUNDATION") != std::string::npos) continue;
            if (std::hypot(m.x - gp.x, m.y - gp.y) < 10 * kUnitsPerMetre) return true;
        }
        return false;
    };
    for (const Objective& o : objectives) {
        if (!o.shown || o.done) continue;
        Entry e;
        e.id = kObjectiveId | o.index;
        e.category = kObjectives;
        e.order = o.index;
        e.tmpl = o.tag;
        e.placed = false;
        e.name = ObjectiveText(o.tag);
        if (o.done) e.name += L" Выполнено.";
        e.nameSource = "lotr.str";
        e.owner = o.state;  // kept for the log
        s.entries.push_back(e);
        auto g = g_guides.find(UpperA(o.tag));
        if (o.done || g == g_guides.end()) continue;
        bool stepListed = false;  // steps go one at a time, in the order of guides.ini
        for (size_t k = 0; k < g->second.places.size(); ++k) {
            // A later step is not looked at: the walk to the sword passes the first lord's place.
            if (g->second.places[k].step && stepListed) continue;
            if (!AnyScriptLive(g->second.places[k].scripts) || visited(g->second.places[k]) || plotBuilt(g->second.places[k])) continue;
            if (g->second.places[k].step) stepListed = true;
            // Gladden's Objective04: the chain of scripts starts at Elrond's chair in Rivendell.
            if (outside(g->second.places[k].x, g->second.places[k].y)) continue;
            Entry pe;
            pe.autoPlace = !g->second.places[k].hand;
            pe.id = kGuideId | (o.index << 8) | (uint32_t)k;
            pe.category = kTargets;
            pe.kind = kAll;
            pe.group = g->first;
            pe.name = g->second.places[k].words;
            pe.nameSource = "guides.ini";
            pe.tmpl = o.tag;
            pe.x = g->second.places[k].x;
            pe.y = g->second.places[k].y;
            s.entries.push_back(pe);
        }
    }
    // Guides tied to a hint the game showed ([HINT:SCRIPT:RivendellHint002]) or to a finished
    // objective ("after ="): places to go when no objective is open (Council of Elrond, leave
    // the Hall of Fire once the three stories are done, 2026-11-01).
    constexpr uint32_t kHintGuideId = 0xA1000000u;
    uint32_t hintNo = 0;
    std::set<std::pair<int, int>> hintPlaces;  // two hints may name the same place
    for (const auto& kv : g_guides) {
        bool pre = kv.first.compare(0, 4, "PRE:") == 0;
        if (!pre && kv.first.compare(0, 5, "HINT:") != 0) continue;
        ++hintNo;
        bool active = pre || HintSeen(kv.first.substr(5));
        for (const Objective& o : objectives)
            if (!active && o.done && !kv.second.after.empty() && UpperA(o.tag) == kv.second.after) active = true;
        // [PRE:<tag>]: what leads to an objective not shown yet. The story goes on in the map's
        // order of objectives: the earliest one waiting comes first (NextStep).
        uint32_t storyOrder = 0;
        if (pre) {
            // g_guides holds every mission: 211 places of Helm's Deep, Minas Tirith and the rest
            // stood on Council's list (log 2026-11-06 00:47:31).
            bool thisMission = false;
            for (const Objective& o : objectives)
                if (UpperA(o.tag) == kv.first.substr(4)) {
                    thisMission = true;
                    storyOrder = o.index;
                    if (o.shown || o.done) active = false;
                }
            active = active && thisMission;
            // A [PRE] with "after =": not before that objective is done. A place behind a wall the
            // mission opens later only sends the player into the wall (Meeting Trigger, 01:24:48).
            if (!kv.second.after.empty()) {
                bool reached = false;
                for (const Objective& o : objectives) reached |= o.done && UpperA(o.tag) == kv.second.after;
                active = active && reached;
            }
        }
        if (!active) continue;
        for (size_t k = 0; k < kv.second.places.size(); ++k) {
            if (!AnyScriptLive(kv.second.places[k].scripts) || visited(kv.second.places[k]) || plotBuilt(kv.second.places[k])) continue;
            if (pre && outside(kv.second.places[k].x, kv.second.places[k].y)) continue;  // another story of the map
            if (!hintPlaces.insert({(int)kv.second.places[k].x, (int)kv.second.places[k].y}).second) continue;
            Entry pe;
            pe.id = kHintGuideId | (hintNo << 8) | (uint32_t)k;
            pe.category = kTargets;
            pe.kind = kAll;
            pe.name = kv.second.places[k].words;
            pe.nameSource = "guides.ini";
            pe.tmpl = kv.first;
            pe.order = storyOrder;
            pe.shelf = "#S";
            pe.x = kv.second.places[k].x;
            pe.y = kv.second.places[k].y;
            s.entries.push_back(pe);
        }
    }
    // A light of a later stage (guide "mark"): named, and off the top of the list.
    for (const Objective& o : objectives) {
        auto g = g_guides.find(UpperA(o.tag));
        if (g == g_guides.end()) continue;
        for (const GuideMark& m : g->second.marks)
            for (Entry& e : s.entries)
                if (e.category == kTargets && e.group.empty() && IsMarker(e.tmpl) && std::hypot(e.x - m.x, e.y - m.y) < 30.f) {
                    e.name = m.words;
                    e.shelf = "#S";
                    e.order = 99;
                }
    }

    // Story maps: what lies under the shroud belongs to another episode or was never reached.
    // Skirmish keeps everything, as nothing there is scenery of another story. What an objective
    // names and the markers stay: the mission itself points at them. The own units check the table:
    // they stand in the clear, or the filter does not trust it.
    ShroudMap shroud;
    s.shroudRead = !objectives.empty() && ReadShroud(s.local, shroud);
    if (s.shroudRead) {
        s.shroudW = shroud.w, s.shroudH = shroud.h, s.shroudPlayer = shroud.player;
        for (const Entry& e : s.entries)
            if (e.addr && e.category == kMine) {
                ++s.ownAll;
                s.ownClear += ShroudAt(shroud, e.x, e.y) == 0;
            }
        s.shroudOn = s.ownAll > 0 && s.ownClear * 2 >= s.ownAll;
    }
    if (s.shroudOn) {
        // Another story lies far off; what stands near the own units belongs to this one, seen
        // or not. The filter hid the Ring 18 m from Bilbo, and without it the cave stays shut.
        std::vector<std::pair<float, float>> own;
        for (const Entry& e : s.entries)
            if (e.addr && e.category == kMine) own.push_back({e.x, e.y});
        auto nearOwn = [&own](const Entry& e) {
            for (const auto& o : own)
                if (std::hypot(o.first - e.x, o.second - e.y) < kThisStoryReach) return true;
            return false;
        };
        for (Entry& e : s.entries)
            if (((e.addr && e.category != kMine && e.group.empty() && !IsMarker(e.tmpl)) || (e.category == kEvents && !e.ping)) &&
                ShroudAt(shroud, e.x, e.y) == 2 && !nearOwn(e)) {
                e.hidden = true;  // an event: a fight of the last story, reported in this one
                ++s.shrouded;
            }
        // Places. The story's next place is usually uncovered by the scripts in advance, so a place
        // under the shroud yields to an uncovered one of the same objective, never leaves it empty:
        // Elrond's chair during Osgiliath, whose border holds Rivendell too.
        auto place = [](const Entry& e) { return e.category == kTargets && !e.addr && strcmp(e.nameSource, "guides.ini") == 0; };
        auto story = [](const Entry& e) { return e.tmpl.rfind("PRE:", 0) == 0; };
        std::set<std::string> uncovered;
        for (const Entry& e : s.entries)
            if (place(e) && ShroudAt(shroud, e.x, e.y) != 2) uncovered.insert(story(e) ? "#S" : e.group);
        for (Entry& e : s.entries) {
            if (!place(e) || !(story(e) ? uncovered.count("#S") : e.autoPlace && uncovered.count(e.group))) continue;
            if (ShroudAt(shroud, e.x, e.y) == 2) e.hidden = true, ++s.shrouded;
        }
    }
    // Objects to be taken in a set order: the first point that still has one is the next step, the
    // others leave the lists but stay in the scan (WatchPickups counts them). Dead Marshes: the
    // nearest barrow stood behind a wall still closed (log 2026-11-08 01:20-01:31).
    for (const auto& w : wanted) {
        const GuideObject& go = *w.second.second;
        if (go.order.empty()) continue;
        std::vector<Entry*> mine;
        for (Entry& e : s.entries)
            if (e.category == kTargets && e.addr && e.group == w.second.first && UpperA(e.tmpl) == go.tmpl) mine.push_back(&e);
        Entry* next = nullptr;
        size_t step = 0;
        for (; step < go.order.size() && !next; ++step)
            for (Entry* e : mine)
                if (std::hypot(e->x - go.order[step].first, e->y - go.order[step].second) < 100.f) { next = e; break; }
        if (!next) continue;  // none stands where the guide says: all are listed
        for (Entry* e : mine) {
            if (e != next) { e->hidden = true; continue; }
            e->hidden = false;
            e->name += L" " + std::to_wstring(step) + L" из " + std::to_wstring(go.order.size());
        }
    }
    for (const Objective& o : objectives) {
        if (!o.shown || o.done) continue;
        auto g = g_guides.find(UpperA(o.tag));
        if (g == g_guides.end()) continue;
        for (const GuideName& gn : g->second.names)
            for (Entry& e : s.entries)
                if (e.category == kTargets && e.addr && e.group == g->first && UpperA(e.tmpl) == gn.tmpl &&
                    std::hypot(e.x - gn.x, e.y - gn.y) < 100.f)
                    e.name = gn.words;
    }
    MarkShutWays(s, objectives);

    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);
    s.ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart;
    return s;
}

void LogFilterDiagnostics() {
    std::wstring line = L"scanner: borders";
    uint32_t tl = U32(kTheTerrainLogic);
    if (Ptr(tl)) {
        uint32_t b = U32(tl + kBoundsBegin), e = U32(tl + kBoundsEnd);
        wchar_t w[80];
        swprintf_s(w, L" logic %08X vec %08X..%08X active %d:", tl, b, e, (int32_t)U32(tl + kBoundsActive));
        line += w;
        for (uint32_t a = b; Ptr(b) && a < e && a < b + 8 * 16; a += 8) {
            swprintf_s(w, L" (%d,%d)", (int32_t)U32(a), (int32_t)U32(a + 4));
            line += w;
        }
        // The fields around, in case the vector or the index sit elsewhere in this build.
        line += L" | +0x20..+0x4C:";
        for (uint32_t off = 0x20; off < 0x50; off += 4) {
            swprintf_s(w, L" %X", U32(tl + off));
            line += w;
        }
    }
    LogLine(line);
    // How the byte obj+0x458 is spread over selectable objects (alive units should read 0).
    uint32_t gl = U32(kTheGameLogic);
    std::map<int, int> values;
    std::wstring samples;
    int n = 0;
    for (uint32_t p = Ptr(gl) ? U32(gl + kListHead) : 0; IsObject(p) && n < 20000; p = U32(p + kObjNext), ++n) {
        const TemplateInfo& ti = TemplateOf(U32(p + kObjTemplate));
        if (!ti.info || !ti.info->Has("SELECTABLE")) continue;
        uint8_t v = 0;
        SafeRead(p + kObjPrivateStatus, &v, 1);
        ++values[v];
        if ((v & 1) && samples.size() < 400) samples += L" " + Widen(ti.name.c_str());
    }
    std::wstring hist = L"scanner: byte obj+0x458 over selectable objects:";
    for (const auto& kv : values) hist += L" " + std::to_wstring(kv.first) + L"x" + std::to_wstring(kv.second);
    LogLine(hist + L"; with bit 0:" + samples);
}

// Selected objects, in the game's selection order.
std::vector<uint32_t> Selection() {
    std::vector<uint32_t> out;
    uint32_t ui = U32(kTheInGameUI);
    if (!Ptr(ui)) return out;
    uint32_t head = U32(ui + kUiSelection);
    if (!Ptr(head)) return out;
    uint32_t node = U32(head);
    for (int steps = 0; Ptr(node) && node != head && steps < 500; ++steps, node = U32(node)) {
        uint32_t d = U32(node + 8);
        uint32_t obj = Ptr(d) ? U32(d + kDrawableObject) : 0;
        if (IsObject(obj)) out.push_back(obj);
    }
    return out;
}

std::wstring NameOf(uint32_t obj) {
    std::string t = game::ReadAsciiString(U32(U32(obj + kObjTemplate) + kTemplateName));
    const ObjectInfo* info = ObjectsDbFind(t);
    return info && !info->displayName.empty() ? info->displayName : CleanName(t);
}

std::wstring Metres(float units) {
    int n = (int)std::lround(units / kUnitsPerMetre);
    int d = n % 10, h = n % 100;
    const wchar_t* w = (d == 1 && h != 11) ? L"метр" : (d >= 2 && d <= 4 && (h < 12 || h > 14)) ? L"метра" : L"метров";
    return std::to_wstring(n) + L" " + w;
}

// Map direction: +x east, +y north (HYPOTHESIS: SAGE map axes, not yet checked in game).
std::wstring Direction(float dx, float dy) {
    static const wchar_t* names[8] = {L"восток", L"северо-восток", L"север", L"северо-запад",
                                      L"запад", L"юго-запад", L"юг", L"юго-восток"};
    double a = std::atan2(dy, dx) * 180.0 / 3.14159265358979;
    int sector = (int)std::floor((a + 22.5 + 360.0) / 45.0) % 8;
    return names[sector];
}

// Scanner state
int g_category = kMine;
int g_kind[kCategories] = {};               // subcategory remembered per category
std::map<int, std::vector<uint32_t>> g_order;  // (category, kind) -> ids in paging order
int g_index = -1;                            // into the current order; -1 = before the first
uint32_t g_refId = 0, g_refAddr = 0;
float g_refX = 0, g_refY = 0, g_sortX = 0, g_sortY = 0;
bool g_haveRef = false;
std::wstring g_refName;
std::vector<uint32_t> g_lastSelection;
bool g_keyDown[256] = {};
bool g_wasInBattle = false;
uint32_t g_lastFrame = 0;
HWND g_wnd = nullptr;
std::map<uint32_t, uint32_t> g_objectiveSecond;  // objective index -> second dword last logged

// Building placement: TheInGameUI+0x53C holds the ThingTemplate picked with a build
// hotkey and 0 otherwise (Zero Hour's InGameUI::m_pendingPlaceType). Proven by the test
// of 2026-11-01 03:08: it went 0 -> MirkwoodVineyard, -> MirkwoodTrainingGrounds, -> 0
// with each hotkey and cancel; no other field of the first 0x2000 bytes held a template.
constexpr uint32_t kUiPendingPlace = 0x53C;
uint32_t g_pendingPlace = 0;
uint32_t g_templateVtable = 0;

// Subcategories of "цели": one per open objective with something to go to, then the shelves
// for what serves no objective, then everything. Picked by key, as the list changes in play.
std::string g_targetKey;  // upper-case tag or a shelf
std::wstring g_targetName;

const wchar_t* ShelfName(const std::string& k) {
    return k == "#G" ? L"метки игры" : k == "#M" ? L"метки" : k == "#I" ? L"предметы" : k == "#T" ? L"разговоры"
           : k == "#S" ? L"дальше по сюжету" : L"все";
}

std::vector<std::string> TargetGroups(const Scan& s) {
    std::vector<std::string> out;
    // The game's own light is where the mission wants the player now.
    for (const Entry& e : s.entries)
        if (e.category == kTargets && e.group.empty() && !strcmp(e.shelf, "#G")) { out.push_back("#G"); break; }
    for (const Objective& o : ReadObjectives()) {
        std::string tag = UpperA(o.tag);
        for (const Entry& e : s.entries)
            if (e.category == kTargets && e.group == tag) { out.push_back(tag); break; }
    }
    for (const char* shelf : {"#S", "#M", "#I", "#T"})
        for (const Entry& e : s.entries)
            if (e.category == kTargets && e.group.empty() && !e.hidden && !strcmp(e.shelf, shelf)) { out.push_back(shelf); break; }
    out.push_back("#A");
    return out;
}

void PickTargetGroup(const std::vector<std::string>& groups, const std::string& key) {
    g_targetKey = key;
    g_targetName = ShelfName(key);
    if (key[0] != '#')
        for (const Objective& o : ReadObjectives())
            if (UpperA(o.tag) == key) g_targetName = ObjectiveText(o.tag);
    (void)groups;
}

int OrderKey() {
    if (g_category == kTargets) return (int)(std::hash<std::string>()(g_targetKey) | 0x40000000u);
    return g_category * kKinds + (HasKinds(g_category) ? g_kind[g_category] : kAll);
}
std::vector<uint32_t>& CurrentOrder() { return g_order[OrderKey()]; }

std::wstring PlaceName() {
    std::wstring t = kCategoryNames[g_category];
    if (HasKinds(g_category)) t += std::wstring(L", ") + kKindNames[g_kind[g_category]];
    if (g_category == kTargets) t += L", " + g_targetName;
    return t;
}

bool Pressed(int vk) {
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool edge = down && !g_keyDown[vk];
    g_keyDown[vk] = down;
    return edge;
}

// An empty list. The reference object is never in its own list, so the selected Gandalf alone
// on the field reads "мои: ничего нет" (log 2026-11-05 13:26:20).
std::wstring NothingWords();

// Reference point: the first selected object, else the local player's command
// centre (citadel), else the last reference.
void UpdateReference(const Scan& s) {
    std::vector<uint32_t> sel = Selection();
    uint32_t obj = sel.empty() ? 0 : sel[0];
    if (!obj) {
        for (const Entry& e : s.entries) {
            if (!e.addr) continue;
            const ObjectInfo* info = ObjectsDbFind(e.tmpl);
            if (e.owner == s.local && info && info->Has("COMMANDCENTER")) { obj = e.addr; break; }
        }
    }
    if (!obj) return;
    g_refAddr = obj;
    g_refId = U32(obj + kObjId);
    float z;
    ReadPosition(obj, g_refX, g_refY, z);
    g_refName = NameOf(obj);
    g_haveRef = true;
}

float Dist(const Entry& e) { return std::hypot(e.x - g_refX, e.y - g_refY); }

// Where to go when the list in hand has no place: between objectives ("задания" is empty after
// Survive the orc ambush, Gladden Fields 2026-11-05 23:46) or under an objective with none of its
// own ("Speak to heroes"). A point the mission marked on the radar in the last five minutes, then
// the game's light, then a place of an open objective, then the story.
const Entry* NextStep(const Scan& s) {
    const Entry* ping = nullptr;
    for (const Entry& e : s.entries) {
        auto t = g_pingTick.find(e.id);
        if (e.ping && !e.hidden && t != g_pingTick.end() && GetTickCount() - t->second < 300000 && (!ping || e.order > ping->order)) ping = &e;
    }
    if (ping) return ping;
    const Entry* best = nullptr;
    int bestRank = 3;
    for (const Entry& e : s.entries) {
        if (e.category != kTargets || e.hidden || e.blocked) continue;
        int rank = !strcmp(e.shelf, "#G") ? 0 : !e.group.empty() ? 1 : !strcmp(e.shelf, "#S") ? 2 : 3;
        if (rank > bestRank || rank == 3) continue;
        bool better = !best || rank < bestRank || (rank == 2 && e.order < best->order) ||
                      ((rank < 2 || e.order == best->order) && Dist(e) < Dist(*best));
        if (better) best = &e, bestRank = rank;
    }
    return best;
}

const Entry* FindId(const Scan& s, uint32_t id) {
    for (const Entry& e : s.entries)
        if (e.id == id) return &e;
    return nullptr;
}

bool InCurrent(const Entry& e) {
    if (e.hidden || e.category != g_category || (e.addr && e.id == g_refId)) return false;
    if (g_category == kTargets)
        return g_targetKey == "#A" || e.group == g_targetKey || (e.group.empty() && g_targetKey == e.shelf);
    int kind = HasKinds(g_category) ? g_kind[g_category] : kAll;
    return kind == kAll || e.kind == kind;
}

// Rebuilds the current list. Objects: nearest first when fresh; otherwise the order
// the player is paging through is kept, gone objects drop out and new ones go at the
// end, nearest first. Events: always newest first. Objectives: in the map's order.
void Reorder(const Scan& s, bool fresh) {
    if (g_category == kTargets) {
        std::vector<std::string> groups = TargetGroups(s);
        if (std::find(groups.begin(), groups.end(), g_targetKey) == groups.end()) {
            PickTargetGroup(groups, groups[0]);
            fresh = true;
        }
    }
    std::vector<const Entry*> in;
    for (const Entry& e : s.entries)
        if (InCurrent(e)) in.push_back(&e);
    if (g_category == kEvents) {
        std::stable_sort(in.begin(), in.end(), [](const Entry* a, const Entry* b) { return a->order > b->order; });
        fresh = true;
    } else if (g_category == kObjectives) {
        std::stable_sort(in.begin(), in.end(), [](const Entry* a, const Entry* b) { return a->order < b->order; });
        fresh = true;
    } else {
        // Targets: the ~50 chatty hobbits of the Shire come after what the objectives need.
        auto chatter = [](const Entry* e) { return e->category == kTargets && UpperA(e->tmpl) == "AOTRDIALOGUEDOTS"; };
        std::stable_sort(in.begin(), in.end(), [&](const Entry* a, const Entry* b) {
            bool ca = chatter(a), cb = chatter(b);
            return ca != cb ? cb : Dist(*a) < Dist(*b);
        });
    }
    std::vector<uint32_t>& order = CurrentOrder();
    uint32_t current = g_index >= 0 && g_index < (int)order.size() ? order[g_index] : 0;
    std::vector<uint32_t> next;
    std::set<uint32_t> present;
    for (const Entry* e : in) present.insert(e->id);
    if (!fresh)
        for (uint32_t id : order)
            if (present.count(id)) next.push_back(id);
    std::set<uint32_t> kept(next.begin(), next.end());
    for (const Entry* e : in)
        if (!kept.count(e->id)) next.push_back(e->id);
    order = next;
    g_index = -1;
    if (current)
        for (size_t i = 0; i < order.size(); ++i)
            if (order[i] == current) g_index = (int)i;
}

// Scans, updates the reference and the current list. Returns the scan.
Scan Refresh(bool forceFresh, bool& resorted) {
    Scan s = RunScan();
    uint32_t oldRef = g_refId;
    bool hadRef = g_haveRef;
    UpdateReference(s);
    resorted = forceFresh || !hadRef || g_refId != oldRef ||
               std::hypot(g_refX - g_sortX, g_refY - g_sortY) > kResortDistance;
    if (resorted) { g_sortX = g_refX; g_sortY = g_refY; }
    Reorder(s, resorted);
    if (resorted && g_category != kEvents && g_category != kObjectives) g_index = -1;
    return s;
}

std::wstring Describe(const Entry& e) {
    if (!e.placed) return e.name;
    float x = e.x, y = e.y, z = e.z;
    if (e.addr) ReadPosition(e.addr, x, y, z);  // live position (the scan is a few ms old)
    float dx = x - g_refX, dy = y - g_refY;
    std::wstring t = e.name;
    if (e.soldiers > 0) t += L", в отряде " + std::to_wstring(e.soldiers);
    if ((e.category == kOthers || e.category == kAllies) && !e.side.empty()) t += L", " + Widen(e.side.c_str());
    t += L", " + Metres(std::hypot(dx, dy));
    if (std::hypot(dx, dy) >= kUnitsPerMetre) t += L", " + Direction(dx, dy);
    return t;
}

void LogObjectives(const Scan& s) {
    for (const Entry& e : s.entries) {
        if (e.category != kObjectives) continue;
        auto it = g_objectiveSecond.find(e.order);
        if (it != g_objectiveSecond.end() && it->second == e.owner) continue;
        g_objectiveSecond[e.order] = e.owner;
        wchar_t b[160];
        swprintf_s(b, L"objectives: [%u] %S second dword %08X", e.order, e.tmpl.c_str(), e.owner);
        LogLine(b);
    }
}

std::wstring NothingWords() {
    if (g_category == kMine && g_kind[kMine] == kAll && g_haveRef && !g_refName.empty())
        return PlaceName() + L": кроме " + g_refName + L" ничего нет";
    return PlaceName() + L": ничего нет";
}

void SpeakCurrent(const Scan& s, const std::wstring& prefix) {
    const std::vector<uint32_t>& order = CurrentOrder();
    if (order.empty()) {
        std::wstring next;
        if (g_category == kObjectives)
            if (const Entry* e = NextStep(s)) next = L" Дальше: " + Describe(*e) + L". Ctrl+Shift+End идти туда.";
        Speak(prefix + NothingWords() + next, true);
        LogLine(L"scanner: empty " + PlaceName());
        return;
    }
    if (g_index < 0 || g_index >= (int)order.size()) g_index = 0;
    const Entry* e = FindId(s, order[g_index]);
    if (!e) {
        Speak(prefix + L"объект исчез", true);
        return;
    }
    std::wstring text = prefix + Describe(*e) + L", " + std::to_wstring(g_index + 1) + L" из " +
                        std::to_wstring(order.size());
    Speak(text, true);
    wchar_t b[200];
    swprintf_s(b, L"scanner: id %08X %S at (%.0f, %.0f) ref id %u (%.0f, %.0f) scan %.1f ms -> ", e->id,
               e->tmpl.c_str(), e->x, e->y, g_refId, g_refX, g_refY, s.ms);
    LogLine(b + text);
    if (g_category == kObjectives) LogObjectives(s);
}

std::wstring ReferencePrefix(uint32_t oldRef) {
    if (!g_haveRef) return L"Нет точки отсчёта. ";
    return g_refId != oldRef ? L"От " + g_refName + L". " : L"";
}

// "Feredir 4, Builder": names in selection order, each with its count when more than one.
std::wstring SelectionWords(const std::vector<uint32_t>& sel) {
    std::vector<std::pair<std::wstring, int>> groups;
    for (uint32_t o : sel) {
        std::wstring n = NameOf(o);
        auto it = std::find_if(groups.begin(), groups.end(), [&](const auto& g) { return g.first == n; });
        if (it == groups.end()) groups.push_back({n, 1});
        else ++it->second;
    }
    std::wstring t;
    for (size_t i = 0; i < groups.size() && i < 6; ++i) {
        if (i) t += L", ";
        t += groups[i].first;
        if (groups[i].second > 1) t += L" " + std::to_wstring(groups[i].second);
    }
    if (groups.size() > 6) t += L" и другие";
    return t;
}

std::wstring MoneyWords() {
    uint32_t local = LocalPlayer();
    if (local && U32(local + kPlayerMoneyVt) == kMoneyVtable) return L"Деньги " + std::to_wstring(U32(local + kPlayerMoney));
    return L"Деньги не найдены";
}

// End: what is selected; with nothing selected, the money (player's choice 2026-11-01).
// Shift+End: the money always.
// Other sides' and neutral units within 30 m of a unit: mission scripts test it as
// UNIT_THREAT_LEVEL ... 300 (Gladden Fields: Isildur shows himself only to Elendur within
// 120 units with threat 0 inside 300 units; map scripts read 2026-11-01).
std::wstring NearbyEnemies(uint32_t unit) {
    float ux, uy, uz;
    ReadPosition(unit, ux, uy, uz);
    Scan s = RunScan();
    int n = 0;
    const Entry* nearest = nullptr;
    float best = 1e30f;
    for (const Entry& e : s.entries) {
        if ((e.category != kOthers && e.category != kNeutral) || e.kind == kBuildings || !e.addr) continue;
        float d = std::hypot(e.x - ux, e.y - uy);
        if (d > 300.f) continue;
        ++n;
        if (d < best) best = d, nearest = &e;
    }
    if (!n) return L"Чужих в 30 метрах нет.";
    return L"Чужих в 30 метрах: " + std::to_wstring(n) + L", ближайший " + nearest->name + L", " +
           std::to_wstring((int)std::lround(best / kUnitsPerMetre)) + L" м.";
}

void Status(bool moneyOnly) {
    std::vector<uint32_t> sel = Selection();
    std::wstring t = moneyOnly || sel.empty() ? MoneyWords() : L"Выделено: " + SelectionWords(sel);
    if (!moneyOnly && !sel.empty()) t += L". " + NearbyEnemies(sel[0]);
    Speak(t, true);
    uint32_t local = LocalPlayer();
    wchar_t b[160];
    swprintf_s(b, L"scanner: status local %08X +6C %u +70 %u -> ", local, local ? U32(local + 0x6C) : 0,
               local ? U32(local + 0x70) : 0);
    LogLine(b + t);
}

uint32_t PendingPlace() {
    uint32_t ui = U32(kTheInGameUI);
    if (!Ptr(ui)) return 0;
    if (!g_templateVtable) {
        uint32_t gl = U32(kTheGameLogic);
        uint32_t first = Ptr(gl) ? U32(gl + kListHead) : 0;
        if (!IsObject(first)) return 0;
        g_templateVtable = U32(U32(first + kObjTemplate));
    }
    uint32_t v = U32(ui + kUiPendingPlace);
    return Ptr(v) && g_templateVtable && U32(v) == g_templateVtable ? v : 0;
}

std::wstring BuildingWords(const TemplateInfo& t, bool withDescription) {
    std::wstring w = t.info && !t.info->displayName.empty() ? t.info->displayName : CleanName(t.name);
    if (t.info && t.info->buildCost > 0) w += L", цена " + std::to_wstring(t.info->buildCost);
    if (withDescription) w += L". " + (t.info && !t.info->description.empty() ? t.info->description : L"нет описания");
    return w;
}

void WatchPlacement() {
    uint32_t cur = PendingPlace();
    if (cur == g_pendingPlace) return;
    uint32_t old = g_pendingPlace;
    g_pendingPlace = cur;
    LogLine(L"placement: " + Widen(old ? TemplateOf(old).name.c_str() : "0") + L" -> " +
            Widen(cur ? TemplateOf(cur).name.c_str() : "0"));
    if (!cur) return;
    std::wstring t = L"Постройка: " + BuildingWords(TemplateOf(cur), false);
    Speak(t, true);
    LogLine(L"placement: spoken -> " + t);
}

// The entry the player is on, re-read from a fresh scan (no re-sorting).
const Entry* CurrentEntry(const Scan& s) {
    const std::vector<uint32_t>& order = CurrentOrder();
    if (order.empty()) return nullptr;
    int i = g_index < 0 || g_index >= (int)order.size() ? 0 : g_index;
    return FindId(s, order[i]);
}

// Shift+Home: what the current entry is for (the tooltip of the button that builds it).
void DescribeCurrent() {
    bool resorted = false;
    Scan s = Refresh(false, resorted);
    const Entry* e = CurrentEntry(s);
    if (!e) { Speak(L"Нет объекта", true); return; }
    std::wstring t;
    if (e->category == kEvents) t = e->name + L". Место, к которому игра переводит камеру пробелом.";
    else if (e->category == kObjectives) {
        t = e->name;
        auto g = g_guides.find(UpperA(e->tmpl));
        if (g != g_guides.end() && !g->second.say.empty()) t += L". " + g->second.say;
    }
    else {
        const ObjectInfo* info = ObjectsDbFind(e->tmpl);
        t = e->name + L". " + (info && !info->description.empty() ? info->description : L"нет описания");
    }
    Speak(t, true);
    LogLine(L"scanner: describe id " + std::to_wstring(e->id) + L" -> " + t);
}

// The camera looks at the centre of the game window: the cursor goes there, on the target.
bool CursorToWindowCentre(POINT& pt) {
    RECT rc;
    if (!g_wnd || !GetClientRect(g_wnd, &rc)) return false;
    pt = {(rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2};
    ClientToScreen(g_wnd, &pt);
    SetCursorPos(pt.x, pt.y);
    return true;
}

void ClickWindowCentre(bool right) {
    POINT pt;
    if (!CursorToWindowCentre(pt)) return;
    Sleep(40);
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_MOUSE;
    in[0].mi.dwFlags = right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
    in[1].mi.dwFlags = right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
    SendInput(1, &in[0], sizeof(INPUT));
    Sleep(60);
    SendInput(1, &in[1], sizeof(INPUT));
    wchar_t b[80];
    swprintf_s(b, L"order: %s click at screen (%ld, %ld)", right ? L"right" : L"left", pt.x, pt.y);
    LogLine(b);
}

void RightClickWindowCentre() { ClickWindowCentre(true); }

// Camera onto a point, waiting until the game thread has moved it. False when it did not.
bool CameraTo(float x, float y, float z) {
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CameraLookAt(x, y, z, done);
    bool ran = done && WaitForSingleObject(done, 1000) == WAIT_OBJECT_0;
    if (done) CloseHandle(done);
    float cx, cy;
    CameraPosition(cx, cy);
    return ran && std::hypot(cx - x, cy - y) < 50.f;
}

// Move order for the selected units straight through the game's message stream, waiting
// until the game thread has sent it. False when it was refused or did not run.
bool DirectMove(float x, float y, float z) {
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool ok = false;
    MoveSelectedTo(x, y, z, done, &ok);
    bool ran = done && WaitForSingleObject(done, 1000) == WAIT_OBJECT_0;
    if (done) CloseHandle(done);
    return ran && ok;
}

// Auto-walk onto "| auto" targets. The objects already on the map when the battle
// starts are the mission's store of pictures (Council of Elrond keeps all nine riddle
// pictures at about (80, 615); its scripts create copies on the answer waypoints, test
// of 2026-11-01 05:04), so only objects created later count. A target waits until one
// of the player's units is within 60 m; then the order is given (camera to the target,
// right click), repeated every 1.5 s while the picture exists, until the unit stands on it.
constexpr float kAutoReach = 600.f;
bool g_autoBaseline = false;
std::set<uint32_t> g_autoPre, g_autoDone, g_autoWaiting;
uint32_t g_autoTarget = 0, g_autoUnit = 0;
int g_autoTries = 0;
DWORD g_autoLast = 0;

// The unit to send: the selected one when it is near the target, else the player's
// nearest hero (then any unit) within 60 m, selected with a left click.
// The selected unit nearest to the target, if within 60 m.
uint32_t SelectedNear(float tx, float ty) {
    uint32_t best = 0;
    float bestD = kAutoReach;
    for (uint32_t o : Selection()) {
        float x, y, z;
        ReadPosition(o, x, y, z);
        float d = std::hypot(x - tx, y - ty);
        if (d < bestD) bestD = d, best = o;
    }
    return best;
}

// Q selects every unit the player controls (the player, 2026-11-01; in the riddles
// only Bilbo). Sent by scan code so the keyboard layout does not matter.
void PressSelectAll() {
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wScan = in[1].ki.wScan = 0x10;  // Q
    in[0].ki.dwFlags = KEYEVENTF_SCANCODE;
    in[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    SendInput(1, &in[0], sizeof(INPUT));
    Sleep(50);
    SendInput(1, &in[1], sizeof(INPUT));
    LogLine(L"auto: pressed Q (select all)");
}

uint32_t AutoWalker(const Scan& s, float tx, float ty) {
    if (uint32_t u = SelectedNear(tx, ty)) return u;
    PressSelectAll();
    Sleep(200);
    if (uint32_t u = SelectedNear(tx, ty)) return u;
    uint32_t best = 0;
    float bestD = kAutoReach;
    bool bestHero = false;
    for (const Entry& e : s.entries) {
        if (!e.addr || e.category != kMine || e.kind == kBuildings) continue;
        float d = std::hypot(e.x - tx, e.y - ty);
        bool hero = e.kind == kHeroes;
        if (d < kAutoReach && ((hero && !bestHero) || (hero == bestHero && d < bestD))) best = e.addr, bestD = d, bestHero = hero;
    }
    if (!best) return 0;
    float x, y, z;
    ReadPosition(best, x, y, z);
    if (!CameraTo(x, y, z)) return 0;
    Sleep(100);
    ClickWindowCentre(false);
    Sleep(150);
    return SelectedNear(tx, ty);
}

// The selected unit counts whoever owns it: in the riddles Bilbo belongs to the team
// "PlyrCivilian/Bilbo", not to the player (test 2026-11-01 05:12: the target waited
// although Bilbo was selected 4 m away).
bool OwnUnitNear(const Scan& s, float tx, float ty) {
    for (uint32_t o : Selection()) {
        float x, y, z;
        ReadPosition(o, x, y, z);
        if (std::hypot(x - tx, y - ty) < kAutoReach) return true;
    }
    for (const Entry& e : s.entries)
        if (e.addr && e.category == kMine && e.kind != kBuildings && std::hypot(e.x - tx, e.y - ty) < kAutoReach)
            return true;
    return false;
}

void AutoWalk(const Scan& s) {
    if (!g_autoBaseline) {
        g_autoBaseline = true;
        for (const Entry& e : s.entries)
            if (e.autoWalk) g_autoPre.insert(e.id);
        LogLine(L"auto: " + std::to_wstring(g_autoPre.size()) + L" auto targets already on the map, left alone");
        return;
    }
    const Entry* target = nullptr;
    for (const Entry& e : s.entries) {
        if (!e.autoWalk || g_autoPre.count(e.id) || g_autoDone.count(e.id)) continue;
        if (e.id == g_autoTarget) { target = &e; break; }
        if (!target) target = &e;
    }
    if (!target) { g_autoTarget = 0; return; }
    float tx, ty, tz;
    ReadPosition(target->addr, tx, ty, tz);
    if (target->id != g_autoTarget) {
        if (!OwnUnitNear(s, tx, ty)) {
            if (g_autoWaiting.insert(target->id).second) {
                std::vector<uint32_t> sel = Selection();
                float sx = 0, sy = 0, sz = 0;
                if (!sel.empty()) ReadPosition(sel[0], sx, sy, sz);
                wchar_t b[220];
                swprintf_s(b, L"auto: id %u at (%.0f, %.0f) waits for a unit within 60 m; selected %zu, first %s at "
                              L"(%.0f, %.0f) owner %08X, local %08X",
                           target->id, tx, ty, sel.size(), sel.empty() ? L"-" : NameOf(sel[0]).c_str(), sx, sy,
                           sel.empty() ? 0 : OwnerOf(sel[0]), LocalPlayer());
                LogLine(b);
            }
            return;
        }
        g_autoTarget = target->id;
        g_autoTries = 0;
        g_autoUnit = 0;
        g_autoLast = 0;
        Speak(L"Ответ: " + target->name + L". Веду героя.", true);
    }
    if (g_autoUnit && IsObject(g_autoUnit)) {
        float ux, uy, uz;
        ReadPosition(g_autoUnit, ux, uy, uz);
        if (std::hypot(ux - tx, uy - ty) < 12.f) {
            LogLine(L"auto: unit on target id " + std::to_wstring(target->id));
            g_autoDone.insert(target->id);
            g_autoTarget = 0;
            return;
        }
    }
    // Scenes can block orders for a while (Gollum talks between riddles): the order is
    // repeated every 1.5 s for as long as the picture is there, 40 times at most.
    if (g_autoTries >= 40) {
        LogLine(L"auto: gave up on id " + std::to_wstring(target->id) + L" after 40 orders");
        g_autoDone.insert(target->id);
        g_autoTarget = 0;
        Speak(L"Не удалось довести героя до ответа", true);
        return;
    }
    if (GetTickCount() - g_autoLast < 1500) return;
    g_autoLast = GetTickCount();
    uint32_t unit = AutoWalker(s, tx, ty);
    if (!unit) { LogLine(L"auto: no unit could be selected near the target"); return; }
    g_autoUnit = unit;
    if (!CameraTo(tx, ty, tz)) { LogLine(L"auto: camera did not reach the target"); return; }
    ++g_autoTries;
    Sleep(100);
    if (!DirectMove(tx, ty, tz)) ClickWindowCentre(true);
    wchar_t b[160];
    float ux, uy, uz;
    ReadPosition(unit, ux, uy, uz);
    swprintf_s(b, L"auto: order %d, %s at (%.0f, %.0f) to id %u at (%.0f, %.0f)", g_autoTries, NameOf(unit).c_str(), ux,
               uy, target->id, tx, ty);
    LogLine(b);
}

// The guide place of an objective nearest to the selected unit (else to the camera).
const Entry* NearestGuidePlace(const Scan& s, const std::string& tag) {
    float rx = 0, ry = 0, rz = 0;
    std::vector<uint32_t> sel = Selection();
    if (!sel.empty()) ReadPosition(sel[0], rx, ry, rz);
    else CameraPosition(rx, ry);
    const Entry* best = nullptr;
    float bestD = 1e30f;
    for (const Entry& o : s.entries) {
        // Objectives with pick-ups only: "find 5 bushels" sends the unit to the nearest bushel.
        bool place = (strcmp(o.nameSource, "guides.ini") == 0 && o.tmpl == tag) || (o.gateStep && o.group == UpperA(tag));
        if (o.hidden || o.blocked || (!place && !((o.pickup || o.attack) && o.group == UpperA(tag)))) continue;
        float d = std::hypot(o.x - rx, o.y - ry);
        if (d < bestD) bestD = d, best = &o;
    }
    return best;
}

// Ctrl+Home: camera and cursor to the current entry. Ctrl+End: also a right click on
// it (the game's own order for the selected units: move, attack, enter, capture).
// North pads hide behind the fortress from the tilted camera: a click there selects the
// fortress (log 2026-11-04 23:03:10, 23:05:58). So the game's own selection is called.
void SelectCurrent() {
    bool resorted = false;
    Scan s = Refresh(false, resorted);
    const Entry* e = CurrentEntry(s);
    // A place (guide point) has no object: the object standing on it is selected, nearest within
    // 10 m, buildings first (Fangorn's plot places said "Это не объект", log 2026-11-08 03:5x).
    if (e && !e->addr) {
        const Entry* best = nullptr;
        float bestD = 1e30f;
        bool bestBuilding = false;
        for (const Entry& o : s.entries) {
            if (!o.addr || o.hidden) continue;
            float d = std::hypot(o.x - e->x, o.y - e->y);
            if (d >= 10 * kUnitsPerMetre) continue;
            bool building = o.kind == kBuildings;
            if (!best || (building && !bestBuilding) || (building == bestBuilding && d < bestD)) best = &o, bestD = d, bestBuilding = building;
        }
        if (best) {
            LogLine(L"scanner: select at a place -> " + best->name);
            e = best;
        }
    }
    if (!e || !e->addr) { Speak(L"Это не объект", true); return; }
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool ok = false;
    SelectObjectDirect(e->addr, done, &ok);
    bool ran = done && WaitForSingleObject(done, 1000) == WAIT_OBJECT_0;
    if (done) CloseHandle(done);
    if (!ran || !ok) Speak(L"Не выделяется: " + e->name, true);
}

// Alt+End: the selected units go to the current entry fighting whatever they meet on the way
// (the game's attack-move, MSG_DO_ATTACKMOVETO), no camera, no click.
void AttackMoveCurrent() {
    bool resorted = false;
    Scan s = Refresh(false, resorted);
    const Entry* e = CurrentEntry(s);
    if (!e && g_category == kObjectives) e = NextStep(s);
    if (!e) { Speak(L"Нет объекта", true); return; }
    if (!e->placed && e->category == kObjectives) {
        const Entry* p = NearestGuidePlace(s, e->tmpl);
        if (!p) p = NextStep(s);
        if (p) e = p;
    }
    if (!e->placed) { Speak(L"У задания нет места на карте", true); return; }
    std::vector<uint32_t> sel = Selection();
    if (sel.empty()) { Speak(L"Ничего не выделено", true); return; }
    float x = e->x, y = e->y, z = e->z;
    if (e->addr) ReadPosition(e->addr, x, y, z);
    bool sent = false;
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    AttackMoveSelectedTo(x, y, z, done, &sent);
    if (done) { WaitForSingleObject(done, 1000); CloseHandle(done); }
    if (!sent) { Speak(L"Приказ не прошёл", true); return; }
    float ux, uy, uz;
    ReadPosition(sel[0], ux, uy, uz);
    Speak(L"С боем к: " + e->name + L", " + Metres(std::hypot(x - ux, y - uy)), true);
}

void CameraOrOrder(bool order) {
    bool resorted = false;
    Scan s = Refresh(false, resorted);
    const Entry* e = CurrentEntry(s);
    if (!e && g_category == kObjectives) e = NextStep(s);
    if (!e) { Speak(L"Нет объекта", true); return; }
    if (!e->placed && e->category == kObjectives) {
        const Entry* p = NearestGuidePlace(s, e->tmpl);
        if (!p) p = NextStep(s);
        if (p) e = p;
    }
    if (!e->placed) { Speak(L"У задания нет места на карте", true); return; }
    if (order && Selection().empty()) { Speak(L"Ничего не выделено", true); return; }
    float x = e->x, y = e->y, z = e->z;
    if (e->addr) ReadPosition(e->addr, x, y, z);
    // Orders to an object go by its id, no camera and no click: nothing to miss, nothing to wait
    // for in a fight. A target the game does not take for an enemy keeps the right click below.
    // A hiding place: the game's enter order, as a right click on it gives. A move there stalled 3 m
    // short and many right clicks were needed (Dead Marshes, log 2026-11-08 01:15-01:18).
    if (order && e->hide) {
        bool sent = EnterObject(e->addr);
        Speak(sent ? L"Прячутся в: " + e->name : L"Приказ не прошёл", true);
        return;
    }
    if (order && e->attack) {
        bool sent = false;
        HANDLE sentDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ForceAttackObject(e->addr, x, y, z, sentDone, &sent);
        if (sentDone) { WaitForSingleObject(sentDone, 1000); CloseHandle(sentDone); }
        Speak(sent ? L"Освободить: " + e->name : L"Приказ не прошёл", true);
        return;
    }
    if (order && e->addr && !e->pickup && !IsMarker(e->tmpl) && AttackObject(e->addr, Selection())) {
        Speak(L"Атаковать: " + e->name, true);
        return;
    }
    float before[2], after[2];
    CameraPosition(before[0], before[1]);
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CameraLookAt(x, y, z, done);
    bool ran = done && WaitForSingleObject(done, 1000) == WAIT_OBJECT_0;
    if (done) CloseHandle(done);
    CameraPosition(after[0], after[1]);
    bool there = std::hypot(after[0] - x, after[1] - y) < 50.f;
    wchar_t b[200];
    swprintf_s(b, L"scanner: %s to id %08X (%.0f, %.0f): job %s, camera (%.0f, %.0f) -> (%.0f, %.0f)%s",
               order ? L"order" : L"camera", e->id, x, y, ran ? L"ran" : L"did not run within 1 s", before[0],
               before[1], after[0], after[1], there ? L", on target" : L", NOT on target");
    LogLine(b);
    if (!ran) { Speak(L"Камера не отвечает", true); return; }
    if (!there) Speak(L"Камера не дошла до цели. ", true);
    Sleep(100);  // a few frames, so the game picks under the cursor with the new camera
    if (!order) {
        POINT pt;
        CursorToWindowCentre(pt);
        Speak(L"Камера и курсор: " + e->name, there);
        return;
    }
    // A place (guide point, objective, radar event) gets a plain move order: a right click
    // at the window centre can land on a cliff drawn over the point (Council of Elrond, Bilbo
    // stopped by the cave cliffs 22 m from the end area, 2026-11-01 06:20 and 06:35). An
    // object keeps the right click, which means attack, enter or capture.
    if ((!e->addr || e->pickup || IsMarker(e->tmpl)) && DirectMove(x, y, z)) {
        Speak(L"Идти: " + e->name, true);
        return;
    }
    RightClickWindowCentre();
    Speak(L"Приказ: " + e->name, true);
}

// Route walk (Ctrl+Shift+End): the selected units go through the guide places of the
// current entry's objective, from the place nearest to them up to the current one, one
// move order per place. A leg ends when the unit is within 3 m, or when it has not moved
// for 4 s (then the mod says so and goes on to the next place). Ctrl+Shift+End again stops.
// Roads (roads.cpp): a leg longer than 80 m follows the map's roads when they are not much
// longer than the straight line; a straight leg that stalls is tried once more along the
// roads; a road leg that stalls gives up the roads and goes straight for its place. Road
// points are walked silently (reached within 8 m, or once the unit has passed them).
struct RoutePoint {
    float x, y;
    std::wstring name;   // for a road point: the place it leads to
    bool road = false;
    bool roadsTried = false;
};
constexpr float kRoadLeg = 800.f, kRoadReach = 80.f;
int g_threatBand = 0;  // 0 clear, 1 near, 2 close, 3 at the detection radius
std::vector<RoutePoint> g_route;
std::string g_routeTag;  // objective of the route: its map path is the fallback
size_t g_routeLeg = 0;
uint32_t g_routeUnit = 0;
float g_routeLastX = 0, g_routeLastY = 0;
DWORD g_routeMoved = 0, g_routeLegStart = 0, g_routeCheck = 0, g_routeLegLimit = 90000;
int g_routeStalls = 0;
DWORD g_routePaused = 0, g_routeTalkHold = 0;  // since when; 0 = not paused / not holding
DWORD g_routeDanger = 0;                       // since when the unit stands for a patrol
bool g_routePauseSaid = false;

void RouteLeg() {
    const RoutePoint& p = g_route[g_routeLeg];
    float ux, uy, uz;
    ReadPosition(g_routeUnit, ux, uy, uz);
    g_routeLastX = ux, g_routeLastY = uy;
    g_routeMoved = g_routeLegStart = GetTickCount();
    // 60 ms a unit: Frodo needed 200 s for 372 m with a detour and heard "Застрял" while walking
    // (log 2026-11-06 00:47:44). A unit that stands is caught by the 4 s check.
    g_routeLegLimit = 90000 + (DWORD)(60.f * std::hypot(p.x - ux, p.y - uy));
    bool ok = DirectMove(p.x, p.y, 0);
    wchar_t b[200];
    swprintf_s(b, L"route: leg %zu of %zu, %s at (%.0f, %.0f) to (%.0f, %.0f) %s", g_routeLeg + 1, g_route.size(),
               NameOf(g_routeUnit).c_str(), ux, uy, p.x, p.y, ok ? L"ordered" : L"order REFUSED");
    LogLine(b + std::wstring(p.road ? L" road to " : L" ") + p.name);
    if (!ok) {
        Speak(L"Приказ не прошёл, маршрут остановлен", true);
        g_route.clear();
    }
}

std::vector<std::string> ObjectiveTags() {
    std::vector<std::string> tags;
    for (const Objective& o : ReadObjectives()) tags.push_back(o.tag);
    return tags;
}

// Road points from (ax, ay) to the place `to`, named after it; false when there are none.
bool RoadsBetween(float ax, float ay, const RoutePoint& to, bool allowDirect, std::vector<RoutePoint>& out) {
    out.clear();
    float maxX = 0, maxY = 0;
    if (!ActiveBorder(maxX, maxY)) maxX = maxY = 0;
    std::vector<RoadPoint> pts;
    if (!RoadsPlan(ObjectiveTags(), ax, ay, to.x, to.y, maxX, maxY, allowDirect, pts)) return false;
    for (const RoadPoint& r : pts) out.push_back({r.x, r.y, to.name, true});
    return !out.empty();
}

// The places with road points put in front of every leg longer than 80 m.
std::vector<RoutePoint> WithRoads(const std::vector<RoutePoint>& places, float ux, float uy, size_t& roadPoints) {
    std::vector<RoutePoint> out;
    float px = ux, py = uy;
    for (const RoutePoint& p : places) {
        std::vector<RoutePoint> r;
        if (std::hypot(p.x - px, p.y - py) > kRoadLeg && RoadsBetween(px, py, p, true, r)) {
            out.insert(out.end(), r.begin(), r.end());
            roadPoints += r.size();
        }
        out.push_back(p);
        out.back().roadsTried = !r.empty();
        px = p.x, py = p.y;
    }
    return out;
}

// Every given unit follows the map path exactly (gamethread.cpp FollowWaypointPath).
int WalkMapPath(const std::vector<uint32_t>& units, const std::string& label) {
    int started = 0;
    for (uint32_t o : units) {
        float x, y, z;
        ReadPosition(o, x, y, z);
        HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        bool ok = false;
        FollowWaypointPath(o, x, y, z, label, done, &ok);
        if (done) { WaitForSingleObject(done, 1000); CloseHandle(done); }
        started += ok;
    }
    return started;
}

// A map path walks a unit straight along its waypoints, whatever stands between. It belongs to the
// stretch of map its objective is about: from 200 m off it took Frodo out of Rivendell for good.
constexpr float kMapPathReach = 2000.f;

bool NearGuide(const Guide& g, float x, float y) {
    for (const GuidePlace& p : g.places)
        if (std::hypot(p.x - x, p.y - y) < kMapPathReach) return true;
    return false;
}

void CameraOrOrder(bool order);

// What a mission keeps shut (data\gates.txt, tools\make_gates.py). Script action 507
// HUMAN_IMPASSABLE_AREA, case 0x7CF317 -> 0x7C4040: marks the pathfinder's cells and records the
// area's state in a std::map at ScriptEngine +0x191D0 (0x6093D8): head [+0], root [head + 4], node
// +8 left, +0xC right, +0x10 AsciiString name, +0x14 byte: 1 shut.
constexpr uint32_t kShutAreas = 0x191D0;
struct Gate {
    std::string area;  // upper-case
    float x1, y1, x2, y2;
    std::wstring words;
    std::vector<std::pair<float, float>> go;    // where to go so that it opens, in story order
    std::vector<std::pair<float, float>> poly;  // the area's own outline (the box is only its bounds)
};
// "@ x y | need A; B | any C; D" (tools\make_gates.py floods the map's walkable cells from the start):
// the guide place at x y is cut off while A or B is shut, or while C and D both are.
struct GateCut {
    float x = 0, y = 0;
    std::vector<std::string> need, any;  // upper-case areas
};
std::map<std::string, std::vector<Gate>> g_gates;        // map folder -> gates
std::map<std::string, std::vector<GateCut>> g_gateCuts;  // map folder -> places they cut off
std::set<std::string> g_gateVisited;  // "map#AREA#i": an own unit stood at go point i of that gate
std::string g_stallGate, g_stallTag;  // the shut area the last route stalled at, and that route's objective

std::vector<std::pair<float, float>> ParsePoints(const std::string& v) {
    std::vector<std::pair<float, float>> out;
    std::stringstream ss(v);
    for (std::string pt; std::getline(ss, pt, ';');) {
        float x, y;
        if (sscanf_s(pt.c_str(), "%f %f", &x, &y) == 2) out.push_back({x, y});
    }
    return out;
}

std::vector<std::string> ParseAreas(const std::string& v) {
    std::vector<std::string> out;
    std::stringstream ss(v);
    for (std::string a; std::getline(ss, a, ';');) {
        size_t b = a.find_first_not_of(' '), e = a.find_last_not_of(' ');
        if (b != std::string::npos) out.push_back(UpperA(a.substr(b, e - b + 1)));
    }
    return out;
}

void LoadGates(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    std::string line, map;
    int n = 0, cuts = 0;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == ';') continue;
        if (line[0] == '[' && line.back() == ']') { map = line.substr(1, line.size() - 2); continue; }
        if (map.empty()) continue;
        std::vector<std::string> part;
        for (size_t from = 0;;) {
            size_t bar = line.find(" | ", from);
            part.push_back(line.substr(from, bar == std::string::npos ? bar : bar - from));
            if (bar == std::string::npos) break;
            from = bar + 3;
        }
        if (line[0] == '@') {
            GateCut c;
            if (sscanf_s(line.c_str() + 1, "%f %f", &c.x, &c.y) != 2) continue;
            for (size_t i = 1; i < part.size(); ++i) {
                if (part[i].compare(0, 4, "need") == 0) c.need = ParseAreas(part[i].substr(4));
                else if (part[i].compare(0, 3, "any") == 0) c.any = ParseAreas(part[i].substr(3));
            }
            g_gateCuts[map].push_back(c);
            ++cuts;
            continue;
        }
        if (part.size() < 3) continue;
        Gate g;
        g.area = UpperA(part[0]);
        if (sscanf_s(part[1].c_str(), "%f %f %f %f", &g.x1, &g.y1, &g.x2, &g.y2) != 4) continue;
        g.words = Widen(part[2].c_str());
        for (size_t i = 3; i < part.size(); ++i) {
            if (part[i].compare(0, 2, "go") == 0) g.go = ParsePoints(part[i].substr(2));
            else if (part[i].compare(0, 4, "poly") == 0) g.poly = ParsePoints(part[i].substr(4));
        }
        g_gates[map].push_back(g);
        ++n;
    }
    LogLine(L"gates: " + std::to_wstring(n) + L" on " + std::to_wstring(g_gates.size()) + L" maps, " +
            std::to_wstring(cuts) + L" places they cut off");
}

std::set<std::string> ShutAreas() {
    std::set<std::string> out;
    uint32_t engine = U32(0xDE3BAC);
    uint32_t head = Ptr(engine) ? U32(engine + kShutAreas) : 0;
    uint32_t root = Ptr(head) ? U32(head + 4) : 0;
    std::vector<uint32_t> todo;
    if (Ptr(root)) todo.push_back(root);
    for (int guard = 0; !todo.empty() && guard < 5000; ++guard) {
        uint32_t node = todo.back();
        todo.pop_back();
        uint32_t l = U32(node + 8), r = U32(node + 0xC);
        if (Ptr(l)) todo.push_back(l);
        if (Ptr(r)) todo.push_back(r);
        uint8_t shut = 0;
        if (SafeRead(node + 0x14, &shut, 1) && shut) out.insert(UpperA(game::ReadAsciiString(U32(node + 0x10))));
    }
    return out;
}

// Does the segment cross the box? Liang-Barsky clipping.
bool SegmentHitsBox(float ax, float ay, float bx, float by, const Gate& g) {
    float t0 = 0, t1 = 1, dx = bx - ax, dy = by - ay;
    const float p[4] = {-dx, dx, -dy, dy};
    const float q[4] = {ax - g.x1, g.x2 - ax, ay - g.y1, g.y2 - ay};
    for (int i = 0; i < 4; ++i) {
        if (p[i] == 0) {
            if (q[i] < 0) return false;
            continue;
        }
        float r = q[i] / p[i];
        if (p[i] < 0) t0 = (std::max)(t0, r);
        else t1 = (std::min)(t1, r);
        if (t0 > t1) return false;
    }
    return true;
}

bool InGate(const Gate& g, float x, float y) {
    if (x < g.x1 || x > g.x2 || y < g.y1 || y > g.y2) return false;
    if (g.poly.size() < 3) return true;
    bool in = false;  // even-odd rule, as tools\objective_places.py inside()
    for (size_t i = 0, j = g.poly.size() - 1; i < g.poly.size(); j = i++) {
        float x1 = g.poly[i].first, y1 = g.poly[i].second, x2 = g.poly[j].first, y2 = g.poly[j].second;
        if ((y1 > y) != (y2 > y) && x < (x2 - x1) * (y - y1) / (y2 - y1) + x1) in = !in;
    }
    return in;
}

// The script engine's table is read once a second at most: every scan asks it.
const std::set<std::string>& ShutAreasNow() {
    static DWORD at = 0;
    static std::set<std::string> shut;
    if (!at || GetTickCount() - at > 1000) {
        shut = ShutAreas();
        at = GetTickCount();
    }
    return shut;
}

// Shut gates that cut the point off: the flood's record for a guide place (walls across a valley
// too), else a shut area the point lies inside (objects of an objective, the opening places).
std::vector<const Gate*> Blockers(const std::vector<Gate>& gates, const std::vector<GateCut>* cuts,
                                  const std::set<std::string>& shut, float x, float y) {
    auto find = [&gates](const std::string& a) -> const Gate* {
        for (const Gate& g : gates)
            if (g.area == a) return &g;
        return nullptr;
    };
    std::vector<const Gate*> out;
    if (cuts)
        for (const GateCut& c : *cuts) {
            if (std::hypot(c.x - x, c.y - y) > 15.f) continue;
            for (const std::string& a : c.need)
                if (shut.count(a))
                    if (const Gate* g = find(a)) out.push_back(g);
            bool all = !c.any.empty();
            for (const std::string& a : c.any) all = all && shut.count(a);
            if (all)
                for (const std::string& a : c.any)
                    if (const Gate* g = find(a)) out.push_back(g);
            break;
        }
    if (out.empty())
        for (const Gate& g : gates)
            if (shut.count(g.area) && InGate(g, x, y)) out.push_back(&g);
    return out;
}

// Westfold, 2026-11-14: the captain stood in the keep, shut until six attacker teams fell, and the
// route walked into the town wall. Each target a shut way cuts off is marked, and the place that
// opens the way ("сначала: ...") goes into "цели" beside it, so the story is walked in its order.
void MarkShutWays(Scan& s, const std::vector<Objective>& objectives) {
    if (objectives.empty()) return;
    std::vector<std::string> tags;
    for (const Objective& o : objectives) tags.push_back(o.tag);
    std::string map = RoadsMapName(tags);
    auto it = g_gates.find(map);
    if (it == g_gates.end()) return;
    const std::set<std::string>& shut = ShutAreasNow();
    if (shut.empty()) return;
    auto ct = g_gateCuts.find(map);
    const std::vector<GateCut>* cuts = ct == g_gateCuts.end() ? nullptr : &ct->second;
    std::vector<std::pair<float, float>> own;
    for (const Entry& e : s.entries)
        if (e.addr && e.category == kMine) own.push_back({e.x, e.y});
    // 15 m: an own unit already there (a game loaded beyond the wall) is proof enough.
    auto ownNear = [&own](float x, float y) {
        for (const auto& o : own)
            if (std::hypot(o.first - x, o.second - y) < 150.f) return true;
        return false;
    };
    auto key = [&map](const Gate& g, size_t i) { return map + "#" + g.area + "#" + std::to_string(i); };
    for (const Gate& g : it->second)
        if (shut.count(g.area))
            for (size_t i = 0; i < g.go.size(); ++i)
                if (ownNear(g.go[i].first, g.go[i].second)) g_gateVisited.insert(key(g, i));
    // The go point of a gate still to be walked to; when it is cut off by another shut gate, that
    // gate's point instead (Westfold: the keep's waves start inside the shut town centre).
    std::function<bool(const Gate*, int, const Gate*&, size_t&)> step = [&](const Gate* g, int depth, const Gate*& og, size_t& oi) {
        // The first point not walked to yet, and never one inside the shut area itself (a wave can
        // spawn in there and walk out); all walked: the last one, where the fight goes on.
        size_t i = g->go.size();
        for (size_t k = 0; k < g->go.size(); ++k) {
            if (InGate(*g, g->go[k].first, g->go[k].second)) continue;
            i = k;
            if (!g_gateVisited.count(key(*g, k))) break;
        }
        if (i == g->go.size()) return false;
        if (depth < 4)
            for (const Gate* b : Blockers(it->second, cuts, shut, g->go[i].first, g->go[i].second))
                if (b != g && step(b, depth + 1, og, oi)) return true;
        og = g, oi = i;
        return true;
    };
    struct Step {
        const Gate* gate;
        size_t point;
        const Entry* from;
    };
    std::vector<Step> steps;
    int marked = 0;
    for (Entry& e : s.entries) {
        if (e.category != kTargets || e.hidden || !e.placed || ownNear(e.x, e.y)) continue;
        std::vector<const Gate*> b = Blockers(it->second, cuts, shut, e.x, e.y);
        if (b.empty()) continue;
        e.blocked = true;
        ++marked;
        const Gate* og = nullptr;
        size_t oi = 0;
        for (const Gate* g : b)
            if (step(g, 0, og, oi)) break;
        if (og) {
            e.name += L", путь пока закрыт";
            bool dup = false;
            for (const Step& st : steps) dup |= st.gate == og && st.from->group == e.group && st.from->tmpl == e.tmpl;
            if (!dup) steps.push_back({og, oi, &e});
        } else {
            const std::wstring& w = b[0]->words;
            e.name += w.empty() ? L", путь пока закрыт, он откроется по ходу миссии" : L", путь пока закрыт. Сначала: " + w;
        }
    }
    // A route that stalled at a shut way the flood did not foresee: its opening place too.
    if (!g_stallGate.empty() && shut.count(g_stallGate)) {
        for (const Entry& e : s.entries) {
            if (e.category != kTargets || e.group != UpperA(g_stallTag) || e.blocked) continue;
            for (const Gate& g : it->second) {
                const Gate* og = nullptr;
                size_t oi = 0;
                if (g.area == g_stallGate && step(&g, 0, og, oi)) steps.push_back({og, oi, &e});
            }
            break;
        }
    }
    std::vector<Entry> add;
    for (const Step& st : steps) {
        float x = st.gate->go[st.point].first, y = st.gate->go[st.point].second;
        // A hand guide walks this stretch already ("путь 1-5" of Westfold's Objective02).
        bool covered = false;
        for (const Entry& h : s.entries)
            covered |= h.category == kTargets && !h.blocked && !h.addr && !h.autoPlace && h.group == st.from->group &&
                       h.tmpl == st.from->tmpl && strcmp(h.nameSource, "guides.ini") == 0 && std::hypot(h.x - x, h.y - y) < 600.f;
        if (covered) continue;
        Entry e;
        e.id = 0xA2000000u | ((uint32_t)add.size() << 8) | (uint32_t)st.point;
        e.category = kTargets;
        e.kind = kAll;
        e.group = st.from->group;
        e.shelf = st.from->shelf;
        e.tmpl = st.from->tmpl;
        e.order = st.from->order;
        e.nameSource = "gates.txt";
        e.gateStep = true;
        e.x = x, e.y = y;
        e.name = st.gate->words.empty() ? L"сначала: сюда, путь дальше откроется по ходу миссии" : L"сначала: " + st.gate->words;
        add.push_back(e);
    }
    s.entries.insert(s.entries.end(), add.begin(), add.end());
    static std::string lastLog;
    std::string sig = map + " " + std::to_string(shut.size()) + " " + std::to_string(marked) + " " + std::to_string(add.size());
    if (sig != lastLog) {
        lastLog = sig;
        wchar_t b[200];
        swprintf_s(b, L"gates: %zu areas shut, %d targets cut off, %zu opening places listed", shut.size(), marked, add.size());
        std::wstring line = b;
        for (const Entry& e : add) line += L"; " + e.name + L" at (" + std::to_wstring((int)e.x) + L", " + std::to_wstring((int)e.y) + L")";
        LogLine(line);
    }
}

// The opening place listed for a target cut off by a shut way, nearest the unit.
const Entry* GateStepFor(const Scan& s, const Entry& target, float ux, float uy) {
    const Entry* best = nullptr;
    for (const Entry& e : s.entries) {
        if (!e.gateStep || e.hidden) continue;
        bool mine = (!target.group.empty() && e.group == target.group) || e.tmpl == target.tmpl ||
                    (target.category == kObjectives && e.group == UpperA(target.tmpl));
        if (mine && (!best || std::hypot(e.x - ux, e.y - uy) < std::hypot(best->x - ux, best->y - uy))) best = &e;
    }
    return best;
}

// The shut area on the way from the unit to the target, or the one the unit stands at. Empty when
// none: the walk failed for another reason.
std::wstring GateHint(float ux, float uy, float tx, float ty) {
    std::vector<std::string> tags;
    for (const Objective& o : ReadObjectives()) tags.push_back(o.tag);
    auto it = g_gates.find(RoadsMapName(tags));
    if (it == g_gates.end()) return L"";
    std::set<std::string> shut = ShutAreas();
    const Gate* best = nullptr;
    float bestD = 1e30f;
    for (const Gate& g : it->second) {
        if (!shut.count(g.area)) continue;
        float cx = (std::max)(g.x1, (std::min)(ux, g.x2)), cy = (std::max)(g.y1, (std::min)(uy, g.y2));
        float d = std::hypot(cx - ux, cy - uy);
        // Near the unit, or across the line to the target within the next 100 m.
        if (d > 300.f && !(d < 1000.f && SegmentHitsBox(ux, uy, tx, ty, g))) continue;
        if (d < bestD) bestD = d, best = &g;
    }
    wchar_t b[160];
    swprintf_s(b, L"gates: %zu areas shut, at (%.0f, %.0f) nearest %S", shut.size(), ux, uy, best ? best->area.c_str() : "none");
    LogLine(b);
    if (!best) return L"";
    // The next scans list the place that opens it under the route's objective (MarkShutWays).
    g_stallGate = best->area;
    g_stallTag = g_routeTag;
    std::wstring there = best->go.empty() ? L"" : L" Место для этого добавлено в цели.";
    return best->words.empty() ? L"Проход закрыт, он откроется по ходу миссии." + there
                               : L"Проход закрыт. Похоже, сначала нужно: " + best->words + L"." + there;
}

void RouteStop(const std::wstring& why) {
    g_route.clear();
    g_routePaused = g_routeTalkHold = g_routeDanger = 0;
    LogLine(L"route: stopped, " + why);
}

void RouteStart() {
    if (!g_route.empty()) { RouteStop(L"by the player"); Speak(L"Маршрут остановлен", true); return; }
    GetAsyncKeyState('S');  // an S pressed before this walk does not end it
    bool resorted = false;
    Scan s = Refresh(false, resorted);
    const Entry* e = CurrentEntry(s);
    if (!e && g_category == kObjectives) e = NextStep(s);
    if (!e) { Speak(L"Нет места для маршрута", true); return; }
    // An objective (category "задания") walks all its guide places; a guide place walks the
    // places of its objective up to itself.
    bool objective = !e->placed && e->category == kObjectives;
    if (e->attack || e->hide) { CameraOrOrder(true); return; }  // it stands still: the order walks there
    bool guidePlace = strcmp(e->nameSource, "guides.ini") == 0 && !e->addr;  // not an objective's object
    if (!e->placed && !objective) { Speak(L"Нет места для маршрута", true); return; }
    std::vector<uint32_t> sel = Selection();
    if (sel.empty()) { Speak(L"Ничего не выделено", true); return; }
    float ux, uy, uz;
    ReadPosition(sel[0], ux, uy, uz);
    std::string tag = (objective || guidePlace) ? e->tmpl : std::string();
    auto g = tag.empty() ? g_guides.end() : g_guides.find(UpperA(tag));
    // A hand-written map path is walked at once, the way the mission's scripts move their
    // own units: ordinary move orders could not get Bilbo out of Gollum's cave (2026-11-01
    // 06:52-06:54, "Застрял"); the dwarves' path did (07:19, objective completed 28 s later).
    if (g != g_guides.end() && !g->second.path.empty() && NearGuide(g->second, ux, uy)) {
        int started = WalkMapPath(sel, g->second.path);
        Speak(started ? L"Иду по дорожке карты к: " + e->name : L"Дорожка карты не найдена", true);
        return;
    }
    std::vector<const Entry*> places;
    if (objective || guidePlace) {
        uint32_t last = objective ? 0xFF : (e->id & 0xFF);
        for (const Entry& o : s.entries)
            if (!o.hidden && strcmp(o.nameSource, "guides.ini") == 0 && o.tmpl == tag && (o.id & 0xFF) <= last) places.push_back(&o);
        std::sort(places.begin(), places.end(), [](const Entry* a, const Entry* b) { return (a->id & 0xFF) < (b->id & 0xFF); });
        // Start at the place nearest to the unit: the ones before it are behind.
        size_t from = 0;
        float best = 1e30f;
        for (size_t i = 0; i < places.size(); ++i) {
            float d = std::hypot(places[i]->x - ux, places[i]->y - uy);
            if (d < best) best = d, from = i;
        }
        if (!places.empty()) places.erase(places.begin(), places.begin() + from);
    } else {
        places.push_back(e);
    }
    // What can be taken comes first: a pick-up can be what opens the way (the Ring in Gollum's cave).
    if (objective) {
        const Entry* best = nullptr;
        for (const Entry& o : s.entries)
            if ((o.pickup || o.attack) && !o.hidden && !o.blocked && o.group == UpperA(tag) &&
                (!best || std::hypot(o.x - ux, o.y - uy) < std::hypot(best->x - ux, best->y - uy)))
                best = &o;
        if (best && best->attack) { CameraOrOrder(true); return; }
        if (best) places.assign(1, best);
    }
    if (places.empty() && objective) {
        const Entry* p = NearestGuidePlace(s, tag);
        if (!p) p = NextStep(s);
        if (p) places.push_back(p);
    }
    // A target a shut way cuts off is no place to walk to yet (MarkShutWays): the place that opens the
    // way comes first. Westfold: Aragorn walked into the town wall toward the captain, 2026-11-14 23:31.
    size_t cutOff = 0;
    std::wstring cutName;
    for (size_t i = 0; i < places.size();) {
        if (!places[i]->blocked) { ++i; continue; }
        if (cutName.empty()) cutName = places[i]->name;
        places.erase(places.begin() + i);
        ++cutOff;
    }
    std::wstring closed;
    if (cutOff) {
        const Entry* step = GateStepFor(s, *e, ux, uy);
        if (step) places.insert(places.begin(), step);
        closed = step ? L"Путь к цели пока закрыт. " : L"";
        LogLine(L"route: " + std::to_wstring(cutOff) + L" places cut off by a shut way" + (step ? L", first " + step->name : L""));
        if (places.empty()) {
            Speak(cutName + L". Обойти пока нельзя: выполните сначала другие цели задания.", true);
            return;
        }
    }
    if (places.empty()) {
        Speak(L"У задания нет мест на карте", true);
        return;
    }
    std::vector<RoutePoint> route;
    for (const Entry* p : places) {
        float x = p->x, y = p->y, z;
        if (p->addr) ReadPosition(p->addr, x, y, z);
        route.push_back({x, y, p->name});
    }
    size_t roadPoints = 0;
    g_route = WithRoads(route, ux, uy, roadPoints);
    g_routeTag = tag;
    g_routeLeg = 0;
    g_routeUnit = sel[0];
    g_routeStalls = 0;
    // "Gladden, Defence Area (для Elendur)": the script tests that hero, another unit does nothing there.
    std::wstring warn;
    size_t who = route[0].name.find(L"(для ");
    if (who != std::wstring::npos && route[0].name.find(NameOf(sel[0]), who) == std::wstring::npos)
        warn = L" Точка для другого героя: " + route[0].name.substr(who + 5, route[0].name.size() - who - 6) + L".";
    Speak(closed + L"Маршрут, точек: " + std::to_wstring(route.size()) + (roadPoints ? L". По дорогам карты" : L"") +
              L". Иду к: " + g_route[0].name + L"." + warn, true);
    RouteLeg();
}

// About every 100 ms from ScannerTick; checks twice a second.
void RouteTick() {
    // The player's own Stop ends the walk: Frodo stopped under the cloaks to hide from the
    // Nazgul, and the stall check sent him on (Shadow of the Past, 2026-11-04 21:47:47).
    bool stopKey = (GetAsyncKeyState('S') & 0x8001) != 0;
    if (!g_route.empty() && stopKey && GetForegroundWindow() == g_wnd && !MenuOverlay()) {
        RouteStop(L"the player pressed Stop");
        Speak(L"Маршрут отменён", true);
        return;
    }
    if (g_route.empty() || GetTickCount() - g_routeCheck < 500) return;
    DWORD sinceCheck = GetTickCount() - g_routeCheck;
    g_routeCheck = GetTickCount();
    // The game stands still under a pause menu: that is no stall (Shadow of the Past, 21:52).
    if (MenuOverlay()) {
        g_routeMoved += sinceCheck;
        g_routeLegStart += sinceCheck;
        return;
    }
    if (!IsObject(g_routeUnit)) { RouteStop(L"the unit is gone"); Speak(L"Маршрут остановлен: отряда нет", true); return; }
    std::vector<uint32_t> sel = Selection();
    if (std::find(sel.begin(), sel.end(), g_routeUnit) == sel.end()) {
        // Scripted scenes clear the selection (5 walks lost in the Shire, 2026-11-02): an empty
        // selection pauses the walk, another unit selected ends it.
        if (!sel.empty()) {
            g_routePaused = 0;
            RouteStop(L"the unit is no longer selected");
            Speak(L"Маршрут остановлен: выделение изменилось", true);
            return;
        }
        DWORD at = GetTickCount();
        if (!g_routePaused) {
            g_routePaused = at, g_routePauseSaid = false;
            LogLine(L"route: paused, nothing selected");
        }
        if (!g_routePauseSaid && at - g_routePaused > 3000) {
            g_routePauseSaid = true;
            Speak(L"Маршрут на паузе: выделите героя, и он пойдёт дальше", false);
        }
        if (at - g_routePaused > 300000) {
            g_routePaused = 0;
            RouteStop(L"pause timed out");
        }
        return;
    }
    if (g_routeDanger) {
        if (GetTickCount() - g_routeDanger < 60000 && g_threatBand >= 2) return;
        g_routeDanger = 0;
        LogLine(L"route: patrol gone, walking on");
        Speak(L"Иду дальше", false);
        RouteLeg();
        return;
    }
    if (g_routePaused) {
        g_routePaused = 0;
        LogLine(L"route: resumed");
        if (g_routePauseSaid) Speak(L"Продолжаю маршрут к: " + g_route[g_routeLeg].name, false);
        RouteLeg();
        return;
    }
    float ux, uy, uz;
    ReadPosition(g_routeUnit, ux, uy, uz);
    const RoutePoint& p = g_route[g_routeLeg];
    float d = std::hypot(p.x - ux, p.y - uy);
    DWORD now = GetTickCount();
    if (std::hypot(ux - g_routeLastX, uy - g_routeLastY) > 10.f) {
        g_routeLastX = ux, g_routeLastY = uy;
        g_routeMoved = now;
    }
    bool reached = d < (p.road ? kRoadReach : 30.f);
    // A road point the unit has already passed (it is nearer the next point than the road
    // point itself) counts as reached: no walking back to it.
    if (p.road && !reached && g_routeLeg + 1 < g_route.size()) {
        const RoutePoint& q = g_route[g_routeLeg + 1];
        reached = std::hypot(q.x - ux, q.y - uy) < std::hypot(q.x - p.x, q.y - p.y);
    }
    bool stalled = !reached && now - g_routeMoved > 4000;
    if (!reached && !stalled && now - g_routeLegStart < g_routeLegLimit) return;
    wchar_t b[200];
    swprintf_s(b, L"route: leg %zu%s %s, unit at (%.0f, %.0f), %.0f units from (%.0f, %.0f)", g_routeLeg + 1,
               p.road ? L" (road)" : L"", reached ? L"reached" : stalled ? L"STALLED" : L"timed out", ux, uy, d, p.x, p.y);
    LogLine(b);
    if (p.road) {
        if (!reached) {
            // Off the roads, straight for the place they led to.
            size_t next = g_routeLeg;
            while (next < g_route.size() && g_route[next].road) ++next;
            g_route.erase(g_route.begin() + g_routeLeg, g_route.begin() + next);
            Speak(L"Дорога не прошла. Иду прямо к: " + g_route[g_routeLeg].name, true);
        } else {
            ++g_routeLeg;
        }
        RouteLeg();
        return;
    }
    if (!reached && !p.roadsTried) {
        g_route[g_routeLeg].roadsTried = true;
        std::vector<RoutePoint> r;
        if (RoadsBetween(ux, uy, g_route[g_routeLeg], false, r)) {
            std::wstring name = g_route[g_routeLeg].name;
            g_route.insert(g_route.begin() + g_routeLeg, r.begin(), r.end());
            Speak(L"Напрямую не прошёл. Иду по дорогам карты к: " + name, true);
            RouteLeg();
            return;
        }
    }
    // A talk in the Shire goes on only while Frodo stands by (the later lines test his area
    // too): the route waits for its end before the next place. Wilmer, 2026-11-02 21:47.
    if (reached && g_routeLeg + 1 < g_route.size()) {
        DWORD talk = HintsLastTalk();
        if (!g_routeTalkHold && now - HintsLastShireTalk() < 3000) {
            g_routeTalkHold = now;
            LogLine(L"route: waiting for the talk to end");
            Speak(L"Жду конца разговора", false);
        }
        if (g_routeTalkHold) {
            if (now - talk < 14000 && now - g_routeTalkHold < 120000) return;
            g_routeTalkHold = 0;
        }
    }
    std::wstring say = reached ? L"" : L"Застрял у точки " + p.name + L", " + std::to_wstring((int)std::lround(d / kUnitsPerMetre)) + L" м. ";
    if (++g_routeLeg >= g_route.size()) {
        g_route.clear();
        LogLine(L"route: finished");
        // No map path here any more: it walks a unit through walls the mission keeps shut. Twice it
        // broke a story (Bilbo out of the cave 01:13:16, past the cocoons 01:51:44). A stall that
        // ends a route is usually such a wall.
        std::wstring hint = reached ? L"" : GateHint(ux, uy, p.x, p.y);
        if (hint.empty()) hint = L"Цель не достигнута. Возможно, проход закрыт: нужно сперва сделать что-то другое.";
        Speak(say + (reached ? L"Маршрут пройден" : hint), true);
        return;
    }
    Speak(say + L"Дальше: " + g_route[g_routeLeg].name, true);
    RouteLeg();
}

// Stealth: detection in these missions is plain distance (scripts "G ... Spotted":
// DISTANCE_BETWEEN_TEAM <patrol> <hero> < radius; Amon Hen 7 m, Pelargir 15 m), so the player
// gets the nearest threat in bands around the radius, and a route stops before the patrol
// gets there.
float g_threatLast = 0;
DWORD g_threatSaid = 0;

const Threat* NearestThreat(const Scan& s, float ux, float uy, float& best) {
    const Threat* who = nullptr;
    best = 1e9f;
    for (const Threat& t : s.threats) {
        float d = std::hypot(t.x - ux, t.y - uy);
        if (d < best) best = d, who = &t;
    }
    return who;
}

void StealthTick() {
    if (NoCatchOn()) { g_threatBand = 0; return; }  // nobody notices: no warnings, no route stops
    bool wanted = false;
    for (const Objective& o : ReadObjectives()) {
        auto g = g_guides.find(UpperA(o.tag));
        wanted |= o.shown && !o.done && g != g_guides.end() && g->second.danger > 0;
    }
    if (!wanted) { g_threatBand = 0; return; }
    std::vector<uint32_t> sel = Selection();
    if (sel.empty()) return;
    Scan s = RunScan();
    float ux, uy, uz;
    ReadPosition(sel[0], ux, uy, uz);
    float d = 0;
    const Threat* t = NearestThreat(s, ux, uy, d);
    // Riders cover 10 m in a second or two: the first word comes 20 m before the radius.
    int band = !t || d > s.danger + 200.f ? 0 : d > s.danger + 100.f ? 1 : d > s.danger + 30.f ? 2 : 3;
    DWORD now = GetTickCount();
    bool closing = d < g_threatLast - 5.f;
    if (band != g_threatBand || (band >= 2 && now - g_threatSaid > 4000)) {
        std::wstring w;
        if (t && band) {
            w = (band == 3 ? L"Опасно! " : L"") + t->name + L", " + std::to_wstring((int)std::lround(d / kUnitsPerMetre)) +
                L" м, " + Direction(t->x - ux, t->y - uy);
        } else if (g_threatBand) {
            w = L"Рядом чисто";
        }
        if (!w.empty()) {
            Speak(w, band == 3);
            g_threatSaid = now;
        }
        LogLine(L"stealth: band " + std::to_wstring(band) + L" " + w);
    }
    g_threatBand = band;
    g_threatLast = d;
    if (!g_route.empty() && !g_routeDanger && band >= 2 && closing && g_routeUnit == sel[0]) {
        g_routeDanger = now;
        DirectMove(ux, uy, uz);
        LogLine(L"route: stopped for a patrol");
        Speak(L"Стою, патруль рядом", false);
    }
}

// Ctrl+Shift+Home: the three nearest threats and the detection radius.
void ThreatReport() {
    if (NoCatchOn()) { Speak(L"Включена вечная скрытность, патрули вас не заметят", true); return; }
    Scan s = RunScan();
    std::vector<uint32_t> sel = Selection();
    if (s.danger <= 0 || sel.empty()) { Speak(L"Сейчас не нужно прятаться", true); return; }
    float ux, uy, uz;
    ReadPosition(sel[0], ux, uy, uz);
    std::sort(s.threats.begin(), s.threats.end(), [&](const Threat& a, const Threat& b) {
        return std::hypot(a.x - ux, a.y - uy) < std::hypot(b.x - ux, b.y - uy);
    });
    std::wstring w = L"Замечают с " + std::to_wstring((int)std::lround(s.danger / kUnitsPerMetre)) + L" м.";
    for (size_t i = 0; i < s.threats.size() && i < 3; ++i) {
        const Threat& t = s.threats[i];
        w += L" " + t.name + L", " + std::to_wstring((int)std::lround(std::hypot(t.x - ux, t.y - uy) / kUnitsPerMetre)) +
             L" м, " + Direction(t.x - ux, t.y - uy) + L".";
    }
    if (s.threats.empty()) w += L" Врагов не видно.";
    Speak(w, true);
}

// New and completed objectives are spoken (queued, so dialogue lines are not cut),
// with the guide's words for a new one. The first read of a battle is the baseline.
std::map<int, std::pair<bool, bool>> g_objState;
bool g_objBaseline = false;
std::set<std::string> g_objSaid;  // upper-case tags announced this battle
std::set<uint32_t> g_markersSeen;  // ids of the game's lights on the map
bool g_markerBaseline = false;

// The game shows its light silently: said as it appears. Those of the first look at a battle are
// no news (a loaded game).
void WatchMarkers(const Scan& s) {
    // The filter can flip when the last unit in the clear dies or a scene moves the heroes: seen in the log.
    static bool lastOn = false;
    if (s.shroudOn != lastOn) {
        lastOn = s.shroudOn;
        wchar_t b[120];
        swprintf_s(b, L"shroud: filter %s, own units %d of %d in the clear, hidden %d", s.shroudOn ? L"on" : L"off", s.ownClear, s.ownAll, s.shrouded);
        LogLine(b);
    }
    std::vector<const Entry*> fresh;
    std::set<uint32_t> now;
    for (const Entry& e : s.entries) {
        if (e.category != kTargets || strcmp(e.shelf, "#G")) continue;
        now.insert(e.id);
        if (!g_markersSeen.count(e.id)) fresh.push_back(&e);
    }
    bool first = !g_markerBaseline;
    g_markerBaseline = true;
    g_markersSeen = now;
    // The mission's own ping: a flash on the radar, gone in seconds for the eye too.
    std::vector<const Entry*> pings;
    for (const Entry& e : s.entries) {
        if (!e.ping || g_pingsSeen.count(e.id)) continue;
        g_pingsSeen.insert(e.id);
        pings.push_back(&e);
        if (!first) g_pingTick[e.id] = GetTickCount();
    }
    // Not said again (still in the events): a ping within 30 m of one said in the last 2 minutes
    // (Fangorn re-creates its ping at one point every ~15 s with a new id, 41 times in 20 min), and
    // a ping within 15 m of the selected unit (Ithilien pings each ranger team as the player's
    // units reach it: "4 метра, запад" told nothing).
    struct SaidPing { float x, y; DWORD tick; };
    static std::vector<SaidPing> said;
    DWORD tickNow = GetTickCount();
    said.erase(std::remove_if(said.begin(), said.end(), [&](const SaidPing& p) { return tickNow - p.tick > 120000; }), said.end());
    if (!first && !pings.empty()) {
        std::vector<uint32_t> selNow = Selection();
        float sx = 0, sy = 0, sz = 0;
        bool haveSel = !selNow.empty();
        if (haveSel) ReadPosition(selNow[0], sx, sy, sz);
        std::vector<const Entry*> keep;
        for (const Entry* p : pings) {
            bool repeat = std::any_of(said.begin(), said.end(), [&](const SaidPing& q) {
                return std::hypot(p->x - q.x, p->y - q.y) < 30 * kUnitsPerMetre;
            });
            bool atUnit = haveSel && std::hypot(p->x - sx, p->y - sy) < 15 * kUnitsPerMetre;
            if (repeat || atUnit) {
                wchar_t b[120];
                swprintf_s(b, L"pings: at (%.0f, %.0f) not said, %s", p->x, p->y, repeat ? L"repeat of a ping said" : L"at the selected unit");
                LogLine(b);
                continue;
            }
            keep.push_back(p);
            said.push_back({p->x, p->y, tickNow});
        }
        pings.swap(keep);
    }
    if (!first && !pings.empty()) {
        std::wstring t = L"Игра отметила на карте точек: " + std::to_wstring(pings.size()) + L". Они в событиях.";
        if (pings.size() == 1) {
            t = L"Игра отметила точку на карте";
            std::vector<uint32_t> sel = Selection();
            if (!sel.empty()) {
                float x, y, z;
                ReadPosition(sel[0], x, y, z);
                float dx = pings[0]->x - x, dy = pings[0]->y - y;
                t += L", " + Metres(std::hypot(dx, dy));
                if (std::hypot(dx, dy) >= kUnitsPerMetre) t += L", " + Direction(dx, dy);
            }
            t += L". Она в событиях.";
        }
        Speak(t, false);
        wchar_t b[120];
        swprintf_s(b, L"pings: %zu new, first at (%.0f, %.0f)", pings.size(), pings[0]->x, pings[0]->y);
        LogLine(b);
    }
    if (first || fresh.empty()) return;
    std::wstring t = L"Новых меток игры: " + std::to_wstring(fresh.size()) + L".";
    if (fresh.size() == 1) {
        // From the unit in hand: the scanner's reference moves only when the player browses.
        t = L"Новая " + fresh[0]->name;
        std::vector<uint32_t> sel = Selection();
        if (!sel.empty()) {
            float x, y, z;
            ReadPosition(sel[0], x, y, z);
            float dx = fresh[0]->x - x, dy = fresh[0]->y - y;
            t += L", " + Metres(std::hypot(dx, dy));
            if (std::hypot(dx, dy) >= kUnitsPerMetre) t += L", " + Direction(dx, dy);
        }
        t += L". Ctrl+Shift+End в целях, чтобы идти туда.";
    }
    Speak(t, false);
    LogLine(L"markers: " + t);
}

std::map<std::string, int> g_pickupsLeft;           // template -> left on the map, open objectives
std::map<std::string, std::wstring> g_pickupNames;  // template -> what the list calls it
std::set<std::string> g_pickupAttack;               // templates freed by a force attack

// A pick-up gives no word of its own: the game shows a sparkle and a counter on screen. Said for
// the pick-ups of open objectives, as the object leaves the list. Called twice a second.
void WatchPickups(const std::vector<Objective>& obj) {
    std::set<std::string> open;
    for (const Objective& o : obj) {
        if (!o.shown || o.done) continue;
        auto g = g_guides.find(UpperA(o.tag));
        if (g == g_guides.end()) continue;
        for (const GuideObject& go : g->second.objects) {
            const ObjectInfo* info = ObjectsDbFind(go.tmpl);
            if ((info && info->Has("CRATE")) || go.attack || go.words.rfind(L"предмет", 0) == 0) open.insert(go.tmpl);
        }
    }
    if (open.empty()) { g_pickupsLeft.clear(); return; }
    std::map<std::string, int> now;
    Scan s = RunScan();
    int freeLeft = 0;  // cocoons are one template each: "Осталось" counts them all
    // Names of this pass go in after the words: an ordered set lists the next one by now, and "Готово"
    // said its number ("Готово: могильник 2 из 5" for the first, log 2026-11-08 02:13:49).
    std::map<std::string, std::wstring> names;
    for (const Entry& e : s.entries) {
        if ((!e.pickup && !e.attack) || e.group.empty()) continue;
        std::string t = UpperA(e.tmpl);
        ++now[t];
        if (!e.hidden || !names.count(t)) names[t] = e.name;  // the listed one of an ordered set
        freeLeft += e.attack;
        if (e.attack) g_pickupAttack.insert(t);
    }
    for (const auto& old : g_pickupsLeft) {
        int left = now.count(old.first) ? now[old.first] : 0;
        if (!open.count(old.first) || left >= old.second) continue;
        std::wstring name = g_pickupNames.count(old.first) ? g_pickupNames[old.first] : names[old.first];
        if (name.rfind(L"предмет: ", 0) == 0) name.erase(0, 9);
        bool freed = g_pickupAttack.count(old.first) != 0;
        std::wstring t = (freed ? L"Готово: " : L"Подобрано: ") + name + L".";
        if (freed ? freeLeft > 0 : left > 0) t += L" Осталось: " + std::to_wstring(freed ? freeLeft : left) + L".";
        Speak(t, false);
        LogLine(L"pickups: " + Widen(old.first.c_str()) + L" " + std::to_wstring(old.second) + L" -> " +
                std::to_wstring(left) + L": " + t);
    }
    g_pickupsLeft = now;
    for (auto& n : names) g_pickupNames[n.first] = n.second;
}

void WatchObjectives() {
    std::vector<Objective> obj = ReadObjectives();
    bool first = !g_objBaseline;
    g_objBaseline = true;
    // The first objective of a mission is shown during the intro, before the first read here:
    // the game's own "new objective" box says it was (Party, Shadow of the Past, Last Alliance).
    std::set<std::string> boxes;
    for (const std::string& tag : HintsTakeObjectiveBoxes()) boxes.insert(UpperA(tag));
    for (const Objective& o : obj) {
        auto old = g_objState.find(o.index);
        bool wasShown = old != g_objState.end() && old->second.first;
        bool wasDone = old != g_objState.end() && old->second.second;
        g_objState[o.index] = {o.shown, o.done};
        bool boxed = boxes.count(UpperA(o.tag)) != 0;
        if (first && !boxed) continue;
        std::wstring t;
        if (o.shown && !o.done && (!wasShown || first) && g_objSaid.insert(UpperA(o.tag)).second) {
            t = L"Новое задание: " + ObjectiveText(o.tag);
            g_targetKey.clear();  // "цели" opens on what is new, not on the shelf of the last story
            auto g = g_guides.find(UpperA(o.tag));
            if (g != g_guides.end() && !g->second.say.empty()) t += L" " + g->second.say;
            if (g != g_guides.end() && !(g->second.places.empty() && g->second.objects.empty())) {
                Scan s = RunScan();
                int places = 0, items = 0;
                const Entry* first = nullptr;
                for (const Entry& e : s.entries) {
                    if (e.hidden || e.category != kTargets || e.group != g->first) continue;
                    places += !e.addr;
                    items += e.pickup || e.attack;
                    if (e.gateStep && !first) first = &e;
                }
                if (places) t += L" Мест в целях: " + std::to_wstring(places) + L".";
                if (items) t += L" Предметов в целях: " + std::to_wstring(items) + L".";
                // The way is shut at the start: say what opens it before the player walks into a wall.
                if (first) t += L" Путь к цели пока закрыт, " + first->name + L".";
            }
        } else if (o.done && !wasDone) {
            t = L"Задание выполнено: " + ObjectiveText(o.tag);
        }
        if (t.empty()) continue;
        Speak(t, false);
        LogLine(L"objectives: [" + std::to_wstring(o.index) + L"] " + Widen(o.tag.c_str()) + L" -> " + t);
    }
    WatchPickups(obj);
}

}  // namespace

void ScannerInit(HWND gameWindow, const std::wstring& modDir) {
    g_wnd = gameWindow;
    LoadLabels(modDir);
    LoadGuides(modDir + L"\\guides.ini", false);
    LoadGuides(modDir + L"\\guides-auto.ini", true);
    LoadGuides(modDir + L"\\stealth.ini", false);
    RoadsLoad(modDir + L"\\roads.txt");
    LoadGates(modDir + L"\\gates.txt");
}

void ScannerTick() {
    bool battle = MenuInBattle();
    if (battle != g_wasInBattle) {
        g_wasInBattle = battle;
        // A new game (the frame counter went back) empties everything; a pause menu
        // that hides the battle HUD for a moment keeps the lists.
        uint32_t gl = U32(kTheGameLogic);
        uint32_t frame = Ptr(gl) ? U32(gl + 0x40) : 0;
        if (battle && frame < g_lastFrame) {
            g_order.clear();
            g_index = -1;
            g_category = kMine;
            for (int& k : g_kind) k = kAll;
            g_haveRef = false;
            g_refId = 0;
            g_lastSelection.clear();
            g_objectiveSecond.clear();
            g_containOffset = -1;
            g_containTried = 0;
            g_autoPre.clear();
            g_autoDone.clear();
            g_autoWaiting.clear();
            g_autoBaseline = false;
            g_objState.clear();
            g_objBaseline = false;
            g_objSaid.clear();
            g_autoTarget = 0;
            g_route.clear();
            g_routeDanger = 0;
            g_pingsSeen.clear();
            g_pingTick.clear();
            g_visited.clear();
            g_scriptTableRead = false;
            g_targetKey.clear();
            HintsForget();
        }
        g_lastFrame = frame;
        g_pickupsLeft.clear();  // a loaded game has its own: counted afresh
        g_markerBaseline = false;
        g_templates.clear();  // a map can define its own templates: names are read again
        g_pendingPlace = 0;
        g_templateVtable = 0;
        if (battle) {
            Scan s = RunScan();
            uint32_t local = s.local;
            int events = 0, objectives = 0;
            for (const Entry& e : s.entries) events += e.category == kEvents, objectives += e.category == kObjectives;
            wchar_t b[600];
            swprintf_s(b, L"scanner: battle; local player %08X \"%s\" faction %S; walked %d, kept %zu, not in ini %d, "
                          L"not selectable %d, soldiers %d (field obj+0x%X), civilians %d, no owner %d, ids unique %d, "
                          L"radar events %d, objectives %d, dead %d, rubble %d, beyond border (%.0f x %.0f) %d, %.2f ms",
                       local, local ? ReadUnicodeString(U32(local + kPlayerName)).c_str() : L"",
                       local ? game::ReadAsciiString(U32(local + kPlayerFaction)).c_str() : "", s.walked,
                       s.entries.size(), s.notInIni, s.notSelectable, s.soldiers, g_containOffset, s.civilians,
                       s.badOwner, (int)s.idsUnique, events, objectives, s.dead, s.rubble, s.maxX, s.maxY, s.outside, s.ms);
            LogLine(b);
            swprintf_s(b, L"shroud: read %d, table %d x %d, player slot %d, own units %d of %d in the clear, filter %s, hidden %d",
                       (int)s.shroudRead, s.shroudW, s.shroudH, s.shroudPlayer, s.ownClear, s.ownAll, s.shroudOn ? L"on" : L"off", s.shrouded);
            LogLine(b);
            LogFilterDiagnostics();
            LogObjectives(s);
        }
    }
    if (!battle) return;
    if (uint32_t gl = U32(kTheGameLogic)) g_lastFrame = U32(gl + 0x40);
    WatchPlacement();
    RouteTick();
    static int tick = 0;
    if (tick % 5 == 0) WatchObjectives();
    if (tick % 2 == 0) StealthTick();
    if (++tick % 5 == 0 && !g_labels.empty()) {
        bool any = false;
        for (const auto& l : g_labels) any |= l.second.autoWalk;
        Scan s = RunScan();
        WatchMarkers(s);
        if (any) AutoWalk(s);
    }
    // Selection changes are spoken: the player selects with the game's own keys.
    std::vector<uint32_t> sel = Selection();
    if (sel == g_lastSelection) return;
    g_lastSelection = sel;
    std::wstring t = sel.empty() ? L"Выделение снято" : L"Выделено: " + SelectionWords(sel);
    Speak(t, true);
    LogLine(L"scanner: selection " + std::to_wstring(sel.size()) + L" -> " + t);
}

std::string ScannerLocalFaction() {
    uint32_t local = LocalPlayer();
    return local ? game::ReadAsciiString(U32(local + kPlayerFaction)) : std::string();
}

// Test aids for the long campaign test (2026-11-01): the player tests for days while fixes
// wait, so a stuck mission must not stop the test, and each problem spot must be findable in
// the log. Ctrl+Shift+F11: a numbered mark with the mission's state. Ctrl+Shift+F10 twice
// within 3 s: the mission ends as won (HintsRequestVictory), the campaign goes on.
void ScannerTestKeys(bool gameFocused) {
    static bool down10 = false, down11 = false;
    static DWORD armed = 0;
    static int marks = 0;
    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    bool k10 = gameFocused && ctrl && shift && (GetAsyncKeyState(VK_F10) & 0x8000);
    bool k11 = gameFocused && ctrl && shift && (GetAsyncKeyState(VK_F11) & 0x8000);
    bool p10 = k10 && !down10, p11 = k11 && !down11;
    down10 = k10, down11 = k11;
    if (p11) {
        ++marks;
        std::vector<std::wstring> lines;
        wchar_t b[200];
        uint32_t gl = U32(kTheGameLogic);
        swprintf_s(b, L"MARK %d by the player: battle %d, frame %u, mission tags:", marks, (int)MenuInBattle(),
                   Ptr(gl) ? U32(gl + 0x40) : 0);
        std::wstring head = b;
        for (const Objective& o : ReadObjectives()) {
            lines.push_back(L"  objective [" + std::to_wstring(o.index) + L"] " + Widen(o.tag.c_str()) +
                            (o.shown ? L" shown" : L"") + (o.done ? L" done" : L"") + L": " + ObjectiveText(o.tag));
        }
        for (uint32_t o : Selection()) {
            float x, y, z;
            ReadPosition(o, x, y, z);
            swprintf_s(b, L"  selected %s (%S) at (%.0f, %.0f, %.0f)", NameOf(o).c_str(),
                       TemplateOf(U32(o + kObjTemplate)).name.c_str(), x, y, z);
            lines.push_back(b);
        }
        float cx = 0, cy = 0;
        CameraPosition(cx, cy);
        swprintf_s(b, L"  camera (%.0f, %.0f), money %d", cx, cy, LocalPlayer() ? (int)U32(LocalPlayer() + kPlayerMoney) : -1);
        lines.push_back(b);
        for (const std::wstring& h : HintsRecent(8)) lines.push_back(L"  recent: " + h);
        lines.insert(lines.begin(), head + L" " + std::to_wstring(lines.size()) + L" lines");
        LogLines(lines);
        Speak(L"Метка " + std::to_wstring(marks) + L" записана", true);
    }
    if (p10) {
        if (!MenuInBattle()) { Speak(L"Пропуск миссии работает только в бою", true); return; }
        if (armed && GetTickCount() - armed < 3000) {
            armed = 0;
            HintsRequestVictory();
            Speak(L"Пропускаю миссию", true);
        } else {
            armed = GetTickCount();
            Speak(L"Пропустить миссию? Нажмите ещё раз в течение 3 секунд", true);
        }
    }
}

std::vector<uint32_t> ScannerSelection() { return Selection(); }

std::vector<std::string> ScannerObjectiveTags() { return ObjectiveTags(); }

std::wstring ScannerNameOf(uint32_t obj) { return NameOf(obj); }

std::string ScannerTemplateName(uint32_t obj) {
    uint32_t t = IsObject(obj) ? U32(obj + kObjTemplate) : 0;
    return Ptr(t) ? TemplateOf(t).name : std::string();
}

// Template +0x70: the command set name (dumps of 2026-11-01 01:00-01:09).
std::string ScannerCommandSetName(uint32_t obj) {
    uint32_t t = IsObject(obj) ? U32(obj + kObjTemplate) : 0;
    return Ptr(t) ? game::ReadAsciiString(U32(t + 0x70)) : std::string();
}

uint32_t ScannerOwnObjectNamed(const char* part) {
    uint32_t local = LocalPlayer(), gl = U32(kTheGameLogic);
    std::string want = UpperA(part);
    int steps = 0;
    for (uint32_t p = Ptr(gl) ? U32(gl + kListHead) : 0; IsObject(p) && steps < 20000; p = U32(p + kObjNext), ++steps) {
        uint32_t t = U32(p + kObjTemplate);
        if (!Ptr(t) || OwnerOf(p) != local) continue;
        if (UpperA(TemplateOf(t).name).find(want) != std::string::npos) return p;
    }
    return 0;
}

bool ScannerPosition(uint32_t obj, float& x, float& y, float& z) {
    if (!IsObject(obj)) return false;
    ReadPosition(obj, x, y, z);
    return true;
}

// Evil sides: playertemplate.ini "Evil = Yes" (Mordor, Isengard, Wild, Guldur, Angmar) and
// the campaign sides of the same camps found in the object INI (Harad, Morgul, Rhun, Rhudaur).
bool EvilSide(const std::string& side) {
    static const char* kEvil[] = {"MORDOR", "ISENGARD", "WILD", "GULDUR", "ANGMAR", "HARAD", "MORGUL", "RHUN", "RHUDAUR"};
    std::string s = UpperA(side);
    for (const char* e : kEvil)
        if (s == e) return true;
    return false;
}

std::vector<ScanTarget> ScannerTargets(int kinds, uint32_t from) {
    std::vector<ScanTarget> out;
    float fx = 0, fy = 0, fz = 0;
    if (!ScannerPosition(from, fx, fy, fz)) CameraPosition(fx, fy);
    bool localEvil = EvilSide(ScannerLocalFaction());
    Scan s = RunScan();
    for (const Entry& e : s.entries) {
        if (!e.addr || e.addr == from) continue;
        int kind = 0;
        if (e.category == kMine || e.category == kAllies) kind = kTargetAlly;
        else if (e.category == kNeutral) kind = e.kind == kBuildings ? kTargetNeutral : kTargetNeutral | kTargetEnemy;  // wild creeps fight
        else if (e.category == kOthers) kind = EvilSide(e.side) != localEvil ? kTargetEnemy : kTargetAlly;
        if (!(kind & kinds)) continue;
        ScanTarget t;
        t.addr = e.addr;
        t.x = e.x, t.y = e.y, t.z = e.z;
        t.name = e.name + (e.soldiers ? L", в отряде " + std::to_wstring(e.soldiers) : L"");
        t.metres = std::hypot(e.x - fx, e.y - fy) / kUnitsPerMetre;
        t.direction = Direction(e.x - fx, e.y - fy);
        t.building = e.kind == kBuildings;
        t.hidden = e.hidden;
        t.mine = e.category == kMine;
        out.push_back(t);
    }
    std::sort(out.begin(), out.end(), [](const ScanTarget& a, const ScanTarget& b) { return a.metres < b.metres; });
    return out;
}

uint32_t ScannerLocalPlayer() { return LocalPlayer(); }
uintptr_t ScannerPlayerList() { return kThePlayerList; }

void ScannerForEachOwnObject(const std::function<void(uint32_t)>& fn) {
    uint32_t local = LocalPlayer(), gl = U32(kTheGameLogic);
    if (!local || !Ptr(gl)) return;
    int n = 0;
    for (uint32_t p = U32(gl + kListHead); IsObject(p) && n < 20000; p = U32(p + kObjNext), ++n)
        if (OwnerOf(p) == local) fn(p);
}

void ScannerForEachObject(const std::function<void(uint32_t)>& fn) {
    uint32_t gl = U32(kTheGameLogic);
    if (!Ptr(gl)) return;
    int n = 0;
    for (uint32_t p = U32(gl + kListHead); IsObject(p) && n < 20000; p = U32(p + kObjNext), ++n) fn(p);
}

// What the player can read in "чужие", buildings left out: a mission's base can be its story.
std::vector<uint32_t> ScannerVisibleEnemies() {
    Scan s = RunScan();
    std::vector<uint32_t> out;
    std::set<uint32_t> hordes;
    for (const Entry& e : s.entries) {
        if (e.category != kOthers || !e.addr || e.hidden || e.kind == kBuildings) continue;
        out.push_back(e.addr);
        if (e.soldiers > 0) hordes.insert(e.addr);
    }
    if (g_containOffset >= 0 && !hordes.empty())
        ScannerForEachObject([&](uint32_t p) {
            if (hordes.count(U32(p + g_containOffset))) out.push_back(p);
        });
    return out;
}

std::vector<TacticalUnit> ScannerTactical() {
    Scan s = RunScan();
    std::vector<TacticalUnit> out;
    ShroudMap shroud;
    bool shroudRead = ReadShroud(s.local, shroud);
    bool localEvil = EvilSide(ScannerLocalFaction());
    for (const Entry& e : s.entries) {
        if (!e.addr || e.hidden) continue;
        TacticalUnit u;
        u.addr = e.addr;
        u.x = e.x, u.y = e.y;
        u.tmpl = e.tmpl;
        u.name = e.name;
        u.building = e.kind == kBuildings;
        if (e.category == kMine) {
            out.push_back(u);
            continue;
        }
        bool foe = (e.category == kOthers && EvilSide(e.side) != localEvil) || e.category == kNeutral;
        if (!foe || u.building) continue;
        u.enemy = true;
        u.seen = !shroudRead || ShroudAt(shroud, e.x, e.y) == 0;
        out.push_back(u);
    }
    return out;
}

std::wstring ScannerDirection(float dx, float dy) { return Direction(dx, dy); }
std::wstring ScannerMetres(float units) { return Metres(units); }
bool ScannerObjectDead(uint32_t obj) { return Dead(obj); }

std::string ScannerPlayerId(uint32_t player) { return player ? game::ReadAsciiString(U32(player + kPlayerId)) : std::string(); }

uint32_t ScannerTeamOfSide(const char* side) {
    uint32_t team = 0;
    ScannerForEachObject([&](uint32_t obj) {
        if (!team && ScannerPlayerId(OwnerOf(obj)) == side) team = U32(obj + kObjTeam);
    });
    return team;
}

std::vector<ScanTarget> ScannerFires(uint32_t from) {
    std::vector<ScanTarget> out;
    float fx = 0, fy = 0, fz = 0;
    bool here = ScannerPosition(from, fx, fy, fz);
    if (!here) CameraPosition(fx, fy);
    Scan s = RunScan();
    auto add = [&](const Entry& e) {
        ScanTarget t;
        t.addr = e.addr;
        t.x = e.x, t.y = e.y, t.z = e.z;
        t.name = e.name;
        t.metres = std::hypot(e.x - fx, e.y - fy) / kUnitsPerMetre;
        t.direction = Direction(e.x - fx, e.y - fy);
        out.push_back(t);
    };
    // Story fires are invisible proxy objects (CINE_ParticleSysProxy 1-3, ParticleSysProxy 1-3) that
    // the extinguisher's DamageNugget kills: weapon.ini PorterFireExtinguisher, radius 100
    // (gamedata.ini PORTER_FIRE_EXTINGUISHER_DECREASEBURNRATE_RADIUS), also for Maggot's cart, whose
    // module names that weapon. A guide's "потушить" place stands for a field of them: it is aimed
    // where most of its live fires fall within 90 (a margin under 100), at their own height (a place
    // has z 0, under the ground). Grey Company's field 3 needs two shots (11 fires over 229 units).
    struct Fire { float x, y, z; bool used; };
    std::vector<Fire> fires;
    ScannerForEachObject([&](uint32_t p) {
        if (UpperA(ScannerTemplateName(p)).find("PARTICLESYSPROXY") == std::string::npos) return;
        Fire f{0, 0, 0, false};
        if (ScannerPosition(p, f.x, f.y, f.z)) fires.push_back(f);
    });
    constexpr float kFieldReach = 170.f, kShotRadius = 90.f;
    int fieldsLeft = 0, fieldsDone = 0;
    for (const Entry& e : s.entries) {
        if (e.category != kTargets) continue;
        bool field = e.name.rfind(L"потушить", 0) == 0;
        if (!field && e.name.rfind(L"огонь", 0) != 0) continue;
        if (!field || e.addr || fires.empty()) {
            add(e);
            continue;
        }
        std::vector<size_t> inField;
        for (size_t i = 0; i < fires.size(); ++i)
            if (std::hypot(fires[i].x - e.x, fires[i].y - e.y) <= kFieldReach) inField.push_back(i);
        if (inField.empty()) { ++fieldsDone; continue; }  // its fires are out
        ++fieldsLeft;
        float bx = 0, by = 0;
        std::vector<size_t> best;
        auto tryAt = [&](float cx, float cy) {
            std::vector<size_t> k;
            for (size_t i : inField)
                if (std::hypot(fires[i].x - cx, fires[i].y - cy) <= kShotRadius) k.push_back(i);
            if (k.size() > best.size()) best = k, bx = cx, by = cy;
        };
        for (size_t a = 0; a < inField.size(); ++a) {
            tryAt(fires[inField[a]].x, fires[inField[a]].y);
            for (size_t b = a + 1; b < inField.size(); ++b)
                tryAt((fires[inField[a]].x + fires[inField[b]].x) / 2, (fires[inField[a]].y + fires[inField[b]].y) / 2);
        }
        float bz = 0;
        for (size_t i : best) bz += fires[i].z;
        bz /= (float)best.size();
        Entry aimed = e;
        aimed.x = bx, aimed.y = by, aimed.z = bz;
        aimed.name = e.name + L", огней " + std::to_wstring(inField.size());
        if (best.size() < inField.size()) aimed.name += L", за одну струю " + std::to_wstring(best.size());
        add(aimed);
    }
    if (!fires.empty() || fieldsDone || fieldsLeft) {
        wchar_t b[140];
        swprintf_s(b, L"commands: fires: %zu proxies alive, fields burning %d, out %d", fires.size(), fieldsLeft, fieldsDone);
        LogLine(b);
    }
    // No proxy read: a place keeps its point, at the builder's height rather than under the ground.
    for (ScanTarget& t : out)
        if (!t.addr && t.z == 0 && here) t.z = fz;
    bool objective = !out.empty() || fieldsDone > 0;
    if (!objective)
        for (const Entry& e : s.entries)
            if ((e.category == kMine || e.category == kAllies) && e.kind == kBuildings) add(e);
    std::sort(out.begin(), out.end(), [](const ScanTarget& a, const ScanTarget& b) { return a.metres < b.metres; });
    // Only an objective's fires are known; otherwise it may burn right by the builder.
    if (here && !objective) {
        ScanTarget t;
        t.x = fx, t.y = fy, t.z = fz;
        t.name = L"рядом со строителем";
        t.direction = L"здесь";
        out.insert(out.begin(), t);
    }
    return out;
}

bool ScannerCurrentPlace(float& x, float& y, float& z, std::wstring& name) {
    bool resorted = false;
    Scan s = Refresh(false, resorted);
    const Entry* e = CurrentEntry(s);
    if (!e || !e->placed) return false;
    x = e->x, y = e->y, z = e->z, name = e->name;
    if (e->addr) ReadPosition(e->addr, x, y, z);
    return true;
}

bool ScannerClickAt(float x, float y, float z, bool right) {
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CameraLookAt(x, y, z, done);
    bool ran = done && WaitForSingleObject(done, 1000) == WAIT_OBJECT_0;
    if (done) CloseHandle(done);
    if (!ran) return false;
    Sleep(100);  // a few frames, so the game picks under the cursor with the new camera
    ClickWindowCentre(right);
    return true;
}

void ScannerClickKeys(bool gameFocused) {
    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    bool left = Pressed(VK_RETURN), right = Pressed(VK_BACK);
    if (!MenuInBattle() || MenuOverlay()) return;
    if (!gameFocused || ctrl || (!left && !right)) return;
    // Backspace is the player's own order to the selected units: a walk under way ends, else its
    // stall check reads the new order as a stall and sends the unit on (7 "STALLED" legs in
    // Ithilien, log 2026-11-09 00:10-00:46, each 7-25 s after a Backspace right click).
    if (right && !g_route.empty()) {
        RouteStop(L"the player's right click (Backspace)");
        Speak(L"Маршрут отменён, ваш приказ", true);
    }
    POINT pt;
    CursorToWindowCentre(pt);  // a building waiting for placement follows the cursor
    Sleep(120);
    ClickWindowCentre(right);
    // The game keeps the building on the cursor when it takes no site: nothing else tells so
    // (log 2026-11-05 12:46:46, 12:48:13: two clicks at one spot failed, the third stood).
    if (!right && PendingPlace()) {
        Sleep(350);
        if (PendingPlace()) Speak(L"Не поставлено. Нажмите ещё раз или выберите другое место.", true);
    }
}

void CameraKeys(bool gameFocused) {
    bool closer = Pressed(VK_OEM_PLUS), farther = Pressed(VK_OEM_MINUS);
    if (!closer && !farther) return;
    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    if (!gameFocused || ctrl || MenuOverlay() || (!MenuInBattle() && !WotrActive())) return;
    auto h = std::make_shared<CameraHeight>();
    CameraToHeightLimit(closer, h);
    if (!h->ran) { Speak(L"Камера не отвечает", true); return; }
    float target = closer ? h->lowest : h->highest;
    if (std::fabs(h->after - target) > 0.5f) Speak(L"Камера сейчас занята", true);
    else if (std::fabs(h->before - target) <= 0.5f) Speak(closer ? L"Камера уже близко" : L"Камера уже далеко", true);
    else Speak(closer ? L"Камера близко" : L"Камера далеко", true);
}

// Ctrl+Shift+F7, twice within 5 s: the selected units stand by the current object of the list. The
// way out of a story the mission cannot leave (Bilbo shut in Mirkwood's arena, 2026-11-06). All of the
// selection goes, in a ring round the point: only the first went, and the hobbits stayed behind the
// walls of the Dead Marshes (player, 2026-11-08 01:31-01:32).
void RescueUnit() {
    static DWORD armed = 0;
    bool resorted = false;
    Scan s = Refresh(false, resorted);
    const Entry* e = CurrentEntry(s);
    std::vector<uint32_t> sel = Selection();
    if (sel.empty()) { Speak(L"Выделите героя", true); return; }
    if (!e || !e->addr) { Speak(L"Выберите в сканере объект: к нему перенесётся герой", true); return; }
    std::wstring who = sel.size() == 1 ? NameOf(sel[0]) : L"выделенных: " + std::to_wstring(sel.size());
    DWORD now = GetTickCount();
    if (!armed || now - armed > 5000) {
        armed = now;
        Speak(L"Перенести " + who + L" к: " + e->name + L". Нажмите ещё раз, чтобы подтвердить.", true);
        return;
    }
    armed = 0;
    float cx, cy, cz;
    ReadPosition(e->addr, cx, cy, cz);
    int moved = 0;
    for (size_t i = 0; i < sel.size() && i < 40; ++i) {
        // beside it, not inside it; each unit a place of its own
        double a = 6.2831853 * (double)i / (double)sel.size();
        float x = cx + 35.f * (float)std::cos(a), y = cy + 35.f * (float)std::sin(a), z = cz;
        HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        TeleportObject(sel[i], x, y, z, done);
        if (done) { WaitForSingleObject(done, 1000); CloseHandle(done); }
        float ax, ay, az;
        ReadPosition(sel[i], ax, ay, az);
        wchar_t b[160];
        swprintf_s(b, L"rescue: id %u to (%.0f, %.0f, %.0f), now at (%.0f, %.0f, %.0f)", U32(sel[i] + kObjId), x, y, z, ax, ay, az);
        LogLine(b);
        moved += std::hypot(ax - x, ay - y) < 50.f;
    }
    if (moved == (int)sel.size()) Speak(sel.size() == 1 ? L"Перенесён" : L"Перенесены все: " + std::to_wstring(moved), true);
    else if (moved) Speak(L"Перенесены " + std::to_wstring(moved) + L" из " + std::to_wstring(sel.size()), true);
    else Speak(L"Не получилось", true);
}

void ScannerKeys(bool gameFocused) {
    static const int keys[] = {VK_NEXT, VK_PRIOR, VK_HOME, VK_END, VK_OEM_PERIOD, VK_F7};
    if (!gameFocused || !MenuInBattle() || CommandsActive()) {
        for (int vk : keys) g_keyDown[vk] = false;
        return;
    }
    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (Pressed(VK_F7) && ctrl && shift) { RescueUnit(); return; }
    if (Pressed(VK_OEM_PERIOD)) { SelectCurrent(); return; }
    bool next = Pressed(VK_NEXT), prev = Pressed(VK_PRIOR), home = Pressed(VK_HOME), end = Pressed(VK_END);
    bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    if (end && alt && !ctrl && !shift) { AttackMoveCurrent(); return; }
    if (end && ctrl && shift) { RouteStart(); return; }
    if (home && ctrl && shift) { ThreatReport(); return; }
    if ((home || end) && ctrl) { CameraOrOrder(end); return; }
    if (home && shift) { DescribeCurrent(); return; }
    if (end) {
        // While a building waits for placement, End says what it is for.
        if (uint32_t t = PendingPlace(); t && !shift) {
            std::wstring w = L"Постройка: " + BuildingWords(TemplateOf(t), true);
            Speak(w, true);
            LogLine(L"placement: describe -> " + w);
        } else {
            Status(shift);
        }
        return;
    }
    if (!next && !prev && !home) return;
    uint32_t oldRef = g_refId;
    bool resorted = false;
    if ((ctrl || shift) && (next || prev)) {
        int step = next ? 1 : -1;
        if (ctrl) {
            g_category = (g_category + step + kCategories) % kCategories;
        } else if (g_category == kTargets) {
            Scan s0 = RunScan();
            std::vector<std::string> groups = TargetGroups(s0);
            int at = (int)(std::find(groups.begin(), groups.end(), g_targetKey) - groups.begin());
            int n = (int)groups.size();
            PickTargetGroup(groups, groups[((at == n ? 0 : at) + step + n) % n]);
        } else if (HasKinds(g_category)) {
            g_kind[g_category] = (g_kind[g_category] + step + kKinds) % kKinds;
        } else {
            Speak(std::wstring(kCategoryNames[g_category]) + L": без подкатегорий", true);
            return;
        }
        Scan s = Refresh(true, resorted);
        g_index = 0;
        std::wstring prefix = ReferencePrefix(oldRef);
        if (!CurrentOrder().empty()) prefix += PlaceName() + L", " + std::to_wstring(CurrentOrder().size()) + L". ";
        SpeakCurrent(s, prefix);
        return;
    }
    Scan s = Refresh(false, resorted);
    int n = (int)CurrentOrder().size();
    if (n > 0) {
        if (next) g_index = (g_index + 1) % n;
        else if (prev) g_index = g_index <= 0 ? n - 1 : g_index - 1;
        else if (g_index < 0) g_index = 0;
    }
    SpeakCurrent(s, ReferencePrefix(oldRef));
}

void ScannerDump(FILE* f) {
    Scan s = RunScan();
    // The dump measures from the current reference without changing the scanner's own.
    uint32_t savedId = g_refId, savedAddr = g_refAddr;
    float rx = g_refX, ry = g_refY;
    bool saved = g_haveRef;
    std::wstring savedName = g_refName;
    UpdateReference(s);
    fprintf(f, "\n\nSCANNER: walked %d, kept %zu, not in ini %d, not selectable %d, soldiers %d (field obj+0x%X), "
               "civilians %d, no owner %d, ids unique %d, dead %d, beyond border (%.0f x %.0f) %d, %.2f ms\n",
            s.walked, s.entries.size(), s.notInIni, s.notSelectable, s.soldiers, g_containOffset, s.civilians,
            s.badOwner, (int)s.idsUnique, s.dead, s.maxX, s.maxY, s.outside, s.ms);
    uint32_t local = s.local;
    fprintf(f, "local player %08X faction %s money %u (vt %08X)\n", local,
            local ? game::ReadAsciiString(U32(local + kPlayerFaction)).c_str() : "", local ? U32(local + kPlayerMoney) : 0,
            local ? U32(local + kPlayerMoneyVt) : 0);
    std::vector<uint32_t> sel = Selection();
    uint32_t ui = U32(kTheInGameUI);
    fprintf(f, "selection: %zu objects, ui+568 = %u:", sel.size(), Ptr(ui) ? U32(ui + kUiSelectedCount) : 0);
    for (uint32_t o : sel) fprintf(f, " %s#%u", Utf8(NameOf(o)).c_str(), U32(o + kObjId));
    uint32_t pending = PendingPlace();
    fprintf(f, "\npending placement (ui+53C): %s", pending ? TemplateOf(pending).name.c_str() : "none");
    float cx, cy;
    CameraPosition(cx, cy);
    fprintf(f, "\ncamera look-at point (view+0C/+10): (%.0f, %.0f)", cx, cy);
    fprintf(f, "\nreference id %u at (%.0f, %.0f)\n", g_refId, g_refX, g_refY);
    uint32_t tracker = U32(kTheObjectiveTracker);
    uint32_t list = Ptr(tracker) ? U32(tracker + 0x10) : 0;
    fprintf(f, "objective list %08X:", list);
    for (int i = 0; Ptr(list) && i < 8; ++i) fprintf(f, " %08X", U32(list + i * 4));
    fprintf(f, "\n");
    for (int cat = 0; cat < kCategories; ++cat) {
        std::vector<const Entry*> in;
        for (const Entry& e : s.entries)
            if (e.category == cat) in.push_back(&e);
        std::sort(in.begin(), in.end(), [](const Entry* a, const Entry* b) { return Dist(*a) < Dist(*b); });
        fprintf(f, "\n[%s] %zu\n", Utf8(kCategoryNames[cat]).c_str(), in.size());
        for (const Entry* e : in)
            fprintf(f, "  id %08X %s \"%s\" (%s) kind %d soldiers %d side %s owner/second %08X order %u at (%.0f, %.0f, "
                       "%.0f) %.0f units\n",
                    e->id, e->tmpl.c_str(), Utf8(e->name).c_str(), e->nameSource, e->kind, e->soldiers,
                    e->side.c_str(), e->owner, e->order, e->x, e->y, e->z, Dist(*e));
    }
    g_refId = savedId, g_refAddr = savedAddr, g_refX = rx, g_refY = ry, g_haveRef = saved, g_refName = savedName;
}

bool ScannerCameraTo(float x, float y) { return CameraTo(x, y, 0); }
bool ScannerCameraToFree(float x, float y) {
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CameraLookAt(x, y, 0, done, true);
    bool ran = done && WaitForSingleObject(done, 1000) == WAIT_OBJECT_0;
    if (done) CloseHandle(done);
    float cx, cy;
    CameraPosition(cx, cy);
    return ran && std::hypot(cx - x, cy - y) < 50.f;
}
void ScannerClickCentre(bool right) { ClickWindowCentre(right); }
