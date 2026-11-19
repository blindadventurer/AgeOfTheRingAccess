// Menu navigator: the mod owns the focus on mouse-only APT menus.
// Each tick it reads the game's button instance list (BIL), builds a list of
// buttons with names and screen points, and lets Up/Down move over it.
// Enter clicks the focused button with the real mouse, so the game's own input
// path does the rest.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "gadgets.h"
#include "game.h"
#include "gamethread.h"
#include "log.h"
#include "memory.h"
#include "menu.h"
#include "screens.h"
#include "screentext.h"
#include "speech.h"
#include "spells.h"
#include "strings.h"
#include "wotrsetup.h"

namespace {

struct Item {
    uintptr_t cih = 0;              // APT button, or 0 for a window gadget
    uintptr_t window = 0;           // window gadget
    int row = -1;                   // list row of that gadget
    std::vector<std::string> path;  // instance names, button first, root last
    std::wstring label;
    float x = 0, y = 0;             // centre in movie coordinates
    bool hasBounds = false;
    std::wstring details;           // longer text for Shift+Space (a map's description)
    float cx = 0, cy = 0;           // window gadget centre in client pixels
    int total = 0;                  // list rows in use (list rows only)
    int group = 0;                  // Tab stop: 0 APT buttons, 1 settings, 2+ lists
    bool inCombo = false;           // a row of an open drop-down: a click also closes it
    int column = 0;                 // inside a group, items with close x share a column
    int picker = 0;                 // Enter opens the choice of: 1 map, 2 War of the Ring scenario, 3 its quick start, 4 skirmish quick start (wotrsetup)
    int spellSlot = 0;              // SpellStore.Buttons.SpellN: N; Enter buys it without the mouse
    std::vector<int> sciences;      // that button's sciences, read while the store was open
};

// The map preview's start position buttons name no place: "Starting Position" while free, "Start
// Position for player 1" when taken. The preview draws the whole map, so the buttons stand like
// Player_N_Start in mapcache.ini, north up: both sets are scaled to their bounding boxes and
// the nearest pairs taken (start 1 and 2 of the loading screen, log 2026-11-05 17:44:24).
std::wstring MapSide(float x, float y, float w, float h) {
    float dx = (x - w / 2) / (w > 0 ? w : 1), dy = (y - h / 2) / (h > 0 ? h : 1);
    bool east = dx > 0.15f, west = dx < -0.15f, north = dy > 0.15f, south = dy < -0.15f;
    if (north) return east ? L"северо-восток" : west ? L"северо-запад" : L"север";
    if (south) return east ? L"юго-восток" : west ? L"юго-запад" : L"юг";
    return east ? L"восток" : west ? L"запад" : L"центр";
}

int GroupFor(uintptr_t key, const std::wstring& name);

void NameStartButtons(std::vector<Item>& items, const std::vector<size_t>& buttons, const MapInfo* map) {
    if (!map || buttons.empty() || buttons.size() != map->starts.size()) return;
    float bx0 = 1e9f, bx1 = -1e9f, by0 = 1e9f, by1 = -1e9f, sx0 = 1e9f, sx1 = -1e9f, sy0 = 1e9f, sy1 = -1e9f;
    for (size_t b : buttons) {
        bx0 = (std::min)(bx0, items[b].x), bx1 = (std::max)(bx1, items[b].x);
        by0 = (std::min)(by0, items[b].y), by1 = (std::max)(by1, items[b].y);
    }
    for (const auto& s : map->starts) {
        sx0 = (std::min)(sx0, s.first), sx1 = (std::max)(sx1, s.first);
        sy0 = (std::min)(sy0, s.second), sy1 = (std::max)(sy1, s.second);
    }
    auto unit = [](float v, float lo, float hi) { return hi - lo < 1.f ? 0.5f : (v - lo) / (hi - lo); };
    struct Pair { float d; size_t button, start; };
    std::vector<Pair> pairs;
    for (size_t b = 0; b < buttons.size(); ++b)
        for (size_t s = 0; s < map->starts.size(); ++s) {
            const Item& it = items[buttons[b]];
            float dx = unit(it.x, bx0, bx1) - unit(map->starts[s].first, sx0, sx1);
            float dy = (1.f - unit(it.y, by0, by1)) - unit(map->starts[s].second, sy0, sy1);  // screen y goes down
            pairs.push_back({dx * dx + dy * dy, b, s});
        }
    std::sort(pairs.begin(), pairs.end(), [](const Pair& l, const Pair& r) { return l.d < r.d; });
    std::vector<bool> usedButton(buttons.size()), usedStart(map->starts.size());
    for (const Pair& p : pairs) {
        if (usedButton[p.button] || usedStart[p.start]) continue;
        usedButton[p.button] = usedStart[p.start] = true;
        Item& it = items[buttons[p.button]];
        const auto& s = map->starts[p.start];
        size_t digit = it.label.find_last_of(L"12345678");
        std::wstring who = it.label.rfind(L"Start Position for player", 0) == 0 && digit != std::wstring::npos
                               ? std::wstring(L", занята: игрок ") + it.label[digit] : L", свободна";
        bool keep = p.start == 0 && map->name.rfind(L"Fortress:", 0) == 0;
        it.label = L"Позиция " + std::to_wstring(p.start + 1) + L", " +
                   (keep ? std::wstring(L"крепость") : MapSide(s.first, s.second, map->width, map->height)) + who;
        it.group = GroupFor(4, L"стартовые позиции");
    }
}

// The choice of a map or a scenario: a small list of its own over the setup screen.
struct PickRow {
    int row = 0;            // row in the game's list
    std::wstring name, details;
    int kind = 0;           // index into kPickKinds
    int players = 0;
    std::wstring army;      // the faction this map is played with
};

HWND g_wnd = nullptr;
std::vector<Item> g_items;
bool g_worldMap = false;
DWORD g_holdFocus = 0;  // until then a rebuild neither moves the focus nor speaks
DWORD g_hoverAt = 0;
uint64_t g_hoverKey = 0;
std::map<uint64_t, std::wstring> g_hoverText;
// Kinds of maps, in the order Left and Right go through them.
const wchar_t* const kPickKinds[] = {L"все карты", L"обычные", L"крепости", L"осады", L"другая фракция", L"приключения"};
int g_focus = 0;
uintptr_t g_screen = 0;
std::string g_screenName;
std::string g_pendingSig, g_sig;
int g_stableTicks = 0;
// 1: movie stretched to the window (proven 2026-10-31 22:40: the game's own movie-space
// mouse at gpPool+0x74/+0x78 read 277,150 for screen 520,211 and 427,150 for 802,212 on
// 1920x1080, i.e. x*1.875, y*1.406). 0: fitted to height and centred (wrong, kept to compare).
int g_mapMode = 1;
bool g_quiet = false;
struct Picker {
    bool open = false;
    int kind = 0, pos = 0, group = 0, players = 0;
    uintptr_t list = 0;
    std::vector<PickRow> rows;
    std::vector<int> shown;  // rows of the current group and player count
} g_pick;
std::vector<PickRow> g_mapRows;
uintptr_t g_mapList = 0;
void MenuSay(const std::wstring& t, bool interrupt) { if (!g_quiet && !g_pick.open && !WotrSetupIsOpen()) Speak(t, interrupt); }
bool g_inGame = false;  // battle HUD on screen: arrows belong to the camera
bool g_overlay = false; // a screen pushed over the battle (pause menu, spell book): arrows are ours
int g_shellCount = 0, g_battleCount = 0;  // screens on the shell now / when the battle HUD appeared
std::map<int, std::set<uintptr_t>> g_shownAt;  // shell count -> APT buttons seen at that count
bool g_keyDown[256] = {};

// Apt instance name: string data {u16 refcount, u16 length, ..., chars at +8}.
std::string AptName(uintptr_t cih) {
    uint32_t p = 0;
    if (!SafeReadU32(cih + 8, p) || p < 0x10000) return {};
    uint16_t len = 0;
    if (!SafeRead(p + 2, &len, 2) || len == 0 || len > 64) return {};
    char buf[65];
    if (!SafeRead(p + 8, buf, len)) return {};
    buf[len] = 0;
    for (int i = 0; i < len; ++i)
        if ((unsigned char)buf[i] < 0x20 || (unsigned char)buf[i] >= 0x7F) return {};
    return buf;
}

std::vector<std::string> PathOf(uintptr_t cih) {
    std::vector<std::string> path;
    for (int depth = 0; cih && depth < 10; ++depth) {
        path.push_back(AptName(cih));
        uint32_t parent = 0;
        if (!SafeReadU32(cih + 0x48, parent)) break;
        cih = parent;
    }
    return path;
}

bool Generic(const std::string& n) {
    if (n.empty()) return true;
    std::string l = n;
    for (auto& c : l) c = (char)tolower((unsigned char)c);
    // "TheButton": QuitMenu.apt's Options/Save/Load/Restart/ExitMission/Resume.TheButton
    // (log 2026-11-01 06:58, spoken as "The Button"): the parent names the button.
    return l == "bttn" || l == "button" || l == "btn" || l == "clip" || l == "hit" || l == "thebutton" ||
           l == "hitarea" || l.rfind("instance", 0) == 0;
}

bool Digits(const std::string& n) {
    return !n.empty() && std::all_of(n.begin(), n.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// Last resort for a name lotr.str does not know: "endTurnButton" -> "End Turn",
// "selectionDetails" -> "Selection Details" (War of the Ring map, 2026-11-01).
std::string StripControlSuffix(std::string n) {
    for (const char* sfx : {"Button", "Btn", "Bttn", "Clip"}) {
        size_t k = strlen(sfx);
        if (n.size() > k + 1 && n.compare(n.size() - k, k, sfx) == 0) { n.resize(n.size() - k); break; }
    }
    if (!n.empty()) n[0] = (char)toupper((unsigned char)n[0]);
    return n;
}

std::wstring SplitCamel(const std::string& n) {
    std::wstring w;
    for (size_t i = 0; i < n.size(); ++i) {
        char c = n[i];
        if (c == '_') { w += L' '; continue; }
        if (i > 0 && isupper((unsigned char)c) && islower((unsigned char)n[i - 1])) w += L' ';
        w += (wchar_t)(unsigned char)c;
    }
    return w;
}

std::wstring Clean(std::wstring s) {
    for (size_t p; (p = s.find(L"\\n")) != std::wstring::npos;) s.replace(p, 2, L" ");
    return s;
}

// The nearest real name on the path decides the label; empty when the button
// and all its parents are unnamed (backgrounds, logos).
std::wstring LabelFor(const std::vector<std::string>& path, const std::string& movie) {
    // Controls the game draws with no caption of their own (map list sort headers).
    static const std::map<std::string, std::wstring> kNamed = {
        {"OnSortName", L"сортировка по названию карты"},
        {"OnSortPlayers", L"сортировка по числу игроков"},
        {"OnSortIcon", L"сортировка по типу карты"},
        {"endTurnButton", L"Конец хода"}, {"objectivesButton", L"Задания"}, {"optionsButton", L"Настройки"},
        {"ems", L"Задача"}, {"commandUI", L"Команда"}};
    // The .const pairing is wrong for these (QuitMenu.apt Resume read "SAVE", ExitMission
    // "OPTIONS", SaveLoad.apt Delete read the delete question; log 2026-11-02).
    static const std::map<std::string, std::string> kFixedKeys = {
        {"Resume", "APT:Resume"}, {"ExitMission", "APT:ExitMission"},
        {"Save", "APT:Save"}, {"Delete", "APT:Delete"}};
    for (size_t i = 0; i < path.size(); ++i) {
        const std::string& name = path[i];
        if (Generic(name)) continue;
        if (name == "globe") return {};  // the palantir's picture on the War of the Ring map
        // Numbered slots ("commandUI.0.button", "checklist.items.0"): the parent's label and
        // the slot counted from 1.
        if (Digits(name)) {
            std::vector<std::string> rest(path.begin() + i + 1, path.end());
            std::wstring parent = LabelFor(rest, movie);
            return (parent.empty() ? std::wstring(L"элемент") : parent) + L" " + std::to_wstring(atoi(name.c_str()) + 1);
        }
        auto named = kNamed.find(name);
        if (named != kNamed.end()) return named->second;
        auto fixed = kFixedKeys.find(name);
        if (fixed != kFixedKeys.end()) {
            std::wstring t = LookupStr(fixed->second);
            if (!t.empty()) return Clean(t);
        }
        if (name[0] == '$') {  // an instance named after its label key ("$Campaign")
            std::wstring t = LookupStr("APT:" + name.substr(1));
            if (!t.empty()) return Clean(t);
        }
        std::string key = ConstKeyFor(movie, name);
        if (!key.empty() && key[0] == '$') {
            std::wstring t = LookupStr("APT:" + key.substr(1));
            if (!t.empty()) return Clean(t);
        }
        std::wstring t = LookupAptName(name);
        if (!t.empty()) return Clean(t);
        std::string shown = name;
        if (shown.rfind("tooltips", 0) == 0 && shown.size() > 8) shown = shown.substr(8);
        return SplitCamel(StripControlSuffix(shown));
    }
    return {};
}

bool Finite(float v) { return std::isfinite(v) && std::fabs(v) < 10000.f; }

// Centre of the button's first hit shape, through the BIL matrix. Layout from
// Open-BFME-2 (1.06): charInst+0xC -> character; character+0x2C record count,
// +0x30 records (0x44 bytes: +4 shape*, +0xC record matrix); shape +8 x0,y0,x1,y1.
bool HitCentre(const game::BilEntry& e, float& x, float& y) {
    uint32_t inst = 0, chr = 0, count = 0, records = 0, shape = 0;
    if (!SafeReadU32(e.cih + 0x4C, inst) || !inst) return false;
    if (!SafeReadU32(inst + 0xC, chr) || !chr) return false;
    if (!SafeReadU32(chr + 0x2C, count) || count == 0 || count > 16) return false;
    if (!SafeReadU32(chr + 0x30, records) || !records) return false;
    if (!SafeReadU32(records + 4, shape) || !shape) return false;
    float m[6], r[4];
    if (!SafeRead(records + 0xC, m, sizeof(m)) || !SafeRead(shape + 8, r, sizeof(r))) return false;
    for (float v : m) if (!Finite(v)) return false;
    for (float v : r) if (!Finite(v)) return false;
    if (r[2] <= r[0] || r[3] <= r[1]) return false;
    float cx = (r[0] + r[2]) / 2, cy = (r[1] + r[3]) / 2;
    float lx = m[0] * cx + m[2] * cy + m[4], ly = m[1] * cx + m[3] * cy + m[5];
    x = e.a * lx + e.c * ly + e.tx;
    y = e.b * lx + e.d * ly + e.ty;
    return Finite(x) && Finite(y);
}

std::string Join(const std::vector<std::string>& path) {
    std::string s;
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
        if (!s.empty()) s += '.';
        s += it->empty() ? "?" : *it;
    }
    return s;
}

std::string Stem(const std::string& file) {
    std::string n = file.substr(file.find_last_of("\\/") + 1);
    return n.substr(0, n.rfind('.'));
}

std::wstring ScreenLabel(const std::string& stem) {
    std::wstring t = LookupStr("APT:" + stem);
    return t.empty() ? SplitCamel(stem) : Clean(t);
}

void HoverFocus();
bool Pressed(int vk);
bool ToClient(float mx, float my, POINT& pt);

void SpeakFocus(bool interrupt) {
    if (g_items.empty()) return;
    MenuSay(g_items[g_focus].label, interrupt);
    HoverFocus();
}

std::wstring GadgetLabel(const Gadget& g) {
    switch (g.kind) {
        case Gadget::Combo:
            return (!g.rule.empty() ? g.rule + L": " : !g.column.empty() ? g.column + L": " : L"") + (g.text.empty() ? L"пусто" : g.text == L"-" ? L"нет" : g.text) +
                   L", список";
        case Gadget::CheckBox:
            return (!g.rule.empty() ? g.rule : g.text.empty() ? L"без названия" : g.text) + L", флажок";
        case Gadget::Entry: return (g.text.empty() ? L"пусто" : g.text) + L", поле ввода";
        case Gadget::Slider: return L"ползунок " + g.text;
        default: return g.text;
    }
}

// Groups (Tab stops) of the current rebuild: 0 = APT buttons, then one per kind of gadget
// area. Numbered by first appearance, keyed so the same list keeps its number.
std::vector<std::pair<uintptr_t, std::wstring>> g_groups;

int GroupFor(uintptr_t key, const std::wstring& name) {
    for (size_t i = 0; i < g_groups.size(); ++i)
        if (g_groups[i].first == key) return (int)i;
    g_groups.push_back({key, name});
    return (int)g_groups.size() - 1;
}

std::map<int, uint64_t> g_lastInGroup;  // group -> key of the item last focused there

int CountInGroup(int group) {
    int n = 0;
    for (const auto& it : g_items) n += it.group == group;
    return n;
}

uint64_t Key(const Item& it) { return it.cih ? it.cih : ((uint64_t)it.window << 20) + (uint64_t)(it.row + 1); }

// Scenario name in the setup screen's combo -> {lotr.str key found, its description}.
// Keys: aotr\data\ini\campaigns\scenarios\*.inc, DisplayName and DisplayDescription.
std::pair<std::wstring, std::wstring> ScenarioDescription(const std::wstring& shown) {
    static const char* const kScenarios[][2] = {
        {"LWScenario:WOTRScenario001", "LWScenario:WOTRScenarioPaperOpenDescription"},
        {"LWScenario:AOTRScenarioWarNorth", "LWScenario:AOTRScenarioWarNorthDescription"},
        {"LWScenario:AOTRScenarioWarSouth", "LWScenario:AOTRScenarioWarSouthDescription"},
        {"LWScenario:AOTRScenarioWarNorthOpen", "LWScenario:WOTRScenario001Description"},
        {"LWScenario:AOTRScenarioWarSouthOpen", "LWScenario:WOTRScenario001Description"},
        {"LWScenario:AOTRScenarioLastAlliance", "LWScenario:AOTRScenarioLastAllianceDescription"},
        {"LWScenario:AOTRScenarioWarDwarfOrc", "LWScenario:AOTRScenarioWarDwarfOrcDescription"},
        {"LWScenario:WOTRScenarioBFME2Good", "LWScenario:WOTRScenarioBFME2GoodDescription"},
        {"LWScenario:WOTRScenarioBFME2Evil", "LWScenario:WOTRScenarioBFME2EvilDescription"},
        {"LWScenario:WOTRScenarioAngmarCampaign", "LWScenario:WOTRScenarioAngmarCampaignDescription"}};
    for (const auto& s : kScenarios)
        if (LookupStr(s[0]) == shown) return {shown, Clean(LookupStr(s[1]))};
    return {};
}

// A row of the game's map list as the choice shows it: name first, the state last.
PickRow MapPickRow(const Gadget& g) {
    PickRow r;
    r.row = g.row;
    std::wstring t = g.text;
    bool done = t.rfind(L"пройдена, ", 0) == 0;
    size_t comma = t.find(L", ");
    if (done || t.rfind(L"не пройдена, ", 0) == 0) t = t.substr(comma + 2);
    size_t last = t.rfind(L", ");
    if (last != std::wstring::npos && last + 2 < t.size() && iswdigit(t[last + 2])) {
        r.players = _wtoi(t.c_str() + last + 2);
        t.resize(last);
    }
    while (!t.empty() && (t.back() == L'*' || t.back() == L' ')) t.pop_back();
    r.name = t;
    const MapInfo* map = FindMapInRow(g.text);
    auto starts = [&](const wchar_t* p) { return t.rfind(p, 0) == 0; };
    r.kind = starts(L"Fortress:") ? 2 : starts(L"Besiege:") ? 3 : starts(L"Alternate Faction:") ? 4 : 1;
    if ((map && map->adventure) || starts(L"Tales of")) r.kind = 5;
    if (r.players) r.name += L", " + std::to_wstring(r.players) + L" игр.";
    if (map && map->adventure) r.name += L", приключение";
    if (map && !map->replaced.empty()) {
        r.name += L", играть за " + map->replaced;
        r.army = map->replaced;
    }
    if (done) r.name += L", пройдена";
    if (map) r.details = (map->role.empty() ? std::wstring() : map->role + L". ") + map->description;
    return r;
}

// Parts of the War of the Ring HUD that are no controls: panel frames and their toggles.
bool WorldMapNoise(const Item& i) {
    std::string p = Join(i.path);
    for (const char* part : {".main.stats", ".main.globe.regionStats", ".main.checklist.expandButton", ".theBar."})
        if (p.find(part) != std::string::npos) return true;
    for (const char* whole : {".main.checklist.?", ".main.selectionDetails.?"})
        if (p.size() >= strlen(whole) && p.compare(p.size() - strlen(whole), std::string::npos, whole) == 0) return true;
    return i.label == L"Bttn Frame";
}

// Hit areas of a dialog's panel, not controls: the focus landed on them instead of OK (log
// 2026-11-05 01:08:49) and on the veterancy list's frame and scroll bar instead of RESUME (01:47:45).
bool DialogNoise(const Item& i) {
    std::string p = Join(i.path);
    for (const char* part : {"bttnDisabled", ".Veterancy.instance"})
        if (p.find(part) != std::string::npos) return true;
    // ".MessageBox.?": the save dialog's box frame, two of them, spoken "Message Box" over and over
    // while "Saving..." showed (log 2026-11-09 01:14:47-01:15:07); its text comes from screentext.
    for (const char* whole : {".StrategicMessageBox.?", ".Main.Veterancy.?", ".MessageBox.?"})
        if (p.size() >= strlen(whole) && p.compare(p.size() - strlen(whole), std::string::npos, whole) == 0) return true;
    return false;
}

// Options.apt: its sliders and check boxes are window gadgets with no captions of their own, and
// the captions are movie text. Places in movie pixels (log 2026-11-04 20:52:31). The sliders are
// proven by their values against Options.ini: 50 Brightness, 29 ScrollFactor 30, 20 Music, 100 SFX,
// 80 Voice, 30 Ambient, 80 Movie. The rest stand on one row with their tooltip buttons.
std::wstring OptionsCaption(float x, float y) {
    static const struct { float x, y; const char* key; const wchar_t* words; } kPlaces[] = {
        {215.7f, 436.6f, "APT:Brightness", nullptr},  {215.7f, 583.8f, "APT:ScrollSpeed", nullptr},
        {512.0f, 268.1f, "APT:Music", nullptr},       {512.0f, 323.6f, "APT:SoundFx", nullptr},
        {512.0f, 379.0f, "APT:Voice", nullptr},       {512.0f, 434.5f, "APT:Ambient", nullptr},
        {512.0f, 489.2f, "APT:Movie", nullptr},       {436.3f, 541.9f, "APT:EAX3", nullptr},
        {436.3f, 578.8f, nullptr, L"High Audio Quality"}, {721.1f, 339.9f, "APT:SendDelay", nullptr},
    };
    for (const auto& p : kPlaces)
        if (std::fabs(p.x - x) < 12.f && std::fabs(p.y - y) < 12.f) return p.key ? LookupStr(p.key) : p.words;
    return {};
}

void OpenPicker(const Item& it);

// SpellStore.Buttons.SpellN: the faction's spell, its cost and tooltip (spells.cpp).
void SpellStoreLabel(Item& it) {
    if (std::find(it.path.begin(), it.path.end(), "SpellStore") == it.path.end()) return;
    for (const auto& name : it.path) {
        if (name.size() < 6 || name.compare(0, 5, "Spell") != 0 || !isdigit((unsigned char)name[5])) continue;
        std::wstring label, details;
        int slot = atoi(name.c_str() + 5);
        bool skirmish = ShellHasScreen("Skirmish.apt");
        if (SpellStoreSlot(slot, skirmish, label, details)) {
            it.label = label;
            it.details = details;
        } else {
            // The store's own button: the campaign's Fellowship side has no set the mod can find.
            uint32_t button = SpellStoreButton(slot), name = 0;
            std::string bname = button && SafeReadU32(button + 0x10, name) ? game::ReadAsciiString(name) : std::string();
            if (!bname.empty() && SpellStoreButtonWords(bname, skirmish, label, details)) {
                it.label = label;
                it.details = details;
            }
            static std::set<int> logged;
            if (logged.insert(slot).second)
                LogLine(L"spells: store slot " + std::to_wstring(slot) + L" button " + Widen(bname.c_str()) + L" -> " + it.label);
        }
        it.spellSlot = slot;
        it.sciences = SpellStoreSciences(slot);
        return;
    }
}

// Spell store buttons that the player owns already say so instead of their cost; the points left
// are spoken once when the store's list arrives (store opened). Returns the points, -1 unread.
int MarkOwnedSpells(std::vector<Item>& items) {
    std::vector<int> first;
    std::vector<size_t> at;
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].spellSlot > 0 && !items[i].sciences.empty()) first.push_back(items[i].sciences[0]), at.push_back(i);
    if (first.empty()) return -1;
    int points = -1;
    std::vector<int> owned = SciencesOwned(first, points);
    std::wstring log = L"spells: store points " + std::to_wstring(points) + L", owned:";
    for (size_t k = 0; k < at.size() && k < owned.size(); ++k) {
        if (owned[k] != 1) continue;
        Item& it = items[at[k]];
        size_t comma = it.label.rfind(L", ");
        it.label = (comma == std::wstring::npos ? it.label : it.label.substr(0, comma)) + L", куплено";
        log += L" " + std::to_wstring(it.spellSlot);
    }
    LogLine(log);
    return points;
}

