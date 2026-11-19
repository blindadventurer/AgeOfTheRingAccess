#include <memory>
#include <cstdio>
#include <mutex>
#include <vector>
#include "gamethread.h"
#include "log.h"
#include "memory.h"

namespace {

constexpr UINT kRunJobs = WM_APP + 0x4A41;  // "JA"
constexpr uintptr_t kTheTacticalView = 0xDE447C;
constexpr uint32_t kLookAtSlot = 0x54;
constexpr uint32_t kSetHeightSlot = 0x130, kViewHeight = 0x40, kViewHeightLimits = 0x24C8;
// Move orders the way the game's own command translator gives them (0x803EB1, disassembly
// 2026-11-01): msg = TheMessageStream->vtable[0x48](MSG_DO_MOVETO), then
// msg->appendLocationArgument(&pos) (0x71115C: copies the Coord3D to the argument, type 6).
// MSG_DO_MOVETO = 0x42F (the message-name switch at 0x7117C4, table 0x71260B, entry 0x45).
// The order goes to the local player's selected units.
constexpr uintptr_t kTheMessageStream = 0xDE6398;
constexpr uint32_t kAppendMessageSlot = 0x48;
constexpr uintptr_t kAppendLocationArgument = 0x71115C;
constexpr uintptr_t kAppendIntegerArgument = 0x7111E5;
constexpr int kMsgDoMoveTo = 0x42F, kMsgLwArmyMove = 0x6A8, kMsgLwBuild = 0x6AA, kMsgLwQueueUnit = 0x6AC, kMsgSelectGroup = 0x3E9, kMsgLwBattleMethod = 0x6A7;
constexpr uintptr_t kAppendBoolArgument = 0x711104, kAppendObjectIdArgument = 0x71111A, kObjectDrawable = 0x70E013;
constexpr uintptr_t kTheInGameUI = 0xDE4830;
constexpr uint32_t kViewLimited = 0x44, kUiDeselectAll = 0x110, kUiSelectDrawable = 0x108;
// NAMED_FOLLOW_WAYPOINTS_EXACT (script action 286, handler 0x7C928D, disassembly 2026-11-01):
// way = TheTerrainLogic [0xDE4690] ->vtable[+0x90](&pos, label) (closest waypoint on the path
// with that label), obj->leaveGroup() (0x68C6EE: clears obj+0x1A8), then
// (AIUpdate [obj+0x260] + 0x20)->0x77100D(way, 1) (follow the path exactly, from script).
// The label is a game AsciiString built with AsciiString(const char*) 0x40611E and freed
// with ~AsciiString 0x4367B0 (both used the same way all over the script code).
constexpr uintptr_t kTheTerrainLogic = 0xDE4690;
constexpr uint32_t kClosestWaypointOnPathSlot = 0x90;
constexpr uintptr_t kLeaveGroup = 0x68C6EE;
constexpr uintptr_t kFollowPathExact = 0x77100D;
// MSG_DO_FORCE_ATTACK_OBJECT = 0x426 (entry 0x3C of the switch at 0x7117C4). The command translator
// at 0x81E519 sends it with the target's id (object +0x74) and the mouse's world position.
constexpr int kMsgForceAttackObject = 0x426;
constexpr uint32_t kObjectId = 0x74;
// MSG_DO_ATTACK_OBJECT = 0x425: the translator at 0x81DC36 sends it with the target's id alone.
// The check before it (0x81D9DE): Object 0x691269 can attack at all, then 0x68D6A6 (thiscall: type,
// target, 0), type 0 = a target of its own choice, 1 = forced; 2 and 3 = can, at once or after moving.
// A horde answers through its first soldier: kind bit at template +0x112 & 0x10, contain 0x68C3A3,
// contain vt+8 (&target position, object +0x38).
constexpr int kMsgAttackObject = 0x425;
constexpr uintptr_t kObjectCanAttack = 0x691269, kObjectAbleToAttack = 0x68D6A6, kObjectContain = 0x68C3A3;
// Script action 502 UNIT_TELEPORT_TO_WAYPOINT, case 0x7CF27B -> 0x7BDBF9: finds the unit and the
// waypoint, then object->this(&waypoint.location, 0): sets the position, updates, stops its AI.
constexpr uintptr_t kObjectTeleport = 0x696E63;
constexpr uintptr_t kAsciiStringCtor = 0x40611E;
constexpr uintptr_t kAsciiStringDtor = 0x4367B0;
constexpr uint32_t kObjAI = 0x260;

HWND g_wnd = nullptr;
WNDPROC g_original = nullptr;
bool g_unicode = true;
std::mutex g_lock;
std::vector<std::function<void()>> g_jobs;
uintptr_t g_codeStart = 0, g_codeEnd = 0;  // game.dat code section, for checking call targets

bool CallGuarded(std::function<void()>* job) {
    __try {
        (*job)();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

LRESULT CALLBACK Hook(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == kRunJobs) {
        std::vector<std::function<void()>> jobs;
        {
            std::lock_guard<std::mutex> l(g_lock);
            jobs.swap(g_jobs);
        }
        for (auto& j : jobs)
            if (!CallGuarded(&j)) LogLine(L"gamethread: a job faulted and was skipped");
        return 0;
    }
    return g_unicode ? CallWindowProcW(g_original, h, msg, wp, lp) : CallWindowProcA(g_original, h, msg, wp, lp);
}

void FindCodeSection() {
    auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
        if ((sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) && strncmp((const char*)sec->Name, ".text", 5) == 0) {
            g_codeStart = (uintptr_t)base + sec->VirtualAddress;
            g_codeEnd = g_codeStart + sec->Misc.VirtualSize;
        }
}

}  // namespace

void GameThreadInit(HWND gameWindow) {
    if (!gameWindow || g_original) return;
    FindCodeSection();
    g_wnd = gameWindow;
    g_unicode = IsWindowUnicode(gameWindow) != FALSE;
    g_original = reinterpret_cast<WNDPROC>(g_unicode ? SetWindowLongPtrW(gameWindow, GWLP_WNDPROC, (LONG_PTR)Hook)
                                                     : SetWindowLongPtrA(gameWindow, GWLP_WNDPROC, (LONG_PTR)Hook));
    wchar_t b[160];
    swprintf_s(b, L"gamethread: window procedure subclassed (%s), original %p, code %08IX-%08IX",
               g_unicode ? L"unicode" : L"ansi", g_original, g_codeStart, g_codeEnd);
    LogLine(g_original ? b : L"gamethread: subclassing the window failed");
}

void RunOnGameThread(std::function<void()> job) {
    if (!g_original) return;
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_jobs.push_back(std::move(job));
    }
    PostMessageW(g_wnd, kRunJobs, 0, 0);
}

