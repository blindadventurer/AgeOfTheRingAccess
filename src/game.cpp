#include <cstring>
#include "game.h"
#include "memory.h"

namespace game {

bool VersionGate() {
    // Shell::top(): mov eax,[ecx+4C]; test eax,eax; jne +1; ret; mov eax,[ecx+eax*4+8]; ret
    static const uint8_t kTopBytes[] = {0x8B, 0x41, 0x4C, 0x85, 0xC0, 0x75, 0x01, 0xC3,
                                        0x8B, 0x44, 0x81, 0x08, 0xC3};
    // validateBIL: mov eax,[gpPool]; mov eax,[eax+10]; dec eax
    static const uint8_t kBilBytes[] = {0xA1, 0x28, 0xD4, 0xDF, 0x00, 0x8B, 0x40, 0x10, 0x48};
    uint8_t buf[sizeof(kTopBytes)];
    if (!SafeRead(kShellTop, buf, sizeof(kTopBytes)) || memcmp(buf, kTopBytes, sizeof(kTopBytes)) != 0)
        return false;
    if (!SafeRead(kValidateBIL, buf, sizeof(kBilBytes)) || memcmp(buf, kBilBytes, sizeof(kBilBytes)) != 0)
        return false;
    return true;
}

int ReadButtonList(BilEntry* out, int maxCount) {
    uint32_t pool = 0, count = 0, array = 0;
    if (!SafeReadU32(kAptPool, pool) || !pool) return -1;
    if (!SafeReadU32(pool + kBilCount, count) || !SafeReadU32(pool + kBilArray, array)) return -1;
    if (count > 4096 || (count && !array)) return -1;
    int n = (int)count < maxCount ? (int)count : maxCount;
    for (int i = 0; i < n; ++i) {
        uint8_t raw[kBilStride];
        if (!SafeRead(array + i * kBilStride, raw, sizeof(raw))) return i;
        memcpy(&out[i].cih, raw, 4);
        memcpy(&out[i].a, raw + 4, 24);
    }
    return n;
}

bool ReadShell(uintptr_t& shell, int& count, uintptr_t stack[16]) {
    uint32_t s = 0, c = 0;
    if (!SafeReadU32(kTheShell, s) || s == 0) return false;
    if (!SafeReadU32(s + kShellCount, c) || c > 16) return false;
    shell = s;
    count = static_cast<int>(c);
    for (int i = 0; i < 16; ++i) {
        uint32_t p = 0;
        stack[i] = (i < count && SafeReadU32(s + kShellStack + i * 4, p)) ? p : 0;
    }
    return true;
}

std::string ReadAsciiString(uintptr_t stringDataPtr) {
    if (!stringDataPtr) return {};
    // Layout seen in memory (dump 2026-10-31): u16 refcount, u16 ?, u16 length, u16 alloc, chars.
    uint16_t len = 0;
    if (!SafeRead(stringDataPtr + 4, &len, 2) || len == 0 || len > 255) return {};
    char buf[256];
    if (!SafeRead(stringDataPtr + 8, buf, len)) return {};
    buf[len] = 0;
    return buf;
}

}  // namespace game