// Enter on a spell: the store's own click only marks it until Accept, a close without Accept clears the
// marks, and the Enter key closes the store (log 2026-11-08 03:02). A message of the mod's own sent while
// the store was open was not bought either (log 2026-11-14 23:18). So the mod chooses the spell the way
// the store's click does and presses Accept: the store buys it as it closes, as for a mouse player.
void BuySpell(const Item& it) {
    size_t comma = it.label.rfind(L", ");
    std::wstring name = comma == std::wstring::npos ? it.label : it.label.substr(0, comma);
    auto r = std::make_shared<StoreChoice>();
    StoreChooseAndAccept(it.spellSlot, r);
    switch (r->result) {
    case StoreChoice::kOwned: MenuSay(L"Уже куплено: " + name, true); return;
    case StoreChoice::kPending: MenuSay(L"Уже выбрано: " + name, true); return;
    case StoreChoice::kClosing: MenuSay(L"Магазин уже закрывается", true); return;
    case StoreChoice::kTooDear:
        MenuSay(L"Не хватает очков на " + name + L": нужно " + std::to_wstring(r->cost) + L", есть " +
                    std::to_wstring(r->points), true);
        return;
    case StoreChoice::kNoPrereq: {
        size_t pre = it.details.find(L"Сначала нужно:");
        MenuSay(L"Пока нельзя: " + name + L". " +
                    (pre == std::wstring::npos ? L"Сначала нужно купить другие силы." : it.details.substr(pre)), true);
        return;
    }
    case StoreChoice::kChosen: break;
    default: MenuSay(L"Покупка не удалась", true); return;
    }
    // The store closes and buys; the purchase reaches the player on a logic frame after that.
    int left = -1;
    bool got = false;
    for (int i = 0; i < 30 && !got; ++i) {
        Sleep(100);
        std::vector<int> owned = SciencesOwned({r->science}, left);
        got = !owned.empty() && owned[0] == 1;
    }
    LogLine(L"spells: science " + std::to_wstring(r->science) + (got ? L" owned now" : L" NOT owned after 3 s") +
            L", points " + std::to_wstring(left));
    MenuSay(got ? L"Куплено: " + name + L". Осталось очков: " + std::to_wstring(left) + L". Магазин закрыт"
                : L"Выбрано и подтверждено: " + name + L". Игра купит его, когда закроет магазин",
            true);
}

