#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Zero Hour window gadgets shown inside APT screens (combo boxes, list boxes,
// check boxes, text entries, sliders). Read-only walk of TheWindowManager's tree.
struct Gadget {
    enum Kind { Combo, ListRow, CheckBox, Entry, Slider, Button } kind;
    uintptr_t window = 0;  // the gadget window (for a list row: the list box)
    int row = -1;          // list row index
    std::wstring text;     // what it shows
    float cx = 0, cy = 0;  // centre in client pixels
    bool inCombo = false;  // a row of an open drop-down
    bool visible = true;   // list row inside the list's view (else a click scrolls first)
    bool selected = false; // list row is the list's current selection
    int total = 0;         // list rows in use
    std::wstring rule;     // match rule this combo or check box sets ("Initial Resources"), else empty
    std::wstring column;   // player matrix: column header and row number ("Army 2"), else empty
};

// Visible gadgets of every window tree, in no particular order.
void CollectGadgets(std::vector<Gadget>& out);

// Scrolls a list so `row` is in view if needed, then gives the row's centre in client pixels.
bool PrepareListRowClick(uintptr_t listWin, int row, float& cx, float& cy);
// The list's selected row, or -1.
int ListSelection(uintptr_t listWin);
// Selects a row through the game's list code, on the game thread. False when it did not run.
bool SelectListRow(uintptr_t listWin, int row);
// Presses a push button window the way a click does, on the game thread. False when it did not run.
bool PressButtonWindow(uintptr_t win);
// What a window shows, or its tooltip.
std::wstring WindowText(uintptr_t win, bool tooltip);
// The rows of a combo box's drop-down, open or closed, and the drop-down's list window.
bool ComboChoices(uintptr_t combo, std::vector<std::wstring>& rows, uintptr_t& listWin);
// One log line of raw list state (scroll candidates, row bottoms, slider) around a row click.
std::wstring ListDiagnostics(uintptr_t listWin);
