// Command panel mode: see commands.h.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "commands.h"
#include "gadgets.h"
#include "game.h"
#include "gamethread.h"
#include "log.h"
#include "memory.h"
#include "menu.h"
#include "objects_db.h"
#include "scanner.h"
#include "speech.h"
#include "spells.h"
#include "strings.h"
#include "training.h"

namespace {

// TheControlBar: InGameUI::init stores the new ControlBar (constructor 0x720302, vtable
// 0x00C23300) at 0xDE7744 (0x69F032 and 0x6A02F2, disassembly 2026-11-01). Its layout is not
// known yet: the research scan below looks for the CommandButtons it holds.
constexpr uintptr_t kTheControlBar = 0xDE7744;
// The 33 command windows at TheControlBar +0xDC, one per slot of the set (0x92FF94; Worldbuilder
// AptInGameSideCommandBar::UpdateButtonVisiblity 0x13E26EA). A window's CommandButton is
// [[window +0x2C] +0x14] (0x72979C), the button's name at +0x10 (log "controlbar ... +16 Command_").
// The menus of a fortress are Radial = Yes buttons: windows around the building, not the
// palantir's CommandButtons (log 2026-11-05 12:44:53: places 1 and 2, Heroes and Upgrades, missing).
constexpr uint32_t kCommandWindows = 0xDC;
constexpr int kCommandWindowCount = 33;
constexpr uint32_t WIN_ENABLED = 0x08, WIN_HIDDEN = 0x10;

struct Item {
    int slot = 0;
    std::string button;
    const CommandButtonInfo* info = nullptr;
    std::wstring label, details;  // details: a hired hero's own description
    wchar_t hotkey = 0;
    int cost = -1;  // a hero's
};

bool g_active = false;
int g_group = 0;  // 0: the selected object's buttons, 1: the spell book
// A menu button's list (PUSH_VISIBLE_COMMAND_RANGE): the ring shows slots start+1 .. start+count
// from its first place, the rest of the set is out of reach until Back. -1: the main buttons.
int g_rangeStart = -1, g_rangeCount = 0;
std::wstring g_rangeTitle;
std::vector<Item> g_items;
int g_index = 0;
uint32_t g_owner = 0;
std::string g_set;
bool g_aiming = false;
std::vector<ScanTarget> g_targets;
int g_target = 0;
Item g_aimItem;
DWORD g_aimStart = 0;
std::set<std::string> g_researched;  // command sets already logged for research
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

std::string WindowButton(uint32_t win) {
    uint32_t data = U32(win + 0x2C), button = Ptr(data) ? U32(data + 0x14) : 0;
    return Ptr(button) ? game::ReadAsciiString(U32(button + 0x10)) : std::string();
}

// The command window showing this button; one that is not hidden wins over a stale one.
uint32_t WindowOf(const std::string& button, uint32_t& status) {
    uint32_t cb = U32(kTheControlBar), found = 0;
    status = 0;
    for (int i = 0; Ptr(cb) && i < kCommandWindowCount; ++i) {
        uint32_t win = U32(cb + kCommandWindows + i * 4);
        if (!Ptr(win) || _stricmp(WindowButton(win).c_str(), button.c_str()) != 0) continue;
        uint32_t s = U32(win + game::kWinStatus);
        if (!found || (status & WIN_HIDDEN)) { found = win; status = s; }
    }
    return found;
}

// Hidden or disabled by the game: no hero to revive, an upgrade already bought, no money.
bool Unavailable(const Item& it) {
    uint32_t status = 0;
    if (!WindowOf(it.button, status)) return false;
    return !(status & WIN_ENABLED) || ((status & WIN_HIDDEN) && it.info->Has("HIDE_WHILE_DISABLED"));
}

// The object the buttons belong to: the selected one for the spell book's group.
uint32_t Caster() {
    if (g_group != 1) return g_owner;
    std::vector<uint32_t> sel = ScannerSelection();
    return sel.empty() ? 0 : sel[0];
}

// Extinguisher feedback and research (player 2026-11-15: two fires of Grey Company's field 3 stayed
// through several shots, log 22:45-22:54, while every other field went out with one). After the
// order the mod watches the fire proxies within 170 of the aim, the caster and the button for 15 s,
// logs each change and says the outcome: the player cannot see whether the cart fired at all.
struct FireWatch {
    bool on = false, grey = false, used = false;
    DWORD start = 0, last = 0;
    uint32_t cart = 0;
    Item item;
    float tx = 0, ty = 0, cx = 0, cy = 0;
    std::vector<uint32_t> fires;
    std::vector<bool> out;
};
FireWatch g_fw;

bool IsFireProxy(uint32_t p) {
    std::string n = ScannerTemplateName(p);
    for (char& c : n) c = (char)toupper((unsigned char)c);
    return n.find("PARTICLESYSPROXY") != std::string::npos;
}

void FireWatchStart(const Item& it, uint32_t cart, float x, float y) {
    if (it.button.find("ExtinguishFire") == std::string::npos) return;
    g_fw = FireWatch();
    g_fw.on = true;
    g_fw.start = g_fw.last = GetTickCount();
    g_fw.cart = cart;
    g_fw.item = it;
    g_fw.tx = x, g_fw.ty = y;
    g_fw.grey = Unavailable(it);
    wchar_t b[200];
    ScannerForEachObject([&](uint32_t p) {
        if (!IsFireProxy(p)) return;
        float fx, fy, fz;
        if (!ScannerPosition(p, fx, fy, fz)) return;
        float d = std::hypot(fx - x, fy - y);
        if (d > 170.f) return;
        g_fw.fires.push_back(p);
        g_fw.out.push_back(false);
        swprintf_s(b, L"commands: fire watch: proxy %08X %S at (%.0f, %.0f, %.0f), %.0f from the aim, dead %d", p,
                   ScannerTemplateName(p).c_str(), fx, fy, fz, d, ScannerObjectDead(p) ? 1 : 0);
        LogLine(b);
    });
    float cz = 0;
    if (cart && ScannerPosition(cart, g_fw.cx, g_fw.cy, cz)) {
        swprintf_s(b, L"commands: fire watch: caster %08X at (%.0f, %.0f, %.0f), aim (%.0f, %.0f) %.0f away, button grey %d",
                   cart, g_fw.cx, g_fw.cy, cz, x, y, std::hypot(g_fw.cx - x, g_fw.cy - y), g_fw.grey ? 1 : 0);
        LogLine(b);
    }
}

void FireWatchTick() {
    if (!g_fw.on) return;
    DWORD now = GetTickCount();
    if (now - g_fw.last < 500) return;
    g_fw.last = now;
    float t = (now - g_fw.start) / 1000.f;
    wchar_t b[200];
    std::set<uint32_t> present;
    ScannerForEachObject([&](uint32_t p) { present.insert(p); });
    size_t gone = 0;
    for (size_t i = 0; i < g_fw.fires.size(); ++i) {
        uint32_t p = g_fw.fires[i];
        if (!g_fw.out[i] && (!present.count(p) || ScannerObjectDead(p) || !IsFireProxy(p))) {
            g_fw.out[i] = true;
            swprintf_s(b, L"commands: fire watch: proxy %08X out after %.1f s", p, t);
            LogLine(b);
        }
        if (g_fw.out[i]) ++gone;
    }
    bool grey = Unavailable(g_fw.item);
    if (grey != g_fw.grey) {
        g_fw.grey = grey;
        if (grey) g_fw.used = true;
        swprintf_s(b, L"commands: fire watch: button grey %d after %.1f s", grey ? 1 : 0, t);
        LogLine(b);
    }
    float x, y, z;
    if (g_fw.cart && present.count(g_fw.cart) && ScannerPosition(g_fw.cart, x, y, z) &&
        std::hypot(x - g_fw.cx, y - g_fw.cy) > 10.f) {
        g_fw.cx = x, g_fw.cy = y;
        swprintf_s(b, L"commands: fire watch: caster at (%.0f, %.0f, %.0f) after %.1f s, %.0f from the aim", x, y, z, t,
                   std::hypot(x - g_fw.tx, y - g_fw.ty));
        LogLine(b);
    }
    if (t < 15.f && (g_fw.fires.empty() || gone < g_fw.fires.size())) return;
    g_fw.on = false;
    size_t left = g_fw.fires.size() - gone;
    swprintf_s(b, L"commands: fire watch: end after %.1f s, %zu of %zu out, water spent %d", t, gone,
               g_fw.fires.size(), g_fw.used ? 1 : 0);
    LogLine(b);
    if (g_fw.fires.empty()) return;
    std::wstring s;
    if (gone)
        s = L"Погасло огней: " + std::to_wstring(gone) + L". " +
            (left ? L"Здесь ещё горит: " + std::to_wstring(left) : std::wstring(L"Здесь огня больше нет"));
    else if (g_fw.used)
        s = L"Струя была, но ни один огонь не погас";
    else
        s = L"Телега не выстрелила за 15 секунд";
    Speak(s, false);
}

// Auto-extinguish, Grey Company, Shire (player 2026-11-16: putting the fields out by hand is dull;
// the farmer should do it alone while the player guards him). Proven by the logs of 2026-11-15/16:
// the cart's WeaponFireSpecialAbilityUpdate never drives to a place out of StartAbilityRange (115,
// gamedata.ini): field 3's last fire at (627, 4628) stayed through every order given 162-172 away,
// and the fields that went out were shot after the cart was walked to them. Each shot spends the
// water (ReloadTime 200 s in the mission) and the pond ("Shires - Fires - Pool Run") refills it.
// So: drive within reach (MoveObjectTo, the cart's own AI, no selection), shoot from the cart
// (IssuePowerFrom with the cart as source, no selection), then refill at the pond, and again.
// The CommandButton comes from the control bar window once the cart has been selected in this
// session: button objects live as long as the game, so a stale window still gives it.
constexpr float kPondX = 552.f, kPondY = 4689.f;  // "Waypoint - Shire - Fields - Water Point" (map)
constexpr float kShotReach = 95.f;                 // under StartAbilityRange 115
constexpr float kStandOff = 70.f;                  // where the cart stops, from the aim point

struct AutoFire {
    bool off = false;            // Ctrl+Shift+F6
    bool announced = false, needSelect = false, allDone = false;
    enum Stage { kIdle, kApproach, kShot, kToPond, kAtPond } stage = kIdle;
    uint32_t cart = 0, button = 0;
    DWORD last = 0, since = 0, moved = 0;
    float ax = 0, ay = 0, az = 0;  // aim point
    float lx = 0, ly = 0;          // the cart's last position, for "stopped"
    int attempt = 0, failedShots = 0, alive = 0;
    // Shot points for this fire, tried in turn after a shot that put nothing out (see AutoFireAims).
    std::vector<std::pair<float, float>> aims;
    int aimTry = 0;
    std::wstring name;
    std::vector<uint32_t> inReach; // live proxies within reach of the aim at the shot
    std::map<std::wstring, DWORD> skip;  // field -> until when it is left alone
};
AutoFire g_af;

std::vector<uint32_t> LiveFires() {
    std::vector<uint32_t> out;
    ScannerForEachObject([&](uint32_t p) {
        if (IsFireProxy(p) && !ScannerObjectDead(p)) out.push_back(p);
    });
    return out;
}

void AutoFireMove(float x, float y, float z) {
    MoveObjectTo(g_af.cart, x, y, z);
    g_af.since = g_af.moved = GetTickCount();
    float cz;
    ScannerPosition(g_af.cart, g_af.lx, g_af.ly, cz);
}

// The approach point for this attempt: on the cart's side of the aim first, then turned by
// 60, -60, 120, -120 and 180 degrees (a hedge or the pond can stand in the way).
void AutoFireApproach() {
    float cx, cy, cz;
    ScannerPosition(g_af.cart, cx, cy, cz);
    float px = g_af.ax, py = g_af.ay;  // toward the shot point being tried
    if (g_af.aimTry < (int)g_af.aims.size()) px = g_af.aims[g_af.aimTry].first, py = g_af.aims[g_af.aimTry].second;
    float base = std::atan2(cy - py, cx - px);
    static const float kTurn[] = {0.f, 60.f, -60.f, 120.f, -120.f, 180.f};
    float a = base + kTurn[g_af.attempt % 6] * 3.14159265f / 180.f;
    float sx = px + kStandOff * std::cos(a), sy = py + kStandOff * std::sin(a);
    wchar_t b[200];
    swprintf_s(b, L"autofire: approach %d to %s, aim (%.0f, %.0f, %.0f), stand (%.0f, %.0f)", g_af.attempt + 1,
               g_af.name.c_str(), g_af.ax, g_af.ay, g_af.az, sx, sy);
    LogLine(b);
    AutoFireMove(sx, sy, g_af.az);
    g_af.stage = AutoFire::kApproach;
}

// Where to shoot. Logs of 2026-11-15/16: the API order put out fields 2, 4, 5, 6, 7 (the cart even
// drove 440 units to field 4 by itself), but at (642, 4692) and (627, 4628), field 3's last two fires,
// every API order of two days was accepted (0x411) and nothing went out, while the player's click,
// which lands near the camera's centre and not on the fire, did spend the water and one went out.
// HYPOTHESIS: the point right on those fires is not passable (pond edge, hedge) and the ability gives
// up there. So the aim (ax, ay) comes first, then points 35 and 60 away from it in eight directions,
// the cart's side first, each kept only if every fire within 100 of the aim stays within 95 of it.
void AutoFireAims(const std::vector<uint32_t>& fires, float cx, float cy) {
    std::vector<std::pair<float, float>> group;
    for (uint32_t p : fires) {
        float fx, fy, fz;
        if (ScannerPosition(p, fx, fy, fz) && std::hypot(fx - g_af.ax, fy - g_af.ay) <= 100.f) group.push_back({fx, fy});
    }
    g_af.aims.assign(1, {g_af.ax, g_af.ay});
    float base = std::atan2(cy - g_af.ay, cx - g_af.ax);
    static const float kTurn[] = {0.f, 45.f, -45.f, 90.f, -90.f, 135.f, -135.f, 180.f};
    for (float r : {35.f, 60.f})
        for (float t : kTurn) {
            float a = base + t * 3.14159265f / 180.f, px = g_af.ax + r * std::cos(a), py = g_af.ay + r * std::sin(a);
            bool all = true;
            for (auto& f : group) all = all && std::hypot(f.first - px, f.second - py) <= 95.f;
            if (all) g_af.aims.push_back({px, py});
        }
    g_af.aimTry = 0;
    wchar_t b[160];
    swprintf_s(b, L"autofire: %zu shot points for %s (%zu fires within 100 of the aim)", g_af.aims.size(),
               g_af.name.c_str(), group.size());
    LogLine(b);
}

void AutoFireTick(bool gameFocused) {
    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (Pressed(VK_F6) && gameFocused && ctrl && shift && g_af.cart) {
        g_af.off = !g_af.off;
        g_af.stage = AutoFire::kIdle;
        LogLine(g_af.off ? L"autofire: off" : L"autofire: on");
        Speak(g_af.off ? L"Автотушение выключено" : L"Автотушение включено", true);
    }
    DWORD now = GetTickCount();
    if (now - g_af.last < 500) return;
    g_af.last = now;
    if (!MenuInBattle()) { g_af.cart = 0; return; }
    uint32_t cart = ScannerOwnObjectNamed("CampaignFarmerMaggotPorter");
    if (!cart || ScannerObjectDead(cart)) {
        if (g_af.cart) LogLine(L"autofire: the cart is gone");
        g_af.cart = 0;
        g_af.stage = AutoFire::kIdle;
        return;
    }
    if (cart != g_af.cart) {
        g_af.cart = cart;
        g_af.stage = AutoFire::kIdle;
        g_af.announced = g_af.needSelect = g_af.allDone = false;
    }
    if (g_af.off) return;
    if (!g_af.button) {
        uint32_t status = 0, win = WindowOf("Command_PorterExtinguishFireMaggot", status);
        uint32_t data = win ? U32(win + 0x2C) : 0, button = Ptr(data) ? U32(data + 0x14) : 0;
        if (Ptr(button) && Ptr(U32(button + 0x44))) {
            g_af.button = button;
            wchar_t b[100];
            swprintf_s(b, L"autofire: extinguish button %08X from window %08X", button, win);
            LogLine(b);
        }
    }
    std::vector<uint32_t> fires = LiveFires();
    if (fires.empty()) {
        if (g_af.announced && !g_af.allDone) {
            g_af.allDone = true;
            LogLine(L"autofire: every fire is out");
            Speak(L"Все огни потушены", false);
        }
        g_af.stage = AutoFire::kIdle;
        return;
    }
    if (!g_af.button) {
        if (!g_af.needSelect) {
            g_af.needSelect = true;
            LogLine(L"autofire: waiting for the cart to be selected once");
            Speak(L"Чтобы фермер тушил поля сам, выделите телегу один раз", false);
        }
        return;
    }
    if (!g_af.announced) {
        g_af.announced = true;
        g_af.alive = (int)fires.size();
        LogLine(L"autofire: started, fires " + std::to_wstring(fires.size()));
        Speak(L"Фермер тушит поля сам, защищайте телегу. Огней: " + std::to_wstring(fires.size()) +
                  L". Выключить: Control Shift F6",
              false);
    }
    if ((int)fires.size() < g_af.alive) {
        int gone = g_af.alive - (int)fires.size();
        LogLine(L"autofire: " + std::to_wstring(gone) + L" out, left " + std::to_wstring(fires.size()));
        Speak(L"Погасло огней: " + std::to_wstring(gone) + L", осталось " + std::to_wstring(fires.size()), false);
    }
    g_af.alive = (int)fires.size();

    float cx, cy, cz;
    ScannerPosition(cart, cx, cy, cz);
    if (std::hypot(cx - g_af.lx, cy - g_af.ly) > 5.f) { g_af.lx = cx, g_af.ly = cy, g_af.moved = now; }
    bool stopped = now - g_af.moved > 3000;
    wchar_t b[220];

    switch (g_af.stage) {
    case AutoFire::kIdle: {
        // The nearest burning field the guides know, aimed where one jet takes the most fires.
        static DWORD nextLook = 0;
        if (now < nextLook) return;
        nextLook = now + 5000;  // ScannerFires runs a scan and logs a line: not every half second
        std::vector<ScanTarget> t = ScannerFires(cart);
        const ScanTarget* pick = nullptr;
        for (const ScanTarget& x : t) {
            auto s = g_af.skip.find(x.name.substr(0, x.name.find(L", огней")));
            if (s != g_af.skip.end() && now < s->second) continue;
            if (x.name.rfind(L"потушить", 0) == 0 || x.name.rfind(L"огонь", 0) == 0) { pick = &x; break; }
        }
        if (!pick) return;
        g_af.ax = pick->x, g_af.ay = pick->y, g_af.az = pick->z;
        g_af.name = pick->name;
        g_af.attempt = 0;
        g_af.failedShots = 0;
        AutoFireAims(fires, cx, cy);
        if (std::hypot(cx - g_af.aims[0].first, cy - g_af.aims[0].second) <= kShotReach) { g_af.stage = AutoFire::kShot; g_af.since = 0; }
        else AutoFireApproach();
        return;
    }
    case AutoFire::kApproach: {
        float d = std::hypot(cx - g_af.aims[g_af.aimTry].first, cy - g_af.aims[g_af.aimTry].second);
        if (d <= kShotReach) { g_af.stage = AutoFire::kShot; g_af.since = 0; return; }
        if (stopped && now - g_af.since > 4000) {
            swprintf_s(b, L"autofire: stopped at (%.0f, %.0f), %.0f from the aim", cx, cy, d);
            LogLine(b);
            if (++g_af.attempt >= 6) {
                std::wstring field = g_af.name.substr(0, g_af.name.find(L", огней"));
                g_af.skip[field] = now + 120000;
                LogLine(L"autofire: no way to reach " + g_af.name + L", left for 2 minutes");
                Speak(L"Телега не может подъехать: " + field, false);
                g_af.stage = AutoFire::kIdle;
                return;
            }
            AutoFireApproach();
        } else if (now - g_af.since > 60000) {
            LogLine(L"autofire: approach took over a minute");
            g_af.attempt++;
            AutoFireApproach();
        }
        return;
    }
    case AutoFire::kShot: {
        float sx = g_af.aims[g_af.aimTry].first, sy = g_af.aims[g_af.aimTry].second;
        if (!g_af.since) {
            g_af.inReach.clear();
            for (uint32_t p : fires) {
                float fx, fy, fz;
                if (ScannerPosition(p, fx, fy, fz) && std::hypot(fx - sx, fy - sy) <= 100.f) g_af.inReach.push_back(p);
            }
            int r = IssuePowerFrom(g_af.button, cart, sx, sy, g_af.az);
            swprintf_s(b, L"autofire: shot %d of %zu at %s, point (%.0f, %.0f), from (%.0f, %.0f), %.0f away, %zu fires in reach -> 0x%X",
                       g_af.aimTry + 1, g_af.aims.size(), g_af.name.c_str(), sx, sy, cx, cy, std::hypot(cx - sx, cy - sy),
                       g_af.inReach.size(), r);
            LogLine(b);
            g_af.since = now;
            if (r <= 0) { g_af.stage = AutoFire::kIdle; return; }
            return;
        }
        std::set<uint32_t> live(fires.begin(), fires.end());
        size_t out = 0;
        for (uint32_t p : g_af.inReach) out += live.count(p) ? 0 : 1;
        if (out > 0 || now - g_af.since > 9000) {
            swprintf_s(b, L"autofire: after the shot %zu of %zu out in %.1f s", out, g_af.inReach.size(), (now - g_af.since) / 1000.f);
            LogLine(b);
            if (out) {
                g_af.attempt = 0, g_af.aimTry = 0;
                g_af.aims.clear();  // a new target is chosen after the pond
            } else if (++g_af.aimTry >= (int)g_af.aims.size()) {
                std::wstring field = g_af.name.substr(0, g_af.name.find(L", огней"));
                g_af.skip[field] = now + 120000;
                g_af.aims.clear();
                LogLine(L"autofire: every shot point failed for " + g_af.name + L", left for 2 minutes");
                Speak(L"Телега не может потушить: " + field, false);
            } else {
                // The player hears nothing of a jet that does not come (2026-11-16: "no sound at all").
                Speak(L"Мимо, телега пробует точку " + std::to_wstring(g_af.aimTry + 1) + L" из " +
                          std::to_wstring(g_af.aims.size()),
                      false);
            }
            // Every jet empties the cart: to the pond, then the next point or fire.
            LogLine(L"autofire: to the pond");
            AutoFireMove(kPondX, kPondY, cz);
            g_af.stage = AutoFire::kToPond;
        }
        return;
    }
    case AutoFire::kToPond: {
        float d = std::hypot(cx - kPondX, cy - kPondY);
        if (d <= 25.f || (stopped && now - g_af.since > 4000 && d <= 60.f)) {
            g_af.stage = AutoFire::kAtPond;
            g_af.since = now;
            swprintf_s(b, L"autofire: at the pond (%.0f, %.0f), %.0f from the water point", cx, cy, d);
            LogLine(b);
        } else if (stopped && now - g_af.since > 4000) {
            swprintf_s(b, L"autofire: stuck on the way to the pond at (%.0f, %.0f), %.0f away; again", cx, cy, d);
            LogLine(b);
            AutoFireMove(kPondX, kPondY, cz);
        }
        return;
    }
    case AutoFire::kAtPond:
        // "Shires - Fires - Pool Reset" re-arms the refill every 5 s.
        if (now - g_af.since > 6000) {
            if (!g_af.aims.empty() && g_af.aimTry > 0) {
                // The same fire, the next shot point.
                float sx = g_af.aims[g_af.aimTry].first, sy = g_af.aims[g_af.aimTry].second;
                if (std::hypot(cx - sx, cy - sy) <= kShotReach) { g_af.stage = AutoFire::kShot; g_af.since = 0; }
                else AutoFireApproach();
            } else {
                g_af.stage = AutoFire::kIdle;
            }
        }
        return;
    }
}

bool PressWindow(const Item& it) {
    uint32_t status = 0, win = WindowOf(it.button, status);
    if (!win) {
        LogLine(L"commands: no window for " + Widen(it.button.c_str()));
        return false;
    }
    bool ran = PressButtonWindow(win);
    wchar_t b[120];
    swprintf_s(b, L"commands: pressed window %08X status %08X %s ", win, status, ran ? L"sent" : L"did not run");
    LogLine(b + Widen(it.button.c_str()));
    return ran;
}

int TargetKinds(const CommandButtonInfo& b) {
    int k = 0;
    if (b.Has("NEED_TARGET_ENEMY_OBJECT")) k |= kTargetEnemy;
    if (b.Has("NEED_TARGET_ALLY_OBJECT")) k |= kTargetAlly;
    if (b.Has("NEED_TARGET_NEUTRAL_OBJECT")) k |= kTargetNeutral;
    return k;
}
bool NeedsTarget(const CommandButtonInfo& b) { return TargetKinds(b) || b.Has("NEED_TARGET_POS"); }

int Cost(const CommandButtonInfo& b) {
    if (b.command == "UNIT_BUILD" || b.command == "DOZER_CONSTRUCT" || b.command == "FOUNDATION_CONSTRUCT") {
        const ObjectInfo* o = ObjectsDbFind(b.object);
        return o ? o->buildCost : -1;
    }
    if (b.command == "OBJECT_UPGRADE" || b.command == "PLAYER_UPGRADE") return UpgradeCost(b.upgrade);
    return -1;
}

std::wstring Words(const Item& it) {
    const CommandButtonInfo& b = *it.info;
    std::wstring w = it.label;
    if (b.Has("NONPRESSABLE")) w += L", пассивное";
    else if (g_rangeStart >= 0) {}  // pressed on the ring, whatever its letter
    else if (it.hotkey) w += std::wstring(L", клавиша ") + it.hotkey;
    else if (b.command == "SPELL_BOOK") w += L", щелчок по книге";
    int cost = it.cost > 0 ? it.cost : Cost(b);
    if (cost > 0) w += L", цена " + std::to_wstring(cost);
    int k = TargetKinds(b);
    std::wstring t;
    if (k & kTargetEnemy) t += L"враг";
    if (k & kTargetAlly) t += std::wstring(t.empty() ? L"" : L" или ") + L"союзник";
    if (k & kTargetNeutral) t += std::wstring(t.empty() ? L"" : L" или ") + L"нейтральный";
    if (b.Has("NEED_TARGET_POS")) t += std::wstring(t.empty() ? L"" : L" или ") + L"место";
    if (!t.empty()) w += L", цель: " + t;
    if (b.autoAbility) w += L", автокаст";
    // The game's own window state: a power on cooldown is disabled too (Legolas's Twin Arrows, status
    // A2260480 without ENABLED 0x08 while the rest read 22260488, log 2026-11-14 23:25:10).
    if (b.command != "POP_VISIBLE_COMMAND_RANGE" && !b.Has("NONPRESSABLE") && Unavailable(it)) w += L", сейчас недоступно";
    return w;
}

void SpeakItem(bool interrupt) {
    if (g_items.empty()) return;
    Speak(Words(g_items[g_index]) + L", " + std::to_wstring(g_index + 1) + L" из " + std::to_wstring(g_items.size()),
          interrupt);
}

// Logged once per command set, after the list is spoken: the HUD buttons and the command windows
// with their texts. The deeper scan of every ControlBar pointer that stood here found the windows
// at +0xDC and was then dropped: its ~10^5 guarded reads held the list back 1-1.5 s (log 2026-11-09).
void Research() {
    std::string key = g_set + "#" + std::to_string(g_group) + "#" + std::to_string(g_rangeStart);
    if (!g_researched.insert(key).second) return;
    std::vector<HudButton> hud;
    MenuHudButtons(hud);
    std::vector<std::wstring> lines;
    lines.push_back(L"commands: research for " + Widen(g_set.c_str()) + L", " + std::to_wstring(hud.size()) + L" HUD buttons");
    for (const HudButton& h : hud) {
        wchar_t b[60];
        swprintf_s(b, L"  hud (%.1f, %.1f) ", h.x, h.y);
        lines.push_back(b + Widen(h.path.c_str()));
    }
    uint32_t cb = U32(kTheControlBar);
    for (int i = 0; Ptr(cb) && i < kCommandWindowCount; ++i) {
        uint32_t win = U32(cb + kCommandWindows + i * 4);
        std::string name = Ptr(win) ? WindowButton(win) : std::string();
        if (name.empty()) continue;
        wchar_t w[120];
        swprintf_s(w, L"  window %d %08X status %08X region (%d,%d) %dx%d ", i, win, U32(win + game::kWinStatus),
                   (int)U32(win + game::kWinRegion), (int)U32(win + game::kWinRegion + 4),
                   (int)U32(win + game::kWinRegion + 8), (int)U32(win + game::kWinRegion + 12));
        lines.push_back(w + Widen(name.c_str()) + L" text \"" + WindowText(win, false) + L"\" tip \"" + WindowText(win, true) + L"\"");
    }
    LogLines(lines);
}

// A CommandSetUpgrade swaps an object's buttons during the mission (Sauron gets his dialogue
// answers through CampaignSauronCommandSet_WhitePillar, campaignsauron.ini): Zero Hour keeps
// that as an AsciiString override in the Object. Its offset is not known in this build, so
// every dword of the object is tried as an AsciiString naming a known command set other than
// the template's; the offset found is logged once (to be pinned after a test).
std::string CommandSetOverride(uint32_t obj, const std::string& templateSet) {
    static int logged = -1;
    for (uint32_t off = 0x08; off < 0x600; off += 4) {
        uint32_t p = U32(obj + off);
        if (!Ptr(p)) continue;
        std::string s = game::ReadAsciiString(p);
        if (s.size() < 6 || s.size() > 80 || _stricmp(s.c_str(), templateSet.c_str()) == 0) continue;
        if (!CommandSetFind(s)) continue;
        if (logged != (int)off) {
            logged = (int)off;
            wchar_t b[200];
            swprintf_s(b, L"commands: command set override at obj+0x%X: %S (template set %S)", off, s.c_str(),
                       templateSet.c_str());
            LogLine(b);
        }
        return s;
    }
    return {};
}

// Heroes of a fortress: the button says only "Revive %s" (the game fills in the name). position:
// its place among the set's revive buttons without the ring hero. False: no hero has this button.
bool NameHero(Item& it, int position) {
    std::string hero = ReviveHero(it.button, position);
    if (hero == "CreateAHero") { it.label = L"Свой герой (Create-a-Hero)"; return true; }
    const ObjectInfo* o = hero.empty() ? nullptr : ObjectsDbFind(hero);
    if (!o) return false;
    it.label = o->displayName.empty() ? Widen(hero.c_str()) : o->displayName;
    it.cost = o->buildCost;
    it.details = o->description;
    if (ScannerOwnObjectNamed(hero.c_str())) it.label += L", уже на поле";
    return true;
}

bool Build(int group, bool keepRange = false) {
    if (!keepRange) g_rangeStart = -1;
    g_items.clear();
    g_index = 0;
    g_group = group;
    g_owner = 0;
    if (group == 0) {
        std::vector<uint32_t> sel = ScannerSelection();
        if (!sel.empty()) g_owner = sel[0];
    } else {
        g_owner = ScannerOwnObjectNamed("SpellBook");
    }
    std::string mission = CommandsUseMission(ScannerObjectiveTags());
    g_set = g_owner ? ScannerCommandSetName(g_owner) : std::string();
    std::string over = g_owner ? CommandSetOverride(g_owner, g_set) : std::string();
    if (!over.empty()) g_set = over;
    LogLine(L"commands: mission " + (mission.empty() ? std::wstring(L"none") : Widen(mission.c_str())));
    const std::map<int, std::string>* set = g_set.empty() ? nullptr : CommandSetFind(g_set);
    if (!set && !g_set.empty() && CommandsUseMapWith(g_set)) set = CommandSetFind(g_set);
    if (set) {
        std::vector<std::pair<int, int>> ranges;  // the menu buttons' lists: first slot, count
        // Revive buttons stand for BuildableHeroesMP by their order in the set, the hidden
        // Command_FakeReviveSlotN too: MenFortressCommandSet's Fake slot is Imrahil's (test
        // 2026-11-05 16:59: GenericReviveSlot7 gave Gandalf, the list's 9th). CreateAHero is the
        // list's first, so a set with no such button starts one further on (HYPOTHESIS).
        std::map<int, int> revivePlace;
        int revives = 1;
        for (const auto& kv : *set) {
            const CommandButtonInfo* b = CommandButtonFind(kv.second);
            if (b && b->command == "PUSH_VISIBLE_COMMAND_RANGE" && b->rangeCount > 0)
                ranges.push_back({b->rangeStart + 1, b->rangeCount});
            if (b && b->command == "REVIVE" && kv.second.find("CreateAHero") != std::string::npos) revives = 0;
        }
        for (const auto& kv : *set) {
            const CommandButtonInfo* b = CommandButtonFind(kv.second);
            if (b && b->command == "REVIVE" && kv.second.find("RingHero") == std::string::npos)
                revivePlace[kv.first] = revives++;
        }
        for (const auto& kv : *set) {
            bool listed = false;
            for (const auto& r : ranges) listed |= kv.first >= r.first && kv.first < r.first + r.second;
            bool here = g_rangeStart >= 0 ? kv.first > g_rangeStart && kv.first <= g_rangeStart + g_rangeCount : !listed;
            if (!here) continue;
            // NEED_UPGRADE Upgrade_MiniHordeLvl10: spacers of the hero menu, never shown.
            if (kv.second.find("FakeRevive") != std::string::npos) continue;
            Item it;
            it.slot = kv.first;
            it.button = kv.second;
            it.info = CommandButtonFind(kv.second);
            if (!it.info) continue;
            it.label = CommandButtonLabel(kv.second, *it.info, it.hotkey);
            if (it.info->command == "REVIVE" && !NameHero(it, revivePlace[kv.first])) continue;
            g_items.push_back(it);
        }
    }
    LogLine(L"commands: group " + std::to_wstring(group) + L" " + Widen(ScannerTemplateName(g_owner).c_str()) + L" set " +
            Widen(g_set.c_str()) + L": " + std::to_wstring(g_items.size()) + L" buttons");
    return !g_items.empty();
}

std::wstring GroupTitle() {
    if (g_rangeStart >= 0) return g_rangeTitle;
    if (g_group == 1) return L"Книга заклинаний";
    return g_owner ? ScannerNameOf(g_owner) : L"Ничего не выделено";
}

void Announce() {
    if (g_items.empty()) {
        Speak(GroupTitle() + (g_owner ? L": кнопок нет" : L""), true);
        return;
    }
    Speak(GroupTitle() + L", кнопок " + std::to_wstring(g_items.size()) + L".", true);
    SpeakItem(false);
}

// The list is spoken first; the research log comes after, so it never delays the speech.
void Show() {
    Announce();
    if (g_owner) Research();
}

void PressLetter(wchar_t letter) {
    UINT vk = (UINT)towupper(letter);
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wScan = in[1].ki.wScan = (WORD)scan;
    in[0].ki.dwFlags = KEYEVENTF_SCANCODE;
    in[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    SendInput(1, &in[0], sizeof(INPUT));
    Sleep(50);
    SendInput(1, &in[1], sizeof(INPUT));
    wchar_t b[60];
    swprintf_s(b, L"commands: pressed %c (scan %02X)", (wchar_t)vk, scan);
    LogLine(b);
}

void SpeakTarget(const std::wstring& prefix) {
    const ScanTarget& t = g_targets[g_target];
    Speak(prefix + L"Цель: " + t.name + L", " + std::to_wstring((int)(t.metres + 0.5f)) + L" м, " + t.direction + L", " +
              std::to_wstring(g_target + 1) + L" из " + std::to_wstring(g_targets.size()),
          true);
}

void StartAiming(const Item& it, const std::wstring& prefix = L"") {
    const CommandButtonInfo& b = *it.info;
    g_targets.clear();
    int kinds = TargetKinds(b);
    bool fire = it.button.find("ExtinguishFire") != std::string::npos;
    if (kinds) g_targets = ScannerTargets(kinds, g_owner);
    if (fire) g_targets = ScannerFires(g_owner);
    if (b.Has("NEED_TARGET_POS")) {
        // A place: the enemies' spots first, then the scanner's current entry.
        if (!kinds && !fire) g_targets = ScannerTargets(kTargetEnemy, g_owner);
        ScanTarget here;
        if (ScannerCurrentPlace(here.x, here.y, here.z, here.name)) {
            here.name = L"текущий объект сканера: " + here.name;
            g_targets.push_back(here);
        }
    }
    g_aimItem = it;
    if (g_targets.empty()) {
        Speak(prefix + it.label + L": целей рядом нет", true);
        return;
    }
    g_aiming = true;
    g_target = 0;
    g_aimStart = GetTickCount();
    SpeakTarget(prefix + it.label + L". ");
}

// Quick cast: what the game's own AI aims this button at (SpecialPowerAIType of the object INI's
// AISpecialPowerUpdate modules), else the button's target options.
struct AutoAim {
    int kinds = kTargetEnemy;
    bool buildings = false;  // structures first (a structure breaker, an economy buff)
    std::string ai;
};

AutoAim AutoAimFor(const Item& it) {
    const CommandButtonInfo& b = *it.info;
    AutoAim a;
    a.ai = ObjectsDbAiPowerType(it.button);
    auto has = [&](const char* w) { return a.ai.find(w) != std::string::npos; };
    static const char* const kAlly[] = {"BASIC_SELF_BUFF", "ASSIST_BATTLE_BUFF", "GIVEXP_AOE", "SELFAOEHEALHEROS",
                                        "SELFAOE_HEALHEROS", "HEAL_AOE", "SPELLBOOK_HEAL", "TRAINARCHERS",
                                        "SPELLBOOK_REBUILD", "BUFFECONOMYBUILDING", "BUFFTERRAIN", "SPELLBOOK_CITADEL"};
    int want = 0;
    if (!a.ai.empty()) {
        want = kTargetEnemy;
        for (const char* w : kAlly)
            if (a.ai.find(w) != std::string::npos && !has("DEBUFF")) want = kTargetAlly;
        if (has("CAPTURE_BUILDING") || has("CAPTURE_CREEP")) want = kTargetNeutral | kTargetEnemy;
        a.buildings = has("STRUCTURE") || has("ECONOMYBUILDING") || has("REBUILD") || has("CAPTURE_BUILDING");
    } else {
        // 678 of the 1528 aimed buttons have no AI module (AotR 10.0.2): their names tell the
        // helping ones apart (Command_AragornAthelas, Command_SpellBookHeal...).
        std::string up = it.button;
        for (auto& ch : up) ch = (char)toupper((unsigned char)ch);
        static const char* const kHelp[] = {"HEAL", "ATHELAS", "BLESS", "RALLY", "INSPIR", "REBUILD", "REPAIR"};
        for (const char* w : kHelp)
            if (up.find(w) != std::string::npos) want = kTargetAlly;
        a.buildings = up.find("REBUILD") != std::string::npos || up.find("REPAIR") != std::string::npos;
    }
    int options = TargetKinds(b);
    if (options) {
        // An object to aim at: the button's own options decide what the game accepts.
        a.kinds = want & options ? want & options : options;
        if ((options & kTargetEnemy) && !want) a.kinds = kTargetEnemy;
    } else {
        a.kinds = want ? want : kTargetEnemy;  // a place only: attacks are the most common
    }
    return a;
}

// Candidates for a quick cast, nearest first: seen before shrouded, the wanted kind of object
// (structures or not) before the rest, within kQuickRange metres. An ally aim adds the caster
// itself at the end: heals and buffs around oneself.
constexpr float kQuickRange = 80.f;
std::vector<ScanTarget> AutoTargets(const AutoAim& a) {
    uint32_t from = g_owner;
    if (g_group == 1) {
        std::vector<uint32_t> sel = ScannerSelection();
        from = sel.empty() ? 0 : sel[0];
    }
    std::vector<ScanTarget> all = ScannerTargets(a.kinds, from), out;
    for (int pass = 0; pass < 4; ++pass)
        for (const ScanTarget& t : all) {
            if (t.metres > kQuickRange) continue;
            bool firstHalf = !t.hidden && t.building == a.buildings;
            bool secondHalf = !t.hidden && t.building != a.buildings;
            bool third = t.hidden && t.building == a.buildings;
            if ((pass == 0 && firstHalf) || (pass == 1 && secondHalf) || (pass == 2 && third) ||
                (pass == 3 && t.hidden && t.building != a.buildings))
                out.push_back(t);
        }
    if ((a.kinds & kTargetAlly) && from) {
        ScanTarget self;
        self.addr = from;
        if (ScannerPosition(from, self.x, self.y, self.z)) {
            self.name = ScannerNameOf(from) + L" (сам)";
            self.mine = true;
            out.push_back(self);
        }
    }
    return out;
}

// Gives the ability the game waits for to the nearest fitting target. The game checks each target
// (issueSpecialPowerCommand returns 0 for a refused one), so the next nearest is tried, up to 8.
void AutoApply(const Item& it) {
    const CommandButtonInfo& b = *it.info;
    AutoAim a = AutoAimFor(it);
    std::vector<ScanTarget> c = AutoTargets(a);
    // An extinguisher has no AI module: the enemy default sent Maggot's cart at the ruffians
    // (log 2026-11-15 21:04:27). Its targets are the fires, nearest first.
    if (it.button.find("ExtinguishFire") != std::string::npos) {
        c = ScannerFires(Caster());
    }
    bool objectAim = TargetKinds(b) != 0;
    wchar_t lb[200];
    swprintf_s(lb, L"commands: quick cast %S ai %S kinds %d%s: %d candidates", it.button.c_str(),
               a.ai.empty() ? "none" : a.ai.c_str(), a.kinds, a.buildings ? L" structures" : L"", (int)c.size());
    LogLine(lb);
    if (c.empty()) {
        StartAiming(it, L"Подходящих целей ближе " + std::to_wstring((int)kQuickRange) + L" метров нет. ");
        return;
    }
    int refused = 0;
    for (const ScanTarget& t : c) {
        if (refused >= 8) break;
        float x = t.x, y = t.y, z = t.z;
        if (t.addr) ScannerPosition(t.addr, x, y, z);
        int kind = -1, sent = -1;
        // The game enters its aim mode a moment after the button's press.
        for (int wait = 0; wait < 4; ++wait) {
            sent = AimPendingAbility(objectAim ? t.addr : 0, x, y, z, &kind);
            if (sent != -1) break;
            Sleep(100);
        }
        std::wstring where = t.name + L", " + std::to_wstring((int)(t.metres + 0.5f)) + L" м" +
                             (t.direction.empty() ? L"" : L", " + t.direction);
        if (sent > 0) {
            LogLine(L"commands: quick cast " + Widen(it.button.c_str()) + L" on " + t.name);
            FireWatchStart(it, Caster(), x, y);
            g_aiming = false;
            Speak(it.label + L": " + where, true);
            return;
        }
        if (sent == 0) { ++refused; continue; }
        if (sent == -1) {
            // Nothing waits for a target: no click, it would select or order something else.
            LogLine(L"commands: quick cast " + Widen(it.button.c_str()) + L": the game waits for no target");
            g_aiming = false;
            Speak(it.label + L": игра не ждёт цель. Возможно, умение ещё перезаряжается", true);
            return;
        }
        // Not a plain special power: the click at the target, as End does.
        bool ok = ScannerClickAt(x, y, z, false);
        if (ok) FireWatchStart(it, Caster(), x, y);
        swprintf_s(lb, L"commands: quick cast %S result %d kind 0x%X, clicked %s", it.button.c_str(), sent, kind,
                   ok ? L"yes" : L"no");
        LogLine(lb + std::wstring(L" ") + t.name);
        g_aiming = false;
        Speak(ok ? it.label + L": " + where : L"Камера не отвечает", true);
        return;
    }
    LogLine(L"commands: quick cast " + Widen(it.button.c_str()) + L": " + std::to_wstring(refused) + L" targets refused");
    StartAiming(it, L"Игра не приняла ближайшие цели. ");
}

void Apply() {
    const ScanTarget& t = g_targets[g_target];
    float x = t.x, y = t.y, z = t.z;
    if (t.addr) ScannerPosition(t.addr, x, y, z);
    // First the game's own command, no mouse: a click missed when something stood over the target.
    int kind = -1;
    int sent = AimPendingAbility(t.addr, x, y, z, &kind);
    if (sent > 0) {
        LogLine(L"commands: aim " + Widen(g_aimItem.button.c_str()) + L" given by the game's command, " + t.name);
        FireWatchStart(g_aimItem, Caster(), x, y);
        g_aiming = false;
        Speak(g_aimItem.label + L": " + t.name, true);
        return;
    }
    if (sent == 0) {
        // The game checked the target and said no: a click would get the same answer. The game
        // still waits for a target, so the aim goes on.
        Speak(L"Игра не принимает эту цель: " + t.name + L". Выберите другую и нажмите End", true);
        return;
    }
    bool ok = ScannerClickAt(x, y, z, false);
    if (ok) FireWatchStart(g_aimItem, Caster(), x, y);
    wchar_t b[160];
    swprintf_s(b, L"commands: aim %s at (%.0f, %.0f) %s", Widen(g_aimItem.button.c_str()).c_str(), x, y,
               ok ? L"clicked" : L"camera did not answer");
    LogLine(b + std::wstring(L" ") + t.name);
    g_aiming = false;
    Speak(ok ? g_aimItem.label + L": " + t.name : L"Камера не отвечает", true);
}

// The game's ring goes to the button's list too, so the list's entries can be pressed there.
void OpenRange(const Item& it) {
    bool pressed = it.hotkey != 0;
    if (pressed) PressLetter(it.hotkey);
    else pressed = PressWindow(it);
    if (!pressed) { Speak(it.label + L": кнопка не найдена", true); return; }
    Sleep(300);
    g_rangeStart = it.info->rangeStart;
    g_rangeCount = it.info->rangeCount;
    g_rangeTitle = it.label;
    Build(g_group, true);
    Show();
}

void CloseRange() {
    for (const Item& it : g_items)
        if (it.info->command == "POP_VISIBLE_COMMAND_RANGE") {
            if (it.hotkey) PressLetter(it.hotkey);
            else PressWindow(it);
        }
    Sleep(200);
    Build(g_group);
    Show();
}

// Ctrl+End holds Ctrl down: the game would take the button's letter as Ctrl+letter and a click
// as a Ctrl+click. A key-up of both Ctrl keys first (scan codes 1D and E0 1D).
void ReleaseCtrl() {
    if (!(GetAsyncKeyState(VK_CONTROL) & 0x8000)) return;
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wScan = in[1].ki.wScan = 0x1D;
    in[0].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    in[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP | KEYEVENTF_EXTENDEDKEY;
    SendInput(2, in, sizeof(INPUT));
    Sleep(30);
    LogLine(L"commands: Ctrl released before the press");
}

// quick: an aimed ability goes to the nearest fitting target at once (Ctrl+End).
void Activate(bool quick = false) {
    if (g_items.empty()) return;
    if (quick) ReleaseCtrl();
    const Item it = g_items[g_index];
    const CommandButtonInfo& b = *it.info;
    if (b.Has("NONPRESSABLE")) { Speak(L"Пассивное умение, нажимать не нужно", true); return; }
    if (b.command == "PUSH_VISIBLE_COMMAND_RANGE") { OpenRange(it); return; }
    if (b.command == "POP_VISIBLE_COMMAND_RANGE") { CloseRange(); return; }
    bool pressed = false;
    if (b.command != "SPELL_BOOK" && Unavailable(it)) {
        LogLine(L"commands: unavailable " + Widen(it.button.c_str()));
        Speak(L"Сейчас недоступно: " + it.label + L". Возможно, перезаряжается", true);
        return;
    }
    if (g_rangeStart < 0 && it.hotkey) {
        PressLetter(it.hotkey);
        pressed = true;
    } else if (b.command == "SPELL_BOOK") {
        // HYPOTHESIS: spell book slot N is the palantir's SpellBookUI.SpellN button.
        pressed = MenuClickHud("SpellBookUI.Spell" + std::to_string(it.slot) + ".", false);
    } else if (Unavailable(it)) {
        LogLine(L"commands: unavailable " + Widen(it.button.c_str()));
        Speak(L"Сейчас недоступно: " + it.label, true);
        return;
    } else {
        // InPalantir buttons also stand on the palantir, at place slot - 1 (log 2026-11-05 12:44:53).
        pressed = PressWindow(it) || MenuClickHud("CommandButtons." + std::to_string(it.slot - 1) + ".", false);
    }
    LogLine(L"commands: activate slot " + std::to_wstring(it.slot) + L" " + Widen(it.button.c_str()) +
            (pressed ? L"" : L" (no way to press it)"));
    if (!pressed) { Speak(it.label + L": кнопка не найдена", true); return; }
    Sleep(150);
    if (NeedsTarget(b) && quick) AutoApply(it);
    else if (NeedsTarget(b)) StartAiming(it);
    else Speak(L"Нажато: " + it.label, true);
}

// Gate levers (Grey Company's barrow, Paths of the Dead grates 01-07): DwarfWaystone, INERT scenery in
// the base INI, made SELECTABLE with CastleGateCommandSet by the mission's map.ini. A script hands it to
// Player_1 when a unit stands by it (in the barrow also: the tomb's spell stone down, no ghosts by the
// main gate levers); its GATE_IS_OPEN opens the gate and the lever goes back to PlyrCivilian within 1 s
// (map scripts read 2026-11-19). Test 2026-11-19 01:11-01:13: the lever was handed over, but the player
// could not find it (the scanner dropped it) and selected spell stones instead. The player chose that
// the mod pulls every lever by itself: select it, press its Command_ToggleGate as command mode does,
// put the selection back (once at once, once more after the gate's camera move, which deselects).
struct AutoLever {
    enum Stage { kIdle, kSelecting, kRestore } stage = kIdle;
    uint32_t lever = 0;
    std::vector<uint32_t> before;
    DWORD since = 0, last = 0;
    bool restored = false;
    std::map<uint32_t, int> tries;
    std::map<uint32_t, DWORD> nextTry;
};
AutoLever g_al;

void AutoLeverRestore() {
    std::vector<uint32_t> keep;
    for (uint32_t p : g_al.before) {
        float x, y, z;
        if (p != g_al.lever && ScannerPosition(p, x, y, z) && !ScannerObjectDead(p)) keep.push_back(p);
    }
    if (!keep.empty()) SelectObjectsDirect(keep);
}

void AutoLeverTick(bool gameFocused) {
    DWORD now = GetTickCount();
    if (now - g_al.last < 250) return;
    g_al.last = now;
    if (!MenuInBattle()) {
        if (g_al.stage != AutoLever::kIdle) LogLine(L"lever: battle HUD gone, stopped");
        g_al.stage = AutoLever::kIdle;
        return;
    }
    wchar_t b[200];
    switch (g_al.stage) {
    case AutoLever::kIdle: {
        if (g_active || g_aiming || TrainingActive()) return;
        uint32_t lever = ScannerOwnObjectNamed("DwarfWaystone");
        if (!lever) return;
        auto nt = g_al.nextTry.find(lever);
        if (nt != g_al.nextTry.end() && now < nt->second) return;
        int& n = g_al.tries[lever];
        if (n >= 3) {
            if (n == 3) {
                ++n;
                LogLine(L"lever: 3 tries and the lever is still ours, given up");
                Speak(L"Рычаг не поддаётся. Он в списке мои: выделите его и нажмите Toggle Gate в командах", false);
            }
            return;
        }
        ++n;
        g_al.lever = lever;
        g_al.before = ScannerSelection();
        float x = 0, y = 0, z = 0;
        ScannerPosition(lever, x, y, z);
        swprintf_s(b, L"lever: %08X at (%.0f, %.0f) is ours, try %d, selection of %zu kept", lever, x, y, n, g_al.before.size());
        LogLine(b);
        SelectObjectDirect(lever, nullptr, nullptr);
        g_al.stage = AutoLever::kSelecting;
        g_al.since = now;
        return;
    }
    case AutoLever::kSelecting: {
        std::vector<uint32_t> sel = ScannerSelection();
        bool selected = sel.size() == 1 && sel[0] == g_al.lever;
        uint32_t status = 0, win = WindowOf("Command_ToggleGate", status);
        bool ready = selected && win && !(status & WIN_HIDDEN) && (status & WIN_ENABLED);
        if (!ready && now - g_al.since < 2000) return;
        bool pressed = false;
        const wchar_t* how = L"nothing";
        if (ready) { pressed = PressButtonWindow(win); how = L"window"; }
        if (!pressed && selected && gameFocused) {
            // Command mode presses a button with a hotkey by its letter (Toggle Gate (&W)).
            wchar_t hotkey = 0;
            const CommandButtonInfo* info = CommandButtonFind("Command_ToggleGate");
            if (info) CommandButtonLabel("Command_ToggleGate", *info, hotkey);
            if (hotkey) { PressLetter(hotkey); pressed = true; how = L"hotkey"; }
        }
        swprintf_s(b, L"lever: selected %d, window %08X status %08X, pressed by %s %s", selected ? 1 : 0, win, status, how,
                   pressed ? L"sent" : L"FAILED");
        LogLine(b);
        if (pressed) Speak(L"Рычаг потянут", false);
        g_al.nextTry[g_al.lever] = now + 6000;  // still ours after 6 s: the press did not take
        g_al.stage = AutoLever::kRestore;
        g_al.since = now;
        g_al.restored = false;
        return;
    }
    case AutoLever::kRestore:
        if (now - g_al.since < 500) return;
        if (!g_al.restored) {
            g_al.restored = true;
            AutoLeverRestore();
            return;
        }
        // The gate's camera move deselects (DESELECT in "Camera Move Start", 2-3 s).
        if (now - g_al.since < 4500) return;
        if (ScannerSelection().empty()) {
            LogLine(L"lever: selection empty after the camera move, put back again");
            AutoLeverRestore();
        }
        g_al.stage = AutoLever::kIdle;
        return;
    }
}

// HYPOTHESIS: ring button CommandButtons.N holds slot N+1 (Tauriel's abilities are slots 1-6).
void ToggleAutocast() {
    if (g_items.empty()) return;
    const Item& it = g_items[g_index];
    if (!it.info->autoAbility) { Speak(it.label + L": автокаста нет", true); return; }
    bool ok = MenuClickHud("CommandButtons." + std::to_string(it.slot - 1) + ".", true);
    Speak(ok ? L"Автокаст переключён: " + it.label : L"Кнопка на палантире не найдена", true);
}

}  // namespace

bool CommandsActive() { return g_active || TrainingActive(); }

void CommandsKeys(bool gameFocused) {
    FireWatchTick();
    AutoFireTick(gameFocused);
    AutoLeverTick(gameFocused);
    if (TrainingActive()) return;
    static const int keys[] = {VK_OEM_7, VK_NEXT, VK_PRIOR, VK_HOME, VK_END};
    if (!gameFocused || !MenuInBattle()) {
        for (int vk : keys) g_keyDown[vk] = false;
        if (g_active && !MenuInBattle()) { g_active = g_aiming = false; LogLine(L"commands: off (battle HUD gone)"); }
        return;
    }
    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    bool toggle = Pressed(VK_OEM_7);
    if (!g_active) {
        if (!toggle) return;
        g_active = true;
        g_aiming = false;
        LogLine(L"commands: on");
        Build(0);
        Show();
        return;
    }
    bool next = Pressed(VK_NEXT), prev = Pressed(VK_PRIOR), home = Pressed(VK_HOME), end = Pressed(VK_END);
    if (g_aiming) {
        if (toggle) {
            // The game waits for a target: a right click cancels it (HYPOTHESIS, as in Zero Hour).
            g_aiming = false;
            if (GetTickCount() - g_aimStart < 60000) {
                POINT pt;
                GetCursorPos(&pt);
                INPUT in[2] = {};
                in[0].type = in[1].type = INPUT_MOUSE;
                in[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
                in[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
                SendInput(2, in, sizeof(INPUT));
                LogLine(L"commands: aim cancelled with a right click");
            }
            Speak(L"Отменено", true);
            return;
        }
        if ((next || prev) && !g_targets.empty()) {
            int n = (int)g_targets.size();
            g_target = (g_target + (next ? 1 : n - 1)) % n;
            SpeakTarget(L"");
        }
        if (home) SpeakTarget(L"");
        if (end && ctrl) { ReleaseCtrl(); AutoApply(g_aimItem); }
        else if (end) Apply();
        return;
    }
    if (toggle && g_rangeStart >= 0) { CloseRange(); return; }
    if (toggle) {
        g_active = false;
        LogLine(L"commands: off");
        Speak(L"Режим команд выключен", true);
        return;
    }
    if (ctrl && (next || prev)) {
        Build(g_group == 0 ? 1 : 0);
        Show();
        return;
    }
    // A new selection while the mode is on: its buttons.
    if (g_group == 0 && (next || prev || home || end)) {
        std::vector<uint32_t> sel = ScannerSelection();
        uint32_t now = sel.empty() ? 0 : sel[0];
        if (now != g_owner) {
            Build(0);
            Show();
            return;
        }
    }
    if (g_items.empty()) {
        if (next || prev || home || end) Announce();
        return;
    }
    int n = (int)g_items.size();
    if (next) { g_index = (g_index + 1) % n; SpeakItem(true); }
    if (prev) { g_index = (g_index + n - 1) % n; SpeakItem(true); }
    if (home && shift) {
        const Item& cur = g_items[g_index];
        std::wstring d = cur.details.empty() ? CommandButtonDescription(*cur.info) : cur.details;
        Speak(d.empty() ? L"Нет описания" : d, true);
    } else if (home) {
        SpeakItem(true);
    }
    if (end && shift) ToggleAutocast();
    else if (end) Activate(ctrl);
}