void Rebuild(const game::BilEntry* bil, int n, const std::vector<Gadget>& gadgets, bool screenChanged) {
    g_mapRows.clear();
    uint64_t keepKey = g_items.empty() ? 0 : Key(g_items[g_focus]);
    std::string keepPath = g_items.empty() ? std::string() : Join(g_items[g_focus].path);
    std::wstring keepLabel = g_items.empty() ? std::wstring() : g_items[g_focus].label;
    std::vector<uint64_t> before;
    for (const auto& it : g_items) before.push_back(Key(it));
    std::string movie = Stem(g_screenName);
    std::vector<Item> items;
    g_groups.clear();
    GroupFor(0, L"кнопки");
    GroupFor(1, L"настройки");
    GroupFor(2, L"правила");
    GroupFor(3, L"выбор");
    for (int i = 0; i < n; ++i) {
        uint32_t typeWord = 0;
        if (!SafeReadU32(bil[i].cih + 4, typeWord) || ((int)typeWord >> 25) != game::kAptTypeButton) continue;
        if (std::fabs(bil[i].a) < 0.01f || std::fabs(bil[i].d) < 0.01f) continue;  // squashed: hidden
        Item it;
        it.cih = bil[i].cih;
        it.path = PathOf(bil[i].cih);
        it.label = LabelFor(it.path, movie);
        SpellStoreLabel(it);
        it.hasBounds = HitCentre(bil[i], it.x, it.y);
        if (!it.hasBounds) { it.x = bil[i].tx; it.y = bil[i].ty; }
        if (it.label.empty()) {  // unnamed: a background or logo, not a control
            wchar_t b[64];
            swprintf_s(b, L"  skipped unnamed (%.1f,%.1f) ", it.x, it.y);
            LogLine(b + Widen(Join(it.path).c_str()));
            continue;
        }
        items.push_back(it);
    }
    // A tab is drawn twice: its button (TabButtons.MapTab.bttn = "Map") and, while it is open,
    // its header clip (TabMap = "MAP") at almost the same place. Clicking the header did
    // nothing (log 2026-11-01 04:55-04:57, three clicks on TabRules): keep the button.
    auto lower = [](std::wstring t) { for (auto& c : t) c = towlower(c); return t; };
    auto isBttn = [](const Item& i) { return !i.path.empty() && i.path[0] == "bttn"; };
    for (size_t a = 0; a < items.size(); ++a)
        for (size_t b = 0; b < items.size(); ++b) {
            if (a == b || items[a].label.empty() || items[b].label.empty()) continue;
            if (lower(items[a].label) != lower(items[b].label)) continue;
            if (std::fabs(items[a].x - items[b].x) > 30.f || std::fabs(items[a].y - items[b].y) > 30.f) continue;
            if (isBttn(items[a]) && !isBttn(items[b])) items[b].label.clear();
        }
    items.erase(std::remove_if(items.begin(), items.end(), [&movie](const Item& i) {
                    // Options: a tooltip button only marks the row of the gadget beside it.
                    bool tip = movie == "Options" && !i.path.empty() && i.path[0].rfind("tooltips", 0) == 0;
                    // The score table's own hit area, not a sort header (log 2026-11-05 12:41:38).
                    bool score = movie == "TimeLine" && Join(i.path).find(".OnSort") != std::string::npos;
                    return i.label.empty() || tip || score || DialogNoise(i);
                }), items.end());
    bool hadSpells = std::any_of(g_items.begin(), g_items.end(), [](const Item& i) { return i.spellSlot > 0; });
    int spellPoints = MarkOwnedSpells(items);
    // Window gadgets: client pixels -> movie coordinates (the movie is stretched to the window).
    RECT rc{0, 0, 1024, 768};
    if (g_wnd) GetClientRect(g_wnd, &rc);
    float sx = 1024.f / (rc.right > 0 ? rc.right : 1024), sy = 768.f / (rc.bottom > 0 ? rc.bottom : 768);
    std::wstring chosenMap;
    const MapInfo* chosenInfo = nullptr;
    std::vector<size_t> startButtons;
    Item mapItem;
    for (const auto& g : gadgets) {
        // The map list goes to its own choice: 368 rows are no place to walk through by Tab.
        if (g.kind == Gadget::ListRow && !g.inCombo && g.total > 50) {
            PickRow r = MapPickRow(g);
            if (g.selected) chosenMap = r.name, chosenInfo = FindMapInRow(g.text);
            if (g_mapRows.empty()) {
                g_mapList = g.window;
                mapItem.window = g.window;
                mapItem.path = {"#mapchoice"};
                mapItem.picker = 1;
                mapItem.group = 3;
                mapItem.hasBounds = true;
            }
            g_mapRows.push_back(r);
            continue;
        }
        Item it;
        it.window = g.window;
        it.row = g.row;
        char id[32];
        sprintf_s(id, "#w%08X:%d", (unsigned)g.window, g.row);
        it.path = {id};
        it.label = GadgetLabel(g);
        if (g.kind == Gadget::ListRow) {
            const MapInfo* map = FindMapInRow(g.text);
            if (map) {
                it.details = map->description;
                if (!map->role.empty()) it.details = map->role + L". " + it.details;
                if (map->adventure) it.label += L", приключение";
                if (!map->replaced.empty()) it.label += L", играть за " + map->replaced;
                if (map->players > 0) it.details += L" Игроков: " + std::to_wstring(map->players) + L".";
            }
        }
        if (g.kind == Gadget::ListRow && g.selected) it.label += L", выбрано";
        it.cx = g.cx;
        it.cy = g.cy;
        it.total = g.total;
        it.x = g.cx * sx;
        // Rows out of view keep their order but must not join another column or group:
        // their x is the list's, and their y follows the row order.
        it.y = g.cy * sy;
        it.hasBounds = g.visible;
        if (movie == "Options" && (g.kind == Gadget::Slider || g.kind == Gadget::CheckBox)) {
            std::wstring caption = OptionsCaption(it.x, it.y);
            if (!caption.empty()) it.label = caption + (g.kind == Gadget::CheckBox ? L", флажок" : L", " + it.label);
        }
        if (g.kind == Gadget::ListRow) {
            it.group = GroupFor(g.window, g.inCombo ? L"варианты" : g.total <= 3 ? L"описание" : L"список");
            it.inCombo = g.inCombo;
        } else if (g.kind == Gadget::Combo && !ScenarioDescription(g.text).first.empty()) {
            it.picker = 2;
            it.group = 3;
            std::wstring sum = WotrScenarioSummary(g.text);
            it.label = L"Сценарий: " + g.text + (sum.empty() ? L"" : L", " + sum) + L", Enter: выбрать другой";
            // The quick start stands first in the choice group: scenario, armies, sides, PLAY.
            Item quick = it;
            quick.picker = 3;
            quick.y -= 1.f;
            quick.path = {"#wotrquick"};
            quick.row = -2;  // its own key: the scenario item has the same window
            quick.label = L"Быстрый старт Войны Кольца: сценарий, ваша армия, противники, и в бой. Enter";
            items.push_back(quick);
        } else {
            it.group = g.rule.empty() ? GroupFor(1, L"настройки") : GroupFor(2, L"правила");
        }
        if (g.kind == Gadget::Button && (g.text == L"Starting Position" || g.text.rfind(L"Start Position for player", 0) == 0))
            startButtons.push_back(items.size());
        items.push_back(it);
    }
    NameStartButtons(items, startButtons, chosenInfo);
    if (mapItem.picker) {
        std::wstring role = chosenInfo && !chosenInfo->role.empty() ? L". " + chosenInfo->role + L". " : L", ";
        mapItem.label = L"Карта: " + (chosenMap.empty() ? std::wstring(L"не выбрана") : chosenMap) + role + L"Enter: выбрать другую";
        // The skirmish quick start stands first in the choice group: map, armies, sides, PLAY.
        Item quick = mapItem;
        quick.picker = 4;
        quick.y -= 1.f;
        quick.path = {"#skirmishquick"};
        quick.row = -2;
        quick.label = L"Быстрый старт схватки: карта, ваша армия, противники, и в бой. Enter";
        items.push_back(quick);
        items.push_back(mapItem);
    }
    if (g_worldMap)
        items.erase(std::remove_if(items.begin(), items.end(), [](const Item& i) { return WorldMapNoise(i); }), items.end());
    // Groups in a stable order: buttons, settings, then lists (open drop-downs last).
    // Inside a group, columns: sorted by x, a new column after a gap of more than 60 movie pixels.
    std::sort(items.begin(), items.end(), [](const Item& l, const Item& r) {
        return l.group != r.group ? l.group < r.group : l.x < r.x;
    });
    for (size_t i = 1; i < items.size(); ++i)
        items[i].column = items[i].group != items[i - 1].group
                              ? 0
                              : items[i - 1].column + (items[i].x - items[i - 1].x > 60.f ? 1 : 0);
    std::stable_sort(items.begin(), items.end(), [](const Item& l, const Item& r) {
        if (l.group != r.group) return l.group < r.group;
        return l.column != r.column ? l.column < r.column : l.y < r.y;
    });
    // Battle HUD parents (log listings 2026-10-31..11-01). A screen pushed over the battle
    // (QuitMenu.apt on Escape, SpellStore.apt the spell book, SaveLoad.apt) shares the button
    // list with the HUD: then only its own buttons are kept and the menu keys work.
    auto isHud = [](const Item& it) {
        static const char* kHud[] = {"PalantirButtons", "SpellBookUI", "ResourceBar", "SideCommandBar",
                                     "CommandButtons", "CommandUI", "PalantirBack", "HeroSelectUI"};
        for (const auto& name : it.path)
            for (const char* h : kHud)
                if (name == h) return true;
        return false;
    };
    // A screen pushed over another (QuitMenu.apt over the War of the Ring map, whose "main.*"
    // HUD is not in kHud; log 2026-11-01 06:58) shares the button list with the screen below:
    // buttons that were already there with fewer screens on the shell belong to it and are dropped.
    for (auto it = g_shownAt.begin(); it != g_shownAt.end();)
        it = it->first >= g_shellCount ? g_shownAt.erase(it) : std::next(it);
    if (g_shellCount > 1) {
        std::set<uintptr_t> below;
        for (const auto& kv : g_shownAt) below.insert(kv.second.begin(), kv.second.end());
        if (!below.empty())
            items.erase(std::remove_if(items.begin(), items.end(), [&](const Item& i) { return i.cih && below.count(i.cih); }),
                        items.end());
    }
    bool wasInGame = g_inGame, wasOverlay = g_overlay;
    g_inGame = std::any_of(items.begin(), items.end(), isHud);
    if (g_inGame && !wasInGame) g_battleCount = g_shellCount;
    g_overlay = g_inGame && g_shellCount > g_battleCount;
    if (g_overlay) items.erase(std::remove_if(items.begin(), items.end(), isHud), items.end());
    g_items = items;
    auto& shownHere = g_shownAt[g_shellCount];
    for (const auto& i : g_items)
        if (i.cih) shownHere.insert(i.cih);
    if (g_inGame != wasInGame || g_overlay != wasOverlay)
        LogLine(g_overlay ? L"menu: screen over the battle, menu keys on"
                          : g_inGame ? L"menu: battle HUD, menu keys off" : L"menu: menu keys on");
    if (g_inGame && !g_overlay) return;
    if (g_overlay && !wasOverlay) screenChanged = true;

    // Focus: the same button if it is still there; else the first new button
    // (a sub-menu that just opened); else the first button.
    g_focus = 0;
    int keepAt = -1, firstNew = -1;
    for (size_t i = 0; i < g_items.size(); ++i) {
        if (Key(g_items[i]) == keepKey || (!keepPath.empty() && Join(g_items[i].path) == keepPath)) {
            if (keepAt < 0) keepAt = (int)i;
        }
        if (firstNew < 0 && std::find(before.begin(), before.end(), Key(g_items[i])) == before.end())
            firstNew = (int)i;
    }
    bool focusMoved = false;
    // A new map brings new start position buttons: they must not cut off its description.
    bool hold = (int)(g_holdFocus - GetTickCount()) > 0 && !screenChanged;
    if (hold) g_focus = keepAt >= 0 ? keepAt : 0;
    else if (screenChanged) { g_focus = 0; g_lastInGroup.clear(); }
    else if (firstNew >= 0 && !before.empty()) { g_focus = firstNew; focusMoved = true; }
    else if (keepAt >= 0) g_focus = keepAt;
    else focusMoved = true;

    // One write per rebuild, items listed in full only when the screen changed: writing
    // 400 flushed lines on every list change made the map list lag.
    std::vector<std::wstring> lines;
    lines.push_back(L"menu: " + Widen(g_screenName.c_str()) + L" " + std::to_wstring(g_items.size()) + L" items");
    for (size_t i = 0; i < g_items.size(); ++i) {
        if (!screenChanged && lines.size() > 12 && !g_worldMap) break;
        if (g_items[i].row > 20) continue;  // long lists: first rows only
        wchar_t b[96];
        swprintf_s(b, L"  [%zu] g%d col %d (%.1f,%.1f)%s ", i, g_items[i].group, g_items[i].column, g_items[i].x,
                   g_items[i].y, g_items[i].hasBounds ? L"" : L" origin");
        lines.push_back(b + Widen(Join(g_items[i].path).c_str()) + L" = \"" + g_items[i].label + L"\"");
    }
    LogLines(lines);

    if (screenChanged) {
        std::wstring summary = ScreenLabel(Stem(g_screenName));
        for (size_t gi = 0; gi < g_groups.size(); ++gi) {
            int c = CountInGroup((int)gi);
            if (c) summary += L", " + g_groups[gi].second + L" " + std::to_wstring(c);
        }
        MenuSay(summary, true);
        SpeakFocus(false);
    } else if (focusMoved && !hold) {
        SpeakFocus(true);
    } else if (!hold && !g_items.empty() && g_items[g_focus].label != keepLabel) {
        SpeakFocus(true);  // the focused control changed its value (a list choice was made)
    }
    if (spellPoints >= 0 && !hadSpells) MenuSay(L"Очков силы: " + std::to_wstring(spellPoints), false);
}

