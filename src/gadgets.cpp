// Reads Zero Hour window gadgets. Structures follow the Zero Hour source
// (GameClient/Gadget.h, read for layout only); GameWindow offsets were proven by
// the dump of 2026-10-31 22:29 (see NOTEBOOK.md). Gadget data layouts are
// HYPOTHESIS until the log shows sane texts.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "gadgets.h"
#include "gamethread.h"
#include "game.h"
#include "log.h"
#include "memory.h"
#include "strings.h"

namespace {

// Zero Hour GWS_* style bits.
constexpr uint32_t GWS_PUSH_BUTTON = 0x1, GWS_CHECK_BOX = 0x4, GWS_VERT_SLIDER = 0x8,
                   GWS_HORZ_SLIDER = 0x10, GWS_SCROLL_LISTBOX = 0x20, GWS_ENTRY_FIELD = 0x40,
                   GWS_COMBO_BOX = 0x8000;
constexpr uint32_t WIN_STATUS_HIDDEN = 0x10;

// DisplayString* -> text: string data pointer at +4, UTF-16 at +8, zero-terminated.
std::wstring DisplayString(uintptr_t ds) {
    uint32_t p = 0;
    if (ds < 0x10000 || !SafeReadU32(ds + 4, p) || p < 0x10000) return {};
    // Read in small chunks: a short string near the end of a heap block must not fail
    // because a fixed 400-byte read would cross into the next region.
    std::wstring s;
    for (int chunk = 0; chunk < 16; ++chunk) {
        wchar_t buf[16];
        if (!SafeRead(p + 8 + chunk * sizeof(buf), buf, sizeof(buf))) return {};
        for (wchar_t ch : buf) {
            if (!ch) return s;
            if (ch < 0x20 && ch != L'\n') return {};  // not text: wrong layout
            // ASCII bytes read as UTF-16 land in CJK and beyond (an image name such as
            // "AptDifficultyNotConquered" read this way, log 2026-10-31 23:44): not text.
            if (ch >= 0x2E80) return {};
            s += ch == L'\n' ? L' ' : ch;
        }
    }
    return s;
}

struct Box { int x0, y0, x1, y1; };

bool ReadWin(uintptr_t w, uint32_t& status, uint32_t& style, Box& r) {
    int reg[4];
    if (!SafeReadU32(w + game::kWinStatus, status) || !SafeReadU32(w + game::kWinStyle, style) ||
        !SafeRead(w + game::kWinRegion, reg, sizeof(reg)))
        return false;
    r = {reg[0], reg[1], reg[2], reg[3]};
    return true;
}

// RotWK 2.01 ListboxData, read from raw dumps of 2026-10-31 23:33 (a 12-entry faction
// drop-down and the 368-map list): +0 short listLength, +2 short columns, then 12 bytes
// of flags (Zero Hour has 7), +0x14 column widths, +0x18 rows, +0x1C up button,
// +0x20 down button, +0x24 slider, +0x28 total height, +0x2C short endPos (rows in use),
// +0x30 selectPos, +0x3C displayHeight (equals the window height), +0x44 displayPos
// (scroll in pixels; 3632 seen in a scrolled map list: HYPOTHESIS). The row and cell
// layouts are found by trying the plausible ones and keeping the one that yields text.
constexpr uint32_t kLbRows = 0x18, kLbEnd = 0x2C, kLbSelect = 0x34, kLbDispH = 0x3C, kLbDispPos = 0x44;

struct RowLayout {
    int rowStride = 12, cellsOff = 8, cellStride = 0x18, dataOff = 8;
    bool ok = false;
};

// Image cell: the data points to an image whose +4 is an AsciiString name (u16 length
// at +4, chars at +8), e.g. "AptDifficultyNotConquered". Empty when it is not one.
std::string ImageName(uintptr_t img) {
    uint32_t p = 0;
    uint16_t len = 0;
    if (img < 0x10000 || !SafeReadU32(img + 4, p) || p < 0x10000 || !SafeRead(p + 4, &len, 2) || len < 3 || len > 64)
        return {};
    char buf[65];
    if (!SafeRead(p + 8, buf, len)) return {};
    buf[len] = 0;
    for (int i = 0; i < len; ++i)
        if (!isalnum((unsigned char)buf[i]) && buf[i] != '_') return {};
    return buf;
}

// Words for the list icons met so far; unknown icons are logged once and left out.
std::wstring IconWords(const std::string& name) {
    static std::vector<std::string> seen;
    if (std::find(seen.begin(), seen.end(), name) == seen.end() && seen.size() < 200) {
        seen.push_back(name);
        LogLine(L"gadgets: list icon " + Widen(name.c_str()));
    }
    if (name.find("NotConquered") != std::string::npos) return L"не пройдена";
    if (name.find("Conquered") != std::string::npos) return L"пройдена";
    return {};
}

// Text cells joined; `textCells` counts the cells that held real text (for layout scoring).
std::wstring RowText(uintptr_t cells, int cols, const RowLayout& L, int* textCells = nullptr) {
    std::wstring text;
    for (int c = 0; c < cols; ++c) {
        uint32_t ds = 0;
        if (!SafeReadU32(cells + c * L.cellStride + L.dataOff, ds)) break;
        std::wstring t = DisplayString(ds);
        if (!t.empty()) {
            if (textCells) ++*textCells;
        } else if (!textCells) {
            t = IconWords(ImageName(ds));
        }
        if (t.empty()) continue;
        if (!text.empty()) text += L", ";
        text += t;
    }
    return text;
}

RowLayout DetectRows(uintptr_t rows, int cols, int used) {
    RowLayout best;
    int bestScore = 0;
    int probe = used < 6 ? used : 6;
    for (int rs : {12, 16, 20, 24, 8})
        for (int co : {8, 4, 12, 16, 0})
            for (int cs : {0x18, 0x1C, 0x20, 0x14, 0x24, 0x10})
                for (int dof : {8, 4, 0xC, 0x10, 0}) {
                    RowLayout L{rs, co, cs, dof, true};
                    // Score: real text cells over the probed rows, so a layout reading every
                    // text column beats one that only hits a single column.
                    int score = 0;
                    for (int i = 0; i < probe; ++i) {
                        uint32_t cells = 0;
                        if (SafeReadU32(rows + i * rs + co, cells) && cells > 0x10000) RowText(cells, cols, L, &score);
                    }
                    if (score > bestScore) { bestScore = score; best = L; }
                    if (bestScore == probe * cols && probe > 0) return best;
                }
    best.ok = bestScore > 0;
    return best;
}

std::map<uintptr_t, RowLayout> g_layouts;  // per ListboxData, found once

const RowLayout* LayoutFor(uintptr_t data, uintptr_t rows, int cols, int used) {
    auto it = g_layouts.find(data);
    if (it != g_layouts.end() && it->second.ok) return &it->second;
    RowLayout L = DetectRows(rows, cols, used);
    char b[160];
    sprintf_s(b, "gadgets: list data %08X rows %08X cols %d used %d -> layout %s stride %d cells +%d cell %d text +%d",
              (unsigned)data, (unsigned)rows, cols, used, L.ok ? "found" : "NOT found", L.rowStride, L.cellsOff,
              L.cellStride, L.dataOff);
    LogLine(Widen(b));
    if (!L.ok) {  // dump the first rows raw, once, for research
        uint32_t raw[16] = {};
        SafeRead(rows, raw, sizeof(raw));
        std::string hex = "gadgets: rows raw";
        for (uint32_t v : raw) { char h[12]; sprintf_s(h, " %08X", v); hex += h; }
        uint32_t cells = 0, craw[12] = {};
        if (SafeReadU32(rows + 8, cells) && cells) SafeRead(cells, craw, sizeof(craw));
        hex += " | cells0";
        for (uint32_t v : craw) { char h[12]; sprintf_s(h, " %08X", v); hex += h; }
        LogLine(Widen(hex.c_str()));
        if (g_layouts.size() < 500) g_layouts[data] = L;  // remember the failure, do not retry every tick
        return nullptr;
    }
    g_layouts[data] = L;
    return &g_layouts[data];
}

struct ListState {
    int16_t len = 0, cols = 0, used = 0;
    uint32_t rows = 0;
    int dispH = 0, dispPos = 0, select = -1;
    const RowLayout* L = nullptr;
};

bool ReadList(uintptr_t data, ListState& s) {
    if (!SafeRead(data, &s.len, 2) || !SafeRead(data + 2, &s.cols, 2)) return false;
    if (s.len <= 0 || s.len > 5000 || s.cols <= 0 || s.cols > 16) return false;
    if (!SafeReadU32(data + kLbRows, s.rows) || s.rows < 0x10000) return false;
    if (!SafeRead(data + kLbEnd, &s.used, 2) || s.used < 0 || s.used > s.len) return false;
    SafeRead(data + kLbDispH, &s.dispH, 4);
    SafeRead(data + kLbDispPos, &s.dispPos, 4);
    SafeRead(data + kLbSelect, &s.select, 4);
    if (s.dispPos < 0 || s.dispPos > 1000000) s.dispPos = 0;
    s.L = s.used > 0 ? LayoutFor(data, s.rows, s.cols, s.used) : nullptr;
    return s.L != nullptr;
}

bool RowGeometry(const ListState& s, int i, int& top, int& h) {
    uint32_t bottom = 0;
    uint8_t hb = 0;
    if (!SafeReadU32(s.rows + i * s.L->rowStride, bottom) || !SafeRead(s.rows + i * s.L->rowStride + 4, &hb, 1))
        return false;
    h = hb ? hb : 20;
    top = (int)bottom - h - s.dispPos;
    return true;
}

// Absolute client position of a window: its region plus every parent's.
bool WindowOrigin(uintptr_t w, int& x, int& y, int& width, int& height) {
    x = y = 0;
    bool first = true;
    for (int depth = 0; w && depth < 16; ++depth) {
        int reg[4];
        if (!SafeRead(w + game::kWinRegion, reg, sizeof(reg))) return false;
        if (first) { width = reg[2] - reg[0]; height = reg[3] - reg[1]; first = false; }
        x += reg[0];
        y += reg[1];
        uint32_t parent = 0;
        if (!SafeReadU32(w + game::kWinParent, parent)) break;
        w = parent;
    }
    return true;
}

// Every row in use, visible or not; rows out of view are marked so a click scrolls first.
void ListRows(uintptr_t listWin, uintptr_t data, int absX, int absY, const Box& r, bool inCombo,
              std::vector<Gadget>& out) {
    ListState s;
    if (!ReadList(data, s)) return;
    int height = s.dispH > 0 ? s.dispH : r.y1 - r.y0;
    for (int i = 0; i < s.used && i < 1000; ++i) {
        uint32_t cells = 0;
        int top = 0, h = 0;
        if (!SafeReadU32(s.rows + i * s.L->rowStride + s.L->cellsOff, cells) || !cells || !RowGeometry(s, i, top, h))
            continue;
        // Long lists (the 368 maps) are read every 100 ms: reuse a row's text while its
        // cell array is the same, so scanning stays cheap (the list lagged on 2026-10-31).
        std::wstring text;
        static std::map<uintptr_t, std::wstring> cache;
        if (s.used > 50) {
            auto c = cache.find(cells);
            if (c != cache.end()) text = c->second;
            else {
                if (cache.size() > 6000) cache.clear();
                text = cache[cells] = RowText(cells, s.cols, *s.L);
            }
        } else {
            text = RowText(cells, s.cols, *s.L);
        }
        if (text.empty()) continue;
        bool visible = top >= -h / 2 && top + h / 2 <= height;
        Gadget g{Gadget::ListRow, listWin, i, text, absX + (r.x1 - r.x0) / 2.f, absY + top + h / 2.f, inCombo};
        g.visible = visible;
        g.selected = i == s.select;
        g.total = s.used;
        out.push_back(g);
    }
}

void Walk(uintptr_t w, int parentX, int parentY, int depth, int& budget, std::vector<Gadget>& out) {
    for (int guard = 0; w && budget > 0 && guard < 600; ++guard, --budget) {
        uint32_t status = 0, style = 0, next = 0, child = 0, user = 0, text = 0;
        Box r{};
        if (!ReadWin(w, status, style, r)) return;
        SafeReadU32(w + game::kWinNext, next);
        if (status & WIN_STATUS_HIDDEN) { w = next; continue; }  // hidden: skip it and its children
        SafeReadU32(w + game::kWinChild, child);
        SafeReadU32(w + 0x2C, user);
        SafeReadU32(w + game::kWinText, text);
        int ax = parentX + r.x0, ay = parentY + r.y0;
        float cx = ax + (r.x1 - r.x0) / 2.f, cy = ay + (r.y1 - r.y0) / 2.f;
        bool descend = true;
        if (style & GWS_COMBO_BOX) {
            // ComboBoxData: +0x14 EntryData* (its +0 DisplayString* is the shown text), +0x28 list box window.
            uint32_t entry = 0, ds = 0, listWin = 0;
            std::wstring shown;
            if (user && SafeReadU32(user + 0x14, entry) && entry && SafeReadU32(entry, ds)) shown = DisplayString(ds);
            out.push_back({Gadget::Combo, w, -1, shown, cx, cy});
            // An open drop-down shows its list box child (style 0x20, hidden while closed).
            // Found by walking the children: ComboBoxData+0x28 gave garbage on 2026-10-31.
            (void)listWin;
            uint32_t c = child;
            for (int k = 0; c && k < 8; ++k) {
                uint32_t ls = 0, lst = 0, ldata = 0, nx = 0;
                Box lr{};
                if (!ReadWin(c, ls, lst, lr)) break;
                if ((lst & GWS_SCROLL_LISTBOX) && !(ls & WIN_STATUS_HIDDEN) && SafeReadU32(c + 0x2C, ldata) && ldata)
                    ListRows(c, ldata, ax + lr.x0, ay + lr.y0, lr, true, out);
                if (!SafeReadU32(c + game::kWinNext, nx)) break;
                c = nx;
            }
            descend = false;
        } else if ((style & GWS_SCROLL_LISTBOX) && user) {
            ListRows(w, user, ax, ay, r, false, out);
            descend = false;
        } else if (style & GWS_CHECK_BOX) {
            out.push_back({Gadget::CheckBox, w, -1, DisplayString(text), cx, cy});
        } else if ((style & GWS_ENTRY_FIELD) && user) {
            uint32_t ds = 0;
            SafeReadU32(user, ds);
            out.push_back({Gadget::Entry, w, -1, DisplayString(ds), cx, cy});
        } else if ((style & (GWS_HORZ_SLIDER | GWS_VERT_SLIDER)) && depth > 0 && user) {
            // SliderData: +0 min, +4 max, +0xC position. Skip list scroll bars (children of lists).
            int v[4] = {};
            if (SafeRead(user, v, sizeof(v)) && v[1] > v[0])
                out.push_back({Gadget::Slider, w, -1,
                               std::to_wstring((v[3] - v[0]) * 100 / (v[1] - v[0])) + L"%", cx, cy});
            descend = false;
        } else if ((style & GWS_PUSH_BUTTON) && depth > 0) {
            // The tooltip says more than a one-character caption ("1" -> "Start Position for player 1").
            uint32_t tip = 0;
            SafeReadU32(w + game::kWinTooltip, tip);
            std::wstring t = DisplayString(text), tt = DisplayString(tip);
            if (t.size() <= 2 && !tt.empty()) t = tt;
            if (!t.empty()) out.push_back({Gadget::Button, w, -1, t, cx, cy});
        }
        if (descend && child) Walk(child, ax, ay, depth + 1, budget, out);
        w = next;
    }
}

}  // namespace