void RunOnGameThreadAndWait(std::function<void()> job) {
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    RunOnGameThread([job, done] {
        job();
        SetEvent(done);
    });
    // After a timeout the job may still run and set the event, so the handle stays open then.
    if (done && WaitForSingleObject(done, 2000) == WAIT_OBJECT_0) CloseHandle(done);
}

void CameraLookAt(float x, float y, float z, HANDLE done, bool unbound) {
    RunOnGameThread([x, y, z, done, unbound] {
        uint32_t view = 0, vt = 0, fn = 0;
        if (!SafeReadU32(kTheTacticalView, view) || !view || !SafeReadU32(view, vt) ||
            !SafeReadU32(vt + kLookAtSlot, fn) || fn < g_codeStart || fn >= g_codeEnd) {
            wchar_t b[120];
            swprintf_s(b, L"camera: lookAt refused (view %08X vtable %08X slot %08X)", view, vt, fn);
            LogLine(b);
            return;
        }
        float pos[3] = {x, y, z};
        uint32_t limit = 0;
        bool lifted = unbound && SafeReadU32(view + kViewLimited, limit);
        if (lifted) SafeWriteI32(view + kViewLimited, (int32_t)(limit & ~0xFFu));
        reinterpret_cast<void(__thiscall*)(void*, const float*)>(fn)(reinterpret_cast<void*>(view), pos);
        if (lifted) SafeWriteI32(view + kViewLimited, (int32_t)limit);
        wchar_t b[120];
        swprintf_s(b, L"camera: lookAt (%.0f, %.0f, %.0f) via %08X", x, y, z, fn);
        LogLine(b);
        if (done) SetEvent(done);
    });
}

void CameraPosition(float& x, float& y) {
    x = y = 0;
    uint32_t view = 0;
    if (!SafeReadU32(kTheTacticalView, view) || !view) return;
    SafeRead(view + 0x0C, &x, 4);
    SafeRead(view + 0x10, &y, 4);
}

void CameraToHeightLimit(bool closest, const std::shared_ptr<CameraHeight>& out) {
    RunOnGameThreadAndWait([closest, out] {
        uint32_t view = 0, vt = 0, setter = 0, limitVt = 0, lowFn = 0, highFn = 0;
        bool good = SafeReadU32(kTheTacticalView, view) && view && SafeReadU32(view, vt) &&
                    SafeReadU32(vt + kSetHeightSlot, setter) && setter >= g_codeStart && setter < g_codeEnd &&
                    SafeReadU32(view + kViewHeightLimits, limitVt) && SafeReadU32(limitVt, lowFn) &&
                    SafeReadU32(limitVt + 4, highFn) && lowFn >= g_codeStart && lowFn < g_codeEnd &&
                    highFn >= g_codeStart && highFn < g_codeEnd;
        if (!good) {
            wchar_t b[120];
            swprintf_s(b, L"camera: height refused (view %08X setter %08X limits %08X)", view, setter, limitVt);
            LogLine(b);
            return;
        }
        using Limit = float(__thiscall*)(void*);
        void* limits = reinterpret_cast<void*>(view + kViewHeightLimits);
        float low = reinterpret_cast<Limit>(lowFn)(limits), high = reinterpret_cast<Limit>(highFn)(limits);
        SafeRead(view + kViewHeight, &out->before, 4);
        // The lowest limit sometimes reads 0 while the highest is right ("(0.0, 450.0)": 7 times in the
        // log, 2026-11-05 to 2026-11-14, each a moment after a camera jump or an order, the next presses
        // fine). The last good lowest of this session stands in, else 5, the smallest one seen (Fangorn,
        // Westfold). Logged with the limit object's state to find what the 0 means.
        static float lastLow = 0;
        if (!(low > 0) && high > 0 && high < 100000) {
            uint8_t limited = 0;
            SafeRead(view + 0x44, &limited, 1);
            uint32_t raw[4] = {};
            SafeRead(view + kViewHeightLimits + 4, raw, sizeof(raw));
            float stand = lastLow > 0 && lastLow < high ? lastLow : (high > 5.f ? 5.f : high);
            wchar_t b[200];
            swprintf_s(b, L"camera: lowest read %.1f (limit object %08X, lowest fn %08X, view+0x44 %u, next %08X %08X %08X %08X), using %.1f",
                       low, limitVt, lowFn, limited, raw[0], raw[1], raw[2], raw[3], stand);
            LogLine(b);
            low = stand;
            out->fallback = true;
        }
        if (!(low > 0) || !(high >= low) || high > 100000) {
            wchar_t b[120];
            swprintf_s(b, L"camera: height limits make no sense (%.1f, %.1f)", low, high);
            LogLine(b);
            return;
        }
        if (!out->fallback) lastLow = low;
        reinterpret_cast<void(__thiscall*)(void*, float)>(setter)(reinterpret_cast<void*>(view), closest ? low : high);
        SafeRead(view + kViewHeight, &out->after, 4);
        out->lowest = low;
        out->highest = high;
        out->ran = true;
        wchar_t b[120];
        swprintf_s(b, L"camera: height %.1f -> %.1f (limits %.1f..%.1f, %s)", out->before, out->after, low, high,
                   closest ? L"closest" : L"farthest");
        LogLine(b);
    });
}

namespace {
void SendLocationOrder(int type, const wchar_t* what, float x, float y, float z, HANDLE done, bool* ok);
}

void MoveSelectedTo(float x, float y, float z, HANDLE done, bool* ok) {
    SendLocationOrder(kMsgDoMoveTo, L"move to", x, y, z, done, ok);
}

constexpr int kMsgDoAttackMoveTo = 0x430;
void AttackMoveSelectedTo(float x, float y, float z, HANDLE done, bool* ok) {
    SendLocationOrder(kMsgDoAttackMoveTo, L"attack-move to", x, y, z, done, ok);
}

