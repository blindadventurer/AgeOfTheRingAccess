#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <deque>
#include <set>
#include <string>
#include <vector>

#include "log.h"
#include "memory.h"
#include "menu.h"
#include "screentext.h"
#include "speech.h"

namespace {

constexpr uintptr_t kTheDisplayStringManager = 0xDE4518;  // initSubsystem at 0x646834
constexpr size_t kKept = 80;

int g_headOff = -1, g_nextOff = -1;
DWORD g_lastTry = 0;
std::set<std::wstring> g_seen;
struct Recent { std::wstring text; DWORD when; };
std::deque<Recent> g_recent;  // newest last
std::set<std::wstring> g_alivePrev;  // texts alive on the last tick
std::deque<Recent> g_appeared;       // texts that came alive in the last 3 s
int g_cursor = -1;
bool g_boxOpen = false, g_keyDown = false;

uint32_t U32(uintptr_t a) {
    uint32_t v = 0;
    SafeReadU32(a, v);
    return v;
}

bool Ptr(uint32_t v) { return v >= 0x10000 && v < 0x7FFF0000; }

// The string at DisplayString +4: data pointer, UTF-16 chars at +8 (dump 2026-10-31 22:29).
std::wstring TextOf(uint32_t ds) {
    uint32_t data = U32(ds + 4);
    if (!Ptr(data)) return {};
    std::wstring out;
    wchar_t buf[33];
    for (uintptr_t p = data + 8; out.size() < 800; p += 64) {
        if (!SafeRead(p, buf, 64)) break;
        buf[32] = 0;
        size_t n = wcslen(buf);
        out.append(buf, n);
        if (n < 32) break;
    }
    for (wchar_t c : out)
        if (c >= 0x2E80 || (c < 32 && c != '\n' && c != '\r')) return {};
    return out;
}

// Longest chain of same-vtable blocks from a manager field, through one field of the blocks.
void Calibrate() {
    if (GetTickCount() - g_lastTry < 5000) return;
    g_lastTry = GetTickCount();
    uint32_t mgr = U32(kTheDisplayStringManager);
    if (!Ptr(mgr)) return;
    int bestLen = 0, bestH = -1, bestN = -1;
    for (int h = 4; h < 0x80; h += 4) {
        uint32_t head = U32(mgr + h);
        if (!Ptr(head)) continue;
        uint32_t vt = U32(head);
        if (vt < 0x400000 || vt > 0xED4000) continue;
        for (int n = 8; n < 0x40; n += 4) {
            int len = 0;
            for (uint32_t p = head; Ptr(p) && U32(p) == vt && len < 30000; p = U32(p + n)) ++len;
            if (len > bestLen) bestLen = len, bestH = h, bestN = n;
        }
    }
    wchar_t b[120];
    swprintf_s(b, L"screentext: list at manager+0x%X, next +0x%X, %d strings", bestH, bestN, bestLen);
    LogLine(b);
    if (bestLen >= 5) g_headOff = bestH, g_nextOff = bestN;
}

bool Worth(const std::wstring& t) {
    if (t.rfind(L"debug:", 0) == 0) return false;  // the veterancy screen's developer leftovers
    bool letters = false;
    for (wchar_t c : t) letters |= (c >= 'A' && c <= 'z') || c >= 0x80;
    return t.size() >= 2 && letters;
}

}  // namespace

std::vector<std::wstring> ScreenTextSince(DWORD when) {
    std::vector<std::wstring> out;
    for (const Recent& r : g_recent)
        if ((int)(r.when - when) >= 0) out.push_back(r.text);
    return out;
}

std::vector<std::wstring> ScreenTextNow(const wchar_t* prefix) {
    std::vector<std::wstring> out;
    uint32_t mgr = U32(kTheDisplayStringManager);
    if (g_headOff < 0 || !Ptr(mgr)) return out;
    size_t n = wcslen(prefix);
    int count = 0;
    for (uint32_t p = U32(mgr + g_headOff); Ptr(p) && count < 30000; p = U32(p + g_nextOff), ++count) {
        std::wstring t = TextOf(p);
        if (t.size() >= n && t.compare(0, n, prefix) == 0 && std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t);
    }
    return out;
}