// Selects a row the way the game's own code does: GadgetListBoxSystem (0x727A0B, cdecl, window,
// message, data1, data2), message 0x4017 = set selection, data1 = int* rows, data2 = count. In a
// single-selection list it stores selectPos at +0x34. It tells the owner (message 0x4014) only
// when the list's parent is a combo box (case at 0x7281BE, style test at 0x72824A); a click on a
// list always does (0x72730B). So for a plain list the same message is sent here: owner = window
// +0x44 (0x982548), TheWindowManager vt+0xE8 (target, message, list, row). The map list's owner
// then looks the map up by row and changes it (0x8459A5 -> 0x8425E9, disassembly 2026-11-04).
// A click on a push button ends in one message to its owner (0x729C52 in the gadget's input code;
// the keyboard code at 0x9138E7 sends the same): TheWindowManager vt+0xE8 (owner = window +0x44,
// GBM_SELECTED 0x4008, the window, its id = window +0x34). No mouse: it reaches the ring around a
// building whatever the camera shows.
bool PressButtonWindow(uintptr_t win) {
    using Send = int(__thiscall*)(void*, uint32_t, uint32_t, uint32_t, uint32_t);
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!done) return false;
    RunOnGameThread([=] {
        uint32_t owner = 0, id = 0, wm = 0, vt = 0, fn = 0;
        if (SafeReadU32(win + 0x44, owner) && owner && SafeReadU32(win + 0x34, id) &&
            SafeReadU32(game::kTheWindowManager, wm) && wm && SafeReadU32(wm, vt) && SafeReadU32(vt + 0xE8, fn) && fn)
            reinterpret_cast<Send>(fn)(reinterpret_cast<void*>(wm), owner, 0x4008, (uint32_t)win, id);
        SetEvent(done);
    });
    bool ran = WaitForSingleObject(done, 1000) == WAIT_OBJECT_0;
    if (ran) CloseHandle(done);
    return ran;
}

