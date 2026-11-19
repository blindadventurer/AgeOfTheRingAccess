// Eternal stealth: see nocatch.h.
//
// The sneaking missions notice a hero by plain distance in a team's generic script ("G Hobbits
// Spotted"). DISABLE_SCRIPT cannot reach those: a team runs a private copy of the script
// (Worldbuilder TeamPrototype::getGenericScript 0xEFC1B0: Script::duplicate; game.dat 0x7A1759), and
// an inactive one is dropped for that team for good (0x7A26E2 -> 0x7A285D). So the mod answers
// where every script comes through, original or copy: the evaluator of a script's conditions,
// ScriptEngine 0x60930F, thiscall (Script*, Team*, Player*), ret 0xC, callers 0x609ABB, 0x609B89,
// 0x609C55 (scripts of a side) and 0x609DC9 (evaluateConditions 0x609D97, teams). The answer false
// leads to the ELSE actions, so a script listed in nocatch.ini must have none.
//
// A Script does not hold its name: +4, +8, +0xC are the three comments (log 2026-11-07 04:10:38,
// +4 read "In this script you set up the conditions..."). The name lives beside it: a team's copy
// in its TeamPrototype, copy at proto +0x2C + i * 4, name at proto +0x244 + i * 4 (0x7A1773, 0x7A17E6;
// +0xAC + i * 4 is the side that holds the script: log 2026-11-07 04:22 read "PlyrCreeps" there);
// a script of a side as the key of the side's table (findScript 0x7B72EC).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include "game.h"
#include "log.h"
#include "memory.h"
#include "nocatch.h"
#include "roads.h"
#include "scanner.h"
#include "speech.h"

