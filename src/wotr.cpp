#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <map>
#include <sstream>
#include <set>
#include <string>
#include <vector>

#include "game.h"
#include "gamethread.h"
#include "log.h"
#include "memory.h"
#include "menu.h"
#include "scanner.h"
#include "screens.h"
#include "screentext.h"
#include "speech.h"
#include "strings.h"
#include "wotr.h"

namespace {

// Everything comes from the running game (dump 2026-11-04 22:27, Worldbuilder.exe, disassembly):
// TheLivingWorldLogic [0xDE4950]: +0x1C armies (vector), +0x8C players (vector), +0x98 local
// player, +0xB0 region manager, +0xF4 turn phase, +0xFC turn (both go into MSG_LW_DONE_PHASE).
// Region manager +8 = the scenario's region campaign: its name AsciiString at +0, a hash map of
// regions at +0x38 (buckets vector +0x3C..+0x40, count +0x48, node {next, key, region}).
// Region: vtable 0x00C4D43C, +0x14 id, +0x18 battle map, +0x38 display name key, +0x3C
// description key, +0x60..+0x64 connections (0x18 bytes each, neighbour id at +4), +0x80/+0x84
// centre, +0x14C index, +0x15C owner = the player's +0x14, -1 for nobody.
// Player: +0x14 id, +0x1C name, +0x24 template ("PlayerMordor").
// Army: the object starts at vtable 0x00C22EA4; a second vtable 0x00C22E9C at +4 is what a heap
// scan finds, offsets below count from there: +0x14 hero object name (empty for a garrison),
// +0x1C army id, +0x30/+0x34 position, +0x38/+0x3C goal, +0x64 army name, +0x68 hero and units
// (lines), +0x88 region.
// Region +0x190..+0x194: build plots. A plot starts 4 bytes before vtable 0x00C7A7A8: +0x18 plot
// id (Worldbuilder GetBuildPlotByIndex), +0x20 the building on it (0 empty, dump 22:27 plot 12).
// TheLivingWorldBuildingTemplateStore [0xDE4AE4]: hash map id -> template at +0x10 (buckets
// +0x14..+0x18, node {next, id, template inline}). Template: +0 name, +4 id, +0x20 name key,
// +0x24 description key, +0x28 turns, +0x2C cost (parse table 0xC1A6F8).
constexpr uintptr_t kTheLivingWorldLogic = 0xDE4950, kBuildingStore = 0xDE4AE4;
constexpr uint32_t kRegionVtable = 0x00C4D43C, kArmyVtable = 0x00C22EA4, kArmyVtable2 = 0x00C22E9C, kPlotVtable = 0x00C7A7A8;

struct Player {
    uint32_t p = 0;
    int id = -1;
    std::wstring name;  // what the player is called in speech
    std::string tmpl;   // "PlayerMordor"
    std::vector<uint32_t> armies;
};
struct Plot {
    int id = 0;
    uint32_t building = 0;
};
struct Building {
    uint32_t t = 0;
    int id = 0, cost = 0, turns = 0;
    std::string name;
    std::wstring display, about;
};
// What a building can train: LivingWorldBuildingNuggetSpawnArmy. Building +0x40 = the nugget
// (0x8E4A3A, none when building +0x20 is -1), its vt+0x1C = canQueueUnit. Its template is 4 bytes
// before it (Worldbuilder canQueueUnit, HYPOTHESIS for game.dat, checked by the shape of the data):
// +8..+0xC ArmyToSpawn entries, 0x58 bytes each, +0x14 QueueSize (parse table 0xC79FDC); the queue
// is a vector of ints at nugget +0x14. Entry (table 0xC78380): +0x10 title key, +0x14 help key,
// +0x2C PlayerArmy, +0x48 BuildTime, +0x4C the id the queue message takes (given at load, 0x905A9F).
struct Spawn {
    int id = 0, turns = 0;
    bool can = false;
    std::string army;
    std::wstring name, about;
};
struct Region {
    uint32_t r = 0;
    std::string id;
    std::wstring name, map, about;
    std::vector<std::string> links;
    std::vector<Plot> plots;
    int index = 0, owner = -1;
    float x = 0, y = 0;
};
struct Army {
    uint32_t a = 0;  // the +4 pointer
    int id = 0, owner = -1;
    bool hero = false;
    std::wstring kind, leader, units;  // first line of the summary, the other lines
    uint32_t region = 0;
    float x = 0, y = 0;
    float gx = 0, gy = 0;  // goal of the march (+0x38/+0x3C), the position when it stands
};
struct Entry {
    bool army = false;
    int index = 0;  // into g_armies or g_regions
};

const wchar_t* const kViews[] = {L"мои армии", L"мои регионы", L"граница", L"чужие регионы", L"ничьи регионы", L"все регионы",
                                 L"чужие армии рядом", L"движения чужих армий", L"битвы"};
constexpr int kNearView = 6, kMovesView = 7, kBattleView = 8;
constexpr int kViewCount = sizeof kViews / sizeof *kViews;

std::vector<Player> g_players;
std::vector<Region> g_regions;
std::vector<Army> g_armies;
std::vector<Entry> g_entries;
std::vector<Building> g_buildings;  // every building of the game
std::map<std::string, std::set<std::string>> g_availableTo;  // building -> player templates
int g_local = -1, g_view = 0, g_pos = 0;
uint32_t g_cur = 0;  // the army or region the list stands on
std::wstring g_scenario;
bool g_onMap = false, g_buttons = false, g_dialog = false, g_said = false;
int g_mapShell = 0, g_gone = 0;
int g_phase = -1, g_turn = -1;
// Turn phases +0xF4 (log 2026-11-05): 0 orders, 1 armies walk, 2 battles (Fight battle in X on the
// checklist, nothing goes on until each is fought), 3.. retreats, income.
constexpr int kPhaseOrders = 0, kPhaseBattles = 2;
std::vector<std::wstring> g_battles;  // region names
// Sending an army: the army and the regions it can go to.
uint32_t g_moving = 0;
std::vector<int> g_goals;
int g_goal = 0;
// A choice over the list: heroes for an attack, plots of a region, buildings for a plot.
enum Choice { kNoChoice, kAttackers, kPlots, kBuildings, kBattle, kUnits } g_choice = kNoChoice;
std::vector<Spawn> g_spawns;
uint32_t g_spawnBuilding = 0, g_spawnNugget = 0;
int g_queueMax = 0;
std::vector<int> g_rows;  // armies, plots or buildings
int g_row = 0, g_plot = 0;
uint32_t g_choiceRegion = 0;
DWORD g_endAsked = 0;
std::set<std::string> g_otherPaths;
bool g_keyDown[256] = {};

bool Pressed(int vk) {
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool edge = down && !g_keyDown[vk];
    g_keyDown[vk] = down;
    return edge;
}

uint32_t U32(uintptr_t a) {
    uint32_t v = 0;
    SafeReadU32(a, v);
    return v;
}

bool Ptr(uint32_t v) { return v >= 0x10000 && v < 0x7FFF0000; }

// UnicodeString data: {u16 refcount, u16 ?, u16 length, u16 alloc, UTF-16 chars}.
std::wstring Unicode(uint32_t data) {
    uint16_t len = 0;
    if (!Ptr(data) || !SafeRead(data + 4, &len, 2) || !len || len > 200) return {};
    wchar_t buf[201];
    if (!SafeRead(data + 8, buf, len * 2)) return {};
    return std::wstring(buf, len);
}

std::wstring Lookup(uint32_t keyData) {
    std::string key = game::ReadAsciiString(keyData);
    if (key.empty()) return {};
    std::wstring t = LookupStr(key);
    for (size_t p; (p = t.find(L"\\n")) != std::wstring::npos;) t.replace(p, 2, L" ");
    return t;
}

void ReadBuildings() {
    uint32_t store = U32(kBuildingStore);
    if (!Ptr(store) || !g_buildings.empty()) return;
    uint32_t b = U32(store + 0x14), e = U32(store + 0x18);
    for (uint32_t q = b; Ptr(b) && q < e; q += 4)
        for (uint32_t n = U32(q), guard = 0; Ptr(n) && guard < 200; n = U32(n), ++guard) {
            Building x;
            x.t = n + 8;
            x.id = (int)U32(n + 4);
            if ((int)U32(x.t + 4) != x.id) continue;
            x.name = game::ReadAsciiString(U32(x.t));
            x.display = Lookup(U32(x.t + 0x20));
            x.about = Lookup(U32(x.t + 0x24));
            x.turns = (int)U32(x.t + 0x28);
            x.cost = (int)U32(x.t + 0x2C);
            g_buildings.push_back(x);
        }
    LogLine(L"wotr: " + std::to_wstring(g_buildings.size()) + L" building templates, store says " + std::to_wstring(U32(store + 0x20)));
}

// The building on a plot: its template is one of its first pointers (which one is logged).
std::wstring BuildingOn(uint32_t b) {
    for (uint32_t off = 0; off < 0x80; off += 4) {
        uint32_t v = U32(b + off);
        for (const Building& x : g_buildings)
            if (v == x.t) {
                static std::set<uint32_t> logged;
                if (logged.insert(off).second) LogLine(L"wotr: building template at +" + std::to_wstring(off));
                return x.display.empty() ? Widen(x.name.c_str()) : x.display;
            }
    }
    return L"постройка";
}

// Armies a player holds: a vector somewhere in the player block whose entries are armies.
// Which field it is gets logged; Worldbuilder's AddArmy suggests +0x78 (not proven).
void ReadPlayerArmies(Player& pl) {
    for (uint32_t off = 0x20; off < 0x300; off += 4) {
        uint32_t b = U32(pl.p + off), e = U32(pl.p + off + 4);
        if (!Ptr(b) || e <= b || (e - b) % 4 || e - b > 4000) continue;
        uint32_t first = U32(b), vt = Ptr(first) ? U32(first) : 0;
        if (vt != kArmyVtable && vt != kArmyVtable2) continue;
        for (uint32_t q = b; q < e; q += 4) {
            uint32_t a = U32(q);
            pl.armies.push_back(U32(a) == kArmyVtable ? a + 4 : a);
        }
        static std::set<uint32_t> logged;
        if (logged.insert(off).second) LogLine(L"wotr: player armies at +" + std::to_wstring(off));
        return;
    }
}

uint32_t g_plotsAt = 0x190;

bool ReadPlots(Region& x, uint32_t off) {
    uint32_t b = U32(x.r + off), e = U32(x.r + off + 4);
    x.plots.clear();
    if (!Ptr(b) || e <= b || (e - b) % 4 || e - b > 256) return false;
    for (uint32_t q = b; q < e; q += 4) {
        uint32_t p = U32(q);
        if (Ptr(p) && U32(p) == kPlotVtable) p -= 4;
        if (!Ptr(p) || U32(p + 4) != kPlotVtable) continue;
        x.plots.push_back({(int)U32(p + 0x18), U32(p + 0x20)});
    }
    return !x.plots.empty();
}

std::vector<uint32_t> CampaignRegions(uint32_t camp) {
    std::vector<uint32_t> out;
    uint32_t b = U32(camp + 0x3C), e = U32(camp + 0x40);
    if (!Ptr(b) || e < b || e - b > 0x10000) return out;
    for (uint32_t q = b; q < e; q += 4)
        for (uint32_t n = U32(q), guard = 0; Ptr(n) && guard < 200; n = U32(n), ++guard) {
            uint32_t r = U32(n + 8);
            if (Ptr(r) && U32(r) == kRegionVtable) out.push_back(r);
        }
    return out;
}

// Reads the whole state: a few thousand small reads, done when a key is pressed.
bool Refresh() {
    uint32_t logic = U32(kTheLivingWorldLogic);
    if (!Ptr(logic)) return false;
    uint32_t mgr = U32(logic + 0xB0), camp = Ptr(mgr) ? U32(mgr + 8) : 0;
    if (!Ptr(camp)) return false;
    g_scenario = Widen(game::ReadAsciiString(U32(camp)).c_str());

    g_players.clear();
    g_local = -1;
    uint32_t localPlayer = U32(logic + 0x98);
    for (uint32_t q = U32(logic + 0x8C), e = U32(logic + 0x90); q < e && g_players.size() < 16; q += 4) {
        Player pl;
        pl.p = U32(q);
        if (!Ptr(pl.p)) continue;
        pl.id = (int)U32(pl.p + 0x14);
        std::string tmpl = game::ReadAsciiString(U32(pl.p + 0x24));
        pl.tmpl = tmpl;
        if (tmpl.rfind("Player", 0) == 0) tmpl = tmpl.substr(6);
        pl.name = Widen(tmpl.c_str());
        ReadPlayerArmies(pl);
        if (pl.p == localPlayer) g_local = pl.id;
        g_players.push_back(pl);
    }

    g_regions.clear();
    for (uint32_t r : CampaignRegions(camp)) {
        Region x;
        x.r = r;
        x.id = game::ReadAsciiString(U32(r + 0x14));
        x.name = Lookup(U32(r + 0x38));
        if (x.name.empty()) x.name = Widen(x.id.c_str());
        x.map = Widen(game::ReadAsciiString(U32(r + 0x18)).c_str());
        x.about = Lookup(U32(r + 0x3C));
        x.index = (int)U32(r + 0x14C);
        x.owner = (int)U32(r + 0x15C);
        SafeRead(r + 0x80, &x.x, 4);
        SafeRead(r + 0x84, &x.y, 4);
        uint32_t b = U32(r + 0x60), e = U32(r + 0x64);
        for (uint32_t c = b; Ptr(b) && c < e && x.links.size() < 40; c += 0x18)
            x.links.push_back(game::ReadAsciiString(U32(c + 4)));
        ReadPlots(x, g_plotsAt);
        g_regions.push_back(x);
    }
    // +0x190 in Worldbuilder; no plots anywhere there: the fields around it are searched once.
    bool anyPlots = false;
    for (const Region& x : g_regions) anyPlots |= !x.plots.empty();
    static bool searched = false;
    if (!anyPlots && !searched) {
        searched = true;
        for (uint32_t off = 0x100; off < 0x260 && !anyPlots; off += 4)
            for (Region& x : g_regions) anyPlots |= ReadPlots(x, off), g_plotsAt = anyPlots ? off : g_plotsAt;
        LogLine(anyPlots ? L"wotr: region plots at +" + std::to_wstring(g_plotsAt) : std::wstring(L"wotr: region plots not found"));
    }
    std::sort(g_regions.begin(), g_regions.end(), [](const Region& a, const Region& b) { return a.name < b.name; });

    g_armies.clear();
    for (uint32_t q = U32(logic + 0x1C), e = U32(logic + 0x20); q < e && g_armies.size() < 2000; q += 4) {
        uint32_t p = U32(q), vt = Ptr(p) ? U32(p) : 0;
        if (vt != kArmyVtable && vt != kArmyVtable2) continue;
        Army a;
        a.a = vt == kArmyVtable ? p + 4 : p;
        a.id = (int)U32(a.a + 0x1C);
        a.hero = !game::ReadAsciiString(U32(a.a + 0x14)).empty();
        a.kind = Unicode(U32(a.a + 0x64));
        a.leader = Unicode(U32(a.a + 0x68));
        // "Faramir\nRanger Battalions", "4 Orc Warrior Hordes\n2 Mordor Hordes": hero, then units.
        for (wchar_t& c : a.leader)
            if (c == L'\r') c = L'\n';
        size_t cut = a.leader.find(L'\n');
        if (cut != std::wstring::npos) {
            std::wstring rest = a.leader.substr(cut + 1);
            a.leader.resize(cut);
            for (size_t p; (p = rest.find(L'\n')) != std::wstring::npos;) rest.replace(p, 1, L", ");
            while (rest.size() >= 2 && rest.compare(rest.size() - 2, 2, L", ") == 0) rest.resize(rest.size() - 2);
            a.units = rest;
        }
        a.region = U32(a.a + 0x88);
        SafeRead(a.a + 0x30, &a.x, 4);
        SafeRead(a.a + 0x34, &a.y, 4);
        SafeRead(a.a + 0x38, &a.gx, 4);
        SafeRead(a.a + 0x3C, &a.gy, 4);
        for (const Player& pl : g_players)
            if (std::find(pl.armies.begin(), pl.armies.end(), a.a) != pl.armies.end()) a.owner = pl.id;
        if (a.owner < 0)
            for (const Region& r : g_regions)
                if (r.r == a.region) a.owner = r.owner;
        g_armies.push_back(a);
    }
    ReadBuildings();
    return !g_regions.empty();
}

const Region* RegionAt(uint32_t r) {
    for (const Region& x : g_regions)
        if (x.r == r) return &x;
    return nullptr;
}

const Region* RegionById(const std::string& id) {
    for (const Region& x : g_regions)
        if (x.id == id) return &x;
    return nullptr;
}

std::wstring OwnerWord(int id) {
    if (id < 0) return L"ничей";
    if (id == g_local) return L"ваш";
    for (const Player& pl : g_players)
        if (pl.id == id) return pl.name;
    return L"игрок " + std::to_wstring(id);
}

// Treasury of a player (disassembly 2026-11-15): the add-to-treasury function 0x6E1A40, which
// LivingWorldLogic calls on a player (0x6BC9E2 charges the army upkeep), keeps
// [[player +0x40] +0x20] + [player +0x318] and caps it at [[player +0x40] +0x24]; it returns
// that sum. Player +0x40 is the living-world player template (dump 2026-11-05 13:34: "PlayerMen",
// "FactionMen", "IconMenStrategic"). Same function in Worldbuilder 0xDED5A0, called by
// LivingWorldBuildingNuggetIncreaseTreasury::addTreasuryToPlayer. HYPOTHESIS until the HUD's
// number is heard to match. -1 when unreadable.
int Treasury(int playerId) {
    for (const Player& pl : g_players) {
        if (pl.id != playerId) continue;
        uint32_t tmpl = U32(pl.p + 0x40);
        if (!Ptr(tmpl)) return -1;
        int base = (int)U32(tmpl + 0x20), extra = (int)U32(pl.p + 0x318);
        long long sum = (long long)base + extra;
        return sum >= 0 && sum < 10000000 ? (int)sum : -1;
    }
    return -1;
}

std::wstring TreasuryWords() {
    int t = Treasury(g_local);
    return t < 0 ? std::wstring() : L"Казна " + std::to_wstring(t) + L". ";
}

void LogTreasury(const wchar_t* when) {
    for (const Player& pl : g_players) {
        if (pl.id != g_local) continue;
        uint32_t tmpl = U32(pl.p + 0x40);
        wchar_t b[200];
        swprintf_s(b, L"wotr: treasury %s: template %08X +20=%d +24=%d, player +318=%d -> %d", when, tmpl, Ptr(tmpl) ? (int)U32(tmpl + 0x20) : -1,
                   Ptr(tmpl) ? (int)U32(tmpl + 0x24) : -1, (int)U32(pl.p + 0x318), Treasury(g_local));
        LogLine(b);
    }
}

// Who stands in a region: heroes by name, then whether a garrison holds anything.
std::wstring Standing(const Region& r) {
    std::wstring heroes;
    bool garrison = false;
    for (const Army& a : g_armies) {
        if (a.region != r.r) continue;
        if (a.hero) heroes += (heroes.empty() ? L"" : L", ") + a.leader;
        else garrison |= !a.leader.empty();
    }
    std::wstring t;
    if (!heroes.empty()) t = L", герои: " + heroes;
    if (garrison) t += L", гарнизон";
    return t;
}

std::wstring RegionWords(const Region& r) { return r.name + L", " + OwnerWord(r.owner) + Standing(r); }

const Region* GoalRegion(const Army& a);

std::wstring ArmyWords(const Army& a) {
    const Region* r = RegionAt(a.region);
    std::wstring who = a.hero ? a.leader : L"гарнизон: " + a.leader;
    if (a.owner != g_local && a.owner >= 0) who += L" (" + OwnerWord(a.owner) + L")";
    if (!a.units.empty()) who += L", " + a.units;
    std::wstring t = who + (r ? L", в регионе " + r->name + (r->owner == g_local && a.owner != g_local ? L", это ваш регион" : L"") : L"");
    if (a.owner != g_local)
        if (const Region* g = GoalRegion(a)) t += L", идёт в " + g->name + (g->owner == g_local ? L", это ваш регион!" : L" (" + OwnerWord(g->owner) + L")");
    return t;
}

// The region whose centre is nearest a point of the map.
const Region* RegionNear(float x, float y) {
    const Region* best = nullptr;
    float bd = 0;
    for (const Region& r : g_regions) {
        float d = (r.x - x) * (r.x - x) + (r.y - y) * (r.y - y);
        if (!best || d < bd) best = &r, bd = d;
    }
    return best;
}

// Where an army marches: the goal +0x38/+0x3C takes the order's point at once (PROVEN for the
// player's armies 2026-11-04); the region nearest that point when it is not the army's own.
const Region* GoalRegion(const Army& a) {
    if (!std::isfinite(a.gx) || !std::isfinite(a.gy) || (a.gx == 0 && a.gy == 0)) return nullptr;
    float dx = a.gx - a.x, dy = a.gy - a.y;
    if (dx * dx + dy * dy < 25.f * 25.f) return nullptr;
    const Region* g = RegionNear(a.gx, a.gy);
    return g && g->r != a.region ? g : nullptr;
}

bool Foreign(const Army& a) { return a.owner >= 0 && a.owner != g_local && a.hero && !a.leader.empty(); }

// An army in one of your regions or in a region next to one.
bool NearMine(const Army& a) {
    const Region* at = RegionAt(a.region);
    if (!at) return false;
    if (at->owner == g_local) return true;
    for (const std::string& id : at->links)
        if (const Region* n = RegionById(id))
            if (n->owner == g_local) return true;
    return false;
}

uint32_t EntryKey(const Entry& e) { return e.army ? g_armies[e.index].a : g_regions[e.index].r; }

void BuildView() {
    g_entries.clear();
    if (g_view == 0) {
        for (int hero = 1; hero >= 0; --hero)
            for (size_t i = 0; i < g_armies.size(); ++i) {
                const Army& a = g_armies[i];
                if (a.owner == g_local && a.hero == (hero != 0) && !a.leader.empty()) g_entries.push_back({true, (int)i});
            }
    } else if (g_view == kNearView) {
        // Other players' hero armies standing in your regions or next to them: who may come.
        for (size_t i = 0; i < g_armies.size(); ++i) {
            const Army& a = g_armies[i];
            if (a.owner == g_local || a.owner < 0 || !a.hero || a.leader.empty()) continue;
            if (NearMine(a)) g_entries.push_back({true, (int)i});
        }
    } else if (g_view == kMovesView) {
        // Other players' heroes on the march: those coming into your regions first.
        for (int pass = 0; pass < 3; ++pass)
            for (size_t i = 0; i < g_armies.size(); ++i) {
                const Army& a = g_armies[i];
                const Region* g = Foreign(a) ? GoalRegion(a) : nullptr;
                if (!g) continue;
                int rank = g->owner == g_local ? 0 : NearMine(a) ? 1 : 2;
                if (rank == pass) g_entries.push_back({true, (int)i});
            }
    } else {
        std::set<std::string> border;
        if (g_view == 2)
            for (const Region& r : g_regions)
                if (r.owner == g_local)
                    for (const std::string& id : r.links) border.insert(id);
        for (size_t i = 0; i < g_regions.size(); ++i) {
            const Region& r = g_regions[i];
            bool mine = r.owner == g_local, nobody = r.owner < 0;
            bool in = g_view == 1 ? mine : g_view == 2 ? !mine && border.count(r.id) > 0 : g_view == 3 ? !mine && !nobody
                      : g_view == 4 ? nobody
                      : g_view == kBattleView ? std::find(g_battles.begin(), g_battles.end(), r.name) != g_battles.end() : true;
            if (in) g_entries.push_back({false, (int)i});
        }
    }
    // The refresh rebuilt the vectors: the list stays on the same army or region.
    if (g_pos >= (int)g_entries.size()) g_pos = 0;
    for (size_t i = 0; i < g_entries.size(); ++i)
        if (EntryKey(g_entries[i]) == g_cur) g_pos = (int)i;
}

void SayEntry(bool interrupt = true) {
    if (g_entries.empty()) { Speak(std::wstring(kViews[g_view]) + L": пусто", true); return; }
    const Entry& e = g_entries[g_pos];
    g_cur = EntryKey(e);
    std::wstring t = e.army ? ArmyWords(g_armies[e.index]) : RegionWords(g_regions[e.index]);
    Speak(t + L", " + std::to_wstring(g_pos + 1) + L" из " + std::to_wstring(g_entries.size()), interrupt);
}

const Region* EntryRegion() {
    if (g_entries.empty()) return nullptr;
    const Entry& e = g_entries[g_pos];
    return e.army ? RegionAt(g_armies[e.index].region) : &g_regions[e.index];
}

void SayDetails(const Region& r) {
    std::wstring t = RegionWords(r) + L".";
    if (!r.about.empty()) t += L" " + r.about + L".";
    if (!r.plots.empty()) {
        int free = 0;
        std::wstring built;
        for (const Plot& p : r.plots) {
            if (p.building) built += (built.empty() ? L"" : L", ") + BuildingOn(p.building);
            else ++free;
        }
        t += L" Участков " + std::to_wstring(r.plots.size()) + L", свободных " + std::to_wstring(free) + L"." +
             (built.empty() ? L"" : L" Постройки: " + built + L".");
    }
    t += L" Соседи:";
    for (const std::string& id : r.links) {
        const Region* n = RegionById(id);
        t += L" " + (n ? RegionWords(*n) : Widen(id.c_str())) + L";";
    }
    Speak(t, true);
}

// Your hero armies in a region and next to it: who can go there this turn.
std::vector<int> ArmiesNear(const Region& r) {
    std::vector<int> out;
    for (size_t i = 0; i < g_armies.size(); ++i) {
        const Army& a = g_armies[i];
        const Region* at = RegionAt(a.region);
        if (a.owner != g_local || !a.hero || !at || at->r == r.r) continue;
        if (std::find(at->links.begin(), at->links.end(), r.id) != at->links.end()) out.push_back((int)i);
    }
    return out;
}

const Region* ChoiceRegion() { return RegionAt(g_choiceRegion); }

const wchar_t* MethodWords(int method);

std::wstring PlotWords(const Region& r, int row) {
    const Plot& p = r.plots[g_rows[row]];
    return L"участок " + std::to_wstring(row + 1) + L": " + (p.building ? BuildingOn(p.building) : L"пусто");
}

int QueueLength(uint32_t nugget) {
    uint32_t b = U32(nugget + 0x14), e = U32(nugget + 0x18);
    return Ptr(b) && e >= b && e - b < 0x1000 ? (int)((e - b) / 4) : 0;
}

// The spawn list of a building, with the game's own answer on each entry.
bool ReadSpawns(uint32_t building) {
    g_spawns.clear();
    g_spawnBuilding = g_spawnNugget = 0;
    uint32_t nugget = U32(building + 0x40);
    if (!Ptr(nugget) || (int)U32(building + 0x20) == -1) return false;
    uint32_t tmpl = U32(nugget - 4), b = Ptr(tmpl) ? U32(tmpl + 8) : 0, e = Ptr(tmpl) ? U32(tmpl + 0xC) : 0;
    wchar_t head[140];
    swprintf_s(head, L"wotr: building %08X nugget %08X template %08X armies %08X..%08X queue size %d", building, nugget, tmpl, b, e,
               Ptr(tmpl) ? (int)U32(tmpl + 0x14) : -1);
    LogLine(head);
    if (!Ptr(b) || e <= b || (e - b) % 0x58 || e - b > 0x58 * 64) return false;
    std::vector<int> ids;
    for (uint32_t q = b; q < e; q += 0x58) {
        Spawn s;
        s.id = (int)U32(q + 0x4C);
        s.turns = (int)U32(q + 0x48);
        s.army = game::ReadAsciiString(U32(q + 0x2C));
        s.name = Lookup(U32(q + 0x10));
        if (s.name.empty()) s.name = Widen(s.army.c_str());
        s.about = Lookup(U32(q + 0x14));
        ids.push_back(s.id);
        g_spawns.push_back(s);
    }
    std::vector<char> can(ids.size(), 0);
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool ok = false;
    AskCanQueue(nugget, ids.data(), (int)ids.size(), can.data(), done, &ok);
    bool ran = done && WaitForSingleObject(done, 1000) == WAIT_OBJECT_0;
    if (done) CloseHandle(done);
    if (!ran) {  // the game thread still holds the vectors: they must outlive the job
        static std::vector<std::vector<int>> keepIds;
        static std::vector<std::vector<char>> keepCan;
        keepIds.push_back(std::move(ids));
        keepCan.push_back(std::move(can));
        g_spawns.clear();
        LogLine(L"wotr: canQueueUnit did not answer");
        return false;
    }
    for (size_t i = 0; i < g_spawns.size(); ++i) {
        g_spawns[i].can = ok && can[i];
        LogLine(L"wotr: spawn " + Widen(g_spawns[i].army.c_str()) + L" id " + std::to_wstring(g_spawns[i].id) + (g_spawns[i].can ? L" can" : L" cannot"));
    }
    g_spawnBuilding = building;
    g_spawnNugget = nugget;
    g_queueMax = (int)U32(tmpl + 0x14);
    return true;
}

void SayChoice(bool interrupt) {
    const Region* r = ChoiceRegion();
    std::wstring t;
    if (g_rows.empty() || !r) return;
    if (g_choice == kBattle) t = MethodWords(g_rows[g_row]);
    else if (g_choice == kUnits) {
        const Spawn& s = g_spawns[g_rows[g_row]];
        t = s.name + L", ходов " + std::to_wstring(s.turns) + (s.can ? L"" : L", сейчас нельзя");
    }
    else if (g_choice == kAttackers) t = ArmyWords(g_armies[g_rows[g_row]]);
    else if (g_choice == kPlots) t = PlotWords(*r, g_row);
    else {
        const Building& b = g_buildings[g_rows[g_row]];
        t = (b.display.empty() ? Widen(b.name.c_str()) : b.display) + L", цена " + std::to_wstring(b.cost) + L", ходов " + std::to_wstring(b.turns);
        int money = Treasury(g_local);
        if (money >= 0 && money < b.cost) t += L", не хватает " + std::to_wstring(b.cost - money);
    }
    Speak(t + L", " + std::to_wstring(g_row + 1) + L" из " + std::to_wstring(g_rows.size()), interrupt);
}

void OpenPlots(const Region& r) {
    g_choiceRegion = r.r;
    g_rows.clear();
    for (size_t i = 0; i < r.plots.size(); ++i) g_rows.push_back((int)i);
    if (g_rows.empty()) { Speak(r.name + L": участков нет. Здесь нельзя строить.", true); return; }
    g_choice = kPlots;
    g_row = 0;
    Speak(r.name + L", участков " + std::to_wstring(r.plots.size()) + L". PageDown: участки, End на пустом: выбрать постройку, апостроф: назад.", true);
    SayChoice(false);
}

void OpenBuildings(int plot) {
    std::string me;
    for (const Player& pl : g_players)
        if (pl.id == g_local) me = pl.tmpl;
    g_rows.clear();
    for (size_t i = 0; i < g_buildings.size(); ++i) {
        auto a = g_availableTo.find(g_buildings[i].name);
        if (a != g_availableTo.end() && a->second.count(me)) g_rows.push_back((int)i);
    }
    if (g_rows.empty()) { Speak(L"Список построек не прочитан", true); return; }
    std::sort(g_rows.begin(), g_rows.end(), [](int a, int b) { return g_buildings[a].cost > g_buildings[b].cost; });
    g_plot = plot;
    g_choice = kBuildings;
    g_row = 0;
    Speak(L"Что строить? " + TreasuryWords() + L"PageDown: постройки, Shift+Home: описание, End: строить, апостроф: назад.", true);
    SayChoice(false);
}

// End on a building: what it trains, or just its name.
void OpenUnits(uint32_t building) {
    std::wstring name = BuildingOn(building);
    if (!ReadSpawns(building) || g_spawns.empty()) { Speak(L"Участок занят: " + name, true); return; }
    g_rows.clear();
    for (size_t i = 0; i < g_spawns.size(); ++i) g_rows.push_back((int)i);
    // What can be trained comes first.
    std::stable_sort(g_rows.begin(), g_rows.end(), [](int a, int b) { return g_spawns[a].can > g_spawns[b].can; });
    g_choice = kUnits;
    g_row = 0;
    Speak(name + L", в очереди " + std::to_wstring(QueueLength(g_spawnNugget)) + L" из " + std::to_wstring(g_queueMax) +
              L". PageDown: отряды, Shift+Home: описание, End: нанять, апостроф: назад.",
          true);
    SayChoice(false);
}

void QueueUnit() {
    const Spawn s = g_spawns[g_rows[g_row]];
    if (!s.can) { Speak(s.name + L": сейчас нельзя. Нет денег, очков командования или места в очереди.", true); return; }
    int before = QueueLength(g_spawnNugget);
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool ok = false;
    SendQueueUnit((int)U32(g_spawnBuilding + 0x18), s.id, done, &ok);
    bool ran = done && WaitForSingleObject(done, 1000) == WAIT_OBJECT_0;
    if (done) CloseHandle(done);
    Sleep(500);  // the logic runs the message on its next frame
    int after = QueueLength(g_spawnNugget);
    LogLine(L"wotr: queue " + Widen(s.army.c_str()) + L": " + std::to_wstring(before) + L" -> " + std::to_wstring(after));
    if (ran && ok && after > before)
        Speak(L"Нанят: " + s.name + L". В очереди " + std::to_wstring(after) + L" из " + std::to_wstring(g_queueMax) + L".", true);
    else
        Speak(L"Игра не приняла найм: " + s.name, true);
    uint32_t building = g_spawnBuilding;
    int row = g_row;
    if (ReadSpawns(building) && !g_spawns.empty()) g_row = (std::min)(row, (int)g_rows.size() - 1);
    else g_choice = kNoChoice;
}

void Build() {
    const Region* r = ChoiceRegion();
    if (!r || g_plot >= (int)r->plots.size()) return;
    const Building b = g_buildings[g_rows[g_row]];
    int plotId = r->plots[g_plot].id, index = r->index;
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool ok = false;
    SendBuild(index, plotId, b.id, done, &ok);
    bool ran = done && WaitForSingleObject(done, 1000) == WAIT_OBJECT_0;
    if (done) CloseHandle(done);
    Sleep(500);  // the logic runs the message on its next frame
    Refresh();
    bool built = false;
    if (const Region* now = ChoiceRegion())
        for (const Plot& p : now->plots) built |= p.id == plotId && p.building;
    LogLine(L"wotr: build " + Widen(b.name.c_str()) + (built ? L" stands on the plot" : L": nothing on the plot"));
    g_choice = kNoChoice;
    Speak(ran && ok && built ? L"Строится: " + b.display : L"Игра не приняла постройку: " + b.display + L". Не хватает денег, или здесь это нельзя.", true);
}

void AttackWith(const Army& a, const Region& to) {
    g_moving = a.a;
    g_goals = {(int)(&to - g_regions.data())};
    g_goal = 0;
}

bool InBattle(const Region& r) { return std::find(g_battles.begin(), g_battles.end(), r.name) != g_battles.end(); }

// A real dialog takes the menu keys. Every other button of the map stays behind the apostrophe,
// or a click on a region would take the map's keys away (log 2026-11-04 22:27:06).
bool DialogPath(std::string p) {
    for (char& c : p) c = (char)tolower((unsigned char)c);
    // "veterancy", "main.select.": the screen after a battle (treasury charge, RESUME, log 01:47:44).
    for (const char* w : {"popup", "prompt", "award", "messagebox", "dialog", "resolver", "retreat", "dlgframe", "veterancy", "main.select."})
        if (p.find(w) != std::string::npos) return true;
    return false;
}

bool DialogNow() {
    std::vector<HudButton> hud;
    MenuHudButtons(hud);
    for (const HudButton& b : hud)
        if (DialogPath(b.path)) return true;
    return false;
}

// The game's way is a click on the battle marker model (LM_BMarker), a pick of its own that a
// click at the region's centre misses (log 2026-11-05 01:31-01:32, Worldbuilder
// ResolveBattlesBehavior::OnMouseLeftClick: FindClickableObject, marker -> battle id), and then the
// prompt. The prompt's buttons only send MSG_LW_SET_CONFLICT_RESOLUTION_METHOD, so that is sent.
// Manager +0x14..+0x18: pending battles (Worldbuilder 0xB59130), id at +0x30, vtable 0x00C4D534.
uint32_t BattleOf(const Region& r) {
    uint32_t logic = U32(kTheLivingWorldLogic), mgr = Ptr(logic) ? U32(logic + 0xB0) : 0;
    if (!Ptr(mgr)) return 0;
    std::vector<uint32_t> all;
    for (uint32_t q = U32(mgr + 0x14), e = U32(mgr + 0x18); q < e && all.size() < 32; q += 4)
        if (Ptr(U32(q)) && U32(U32(q)) == 0x00C4D534) all.push_back(U32(q));
    for (uint32_t b : all)
        for (uint32_t off = 4; off < 0x100; off += 4)
            if (U32(b + off) == r.r) {
                static std::set<uint32_t> logged;
                if (logged.insert(off).second) LogLine(L"wotr: battle region at +" + std::to_wstring(off));
                return b;
            }
    LogLine(L"wotr: " + std::to_wstring(all.size()) + L" pending battles, none names " + r.name);
    return all.empty() ? 0 : all[0];
}

const wchar_t* MethodWords(int method) { return method == 3 ? L"Бой вручную" : method == 1 ? L"Автобой" : L"Отступить"; }

void OpenBattle(const Region& r) {
    g_choiceRegion = r.r;
    g_rows = {3, 1, 2};
    g_row = 0;
    g_choice = kBattle;
    Speak(L"Битва: " + r.name + Standing(r) + L". PageDown: как сражаться, End: начать, апостроф: назад.", true);
    SayChoice(false);
}

// What auto-resolve answered is unknown: 2026-11-05 12:55:10 method 1 changed nothing in 8 s, on a
// story mission. So the log and the player hear it when the battle stays on the checklist.
std::wstring g_sentBattle;
int g_sentMethod = 0;
DWORD g_sentAt = 0;

void SendBattle(const Region& r, int method) {
    uint32_t battle = BattleOf(r);
    g_choice = kNoChoice;
    if (!battle) { Speak(L"Битва не найдена в памяти игры", true); return; }
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool ok = false;
    SendBattleMethod(method, (int)U32(battle + 0x30), done, &ok);
    bool ran = done && WaitForSingleObject(done, 1000) == WAIT_OBJECT_0;
    if (done) CloseHandle(done);
    Speak(ran && ok ? std::wstring(MethodWords(method)) + L": " + r.name : L"Команда не прошла", true);
    if (ran && ok && method != 3) {
        g_sentBattle = r.name;
        g_sentMethod = method;
        g_sentAt = GetTickCount();
    }
}

void WatchBattles() {
    static DWORD last = 0;
    if (g_phase != kPhaseBattles) { g_battles.clear(); g_sentAt = 0; return; }
    if (GetTickCount() - last < 1000) return;
    last = GetTickCount();
    const std::wstring head = L"Fight battle in ";  // STRATEGICHUD:ResolveBattleChecklistItem
    std::vector<std::wstring> now;
    for (std::wstring t : ScreenTextNow(head.c_str())) {
        t = t.substr(head.size());
        if (!t.empty() && t.back() == L'.') t.pop_back();
        now.push_back(t);
    }
    bool fresh = false;
    for (const std::wstring& t : now)
        if (std::find(g_battles.begin(), g_battles.end(), t) == g_battles.end()) {
            fresh = true;
            LogLine(L"wotr: battle in " + t);
            Speak(L"Битва: " + t + L". End на этом регионе: выбрать, как сражаться.", false);
        }
    g_battles = now;
    if (g_sentAt && GetTickCount() - g_sentAt > 8000) {
        bool waiting = std::find(now.begin(), now.end(), g_sentBattle) != now.end();
        LogLine(L"wotr: 8 s after method " + std::to_wstring(g_sentMethod) + L", " + g_sentBattle + (waiting ? L" still waits" : L" is gone"));
        if (waiting) Speak(std::wstring(MethodWords(g_sentMethod)) + L" не сработал: битва " + g_sentBattle + L" ещё ждёт.", false);
        g_sentAt = 0;
    }
    if (fresh) {
        g_view = kBattleView;
        g_pos = 0;
        g_cur = 0;
        g_choice = kNoChoice;
        g_moving = 0;
    }
}

std::wstring PhaseWords(int phase);

void SayStatus() {
    int regions = 0, heroes = 0;
    for (const Region& r : g_regions) regions += r.owner == g_local;
    for (const Army& a : g_armies) heroes += a.owner == g_local && a.hero;
    std::wstring side;
    for (const Player& pl : g_players)
        if (pl.id == g_local) side = pl.name;
    std::wstring phase = PhaseWords(g_phase);
    LogTreasury(L"status");
    Speak(L"Ход " + std::to_wstring(g_turn + 1) + L". " + (phase.empty() ? L"" : phase + L". ") + L"Вы играете за " + side + L". " + TreasuryWords() + L"Регионов " +
              std::to_wstring(regions) + L" из " +
              std::to_wstring(g_regions.size()) + L", героев " + std::to_wstring(heroes) + L".",
          true);
    // Where the HUD keeps the money is not known yet: its numbers go to the log.
    std::wstring numbers;
    for (const std::wstring& t : ScreenTextNow(L""))
        if (t.size() < 24 && std::any_of(t.begin(), t.end(), iswdigit)) numbers += L" [" + t + L"]";
    LogLine(L"wotr: hud numbers" + numbers.substr(0, 400));
}

void CameraOn(const Region& r, bool click) {
    float bx = 0, by = 0, cx = 0, cy = 0;
    CameraPosition(bx, by);
    bool on = ScannerCameraToFree(r.x, r.y);
    CameraPosition(cx, cy);
    wchar_t b[160];
    swprintf_s(b, L"wotr: camera (%.0f, %.0f) -> (%.0f, %.0f) for (%.0f, %.0f), %s", bx, by, cx, cy, r.x, r.y, on ? L"on target" : L"NOT on target");
    LogLine(b + (L" " + r.name));
    if (!click) { Speak((on ? L"Камера: " : L"Камера не дошла: ") + r.name, true); return; }
    Sleep(150);
    ScannerClickCentre(false);
    Speak((on ? L"Выбран регион " : L"Камера не дошла, щелчок мимо: ") + r.name, true);
}

void StartMove(const Army& a) {
    const Region* from = RegionAt(a.region);
    if (!from) { Speak(L"Не знаю, где стоит эта армия", true); return; }
    g_goals.clear();
    for (const std::string& id : from->links) {
        const Region* n = RegionById(id);
        if (n) g_goals.push_back((int)(n - g_regions.data()));
    }
    if (g_goals.empty()) { Speak(L"Отсюда идти некуда", true); return; }
    g_moving = a.a;
    g_goal = 0;
    Speak(a.leader + L" в " + from->name + L". Куда идти? PageDown: соседи, End: отправить, апостроф: отмена.", true);
    Speak(RegionWords(g_regions[g_goals[0]]) + L", 1 из " + std::to_wstring(g_goals.size()), false);
}

void SendMove() {
    const Army* a = nullptr;
    for (const Army& x : g_armies)
        if (x.a == g_moving) a = &x;
    const Region& to = g_regions[g_goals[g_goal]];
    g_moving = 0;
    if (!a) { Speak(L"Армии уже нет", true); return; }
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool ok = false;
    SendArmyMove(a->id, to.index, to.x, to.y, done, &ok);
    bool ran = done && WaitForSingleObject(done, 1000) == WAIT_OBJECT_0;
    if (done) CloseHandle(done);
    Speak(ran && ok ? L"Приказ: " + a->leader + L" идёт в " + to.name : std::wstring(L"Приказ не прошёл"), true);
    Sleep(400);
    float v[6] = {};
    SafeRead(a->a + 0x30, v, sizeof v);
    wchar_t b[160];
    swprintf_s(b, L"wotr: army %d after the order: (%.0f, %.0f) (%.0f, %.0f) (%.0f, %.0f), region %08X", a->id, v[0], v[1], v[2], v[3], v[4], v[5],
               U32(a->a + 0x88));
    LogLine(b);
}

void EndTurn() {
    if (g_phase == kPhaseBattles && !g_battles.empty()) {
        Speak(L"Сначала битва: " + g_battles[0] + L". End на этом регионе.", true);
        return;
    }
    DWORD now = GetTickCount();
    if (now - g_endAsked > 4000) {
        g_endAsked = now;
        Speak(L"Закончить ход? Ещё раз Ctrl+End.", true);
        return;
    }
    g_endAsked = 0;
    // The game's own end-phase call, as its End Turn button handler makes it: no click.
    int r = WotrEndPhase();
    if (r == 1) { Speak(L"Ход закончен", true); return; }
    if (r == 0) {
        Speak(L"Игра пока не даёт закончить ход: сначала дела из списка задач на экране. Апостроф: кнопки карты.", true);
        return;
    }
    // The code differs or the call failed: the button, as before.
    bool ok = MenuClickHud("endTurnButton", false);
    Speak(ok ? L"Ход закончен кнопкой" : L"Кнопка конца хода не найдена", true);
}

// The HUD's titles are APT:TacticalPhaseTitle, BattlePhaseTitle, RetreatPhaseTitle. Which number
// is which, from the log of 2026-11-05: 0 Tactical, 2 Battle, 3 Retreat; 1 is the armies walking.
// 4 to 6 have no title and need nothing from the player.
std::wstring PhaseWords(int phase) {
    switch (phase) {
        case kPhaseOrders: return L"Фаза приказов";
        case 1: return L"Армии идут";
        case kPhaseBattles: return L"Фаза битв";
        case 3: return L"Фаза отступления";
        default: return {};
    }
}

// What changed since the last orders phase: regions won and lost, new armies of yours, other
// players' heroes at your borders, and the treasury. Said once each time the orders phase begins.
std::map<std::string, int> g_lastOwner;
std::set<uint32_t> g_lastMine;
std::map<uint32_t, uint32_t> g_lastRegionOf;  // other players' heroes: army -> region last turn
int g_lastMoney = -1, g_reportedTurn = -1;

void TurnReport() {
    if (!Refresh()) return;
    std::wstring won, lost, fresh, nearby;
    bool first = g_lastOwner.empty();
    for (const Region& r : g_regions) {
        auto was = g_lastOwner.find(r.id);
        if (!first && was != g_lastOwner.end()) {
            if (r.owner == g_local && was->second != g_local) won += (won.empty() ? L"" : L", ") + r.name;
            if (r.owner != g_local && was->second == g_local) lost += (lost.empty() ? L"" : L", ") + r.name + L" (" + OwnerWord(r.owner) + L")";
        }
        g_lastOwner[r.id] = r.owner;
    }
    std::set<uint32_t> mine;
    int nearCount = 0;
    for (const Army& a : g_armies) {
        if (a.owner == g_local && !a.leader.empty()) {
            mine.insert(a.a);
            if (!first && !g_lastMine.count(a.a)) fresh += (fresh.empty() ? L"" : L", ") + a.leader;
        } else if (a.owner >= 0 && a.hero && !a.leader.empty() && NearMine(a)) {
            if (++nearCount <= 3) {
                const Region* at = RegionAt(a.region);
                nearby += (nearby.empty() ? L"" : L", ") + a.leader + (at ? L" в " + at->name : L"");
            }
        }
    }
    g_lastMine = mine;
    // Other players' heroes that changed region since the last report and now stand by you.
    std::wstring came;
    int cameCount = 0, movedCount = 0;
    std::map<uint32_t, uint32_t> regionOf;
    for (const Army& a : g_armies) {
        if (!Foreign(a)) continue;
        regionOf[a.a] = a.region;
        auto was = g_lastRegionOf.find(a.a);
        if (first || was == g_lastRegionOf.end() || was->second == a.region) continue;
        ++movedCount;
        if (NearMine(a) && ++cameCount <= 4) {
            const Region* at = RegionAt(a.region);
            came += (came.empty() ? L"" : L", ") + a.leader + (at ? L" в " + at->name : L"");
        }
    }
    g_lastRegionOf = regionOf;
    int money = Treasury(g_local);
    LogTreasury(L"turn");
    std::wstring t = L"Ход " + std::to_wstring(g_turn + 1) + L", ваши приказы.";
    if (money >= 0) {
        t += L" Казна " + std::to_wstring(money);
        if (g_lastMoney >= 0 && money != g_lastMoney) t += (money > g_lastMoney ? L", плюс " : L", минус ") + std::to_wstring(std::abs(money - g_lastMoney));
        t += L".";
    }
    g_lastMoney = money;
    if (!won.empty()) t += L" Захвачено: " + won + L".";
    if (!lost.empty()) t += L" Потеряно: " + lost + L".";
    if (!fresh.empty()) t += L" Новые армии: " + fresh + L".";
    if (movedCount) t += L" Чужих героев перешло: " + std::to_wstring(movedCount) + (cameCount ? L", к вашим границам: " + came : L"") + L".";
    if (nearCount) t += L" У ваших границ чужих героев " + std::to_wstring(nearCount) + L": " + nearby + (nearCount > 3 ? L" и другие" : L"") + L". Список: чужие армии рядом.";
    LogLine(L"wotr: report " + t);
    Speak(t, true);
}

// Other players' heroes on the march (phase 1, armies walk): who comes into your regions, who walks
// along your borders, how many elsewhere. Every march goes to the log with its points.
void MarchesReport() {
    if (!Refresh()) return;
    std::wstring tome, border;
    int toMe = 0, atBorder = 0, other = 0;
    for (const Army& a : g_armies) {
        if (!Foreign(a)) continue;
        const Region* g = GoalRegion(a);
        if (!g) continue;
        const Region* from = RegionAt(a.region);
        wchar_t b[200];
        swprintf_s(b, L"wotr: march %d (%.0f, %.0f) -> (%.0f, %.0f) ", a.id, a.x, a.y, a.gx, a.gy);
        LogLine(b + a.leader + L" " + OwnerWord(a.owner) + L": " + (from ? from->name : L"?") + L" -> " + g->name);
        std::wstring w = a.leader + L" (" + OwnerWord(a.owner) + L")" + (from ? L" из " + from->name : L"") + L" в " + g->name;
        if (g->owner == g_local) { ++toMe; tome += (tome.empty() ? L"" : L"; ") + w; }
        else if (NearMine(a)) { if (++atBorder <= 4) border += (border.empty() ? L"" : L"; ") + w; }
        else ++other;
    }
    if (!toMe && !atBorder && !other) { LogLine(L"wotr: marches: none"); return; }
    std::wstring t = L"Передвижения.";
    if (toMe) t += L" На ваши земли: " + tome + L".";
    if (atBorder) t += L" У ваших границ: " + border + (atBorder > 4 ? L" и ещё " + std::to_wstring(atBorder - 4) : L"") + L".";
    if (other) t += L" Вдали от вас: " + std::to_wstring(other) + L".";
    t += L" Список: движения чужих армий.";
    LogLine(L"wotr: marches " + t);
    Speak(t, false);
}

// The first look at a scenario's map: what the next report compares with, said nowhere.
std::wstring g_baseScenario;
void Baseline() {
    if (g_scenario == g_baseScenario) return;
    g_baseScenario = g_scenario;
    g_lastOwner.clear();
    g_lastMine.clear();
    for (const Region& r : g_regions) g_lastOwner[r.id] = r.owner;
    g_lastRegionOf.clear();
    for (const Army& a : g_armies) {
        if (a.owner == g_local && !a.leader.empty()) g_lastMine.insert(a.a);
        if (Foreign(a)) g_lastRegionOf[a.a] = a.region;
    }
    g_lastMoney = Treasury(g_local);
    uint32_t logic = U32(kTheLivingWorldLogic);
    g_reportedTurn = Ptr(logic) ? (int)U32(logic + 0xFC) : -1;
    LogTreasury(L"first look");
}

void WatchTurn() {
    uint32_t logic = U32(kTheLivingWorldLogic);
    if (!Ptr(logic)) return;
    int phase = (int)U32(logic + 0xF4), turn = (int)U32(logic + 0xFC);
    if (phase == kPhaseOrders && turn != g_reportedTurn && g_phase >= 0 && (phase != g_phase || turn != g_turn)) {
        g_phase = phase;
        g_turn = turn;
        g_reportedTurn = turn;
        LogLine(L"wotr: phase " + std::to_wstring(phase) + L" turn " + std::to_wstring(turn));
        TurnReport();
        return;
    }
    if (phase == g_phase && turn == g_turn) return;
    bool first = g_phase < 0;
    LogLine(L"wotr: phase " + std::to_wstring(phase) + L" turn " + std::to_wstring(turn));
    bool newTurn = turn != g_turn;
    g_phase = phase;
    g_turn = turn;
    if (first) return;
    // Each line replaces the last: phases 1 to 5 pass in about a second each when nothing happens.
    std::wstring words = PhaseWords(phase);
    if (newTurn) words = L"Ход " + std::to_wstring(turn + 1) + L". " + words;
    if (!words.empty()) Speak(words, true);
    if (phase == 1) MarchesReport();
}

void Poll() {
    static DWORD last = 0;
    if (GetTickCount() - last < 300) return;
    last = GetTickCount();
    bool map = false, dialog = false;
    uintptr_t screen = 0;
    std::string name;
    int count = 0;
    TopScreen(screen, name, count);
    if (!MenuInBattle()) {
        std::vector<HudButton> hud;
        MenuHudButtons(hud);
        for (const HudButton& b : hud) {
            if (b.path.find("main.globe") != std::string::npos || b.path.find("main.endTurn") != std::string::npos) map = true;
            else if (DialogPath(b.path)) dialog = true;
            else if (b.path.find(".main.") == std::string::npos && g_onMap && g_otherPaths.insert(b.path).second && g_otherPaths.size() < 300)
                LogLine(L"wotr: other button " + Widen(b.path.c_str()));
        }
    }
    // The list is empty for a moment when a screen closes: that is not leaving the map.
    if (map) g_gone = 0;
    else if (g_onMap && ++g_gone < 4) map = true;
    if (map && !g_onMap) {
        LogLine(L"wotr: strategic map");
        g_mapShell = count;
        g_buttons = false;
        g_moving = 0;
        g_choice = kNoChoice;
        if (!g_said && Refresh()) {
            g_said = true;
            BuildView();
            LogLine(L"wotr: scenario " + g_scenario + L", " + std::to_wstring(g_regions.size()) + L" regions, " +
                    std::to_wstring(g_armies.size()) + L" armies, " + std::to_wstring(g_players.size()) + L" players, local " +
                    std::to_wstring(g_local));
            int regions = 0;
            for (const Region& r : g_regions) regions += r.owner == g_local;
            Baseline();
            Speak(L"Глобальная карта. Ваших регионов " + std::to_wstring(regions) + L". " + TreasuryWords() +
                      L"PageDown: список. Ctrl+PageDown: другой список. End: на герое поход, на своём регионе стройка, на чужом атака. "
                      L"Shift+Home: подробно о регионе. Shift+End: сводка. Ctrl+End: конец хода.",
                  true);
        }
    } else if (!map && g_onMap) {
        LogLine(L"wotr: left the map");
        g_said = count > g_mapShell;  // a screen over the map; a battle or the menu starts anew
    }
    if (map && count > g_mapShell) dialog = true;
    if (dialog != g_dialog && map) LogLine(dialog ? L"wotr: dialog, menu keys on" : L"wotr: dialog gone");
    g_onMap = map;
    g_dialog = dialog && map;
    if (g_onMap) { WatchTurn(); WatchBattles(); }
    MenuSetWorldMap(g_onMap);
    MenuSetQuiet(g_onMap && !g_buttons && !g_dialog);
}

}  // namespace

