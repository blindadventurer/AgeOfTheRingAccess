// Training menu: see training.h.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "commands.h"
#include "gamethread.h"
#include "log.h"
#include "memory.h"
#include "menu.h"
#include "nocatch.h"
#include "scanner.h"
#include "screens.h"
#include "speech.h"
#include "training.h"
#include "waves.h"

namespace {

// Money::deposit, thiscall on player +0x90: amount, 0, 1, as PLAYER_GIVE_MONEY calls it (0x7BCBAB).
constexpr uintptr_t kMoneyDeposit = 0x7B18B8;
// Adds to the player's command points, thiscall on player +0x60 (0x6A7AE2: add [ecx+0xC]); the
// total the game checks is [+4] + [+0xC] + bonuses (0x6A7B9F).
constexpr uintptr_t kCommandPointsAdd = 0x6A7AE2;
constexpr uint32_t kPlayerMoneyObject = 0x90, kPlayerMoneyAmount = 0x94, kPlayerCommandPoints = 0x60;
// UNIT_AFFECT_OBJECT_PANEL_FLAGS "Indestructible" (0x7C56FD): body at object +0x25C, vt+0x88 takes a bool.
constexpr uint32_t kObjBody = 0x25C, kBodySetIndestructible = 0x88;
constexpr int kMoney = 5000, kPoints = 1000;
// Object::kill, thiscall: 8, 0, as NAMED_KILL calls it (action 75, case 0x7CC52B -> 0x7BBF53).
constexpr uintptr_t kObjectKill = 0x698EC3;

using Deposit = void(__thiscall*)(void*, uint32_t, int, int);
using AddPoints = void(__thiscall*)(void*, int);
using SetIndestructible = void(__thiscall*)(void*, int);
using KillObject = void(__thiscall*)(void*, int, int);

constexpr int kItems = 10;
bool g_open = false, g_immortal = false, g_network = false;
int g_index = 0;
bool g_down[256] = {};
std::atomic<bool> g_jobQueued{false};
DWORD g_lastTick = 0, g_lastScreenCheck = 0;

bool Pressed(int vk) {
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool edge = down && !g_down[vk];
    g_down[vk] = down;
    return edge;
}

uint32_t U32(uintptr_t a) {
    uint32_t v = 0;
    SafeReadU32(a, v);
    return v;
}
bool Ptr(uint32_t v) { return v >= 0x10000 && v < 0x7FFF0000; }

bool GuardedDeposit(uint32_t money, uint32_t amount) {
    __try {
        reinterpret_cast<Deposit>(kMoneyDeposit)(reinterpret_cast<void*>((uintptr_t)money), amount, 0, 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The total the game checks is min([+4] + [+0xC] + bonuses, [+0x10]) (0x6A7B9F: cmovg with
// [esi+0x10]); used points are [+8], available = total - used (0x6A7F6A; the debug line at
// 0x44806C "CmdPoints Total:%d, Used:%d ... Available:%d"). So the cap +0x10 rises too, or the
// added points stay above the cap and nothing changes (player 2026-11-15: 20 presses, base 480,
// added 21100, still no room to train).
bool GuardedPoints(uint32_t points, int amount) {
    __try {
        reinterpret_cast<AddPoints>(kCommandPointsAdd)(reinterpret_cast<void*>((uintptr_t)points), amount);
        int* cap = reinterpret_cast<int*>((uintptr_t)points + 0x10);
        if (*cap < 1000000) *cap += amount;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool GuardedBody(uint32_t body, uint32_t fn, bool on) {
    __try {
        reinterpret_cast<SetIndestructible>((uintptr_t)fn)(reinterpret_cast<void*>((uintptr_t)body), on ? 1 : 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool GuardedKill(uint32_t obj) {
    __try {
        reinterpret_cast<KillObject>(kObjectKill)(reinterpret_cast<void*>((uintptr_t)obj), 8, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// On the game thread. Horde soldiers are objects of the list too, so each gets its own flag.
void ApplyImmortal(bool on) {
    int done = 0, failed = 0;
    ScannerForEachOwnObject([&](uint32_t obj) {
        uint32_t body = U32(obj + kObjBody), vt = Ptr(body) ? U32(body) : 0;
        uint32_t fn = Ptr(vt) ? U32(vt + kBodySetIndestructible) : 0;
        if (!Ptr(fn)) return;
        if (GuardedBody(body, fn, on)) ++done;
        else ++failed;
    });
    static int logged = -1;
    if (done != logged || failed) {
        logged = done;
        wchar_t b[100];
        swprintf_s(b, L"training: immortal %d on %d objects, %d failed", on ? 1 : 0, done, failed);
        LogLine(b);
    }
}

// For a later real check of human players: Worldbuilder's Player::setPlayerType writes +0x300.
void LogPlayers() {
    static bool logged = false;
    if (logged) return;
    logged = true;
    uint32_t list = U32(ScannerPlayerList());
    for (int i = 0; Ptr(list) && i < 20; ++i) {
        uint32_t p = U32(list + 0x18 + i * 4);
        if (!Ptr(p)) continue;
        wchar_t b[100];
        swprintf_s(b, L"training: player slot %d at %08X %S, +0x300 = %u", i, p, ScannerPlayerId(p).c_str(), U32(p + 0x300));
        LogLine(b);
    }
}

std::wstring ItemWords() {
    std::wstring w;
    switch (g_index) {
        case 0: w = std::wstring(L"Осада, волны по таймеру: ") + (WavesOn() ? L"включена" : L"выключена"); break;
        case 1: w = L"Сложность: " + WavesDifficultyName(); break;
        case 2: w = L"Вызвать волну сейчас"; break;
        case 3: w = L"Сводка осады"; break;
        case 4: w = L"Добавить " + std::to_wstring(kMoney) + L" денег"; break;
        case 5: w = L"Добавить " + std::to_wstring(kPoints) + L" командных очков"; break;
        case 7: w = L"Убить выделенных: нажмите End дважды"; break;
        case 8: w = std::wstring(L"Вечная скрытность: ") + (NoCatchOn() ? L"включена" : L"выключена"); break;
        case 9: w = L"Убить видимых врагов: нажмите End дважды"; break;
        default: w = std::wstring(L"Бессмертие моих войск: ") + (g_immortal ? L"включено" : L"выключено");
    }
    return w + L", " + std::to_wstring(g_index + 1) + L" из " + std::to_wstring(kItems);
}

void GiveMoney() {
    uint32_t local = ScannerLocalPlayer();
    if (!local) { Speak(L"Игрок не найден", true); return; }
    auto ok = std::make_shared<bool>(false);
    RunOnGameThreadAndWait([ok, local] { *ok = GuardedDeposit(local + kPlayerMoneyObject, kMoney); });
    uint32_t now = U32(local + kPlayerMoneyAmount);
    LogLine(L"training: money +" + std::to_wstring(kMoney) + (*ok ? L", now " : L" FAILED, now ") + std::to_wstring(now));
    Speak(*ok ? L"Деньги: " + std::to_wstring(now) : L"Не получилось добавить деньги", true);
}

void GivePoints() {
    uint32_t local = ScannerLocalPlayer();
    if (!local) { Speak(L"Игрок не найден", true); return; }
    uint32_t cp = local + kPlayerCommandPoints;
    uint32_t before = U32(cp + 0xC), capBefore = U32(cp + 0x10);
    auto ok = std::make_shared<bool>(false);
    RunOnGameThreadAndWait([ok, cp] { *ok = GuardedPoints(cp, kPoints); });
    int base = (int)U32(cp + 4), added = (int)U32(cp + 0xC), cap = (int)U32(cp + 0x10), used = (int)U32(cp + 8);
    int total = (std::min)(base + added, cap);  // without the bonuses of 0x6A7B9F
    wchar_t b[200];
    swprintf_s(b, L"training: command points base %d, added %u -> %d, cap %u -> %d, used %d, total about %d, %s", base, before, added, capBefore,
               cap, used, total, *ok ? L"ok" : L"FAILED");
    LogLine(b);
    Speak(*ok ? L"Добавлено " + std::to_wstring(kPoints) + L" командных очков. Свободно примерно " + std::to_wstring(total - used)
              : L"Не получилось добавить очки",
          true);
}

// Two presses of End within 5 s. A story's retry starts with its hero's death (Mirkwood's rescue:
// "Fail - Start - Rescue" on NAMED_DESTROYED Bilbo), so this is the way back when the hero is stuck.
void KillSelected() {
    static DWORD armed = 0;
    std::vector<uint32_t> sel = ScannerSelection();
    if (sel.empty()) { Speak(L"Ничего не выделено", true); return; }
    if (g_immortal) { Speak(L"Сначала выключите бессмертие", true); return; }
    DWORD now = GetTickCount();
    if (!armed || now - armed > 5000) {
        armed = now;
        Speak(L"Убить: " + ScannerNameOf(sel[0]) + (sel.size() > 1 ? L" и других, всего " + std::to_wstring(sel.size()) : L"") +
                  L". Нажмите End ещё раз.", true);
        return;
    }
    armed = 0;
    auto done = std::make_shared<int>(0);
    RunOnGameThreadAndWait([sel, done] {
        for (uint32_t o : sel) *done += GuardedKill(o);
    });
    wchar_t b[80];
    swprintf_s(b, L"training: killed %d of %zu selected", *done, sel.size());
    LogLine(b);
    Speak(*done ? L"Убито: " + std::to_wstring(*done) : L"Не получилось", true);
}

// Everything of "чужие" but the buildings. Story enemies are often Indestructible (Amon Hen's
// patrols: TEAM_AFFECT_OBJECT_PANEL_FLAGS), and such a body takes no damage, so the flag goes first.
void KillVisibleEnemies() {
    static DWORD armed = 0;
    std::vector<uint32_t> foes = ScannerVisibleEnemies();
    if (foes.empty()) { armed = 0; Speak(L"Врагов не видно", true); return; }
    DWORD now = GetTickCount();
    if (!armed || now - armed > 5000) {
        armed = now;
        Speak(L"Убить врагов, видно: " + std::to_wstring(foes.size()) + L". Нажмите End ещё раз.", true);
        return;
    }
    armed = 0;
    auto done = std::make_shared<int>(0);
    RunOnGameThreadAndWait([foes, done] {
        for (uint32_t o : foes) {
            uint32_t body = U32(o + kObjBody), vt = Ptr(body) ? U32(body) : 0;
            uint32_t fn = Ptr(vt) ? U32(vt + kBodySetIndestructible) : 0;
            if (Ptr(fn)) GuardedBody(body, fn, false);
            *done += GuardedKill(o);
        }
    });
    wchar_t b[80];
    swprintf_s(b, L"training: killed %d of %zu visible enemies", *done, foes.size());
    LogLine(b);
    Speak(*done ? L"Убито: " + std::to_wstring(*done) : L"Не получилось", true);
}

void ToggleImmortal() {
    g_immortal = !g_immortal;
    bool on = g_immortal;
    RunOnGameThreadAndWait([on] { ApplyImmortal(on); });
    Speak(on ? L"Бессмертие включено" : L"Бессмертие выключено", true);
}

}  // namespace

bool TrainingActive() { return g_open; }

void TrainingKeys(bool gameFocused) {
    WavesTick();
    DWORD now = GetTickCount();
    // Screens of LAN and online play: from there on this launch stays closed to training.
    if (!g_network && now - g_lastScreenCheck > 500) {
        g_lastScreenCheck = now;
        if (ShellHasScreen("LanLobby.apt") || ShellHasScreen("OnlineShell.apt")) {
            g_network = true;
            LogLine(L"training: network screen seen, training is off until the game restarts");
        }
    }
    if (!MenuInBattle()) {
        g_open = g_immortal = false;
        return;
    }
    // New units are born without the flag.
    if (g_immortal && now - g_lastTick > 1000 && !g_jobQueued.exchange(true)) {
        g_lastTick = now;
        RunOnGameThread([] {
            ApplyImmortal(true);
            g_jobQueued = false;
        });
    }
    if (!gameFocused) return;
    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    bool f9 = Pressed(VK_F9);
    if (Pressed(VK_F8) && ctrl && shift && !g_network) WavesSpeakStatus();
    if (!g_open) {
        if (!f9 || !ctrl || !shift) return;
        if (g_network) { Speak(L"Тренировка работает только в одиночной игре", true); return; }
        if (CommandsActive()) { Speak(L"Сначала выйдите из режима команд", true); return; }
        g_open = true;
        g_index = 0;
        for (int vk : {VK_NEXT, VK_PRIOR, VK_HOME, VK_END}) g_down[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
        LogLine(L"training: on");
        LogPlayers();
        Speak(L"Тренировка. " + ItemWords(), true);
        return;
    }
    if (f9 && ctrl && shift) {
        g_open = false;
        LogLine(L"training: off");
        Speak(L"Тренировка закрыта", true);
        return;
    }
    if (Pressed(VK_NEXT)) { g_index = (g_index + 1) % kItems; Speak(ItemWords(), true); }
    if (Pressed(VK_PRIOR)) { g_index = (g_index + kItems - 1) % kItems; Speak(ItemWords(), true); }
    if (Pressed(VK_HOME)) Speak(ItemWords(), true);
    if (Pressed(VK_END)) {
        switch (g_index) {
            case 0: WavesToggle(); break;
            case 1: WavesNextDifficulty(); break;
            case 2: WavesCallNow(); break;
            case 3: WavesSpeakStatus(); break;
            case 4: GiveMoney(); break;
            case 5: GivePoints(); break;
            case 7: KillSelected(); break;
            case 8: NoCatchToggle(); break;
            case 9: KillVisibleEnemies(); break;
            default: ToggleImmortal();
        }
    }
}