namespace {

constexpr uintptr_t kEvaluate = 0x60930F;
constexpr uint8_t kExpected[5] = {0xB8, 0x76, 0x0F, 0xB8, 0x00};  // mov eax, 0xB80F76
// TheSidesList: side count +0x3C, side i at +0x40 + i * 0x60, its ScriptList at side + 8. The script
// table is an object at list +0x2C: entries begin list +0x38, end list +0x3C, stride 0x14, entry +8
// AsciiString key, +0x10 value, Script = value + 4 (0x7B72EC; the group table at +0xC is built alike).
constexpr uintptr_t kTheSidesList = 0xDE77A0;
constexpr uint32_t kTeamProto = 0x30, kProtoCopies = 0x2C, kProtoNames = 0x244;
constexpr int kGenericScripts = 32;

uintptr_t g_resume = kEvaluate + 5;
volatile int g_on = 0;
bool g_hooked = false;
std::wstring g_flagFile;
std::set<std::string> g_names;                       // upper case, of all missions
std::map<std::string, std::vector<std::string>> g_byMap;  // mission -> names
// "+Script | Template range": the script answers true while an object of Template stands within range
// of one of the local player's units, else it runs as usual. Dead Marshes: "G Nazgul Leave" waits for
// all three hobbits in a bush; without "Attack" the Nazgul still went for them (player, test
// 2026-11-08 01:14-01:36), so it is sent off as soon as it comes close.
struct Force {
    std::string tmpl;  // upper case
    float range = 0;
    bool close = false;
    DWORD checked = 0;
};
std::map<std::string, Force> g_force;  // upper-case script name -> rule

std::string Upper(std::string s) {
    for (char& c : s) c = (char)toupper((unsigned char)c);
    return s;
}

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
    if (a == std::string::npos) return std::string();
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

uint32_t Field(uint32_t a) {
    uint32_t v = 0;
    SafeReadU32(a, v);
    return v;
}

// The name as it stands in a map's list, in any case, with or without its side's prefix.
std::string Key(const std::string& name) {
    std::string u = Upper(name);
    size_t slash = u.rfind('/');
    return slash == std::string::npos || g_names.count(u) || g_force.count(u) ? u : u.substr(slash + 1);
}
bool Listed(const std::string& name) { return g_names.count(Key(name)) > 0; }
Force* Forced(const std::string& name) {
    auto it = g_force.find(Key(name));
    return it == g_force.end() ? nullptr : &it->second;
}

// Object list walk, game thread (offsets of scanner.cpp: TheGameLogic 0xDE412C, list head +0xAC,
// next +0x8C, template +4 with its name at +0x64, team +0x31C -> proto +0x30 -> player +8, position
// +0x14 / +0x24, status byte +0x458 bit 0 dying; ThePlayerList 0xDE4928 +0x10 the local player).
constexpr uintptr_t kTheGameLogic = 0xDE412C, kThePlayerList = 0xDE4928;
constexpr uint32_t kObjectVtable = 0x00C123D8;
std::unordered_map<uint32_t, std::string> g_tmplNames;

bool NearOwn(Force& f) {
    DWORD now = GetTickCount();
    if (now - f.checked < 250) return f.close;
    f.checked = now;
    uint32_t local = Field(Field(kThePlayerList) + 0x10);
    std::vector<std::pair<float, float>> own, them;
    uint32_t gl = Field(kTheGameLogic);
    int steps = 0;
    for (uint32_t p = gl ? Field(gl + 0xAC) : 0; p && Field(p) == kObjectVtable && steps < 20000; p = Field(p + 0x8C), ++steps) {
        uint8_t status = 0;
        if (SafeRead(p + 0x458, &status, 1) && (status & 1)) continue;
        float x = 0, y = 0;
        SafeRead(p + 0x14, &x, 4);
        SafeRead(p + 0x24, &y, 4);
        uint32_t team = Field(p + 0x31C), proto = team ? Field(team + 0x30) : 0;
        if (local && proto && Field(proto + 8) == local) own.push_back({x, y});
        uint32_t t = Field(p + 4);
        if (!t) continue;
        auto n = g_tmplNames.find(t);
        if (n == g_tmplNames.end()) n = g_tmplNames.emplace(t, Upper(game::ReadAsciiString(Field(t + 0x64)))).first;
        if (n->second == f.tmpl) them.push_back({x, y});
    }
    bool close = false;
    for (const auto& a : them)
        for (const auto& b : own)
            if ((a.first - b.first) * (a.first - b.first) + (a.second - b.second) * (a.second - b.second) < f.range * f.range) close = true;
    if (close != f.close) LogLine(std::wstring(L"nocatch: ") + Widen(f.tmpl.c_str()) + (close ? L" came near, script made true" : L" away"));
    f.close = close;
    return close;
}

// game thread only: Script -> 0 runs, 1 held back, 2 + i the forcing rule i
std::unordered_map<uint32_t, int> g_seen;
std::vector<Force*> g_forceBySeen;
std::set<uint32_t> g_sideBlocked;           // Scripts of the sides' tables that are listed
std::set<std::string> g_saidBlocked;
uint32_t g_tables = 0, g_calls = 0;
bool g_built = false;
int g_samples = 0;

// Changes when a map is loaded: the tables are built anew.
uint32_t TablesStamp() {
    uint32_t sides = Field(kTheSidesList), stamp = 0;
    int count = sides ? (int)Field(sides + 0x3C) : 0;
    for (int i = 0; i < count && i < 32; ++i) stamp = stamp * 31 + Field(sides + 0x40 + i * 0x60 + 8 + 0x38);
    return stamp;
}

void ReadSideTables() {
    g_sideBlocked.clear();
    uint32_t sides = Field(kTheSidesList);
    int count = sides ? (int)Field(sides + 0x3C) : 0, all = 0;
    for (int i = 0; i < count && i < 32; ++i) {
        uint32_t list = sides + 0x40 + i * 0x60 + 8;
        uint32_t begin = Field(list + 0x38), end = Field(list + 0x3C);
        if (!begin || end <= begin || (end - begin) / 0x14 > 20000) continue;
        for (uint32_t e = begin; e < end; e += 0x14) {
            uint32_t value = Field(e + 0x10), key = Field(e + 8);
            if (!value || !key) continue;
            ++all;
            if (Listed(game::ReadAsciiString(key))) g_sideBlocked.insert(value + 4);
        }
    }
    wchar_t b[100];
    swprintf_s(b, L"nocatch: side tables hold %d scripts, %zu of them listed", all, g_sideBlocked.size());
    LogLine(b);
}

void Refresh() {
    uint32_t stamp = TablesStamp();
    if (g_built && stamp == g_tables) return;
    g_built = true;
    g_tables = stamp;
    g_seen.clear();
    ReadSideTables();
}

// The name of a team's copy, from the team's prototype.
std::string CopyName(uint32_t script, uint32_t team) {
    uint32_t proto = Field(team + kTeamProto);
    if (!proto) return std::string();
    for (int i = 0; i < kGenericScripts; ++i) {
        if (Field(proto + kProtoCopies + i * 4) != script) continue;
        uint32_t name = Field(proto + kProtoNames + i * 4);
        return name ? game::ReadAsciiString(name) : std::string();
    }
    return std::string();
}

// On the game thread, for every script evaluated while the mode is on: 0 runs it, 1 answers false,
// 2 answers true.
int Answer(int kind) {
    if (kind < 2) return kind;
    size_t i = (size_t)(kind - 2);
    return i < g_forceBySeen.size() && NearOwn(*g_forceBySeen[i]) ? 2 : 0;
}

int __cdecl Blocked(uint32_t script, uint32_t team) {
    if (!g_built || (++g_calls & 0xFF) == 0) Refresh();
    auto it = g_seen.find(script);
    if (it != g_seen.end()) return Answer(it->second);
    if (g_seen.size() > 30000) g_seen.clear();
    bool blocked = g_sideBlocked.count(script) > 0;
    std::string name;
    if (team) {
        name = CopyName(script, team);
        blocked = blocked || (!name.empty() && Listed(name));
        if (g_samples < 8 && !name.empty()) {
            ++g_samples;
            LogLine(L"nocatch: a team's script " + Widen(name.c_str()));
        }
        if (Force* f = name.empty() ? nullptr : Forced(name)) {
            size_t i = std::find(g_forceBySeen.begin(), g_forceBySeen.end(), f) - g_forceBySeen.begin();
            if (i == g_forceBySeen.size()) g_forceBySeen.push_back(f);
            g_seen[script] = 2 + (int)i;
            if (g_saidBlocked.insert(name).second) LogLine(L"nocatch: watching " + Widen(name.c_str()));
            return Answer(2 + (int)i);
        }
    }
    g_seen[script] = blocked;
    if (blocked && g_saidBlocked.insert(name).second) {
        LogLine(L"nocatch: holding back " + Widen((name.empty() ? std::string("a script of a side") : name).c_str()));
    }
    return blocked;
}

// Entry: [esp] = return address, [esp+4] = Script*, [esp+8] = Team*, ecx = ScriptEngine.
__declspec(naked) void Stub() {
    __asm {
        cmp dword ptr [g_on], 0
        je run
        pushad
        mov eax, [esp + 40]
        push eax
        mov eax, [esp + 40]
        push eax
        call Blocked
        add esp, 8
        mov [esp + 28], eax
        popad
        test eax, eax
        je run
        cmp eax, 2
        je yes
        xor eax, eax
        ret 0xC
    yes:
        mov eax, 1
        ret 0xC
    run:
        mov eax, 0x00B80F76
        jmp dword ptr [g_resume]
    }
}

void Load(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    std::string line, map;
    while (std::getline(f, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            map = line.substr(1, line.size() - 2);
            continue;
        }
        g_byMap[map].push_back(line);
        if (line[0] == '+') {
            size_t bar = line.find('|');
            Force f;
            char tmpl[128] = "";
            if (bar == std::string::npos || sscanf_s(line.c_str() + bar + 1, " %127s %f", tmpl, (unsigned)sizeof tmpl, &f.range) != 2)
                continue;
            f.tmpl = Upper(tmpl);
            g_force[Upper(Trim(line.substr(1, bar - 1)))] = f;
            continue;
        }
        g_names.insert(Upper(line));
    }
    wchar_t b[100];
    swprintf_s(b, L"nocatch: %zu scripts of %zu missions", g_names.size(), g_byMap.size());
    LogLine(b);
}

void Remember(bool on) {
    if (on) {
        std::ofstream(g_flagFile, std::ios::binary) << "on\n";
    } else {
        DeleteFileW(g_flagFile.c_str());
    }
}

}  // namespace