std::wstring WindowText(uintptr_t win, bool tooltip) {
    uint32_t ds = 0;
    SafeReadU32(win + (tooltip ? game::kWinTooltip : game::kWinText), ds);
    return ds ? DisplayString(ds) : std::wstring();
}

bool SelectListRow(uintptr_t listWin, int row) {
    using System = int(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t);
    using Send = int(__thiscall*)(void*, uint32_t, uint32_t, uint32_t, uint32_t);
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!done) return false;
    RunOnGameThread([=] {
        int r = row;
        reinterpret_cast<System>(0x727A0B)((uint32_t)listWin, 0x4017, (uint32_t)&r, 1);
        uint32_t parent = 0, style = 0, owner = 0, wm = 0, vt = 0, fn = 0;
        SafeReadU32(listWin + game::kWinParent, parent);
        if (parent) SafeReadU32(parent + game::kWinStyle, style);
        if (!(style & GWS_COMBO_BOX) && SafeReadU32(listWin + 0x44, owner) && owner &&
            SafeReadU32(game::kTheWindowManager, wm) && wm && SafeReadU32(wm, vt) && SafeReadU32(vt + 0xE8, fn) && fn) {
            reinterpret_cast<Send>(fn)(reinterpret_cast<void*>(wm), owner, 0x4014, (uint32_t)listWin, (uint32_t)r);
            LogLine(L"gadgets: told the list's owner about row " + std::to_wstring(r));
        }
        SetEvent(done);
    });
    bool ran = WaitForSingleObject(done, 1000) == WAIT_OBJECT_0;
    if (ran) CloseHandle(done);
    return ran;
}