// Hover: the mouse goes to the focused button; what the game then shows is its tooltip.
void HoverFocus() {
    if (!g_worldMap || g_items.empty()) return;
    const Item& it = g_items[g_focus];
    POINT pt;
    if (!it.cih || !ToClient(it.x, it.y, pt)) return;
    SetCursorPos(pt.x, pt.y);
    auto known = g_hoverText.find(Key(it));
    if (known != g_hoverText.end()) { MenuSay(known->second, false); g_hoverAt = 0; return; }
    g_hoverAt = GetTickCount();
    g_hoverKey = Key(it);
}

void HoverTick() {
    if (!g_hoverAt || GetTickCount() - g_hoverAt < 800) return;
    std::wstring all;
    for (const std::wstring& t : ScreenTextSince(g_hoverAt)) {
        if (t.find(L".apt") != std::wstring::npos) continue;
        all += (all.empty() ? L"" : L" ") + Clean(t);
    }
    g_hoverAt = 0;
    if (all.empty()) return;
    g_hoverText[g_hoverKey] = all;
    LogLine(L"menu: tooltip: " + all);
    if (!g_items.empty() && Key(g_items[g_focus]) == g_hoverKey) MenuSay(all, false);
}

void PickRebuild() {
    g_pick.shown.clear();
    for (size_t i = 0; i < g_pick.rows.size(); ++i) {
        const PickRow& r = g_pick.rows[i];
        if ((g_pick.group == 0 || r.kind == g_pick.group) && (!g_pick.players || r.players == g_pick.players))
            g_pick.shown.push_back((int)i);
    }
    g_pick.pos = 0;
}

