#pragma once
#include <cstdint>
#include <string>

// Addresses in rotwk\game.dat 2.1.2614.37001 (RotWK 2.01), image base 0x400000.
// Each was found by disassembly; see NOTEBOOK.md "Menus".
namespace game {

constexpr uintptr_t kTheShell = 0xDE7890;      // Shell* global
constexpr uintptr_t kShellTop = 0x75D9EB;      // Shell::top(), thiscall
constexpr uint32_t kShellCount = 0x4C;         // int screen count
constexpr uint32_t kShellStack = 0x0C;         // screen pointers, 16 slots

// Apt animation pool (gpPool). Its button instance list (BIL) holds every button
// drawn this frame: count at +0x10, array at +0x14, 28-byte entries
// {AptCIH* button, float matrix a,b,c,d,tx,ty}. Found in validateBIL at 0xB0E220.
constexpr uintptr_t kAptPool = 0xDFD428;
constexpr uintptr_t kValidateBIL = 0xB0E220;
constexpr uint32_t kBilCount = 0x10;
constexpr uint32_t kBilArray = 0x14;
constexpr uint32_t kBilStride = 0x1C;
constexpr int kAptTypeButton = 0xE;  // AptCIH type = [cih+4] >> 25 (getter at 0xAEFD00)

struct BilEntry {
    uintptr_t cih;
    float a, b, c, d, tx, ty;
};

// Copies up to maxCount entries of the button list; returns how many were read, -1 on failure.
int ReadButtonList(BilEntry* out, int maxCount);

// Zero Hour window system (combo boxes, list boxes, sliders inside APT screens).
// TheWindowManager: the global most used for vtable calls +0xE8 (winSendSystemMsg,
// 32 sites) and +0x7C (winCreateFromScript, e.g. 0x6C35F0 with "Menus/MessageBox.wnd").
// GameWindow offsets follow Open-BFME-2's BFME2 1.06 notes: HYPOTHESIS until a dump confirms.
constexpr uintptr_t kTheWindowManager = 0xDE495C;
constexpr uint32_t kWmWindowList = 0x0C;
constexpr uint32_t kWinStatus = 0x08, kWinRegion = 0x14, kWinStyle = 0x3C;
constexpr uint32_t kWinText = 0x1CC, kWinTooltip = 0x1D0;
constexpr uint32_t kWinNext = 0x1F8, kWinParent = 0x200, kWinChild = 0x204;

// True when the bytes at the known functions match this build. Every reader
// checks it first and stays off on another build.
bool VersionGate();

// Screen stack, read with guarded reads. Returns false when the shell is absent.
bool ReadShell(uintptr_t& shell, int& count, uintptr_t stack[16]);

// Text of an AsciiString (pointer to {u16 refcount, u16 ?, u16 length, u16 alloc, chars...}).
std::string ReadAsciiString(uintptr_t stringDataPtr);

}  // namespace game