bool ComboChoices(uintptr_t combo, std::vector<std::wstring>& rows, uintptr_t& listWin) {
    rows.clear();
    uint32_t c = 0;
    SafeReadU32(combo + game::kWinChild, c);
    for (int k = 0; c && k < 8; ++k) {
        uint32_t ls = 0, lst = 0, data = 0;
        Box r{};
        ListState st;
        if (!ReadWin(c, ls, lst, r)) return false;
        if ((lst & GWS_SCROLL_LISTBOX) && SafeReadU32(c + 0x2C, data) && data && ReadList(data, st)) {
            listWin = c;
            for (int i = 0; i < st.used; ++i) {
                uint32_t cells = 0;
                if (!SafeReadU32(st.rows + i * st.L->rowStride + st.L->cellsOff, cells) || !cells) break;
                rows.push_back(RowText(cells, st.cols, *st.L));
            }
            return !rows.empty();
        }
        if (!SafeReadU32(c + game::kWinNext, c)) break;
    }
    return false;
}

bool PrepareListRowClick(uintptr_t listWin, int row, float& cx, float& cy) {
    uint32_t data = 0;
    ListState s;
    if (!SafeReadU32(listWin + 0x2C, data) || !data || !ReadList(data, s) || row < 0 || row >= s.used) return false;
    int x, y, w, h;
    if (!WindowOrigin(listWin, x, y, w, h)) return false;
    int view = s.dispH > 0 ? s.dispH : h;
    int top = 0, rh = 0;
    if (!RowGeometry(s, row, top, rh)) return false;
    if (top < 0 || top + rh > view) {
        // Scroll the list so the row sits in the middle: write displayPos, as the game's own
        // "set top visible entry" does (memory write; logged).
        int total = 0;
        SafeRead(data + 0x28, &total, 4);
        int rowTop = top + s.dispPos;
        int pos = rowTop - (view - rh) / 2;
        if (pos > total - view) pos = total - view;
        if (pos < 0) pos = 0;
        if (!SafeWriteI32(data + kLbDispPos, pos)) return false;
        LogLine(L"gadgets: scrolled list to " + std::to_wstring(pos) + L" for row " + std::to_wstring(row));
        Sleep(150);
        if (!ReadList(data, s) || !RowGeometry(s, row, top, rh)) return false;
    }
    cx = x + w / 2.f;
    cy = y + top + rh / 2.f;
    return true;
}

