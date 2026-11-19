#include "hints.h"
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>
#include "game.h"
#include "log.h"
#include "memory.h"
#include "objects_db.h"
#include "speech.h"
#include "strings.h"

namespace {

// ScriptActions::executeAction in game.dat 2.01 (disassembly 2026-11-01): starts with
// "mov eax, 0xB977B3; call 0xA3CEF0" (EH prolog), reads the ScriptAction* at [ebp+8],
// skips it when byte [action+0x41] is 0, then switches on [action+4] (600 cases, table
// 0x7CF857). Its first instruction is 5 bytes and position independent: the hook
// overwrites it with a jmp and the stub runs it again before jumping back.
constexpr uintptr_t kExecuteAction = 0x7CAFA5;
constexpr uintptr_t kExecuteActionResume = kExecuteAction + 5;
const uint8_t kExpected[10] = {0xB8, 0xB3, 0x77, 0xB9, 0x00, 0xE8, 0x41, 0x1F, 0x27, 0x00};

// ScriptAction: +4 type, +8 parameter count, +0xC Parameter* array (getParameter 0x602EFB).
// Parameter: +8 int, +0xC real, +0x10 AsciiString.
constexpr uint32_t kActType = 0x04, kActCount = 0x08, kActParams = 0x0C, kActEnabled = 0x41;
constexpr uint32_t kParamString = 0x10;

// Action types: index of each template in the ScriptEngine's action table (stride 0x80,
// names registered from 0x7D5270; DEBUG_MESSAGE_BOX 0, NO_OP 5 as in Zero Hour).
constexpr int kSoundPlayNamed = 84;    // sound name, object       -> doNamedSound 0x7BE239
constexpr int kSpeechPlay = 85;        // speech name, ...
constexpr int kMilitaryCaption = 142;  // label, seconds           -> TheInGameUI vt+0x5C
constexpr int kCountdownTimer = 163;   // timer name, label        -> 0x80CCD3
constexpr int kNotification = 589;     // type, label, seconds     -> 0x7C5507
constexpr int kNotificationImage = 593;  // type, label, seconds, object type (portrait)
// QUICKVICTORY (index 327: its names are stored at 0xA3AC = 327*0x80 + 0x2C, 0x7D57D4) ends
// the mission as won at once: the missions' own outros end with it (Orthanc "Orthanc - Outro -
// Movie", Council of Elrond "Victory Movie"). The mod's mission skip runs it.
constexpr int kQuickVictory = 327;

uintptr_t g_scriptActions = 0;     // ecx of the last executeAction call (ScriptActions)
volatile bool g_skipPending = false;

uintptr_t g_resume = kExecuteActionResume;
bool g_hooked = false;

std::unordered_map<std::string, std::wstring> g_mapText;  // upper-case label -> text (maptext.txt)

struct Message {
    std::wstring text;
    bool dialogue;
};
std::mutex g_lock;
std::deque<Message> g_history;  // oldest first
DWORD g_lastTalk = 0, g_lastShireTalk = 0;
std::vector<std::string> g_objectiveBoxes;
constexpr size_t kHistoryMax = 80;
int g_cursor = -1;              // index into g_history; reset to the newest on each message
bool g_speakDialogue = false;   // voiced lines are heard anyway (player, 2026-11-01)
std::wstring g_lastText;
DWORD g_lastTick = 0;
std::set<std::string> g_seen;  // upper-case labels shown this game (guides.ini [HINT:...])

std::string Upper(std::string s) {
    for (auto& c : s) c = (char)toupper((unsigned char)c);
    return s;
}

std::wstring Lookup(const std::string& label) {
    if (label.empty()) return {};
    auto it = g_mapText.find(Upper(label));
    if (it != g_mapText.end()) return it->second;  // map.str overrides lotr.str in the game
    std::wstring t = LookupStr(label);
    for (size_t p; (p = t.find(L"\\n")) != std::wstring::npos;) t.replace(p, 2, L". ");
    return t;
}

// "mgfotr_gandalf316" -> "Gandalf"; "ALEP_FLORIN" -> "Florin".
std::wstring SpeakerFromName(std::string s) {
    size_t u = s.rfind('_');
    if (u != std::string::npos) s = s.substr(u + 1);
    while (!s.empty() && isdigit((unsigned char)s.back())) s.pop_back();
    if (s.empty()) return {};
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    s[0] = (char)toupper((unsigned char)s[0]);
    return Widen(s.c_str());
}

std::wstring SpeakerFromTemplate(const std::string& tmpl) {
    if (tmpl.empty()) return {};
    const ObjectInfo* o = ObjectsDbFind(tmpl);
    return o && !o->displayName.empty() ? o->displayName : Widen(tmpl.c_str());
}

// Called on the game thread for every message; speaks it unless it is a dialogue line
// and dialogue reading is off. The same text twice within 5 s (a box and its sound) is kept once.
void Add(const std::wstring& text, bool dialogue, const std::string& logWhat) {
    if (text.empty()) return;
    bool speak = false;
    {
        std::lock_guard<std::mutex> l(g_lock);
        DWORD now = GetTickCount();
        if (text == g_lastText && now - g_lastTick < 5000) return;
        g_lastText = text;
        g_lastTick = now;
        g_history.push_back({text, dialogue});
        if (g_history.size() > kHistoryMax) g_history.pop_front();
        g_cursor = (int)g_history.size() - 1;
        speak = !dialogue || g_speakDialogue;
    }
    LogLine(L"hints: " + Widen(logWhat.c_str()) + (speak ? L" -> say \"" : L" -> history \"") + text + L"\"");
    if (speak) Speak(text, false);
}

std::string ParamString(uintptr_t action, int count, int i) {
    if (i >= count) return {};
    uint32_t param = 0, data = 0;
    if (!SafeReadU32(action + kActParams + 4 * i, param) || !param) return {};
    if (!SafeReadU32(param + kParamString, data)) return {};
    return game::ReadAsciiString(data);
}

void Remember(const std::string& label) {
    if (label.empty()) return;
    std::lock_guard<std::mutex> l(g_lock);
    g_seen.insert(Upper(label));
}

void Handle(uintptr_t action, int type) {
    uint32_t count = 0;
    if (!SafeReadU32(action + kActCount, count) || count > 16) return;
    int n = (int)count;
    if (type == kMilitaryCaption) Remember(ParamString(action, n, 0));
    if (type == kCountdownTimer || type == kNotification || type == kNotificationImage) Remember(ParamString(action, n, 1));
    switch (type) {
    case kMilitaryCaption: {
        std::string label = ParamString(action, n, 0);
        Add(Lookup(label), false, "caption " + label);
        break;
    }
    case kCountdownTimer: {
        std::string label = ParamString(action, n, 1);
        std::wstring t = Lookup(label);
        if (!t.empty()) Add(L"Таймер: " + t, false, "timer " + ParamString(action, n, 0) + " " + label);
        break;
    }
    case kNotification:
    case kNotificationImage: {
        std::string kind = ParamString(action, n, 0), label = ParamString(action, n, 1);
        std::string portrait = type == kNotificationImage ? ParamString(action, n, 3) : std::string();
        std::wstring t = Lookup(label);
        if (t.empty()) {
            LogLine(L"hints: notification " + Widen((kind + " " + label).c_str()) + L" has no text");
            break;
        }
        std::string k = Upper(kind);
        std::string what = "notification " + kind + " " + label + (portrait.empty() ? "" : " " + portrait);
        // New and completed objectives are already spoken by the scanner's objective watcher.
        if (k == "NEWOBJECTIVE" || k == "NEWBONUSOBJECTIVE" || k == "OBJECTIVECOMPLETED" ||
            k == "BONUSOBJECTIVECOMPLETED") {
            LogLine(L"hints: " + Widen(what.c_str()) + L" left to the objective watcher");
            if (k == "NEWOBJECTIVE" || k == "NEWBONUSOBJECTIVE") {
                std::lock_guard<std::mutex> l(g_lock);
                g_objectiveBoxes.push_back(label);
            }
            break;
        }
        if (k == "HINT" || k == "INSTRUCTIONAL") {
            Add(L"Подсказка: " + t, false, what);
        } else if (k == "OBJECTIVEREMINDER") {
            Add(L"Напоминание: " + t, false, what);
        } else if (k.compare(0, 5, "ALEP_") == 0) {
            g_lastTalk = g_lastShireTalk = GetTickCount();
            Add(SpeakerFromName(kind) + L": " + t, false, what);
        } else if (!portrait.empty()) {
            // A box with a character's portrait: the subtitle of a voiced line.
            g_lastTalk = GetTickCount();
            Add(SpeakerFromTemplate(portrait) + L": " + t, true, what);
        } else {
            Add(t, false, what);
        }
        break;
    }
    case kSoundPlayNamed:
    case kSpeechPlay: {
        std::string name = ParamString(action, n, 0);
        auto it = g_mapText.find("SCRIPT:" + Upper(name));
        if (it != g_mapText.end()) Add(SpeakerFromName(name) + L": " + it->second, true, "line " + name);
        break;
    }
    }
}

int ActionType(uintptr_t action) {
    __try {
        if (!*reinterpret_cast<const uint8_t*>(action + kActEnabled)) return -1;
        return *reinterpret_cast<const int*>(action + kActType);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool HandleGuarded(uintptr_t action, int type) {
    __try {
        Handle(action, type);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallExecuteAction(uint8_t* scriptAction) {
    using ExecuteAction = void(__thiscall*)(void* self, void* action);
    __try {
        reinterpret_cast<ExecuteAction>(kExecuteAction)(reinterpret_cast<void*>(g_scriptActions), scriptAction);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The mission skip, inside the game's own script processing: a copy of the action being run,
// turned into QUICKVICTORY with no parameters, goes through executeAction first.
void RunQuickVictory(uintptr_t action) {
    static uint8_t copy[0x80];
    if (!SafeRead(action, copy, sizeof copy) || !g_scriptActions) {
        LogLine(L"skip: could not copy a script action");
        return;
    }
    *reinterpret_cast<int*>(copy + kActType) = kQuickVictory;
    *reinterpret_cast<int*>(copy + kActCount) = 0;
    *reinterpret_cast<uint32_t*>(copy + kActParams) = 0;
    copy[kActEnabled] = 1;
    LogLine(CallExecuteAction(copy) ? L"skip: QUICKVICTORY executed" : L"skip: QUICKVICTORY faulted");
}

void __cdecl OnScriptAction(uintptr_t action) {
    if (!action) return;
    if (g_skipPending) {
        g_skipPending = false;
        RunQuickVictory(action);
    }
    int type = ActionType(action);
    if (type != kMilitaryCaption && type != kCountdownTimer && type != kNotification &&
        type != kNotificationImage && type != kSoundPlayNamed && type != kSpeechPlay)
        return;
    if (!HandleGuarded(action, type)) LogLine(L"hints: reading a script action faulted");
}

// Entry: [esp] = return address, [esp+4] = ScriptAction*, ecx = ScriptActions.
__declspec(naked) void HookStub() {
    __asm {
        mov dword ptr [g_scriptActions], ecx
        pushad
        pushfd
        mov eax, [esp + 40]
        push eax
        call OnScriptAction
        add esp, 4
        popfd
        popad
        mov eax, 0x00B977B3
        jmp dword ptr [g_resume]
    }
}

void LoadMapText(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        g_mapText[line.substr(0, tab)] = Widen(line.substr(tab + 1).c_str());
    }
    LogLine(L"hints: " + path + L": " + std::to_wstring(g_mapText.size()) + L" mission texts");
}

bool g_keyDown[256];
bool Pressed(int vk) {
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool edge = down && !g_keyDown[vk];
    g_keyDown[vk] = down;
    return edge;
}

}  // namespace

DWORD HintsLastTalk() { return g_lastTalk; }
std::vector<std::string> HintsTakeObjectiveBoxes() {
    std::lock_guard<std::mutex> l(g_lock);
    std::vector<std::string> out;
    out.swap(g_objectiveBoxes);
    return out;
}
DWORD HintsLastShireTalk() { return g_lastShireTalk; }

bool HintSeen(const std::string& upperLabel) {
    std::lock_guard<std::mutex> l(g_lock);
    return g_seen.count(upperLabel) > 0;
}

void HintsForget() {
    std::lock_guard<std::mutex> l(g_lock);
    g_seen.clear();
}

void HintsInit(const std::wstring& modDir) {
    LoadMapText(modDir + L"\\maptext.txt");
}

void HintsInstallHook() {
    if (g_hooked) return;
    uint8_t code[10];
    if (!SafeRead(kExecuteAction, code, sizeof code) || memcmp(code, kExpected, sizeof code) != 0) {
        wchar_t b[120];
        swprintf_s(b, L"hints: executeAction bytes differ (%02X %02X %02X %02X %02X %02X), no hook", code[0], code[1],
                   code[2], code[3], code[4], code[5]);
        LogLine(b);
        return;
    }
    DWORD old = 0;
    auto at = reinterpret_cast<uint8_t*>(kExecuteAction);
    if (!VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &old)) {
        LogLine(L"hints: VirtualProtect failed, no hook");
        return;
    }
    int32_t rel = (int32_t)((uintptr_t)&HookStub - (kExecuteAction + 5));
    at[0] = 0xE9;
    memcpy(at + 1, &rel, 4);
    VirtualProtect(at, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 5);
    g_hooked = true;
    wchar_t b[100];
    swprintf_s(b, L"hints: script action hook installed at %08IX -> %p", kExecuteAction, &HookStub);
    LogLine(b);
}

std::vector<std::wstring> HintsRecent(size_t count) {
    std::lock_guard<std::mutex> l(g_lock);
    std::vector<std::wstring> out;
    for (size_t i = g_history.size() > count ? g_history.size() - count : 0; i < g_history.size(); ++i)
        out.push_back(g_history[i].text);
    return out;
}

void HintsRequestVictory() {
    if (!g_hooked) { Speak(L"Пропуск невозможен: нет перехвата скриптов", true); return; }
    g_skipPending = true;
    LogLine(L"skip: requested by the player");
}

void HintsKeys(bool gameFocused) {
    if (!gameFocused) {
        g_keyDown[VK_OEM_4] = g_keyDown[VK_OEM_6] = false;
        return;
    }
    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    bool older = Pressed(VK_OEM_4), newer = Pressed(VK_OEM_6);
    if (!older && !newer) return;
    if (newer && ctrl) {
        bool on;
        {
            std::lock_guard<std::mutex> l(g_lock);
            on = g_speakDialogue = !g_speakDialogue;
        }
        Speak(on ? L"Реплики читаются вслух" : L"Реплики только в истории", true);
        LogLine(std::wstring(L"hints: dialogue reading ") + (on ? L"on" : L"off"));
        return;
    }
    if (ctrl) return;
    std::wstring say;
    {
        std::lock_guard<std::mutex> l(g_lock);
        int n = (int)g_history.size();
        if (n == 0) {
            say = L"Сообщений нет";
        } else {
            if (g_cursor < 0 || g_cursor >= n) g_cursor = n - 1;
            bool edge = false;
            if (older) {
                if (g_cursor > 0) --g_cursor; else edge = true;
            } else if (g_cursor < n - 1) {
                ++g_cursor;  // at the newest, ] repeats it
            }
            say = (edge ? L"Самое старое. " : L"") + g_history[g_cursor].text;
        }
    }
    Speak(say, true);
}
