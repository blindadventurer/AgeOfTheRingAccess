// Screen watcher ("eyes" v1): polls the game's shell screen stack, speaks the
// name of the top screen when it changes, and writes raw dumps for research.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <string>
#include "game.h"
#include "log.h"
#include "memory.h"
#include "scanner.h"
#include "screens.h"
#include "objects_research.h"
#include "speech.h"

namespace {

std::wstring g_dumpDir;
uintptr_t g_lastTop = 0;
int g_lastCount = -1;
int g_nameOffset = -1;  // offset of the file name AsciiString in a screen object, once found

bool EndsWithScreenExt(const std::string& s) {
    if (s.size() < 5) return false;
    std::string ext = s.substr(s.size() - 4);
    for (auto& c : ext) c = (char)tolower((unsigned char)c);
    return ext == ".apt" || ext == ".wnd";
}

// Looks inside a screen object for an AsciiString holding its file name.
std::string ScreenName(uintptr_t screen, int* foundOffset) {
    if (!screen) return {};
    auto tryOffset = [&](int off) -> std::string {
        uint32_t p = 0;
        if (!SafeReadU32(screen + off, p) || p < 0x10000 || !IsReadable(p)) return {};
        std::string s = game::ReadAsciiString(p);
        return EndsWithScreenExt(s) ? s : std::string();
    };
    if (g_nameOffset >= 0) {
        std::string s = tryOffset(g_nameOffset);
        if (!s.empty()) { if (foundOffset) *foundOffset = g_nameOffset; return s; }
    }
    for (int off = 0; off < 0x200; off += 4) {
        std::string s = tryOffset(off);
        if (!s.empty()) { if (foundOffset) *foundOffset = off; return s; }
    }
    return {};
}

std::wstring Hex(uint32_t v) {
    wchar_t b[16];
    swprintf_s(b, L"%08X", v);
    return b;
}

std::string Printable(const uint8_t* p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; ++i) s += (p[i] >= 0x20 && p[i] < 0x7F) ? (char)p[i] : '.';
    return s;
}

void DumpObject(FILE* f, const char* title, uintptr_t obj, int bytes) {
    fprintf(f, "\n== %s at %08X\n", title, (unsigned)obj);
    for (int off = 0; off < bytes; off += 4) {
        uint32_t v = 0;
        if (!SafeReadU32(obj + off, v)) { fprintf(f, "+%03X  <unreadable>\n", off); break; }
        fprintf(f, "+%03X  %08X", off, v);
        if (v >= 0x10000 && IsReadable(v)) {
            uint8_t peek[32] = {};
            if (SafeRead(v, peek, sizeof(peek))) {
                fprintf(f, "  -> ");
                for (int i = 0; i < 16; ++i) fprintf(f, "%02X ", peek[i]);
                fprintf(f, " |%s|", Printable(peek, sizeof(peek)).c_str());
                std::string as = game::ReadAsciiString(v);
                if (as.size() >= 3 && as.size() < 200) {
                    bool text = true;
                    for (char c : as) if ((unsigned char)c < 0x20) { text = false; break; }
                    if (text) fprintf(f, "  str=\"%s\"", as.c_str());
                }
            }
        }
        fprintf(f, "\n");
    }
}

// Best effort UTF-16 text behind a DisplayString*: tries the first pointers inside
// it as string data with the text at +8 (the AsciiString layout) or +4.
std::string DisplayText(uintptr_t ds) {
    if (ds < 0x10000 || !IsReadable(ds)) return {};
    for (int off = 0; off < 0x10; off += 4) {
        uint32_t p = 0;
        if (!SafeReadU32(ds + off, p) || p < 0x10000 || !IsReadable(p)) continue;
        for (int text : {8, 4}) {
            wchar_t buf[128] = {};
            if (!SafeRead(p + text, buf, sizeof(buf) - 2)) continue;
            int n = 0;
            while (n < 127 && buf[n] >= 0x20 && buf[n] < 0x2500) ++n;
            if (n >= 1 && buf[n] == 0) {
                // No fixed buffer here: 127 UTF-16 chars can need ~380 UTF-8 bytes, and an
                // overflowing sprintf_s killed the game (0xC0000409, 2026-10-31 22:44).
                return "+" + std::to_string(off) + "/+" + std::to_string(text) + " \"" +
                       Utf8(std::wstring(buf, n)) + "\"";
            }
        }
    }
    return {};
}