// issueSpecialPowerCommand (0x81E147, thiscall, ret 0x14; `this` is never read: ecx is overwritten
// on both paths before any use): (CommandButton*, evaluate type 0 = do it, Drawable* target or 0,
// const Coord3D* pos, Object* source or 0 = the selection). The pending command comes from
// TheInGameUI vt+0xC0 (0x81E7DD, before the switch on the button kind at +0x14); kind 0x18 is
// the one the switch sends there with source 0 (0x81EB28). vt+0xBC is setGUICommand: the control
// bar calls it with the button (0x94116C), nine other sites with 0.
constexpr uintptr_t kIssueSpecialPower = 0x81E147;
constexpr uint32_t kUiGetGuiCommand = 0xC0, kUiSetGuiCommand = 0xBC, kCommandKind = 0x14;
constexpr int kKindSpecialPower = 0x18;

namespace {
int AimPendingOnGameThread(uint32_t target, float x, float y, float z, int* kind) {
    __try {
        uint32_t ui = 0, vt = 0, get = 0, set = 0;
        if (!SafeReadU32(kTheInGameUI, ui) || !ui || !SafeReadU32(ui, vt) || !SafeReadU32(vt + kUiGetGuiCommand, get) ||
            !SafeReadU32(vt + kUiSetGuiCommand, set) || get < g_codeStart || get >= g_codeEnd || set < g_codeStart || set >= g_codeEnd)
            return -1;
        void* uiP = reinterpret_cast<void*>((uintptr_t)ui);
        void* cmd = reinterpret_cast<void*(__thiscall*)(void*)>(get)(uiP);
        if (!cmd) return -1;
        uint32_t k = 0, power = 0;
        SafeReadU32((uintptr_t)cmd + kCommandKind, k);
        SafeReadU32((uintptr_t)cmd + 0x44, power);
        if (kind) *kind = (int)k;
        if ((int)k != kKindSpecialPower || !power) return -2;
        void* drawable = target ? reinterpret_cast<void*(__thiscall*)(void*)>(kObjectDrawable)(reinterpret_cast<void*>((uintptr_t)target)) : nullptr;
        float pos[3] = {x, y, z};
        using Issue = int(__thiscall*)(void*, void*, int, void*, const float*, void*);
        int r = reinterpret_cast<Issue>(kIssueSpecialPower)(nullptr, cmd, 0, drawable, pos, nullptr);
        // An ability for places and objects refuses an object it cannot take: the place then.
        if (!r && drawable) r = reinterpret_cast<Issue>(kIssueSpecialPower)(nullptr, cmd, 0, nullptr, pos, nullptr);
        if (r) reinterpret_cast<void(__thiscall*)(void*, void*)>(set)(uiP, nullptr);
        return r;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -3;
    }
}
}  // namespace

int AimPendingAbility(uint32_t target, float x, float y, float z, int* kind) {
    auto r = std::make_shared<int>(-1);
    auto k = std::make_shared<int>(-1);
    RunOnGameThreadAndWait([=] { *r = AimPendingOnGameThread(target, x, y, z, k.get()); });
    if (kind) *kind = *k;
    wchar_t b[140];
    swprintf_s(b, L"order: aim pending ability (kind 0x%X) at %08X (%.0f, %.0f, %.0f) -> 0x%X", *k, target, x, y, z, *r);
    LogLine(b);
    return *r;
}

// AICommandInterface::aiMoveToPosition (0x66C4CA, thiscall on [obj+0x260]+0x20, ret 8: const Coord3D*,
// command source): AICommandParms(cmd 0, source) by 0x7536DD, the point copied to parms+8, then the
// interface's vtable slot 0 (aiDoCommand). The game calls it this way at 0x66D41D (source 2) and
// 0x774DF6 (disassembly 2026-11-16). Its sibling 0x77100D is the scripts' follow-path-exact (cmd 0x32).
constexpr uintptr_t kAiMoveToPosition = 0x66C4CA;