void PickSay(bool interrupt) {
    if (g_pick.shown.empty()) { Speak(L"Пусто", interrupt); return; }
    const PickRow& r = g_pick.rows[g_pick.shown[g_pick.pos]];
    Speak(r.name + L", " + std::to_wstring(g_pick.pos + 1) + L" из " + std::to_wstring(g_pick.shown.size()), interrupt);
}

void OpenPicker(const Item& it) {
    Picker p;
    p.kind = it.picker;
    if (it.picker == 1) {
        p.list = g_mapList;
        p.rows = g_mapRows;
        auto low = [](std::wstring s) { for (auto& c : s) c = towlower(c); return s; };
        std::sort(p.rows.begin(), p.rows.end(), [&](const PickRow& a, const PickRow& b) { return low(a.name) < low(b.name); });
    } else {
        std::vector<std::wstring> texts;
        if (!ComboChoices(it.window, texts, p.list)) { Speak(L"Список сценариев не прочитан", true); return; }
        for (size_t i = 0; i < texts.size(); ++i) {
            PickRow r;
            r.row = (int)i;
            r.name = texts[i];
            std::wstring sum = WotrScenarioSummary(texts[i]);
            r.details = (sum.empty() ? L"" : sum + L". ") + ScenarioDescription(texts[i]).second;
            p.rows.push_back(r);
        }
    }
    if (p.rows.empty()) { Speak(L"Выбирать не из чего", true); return; }
    g_pick = p;
    g_pick.open = true;
    PickRebuild();
    int sel = ListSelection(p.list);
    for (size_t i = 0; i < g_pick.shown.size(); ++i)
        if (g_pick.rows[g_pick.shown[i]].row == sel) g_pick.pos = (int)i;
    LogLine(L"menu: choice opened, kind " + std::to_wstring(p.kind) + L", " + std::to_wstring(p.rows.size()) + L" rows, list selection " +
            std::to_wstring(sel));
    std::wstring hint = L". Вверх и вниз: строки, буква: переход, Shift+пробел: описание, Enter: выбрать, апостроф: выйти.";
    if (p.kind == 1) hint += L" Влево и вправо: виды карт, цифра: число игроков.";
    Speak((p.kind == 1 ? L"Выбор карты" : L"Выбор сценария") + hint, true);
    PickSay(false);
}

