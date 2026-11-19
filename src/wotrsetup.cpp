// Quick start of War of the Ring: a spoken wizard over the setup screen (see wotrsetup.h).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "gadgets.h"
#include "log.h"
#include "menu.h"
#include "speech.h"
#include "strings.h"
#include "wotrsetup.h"

namespace {

// One StartingRestriction block of a scenario: these armies start in these regions, in this team
// (0: the block allows several teams or names none).
struct Restriction {
    std::vector<std::string> factions, regions;
    int team = 0;
};
struct Scenario {
    std::string id, nameKey, descKey;
    int minPlayers = 2, maxPlayers = 6;
    bool historical = false;
    std::set<std::string> disabled;
    std::vector<Restriction> restrictions;
};
std::vector<Scenario> g_scenarios;

// The eleven armies (playertemplate.ini PlayerTemplate Faction*); lotr.str INI:<name> is how the
// setup screen's Army combo shows them (INI:FactionArnor "Rivendell", INI:FactionAngmar "Haradwaith").
const char* const kFactions[] = {"FactionMen",   "FactionRohan",    "FactionDwarves", "FactionElves",  "FactionArnor", "FactionMirkwood",
                                 "FactionMordor", "FactionIsengard", "FactionWild",    "FactionGuldur", "FactionAngmar"};

// The story campaigns want one army: their descriptions say "Choose Rivendell" (and the like); with
// another the player stands on the wrong side (Mordor in War in the North, log 2026-11-05 01:03).
// Same table as menu.cpp StoryArmy.
const char* const kStory[][2] = {{"LWScenario:WOTRScenarioBFME2Good", "FactionArnor"},
                                 {"LWScenario:WOTRScenarioBFME2Evil", "FactionMordor"},
                                 {"LWScenario:WOTRScenarioAngmarCampaign", "FactionGuldur"}};

std::string Trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string StripComment(std::string s) {
    size_t c = s.find(';');
    if (c != std::string::npos) s.resize(c);
    c = s.find("//");
    if (c != std::string::npos) s.resize(c);
    return Trim(s);
}

std::vector<std::string> Words(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream in(s);
    for (std::string w; in >> w;) out.push_back(w);
    return out;
}

void ParseScenario(const std::wstring& path) {
    std::ifstream f(path);
    if (!f) { LogLine(L"wotr setup: cannot read " + path); return; }
    Scenario s;
    Restriction r;
    bool inRestriction = false;
    for (std::string line; std::getline(f, line);) {
        line = StripComment(line);
        if (line.empty()) continue;
        std::string key = line, value;
        size_t eq = line.find('=');
        if (eq != std::string::npos) key = Trim(line.substr(0, eq)), value = Trim(line.substr(eq + 1));
        std::vector<std::string> head = Words(key);
        if (head.empty()) continue;
        if (eq == std::string::npos) {
            if (head[0] == "LivingWorldCampaign" && head.size() > 1) s.id = head[1];
            else if (head[0] == "StartingRestriction") { inRestriction = true; r = Restriction(); }
            else if (head[0] == "End" && inRestriction) { inRestriction = false; s.restrictions.push_back(r); }
            continue;
        }
        std::vector<std::string> vals = Words(value);
        if (inRestriction) {
            if (key == "Factions") r.factions = vals;
            else if (key == "Regions") r.regions = vals;
            else if (key == "Teams") r.team = vals.size() == 1 ? atoi(vals[0].c_str()) : 0;
            continue;
        }
        if (key == "DisplayName" && s.nameKey.empty()) s.nameKey = value;
        else if (key == "DisplayDescription" && s.descKey.empty()) s.descKey = value;
        else if (key == "MinPlayers") s.minPlayers = atoi(value.c_str());
        else if (key == "MaxPlayers") s.maxPlayers = atoi(value.c_str());
        else if (key == "HistoricalScenario") s.historical = _stricmp(value.c_str(), "Yes") == 0;
        else if (key == "DisabledFactions") s.disabled.insert(vals.begin(), vals.end());
    }
    if (!s.nameKey.empty()) g_scenarios.push_back(s);
}

std::wstring Low(std::wstring s) {
    for (wchar_t& c : s) c = towlower(c);
    return s;
}

// A combo's value as the game shows it, without the default's "*" ("1000*", "Fhaleen*").
std::wstring Value(std::wstring t) {
    while (!t.empty() && (t.back() == L'*' || t.back() == L' ')) t.pop_back();
    return t;
}

std::wstring Army(const std::string& faction) {
    std::wstring t = LookupStr("INI:" + faction);
    return t.empty() ? Widen(faction.c_str()) : t;
}

std::string FactionOf(const std::wstring& army) {
    for (const char* f : kFactions)
        if (Low(Army(f)) == Low(Value(army))) return f;
    return {};
}

std::wstring RegionName(const std::string& id) {
    std::string key = "LW:DisplayName";
    for (char c : id)
        if (c != '_') key += c;
    std::wstring t = LookupStr(key);
    if (!t.empty()) return t;
    std::wstring w = Widen(id.c_str());
    std::replace(w.begin(), w.end(), L'_', L' ');
    return w;
}

const Scenario* ScenarioNamed(const std::wstring& shown) {
    for (const Scenario& s : g_scenarios)
        if (Low(LookupStr(s.nameKey)) == Low(Value(shown))) return &s;
    return nullptr;
}

const char* StoryFaction(const Scenario* s) {
    if (!s) return nullptr;
    for (const auto& k : kStory)
        if (s->nameKey == k[0]) return k[1];
    return nullptr;
}

// ---- the wizard ----

enum Step { kScenario, kFaction, kCount, kOther, kDifficulty, kConfirm };
struct Row {
    std::wstring text, details;
    std::wstring value;  // what the setup combo gets (army, difficulty), or a word for the step
    int number = 0;      // count, team, restriction index
};
struct Other {
    std::wstring army;
    int team = 0;  // -1: no team (skirmish, every one for himself)
    bool ally = false;
    bool alone = false;  // skirmish: fights everyone
};

// Skirmish mode: step 1 is the map, from the map list menu.cpp read.
bool g_skirmish = false;
uintptr_t g_mapList = 0;
std::vector<SetupMap> g_maps;
int g_mapKind = 0, g_mapFilter = 0;  // shown kind (0 all) and players (0 any)
int g_mapRow = -1, g_mapPlayers = 0;
std::wstring g_mapArmy;
const wchar_t* const kMapKinds[] = {L"все карты", L"обычные", L"крепости", L"осады", L"другая фракция", L"приключения"};

bool g_open = false;
Step g_step = kScenario;
std::vector<Row> g_rows;
int g_pos = 0;
uintptr_t g_combo = 0, g_comboList = 0;
const Scenario* g_scen = nullptr;
std::wstring g_scenName;
std::wstring g_myArmy;
int g_myTeam = 0;
std::vector<Other> g_others;
int g_cur = 0;  // the other player being chosen
bool g_fixed = false;  // the scenario sets every army itself (historical, story)
std::wstring g_difficulty;
bool g_keyDown[256] = {};

bool Pressed(int vk) {
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool edge = down && !g_keyDown[vk];
    g_keyDown[vk] = down;
    return edge;
}

void SayRow(bool interrupt) {
    if (g_rows.empty()) { Speak(L"Пусто", interrupt); return; }
    std::wstring t = g_rows[g_pos].text;
    if (g_step == kOther) t += g_others[g_cur].alone ? L", сам за себя" : g_others[g_cur].ally ? L", союзник" : L", враг";
    Speak(t + L", " + std::to_wstring(g_pos + 1) + L" из " + std::to_wstring(g_rows.size()), interrupt);
}

// The setup screen's player matrix now: column name ("Army 2") -> gadget.
bool MatrixCombo(const std::wstring& column, Gadget& out) {
    std::vector<Gadget> all;
    CollectGadgets(all);
    for (const Gadget& g : all)
        if (g.kind == Gadget::Combo && g.column == column) { out = g; return true; }
    return false;
}

std::wstring Header(const char* key, const wchar_t* fallback) {
    std::wstring h = LookupStr(key);
    return h.empty() ? fallback : h;
}
std::wstring PlayerCol(int i) { return Header("GUI:Player", L"Player") + L" " + std::to_wstring(i); }
std::wstring ArmyCol(int i) { return Header("APT:HeaderFaction", L"Army") + L" " + std::to_wstring(i); }
std::wstring TeamCol(int i) { return Header("GUI:Team", L"Team") + L" " + std::to_wstring(i); }

// Army rows as the setup screen offers them now (Army 1), without the armies the scenario turns off.
std::vector<Row> ArmyRows() {
    std::vector<Row> rows;
    Gadget g;
    std::vector<std::wstring> texts;
    uintptr_t list = 0;
    if (MatrixCombo(ArmyCol(1), g)) ComboChoices(g.window, texts, list);
    if (texts.empty())
        for (const char* f : kFactions) texts.push_back(Army(f));
    for (const std::wstring& t : texts) {
        std::wstring v = Value(t);
        if (v.empty() || v == L"-") continue;
        std::string f = FactionOf(v);
        if (g_scen && !f.empty() && g_scen->disabled.count(f)) continue;
        Row r;
        r.value = v;
        r.text = Low(v) == Low(LookupStr("GUI:Random")) ? v + L", случайная армия" : v;
        rows.push_back(r);
    }
    return rows;
}

std::wstring DifficultyDefault() { std::wstring t = LookupStr("GUI:MediumAI"); return t.empty() ? L"Soldier" : t; }

std::wstring OthersWords() {
    std::wstring t;
    for (size_t i = 0; i < g_others.size(); ++i) {
        const Other& o = g_others[i];
        t += L" Игрок " + std::to_wstring(i + 2) + L": " + o.army + (o.alone ? L", сам за себя" : o.ally ? L", союзник" : L", враг");
        if (o.team > 0) t += L", команда " + std::to_wstring(o.team);
        t += L".";
    }
    return t;
}

std::wstring Summary() {
    std::wstring t = (g_skirmish ? L"Карта: " : L"Сценарий: ") + g_scenName + L". Вы: " + g_myArmy +
                     (g_myTeam ? L", команда " + std::to_wstring(g_myTeam) : L"") + L".";
    if (g_others.empty()) return t + L" Без компьютеров.";
    t += OthersWords();
    t += L" Компьютер: " + g_difficulty + L".";
    return t;
}

void Enter(Step step);

void StepIntro() {
    switch (g_step) {
        case kScenario:
            if (g_skirmish)
                Speak(L"Быстрый старт схватки. Шаг 1: карта. Вверх и вниз, буква: переход, влево и вправо: виды карт, цифра: число "
                      L"игроков, Shift+пробел: описание, Enter: выбрать, Backspace: выйти. " +
                          std::wstring(kMapKinds[g_mapKind]) + (g_mapFilter ? L" на " + std::to_wstring(g_mapFilter) + L" игроков" : L"") +
                          L", " + std::to_wstring(g_rows.size()) + L".",
                      true);
            else Speak(L"Быстрый старт Войны Кольца. Шаг 1: сценарий. Вверх и вниз, Shift+пробел: описание, Enter: выбрать, Backspace: выйти.", true);
            break;
        case kFaction: Speak(L"Шаг 2: ваша армия. " + std::wstring(g_fixed ? L"Остальные армии сценарий ставит сам." : L"") + L" Enter: выбрать, Backspace: назад.", true); break;
        case kCount: Speak(L"Шаг 3: сколько игроков-компьютеров. Enter: выбрать, Backspace: назад.", true); break;
        case kOther:
            Speak(L"Игрок " + std::to_wstring(g_cur + 2) + L" из " + std::to_wstring(g_others.size() + 1) + L": его армия. Влево и вправо: " +
                      (g_skirmish ? L"враг, союзник или сам за себя" : L"враг или союзник") + L". Enter: выбрать, Backspace: назад.",
                  true);
            break;
        case kDifficulty: Speak(L"Сложность компьютера. Enter: выбрать, Backspace: назад.", true); break;
        case kConfirm: Speak(L"Всё готово. " + Summary() + L" Enter: выбрать действие, Backspace: назад.", true); break;
    }
    SayRow(false);
}

// Skirmish step 1: the maps of the shown kind and player count, in name order.
void BuildMapRows() {
    g_rows.clear();
    int sel = ListSelection(g_mapList);
    g_pos = 0;
    for (size_t i = 0; i < g_maps.size(); ++i) {
        const SetupMap& m = g_maps[i];
        if ((g_mapKind && m.kind != g_mapKind) || (g_mapFilter && m.players != g_mapFilter)) continue;
        Row r;
        r.value = m.name;
        r.text = m.name;
        r.details = m.details;
        r.number = (int)i;
        if (m.row == (g_mapRow >= 0 ? g_mapRow : sel)) g_pos = (int)g_rows.size();
        g_rows.push_back(r);
    }
}

void BuildScenarioRows() {
    if (g_skirmish) { BuildMapRows(); return; }
    g_rows.clear();
    std::vector<std::wstring> texts;
    if (!ComboChoices(g_combo, texts, g_comboList)) return;
    for (size_t i = 0; i < texts.size(); ++i) {
        Row r;
        r.value = texts[i];
        r.number = (int)i;
        std::wstring sum = WotrScenarioSummary(texts[i]);
        r.text = texts[i] + (sum.empty() ? L"" : L", " + sum);
        if (const Scenario* s = ScenarioNamed(texts[i])) {
            std::wstring d = LookupStr(s->descKey);
            for (size_t p; (p = d.find(L"\\n")) != std::wstring::npos;) d.replace(p, 2, L" ");
            r.details = d;
        }
        g_rows.push_back(r);
    }
    int sel = ListSelection(g_comboList);
    g_pos = sel >= 0 && sel < (int)g_rows.size() ? sel : 0;
}

void BuildFactionRows() {
    g_rows.clear();
    g_fixed = false;
    const char* story = StoryFaction(g_scen);
    if (g_skirmish && !g_mapArmy.empty()) {
        // Alternate faction maps: the map's description names the army ("Replaced Faction: X").
        Row r;
        r.value = g_mapArmy;
        r.text = g_mapArmy + L", армия этой карты";
        g_rows.push_back(r);
    } else if (story) {
        g_fixed = true;
        Row r;
        r.value = Army(story);
        r.text = r.value + L", армия этой кампании";
        r.number = -1;
        g_rows.push_back(r);
    } else if (g_scen && g_scen->historical && !g_scen->restrictions.empty()) {
        g_fixed = true;
        for (size_t i = 0; i < g_scen->restrictions.size(); ++i) {
            const Restriction& x = g_scen->restrictions[i];
            if (x.factions.empty()) continue;
            Row r;
            r.value = Army(x.factions[0]);
            r.number = (int)i;
            r.text = r.value;
            if (x.team) r.text += L", команда " + std::to_wstring(x.team);
            if (!x.regions.empty()) r.text += L", старт: " + RegionName(x.regions[0]);
            g_rows.push_back(r);
        }
    } else {
        g_rows = ArmyRows();
    }
    g_pos = 0;
    for (size_t i = 0; i < g_rows.size(); ++i)
        if (Low(g_rows[i].value) == Low(g_myArmy)) g_pos = (int)i;
}

void BuildCountRows() {
    g_rows.clear();
    int lo = g_scen ? (std::max)(g_scen->minPlayers, 2) : 2, hi = g_scen ? g_scen->maxPlayers : 6;
    if (g_skirmish) {
        hi = g_mapPlayers > 0 ? (std::min)(g_mapPlayers, 8) : 8;
        lo = hi <= 1 ? 1 : 2;
    }
    if (hi < lo) hi = lo;
    for (int n = lo; n <= hi; ++n) {
        Row r;
        r.number = n - 1;
        r.text = (n - 1 == 1 ? std::wstring(L"1 компьютер") : std::to_wstring(n - 1) + L" компьютера") + L", всего игроков " + std::to_wstring(n);
        if (n - 1 >= 5) r.text = std::to_wstring(n - 1) + L" компьютеров, всего игроков " + std::to_wstring(n);
        if (n == 1) r.text = L"Без компьютеров, вы один";
        g_rows.push_back(r);
    }
    g_pos = 0;
    for (size_t i = 0; i < g_rows.size(); ++i)
        if (g_rows[i].number == (int)g_others.size()) g_pos = (int)i;
}

void BuildOtherRows() {
    g_rows = ArmyRows();
    g_pos = 0;
    for (size_t i = 0; i < g_rows.size(); ++i)
        if (Low(g_rows[i].value) == Low(g_others[g_cur].army)) g_pos = (int)i;
}

void BuildDifficultyRows() {
    g_rows.clear();
    for (const char* k : {"GUI:EasyAI", "GUI:MediumAI", "GUI:HardAI", "GUI:BrutalAI"}) {
        Row r;
        r.value = LookupStr(k);
        if (r.value.empty()) continue;
        r.text = r.value;
        if (std::string(k) == "GUI:EasyAI") r.text += L", лёгкий";
        else if (std::string(k) == "GUI:MediumAI") r.text += L", средний";
        else if (std::string(k) == "GUI:HardAI") r.text += L", трудный";
        else r.text += L", очень трудный";
        g_rows.push_back(r);
    }
    if (g_difficulty.empty()) g_difficulty = DifficultyDefault();
    g_pos = 0;
    for (size_t i = 0; i < g_rows.size(); ++i)
        if (g_rows[i].value == g_difficulty) g_pos = (int)i;
}

void BuildConfirmRows() {
    g_rows.clear();
    Row a, b, c;
    a.value = L"start", a.text = L"Расставить и начать игру";
    b.value = L"set", b.text = L"Только расставить, PLAY нажму сам";
    c.value = L"say", c.text = L"Прочитать итог ещё раз";
    g_rows = {a, b, c};
    g_pos = 0;
}

void Enter(Step step) {
    g_step = step;
    switch (step) {
        case kScenario: BuildScenarioRows(); break;
        case kFaction: BuildFactionRows(); break;
        case kCount: BuildCountRows(); break;
        case kOther: BuildOtherRows(); break;
        case kDifficulty: BuildDifficultyRows(); break;
        case kConfirm: BuildConfirmRows(); break;
    }
    StepIntro();
}

// Sets one combo of the player matrix to the row whose text is want (team: the row holding that
// number). Re-reads the screen each time: choosing a scenario or a slot refills the combos.
enum SetResult { kSet, kAlready, kNoCombo, kNoRow, kNotTaken };
SetResult SetCombo(const std::wstring& column, const std::wstring& want, int teamNumber = 0) {
    Gadget g;
    bool found = false;
    for (int tries = 0; tries < 3 && !(found = MatrixCombo(column, g)); ++tries) Sleep(200);
    if (!found) { LogLine(L"wotr setup: no combo " + column); return kNoCombo; }
    auto fits = [&](const std::wstring& text) {
        std::wstring v = Value(text);
        if (!teamNumber) return Low(v) == Low(want);
        std::wstring digits;
        for (wchar_t c : v)
            if (iswdigit(c)) digits += c;
        return !digits.empty() && _wtoi(digits.c_str()) == teamNumber;
    };
    if (fits(g.text)) return kAlready;
    std::vector<std::wstring> rows;
    uintptr_t list = 0;
    if (!ComboChoices(g.window, rows, list)) { LogLine(L"wotr setup: no rows in " + column); return kNoRow; }
    int row = -1;
    for (size_t i = 0; i < rows.size() && row < 0; ++i)
        if (fits(rows[i])) row = (int)i;
    if (row < 0) {
        std::wstring all;
        for (const std::wstring& r : rows) all += L" [" + r + L"]";
        LogLine(L"wotr setup: " + column + L" has no row " + want + L":" + all);
        return kNoRow;
    }
    SelectListRow(list, row);
    Sleep(300);
    Gadget now;
    bool took = MatrixCombo(column, now) && fits(now.text);
    LogLine(L"wotr setup: " + column + L" = " + want + (took ? L" set" : L" NOT taken, shows " + now.text));
    return took ? kSet : kNotTaken;
}

void Apply(bool start) {
    std::vector<std::wstring> failed;
    LogLine(L"wotr setup: apply " + Summary());
    Speak(L"Расставляю.", true);
    // 1. The scenario or map (the wizard chose it already, check it held).
    if (g_skirmish) {
        if (ListSelection(g_mapList) != g_mapRow) failed.push_back(L"карта");
    } else {
        std::vector<Gadget> all;
        CollectGadgets(all);
        bool scenarioOk = false;
        for (const Gadget& g : all)
            if (g.kind == Gadget::Combo && Low(Value(g.text)) == Low(g_scenName)) scenarioOk = true;
        if (!scenarioOk) failed.push_back(L"сценарий");
    }
    // 2. Who plays: player 1 is you; the next ones are computers, the rest closed.
    Gadget p1;
    if (MatrixCombo(PlayerCol(1), p1)) LogLine(L"wotr setup: player 1 is " + p1.text);
    std::wstring closed = LookupStr("GUI:Closed");
    if (closed.empty()) closed = L"Closed";
    for (int i = 2; i <= 8; ++i) {
        Gadget g;
        if (!MatrixCombo(PlayerCol(i), g)) break;
        bool used = i - 2 < (int)g_others.size();
        SetResult r = SetCombo(PlayerCol(i), used ? g_difficulty : closed);
        if (used && r != kSet && r != kAlready) failed.push_back(L"игрок " + std::to_wstring(i));
    }
    if ((int)g_others.size() > 0) {
        Gadget g;
        if (!MatrixCombo(PlayerCol((int)g_others.size() + 1), g)) failed.push_back(L"на экране меньше строк игроков, чем нужно");
    }
    // 3. Armies and teams.
    struct Want { std::wstring army; int team; };
    std::vector<Want> wants = {{g_myArmy, g_myTeam}};
    for (const Other& o : g_others) wants.push_back({o.army, o.team});
    for (size_t i = 0; i < wants.size(); ++i) {
        int slot = (int)i + 1;
        SetResult a = SetCombo(ArmyCol(slot), wants[i].army);
        if (a != kSet && a != kAlready) failed.push_back(L"армия игрока " + std::to_wstring(slot));
        if (wants[i].team) {
            // -1: no team, the row "-" (the screen shows "Team 2: -" for none, log 2026-11-05 18:48).
            SetResult t = wants[i].team < 0 ? SetCombo(TeamCol(slot), L"-") : SetCombo(TeamCol(slot), L"", wants[i].team);
            if (t != kSet && t != kAlready) failed.push_back(L"команда игрока " + std::to_wstring(slot));
        }
    }
    g_open = false;
    MenuHoldFocus(3000);
    if (!failed.empty()) {
        std::wstring t;
        for (const std::wstring& f : failed) t += (t.empty() ? L"" : L", ") + f;
        LogLine(L"wotr setup: not set: " + t);
        Speak(L"Не выставилось: " + t + L". Проверьте эти списки на экране" + (start ? L" и нажмите PLAY сами." : L"."), true);
        return;
    }
    if (!start) { Speak(L"Расставлено. " + Summary() + L" Кнопка PLAY начнёт игру.", true); return; }
    Speak(L"Расставлено. Начинаю игру.", true);
    Sleep(300);
    if (!MenuClickHud("StartGame", false)) Speak(L"Кнопка PLAY не найдена, нажмите её сами.", true);
}

void Choose() {
    if (g_rows.empty()) return;
    const Row r = g_rows[g_pos];
    switch (g_step) {
        case kScenario: {
            if (g_skirmish) {
                const SetupMap& m = g_maps[r.number];
                int sel = ListSelection(g_mapList);
                if (sel != m.row) {
                    SelectListRow(g_mapList, m.row);
                    Sleep(800);  // the setup screen refills its player rows for the new map
                    sel = ListSelection(g_mapList);
                    LogLine(L"skirmish setup: map row " + std::to_wstring(m.row) + L", selection now " + std::to_wstring(sel) + L": " + m.name);
                    if (sel != m.row) { Speak(L"Карта не выбралась: " + m.name, true); return; }
                }
                g_mapRow = m.row;
                g_mapPlayers = m.players;
                g_mapArmy = m.army;
                g_scenName = m.name;
                g_scen = nullptr;
                g_others.clear();
                g_myArmy.clear();
                g_myTeam = 0;
                Enter(kFaction);
                return;
            }
            int sel = ListSelection(g_comboList);
            if (sel != r.number) {
                SelectListRow(g_comboList, r.number);
                Sleep(800);  // the setup screen refills its combos for the new scenario
                sel = ListSelection(g_comboList);
                LogLine(L"wotr setup: scenario row " + std::to_wstring(r.number) + L", selection now " + std::to_wstring(sel) + L": " + r.value);
                if (sel != r.number) { Speak(L"Сценарий не выбрался: " + r.value, true); return; }
            }
            g_scen = ScenarioNamed(r.value);
            g_scenName = r.value;
            if (!g_scen) LogLine(L"wotr setup: no rules known for " + r.value);
            g_others.clear();
            g_myArmy.clear();
            g_myTeam = 0;
            Enter(kFaction);
            return;
        }
        case kFaction: {
            g_myArmy = r.value;
            g_others.clear();
            if (g_fixed) {
                const char* story = StoryFaction(g_scen);
                g_myTeam = r.number >= 0 && g_scen ? g_scen->restrictions[r.number].team : 0;
                for (size_t i = 0; g_scen && i < g_scen->restrictions.size(); ++i) {
                    const Restriction& x = g_scen->restrictions[i];
                    if (x.factions.empty() || (int)i == r.number || (story && x.factions[0] == story)) continue;
                    Other o;
                    o.army = Army(x.factions[0]);
                    o.team = x.team;
                    o.ally = x.team && x.team == g_myTeam;
                    g_others.push_back(o);
                }
                Enter(kDifficulty);
            } else {
                g_myTeam = 1;
                Enter(kCount);
            }
            return;
        }
        case kCount: {
            std::vector<Row> armies = ArmyRows();
            std::vector<Other> keep = g_others;
            g_others.clear();
            // New players get armies other than yours, in the list's order, one each.
            size_t next = 0;
            for (int i = 0; i < r.number; ++i) {
                Other o;
                if (i < (int)keep.size()) o = keep[i];
                else {
                    for (size_t k = 0; k < armies.size() && o.army.empty(); ++k) {
                        const Row& a = armies[(next + k) % armies.size()];
                        bool taken = Low(a.value) == Low(g_myArmy) || a.text != a.value;  // yours, or Random
                        for (const Other& x : g_others) taken |= Low(x.army) == Low(a.value);
                        if (!taken) { o.army = a.value; next = (next + k + 1) % armies.size(); }
                    }
                    if (o.army.empty() && !armies.empty()) o.army = armies[0].value;
                }
                g_others.push_back(o);
            }
            g_cur = 0;
            if (g_others.empty()) { Enter(kConfirm); return; }  // a one-player map
            Enter(kOther);
            return;
        }
        case kOther:
            g_others[g_cur].army = r.value;
            g_others[g_cur].team = g_others[g_cur].alone ? -1 : g_others[g_cur].ally ? 1 : 2;
            if (++g_cur < (int)g_others.size()) Enter(kOther);
            else Enter(kDifficulty);
            return;
        case kDifficulty:
            g_difficulty = r.value;
            // Open games: allies share your team 1, enemies team 2 (one alliance against you).
            if (!g_fixed)
                for (Other& o : g_others) o.team = o.alone ? -1 : o.ally ? 1 : 2;
            Enter(kConfirm);
            return;
        case kConfirm:
            if (r.value == L"say") { Speak(Summary(), true); return; }
            Apply(r.value == L"start");
            return;
    }
}

void Back() {
    switch (g_step) {
        case kScenario: g_open = false; Speak(L"Быстрый старт закрыт", true); return;
        case kFaction: Enter(kScenario); return;
        case kCount: Enter(kFaction); return;
        case kOther:
            if (g_cur > 0) { --g_cur; Enter(kOther); }
            else Enter(kCount);
            return;
        case kDifficulty:
            if (g_fixed) Enter(kFaction);
            else { g_cur = (std::max)(0, (int)g_others.size() - 1); Enter(g_others.empty() ? kCount : kOther); }
            return;
        case kConfirm: Enter(g_others.empty() && !g_fixed ? kCount : kDifficulty); return;
    }
}

}  // namespace

