// Loads the game's own words. lotr.str is plain text:
//   KEY / optional // comments / "text" / END
// APT .const files hold the movie's string constants; a "$Key" right after an
// instance name (skipping "buttonName") is that instance's label key.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>
#include "log.h"
#include "strings.h"

namespace {

std::map<std::string, std::wstring> g_str;                        // upper-case key -> text
std::map<std::string, std::map<std::string, std::string>> g_const; // movie(lower) -> name -> $Key
std::map<std::string, std::string> g_constAny;                     // name -> $Key, first seen

std::string Upper(std::string s) {
    for (auto& c : s) c = (char)toupper((unsigned char)c);
    return s;
}
std::string Lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::wstring FromAnsi(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(1252, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(1252, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

bool ReadWhole(const std::wstring& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

void LoadStr(const std::wstring& path) {
    std::string data;
    if (!ReadWhole(path, data)) { LogLine(L"strings: cannot read " + path); return; }
    std::istringstream in(data);
    std::string line, key, text;
    int count = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t a = line.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        line = line.substr(a);
        if (line.rfind("//", 0) == 0) continue;
        // Some keys end in spaces ("SCRIPT:Objective_G01_01 ", read as raw labels on 2026-11-05).
        if (line[0] != '"') line.erase(line.find_last_not_of(" \t") + 1);
        if (key.empty()) { key = line; text.clear(); continue; }
        bool isEnd = Upper(line) == "END";
        if (isEnd || (line[0] != '"' && !text.empty())) {
            // END closes the entry; a new key line before END also closes it (some entries miss END).
            if (!g_str.count(Upper(key))) { g_str[Upper(key)] = FromAnsi(text); ++count; }
            key = isEnd ? std::string() : line;
            text.clear();
            continue;
        }
        if (line[0] == '"' && text.empty()) {
            size_t b = line.rfind('"');
            text = b > 0 ? line.substr(1, b - 1) : line.substr(1);
        }
    }
    LogLine(L"strings: " + std::to_wstring(count) + L" entries from " + path);
}

// Printable runs of 2+ chars, in file order: the .const string constants.
void ParseConst(const std::string& movie, const std::string& data) {
    std::vector<std::string> s;
    std::string cur;
    for (unsigned char c : data) {
        if (c >= 0x20 && c < 0x7F) cur += (char)c;
        else { if (cur.size() >= 2) s.push_back(cur); cur.clear(); }
    }
    auto& m = g_const[Lower(movie)];
    for (size_t i = 1; i < s.size(); ++i) {
        if (s[i][0] != '$' || s[i].size() < 2) continue;
        size_t j = i - 1;
        if (s[j] == "buttonName" || s[j] == "stringName") { if (j == 0) continue; --j; }
        const std::string& name = s[j];
        if (name[0] == '$' || name == "this" || name.find(' ') != std::string::npos) continue;
        if (!m.count(name)) m[name] = s[i];
        if (!g_constAny.count(name)) g_constAny[name] = s[i];
    }
}

std::string Stem(const std::string& name) {
    std::string n = name.substr(name.find_last_of("\\/") + 1);
    return n.substr(0, n.rfind('.'));
}

void ScanBig(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    char head[16];
    if (!f.read(head, 16) || (memcmp(head, "BIGF", 4) && memcmp(head, "BIG4", 4))) return;
    auto be = [](const char* p) {
        return (uint32_t)(uint8_t)p[0] << 24 | (uint32_t)(uint8_t)p[1] << 16 |
               (uint32_t)(uint8_t)p[2] << 8 | (uint8_t)p[3];
    };
    uint32_t count = be(head + 8);
    struct E { std::string name; uint32_t off, size; };
    std::vector<E> consts;
    for (uint32_t i = 0; i < count && i < 200000; ++i) {
        char rec[8];
        if (!f.read(rec, 8)) return;
        std::string name;
        char c;
        while (f.get(c) && c) name += c;
        if (Lower(name).size() > 6 && Lower(name).substr(name.size() - 6) == ".const")
            consts.push_back({name, be(rec), be(rec + 4)});
    }
    for (auto& e : consts) {
        if (e.size > 4 * 1024 * 1024) continue;
        std::string data(e.size, '\0');
        f.seekg(e.off);
        if (f.read(&data[0], e.size)) ParseConst(Stem(e.name), data);
    }
}

void ScanFolder(const std::wstring& folder) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((folder + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring n = fd.cFileName, path = folder + L"\\" + n;
        std::wstring ln = n;
        std::transform(ln.begin(), ln.end(), ln.begin(), ::towlower);
        if (ln.size() > 4 && ln.substr(ln.size() - 4) == L".big") {
            ScanBig(path);
        } else if (ln.size() > 6 && ln.substr(ln.size() - 6) == L".const") {
            std::string data;
            if (ReadWhole(path, data)) ParseConst(Stem(Utf8(n)), data);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

std::map<std::wstring, MapInfo> g_maps;  // lower-case display name -> map

// mapcache.ini escapes bytes as _XX (hex); the text fields are UTF-16LE that way.
std::string Unescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '_' && i + 2 < s.size() + 0 && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            out += (char)std::stoi(s.substr(i + 1, 2), nullptr, 16);
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::wstring Utf16Le(const std::string& bytes) {
    std::wstring w;
    for (size_t i = 0; i + 1 < bytes.size(); i += 2) w += (wchar_t)((uint8_t)bytes[i] | ((uint8_t)bytes[i + 1] << 8));
    return w;
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = towlower(c);
    return s;
}

void LoadMapCache(const std::wstring& path) {
    std::string data;
    if (!ReadWhole(path, data)) { LogLine(L"strings: no " + path); return; }
    std::istringstream in(data);
    std::string line, file;
    MapInfo cur;
    int count = 0, adventures = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t a = line.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        line = line.substr(a);
        if (line.rfind("MapCache ", 0) == 0) {
            file = Unescape(line.substr(9));
            cur = MapInfo();
            cur.adventure = Lower(Widen(file.c_str())).find(L"map sp ") != std::wstring::npos;
            continue;
        }
        size_t eq = line.find(" = ");
        std::string key = eq == std::string::npos ? line : line.substr(0, eq);
        std::string val = eq == std::string::npos ? "" : line.substr(eq + 3);
        if (key == "numPlayers") cur.players = atoi(val.c_str());
        else if (key == "extentMax") sscanf_s(val.c_str(), "X:%f Y:%f", &cur.width, &cur.height);
        else if (key.size() == 14 && key.rfind("Player_", 0) == 0 && key.compare(8, 6, "_Start") == 0) {
            size_t n = (size_t)(key[7] - '0');
            float x = 0, y = 0;
            if (n >= 1 && n <= 8 && sscanf_s(val.c_str(), "X:%f Y:%f", &x, &y) == 2) {
                if (cur.starts.size() < n) cur.starts.resize(n);
                cur.starts[n - 1] = {x, y};
            }
        } else if (key == "displayName" || key == "description") {
            std::wstring t = Utf16Le(Unescape(val));
            if (!t.empty() && t[0] == L'$') t = t.substr(1);
            std::wstring resolved = LookupStr(Utf8(t));
            if (key == "displayName") {
                cur.name = resolved.empty() ? t : resolved;
            } else {
                // lotr.str keeps "\n" as two characters; speak them as a pause.
                for (size_t p; (p = resolved.find(L"\\n")) != std::wstring::npos;) resolved.replace(p, 2, L". ");
                cur.description = resolved;
                // Alternate-faction maps end their description with "Replaced Faction: Erebor"
                // (lotr.str "Map:MAPMPalternatedurinsfolk/Desc", checked 2026-11-01).
                // Adventures say "Player Faction: Erebor" (Map:MAPSPbalinsexpedition/Desc).
                size_t r = resolved.find(L"Replaced Faction:");
                if (r == std::wstring::npos) r = resolved.find(L"Player Faction:");
                if (r != std::wstring::npos) {
                    std::wstring rest = resolved.substr(resolved.find(L':', r) + 1);
                    size_t a = rest.find_first_not_of(L" ");
                    rest = a == std::wstring::npos ? L"" : rest.substr(a);
                    size_t e = (std::min)(rest.find_first_of(L".\r\n"), rest.find(L"\\n"));
                    cur.replaced = rest.substr(0, e);
                    while (!cur.replaced.empty() && cur.replaced.back() == L' ') cur.replaced.pop_back();
                }
            }
        } else if (key == "END" && !cur.name.empty()) {
            // Map files 2026-11-05: on "Fortress:" maps Player_1 holds the keep; on "Besiege:" maps
            // PlyrCreeps holds it and every start is outside.
            if (cur.name.rfind(L"Fortress:", 0) == 0) cur.role = L"Позиция 1 в крепости, остальные её осаждают";
            else if (cur.name.rfind(L"Besiege:", 0) == 0) cur.role = L"Крепость держит компьютер, все игроки снаружи и нападают";
            // Adventure names ("Map:mapspgooderebor") are in neither lotr.str (checked
            // 2026-10-31), so the list may show something else: index the raw key and the
            // folder name too, until the log shows what the rows really say.
            g_maps[Lower(cur.name)] = cur;
            std::wstring folder = Widen(file.c_str());
            size_t s1 = folder.find(L'\\'), s2 = folder.find(L'\\', s1 + 1);
            if (s1 != std::wstring::npos && s2 != std::wstring::npos) {
                std::wstring dir = Lower(folder.substr(s1 + 1, s2 - s1 - 1));  // "map sp good erebor"
                g_maps[dir] = cur;
                if (dir.rfind(L"map ", 0) == 0) g_maps[dir.substr(4)] = cur;
            }
            ++count;
            adventures += cur.adventure;
        }
    }
    LogLine(L"strings: " + std::to_wstring(count) + L" maps (" + std::to_wstring(adventures) +
            L" adventures) from " + path);
}

}  // namespace

const MapInfo* FindMapInRow(const std::wstring& rowText) {
    size_t start = 0;
    while (start <= rowText.size()) {
        size_t end = rowText.find(L", ", start);
        std::wstring cell = rowText.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        auto it = g_maps.find(Lower(cell));
        if (it != g_maps.end()) return &it->second;
        if (end == std::wstring::npos) break;
        start = end + 2;
    }
    return nullptr;
}

void StringsLoad(const std::wstring& modFolder, const std::wstring& gameFolder) {
    LoadStr(modFolder + L"\\data\\lotr.str");
    ScanFolder(modFolder);
    ScanFolder(gameFolder);
    LoadMapCache(modFolder + L"\\maps\\mapcache.ini");
    LogLine(L"strings: label keys for " + std::to_wstring(g_const.size()) + L" movies, " +
            std::to_wstring(g_constAny.size()) + L" names");
}

std::wstring LookupStr(const std::string& key) {
    auto it = g_str.find(Upper(key));
    return it == g_str.end() ? std::wstring() : it->second;
}

std::wstring LookupAptName(const std::string& name) {
    static const char* kPrefixes[] = {"tooltips", "tooltip", "On"};
    static const char* kSuffixes[] = {"Nav", "Button", "Btn", "Bttn", "Tab"};
    static const std::map<std::string, std::string> kOverrides = {
        {"QuitMainMenu", "APT:Exit"}, {"MultiPlayNav", "APT:Multiplayer"}};
    auto ov = kOverrides.find(name);
    if (ov != kOverrides.end()) {
        std::wstring t = LookupStr(ov->second);
        if (!t.empty()) return t;
    }
    std::vector<std::string> cands = {name};
    for (const char* p : kPrefixes) {
        size_t n = strlen(p);
        if (name.size() > n + 1 && name.compare(0, n, p) == 0 && isupper((unsigned char)name[n]))
            cands.push_back(name.substr(n));
    }
    for (size_t i = 0, e = cands.size(); i < e; ++i)
        for (const char* s : kSuffixes) {
            size_t n = strlen(s);
            if (cands[i].size() > n + 1 && cands[i].compare(cands[i].size() - n, n, s) == 0)
                cands.push_back(cands[i].substr(0, cands[i].size() - n));
        }
    for (const auto& c : cands) {
        std::wstring t = LookupStr("APT:" + c);
        if (!t.empty()) return t;
    }
    // A unique key that extends the name by at most 3 letters ("MultiPlay" -> "Multiplayer").
    for (const auto& c : cands) {
        std::string k = Upper("APT:" + c);
        auto it = g_str.lower_bound(k);
        std::wstring found;
        int hits = 0;
        for (; it != g_str.end() && it->first.compare(0, k.size(), k) == 0; ++it)
            if (it->first.size() <= k.size() + 3) { found = it->second; ++hits; }
        if (hits == 1) return found;
    }
    return {};
}

std::string ConstKeyFor(const std::string& movie, const std::string& instanceName) {
    auto m = g_const.find(Lower(movie));
    if (m != g_const.end()) {
        auto it = m->second.find(instanceName);
        if (it != m->second.end()) return it->second;
    }
    auto it = g_constAny.find(instanceName);
    return it == g_constAny.end() ? std::string() : it->second;
}