// The story scenarios want one army: their descriptions say "Choose Rivendell", and with another
// the player stands on the wrong side (Mordor in War in the North, log 2026-11-05 01:03).
void SetArmy(const std::wstring& army, const std::wstring& note) {
    Sleep(600);  // the setup screen refills its combos for the new scenario or map
    std::vector<Gadget> gadgets;
    CollectGadgets(gadgets);
    std::wstring header = LookupStr("APT:HeaderFaction");
    for (const Gadget& g : gadgets) {
        if (g.kind != Gadget::Combo || g.column != (header.empty() ? L"Army" : header) + L" 1") continue;
        std::vector<std::wstring> rows;
        uintptr_t list = 0;
        if (!ComboChoices(g.window, rows, list)) break;
        for (size_t i = 0; i < rows.size(); ++i)
            if (rows[i] == army && SelectListRow(list, (int)i)) {
                LogLine(L"menu: army set to " + army);
                Speak(L"Армия: " + army + L". " + note, false);
                return;
            }
    }
    LogLine(L"menu: army not set: " + army);
    Speak(L"Выберите армию сами: " + army, false);
}

void StoryArmy(const std::wstring& scenario) {
    static const char* const kStory[][2] = {{"LWScenario:WOTRScenarioBFME2Good", "Rivendell"},
                                            {"LWScenario:WOTRScenarioBFME2Evil", "Mordor"},
                                            {"LWScenario:WOTRScenarioAngmarCampaign", "Dol Guldur"}};
    for (const auto& s : kStory)
        if (LookupStr(s[0]) == scenario) SetArmy(Widen(s[1]), L"Потеря героя в миссии означает поражение.");
}

void ChoosePicked() {
    const PickRow r = g_pick.rows[g_pick.shown[g_pick.pos]];
    uintptr_t list = g_pick.list;
    g_pick.open = false;
    g_holdFocus = GetTickCount() + 4000;
    bool ran = SelectListRow(list, r.row);
    Sleep(150);
    int sel = ListSelection(list);
    LogLine(L"menu: chose row " + std::to_wstring(r.row) + L", selection now " + std::to_wstring(sel) + L": " + r.name);
    if (!ran || sel != r.row) { Speak(L"Не выбралось: " + r.name, true); return; }
    Speak(L"Выбрано: " + r.name + L". " + r.details, true);
    if (g_pick.kind == 2) StoryArmy(r.name);
    else if (!r.army.empty()) SetArmy(r.army, L"");
}

// The skirmish quick start gets the map list as the map choice reads it.
void OpenSkirmishQuick() {
    std::vector<SetupMap> maps;
    for (const PickRow& r : g_mapRows) {
        SetupMap m;
        m.name = r.name, m.details = r.details, m.army = r.army;
        m.row = r.row, m.kind = r.kind, m.players = r.players;
        maps.push_back(m);
    }
    SkirmishSetupOpen(g_mapList, maps);
}

void PickerKeys() {
    bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    int n = (int)g_pick.shown.size();
    if (Pressed(VK_OEM_7) || Pressed(VK_BACK)) { g_pick.open = false; Speak(L"Выбор закрыт", true); return; }
    int move = Pressed(VK_DOWN) ? 1 : Pressed(VK_UP) ? -1 : Pressed(VK_NEXT) ? 10 : Pressed(VK_PRIOR) ? -10 : 0;
    if (move && n) {
        int to = g_pick.pos + move;
        g_pick.pos = to < 0 ? (std::abs(move) > 1 ? 0 : n - 1) : to >= n ? (std::abs(move) > 1 ? n - 1 : 0) : to;
        PickSay(true);
    }
    if (Pressed(VK_HOME) && n) { g_pick.pos = 0; PickSay(true); }
    if (Pressed(VK_END) && n) { g_pick.pos = n - 1; PickSay(true); }
    bool left = Pressed(VK_LEFT), right = Pressed(VK_RIGHT);
    if ((left || right) && g_pick.kind == 1) {
        int count = (int)(sizeof kPickKinds / sizeof *kPickKinds);
        for (int step = 1; step <= count; ++step) {
            int g = ((g_pick.group + (right ? step : -step)) % count + count) % count;
            bool any = g == 0;
            for (const PickRow& r : g_pick.rows) any |= r.kind == g;
            if (!any) continue;
            g_pick.group = g;
            PickRebuild();
            Speak(std::wstring(kPickKinds[g]) + L", " + std::to_wstring(g_pick.shown.size()), true);
            PickSay(false);
            break;
        }
    }
    for (int d = 0; d <= 8 && g_pick.kind == 1; ++d) {
        if (!Pressed('0' + d)) continue;
        g_pick.players = d;
        PickRebuild();
        Speak((d ? L"Карты на " + std::to_wstring(d) + L" игроков, " : std::wstring(L"Любое число игроков, ")) +
              std::to_wstring(g_pick.shown.size()), true);
        PickSay(false);
    }
    n = (int)g_pick.shown.size();
    for (int vk = 'A'; vk <= 'Z' && !ctrlDown; ++vk) {
        if (!Pressed(vk)) continue;
        for (int step = 1; step <= n; ++step) {
            int i = (g_pick.pos + step) % n;
            std::wstring name = g_pick.rows[g_pick.shown[i]].name;
            if (!name.empty() && (wchar_t)towupper(name[0]) == (wchar_t)vk) { g_pick.pos = i; PickSay(true); return; }
        }
        Speak(L"Нет на эту букву", true);
        return;
    }
    if (Pressed(VK_SPACE) && n) {
        const PickRow& r = g_pick.rows[g_pick.shown[g_pick.pos]];
        if (shift) Speak(r.details.empty() ? L"нет описания" : r.details, true);
        else PickSay(true);
    }
    if (Pressed(VK_RETURN) && n) ChoosePicked();
}

