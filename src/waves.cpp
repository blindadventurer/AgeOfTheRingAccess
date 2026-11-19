// Siege mode: see waves.h.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "gamethread.h"
#include "log.h"
#include "memory.h"
#include "menu.h"
#include "objects_db.h"
#include "scanner.h"
#include "speech.h"
#include "waves.h"

namespace {

// The steps of the script action CREATE_UNNAMED_ON_TEAM_AT_WAYPOINT (0x7C5B3B): TheThingFactory
// findTemplate (AsciiString*), TheScriptEngine getTeamNamed (AsciiString by value, 1), newObject
// (template, team, 16 zero status bytes, 0), setPosition, then 0x6902F5. A waypoint: TheTerrainLogic
// vt+0x88 (AsciiString*), position at +0xC. NAMED_HUNT (0x7C9531): 0x6AF150 on the object's AI +
// 0x20 with 1, the same source scripts use. Money::deposit: see training.cpp.
constexpr uintptr_t kTheThingFactory = 0xDE4A40, kTheScriptEngine = 0xDE3BAC, kTheTerrainLogic = 0xDE4690;
constexpr uintptr_t kFindTemplate = 0x6D1305, kGetTeamNamed = 0x759FDA, kNewObject = 0x6D165E;
constexpr uintptr_t kSetPosition = 0x70C201, kObjectReady = 0x6902F5, kAiHunt = 0x6AF150;
constexpr uintptr_t kAsciiStringCtor = 0x40611E, kAsciiStringDtor = 0x4367B0, kMoneyDeposit = 0x7B18B8;
constexpr uint32_t kObjAI = 0x260, kAiHuntPart = 0x20, kWaypointSlot = 0x88, kWaypointPosition = 0xC;
constexpr uint32_t kPlayerMoneyObject = 0x90;

uint32_t U32(uintptr_t a) {
    uint32_t v = 0;
    SafeReadU32(a, v);
    return v;
}
bool Ptr(uint32_t v) { return v >= 0x10000 && v < 0x7FFF0000; }

void NewAscii(uint32_t* str, const char* text) {
    reinterpret_cast<void*(__thiscall*)(void*, const char*)>(kAsciiStringCtor)(str, text);
}
void FreeAscii(uint32_t* str) { reinterpret_cast<void(__thiscall*)(void*)>(kAsciiStringDtor)(str); }

uint32_t GuardedTemplate(const char* name) {
    __try {
        uint32_t str = 0;
        NewAscii(&str, name);
        uint32_t t = reinterpret_cast<uint32_t(__thiscall*)(void*, const uint32_t*)>(kFindTemplate)(
            reinterpret_cast<void*>((uintptr_t)U32(kTheThingFactory)), &str);
        FreeAscii(&str);
        return t;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

uint32_t GuardedTeam(const char* name) {
    __try {
        uint32_t str = 0;  // by value: the callee frees it
        NewAscii(&str, name);
        return reinterpret_cast<uint32_t(__thiscall*)(void*, uint32_t, int)>(kGetTeamNamed)(
            reinterpret_cast<void*>((uintptr_t)U32(kTheScriptEngine)), str, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool GuardedWaypoint(const char* name, float* pos) {
    __try {
        uint32_t terrain = U32(kTheTerrainLogic), vt = Ptr(terrain) ? U32(terrain) : 0;
        uint32_t fn = Ptr(vt) ? U32(vt + kWaypointSlot) : 0;
        if (!Ptr(fn)) return false;
        uint32_t str = 0;
        NewAscii(&str, name);
        uint32_t way = reinterpret_cast<uint32_t(__thiscall*)(void*, const uint32_t*)>((uintptr_t)fn)(
            reinterpret_cast<void*>((uintptr_t)terrain), &str);
        FreeAscii(&str);
        if (!Ptr(way)) return false;
        return SafeRead(way + kWaypointPosition, pos, 12);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint32_t GuardedSpawn(uint32_t tmpl, uint32_t team, const float* pos) {
    __try {
        uint32_t status[4] = {};
        uint32_t obj = reinterpret_cast<uint32_t(__thiscall*)(void*, uint32_t, uint32_t, uint32_t*, int)>(kNewObject)(
            reinterpret_cast<void*>((uintptr_t)U32(kTheThingFactory)), tmpl, team, status, 0);
        if (!Ptr(obj)) return 0;
        reinterpret_cast<void(__thiscall*)(void*, const float*)>(kSetPosition)(reinterpret_cast<void*>((uintptr_t)obj), pos);
        reinterpret_cast<void(__thiscall*)(void*)>(kObjectReady)(reinterpret_cast<void*>((uintptr_t)obj));
        return obj;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool GuardedHunt(uint32_t obj) {
    __try {
        uint32_t ai = U32(obj + kObjAI);
        if (!Ptr(ai)) return false;
        reinterpret_cast<void(__thiscall*)(void*, int)>(kAiHunt)(reinterpret_cast<void*>((uintptr_t)(ai + kAiHuntPart)), 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool GuardedDeposit(uint32_t money, uint32_t amount) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*, uint32_t, int, int)>(kMoneyDeposit)(
            reinterpret_cast<void*>((uintptr_t)money), amount, 0, 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Each enemy host: footmen from the first wave, stronger troops later, its captains on the
// boss waves. All names checked against the object INI files 2026-11-05.
struct Theme {
    const wchar_t* name;
    std::vector<const char*> light, middle, heavy, boss;
};
const std::vector<Theme>& Themes() {
    static const std::vector<Theme> t = {
        {L"гоблины Мглистых гор", {"GoblinFighterHorde", "GoblinArcherHorde"}, {"GoblinTownFighterHorde", "GoblinSpiderRiderHorde"},
         {"GoblinCaveTroll"}, {"WildBolgNew", "WildGolfimbul", "WildGreatGoblin", "WildMuzgash"}},
        {L"Мордор", {"MordorFighterHorde", "MordorArcherHorde"}, {"MordorMorgulOrcHorde", "MordorEasterlingSwordsmanHorde"},
         {"MordorMountainTroll", "MordorTrollCatapult"}, {"MordorGothmog", "MordorShagrat", "MordorGorbag"}},
        {L"Изенгард", {"IsengardWildmanHorde", "IsengardWildmanSpearHorde", "IsengardDunlandAxemenHorde"},
         {"IsengardFighterHorde", "IsengardPikemanHorde", "IsengardUrukCrossbowHorde"}, {"IsengardBerserkerHorde"},
         {"IsengardLurtz", "IsengardUgluk", "IsengardSharku"}},
        {L"Ангмар", {"AngmarSnagaHorde", "AngmarWolfHorde"},
         {"AngmarGundadbadWarriorHorde", "AngmarGundabadArcherHorde", "AngmarRisenDeadHorde"},
         {"AngmarHillTrollHorde", "AngmarSnowTrollHorde"}, {"AngmarKarsh", "AngmarRogash"}},
        {L"Дол Гулдур", {"GuldurOrcPikemanHorde", "GuldurOrcArcherHorde", "GuldurBrokenRabbleHorde"},
         {"GuldurRisenDeadHorde", "GuldurBlackWargHorde", "GuldurSpiderRiderHorde"}, {"GuldurTombGuardHorde", "GuldurFellBroodHorde"},
         {"GuldurKrimpul", "GuldurCargast", "GuldurKhamul"}},
        {L"Харад", {"HaradTribalSpearmenHorde", "HaradTribalAxemenHorde", "HaradTribalJavelineersHorde"},
         {"HaradUmbarRaiderHorde", "HaradCorsairsOfUmbarHorde", "HaradMahudWarriorHorde"},
         {"HaradMumakHorde", "HaradBlackNumenoreanVanguardHorde"}, {"HaradSuladan", "HaradCaranLambar", "HaradCassimir"}},
    };
    return t;
}

struct Difficulty {
    const wchar_t* name;
    int firstSeconds, everySeconds;
    float size, reward;
};
const Difficulty kDifficulties[3] = {
    {L"лёгкая", 240, 210, 0.7f, 1.5f}, {L"обычная", 180, 170, 1.0f, 1.0f}, {L"тяжёлая", 120, 130, 1.4f, 1.0f}};

struct Group {
    int theme = 0;
    float x = 0, y = 0, z = 0;
};
struct Plan {
    int number = 0;
    std::vector<Group> groups;
};

const char* Pick(const std::vector<const char*>& v) { return v[rand() % v.size()]; }

bool g_mode = false;
int g_difficulty = 1;
int g_wave = 0, g_cleared = 0, g_record[3] = {};
int g_unpaid = 0;  // reward of the waves on the field
long g_remaining = 0;  // ms to the next wave
bool g_warned30 = false, g_warned10 = false, g_near150 = false, g_near60 = false;
Plan g_plan;
std::wstring g_recordPath;
DWORD g_lastPass = 0, g_lastUpdate = 0;
int g_updates = 0;
std::atomic<bool> g_updating{false};

std::set<uint32_t> g_units;  // objects of the waves: game thread only
std::atomic<int> g_alive{0};
std::atomic<float> g_nearest{0}, g_nearX{0}, g_nearY{0};
float g_baseX = 0, g_baseY = 0;

bool FindBase() {
    uint32_t base = 0;
    for (const char* part : {"Citadel", "Fortress", "Castle", "Keep"})
        if (!base) base = ScannerOwnObjectNamed(part);
    float z = 0;
    if (base && ScannerPosition(base, g_baseX, g_baseY, z)) return true;
    std::vector<uint32_t> sel = ScannerSelection();
    return !sel.empty() && ScannerPosition(sel[0], g_baseX, g_baseY, z);
}

// "с северо-запада": ScannerDirection gives the nominative.
std::wstring FromSide(float x, float y) {
    static const struct { const wchar_t* side; const wchar_t* from; } kWords[] = {
        {L"север", L"с севера"}, {L"юг", L"с юга"}, {L"запад", L"с запада"}, {L"восток", L"с востока"},
        {L"северо-запад", L"с северо-запада"}, {L"северо-восток", L"с северо-востока"},
        {L"юго-запад", L"с юго-запада"}, {L"юго-восток", L"с юго-востока"}};
    std::wstring d = ScannerDirection(x - g_baseX, y - g_baseY);
    for (const auto& w : kWords)
        if (d == w.side) return w.from;
    return d;
}

std::wstring PlanWords(const Plan& p) {
    std::wstring w;
    for (size_t i = 0; i < p.groups.size(); ++i) {
        if (i) w += L", ";
        w += Themes()[p.groups[i].theme].name + std::wstring(L" ") + FromSide(p.groups[i].x, p.groups[i].y);
    }
    return w;
}

// On the game thread: start positions away from the base; later waves come from several sides.
bool MakePlan(Plan* p) {
    std::vector<std::vector<float>> places;
    for (int i = 1; i <= 8; ++i) {
        char name[32];
        sprintf_s(name, "Player_%d_Start", i);
        float at[3] = {};
        if (GuardedWaypoint(name, at) && std::hypot(at[0] - g_baseX, at[1] - g_baseY) > 1000.f)
            places.push_back({at[0], at[1], at[2]});
    }
    if (places.empty()) return false;
    p->number = g_wave + 1;
    int hosts = p->number <= 3 ? 1 : p->number <= 8 ? 2 : 3;
    std::vector<int> themes;
    for (int i = 0; i < (int)Themes().size(); ++i) themes.push_back(i);
    for (size_t i = themes.size() - 1; i > 0; --i) std::swap(themes[i], themes[rand() % (i + 1)]);
    size_t first = rand() % places.size();
    p->groups.clear();
    for (int i = 0; i < hosts; ++i) {
        const std::vector<float>& at = places[(first + i) % places.size()];
        Group g;
        g.theme = themes[i];
        g.x = at[0], g.y = at[1], g.z = at[2];
        p->groups.push_back(g);
    }
    return true;
}

const char* UnitOf(const Theme& t, int number) {
    int roll = rand() % 100;
    int heavy = number >= 6 ? (std::min)(10 + (number - 6) * 5, 40) : 0;
    int middle = number >= 3 ? (std::min)(30 + (number - 3) * 6, 50) : 0;
    if (roll < heavy) return Pick(t.heavy);
    if (roll < heavy + middle) return Pick(t.middle);
    return Pick(t.light);
}

struct Spawned {
    int units = 0, failed = 0;
    bool noTeam = false;
    std::vector<std::string> bosses;
};

void SpawnPlan(const Plan& p, float size, Spawned* r) {
    // Scripts name a team with its side (test 18:29: the bare name found nothing; dumps hold
    // "PlyrCreeps/teamPlyrCreeps"). The objects of that side know their team too.
    uint32_t team = GuardedTeam("PlyrCreeps/teamPlyrCreeps");
    if (!Ptr(team)) team = ScannerTeamOfSide("PlyrCreeps");
    if (!Ptr(team)) { r->noTeam = true; return; }
    int total = (std::min)((int)std::lround((2.f + p.number * 0.8f) * size), 16);
    total = (std::max)(total, (int)p.groups.size());
    wchar_t line[140];
    for (size_t g = 0; g < p.groups.size(); ++g) {
        const Group& at = p.groups[g];
        const Theme& theme = Themes()[at.theme];
        int count = total / (int)p.groups.size() + ((int)g < total % (int)p.groups.size() ? 1 : 0);
        bool boss = p.number % 5 == 0 && (g == 0 || p.number >= 15);
        for (int i = 0; i < count + (boss ? 1 : 0); ++i) {
            const char* name = boss && i == count ? Pick(theme.boss) : UnitOf(theme, p.number);
            uint32_t tmpl = GuardedTemplate(name);
            float pos[3] = {at.x + (i % 4) * 90.f, at.y + (i / 4) * 90.f, at.z};
            uint32_t obj = Ptr(tmpl) ? GuardedSpawn(tmpl, team, pos) : 0;
            bool hunt = obj && GuardedHunt(obj);
            if (obj) {
                ++r->units;
                g_units.insert(obj);
                if (boss && i == count) r->bosses.push_back(name);
            } else {
                ++r->failed;
            }
            swprintf_s(line, L"waves: unit %S template %08X object %08X hunt %d", name, tmpl, obj, hunt ? 1 : 0);
            LogLine(line);
        }
    }
    // Until the next count: nobody may read the wave as beaten, or as already at the base.
    g_alive = (int)g_units.size();
    g_nearest = 1e9f;
}

// On the game thread: who of the waves still stands, the nearest to the base, and now and then
// the order again, since a horde's soldiers come a moment after the horde. Only objects found
// in the list are touched: a dead one's memory is gone.
void UpdateUnits(bool hunt) {
    std::set<uint32_t> alive;
    float best = 1e9f, bx = 0, by = 0;
    ScannerForEachObject([&](uint32_t obj) {
        if (!g_units.count(obj) || ScannerObjectDead(obj)) return;
        alive.insert(obj);
        float x = 0, y = 0, z = 0;
        if (ScannerPosition(obj, x, y, z)) {
            float d = std::hypot(x - g_baseX, y - g_baseY);
            if (d < best) best = d, bx = x, by = y;
        }
        if (hunt) GuardedHunt(obj);
    });
    g_units.swap(alive);
    g_alive = (int)g_units.size();
    g_nearest = best;
    g_nearX = bx;
    g_nearY = by;
}

void SaveRecord() {
    std::ofstream f(g_recordPath);
    if (f) f << g_record[0] << " " << g_record[1] << " " << g_record[2] << "\n";
}

int Reward(int number, bool early) {
    float r = (200.f + 100.f * number) * kDifficulties[g_difficulty].reward;
    return (int)(early ? r * 1.5f : r);
}

void WaveCleared() {
    int reward = g_unpaid;
    g_unpaid = 0;
    g_cleared = g_wave;
    uint32_t local = ScannerLocalPlayer();
    if (local && reward > 0) RunOnGameThreadAndWait([local, reward] { GuardedDeposit(local + kPlayerMoneyObject, reward); });
    if (g_cleared > g_record[g_difficulty]) {
        g_record[g_difficulty] = g_cleared;
        SaveRecord();
    }
    wchar_t b[100];
    swprintf_s(b, L"waves: cleared %d, reward %d", g_cleared, reward);
    LogLine(b);
    Speak(L"Волна " + std::to_wstring(g_cleared) + L" отбита. Награда " + std::to_wstring(reward) + L".", false);
}

void StartWave(bool early) {
    if (!FindBase()) { Speak(L"Не нашёл вашу базу", true); return; }
    Plan plan = g_plan;
    if (plan.number != g_wave + 1) {
        auto made = std::make_shared<Plan>();
        auto ok = std::make_shared<bool>(false);
        RunOnGameThreadAndWait([made, ok] { *ok = MakePlan(made.get()); });
        if (!*ok) { Speak(L"На карте нет места для появления врагов", true); return; }
        plan = *made;
    }
    auto r = std::make_shared<Spawned>();
    float size = kDifficulties[g_difficulty].size;
    RunOnGameThreadAndWait([plan, size, r] { SpawnPlan(plan, size, r.get()); });
    wchar_t b[140];
    swprintf_s(b, L"waves: wave %d, %d units, %d failed, base (%.0f, %.0f)", plan.number, r->units, r->failed, g_baseX, g_baseY);
    LogLine(b);
    if (r->noTeam) { Speak(L"Не нашёл сторону для врагов", true); return; }
    if (!r->units) { Speak(L"Волну создать не удалось", true); return; }
    g_wave = plan.number;
    g_unpaid += Reward(plan.number, early);
    g_plan = Plan();
    g_warned30 = g_warned10 = g_near150 = g_near60 = false;
    g_lastUpdate = GetTickCount();
    std::wstring words = L"Волна " + std::to_wstring(g_wave) + L". " + PlanWords(plan) + L". Отрядов: " + std::to_wstring(r->units) + L".";
    for (const std::string& name : r->bosses) {
        const ObjectInfo* o = ObjectsDbFind(name);
        words += L" Ведёт их " + (o && !o->displayName.empty() ? o->displayName : std::wstring(name.begin(), name.end())) + L".";
    }
    Speak(words, true);
}

void Warn() {
    if (!FindBase()) return;
    auto made = std::make_shared<Plan>();
    auto ok = std::make_shared<bool>(false);
    RunOnGameThreadAndWait([made, ok] { *ok = MakePlan(made.get()); });
    if (!*ok) return;
    g_plan = *made;
    Speak(L"Через 30 секунд волна " + std::to_wstring(g_plan.number) + L": " + PlanWords(g_plan) + L".", false);
}

void Finish() {
    bool played = g_wave > 0;
    if (played) {
        wchar_t b[80];
        swprintf_s(b, L"waves: finished, %d waves called, %d repelled", g_wave, g_cleared);
        LogLine(b);
        Speak(L"Осада окончена. Отбито волн: " + std::to_wstring(g_cleared) + L". Рекорд: " + std::to_wstring(g_record[g_difficulty]) + L".", false);
    }
    g_mode = false;
    g_wave = g_cleared = g_unpaid = 0;
    g_alive = 0;
    g_plan = Plan();
    if (played) RunOnGameThread([] { g_units.clear(); });
}

}  // namespace

void WavesInit(const std::wstring& modDir) {
    g_recordPath = modDir + L"\\waves-record.txt";
    std::ifstream f(g_recordPath);
    f >> g_record[0] >> g_record[1] >> g_record[2];
    srand(GetTickCount());
}

void WavesTick() {
    DWORD now = GetTickCount();
    long dt = (long)(now - g_lastPass);
    g_lastPass = now;
    // The HUD also goes for a moment: a campaign's cutscene hides it.
    static DWORD outSince = 0;
    if (!MenuInBattle()) {
        if (!outSince) outSince = now;
        if ((g_mode || g_wave) && now - outSince > 8000) Finish();
        return;
    }
    outSince = 0;
    if (g_mode && !MenuOverlay()) {
        g_remaining -= dt;
        if (g_remaining <= 30000 && !g_warned30) { g_warned30 = true; Warn(); }
        if (g_remaining <= 10000 && !g_warned10) { g_warned10 = true; Speak(L"10 секунд", false); }
        if (g_remaining <= 0) {
            StartWave(false);
            g_remaining = kDifficulties[g_difficulty].everySeconds * 1000L;
        }
    }
    if (g_wave && now - g_lastUpdate > 5000 && !g_updating.exchange(true)) {
        g_lastUpdate = now;
        bool hunt = ++g_updates % 4 == 0;
        RunOnGameThread([hunt] {
            UpdateUnits(hunt);
            g_updating = false;
        });
        int alive = g_alive;
        float nearest = g_nearest;
        if (g_unpaid && alive == 0 && nearest > 0) {
            WaveCleared();
        } else if (alive) {
            if (hunt) {
                wchar_t b[90];
                swprintf_s(b, L"waves: %d units, nearest %.0f m from the base", alive, nearest / 10.f);
                LogLine(b);
            }
            if (nearest < 600.f && !g_near60) {
                g_near60 = g_near150 = true;
                Speak(L"Враги у базы: " + ScannerMetres(nearest) + L", " + ScannerDirection(g_nearX - g_baseX, g_nearY - g_baseY) + L".", false);
            } else if (nearest < 1500.f && !g_near150) {
                g_near150 = true;
                Speak(L"Враги подходят: " + ScannerMetres(nearest) + L", " + ScannerDirection(g_nearX - g_baseX, g_nearY - g_baseY) + L".", false);
            }
        }
    }
}

bool WavesOn() { return g_mode; }

std::wstring WavesDifficultyName() { return kDifficulties[g_difficulty].name; }

void WavesToggle() {
    g_mode = !g_mode;
    if (g_mode) {
        g_remaining = kDifficulties[g_difficulty].firstSeconds * 1000L;
        g_warned30 = g_warned10 = false;
        LogLine(L"waves: on");
        Speak(L"Осада началась, сложность " + WavesDifficultyName() + L". Первая волна через " +
                  std::to_wstring(kDifficulties[g_difficulty].firstSeconds / 60) + L" минуты. Стройте оборону.", true);
    } else {
        LogLine(L"waves: off");
        Speak(L"Новых волн не будет", true);
    }
}

void WavesNextDifficulty() {
    g_difficulty = (g_difficulty + 1) % 3;
    Speak(L"Сложность: " + WavesDifficultyName(), true);
}

void WavesCallNow() {
    StartWave(g_mode);
    if (g_mode) g_remaining = kDifficulties[g_difficulty].everySeconds * 1000L;
}

void WavesSpeakStatus() {
    std::wstring w = g_mode ? L"Осада идёт, сложность " + WavesDifficultyName() + L". " : std::wstring(L"Осада выключена. ");
    if (g_mode) w += L"Волна " + std::to_wstring(g_wave + 1) + L" через " + std::to_wstring((std::max)(g_remaining, 0L) / 1000) + L" секунд. ";
    w += L"Врагов на поле: " + std::to_wstring((int)g_alive) + L". Отбито волн: " + std::to_wstring(g_cleared) +
         L". Рекорд: " + std::to_wstring(g_record[g_difficulty]) + L".";
    Speak(w, true);
}