bool NoCatchOn() { return g_on != 0; }

void NoCatchInit(const std::wstring& modDir) {
    Load(modDir + L"\\nocatch.ini");
    g_flagFile = modDir + L"\\nocatch-on.txt";
    if (GetFileAttributesW(g_flagFile.c_str()) != INVALID_FILE_ATTRIBUTES) {
        g_on = 1;
        LogLine(L"nocatch: on, remembered from the last game");
    }
}

void NoCatchInstallHook() {
    if (g_hooked) return;
    uint8_t code[5];
    if (!SafeRead(kEvaluate, code, sizeof code) || memcmp(code, kExpected, sizeof code) != 0) {
        LogLine(L"nocatch: evaluator bytes differ, no hook");
        return;
    }
    DWORD old = 0;
    auto at = reinterpret_cast<uint8_t*>(kEvaluate);
    if (!VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &old)) {
        LogLine(L"nocatch: VirtualProtect failed, no hook");
        return;
    }
    int32_t rel = (int32_t)((uintptr_t)&Stub - (kEvaluate + 5));
    at[0] = 0xE9;
    memcpy(at + 1, &rel, 4);
    VirtualProtect(at, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 5);
    g_hooked = true;
    LogLine(L"nocatch: hook installed");
}

void NoCatchToggle() {
    if (!g_hooked) {
        Speak(L"Скрытность не работает: игра другой версии", true);
        return;
    }
    bool on = !g_on;
    g_on = on ? 1 : 0;
    Remember(on);
    std::string map = RoadsMapName(ScannerObjectiveTags());
    auto it = g_byMap.find(map);
    size_t count = it == g_byMap.end() ? 0 : it->second.size();
    LogLine(L"nocatch: " + std::wstring(on ? L"on" : L"off") + L", mission " + Widen(map.c_str()) + L", scripts " +
            std::to_wstring(count));
    if (!on) {
        Speak(L"Вечная скрытность выключена", true);
    } else if (count) {
        Speak(L"Вечная скрытность включена. Враги этой миссии вас не заметят, предупреждения о патрулях выключены. Настройка сохраняется.", true);
    } else {
        Speak(L"Вечная скрытность включена. В этой миссии прятаться не нужно, сработает в тех, где нужно. Настройка сохраняется.", true);
    }
}