// Left/Right: the nearest button by height in the next column in that direction.
int NeighbourColumn(int dir) {
    const Item& cur = g_items[g_focus];
    int best = -1, bestCol = 0;
    float bestDy = 0;
    for (size_t i = 0; i < g_items.size(); ++i) {
        if (g_items[i].group != cur.group) continue;
        int col = g_items[i].column;
        if ((dir > 0 && col <= cur.column) || (dir < 0 && col >= cur.column)) continue;
        float dy = std::fabs(g_items[i].y - cur.y);
        bool closerCol = best < 0 || (dir > 0 ? col < bestCol : col > bestCol);
        bool sameCol = best >= 0 && col == bestCol;
        if (closerCol || (sameCol && dy < bestDy)) { best = (int)i; bestCol = col; bestDy = dy; }
    }
    return best;
}

bool ToClient(float mx, float my, POINT& pt) {
    RECT rc;
    if (!g_wnd || !GetClientRect(g_wnd, &rc)) return false;
    float w = (float)(rc.right - rc.left), h = (float)(rc.bottom - rc.top);
    float sx, sy, ox = 0, oy = 0;
    if (g_mapMode == 0) {
        sx = sy = h / 768.f;
        ox = (w - 1024.f * sx) / 2;
    } else {
        sx = w / 1024.f;
        sy = h / 768.f;
    }
    pt.x = (LONG)std::lround(ox + mx * sx);
    pt.y = (LONG)std::lround(oy + my * sy);
    return ClientToScreen(g_wnd, &pt) != 0;
}

void LeftClickAt(POINT pt) {
    SetCursorPos(pt.x, pt.y);
    Sleep(150);
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_MOUSE;
    in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &in[0], sizeof(INPUT));
    Sleep(80);
    SendInput(1, &in[1], sizeof(INPUT));
}

// Window gadgets are clicked in client pixels straight from the window tree; a list row
// out of view is scrolled into view first.
void ClickGadget(const Item& it) {
    // A list row is chosen through the game's own list code: a click depends on the scroll,
    // and writing the scroll position misses rows far down the map list.
    if (it.row >= 0 && !it.inCombo && SelectListRow(it.window, it.row)) {
        Sleep(100);
        int sel = ListSelection(it.window);
        LogLine(L"menu: list row " + std::to_wstring(it.row) + L" set, selection now " + std::to_wstring(sel) +
                L": " + it.label);
        MenuSay(sel == it.row ? L"Выбрано" : L"Строка не выбралась", true);
        return;
    }
    float cx = it.cx, cy = it.cy;
    if (it.row >= 0 && !PrepareListRowClick(it.window, it.row, cx, cy)) {
        LogLine(L"menu: cannot place list row " + std::to_wstring(it.row));
        return;
    }
    POINT pt{(LONG)std::lround(cx), (LONG)std::lround(cy)};
    POINT client = pt;
    if (!g_wnd || !ClientToScreen(g_wnd, &pt)) return;
    if (it.row >= 0) LogLine(L"menu: list before click: " + ListDiagnostics(it.window));
    LeftClickAt(pt);
    wchar_t b[160];
    swprintf_s(b, L"menu: gadget click [%d] client (%ld,%ld) row %d ", g_focus, client.x, client.y, it.row);
    LogLine(b + Widen(Join(it.path).c_str()));
    if (it.row >= 0) {
        Sleep(100);
        int sel = ListSelection(it.window);
        LogLine(L"menu: list after click: " + ListDiagnostics(it.window));
        LogLine(L"menu: list selection after click = " + std::to_wstring(sel) +
                (sel == it.row ? L" (as aimed)" : L" (NOT the aimed row)"));
    }
}

void Click() {
    if (g_items.empty()) return;
    const Item& it = g_items[g_focus];
    if (it.picker == 3) { WotrSetupOpen(it.window); return; }
    if (it.picker == 4) { OpenSkirmishQuick(); return; }
    if (it.picker) { OpenPicker(it); return; }
    if (it.spellSlot > 0) { BuySpell(it); return; }
    if (it.window) { ClickGadget(it); return; }
    POINT pt;
    if (!ToClient(it.x, it.y, pt)) { LogLine(L"menu: no game window for the click"); return; }
    // Research: gpPool +0x6C (current button?) and +0x74/+0x78 (mouse in movie space?),
    // per Open-BFME-2's AptInput notes; logged before and after the cursor moves.
    auto poolState = [](const wchar_t* when) {
        uint32_t pool = 0, v[4] = {};
        if (!SafeReadU32(game::kAptPool, pool) || !pool || !SafeRead(pool + 0x6C, v, sizeof(v))) return;
        float fx, fy;
        memcpy(&fx, &v[2], 4);
        memcpy(&fy, &v[3], 4);
        wchar_t b[160];
        swprintf_s(b, L"menu: pool %s +6C=%08X +70=%08X +74=%08X(%d|%.1f) +78=%08X(%d|%.1f)", when, v[0], v[1],
                   v[2], (int)v[2], fx, v[3], (int)v[3], fy);
        LogLine(b);
    };
    poolState(L"before");
    SetCursorPos(pt.x, pt.y);
    Sleep(150);  // let the game see the hover first
    poolState(L"hover");
    // Closed loop: compare the game's own movie-space mouse with the target and
    // correct once, so a resolution or stretch we did not foresee still lands.
    uint32_t pool = 0;
    int gm[2] = {};
    // Only for APT buttons: over a window gadget or an open drop-down the Apt mouse
    // is not updated (it read 0,0 on 2026-10-31 23:50 and the "correction" flew off screen).
    if (it.cih && SafeReadU32(game::kAptPool, pool) && pool && SafeRead(pool + 0x74, gm, sizeof(gm)) &&
        (gm[0] || gm[1])) {
        float dx = it.x - gm[0], dy = it.y - gm[1];
        if ((std::fabs(dx) > 4 || std::fabs(dy) > 4) && std::fabs(dx) < 120 && std::fabs(dy) < 120) {
            RECT rc;
            GetClientRect(g_wnd, &rc);
            pt.x += (LONG)std::lround(dx * (rc.right - rc.left) / 1024.f);
            pt.y += (LONG)std::lround(dy * (rc.bottom - rc.top) / 768.f);
            SetCursorPos(pt.x, pt.y);
            Sleep(150);
            poolState(L"corrected");
        }
    }
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_MOUSE;
    in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &in[0], sizeof(INPUT));
    Sleep(80);
    SendInput(1, &in[1], sizeof(INPUT));
    wchar_t b[128];
    swprintf_s(b, L"menu: click [%d] movie (%.1f,%.1f) -> screen (%ld,%ld) mode %d ", g_focus, it.x, it.y,
               pt.x, pt.y, g_mapMode);
    LogLine(b + Widen(Join(it.path).c_str()));
}

bool Pressed(int vk) {
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool edge = down && !g_keyDown[vk];
    g_keyDown[vk] = down;
    return edge;
}

}  // namespace

void MenuInit(HWND gameWindow) { g_wnd = gameWindow; }