uint32_t g_seenStyles[64];  // gadget styles already sampled in this dump
int g_seenCount = 0;

// Research: every text reachable from obj through up to `depth` pointer hops.
void DeepText(FILE* f, uintptr_t obj, int depth, const std::string& prefix, int span, int& found) {
    for (int off = 0; off < span && found < 24; off += 4) {
        uint32_t p = 0;
        if (!SafeReadU32(obj + off, p) || p < 0x10000 || !IsReadable(p)) continue;
        char here[16];
        sprintf_s(here, "+%X", off);
        std::string t = DisplayText(p);
        if (!t.empty()) {
            fprintf(f, "      text at %s%s -> %s\n", prefix.c_str(), here, t.c_str());
            ++found;
        }
        if (depth > 0) DeepText(f, p, depth - 1, prefix + here + "->", 0x40, found);
    }
}

void DumpWindowTree(FILE* f, uintptr_t win, int depth, int& budget) {
    for (int guard = 0; win && budget > 0 && guard < 400; ++guard, --budget) {
        uint32_t status = 0, style = 0, region[4] = {}, text = 0, tip = 0, next = 0, child = 0, vt = 0;
        SafeReadU32(win, vt);
        SafeReadU32(win + game::kWinStatus, status);
        SafeRead(win + game::kWinRegion, region, sizeof(region));
        SafeReadU32(win + game::kWinStyle, style);
        SafeReadU32(win + game::kWinText, text);
        SafeReadU32(win + game::kWinTooltip, tip);
        SafeReadU32(win + game::kWinNext, next);
        SafeReadU32(win + game::kWinChild, child);
        fprintf(f, "%*s%08X vt=%08X status=%08X style=%08X region=(%d,%d)-(%d,%d) text=%.300s tip=%.300s\n",
                depth * 2, "", (unsigned)win, vt, status, style, (int)region[0], (int)region[1],
                (int)region[2], (int)region[3], DisplayText(text).c_str(), DisplayText(tip).c_str());
        // Visible gadgets (not hidden 0x10): look for their texts once per style.
        bool seen = false;
        for (int i = 0; i < g_seenCount; ++i) seen |= g_seenStyles[i] == style;
        if (style && !(status & 0x10) && !seen && g_seenCount < 64) {
            g_seenStyles[g_seenCount++] = style;
            uint32_t user = 0;
            SafeReadU32(win + 0x2C, user);
            fprintf(f, "%*s  [style %X sample] userData(+2C)=%08X\n", depth * 2, "", style, user);
            int found = 0;
            if (user) DeepText(f, user, 2, "user", 0x80, found);
            DeepText(f, win, 1, "win", 0x1F8, found);
        }
        if (child) DumpWindowTree(f, child, depth + 1, budget);
        win = next;
    }
}

}  // namespace

void ScreensInit(const std::wstring& dumpDir) { g_dumpDir = dumpDir; }

void ScreensTick() {
    uintptr_t shell = 0, stack[16];
    int count = 0;
    if (!game::ReadShell(shell, count, stack)) return;
    uintptr_t top = count > 0 ? stack[count - 1] : 0;
    if (top == g_lastTop && count == g_lastCount) return;
    g_lastTop = top;
    g_lastCount = count;

    std::wstring line = L"shell: count=" + std::to_wstring(count) + L" stack=";
    std::string topName;
    for (int i = 0; i < count; ++i) {
        int off = -1;
        std::string name = ScreenName(stack[i], &off);
        if (off >= 0 && g_nameOffset < 0) {
            g_nameOffset = off;
            LogLine(L"shell: screen file name found at offset +" + std::to_wstring(off));
        }
        line += L"[" + Hex((uint32_t)stack[i]) + L" " + Widen(name.c_str()) + L"]";
        if (i == count - 1) topName = name;
    }
    LogLine(line);

}