void WotrSetupInit(const std::wstring& modFolder) {
    std::wstring dir = modFolder + L"\\data\\ini\\campaigns\\";
    std::ifstream f(dir + L"riskcampaign.ini");
    for (std::string line; std::getline(f, line);) {
        std::string t = Trim(line);
        if (t.rfind("#include", 0) != 0) continue;  // ";#include" and "//#include" are switched off
        size_t a = t.find('"'), b = a == std::string::npos ? a : t.find('"', a + 1);
        if (b == std::string::npos) continue;
        std::string inc = t.substr(a + 1, b - a - 1);
        if (_strnicmp(inc.c_str(), "Scenarios\\", 10) != 0) continue;
        ParseScenario(dir + Widen(inc.c_str()));
    }
    std::wstring names;
    for (const Scenario& s : g_scenarios)
        names += L" " + Widen(s.id.c_str()) + L" (" + std::to_wstring(s.minPlayers) + L"-" + std::to_wstring(s.maxPlayers) + L", " +
                 std::to_wstring(s.restrictions.size()) + L" fixed)";
    LogLine(L"wotr setup: " + std::to_wstring(g_scenarios.size()) + L" scenarios:" + names);
}

std::wstring WotrScenarioSummary(const std::wstring& shownName) {
    const Scenario* s = ScenarioNamed(shownName);
    if (!s) return {};
    if (const char* story = StoryFaction(s)) return L"сюжетная кампания за " + Army(story);
    std::wstring players = s->minPlayers == s->maxPlayers ? L"ровно " + std::to_wstring(s->maxPlayers) + L" игроков"
                                                          : L"игроков от 2 до " + std::to_wstring(s->maxPlayers);
    if (!s->historical || s->restrictions.empty()) return players + L", армии любые";
    std::wstring t1, t2;
    for (const Restriction& r : s->restrictions) {
        if (r.factions.empty()) continue;
        std::wstring& side = r.team == 2 ? t2 : t1;
        side += (side.empty() ? L"" : L", ") + Army(r.factions[0]);
    }
    return players + L", исторический: " + t1 + (t2.empty() ? L"" : L" против " + t2);
}