// Who may build what is read from the mod's INI: AvailableTo = PlayerMen PlayerRohan ...
void WotrInit(const std::wstring& modFolder) {
    std::ifstream f(modFolder + L"\\data\\ini\\livingworldbuildings.ini");
    std::string line, current;
    while (std::getline(f, line)) {
        std::stringstream in(line);
        std::string a, b;
        in >> a;
        if (a == "LivingWorldBuilding") in >> current;
        else if (a == "AvailableTo" && in >> b && b == "=")
            for (std::string who; in >> who && who[0] != ';' && who[0] != '/';) g_availableTo[current].insert(who);
    }
    LogLine(L"wotr: " + std::to_wstring(g_availableTo.size()) + L" buildings with AvailableTo");
}

bool WotrActive() { return g_onMap && !g_buttons && !g_dialog; }

void WotrKeys(bool gameFocused) {
    static const int keys[] = {VK_OEM_7, VK_NEXT, VK_PRIOR, VK_HOME, VK_END};
    Poll();
    if (!gameFocused || !g_onMap) {
        for (int vk : keys) g_keyDown[vk] = false;
        return;
    }
    if (Pressed(VK_OEM_7) && !g_dialog) {
        if (g_moving) { g_moving = 0; Speak(L"Отмена", true); return; }
        if (g_choice == kBuildings || g_choice == kUnits) {
            if (const Region* r = ChoiceRegion()) OpenPlots(*r);
            else g_choice = kNoChoice;
            return;
        }
        if (g_choice != kNoChoice) { g_choice = kNoChoice; Speak(L"Назад к списку", true); return; }
        g_buttons = !g_buttons;
        LogLine(g_buttons ? L"wotr: palantir buttons" : L"wotr: map keys");
        MenuSetQuiet(!g_buttons);
        Speak(g_buttons ? L"Кнопки карты. Вверх и вниз, Enter нажимает." : L"Карта.", true);
        return;
    }
    if (!WotrActive()) return;
    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    bool next = Pressed(VK_NEXT), prev = Pressed(VK_PRIOR), home = Pressed(VK_HOME), end = Pressed(VK_END);
    if (!next && !prev && !home && !end) return;
    if (!Refresh()) { Speak(L"Карта не прочитана", true); return; }
    int step = next ? 1 : -1;
    if (g_moving) {
        int n = (int)g_goals.size();
        if (next || prev) g_goal = (g_goal + step + n) % n;
        const Region& goal = g_regions[g_goals[g_goal]];
        if (next || prev || (home && !shift)) Speak(RegionWords(goal) + L", " + std::to_wstring(g_goal + 1) + L" из " + std::to_wstring(n), true);
        else if (home) SayDetails(goal);
        else if (end) SendMove();
        return;
    }
    if (g_choice != kNoChoice) {
        const Region* r = ChoiceRegion();
        int n = (int)g_rows.size();
        if (!r || !n) { g_choice = kNoChoice; return; }
        if (g_choice == kPlots) {  // the refresh re-read the plots: same regions, same order
            if (g_row >= (int)r->plots.size()) g_row = 0;
        }
        if (next || prev) { g_row = (g_row + step + n) % n; SayChoice(true); }
        else if (home && shift && g_choice == kBuildings) {
            const Building& b = g_buildings[g_rows[g_row]];
            Speak(b.about.empty() ? L"нет описания" : b.about, true);
        } else if (home && shift && g_choice == kUnits) {
            const Spawn& s = g_spawns[g_rows[g_row]];
            Speak(s.about.empty() ? L"нет описания" : s.about, true);
        } else if (home) SayChoice(true);
        else if (end && g_choice == kBattle) SendBattle(*r, g_rows[g_row]);
        else if (end && g_choice == kUnits) QueueUnit();
        else if (end && g_choice == kAttackers) {
            AttackWith(g_armies[g_rows[g_row]], *r);
            g_choice = kNoChoice;
            SendMove();
        } else if (end && g_choice == kPlots) {
            const Plot& p = r->plots[g_rows[g_row]];
            if (p.building) OpenUnits(p.building);
            else OpenBuildings(g_rows[g_row]);
        } else if (end) Build();
        return;
    }
    if (ctrl && shift && end) {
        BuildView();
        if (const Region* r = g_entries.empty() ? nullptr : EntryRegion()) CameraOn(*r, true);
        return;
    }
    if (ctrl && end) { EndTurn(); return; }
    if (shift && end) { SayStatus(); return; }
    if (ctrl && (next || prev)) {
        g_view = (g_view + step + kViewCount) % kViewCount;
        g_pos = 0;
        BuildView();
        // An interrupting line clears what is still queued: the entry must not cut the list's name.
        Speak(std::wstring(kViews[g_view]) + L", " + std::to_wstring(g_entries.size()), true);
        if (!g_entries.empty()) SayEntry(false);
        return;
    }
    BuildView();
    int n = (int)g_entries.size();
    if (!n) { SayEntry(); return; }
    if (next || prev) {
        g_pos = (g_pos + step + n) % n;
        SayEntry();
        return;
    }
    const Region* r = EntryRegion();
    if (home) {
        if (ctrl) { if (r) CameraOn(*r, false); }
        else if (shift) { if (r) SayDetails(*r); }
        else SayEntry();
        return;
    }
    const Entry& e = g_entries[g_pos];
    if (r && g_phase == kPhaseBattles && InBattle(*r)) { OpenBattle(*r); return; }
    if (g_phase != kPhaseOrders && (e.army || (r && r->owner != g_local))) {
        Speak(L"Сейчас приказы не принимаются. Ctrl+End: дальше.", true);
        return;
    }
    // Your hero: where it goes. Another player's army: an attack on the region it stands in.
    if (e.army && g_armies[e.index].hero && g_armies[e.index].owner == g_local) { StartMove(g_armies[e.index]); return; }
    if (!r) return;
    if (r->owner == g_local) { OpenPlots(*r); return; }
    // A foreign region: who can go there.
    g_rows = ArmiesNear(*r);
    if (g_rows.empty()) {
        Speak(L"Рядом с " + r->name + L" нет ваших героев. Сюда идут только из соседнего региона.", true);
        return;
    }
    g_choiceRegion = r->r;
    g_choice = kAttackers;
    g_row = 0;
    Speak(L"Кто идёт на " + r->name + L"? PageDown: герои рядом, End: отправить, апостроф: отмена.", true);
    SayChoice(false);
}