std::wstring ListDiagnostics(uintptr_t listWin) {
    uint32_t data = 0;
    if (!SafeReadU32(listWin + 0x2C, data) || !data) return L"no list data";
    // ListboxData +0x28..+0x5F (endPos, selectPos, displayHeight, displayPos candidates), the
    // first rows' bottoms and heights, the window region, and the scroll slider's position:
    // kLbDispPos = +0x44 is a HYPOTHESIS that failed on 2026-11-01 04:57-04:58 (clicks on rows
    // of the faction drop-down and of a scrolled map list landed on other rows).
    wchar_t b[96];
    std::wstring out = L"data+28:";
    for (uint32_t off = 0x28; off < 0x60; off += 4) {
        uint32_t v = 0;
        SafeReadU32(data + off, v);
        swprintf_s(b, L" %X=%d", off, (int)v);
        out += b;
    }
    ListState s;
    if (ReadList(data, s)) {
        out += L" | rows";
        for (int i = 0; i < s.used && i < 4; ++i) {
            uint32_t bottom = 0;
            uint8_t hb = 0;
            SafeReadU32(s.rows + i * s.L->rowStride, bottom);
            SafeRead(s.rows + i * s.L->rowStride + 4, &hb, 1);
            swprintf_s(b, L" %d:%d/%d", i, (int)bottom, hb);
            out += b;
        }
    }
    int x, y, w, h;
    if (WindowOrigin(listWin, x, y, w, h)) {
        swprintf_s(b, L" | window (%d,%d) %dx%d", x, y, w, h);
        out += b;
    }
    // The list's slider: a child window whose user data reads as SliderData {min, max, ?, pos}.
    uint32_t child = 0;
    SafeReadU32(listWin + game::kWinChild, child);
    for (int n = 0; child && n < 8; ++n) {
        uint32_t user = 0, style = 0;
        SafeReadU32(child + 0x2C, user);
        SafeReadU32(child + game::kWinStyle, style);
        int v[4] = {};
        if (user && SafeRead(user, v, sizeof(v))) {
            swprintf_s(b, L" | child style %X data %d %d %d %d", style, v[0], v[1], v[2], v[3]);
            out += b;
        }
        SafeReadU32(child + game::kWinNext, child);
    }
    return out;
}

