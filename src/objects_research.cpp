// Research written into the Ctrl+F12 dump. Read only.
//
// v4 (players, money, owner, selection) is done: its results live in scanner.cpp and
// NOTEBOOK.md "Scanner research". v5 looks for three things nobody has read yet:
//   - campaign mission objectives: TheMissionObjectiveTracker [0xDE8C94]; a map.ini
//     lists them as "MissionObjectiveTag = SCRIPT:LAObjective00" (lotr.str texts);
//   - radar events (what Space jumps to): TheRadar [0xDE4AB8];
//   - War of the Ring (Living World) state: TheLivingWorldManager [0xDE3C08],
//     TheLivingWorldLogic [0xDE4950], TheLivingWorldCampaignManager [0xDE87AC].
// Each block is printed in full with the texts behind its pointers (one and two
// levels deep), and compared with the same block of the previous dump.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "memory.h"
#include "objects_research.h"

namespace {

uint32_t U32(uintptr_t a) {
    uint32_t v = 0;
    SafeReadU32(a, v);
    return v;
}

bool Ptr(uint32_t v) { return v >= 0x10000 && v < 0x7FFF0000 && IsReadable(v); }

// Text behind a pointer, tried as AsciiString data (length u16 at +4, chars at +8),
// the same layout with UTF-16 chars, and a plain C string. Empty when none fits.
std::string TextAt(uint32_t p) {
    if (!Ptr(p)) return {};
    uint16_t len = 0;
    if (SafeRead(p + 4, &len, 2) && len >= 1 && len <= 120) {
        char a[121];
        if (SafeRead(p + 8, a, len)) {
            bool ok = true;
            for (int i = 0; i < len && ok; ++i) ok = a[i] >= 0x20 && a[i] < 0x7F;
            if (ok) return "a\"" + std::string(a, len) + "\"";
        }
        wchar_t w[121];
        if (SafeRead(p + 8, w, len * 2)) {
            std::string s;
            bool ok = true;
            for (int i = 0; i < len && ok; ++i) {
                ok = w[i] >= 0x20 && w[i] < 0x2E80;
                s += w[i] < 0x80 ? (char)w[i] : '?';
            }
            if (ok) return "u\"" + s + "\"";
        }
    }
    char c[65] = {};
    if (SafeRead(p, c, 64)) {
        int n = 0;
        while (n < 64 && c[n] >= 0x20 && c[n] < 0x7F) ++n;
        if (n >= 4 && (n == 64 || c[n] == 0)) return "c\"" + std::string(c, n) + "\"";
    }
    return {};
}

std::map<std::string, std::vector<uint32_t>> g_prev;  // previous dump's blocks, by key

// Prints a block (dwords, then floats that look like map coordinates, then texts
// behind pointers one and two levels deep) and every dword changed since the
// previous dump under the same key.
void Block(FILE* f, const std::string& key, const char* title, uint32_t at, int bytes) {
    std::vector<uint32_t> w(bytes / 4);
    for (size_t i = 0; i < w.size(); ++i) w[i] = U32(at + i * 4);
    fprintf(f, "\n  %s %08X:", title, at);
    for (size_t i = 0; i < w.size(); ++i) {
        if (i % 8 == 0) fprintf(f, "\n    +%04X", (unsigned)i * 4);
        fprintf(f, " %08X", w[i]);
    }
    fprintf(f, "\n  floats in -10000..10000 (not small ints):");
    for (size_t i = 0; i < w.size(); ++i) {
        float v;
        memcpy(&v, &w[i], 4);
        if (w[i] > 0x10000 && v == v && v > -10000.f && v < 10000.f && (v > 0.01f || v < -0.01f))
            fprintf(f, " +%X=%.1f", (unsigned)i * 4, v);
    }
    fprintf(f, "\n");
    for (size_t i = 0; i < w.size(); ++i) {
        std::string t = TextAt(w[i]);
        if (!t.empty()) { fprintf(f, "    +%04X -> %s\n", (unsigned)i * 4, t.c_str()); continue; }
        if (!Ptr(w[i])) continue;
        // one level deeper: texts inside the pointed block
        std::string inner;
        for (int y = 0; y < 0x80; y += 4) {
            std::string t2 = TextAt(U32(w[i] + y));
            if (!t2.empty()) inner += " +" + std::to_string(y) + ":" + t2;
        }
        if (!inner.empty()) fprintf(f, "    +%04X -> %08X {%s }\n", (unsigned)i * 4, w[i], inner.c_str());
    }
    auto prev = g_prev.find(key);
    if (prev != g_prev.end() && prev->second.size() == w.size()) {
        int changed = 0;
        for (size_t i = 0; i < w.size(); ++i) {
            uint32_t a = prev->second[i], b = w[i];
            if (a == b) continue;
            if (changed++ == 0) fprintf(f, "  changes in %s since previous dump:\n", title);
            if (changed > 120) continue;
            float fa, fb;
            memcpy(&fa, &a, 4);
            memcpy(&fb, &b, 4);
            fprintf(f, "    +%04X %08X -> %08X  int %d -> %d  float %g -> %g\n", (unsigned)i * 4, a, b, (int)a,
                    (int)b, fa, fb);
        }
        if (changed == 0) fprintf(f, "  no changes in %s since previous dump\n", title);
        else if (changed > 120) fprintf(f, "    ... %d changed dwords in all\n", changed);
    }
    g_prev[key] = std::move(w);
}

// Every path up to three pointers deep from a block to a text containing one of the
// words (for "SCRIPT:" objective tags inside the tracker), at most 60 hits.
void FindTexts(FILE* f, const char* title, uint32_t root, int bytes, const std::vector<const char*>& words) {
    fprintf(f, "\n  texts reachable from %s containing", title);
    for (const char* w : words) fprintf(f, " \"%s\"", w);
    fprintf(f, ":\n");
    std::set<uint32_t> seen;
    int hits = 0;
    struct Item { uint32_t addr; std::string path; int depth; int size; };
    std::vector<Item> queue = {{root, "", 0, bytes}};
    for (size_t q = 0; q < queue.size() && queue.size() < 6000 && hits < 60; ++q) {
        Item it = queue[q];
        for (int off = 0; off < it.size && hits < 60; off += 4) {
            uint32_t v = U32(it.addr + off);
            if (!Ptr(v)) continue;
            char step[16];
            sprintf_s(step, "+%X", off);
            std::string path = it.path + step;
            std::string t = TextAt(v);
            if (!t.empty()) {
                for (const char* w : words)
                    if (t.find(w) != std::string::npos) {
                        fprintf(f, "    %s -> %s\n", path.c_str(), t.c_str());
                        ++hits;
                        break;
                    }
                continue;
            }
            if (it.depth < 3 && seen.insert(v).second) queue.push_back({v, path + "->", it.depth + 1, 0x100});
        }
    }
    fprintf(f, "  (%d hits)\n", hits);
}

void Global(FILE* f, const char* name, uintptr_t global, int bytes) {
    uint32_t p = U32(global);
    fprintf(f, "\n%s = [%08IX] = %08X%s\n", name, global, p, Ptr(p) ? "" : " (not readable)");
    if (!Ptr(p)) return;
    Block(f, name, name, p, bytes);
}

// v7: War of the Ring runtime objects found by their vtable anywhere in the game's heap.
// Class names sit right after each vtable in game.dat (disassembly 2026-11-01):
// LivingWorldRegion 0x00C4D43C, LivingWorldArmy 0x00C22E9C, LivingWorldBuildPlot 0x00C7A7A8,
// LivingWorldRegionConnection 0x00C4D2DC, LivingWorldBattle 0x00C4D514 (its second base at
// 0x00C4D534 was seen in the region manager's vector at +0x14 on 2026-11-01 06:58).
void ResearchLivingWorldObjects(FILE* f) {
    struct Cls { const char* name; uint32_t vt; int bytes; int max; };
    const Cls classes[] = {{"Region", 0x00C4D43C, 0x180, 260}, {"Army", 0x00C22E9C, 0x180, 120},
                           {"BuildPlot", 0x00C7A7A8, 0x100, 120}, {"Connection", 0x00C4D2DC, 0x40, 20},
                           {"Battle", 0x00C4D514, 0x180, 10}};
    std::map<uint32_t, std::vector<uint32_t>> hits;
    DWORD t0 = GetTickCount();
    size_t scanned = 0;
    std::vector<uint32_t> buf;
    for (uintptr_t a = 0x10000; a < 0x7FFF0000;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mi, sizeof mi)) break;
        uintptr_t next = reinterpret_cast<uintptr_t>(mi.BaseAddress) + mi.RegionSize;
        bool rw = mi.State == MEM_COMMIT && mi.Type == MEM_PRIVATE && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                  (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE));
        if (rw) {
            for (uintptr_t p = reinterpret_cast<uintptr_t>(mi.BaseAddress); p < next; p += 0x10000) {
                size_t n = (std::min<uintptr_t>)(0x10000, next - p);
                buf.resize(n / 4);
                if (!SafeRead(p, buf.data(), n)) continue;
                scanned += n;
                for (size_t i = 0; i < buf.size(); ++i)
                    for (const Cls& c : classes)
                        if (buf[i] == c.vt && hits[c.vt].size() < (size_t)c.max) hits[c.vt].push_back((uint32_t)(p + i * 4));
            }
        }
        a = next;
    }
    fprintf(f, "\n\nRESEARCH v7: War of the Ring objects by vtable (%zu MB scanned in %lu ms)\n", scanned >> 20,
            GetTickCount() - t0);
    for (const Cls& c : classes) {
        const auto& h = hits[c.vt];
        fprintf(f, "\n%s (vtable %08X): %zu found\n", c.name, c.vt, h.size());
        // One line per object: the texts behind its first 0x40 dwords (ids, names, owners).
        for (size_t i = 0; i < h.size(); ++i) {
            std::string line;
            for (int off = 4; off < c.bytes && line.size() < 220; off += 4) {
                std::string t = TextAt(U32(h[i] + off));
                if (!t.empty()) line += " +" + std::to_string(off) + ":" + t;
            }
            fprintf(f, "  [%zu] %08X%s\n", i, h[i], line.c_str());
        }
        for (size_t i = 0; i < h.size() && i < 60; ++i) {
            char key[48], title[48];
            sprintf_s(key, "lw%s#%08X", c.name, h[i]);
            sprintf_s(title, "%s[%zu]", c.name, i);
            Block(f, key, title, h[i], c.bytes);
        }
    }
}

}  // namespace

