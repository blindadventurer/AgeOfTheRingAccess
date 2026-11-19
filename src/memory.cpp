// Guarded reads of game memory: a bad pointer returns false instead of crashing.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstring>
#include "memory.h"

// Regions VirtualQuery found readable lately (per thread), so most reads skip the query. One
// entry was not enough: a walk over game objects jumps between the image, the heap and other
// regions, and each jump cost a VirtualQuery (the command list's 1-1.5 s, log 2026-11-09).
// Entries live 500 ms, so a region freed meanwhile is soon queried again; __try covers the rest.
namespace {
struct OkRegion {
    uintptr_t base = 0, end = 0;
    DWORD stamp = 0;
};
constexpr int kOkRegions = 8;
thread_local OkRegion t_ok[kOkRegions];
thread_local int t_okNext = 0;
}  // namespace

static bool RangeReadable(uintptr_t address, size_t size) {
    DWORD now = GetTickCount();
    for (OkRegion& r : t_ok)
        if (address >= r.base && address + size <= r.end && now - r.stamp < 500) return true;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(mbi.Protect & 0xFE))
        return false;
    uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    if (address + size > end) return false;  // spans two regions: refuse rather than guess
    OkRegion& slot = t_ok[t_okNext];
    t_okNext = (t_okNext + 1) % kOkRegions;
    slot.base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    slot.end = end;
    slot.stamp = now;
    return true;
}

bool SafeRead(uintptr_t address, void* out, size_t size) {
    if (!RangeReadable(address, size)) return false;
    __try {
        memcpy(out, reinterpret_cast<const void*>(address), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeWriteI32(uintptr_t address, int32_t value) {
    if (!RangeReadable(address, sizeof(value))) return false;
    __try {
        *reinterpret_cast<volatile int32_t*>(address) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeReadU32(uintptr_t address, uint32_t& out) { return SafeRead(address, &out, sizeof(out)); }

bool SafeReadCString(uintptr_t address, char* out, size_t capacity) {
    if (capacity == 0) return false;
    for (size_t i = 0; i + 1 < capacity; ++i) {
        char c;
        if (!SafeRead(address + i, &c, 1)) { out[0] = 0; return false; }
        out[i] = c;
        if (c == 0) return true;
    }
    out[capacity - 1] = 0;
    return true;
}

bool IsReadable(uintptr_t address) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    // A guard page (a thread stack's growth page) must never be touched: the fault
    // would be swallowed by SafeRead and the stack could no longer grow.
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    DWORD p = mbi.Protect & 0xFF;
    return p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_EXECUTE_READ ||
           p == PAGE_EXECUTE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_WRITECOPY;
}