int ListSelection(uintptr_t listWin) {
    uint32_t data = 0;
    int sel = -1;
    if (SafeReadU32(listWin + 0x2C, data) && data) SafeRead(data + kLbSelect, &sel, 4);
    return sel;
}

// Match rules (Skirmish RULES tab, War of the Ring setup). The movie's RuleComboBox_N /
// RuleCheckBox_N clips (MpGameSetup.const) wrap window gadgets; their RuleText caption is an
// APT text the mod cannot read, so a rule combo is named by its values. The game's rule
// records (game.dat 0xC82528..0xC826F0, {rule id, values, count, default}; names from the
// table at 0xDB7794 through lotr.str "RULE:<name>", disassembly 2026-11-01):
//   3 CommandPointFactor  1/3X 1/2X 1X 2X 4X 8X 10X       4 InitialResources 500..4000
//   6 StrategicPhaseTimer No Timer, 5..500 seconds         7 BattleType  Auto Resolve and RTS / Auto Resolve / RTS
//   8 BattleChoice        Auto Resolve / RTS               9 AutoResolveType  Dynamic / Quick
//   5 VictoryType         three LWScenario game types, World Domination - 20/30/40/All
// Check boxes 0 AllowCustomHeroes, 2 AllowRingHeroes, 1 ClanGame, in that order in both rule
// sets (record table 0xDB77BC). The default value carries "*" (VALUE:Default).
std::wstring RuleName(const char* key, const wchar_t* fallback) {
    std::wstring t = LookupStr(std::string("RULE:") + key);
    return t.empty() ? fallback : t;
}

