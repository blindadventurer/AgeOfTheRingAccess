// Saves with a horde in an alternate formation: see savefix.h.
//
// A contain's melee behaviour is a class chosen by its formation's INI (HordeContain ctor 0x872A16:
// module data +0x260). IsengardPikemanHorde and 78 others say MeleeBehavior = Amoeba, their Porcupine
// says nothing, so the default HordeMeleeSwarm. A save in Porcupine holds Swarm data; a load builds
// the main formation and hands that data to Amoeba's xfer 0x990B81, which at version < 2 leaves after
// the version byte, the rest is read as the following fields and the load throws. Amoeba writes
// version 3 (saves checked), Swarm writes 1, so version 1 here means Swarm data. This hook reads it
// to its end by Swarm's own layout (xfer 0x98CAFF): name, bool, count, count * (Coord3D, bool).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>
#include "log.h"
#include "memory.h"
#include "savefix.h"

namespace {

// 0x990BAE: cmp byte ptr [ebp+9], 2; jb 0x990C4A. edi = Xfer, ebx = the formation object.
constexpr uintptr_t kSite = 0x990BAE;
constexpr uint8_t kBytes[10] = {0x80, 0x7D, 0x09, 0x02, 0x0F, 0x82, 0x92, 0x00, 0x00, 0x00};
constexpr uint32_t kXferRead = 0x98;     // thiscall (tag, data, size); no tags in a save
constexpr uint32_t kResize = 0x04;       // of the formation: thiscall (count), 0x990B5B
constexpr uint32_t kEntries = 0x08, kEntriesEnd = 0x0C, kEntrySize = 0x3C, kEntryReady = 0x39;
constexpr int kEntryBytes = 13, kMaxSoldiers = 1000, kMaxLogged = 30;

uintptr_t g_normal = kSite + 10, g_done = 0x990C4A;
int g_logged = 0;

using ReadFn = void(__fastcall*)(uint32_t self, uint32_t edx, uint32_t tag, void* data, int size);
using ResizeFn = void(__fastcall*)(uint32_t self, uint32_t edx, int count);

uint32_t Vtable(uint32_t object, uint32_t slot) {
    return *reinterpret_cast<uint32_t*>(*reinterpret_cast<uint32_t*>((uintptr_t)object) + slot);
}

void Read(uint32_t xfer, void* data, int size) {
    reinterpret_cast<ReadFn>((uintptr_t)Vtable(xfer, kXferRead))(xfer, 0, 0, data, size);
}

void Log(const wchar_t* text, int number) {
    if (g_logged >= kMaxLogged) return;
    ++g_logged;
    LogLine(std::wstring(L"savefix: ") + text + L" " + std::to_wstring(number));
}

// A failure here is left to the game's own check: the load then ends with its usual message.
void __cdecl SkipSwarm(uint32_t xfer, uint32_t formation) {
    uint8_t length = 0;
    Read(xfer, &length, 1);
    char name[256] = "";
    if (length) Read(xfer, name, length);
    if (strcmp(name, "HordeMeleeSwarm") != 0) {
        Log(L"version 1 without Swarm's name, length", length);
        return;
    }
    uint8_t flag = 0;
    int count = 0;
    Read(xfer, &flag, 1);
    Read(xfer, &count, 4);
    if (count < 0 || count > kMaxSoldiers) {
        Log(L"strange count", count);
        return;
    }
    for (int i = 0; i < count; ++i) {
        uint8_t entry[kEntryBytes];
        Read(xfer, entry, kEntryBytes);
    }
    // Each soldier needs his place in Amoeba's list. Ready as the game's own reading of old data does.
    reinterpret_cast<ResizeFn>((uintptr_t)Vtable(formation, kResize))(formation, 0, count);
    uint32_t it = 0, end = 0;
    SafeReadU32(formation + kEntries, it);
    SafeReadU32(formation + kEntriesEnd, end);
    for (; it && it < end; it += kEntrySize) *reinterpret_cast<uint8_t*>((uintptr_t)it + kEntryReady) = 1;
    Log(L"horde saved in another formation, soldiers", count);
}

__declspec(naked) void Stub() {
    __asm {
        cmp byte ptr [ebp + 9], 2
        jb old
        jmp dword ptr [g_normal]
    old:
        pushad
        push ebx
        push edi
        call SkipSwarm
        add esp, 8
        popad
        jmp dword ptr [g_done]
    }
}

}  // namespace

void SaveFixInstallHook() {
    uint8_t code[sizeof kBytes];
    if (!SafeRead(kSite, code, sizeof code) || memcmp(code, kBytes, sizeof code) != 0) {
        LogLine(L"savefix: bytes differ, no hook");
        return;
    }
    DWORD old = 0;
    auto at = reinterpret_cast<uint8_t*>(kSite);
    if (!VirtualProtect(at, sizeof code, PAGE_EXECUTE_READWRITE, &old)) {
        LogLine(L"savefix: VirtualProtect failed, no hook");
        return;
    }
    int32_t rel = (int32_t)((uintptr_t)&Stub - (kSite + 5));
    at[0] = 0xE9;
    memcpy(at + 1, &rel, 4);
    memset(at + 5, 0x90, sizeof code - 5);
    VirtualProtect(at, sizeof code, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, sizeof code);
    LogLine(L"savefix: hook installed");
}