void ScreenTextTick() {
    if (MenuInBattle() && !MenuOverlay()) return;  // the battle HUD renews its numbers all the time
    if (g_headOff < 0) { Calibrate(); return; }
    uint32_t mgr = U32(kTheDisplayStringManager);
    if (!Ptr(mgr)) return;
    DWORD now = GetTickCount();
    int count = 0;
    // Texts alive now; those not alive on the last tick "appeared", even if seen before: the save
    // dialog's "Save Progress", "Saving...", "Game Saved" come back each save, and the first-seen
    // rule kept them silent from the second save on (log 2026-11-09 01:14:47).
    std::set<std::wstring> alive;
    for (uint32_t p = U32(mgr + g_headOff); Ptr(p) && count < 30000; p = U32(p + g_nextOff), ++count) {
        std::wstring t = TextOf(p);
        if (!Worth(t)) continue;
        alive.insert(t);
        if (!g_seen.insert(t).second) continue;
        g_recent.push_back({t, now});
        if (g_recent.size() > kKept) g_recent.pop_front();
        g_cursor = (int)g_recent.size();
        LogLine(L"text: " + t.substr(0, 200));
    }
    if (g_seen.size() > 8000) g_seen.clear();
    for (const std::wstring& t : alive)
        if (!g_alivePrev.count(t)) g_appeared.push_back({t, now});
    while (!g_appeared.empty() && (now - g_appeared.front().when > 3000 || g_appeared.size() > 60)) g_appeared.pop_front();
    g_alivePrev.swap(alive);
    // A message box: what appeared with it (within 1.5 s) is read aloud when it opens, and what
    // appears while it stays open ("Saving..." then "Game Saved") as it comes.
    std::vector<HudButton> hud;
    MenuHudButtons(hud);
    bool box = false;
    // "popup": the Are you sure question over a pause menu (log 2026-11-04 22:25:26).
    for (const HudButton& b : hud) box |= b.path.find("MessageBox") != std::string::npos || b.path.find("popup") != std::string::npos ||
                                          b.path.find("dlgFrame") != std::string::npos;
    auto buttonWord = [](const std::wstring& t) {
        for (const wchar_t* w : {L"OK", L"YES", L"NO", L"CANCEL", L"ACCEPT", L"BACK"})
            if (_wcsicmp(t.c_str(), w) == 0) return true;
        return false;
    };
    static std::deque<Recent> spoken;  // a text that blinks is said once in 5 s
    while (!spoken.empty() && now - spoken.front().when > 5000) spoken.pop_front();
    if (box) {
        int said = 0;
        for (const Recent& r : g_appeared) {
            bool fresh = g_boxOpen ? r.when == now : now - r.when <= 1500;
            if (!fresh || buttonWord(r.text) || said >= 4) continue;
            if (std::any_of(spoken.begin(), spoken.end(), [&](const Recent& s) { return s.text == r.text; })) continue;
            spoken.push_back({r.text, now});
            Speak(r.text, false);
            LogLine(L"text: message box says " + r.text.substr(0, 200));
            ++said;
        }
    }
    g_boxOpen = box;
}

void ScreenTextKeys(bool gameFocused) {
    bool down = gameFocused && (GetAsyncKeyState(VK_OEM_1) & 0x8000) != 0;
    bool edge = down && !g_keyDown;
    g_keyDown = down;
    if (!edge) return;
    if (g_recent.empty()) { Speak(L"Текстов нет", true); return; }
    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    g_cursor += shift ? 1 : -1;
    int n = (int)g_recent.size();
    if (g_cursor < 0) g_cursor = 0;
    if (g_cursor >= n) g_cursor = n - 1;
    Speak(g_recent[g_cursor].text, true);
}