void SkirmishSetupOpen(uintptr_t mapList, const std::vector<SetupMap>& maps) {
    g_skirmish = true;
    g_mapList = mapList;
    g_maps = maps;
    std::sort(g_maps.begin(), g_maps.end(), [](const SetupMap& a, const SetupMap& b) { return Low(a.name) < Low(b.name); });
    g_mapRow = -1;
    g_mapArmy.clear();
    g_scen = nullptr;
    g_fixed = false;
    g_open = true;
    for (bool& k : g_keyDown) k = true;  // the Enter that opened it must not choose at once
    LogLine(L"skirmish setup: opened, " + std::to_wstring(g_maps.size()) + L" maps");
    Enter(kScenario);
    if (g_maps.empty()) { g_open = false; Speak(L"Список карт не прочитан", true); }
}

void WotrSetupOpen(uintptr_t scenarioCombo) {
    g_skirmish = false;
    g_combo = scenarioCombo;
    g_open = true;
    for (bool& k : g_keyDown) k = true;  // the Enter that opened it must not choose at once
    Enter(kScenario);
    if (g_rows.empty()) { g_open = false; Speak(L"Список сценариев не прочитан", true); }
}

bool WotrSetupIsOpen() { return g_open; }

void WotrSetupClose() {
    if (g_open) LogLine(L"wotr setup: closed, the screen went away");
    g_open = false;
}

