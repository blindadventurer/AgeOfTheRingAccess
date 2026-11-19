#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "commands.h"
#include "gamethread.h"
#include "log.h"
#include "menu.h"
#include "objects_db.h"
#include "scanner.h"
#include "speech.h"
#include "tactics.h"
#include "wotr.h"

namespace {

constexpr float kAlert = 700.f;      // 70 m from an own object: an enemy is coming
constexpr float kLeave = 1050.f;     // 105 m: it went away and may be told of again
constexpr float kGroup = 400.f;      // 40 m: one group in a report
constexpr float kSummary = 1500.f;   // 150 m: the summary and the target roles
constexpr DWORD kReportGap = 6000;   // a new report at most every 6 s

enum Role { kHero, kSiege, kMonster, kHorseArcher, kCavalry, kPike, kArcher, kInfantry, kOther, kRoles };
// Genitive plural after "N отрядов" for the troops; the plain words for the summary's headings.
const wchar_t* kOf[kRoles] = {L"героев", L"осадных машин", L"монстров", L"конных лучников", L"конницы", L"копейщиков",
                              L"лучников", L"пехоты", L"прочих"};
const wchar_t* kHeading[kRoles] = {L"герои", L"осадные машины", L"монстры", L"конные лучники", L"конница", L"копейщики",
                                   L"лучники", L"пехота", L"прочие"};

bool g_auto = true;
DWORD g_lastReport = 0;
std::set<uint32_t> g_told;  // enemies already reported and still near
int g_role = -1;            // chosen target role, -1: any
uint32_t g_armedTarget = 0; // cavalry on pikes: the target of the first press
DWORD g_armedAt = 0;
bool g_keyDown[256] = {};

bool Pressed(int vk) {
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool edge = down && !g_keyDown[vk];
    g_keyDown[vk] = down;
    return edge;
}

// -1: no fighting unit (a sheep, a sign), else its role.
int RoleOf(const std::string& tmpl) {
    const ObjectInfo* info = ObjectsDbFind(tmpl);
    if (!info) return kOther;
    if (info->Has("HERO")) return kHero;
    if (info->Has("SIEGEENGINE")) return kSiege;
    if (info->Has("MONSTER") || info->Has("BIG_MONSTER") || info->Has("TROLL") || info->Has("HORDE_MONSTER")) return kMonster;
    if (info->Has("CAVALRY")) return info->Has("ARCHER") ? kHorseArcher : kCavalry;
    if (info->Has("PIKE")) return kPike;
    if (info->Has("ARCHER")) return kArcher;
    if (info->Has("INFANTRY")) return kInfantry;
    return info->Has("CAN_ATTACK") ? kOther : -1;
}

bool Named(int role) { return role == kHero || role == kSiege || role == kMonster; }

std::wstring Troops(int n) {
    int d = n % 10, h = n % 100;
    const wchar_t* w = (d == 1 && h != 11) ? L"отряд" : (d >= 2 && d <= 4 && (h < 12 || h > 14)) ? L"отряда" : L"отрядов";
    return std::to_wstring(n) + L" " + w;
}

float Dist(float ax, float ay, float bx, float by) { return std::hypot(ax - bx, ay - by); }

struct Foe {
    const TacticalUnit* u;
    int role;
};

// "3 отряда копейщиков, 1 отряд конницы, монстры: Cave Troll 2, герои: Gothmog"
std::wstring Composition(const std::vector<Foe>& foes) {
    std::map<int, int> troops;
    std::map<int, std::map<std::wstring, int>> named;
    for (const Foe& f : foes) {
        if (Named(f.role)) ++named[f.role][f.u->name];
        else ++troops[f.role];
    }
    std::wstring out;
    auto add = [&out](const std::wstring& part) { out += (out.empty() ? L"" : L", ") + part; };
    for (int r : {kHero, kMonster, kSiege}) {
        auto it = named.find(r);
        if (it == named.end()) continue;
        std::wstring names;
        for (const auto& kv : it->second)
            names += (names.empty() ? L"" : L", ") + kv.first + (kv.second > 1 ? L" " + std::to_wstring(kv.second) : L"");
        add(std::wstring(kHeading[r]) + L": " + names);
    }
    for (int r : {kCavalry, kHorseArcher, kPike, kArcher, kInfantry, kOther}) {
        auto it = troops.find(r);
        if (it != troops.end()) add(Troops(it->second) + L" " + kOf[r]);
    }
    return out;
}

struct Picture {
    std::vector<TacticalUnit> all;
    std::vector<const TacticalUnit*> own;
    std::vector<Foe> foes;  // seen, fighting
};

Picture Look() {
    Picture p;
    p.all = ScannerTactical();
    for (const TacticalUnit& u : p.all) {
        if (!u.enemy) { p.own.push_back(&u); continue; }
        if (!u.seen) continue;
        int role = RoleOf(u.tmpl);
        if (role >= 0) p.foes.push_back({&u, role});
    }
    return p;
}

const TacticalUnit* NearestOwn(const Picture& p, float x, float y, float& d) {
    const TacticalUnit* best = nullptr;
    d = 1e9f;
    for (const TacticalUnit* o : p.own) {
        float k = Dist(o->x, o->y, x, y);
        if (k < d) d = k, best = o;
    }
    return best;
}

// Where distances are measured from: the first selected unit, else none (each enemy from the
// own object nearest to it).
bool Reference(float& x, float& y) {
    std::vector<uint32_t> sel = ScannerSelection();
    float z;
    for (uint32_t o : sel)
        if (ScannerPosition(o, x, y, z)) return true;
    return false;
}

std::wstring Where(float fromX, float fromY, float x, float y) {
    return ScannerMetres(Dist(fromX, fromY, x, y)) + L", " + ScannerDirection(x - fromX, y - fromY);
}

void AutoReport() {
    DWORD now = GetTickCount();
    Picture p = Look();
    if (p.own.empty()) return;
    std::vector<std::pair<float, const Foe*>> coming;
    std::set<uint32_t> nearby;
    for (const Foe& f : p.foes) {
        float d;
        NearestOwn(p, f.u->x, f.u->y, d);
        if (d < kLeave) nearby.insert(f.u->addr);
        if (d < kAlert && !g_told.count(f.u->addr)) coming.push_back({d, &f});
    }
    // Forget who went away (or died), so a second approach is told of again.
    for (auto it = g_told.begin(); it != g_told.end();) it = nearby.count(*it) ? std::next(it) : g_told.erase(it);
    if (coming.empty() || now - g_lastReport < kReportGap) return;
    std::sort(coming.begin(), coming.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    const Foe* seed = coming[0].second;
    std::vector<Foe> group;
    int others = 0;
    for (const auto& c : coming) {
        if (Dist(c.second->u->x, c.second->u->y, seed->u->x, seed->u->y) < kGroup) group.push_back(*c.second);
        else ++others;
        g_told.insert(c.second->u->addr);
    }
    float d;
    const TacticalUnit* at = NearestOwn(p, seed->u->x, seed->u->y, d);
    std::wstring t = L"Враги у " + at->name + L", " + Where(at->x, at->y, seed->u->x, seed->u->y) + L": " + Composition(group) + L".";
    if (others) t += L" И ещё " + Troops(others) + L" в других местах.";
    g_lastReport = now;
    Speak(t, false);
    LogLine(L"tactics: report (" + std::to_wstring(p.foes.size()) + L" enemies seen of " +
            std::to_wstring(std::count_if(p.all.begin(), p.all.end(), [](const TacticalUnit& u) { return u.enemy; })) +
            L") -> " + t);
}

// Seen enemies within 150 m of the reference (else of any own object), by role.
std::map<int, std::vector<std::pair<float, const Foe*>>> Around(const Picture& p, bool& fromSelection, float& rx, float& ry) {
    fromSelection = Reference(rx, ry);
    std::map<int, std::vector<std::pair<float, const Foe*>>> by;
    for (const Foe& f : p.foes) {
        float d;
        if (fromSelection) d = Dist(rx, ry, f.u->x, f.u->y);
        else NearestOwn(p, f.u->x, f.u->y, d);
        if (d < kSummary) by[f.role].push_back({d, &f});
    }
    for (auto& kv : by)
        std::sort(kv.second.begin(), kv.second.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return by;
}

// How far and where the enemy is, from the reference or from the own object nearest to it.
std::wstring Place(const Picture& p, bool fromSelection, float rx, float ry, const TacticalUnit* u) {
    if (fromSelection) return Where(rx, ry, u->x, u->y);
    float d;
    const TacticalUnit* at = NearestOwn(p, u->x, u->y, d);
    return at ? Where(at->x, at->y, u->x, u->y) + L" от " + at->name : std::wstring();
}

void Summary() {
    Picture p = Look();
    bool fromSel;
    float rx = 0, ry = 0;
    auto by = Around(p, fromSel, rx, ry);
    if (by.empty()) {
        // The nearest seen enemy beyond the summary's reach, if any.
        const Foe* best = nullptr;
        float bd = 1e9f;
        for (const Foe& f : p.foes) {
            float d;
            if (fromSel) d = Dist(rx, ry, f.u->x, f.u->y);
            else NearestOwn(p, f.u->x, f.u->y, d);
            if (d < bd) bd = d, best = &f;
        }
        Speak(best ? L"Рядом врагов нет. Ближайший: " + best->u->name + L", " + Place(p, fromSel, rx, ry, best->u)
                   : L"Врагов не видно", true);
        return;
    }
    int total = 0;
    for (const auto& kv : by) total += (int)kv.second.size();
    std::wstring t = L"Рядом врагов: " + std::to_wstring(total) + (fromSel ? L", от выделенных" : L"") + L". ";
    for (int r = 0; r < kRoles; ++r) {
        auto it = by.find(r);
        if (it == by.end()) continue;
        const TacticalUnit* first = it->second[0].second->u;
        if (Named(r)) {
            std::vector<Foe> list;
            for (const auto& e : it->second) list.push_back(*e.second);
            t += Composition(list) + L"; ближе всех " + first->name + L", " + Place(p, fromSel, rx, ry, first) + L". ";
        } else {
            t += std::wstring(kHeading[r]) + L": " + std::to_wstring(it->second.size()) + L", ближайший " +
                 Place(p, fromSel, rx, ry, first) + L". ";
        }
    }
    Speak(t, true);
    LogLine(L"tactics: summary -> " + t);
}

void NextRole() {
    Picture p = Look();
    bool fromSel;
    float rx = 0, ry = 0;
    auto by = Around(p, fromSel, rx, ry);
    if (by.empty()) { g_role = -1; Speak(L"Целей рядом нет", true); return; }
    // The next role present after the current one; "any" comes after the last.
    int next = -1;
    for (const auto& kv : by)
        if (kv.first > g_role) { next = kv.first; break; }
    g_role = next;
    if (g_role < 0) { Speak(L"Цель: любой ближайший враг", true); return; }
    const auto& list = by[g_role];
    const TacticalUnit* first = list[0].second->u;
    Speak(L"Цель: " + std::wstring(kHeading[g_role]) + L", " + std::to_wstring(list.size()) + L". Ближайший " + first->name +
              L", " + Place(p, fromSel, rx, ry, first), true);
}

void Attack() {
    std::vector<uint32_t> sel = ScannerSelection();
    if (sel.empty()) { Speak(L"Выделите отряды", true); return; }
    Picture p = Look();
    float rx = 0, ry = 0;
    if (!Reference(rx, ry)) { Speak(L"Не знаю, где выделенные", true); return; }
    const Foe* best = nullptr;
    float bd = 1e9f;
    for (const Foe& f : p.foes) {
        if (g_role >= 0 && f.role != g_role) continue;
        float d = Dist(rx, ry, f.u->x, f.u->y);
        if (d < bd) bd = d, best = &f;
    }
    if (!best) {
        Speak(g_role >= 0 ? L"Не вижу врагов: " + std::wstring(kHeading[g_role]) : L"Не вижу врагов", true);
        return;
    }
    // The clearest mistake: horsemen charging into spears.
    bool cavalry = false;
    for (uint32_t o : sel) {
        int r = RoleOf(ScannerTemplateName(o));
        cavalry |= r == kCavalry || r == kHorseArcher;
    }
    DWORD now = GetTickCount();
    if (cavalry && best->role == kPike && !(g_armedTarget == best->u->addr && now - g_armedAt < 4000)) {
        g_armedTarget = best->u->addr;
        g_armedAt = now;
        Speak(L"Осторожно: конница против копейщиков, " + best->u->name + L". Нажмите ещё раз, чтобы атаковать.", true);
        return;
    }
    g_armedTarget = 0;
    std::wstring where = Where(rx, ry, best->u->x, best->u->y);
    bool sent = AttackObject(best->u->addr, sel);
    LogLine(L"tactics: attack " + best->u->name + L" role " + std::to_wstring(best->role) + (sent ? L" sent" : L" refused"));
    Speak(sent ? L"Атака: " + best->u->name + L", " + where : L"Выделенные не могут атаковать " + best->u->name, true);
}

}  // namespace

void TacticsTick() {
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (now - last < 1000) return;
    last = now;
    if (!g_auto || !MenuInBattle() || MenuOverlay() || WotrActive()) return;
    AutoReport();
}

void TacticsKeys(bool gameFocused) {
    bool comma = Pressed(VK_OEM_COMMA);
    if (!comma || !gameFocused || !MenuInBattle() || MenuOverlay() || CommandsActive()) return;
    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (ctrl && shift) {
        g_auto = !g_auto;
        g_told.clear();
        Speak(g_auto ? L"Сводки о врагах включены" : L"Сводки о врагах выключены", true);
        LogLine(g_auto ? L"tactics: auto reports on" : L"tactics: auto reports off");
    } else if (ctrl) {
        NextRole();
    } else if (shift) {
        Attack();
    } else {
        Summary();
    }
}