void ResearchObjects(FILE* f) {
    fprintf(f, "\n\nRESEARCH v5: objectives, radar events, War of the Ring\n");
    Global(f, "TheMissionObjectiveTracker", 0xDE8C94, 0x200);
    if (uint32_t t = U32(0xDE8C94))
        if (Ptr(t)) FindTexts(f, "TheMissionObjectiveTracker", t, 0x200, {"SCRIPT:", "Objective", "OBJECTIVE"});
    Global(f, "TheRadar", 0xDE4AB8, 0x1400);
    Global(f, "TheLivingWorldManager", 0xDE3C08, 0x800);
    Global(f, "TheLivingWorldLogic", 0xDE4950, 0x800);
    Global(f, "TheLivingWorldCampaignManager", 0xDE87AC, 0x800);
    for (const auto& g : {std::make_pair("TheLivingWorldManager", 0xDE3C08u), std::make_pair("TheLivingWorldLogic", 0xDE4950u),
                          std::make_pair("TheLivingWorldCampaignManager", 0xDE87ACu)})
        if (uint32_t t = U32(g.second))
            if (Ptr(t)) FindTexts(f, g.first, t, 0x800, {"LW:", "Region", "MAP WOR", "Player", "Army", "Fornost", "Rohan"});

    // v6: the War of the Ring regions. LivingWorldLogic's constructor (0x6B9F80, vtable
    // 0x00C14574) news a 0x40-byte LivingWorldRegionManager (ctor 0x6101EF, vtables
    // 0x00BFA508 / 0x00BFA500) into +0xB0; its destructor (0x6108F3) deletes the pointers of
    // the vector at +0x34 with the region destructor 0x6105E5 (disassembly 2026-11-01).
    // The region INI fields (parse table at 0xC4C8A0) give offsets inside a region
    // template: +0x18 MapName, +0x38 DisplayName, +0x3C DisplayDescription, +0x58
    // StrategicValue, +0x60 ConnectsTo, +0x80 CenterPoint, +0x100 OwnedByPlayer,
    // +0x104 HeroArmySpot, +0x110 GarrisonArmySpot, +0x11C BuildingSpot, +0x128 CPLimit.
    // Whether the vector's entries are those templates or runtime wrappers is what this shows.
    ResearchLivingWorldObjects(f);
    fprintf(f, "\n\nRESEARCH v6: War of the Ring regions\n");
    uint32_t logic = U32(0xDE4950);
    uint32_t mgr = Ptr(logic) ? U32(logic + 0xB0) : 0;
    fprintf(f, "LivingWorldLogic %08X (vtable %08X), region manager [+0xB0] = %08X (vtable %08X)\n", logic,
            Ptr(logic) ? U32(logic) : 0, mgr, Ptr(mgr) ? U32(mgr) : 0);
    if (!Ptr(mgr)) return;
    Block(f, "RegionManager", "RegionManager", mgr, 0x40);
    // v8: the running scenario: the local player, every player, and the regions of the active
    // region campaign [manager+8] (hash map buckets +0x3C..+0x40, node {next, key, region}).
    uint32_t local = U32(logic + 0x98);
    if (Ptr(local)) Block(f, "lwLocalPlayer", "local player", local, 0x400);
    int pn = 0;
    for (uint32_t q = U32(logic + 0x8C), e = U32(logic + 0x90); q < e && pn < 8; q += 4, ++pn) {
        char key[32], title[32];
        sprintf_s(key, "lwPlayer#%d", pn);
        sprintf_s(title, "player[%d]", pn);
        if (Ptr(U32(q))) Block(f, key, title, U32(q), 0x200);
    }
    uint32_t camp = U32(mgr + 8);
    if (Ptr(camp)) {
        int rn = 0;
        for (uint32_t q = U32(camp + 0x3C), e = U32(camp + 0x40); q < e && rn < 24; q += 4)
            for (uint32_t n = U32(q), guard = 0; Ptr(n) && guard < 50 && rn < 24; n = U32(n), ++guard) {
                char key[32], title[32];
                sprintf_s(key, "lwActive#%d", rn);
                sprintf_s(title, "active region[%d]", rn);
                Block(f, key, title, U32(n + 8), 0x200);
                ++rn;
            }
    }
    for (uint32_t vecAt : {0x08u, 0x14u, 0x20u, 0x34u}) {
        uint32_t b = U32(mgr + vecAt), e = U32(mgr + vecAt + 4);
        if (!Ptr(b) || e < b || e - b > 4 * 400 || (e - b) % 4) continue;
        int n = (int)((e - b) / 4);
        fprintf(f, "\n  vector at manager+0x%X: %d entries\n", vecAt, n);
        for (int i = 0; i < n; ++i) {
            uint32_t r = U32(b + 4 * i);
            if (!Ptr(r)) continue;
            // One line per entry: the template fields read as AsciiStrings, then the block.
            std::string map = TextAt(U32(r + 0x18)), name = TextAt(U32(r + 0x38)), owner = TextAt(U32(r + 0x100));
            fprintf(f, "  [%d] %08X vt %08X map %s name %s owner %s\n", i, r, U32(r), map.c_str(), name.c_str(),
                    owner.c_str());
            char key[40], title[40];
            sprintf_s(key, "region%X#%d", vecAt, i);
            sprintf_s(title, "region[%d]", i);
            Block(f, key, title, r, 0x160);
        }
    }
}