bool TopScreen(uintptr_t& screen, std::string& name, int& count) {
    uintptr_t shell = 0, stack[16];
    if (!game::ReadShell(shell, count, stack)) return false;
    screen = count > 0 ? stack[count - 1] : 0;
    name = screen ? ScreenName(screen, nullptr) : std::string();
    return true;
}

bool ShellHasScreen(const char* file) {
    uintptr_t shell = 0, stack[16];
    int count = 0;
    if (!game::ReadShell(shell, count, stack)) return false;
    for (int i = 0; i < count && i < 16; ++i)
        if (stack[i] && _stricmp(ScreenName(stack[i], nullptr).c_str(), file) == 0) return true;
    return false;
}

void ScreensDump() {
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t name[64];
    swprintf_s(name, L"\\dump-%02d%02d%02d.txt", t.wHour, t.wMinute, t.wSecond);
    std::wstring path = g_dumpDir + name;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"w") != 0 || !f) {
        LogLine(L"dump: cannot write " + path);
        return;
    }
    uintptr_t shell = 0, stack[16];
    int count = 0;
    if (!game::ReadShell(shell, count, stack)) {
        fprintf(f, "no shell\n");
    } else {
        fprintf(f, "TheShell=%08X count=%d name offset=%d\n", (unsigned)shell, count, g_nameOffset);
        DumpObject(f, "Shell", shell, 0x80);
        for (int i = count - 1; i >= 0; --i) {
            char title[64];
            sprintf_s(title, "screen[%d]%s %s", i, i == count - 1 ? " (top)" : "",
                      ScreenName(stack[i], nullptr).c_str());
            DumpObject(f, title, stack[i], 0x200);
        }
    }
    static game::BilEntry bil[256];
    int n = game::ReadButtonList(bil, 256);
    fprintf(f, "\n\nBUTTON LIST: %d entries\n", n);
    for (int i = 0; i < n; ++i) {
        uint32_t typeWord = 0;
        SafeReadU32(bil[i].cih + 4, typeWord);
        fprintf(f, "[%d] cih=%08X type=%d matrix a=%.3f b=%.3f c=%.3f d=%.3f tx=%.1f ty=%.1f\n", i,
                (unsigned)bil[i].cih, (int)typeWord >> 25, bil[i].a, bil[i].b, bil[i].c, bil[i].d,
                bil[i].tx, bil[i].ty);
    }
    uint32_t wm = 0, head = 0;
    SafeReadU32(game::kTheWindowManager, wm);
    if (wm) SafeReadU32(wm + game::kWmWindowList, head);
    fprintf(f, "\n\nWINDOW MANAGER %08X list head %08X\n", wm, head);
    if (wm) DumpObject(f, "WindowManager", wm, 0x40);
    int budget = 600;
    g_seenCount = 0;
    if (head) DumpWindowTree(f, head, 0, budget);
    uint32_t pool = 0;
    if (SafeReadU32(game::kAptPool, pool) && pool) DumpObject(f, "AptAnimationPool (gpPool)", pool, 0xB0);
    for (int k = 0; head && k < 3; ++k) {  // raw bytes of the first windows, to check the offsets
        char title[32];
        sprintf_s(title, "window #%d raw", k);
        DumpObject(f, title, head, 0x210);
        uint32_t next = 0;
        if (!SafeReadU32(head + game::kWinNext, next)) break;
        head = next;
    }

    for (int i = 0; i < n && i < 40; ++i) {
        char title[32];
        sprintf_s(title, "button[%d]", i);
        DumpObject(f, title, bil[i].cih, 0xA0);
    }
    ResearchObjects(f);  // research v5: objectives, radar events, War of the Ring state
    ScannerDump(f);
    fclose(f);
    LogLine(L"dump: wrote " + path);
    Speak(L"Снимок записан", true);
}