int MenuHudButtons(std::vector<HudButton>& out) {
    out.clear();
    static game::BilEntry bil[512];
    int n = game::ReadButtonList(bil, 512);
    for (int i = 0; i < n; ++i) {
        HudButton h;
        h.path = Join(PathOf(bil[i].cih));
        if (!HitCentre(bil[i], h.x, h.y)) h.x = bil[i].tx, h.y = bil[i].ty;
        out.push_back(h);
    }
    return (int)out.size();
}

bool MenuClickHud(const std::string& pathPart, bool right) {
    std::vector<HudButton> all;
    MenuHudButtons(all);
    for (const HudButton& h : all) {
        if (h.path.find(pathPart) == std::string::npos) continue;
        POINT pt;
        if (!ToClient(h.x, h.y, pt)) return false;
        SetCursorPos(pt.x, pt.y);
        Sleep(150);  // hover first, as for menu buttons
        INPUT in[2] = {};
        in[0].type = in[1].type = INPUT_MOUSE;
        in[0].mi.dwFlags = right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
        in[1].mi.dwFlags = right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
        SendInput(1, &in[0], sizeof(INPUT));
        Sleep(80);
        SendInput(1, &in[1], sizeof(INPUT));
        wchar_t b[160];
        swprintf_s(b, L"menu: hud %s click movie (%.1f,%.1f) -> screen (%ld,%ld) ", right ? L"right" : L"left", h.x, h.y,
                   pt.x, pt.y);
        LogLine(b + Widen(h.path.c_str()));
        return true;
    }
    LogLine(L"menu: hud button not found: " + Widen(pathPart.c_str()));
    return false;
}

bool MenuInBattle() { return g_inGame; }
bool MenuOverlay() { return g_overlay; }
void MenuSetQuiet(bool quiet) { g_quiet = quiet; }
void MenuHoldFocus(DWORD ms) { g_holdFocus = GetTickCount() + ms; }
void MenuSetWorldMap(bool on) {
    if (on == g_worldMap) return;
    g_worldMap = on;
    g_sig.clear();  // the list changes: rebuild it
    g_hoverAt = 0;
}

void MenuTick() {
    uintptr_t screen = 0;
    std::string name;
    int count = 0;
    bool haveShell = TopScreen(screen, name, count);
    if (haveShell) g_shellCount = count;
    if (!haveShell || count == 0) {
        if (!g_items.empty()) { g_items.clear(); LogLine(L"menu: no shell screen"); }
        g_screen = 0;
        g_sig.clear();
        return;
    }
    static game::BilEntry bil[512];
    int n = game::ReadButtonList(bil, 512);
    if (n < 0) return;
    std::string sig = std::to_string(screen) + ":";
    std::vector<uintptr_t> ids;
    for (int i = 0; i < n; ++i) ids.push_back(bil[i].cih);
    std::sort(ids.begin(), ids.end());
    for (auto id : ids) sig += std::to_string(id) + ",";
    static std::vector<Gadget> gadgets;
    gadgets.clear();
    CollectGadgets(gadgets);
    sig += "|";
    for (const auto& g : gadgets)
        sig += std::to_string(g.window) + ":" + std::to_string(g.row) + (g.selected ? "*=" : "=") + Utf8(g.text) + ";";
    if (sig == g_sig) return;
    if (sig != g_pendingSig) { g_pendingSig = sig; g_stableTicks = 0; return; }
    if (++g_stableTicks < 4) return;  // same list for 5 ticks (0.5 s): buttons stopped sliding in
    bool screenChanged = screen != g_screen;
    g_screen = screen;
    g_screenName = name;
    g_sig = sig;
    Rebuild(bil, n, gadgets, screenChanged);
}

void MenuKeys(bool gameFocused) {
    if (!gameFocused) {
        for (bool& k : g_keyDown) k = false;
        return;
    }
    HoverTick();
    if (WotrSetupIsOpen()) {
        if (g_screen == 0 || g_inGame) WotrSetupClose();
        else { WotrSetupKeys(); return; }
    }
    if (g_pick.open) {
        if (g_screen == 0 || g_inGame) g_pick.open = false;
        else { PickerKeys(); return; }
    }
    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    bool up = Pressed(VK_UP), down = Pressed(VK_DOWN), left = Pressed(VK_LEFT), right = Pressed(VK_RIGHT);
    bool home = Pressed(VK_HOME), end = Pressed(VK_END), tab = Pressed(VK_TAB);
    bool enter = Pressed(VK_RETURN), space = Pressed(VK_SPACE), f11 = Pressed(VK_F11);
    if (ctrl && f11) {
        g_mapMode = 1 - g_mapMode;
        MenuSay(g_mapMode == 0 ? L"Щелчок: режим по высоте" : L"Щелчок: режим растянутый", true);
        return;
    }
    if (g_screen == 0 || g_items.empty() || (g_inGame && !g_overlay)) return;
    if (Pressed(VK_OEM_7) && !g_inGame)
        for (const Item& it : g_items) {
            if (it.picker == 3) { WotrSetupOpen(it.window); return; }
            if (it.picker == 4) { OpenSkirmishQuick(); return; }
            if (it.picker) { OpenPicker(it); return; }
        }
    int last = (int)g_items.size() - 1;
    int col = g_items[g_focus].column, grp = g_items[g_focus].group;
    // In a list a letter jumps to the next row starting with it. The map list's rows begin
    // with the state of the map ("не пройдена, Bruinen Gorges*, 2").
    if (g_items[g_focus].row >= 0 && !ctrl) {
        for (int vk = 'A'; vk <= 'Z'; ++vk) {
            if (!Pressed(vk)) continue;
            auto first = [](const std::wstring& label) {
                size_t at = label.rfind(L"пройдена, ", 0) == 0 || label.rfind(L"не пройдена, ", 0) == 0 ? label.find(L", ") + 2 : 0;
                return at < label.size() ? (wchar_t)towupper(label[at]) : L' ';
            };
            int n = last + 1;
            for (int step = 1; step <= n; ++step) {
                int i = (g_focus + step) % n;
                if (g_items[i].group == grp && first(g_items[i].label) == (wchar_t)vk) {
                    g_focus = i;
                    SpeakFocus(true);
                    return;
                }
            }
            MenuSay(L"Нет на эту букву", true);
            return;
        }
    }
    auto same = [&](int i) { return g_items[i].group == grp && g_items[i].column == col; };
    // Up/Down stay inside the column of the group; at its end they stay put silently.
    if (down && g_focus < last && same(g_focus + 1)) { ++g_focus; SpeakFocus(true); }
    if (up && g_focus > 0 && same(g_focus - 1)) { --g_focus; SpeakFocus(true); }
    if (left || right) {
        int to = NeighbourColumn(right ? 1 : -1);
        if (to >= 0) { g_focus = to; SpeakFocus(true); }
    }
    // Home/End: first and last item of the group.
    if (home || end) {
        int to = g_focus;
        for (int i = 0; i <= last; ++i)
            if (g_items[i].group == grp) { if (home) { to = i; break; } to = i; }
        g_focus = to;
        SpeakFocus(true);
    }
    // Tab / Shift+Tab: next or previous group that has items; lands where the player last was.
    if (tab) {
        int groups = (int)g_groups.size();
        for (int step = 1; step <= groups; ++step) {
            int g = ((grp + (shift ? -step : step)) % groups + groups) % groups;
            int first = -1, remembered = -1;
            for (int i = 0; i <= last; ++i) {
                if (g_items[i].group != g) continue;
                if (first < 0) first = i;
                if (g_lastInGroup.count(g) && Key(g_items[i]) == g_lastInGroup[g]) remembered = i;
            }
            if (first < 0) continue;
            g_lastInGroup[grp] = Key(g_items[g_focus]);
            g_focus = remembered >= 0 ? remembered : first;
            MenuSay(g_groups[g].second + L", " + std::to_wstring(CountInGroup(g)), true);
            SpeakFocus(false);
            break;
        }
    }
    if (space && shift) {
        const Item& it = g_items[g_focus];
        MenuSay(it.details.empty() ? L"нет описания" : it.details, true);
    } else if (space) {
        const Item& it = g_items[g_focus];
        int pos = 0, count = 0;
        for (int i = 0; i <= last; ++i)
            if (g_items[i].group == it.group) { ++count; if (i == g_focus) pos = count; }
        MenuSay(it.label + L", " + std::to_wstring(pos) + L" из " + std::to_wstring(count), true);
    }
    if (enter) Click();
}