void WotrSetupKeys() {
    if (!g_open) return;
    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    if (Pressed(VK_BACK) || Pressed(VK_OEM_7)) { Back(); return; }
    int n = (int)g_rows.size();
    int move = Pressed(VK_DOWN) ? 1 : Pressed(VK_UP) ? -1 : 0;
    if (move && n) { g_pos = (g_pos + move + n) % n; SayRow(true); }
    if (Pressed(VK_HOME) && n) { g_pos = 0; SayRow(true); }
    if (Pressed(VK_END) && n) { g_pos = n - 1; SayRow(true); }
    bool left = Pressed(VK_LEFT), right = Pressed(VK_RIGHT);
    if ((left || right) && g_step == kOther) {
        Other& o = g_others[g_cur];
        // враг -> союзник (-> сам за себя in a skirmish) -> враг; Left goes the other way.
        int side = o.alone ? 2 : o.ally ? 1 : 0, sides = g_skirmish ? 3 : 2;
        side = (side + (right ? 1 : sides - 1)) % sides;
        o.ally = side == 1, o.alone = side == 2;
        Speak(o.alone ? L"сам за себя" : o.ally ? L"союзник" : L"враг", true);
    }
    if ((left || right) && g_step == kScenario && g_skirmish) {
        int count = (int)(sizeof kMapKinds / sizeof *kMapKinds);
        for (int step = 1; step <= count; ++step) {
            int k = ((g_mapKind + (right ? step : -step)) % count + count) % count;
            bool any = k == 0;
            for (const SetupMap& m : g_maps) any |= m.kind == k;
            if (!any) continue;
            g_mapKind = k;
            BuildMapRows();
            Speak(std::wstring(kMapKinds[k]) + L", " + std::to_wstring(g_rows.size()), true);
            SayRow(false);
            return;
        }
    }
    for (int d = 0; d <= 8 && g_step == kScenario && g_skirmish; ++d) {
        if (!Pressed('0' + d)) continue;
        g_mapFilter = d;
        BuildMapRows();
        Speak((d ? L"Карты на " + std::to_wstring(d) + L" игроков, " : std::wstring(L"Любое число игроков, ")) + std::to_wstring(g_rows.size()),
              true);
        SayRow(false);
        return;
    }
    n = (int)g_rows.size();
    for (int vk = 'A'; vk <= 'Z' && !ctrl; ++vk) {
        if (!Pressed(vk) || !n) continue;
        for (int step = 1; step <= n; ++step) {
            int i = (g_pos + step) % n;
            const std::wstring& t = g_rows[i].text;
            if (!t.empty() && (wchar_t)towupper(t[0]) == (wchar_t)vk) { g_pos = i; SayRow(true); return; }
        }
        Speak(L"Нет на эту букву", true);
        return;
    }
    if (Pressed(VK_SPACE) && n) {
        if (shift) Speak(g_rows[g_pos].details.empty() ? L"нет описания" : g_rows[g_pos].details, true);
        else SayRow(true);
    }
    if (Pressed(VK_RETURN) && n) Choose();
}