// Rows in use of a combo's drop-down list (open or not), -1 when unreadable.
int ComboRows(uintptr_t combo) {
    uint32_t c = 0;
    SafeReadU32(combo + game::kWinChild, c);
    for (int k = 0; c && k < 8; ++k) {
        uint32_t ls = 0, lst = 0, data = 0;
        Box r{};
        if (!ReadWin(c, ls, lst, r)) break;
        ListState st;
        if ((lst & GWS_SCROLL_LISTBOX) && SafeReadU32(c + 0x2C, data) && data && ReadList(data, st)) return st.used;
        if (!SafeReadU32(c + game::kWinNext, c)) break;
    }
    return -1;
}

std::wstring RuleForCombo(const Gadget& g) {
    std::wstring v = g.text;
    while (!v.empty() && (v.back() == L'*' || v.back() == L' ')) v.pop_back();
    if (v.empty()) return {};
    bool digits = std::all_of(v.begin(), v.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
    if (digits && _wtoi(v.c_str()) >= 500) return RuleName("InitialResources", L"Initial Resources");
    if (v.back() == L'X' && iswdigit(v[0])) return RuleName("CommandPointFactor", L"Command Point Factor");
    std::wstring noTimer = LookupStr("VALUE:NoTimer"), seconds = LookupStr("VALUE:Seconds");  // "%d seconds"
    std::wstring secWord = seconds.size() > 3 ? seconds.substr(seconds.find(L' ') + 1) : L"seconds";
    if (v == noTimer || (v.size() > secWord.size() && v.compare(v.size() - secWord.size(), secWord.size(), secWord) == 0))
        return RuleName("StrategicPhaseTimer", L"Tactical Phase Timer");
    if (v == LookupStr("VALUE:Dynamic") || v == LookupStr("VALUE:Quick")) return RuleName("AutoResolveType", L"Auto-Resolve Display");
    if (v == LookupStr("VALUE:AutoResolveAndRTS")) return RuleName("BattleType", L"Battle Type");
    if (v == LookupStr("VALUE:AutoResolve") || v == LookupStr("VALUE:RTS"))
        return ComboRows(g.window) == 2 ? RuleName("BattleChoice", L"Battle Type Priority") : RuleName("BattleType", L"Battle Type");
    for (const char* k : {"VALUE:WorldDomA", "VALUE:WorldDomB", "VALUE:WorldDomC", "VALUE:WorldDomAll", "LWScenario:WOTRGameType001",
                          "LWScenario:WOTRGameType002", "LWScenario:WOTRGameType004"})
        if (v == LookupStr(k)) return RuleName("VictoryType", L"Victory Type");
    for (const char* k : {"VALUE:MapRevealModeExplored", "VALUE:MapRevealModeRevealed", "VALUE:MapRevealModeUnrevealed"})
        if (v == LookupStr(k)) return RuleName("MapRevealMode", L"Map Shroud Settings");
    return {};
}

void NameRules(std::vector<Gadget>& out) {
    bool rules = false;
    for (auto& g : out)
        if (g.kind == Gadget::Combo && (g.rule = RuleForCombo(g), !g.rule.empty())) rules = true;
    if (!rules) return;
    // Caption-less check boxes beside rule combos: the rule check boxes, top to bottom
    // in the game's order (HYPOTHESIS: the order on screen follows the box numbers).
    std::vector<Gadget*> boxes;
    for (auto& g : out)
        if (g.kind == Gadget::CheckBox && g.text.empty()) boxes.push_back(&g);
    std::sort(boxes.begin(), boxes.end(), [](const Gadget* a, const Gadget* b) { return a->cy != b->cy ? a->cy < b->cy : a->cx < b->cx; });
    const std::wstring names[] = {RuleName("AllowCustomHeroes", L"Allow Custom Heroes"), RuleName("AllowRingHeroes", L"Allow Ring Heroes"),
                                  RuleName("ClanGame", L"Clan Game")};
    for (size_t i = 0; i < boxes.size() && i < 3; ++i) boxes[i]->rule = names[i];
    std::wstring summary;
    for (const auto& g : out)
        if (!g.rule.empty()) summary += L" [" + g.rule + L" = " + g.text + L" at " + std::to_wstring((int)g.cx) + L"," +
                                        std::to_wstring((int)g.cy) + L"]";
    static std::wstring last;
    if (summary != last) {
        last = summary;
        LogLine(L"gadgets: rules" + summary);
    }
}

// Player matrix of the game setup (Skirmish, lobbies): rows of five combos. Column headers
// from MpGameSetup.const ($GUI:Player, $HeaderFaction, $HeaderHero, $GUI:Team, $Color (a user
// window, not a combo), $HdrHandicap) through lotr.str: Player, Army, Captain, Team, Handicap.
// Window positions from the dump of 2026-10-31 22:29: combos at x 277-691, 691-890, 891-1306,
// 1306-1411, 1510-1667 on rows 38 pixels apart.
void NameMatrix(std::vector<Gadget>& out) {
    std::vector<Gadget*> combos;
    for (auto& g : out)
        if (g.kind == Gadget::Combo && g.rule.empty()) combos.push_back(&g);
    std::sort(combos.begin(), combos.end(), [](const Gadget* a, const Gadget* b) { return a->cy != b->cy ? a->cy < b->cy : a->cx < b->cx; });
    const char* keys[] = {"GUI:Player", "APT:HeaderFaction", "APT:HeaderHero", "GUI:Team", "APT:HdrHandicap"};
    const wchar_t* fallback[] = {L"Player", L"Army", L"Captain", L"Team", L"Handicap"};
    int row = 0;
    for (size_t i = 0; i < combos.size();) {
        size_t j = i;
        while (j < combos.size() && std::fabs(combos[j]->cy - combos[i]->cy) < 6.f) ++j;
        if (j - i == 5) {
            ++row;
            for (size_t k = 0; k < 5; ++k) {
                std::wstring h = LookupStr(keys[k]);
                combos[i + k]->column = (h.empty() ? fallback[k] : h) + L" " + std::to_wstring(row);
            }
        }
        i = j;
    }
}

void CollectGadgets(std::vector<Gadget>& out) {
    uint32_t wm = 0, head = 0;
    if (!SafeReadU32(game::kTheWindowManager, wm) || !wm || !SafeReadU32(wm + game::kWmWindowList, head)) return;
    int budget = 3000;
    Walk(head, 0, 0, 0, budget, out);
    NameRules(out);
    NameMatrix(out);
}