namespace {
bool MoveObjectOnGameThread(uint32_t ai, float x, float y, float z) {
    __try {
        float pos[3] = {x, y, z};
        reinterpret_cast<void(__thiscall*)(void*, const float*, int)>(kAiMoveToPosition)(
            reinterpret_cast<void*>((uintptr_t)ai + 0x20), pos, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}  // namespace

void MoveObjectTo(uint32_t obj, float x, float y, float z) {
    RunOnGameThread([obj, x, y, z] {
        uint32_t ai = 0;
        bool good = SafeReadU32(obj + kObjAI, ai) && ai && MoveObjectOnGameThread(ai, x, y, z);
        wchar_t b[140];
        swprintf_s(b, L"order: object %08X move to (%.0f, %.0f, %.0f) %s (ai %08X)", obj, x, y, z,
                   good ? L"sent" : L"REFUSED", ai);
        LogLine(b);
    });
}

namespace {
int PowerFromOnGameThread(uint32_t button, uint32_t source, float x, float y, float z) {
    __try {
        uint32_t power = 0;
        if (!button || !SafeReadU32(button + 0x44, power) || !power) return -1;
        float pos[3] = {x, y, z};
        using Issue = int(__thiscall*)(void*, void*, int, void*, const float*, void*);
        return reinterpret_cast<Issue>(kIssueSpecialPower)(nullptr, reinterpret_cast<void*>((uintptr_t)button), 0,
                                                           nullptr, pos, reinterpret_cast<void*>((uintptr_t)source));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -3;
    }
}
}  // namespace

int IssuePowerFrom(uint32_t button, uint32_t source, float x, float y, float z) {
    auto r = std::make_shared<int>(-5);
    RunOnGameThreadAndWait([=] { *r = PowerFromOnGameThread(button, source, x, y, z); });
    wchar_t b[160];
    swprintf_s(b, L"order: power of button %08X from %08X at (%.0f, %.0f, %.0f) -> 0x%X", button, source, x, y, z, *r);
    LogLine(b);
    return *r;
}

namespace {
void SendLocationOrder(int type, const wchar_t* what, float x, float y, float z, HANDLE done, bool* ok) {
    RunOnGameThread([type, what, x, y, z, done, ok] {
        uint32_t stream = 0, vt = 0, fn = 0;
        bool good = SafeReadU32(kTheMessageStream, stream) && stream && SafeReadU32(stream, vt) &&
                    SafeReadU32(vt + kAppendMessageSlot, fn) && fn >= g_codeStart && fn < g_codeEnd;
        void* msg = nullptr;
        if (good) {
            msg = reinterpret_cast<void*(__thiscall*)(void*, int)>(fn)(reinterpret_cast<void*>(stream), type);
            good = msg != nullptr;
        }
        if (good) {
            float pos[3] = {x, y, z};
            reinterpret_cast<void(__thiscall*)(void*, const float*)>(kAppendLocationArgument)(msg, pos);
        }
        wchar_t b[160];
        swprintf_s(b, L"order: %s (%.0f, %.0f, %.0f) %s (stream %08X, append %08X, message %p)", what, x, y, z,
                   good ? L"sent" : L"REFUSED", stream, fn, msg);
        LogLine(b);
        if (ok) *ok = good;
        if (done) SetEvent(done);
    });
}
}  // namespace

void SelectObjectDirect(uint32_t obj, HANDLE done, bool* ok) {
    RunOnGameThread([obj, done, ok] {
        uint32_t stream = 0, vt = 0, fn = 0, ui = 0, uiVt = 0, clear = 0, pick = 0, id = 0;
        bool good = SafeReadU32(kTheMessageStream, stream) && stream && SafeReadU32(stream, vt) &&
                    SafeReadU32(vt + kAppendMessageSlot, fn) && SafeReadU32(kTheInGameUI, ui) && ui && SafeReadU32(ui, uiVt) &&
                    SafeReadU32(uiVt + kUiDeselectAll, clear) && SafeReadU32(uiVt + kUiSelectDrawable, pick) &&
                    SafeReadU32(obj + 0x74, id) && fn >= g_codeStart && fn < g_codeEnd && clear >= g_codeStart &&
                    clear < g_codeEnd && pick >= g_codeStart && pick < g_codeEnd;
        if (good) {
            reinterpret_cast<void(__thiscall*)(void*)>(clear)(reinterpret_cast<void*>(ui));
            void* msg = reinterpret_cast<void*(__thiscall*)(void*, int)>(fn)(reinterpret_cast<void*>(stream), kMsgSelectGroup);
            reinterpret_cast<void(__thiscall*)(void*, int)>(kAppendBoolArgument)(msg, 1);
            reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(kAppendObjectIdArgument)(msg, id);
            void* drawable = reinterpret_cast<void*(__thiscall*)(void*)>(kObjectDrawable)(reinterpret_cast<void*>(obj));
            if (drawable) reinterpret_cast<void(__thiscall*)(void*, void*)>(pick)(reinterpret_cast<void*>(ui), drawable);
            good = drawable != nullptr;
        }
        wchar_t b[100];
        swprintf_s(b, L"order: select object %08X id %u %s", obj, id, good ? L"done" : L"REFUSED");
        LogLine(b);
        if (ok) *ok = good;
        if (done) SetEvent(done);
    });
}

void SelectObjectsDirect(const std::vector<uint32_t>& objs) {
    if (objs.empty()) return;
    RunOnGameThread([objs] {
        uint32_t stream = 0, vt = 0, fn = 0, ui = 0, uiVt = 0, clear = 0, pick = 0;
        bool good = SafeReadU32(kTheMessageStream, stream) && stream && SafeReadU32(stream, vt) &&
                    SafeReadU32(vt + kAppendMessageSlot, fn) && SafeReadU32(kTheInGameUI, ui) && ui && SafeReadU32(ui, uiVt) &&
                    SafeReadU32(uiVt + kUiDeselectAll, clear) && SafeReadU32(uiVt + kUiSelectDrawable, pick) &&
                    fn >= g_codeStart && fn < g_codeEnd && clear >= g_codeStart && clear < g_codeEnd &&
                    pick >= g_codeStart && pick < g_codeEnd;
        int n = 0;
        if (good) {
            std::vector<uint32_t> ids;
            for (uint32_t obj : objs) {
                uint32_t id = 0;
                if (SafeReadU32(obj + 0x74, id) && id) ids.push_back(id);
            }
            good = !ids.empty();
            if (good) {
                reinterpret_cast<void(__thiscall*)(void*)>(clear)(reinterpret_cast<void*>(ui));
                void* msg = reinterpret_cast<void*(__thiscall*)(void*, int)>(fn)(reinterpret_cast<void*>(stream), kMsgSelectGroup);
                reinterpret_cast<void(__thiscall*)(void*, int)>(kAppendBoolArgument)(msg, 1);
                for (uint32_t id : ids) reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(kAppendObjectIdArgument)(msg, id);
                for (uint32_t obj : objs) {
                    void* drawable = reinterpret_cast<void*(__thiscall*)(void*)>(kObjectDrawable)(reinterpret_cast<void*>(obj));
                    if (drawable) reinterpret_cast<void(__thiscall*)(void*, void*)>(pick)(reinterpret_cast<void*>(ui), drawable), ++n;
                }
            }
        }
        wchar_t b[100];
        swprintf_s(b, L"order: select %zu objects, %d drawables %s", objs.size(), n, good ? L"done" : L"REFUSED");
        LogLine(b);
    });
}

void SendBattleMethod(int method, int battleId, HANDLE done, bool* ok) {
    RunOnGameThread([method, battleId, done, ok] {
        uint32_t stream = 0, vt = 0, fn = 0;
        bool good = SafeReadU32(kTheMessageStream, stream) && stream && SafeReadU32(stream, vt) &&
                    SafeReadU32(vt + kAppendMessageSlot, fn) && fn >= g_codeStart && fn < g_codeEnd;
        void* msg = good ? reinterpret_cast<void*(__thiscall*)(void*, int)>(fn)(reinterpret_cast<void*>(stream), kMsgLwBattleMethod) : nullptr;
        good = msg != nullptr;
        if (good) {
            using AppendInt = void(__thiscall*)(void*, int);
            reinterpret_cast<AppendInt>(kAppendIntegerArgument)(msg, method);
            reinterpret_cast<AppendInt>(kAppendIntegerArgument)(msg, battleId);
        }
        wchar_t b[100];
        swprintf_s(b, L"order: battle %d method %d %s", battleId, method, good ? L"sent" : L"REFUSED");
        LogLine(b);
        if (ok) *ok = good;
        if (done) SetEvent(done);
    });
}

void SendBuild(int regionIndex, int plotId, int templateId, HANDLE done, bool* ok) {
    RunOnGameThread([regionIndex, plotId, templateId, done, ok] {
        uint32_t stream = 0, vt = 0, fn = 0;
        bool good = SafeReadU32(kTheMessageStream, stream) && stream && SafeReadU32(stream, vt) &&
                    SafeReadU32(vt + kAppendMessageSlot, fn) && fn >= g_codeStart && fn < g_codeEnd;
        void* msg = good ? reinterpret_cast<void*(__thiscall*)(void*, int)>(fn)(reinterpret_cast<void*>(stream), kMsgLwBuild) : nullptr;
        good = msg != nullptr;
        if (good) {
            using AppendInt = void(__thiscall*)(void*, int);
            reinterpret_cast<AppendInt>(kAppendIntegerArgument)(msg, regionIndex);
            reinterpret_cast<AppendInt>(kAppendIntegerArgument)(msg, plotId);
            reinterpret_cast<AppendInt>(kAppendIntegerArgument)(msg, templateId);
        }
        wchar_t b[120];
        swprintf_s(b, L"order: build %d in region %d plot %d %s", templateId, regionIndex, plotId, good ? L"sent" : L"REFUSED");
        LogLine(b);
        if (ok) *ok = good;
        if (done) SetEvent(done);
    });
}

void SendQueueUnit(int buildingId, int armyId, HANDLE done, bool* ok) {
    RunOnGameThread([buildingId, armyId, done, ok] {
        uint32_t stream = 0, vt = 0, fn = 0;
        bool good = SafeReadU32(kTheMessageStream, stream) && stream && SafeReadU32(stream, vt) &&
                    SafeReadU32(vt + kAppendMessageSlot, fn) && fn >= g_codeStart && fn < g_codeEnd;
        void* msg = good ? reinterpret_cast<void*(__thiscall*)(void*, int)>(fn)(reinterpret_cast<void*>(stream), kMsgLwQueueUnit) : nullptr;
        good = msg != nullptr;
        if (good) {
            using AppendInt = void(__thiscall*)(void*, int);
            reinterpret_cast<AppendInt>(kAppendIntegerArgument)(msg, buildingId);
            reinterpret_cast<AppendInt>(kAppendIntegerArgument)(msg, armyId);
        }
        wchar_t b[120];
        swprintf_s(b, L"order: queue army %d in building %d %s", armyId, buildingId, good ? L"sent" : L"REFUSED");
        LogLine(b);
        if (ok) *ok = good;
        if (done) SetEvent(done);
    });
}

void AskCanQueue(uint32_t nugget, const int* ids, int count, char* can, HANDLE done, bool* ok) {
    RunOnGameThread([nugget, ids, count, can, done, ok] {
        uint32_t vt = 0, fn = 0;
        bool good = SafeReadU32(nugget, vt) && SafeReadU32(vt + 0x1C, fn) && fn >= g_codeStart && fn < g_codeEnd;
        if (good)
            for (int i = 0; i < count; ++i)
                can[i] = reinterpret_cast<bool(__thiscall*)(void*, int)>(fn)(reinterpret_cast<void*>(nugget), ids[i]) ? 1 : 0;
        if (ok) *ok = good;
        if (done) SetEvent(done);
    });
}

namespace {
// Bytes at the start of LivingWorldLogic's end-phase function, checked before the call so a game
// update cannot send the mod into foreign code: push esi; mov esi, ecx; cmp dword [esi+0xF4], 6.
const unsigned char kEndPhaseHead[] = {0x56, 0x8B, 0xF1, 0x83, 0xBE, 0xF4, 0x00, 0x00, 0x00, 0x06};
constexpr uintptr_t kEndPhaseFn = 0x6B6BA1, kLwActive = 0x441E4A;

int EndPhaseOnGameThread() {
    __try {
        uint32_t logic = 0;
        if (!SafeReadU32(0xDE4950, logic) || !logic) return -1;
        unsigned char head[sizeof kEndPhaseHead];
        if (!SafeRead(kEndPhaseFn, head, sizeof head) || memcmp(head, kEndPhaseHead, sizeof head)) return -2;
        if (!reinterpret_cast<bool(__thiscall*)(void*)>(kLwActive)(reinterpret_cast<void*>(logic))) return -4;
        return reinterpret_cast<bool(__thiscall*)(void*)>(kEndPhaseFn)(reinterpret_cast<void*>(logic)) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -3;
    }
}
}  // namespace

int WotrEndPhase() {
    auto r = std::make_shared<int>(-5);
    RunOnGameThreadAndWait([r] { *r = EndPhaseOnGameThread(); });
    LogLine(L"order: end phase -> " + std::to_wstring(*r));
    return *r;
}

void SendArmyMove(int armyId, int regionIndex, float x, float y, HANDLE done, bool* ok) {
    RunOnGameThread([armyId, regionIndex, x, y, done, ok] {
        uint32_t stream = 0, vt = 0, fn = 0;
        bool good = SafeReadU32(kTheMessageStream, stream) && stream && SafeReadU32(stream, vt) &&
                    SafeReadU32(vt + kAppendMessageSlot, fn) && fn >= g_codeStart && fn < g_codeEnd;
        void* msg = nullptr;
        if (good) {
            msg = reinterpret_cast<void*(__thiscall*)(void*, int)>(fn)(reinterpret_cast<void*>(stream), kMsgLwArmyMove);
            good = msg != nullptr;
        }
        if (good) {
            using AppendInt = void(__thiscall*)(void*, int);
            reinterpret_cast<AppendInt>(kAppendIntegerArgument)(msg, armyId);
            reinterpret_cast<AppendInt>(kAppendIntegerArgument)(msg, regionIndex);
            float pos[3] = {x, y, 0.f};
            reinterpret_cast<void(__thiscall*)(void*, const float*)>(kAppendLocationArgument)(msg, pos);
        }
        wchar_t b[160];
        swprintf_s(b, L"order: army %d to region %d at (%.0f, %.0f) %s", armyId, regionIndex, x, y, good ? L"sent" : L"REFUSED");
        LogLine(b);
        if (ok) *ok = good;
        if (done) SetEvent(done);
    });
}

void ForceAttackObject(uint32_t obj, float x, float y, float z, HANDLE done, bool* ok) {
    RunOnGameThread([obj, x, y, z, done, ok] {
        uint32_t stream = 0, vt = 0, fn = 0, id = 0;
        bool good = SafeReadU32(kTheMessageStream, stream) && stream && SafeReadU32(stream, vt) &&
                    SafeReadU32(vt + kAppendMessageSlot, fn) && fn >= g_codeStart && fn < g_codeEnd &&
                    SafeReadU32(obj + kObjectId, id) && id;
        if (good) {
            void* msg = reinterpret_cast<void*(__thiscall*)(void*, int)>(fn)(reinterpret_cast<void*>(stream), kMsgForceAttackObject);
            good = msg != nullptr;
            if (good) {
                float pos[3] = {x, y, z};
                reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(kAppendObjectIdArgument)(msg, id);
                reinterpret_cast<void(__thiscall*)(void*, const float*)>(kAppendLocationArgument)(msg, pos);
            }
        }
        wchar_t b[120];
        swprintf_s(b, L"order: force attack id %u at (%.0f, %.0f, %.0f) %s", id, x, y, z, good ? L"sent" : L"REFUSED");
        LogLine(b);
        if (ok) *ok = good;
        if (done) SetEvent(done);
    });
}

namespace {
int AbleToAttack(uint32_t attacker, uint32_t target) {
    __try {
        void* a = reinterpret_cast<void*>((uintptr_t)attacker);
        if (!reinterpret_cast<bool(__thiscall*)(void*)>(kObjectCanAttack)(a)) return 0;
        using Able = int(__thiscall*)(void*, int, void*, int);
        void* t = reinterpret_cast<void*>((uintptr_t)target);
        int r = reinterpret_cast<Able>(kObjectAbleToAttack)(a, 0, t, 0);
        if (r == 2 || r == 3) return r;
        uint32_t tmpl = 0;
        uint8_t kinds = 0;
        if (!SafeReadU32(attacker + 4, tmpl) || !tmpl || !SafeRead(tmpl + 0x112, &kinds, 1) || !(kinds & 0x10)) return r;
        void* contain = reinterpret_cast<void*(__thiscall*)(void*)>(kObjectContain)(a);
        uint32_t vt = 0, first = 0;
        if (!contain || !SafeReadU32((uintptr_t)contain, vt) || !SafeReadU32(vt + 8, first)) return r;
        void* member = reinterpret_cast<void*(__thiscall*)(void*, const void*)>((uintptr_t)first)(contain, reinterpret_cast<const char*>(t) + 0x38);
        return member ? reinterpret_cast<Able>(kObjectAbleToAttack)(member, 0, t, 0) : r;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
}  // namespace

bool AttackObject(uint32_t target, const std::vector<uint32_t>& selection) {
    auto sent = std::make_shared<bool>(false);
    RunOnGameThreadAndWait([target, selection, sent] {
        int best = 0;
        for (uint32_t o : selection) {
            int r = AbleToAttack(o, target);
            if (r == 2 || r == 3) { best = r; break; }
            if (r != 0) best = r;
        }
        uint32_t stream = 0, vt = 0, fn = 0, id = 0;
        bool can = best == 2 || best == 3;
        bool good = can && SafeReadU32(kTheMessageStream, stream) && stream && SafeReadU32(stream, vt) &&
                    SafeReadU32(vt + kAppendMessageSlot, fn) && fn >= g_codeStart && fn < g_codeEnd &&
                    SafeReadU32(target + kObjectId, id) && id;
        if (good) {
            void* msg = reinterpret_cast<void*(__thiscall*)(void*, int)>(fn)(reinterpret_cast<void*>(stream), kMsgAttackObject);
            good = msg != nullptr;
            if (good) reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(kAppendObjectIdArgument)(msg, id);
        }
        wchar_t b[100];
        swprintf_s(b, L"order: attack id %u, check %d, %s", id, best, good ? L"sent" : L"not sent");
        LogLine(b);
        *sent = good;
    });
    return *sent;
}

// MSG_ENTER = 0x42C (entry 0x42 of the switch at 0x7117C4, name at 0xC20F0C). The translator at
// 0x81D846 appends object id 0, then the id of the drawable's object ([drawable + 0xFC] + 0x74).
constexpr int kMsgEnter = 0x42C;

bool EnterObject(uint32_t container) {
    auto sent = std::make_shared<bool>(false);
    RunOnGameThreadAndWait([container, sent] {
        uint32_t stream = 0, vt = 0, fn = 0, id = 0;
        bool good = SafeReadU32(kTheMessageStream, stream) && stream && SafeReadU32(stream, vt) &&
                    SafeReadU32(vt + kAppendMessageSlot, fn) && fn >= g_codeStart && fn < g_codeEnd &&
                    SafeReadU32(container + kObjectId, id) && id;
        if (good) {
            void* msg = reinterpret_cast<void*(__thiscall*)(void*, int)>(fn)(reinterpret_cast<void*>(stream), kMsgEnter);
            good = msg != nullptr;
            if (good) {
                reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(kAppendObjectIdArgument)(msg, 0);
                reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(kAppendObjectIdArgument)(msg, id);
            }
        }
        wchar_t b[80];
        swprintf_s(b, L"order: enter id %u %s", id, good ? L"sent" : L"not sent");
        LogLine(b);
        *sent = good;
    });
    return *sent;
}

namespace {
constexpr uintptr_t kTheSpellStore = 0xDE8AD4, kThePlayerList = 0xDE4928, kTheScienceStore = 0xDE3B20;
constexpr uint32_t kStoreButtons = 0x2A8, kButtonSciences = 0xA4, kPlayerSciencePoints = 0x24, kPlayerIndex = 0x54;
constexpr uintptr_t kPlayerHasScience = 0x6AC207, kSciencePrereqsMet = 0x5FED05, kScienceCost = 0x5FEC64;
constexpr int kMsgPurchaseScience = 0x414;

uint32_t LocalPlayer() {
    uint32_t list = 0, player = 0;
    return SafeReadU32(kThePlayerList, list) && list && SafeReadU32(list + 0x10, player) ? player : 0;
}
}  // namespace

std::vector<int> SpellStoreSciences(int slot) {
    std::vector<int> out;
    uint32_t store = 0, button = 0, begin = 0, end = 0;
    if (slot < 1 || slot > 20 || !SafeReadU32(kTheSpellStore, store) || !store ||
        !SafeReadU32(store + kStoreButtons + (slot - 1) * 8, button) || !button ||
        !SafeReadU32(button + kButtonSciences, begin) || !SafeReadU32(button + kButtonSciences + 4, end) ||
        end < begin || end - begin > 16 * 4)
        return out;
    for (uint32_t p = begin; p < end; p += 4) {
        uint32_t v = 0;
        if (SafeReadU32(p, v) && v < 100000) out.push_back((int)v);
    }
    return out;
}

void PurchaseScience(const std::vector<int>& sciences, const std::shared_ptr<SciencePurchase>& out) {
    RunOnGameThreadAndWait([sciences, out] {
        uint32_t player = LocalPlayer(), store = 0, points = 0, index = 0;
        if (!player || !SafeReadU32(kTheScienceStore, store) || !store || !SafeReadU32(player + kPlayerSciencePoints, points) ||
            !SafeReadU32(player + kPlayerIndex, index))
            return;
        out->ran = true;
        out->points = (int)points;
        void* p = reinterpret_cast<void*>((uintptr_t)player);
        void* s = reinterpret_cast<void*>((uintptr_t)store);
        int owned = 0;
        for (int sci : sciences) {
            if (reinterpret_cast<bool(__thiscall*)(void*, int)>(kPlayerHasScience)(p, sci)) { ++owned; continue; }
            if (!reinterpret_cast<bool(__thiscall*)(void*, void*, int)>(kSciencePrereqsMet)(
                    s, reinterpret_cast<char*>(p) + 4, sci)) { out->noPrereq = true; continue; }
            int cost = reinterpret_cast<int(__thiscall*)(void*, int)>(kScienceCost)(s, sci);
            out->cost = cost;
            if (cost > (int)points) { out->tooDear = true; continue; }
            out->science = sci;
            break;
        }
        out->owned = !sciences.empty() && owned == (int)sciences.size();
        uint32_t stream = 0, vt = 0, fn = 0;
        if (out->science >= 0 && SafeReadU32(kTheMessageStream, stream) && stream && SafeReadU32(stream, vt) &&
            SafeReadU32(vt + kAppendMessageSlot, fn) && fn >= g_codeStart && fn < g_codeEnd) {
            void* msg = reinterpret_cast<void*(__thiscall*)(void*, int)>(fn)(reinterpret_cast<void*>(stream), kMsgPurchaseScience);
            if (msg) {
                using AppendInt = void(__thiscall*)(void*, int);
                reinterpret_cast<AppendInt>(kAppendIntegerArgument)(msg, (int)index);
                reinterpret_cast<AppendInt>(kAppendIntegerArgument)(msg, out->science);
                out->sent = true;
            }
        }
        std::wstring list;
        for (int sci : sciences) list += std::to_wstring(sci) + L" ";
        wchar_t b[200];
        swprintf_s(b, L"spells: purchase sciences [%s] player %08X index %u points %u -> science %d cost %d owned %d "
                      L"noPrereq %d tooDear %d %s",
                   list.c_str(), player, index, points, out->science, out->cost, (int)out->owned, (int)out->noPrereq,
                   (int)out->tooDear, out->sent ? L"SENT" : L"not sent");
        LogLine(b);
    });
}

uint32_t SpellStoreButton(int slot) {
    uint32_t store = 0, button = 0;
    if (slot < 1 || slot > 20 || !SafeReadU32(kTheSpellStore, store) || !store ||
        !SafeReadU32(store + kStoreButtons + (slot - 1) * 8, button))
        return 0;
    return button;
}

namespace {
// The store's own choice (disassembly 2026-11-14 of game.dat b54aa41b): OnBttnSpell 0x8235A5 tests the
// pending science set at store +0x288 (vt+0 "has", thiscall one int), the prerequisites against that set
// (ScienceStore 0x5FED5B, thiscall (set, science)), then pushes the CommandButton onto the vector at
// store +0x27C (0x90BE00, thiscall (&vector, &button)) and adds the science and its cost to the set
// (0x82355F, thiscall one int; cost summed at set +0x14). Accept is OnBttnClose 0x82289A (thiscall, ret 4,
// the argument unread): it sets store +0x2A2 and plays "Close". The store's destructor (0x8233DF) then
// gives every button of the vector to TheControlBar 0x940435 (button, 1, 0), whose PURCHASE_SCIENCE case
// sends MSG_PURCHASE_SCIENCE. Without +0x2A2 a close clears the vector first (0x823484), so a close by
// Enter lost the choice, and the message the mod sent itself while the store was open was not bought
// (log 2026-11-14 23:18:07 and 23:18:15).
constexpr uint32_t kStorePending = 0x288, kStoreChosen = 0x27C, kStoreAccepted = 0x2A2, kPendingCost = 0x14;
constexpr uintptr_t kPrereqsMetInSet = 0x5FED5B, kVectorPushBack = 0x90BE00, kPendingAdd = 0x82355F;
constexpr uintptr_t kStoreOnBttnClose = 0x82289A, kStoreClose = 0x822A0A;
}  // namespace

namespace {
// No C++ objects in here: __try cannot sit in a function that unwinds them.
void StoreChooseBody(int slot, StoreChoice* out, uint32_t* pendingOut) {
        __try {
            uint32_t store = 0, button = 0, list = 0, player = LocalPlayer(), science = 0, sstore = 0, points = 0,
                     pending = 0;
            if (!SafeReadU32(kTheSpellStore, store) || !store || !player ||
                !SafeReadU32(store + kStoreButtons + (slot - 1) * 8, button) || !button ||
                !SafeReadU32(button + kButtonSciences, list) || !list || !SafeReadU32(list, science) ||
                !SafeReadU32(kTheScienceStore, sstore) || !sstore || !SafeReadU32(player + kPlayerSciencePoints, points)) {
                out->result = StoreChoice::kNoStore;
                return;
            }
            void* set = reinterpret_cast<void*>((uintptr_t)(store + kStorePending));
            void* s = reinterpret_cast<void*>((uintptr_t)sstore);
            out->science = (int)science;
            out->points = (int)points;
            SafeReadU32(store + kStorePending + kPendingCost, pending);
            out->cost = reinterpret_cast<int(__thiscall*)(void*, int)>(kScienceCost)(s, (int)science);
            uint8_t accepted = 0;
            SafeRead(store + kStoreAccepted, &accepted, 1);
            uint32_t setVt = 0, has = 0;
            if (reinterpret_cast<bool(__thiscall*)(void*, int)>(kPlayerHasScience)(
                    reinterpret_cast<void*>((uintptr_t)player), (int)science)) {
                out->result = StoreChoice::kOwned;
            } else if (accepted) {
                out->result = StoreChoice::kClosing;
            } else if (!SafeReadU32(store + kStorePending, setVt) || !SafeReadU32(setVt, has) || has < g_codeStart ||
                       has >= g_codeEnd) {
                out->result = StoreChoice::kNoStore;
            } else if (reinterpret_cast<bool(__thiscall*)(void*, int)>(has)(set, (int)science)) {
                out->result = StoreChoice::kPending;
            } else if (!reinterpret_cast<bool(__thiscall*)(void*, void*, int)>(kPrereqsMetInSet)(s, set, (int)science)) {
                out->result = StoreChoice::kNoPrereq;
            } else if (out->cost + (int)pending > (int)points) {
                out->result = StoreChoice::kTooDear;
            } else {
                uint32_t b = button;
                reinterpret_cast<void(__thiscall*)(void*, uint32_t*)>(kVectorPushBack)(
                    reinterpret_cast<void*>((uintptr_t)(store + kStoreChosen)), &b);
                reinterpret_cast<void(__thiscall*)(void*, int)>(kPendingAdd)(set, (int)science);
                reinterpret_cast<void(__thiscall*)(void*, int)>(kStoreOnBttnClose)(reinterpret_cast<void*>((uintptr_t)store), 0);
                SafeRead(store + kStoreAccepted, &accepted, 1);
                if (!accepted) {
                    // OnBttnClose returns early in game mode 6 unless TheInGameUI +0x16: the flag and the
                    // close OnClosed would run (0x822A80 -> 0x822A0A) are done here.
                    *reinterpret_cast<volatile uint8_t*>((uintptr_t)(store + kStoreAccepted)) = 1;
                    reinterpret_cast<void(*)()>(kStoreClose)();
                    out->forced = true;
                }
                out->result = StoreChoice::kChosen;
            }
            *pendingOut = pending;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            out->result = StoreChoice::kCrash;
        }
}
}  // namespace

void StoreChooseAndAccept(int slot, const std::shared_ptr<StoreChoice>& out) {
    RunOnGameThreadAndWait([slot, out] {
        uint32_t pending = 0;
        StoreChooseBody(slot, out.get(), &pending);
        wchar_t b[200];
        swprintf_s(b, L"spells: store choice slot %d science %d cost %d pending %u points %d -> result %d%s", slot,
                   out->science, out->cost, pending, out->points, out->result, out->forced ? L" (accept forced)" : L"");
        LogLine(b);
    });
}

std::vector<int> SciencesOwned(const std::vector<int>& sciences, int& points) {
    auto has = std::make_shared<std::vector<int>>(sciences.size(), -1);
    auto left = std::make_shared<int>(-1);
    RunOnGameThreadAndWait([sciences, has, left] {
        uint32_t player = LocalPlayer(), pts = 0;
        if (!player) return;
        if (SafeReadU32(player + kPlayerSciencePoints, pts)) *left = (int)pts;
        void* p = reinterpret_cast<void*>((uintptr_t)player);
        for (size_t i = 0; i < sciences.size(); ++i)
            (*has)[i] = reinterpret_cast<bool(__thiscall*)(void*, int)>(kPlayerHasScience)(p, sciences[i]) ? 1 : 0;
    });
    points = *left;
    return *has;
}

void TeleportObject(uint32_t obj, float x, float y, float z, HANDLE done) {
    RunOnGameThread([obj, x, y, z, done] {
        float pos[3] = {x, y, z};
        reinterpret_cast<void(__thiscall*)(void*, const float*, int)>(kObjectTeleport)(reinterpret_cast<void*>(obj), pos, 0);
        if (done) SetEvent(done);
    });
}

void FollowWaypointPath(uint32_t obj, float x, float y, float z, const std::string& label, HANDLE done, bool* ok) {
    RunOnGameThread([obj, x, y, z, label, done, ok] {
        uint32_t terrain = 0, vt = 0, fn = 0, ai = 0, way = 0;
        bool good = SafeReadU32(kTheTerrainLogic, terrain) && terrain && SafeReadU32(terrain, vt) &&
                    SafeReadU32(vt + kClosestWaypointOnPathSlot, fn) && fn >= g_codeStart && fn < g_codeEnd &&
                    SafeReadU32(obj + kObjAI, ai) && ai;
        if (good) {
            uint32_t str = 0;  // the AsciiString object: one pointer to its data
            reinterpret_cast<void*(__thiscall*)(void*, const char*)>(kAsciiStringCtor)(&str, label.c_str());
            float pos[3] = {x, y, z};
            way = reinterpret_cast<uint32_t(__thiscall*)(void*, const float*, const void*)>(fn)(
                reinterpret_cast<void*>(terrain), pos, &str);
            reinterpret_cast<void(__thiscall*)(void*)>(kAsciiStringDtor)(&str);
            good = way != 0;
        }
        if (good) {
            reinterpret_cast<void(__thiscall*)(void*)>(kLeaveGroup)(reinterpret_cast<void*>(obj));
            reinterpret_cast<void(__thiscall*)(void*, uint32_t, int)>(kFollowPathExact)(reinterpret_cast<void*>(ai + 0x20), way, 1);
        }
        wchar_t b[200];
        swprintf_s(b, L"order: follow path \"%hs\" from (%.0f, %.0f) %s (terrain %08X, closest %08X, ai %08X, waypoint %08X)",
                   label.c_str(), x, y, good ? L"started" : L"REFUSED", terrain, fn, ai, way);
        LogLine(b);
        if (ok) *ok = good;
        if (done) SetEvent(done);
    });
}
