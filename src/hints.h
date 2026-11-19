#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>

// On-screen mission texts: hooks the game's script action executor
// (ScriptActions::executeAction, 0x7CAFA5, thiscall, ScriptAction* on the stack) and
// speaks what the mission scripts show: military captions, notification boxes (hints,
// instructions, dialogue boxes with a portrait), countdown timer captions. Voiced
// dialogue lines (SOUND_PLAY_NAMED / SPEECH_PLAY with a "SCRIPT:<sound>" text) and
// dialogue boxes go to the history only, unless dialogue reading is on.
// Texts: lotr.str (strings.cpp) and maptext.txt (every map.str, tools\make_maptext.py).
// Keys (game focused):
//   [         older message      ]   newer message (at the newest: repeat it)
//   Ctrl+]    dialogue lines read automatically: on / off
void HintsInit(const std::wstring& modDir);
// Installs the hook; call on the game thread (RunOnGameThread), after the version gate.
void HintsInstallHook();
void HintsKeys(bool gameFocused);
// True once a notification, caption or timer with this text label (upper-case, e.g.
// "SCRIPT:RIVENDELLHINT002") was shown in this game session. HintsForget() clears them.
bool HintSeen(const std::string& upperLabel);
void HintsForget();
// GetTickCount of the last line of talk: any dialogue box, and only the Shire's ALEP_ boxes.
DWORD HintsLastTalk();
// Tags of the "new objective" boxes shown since the last call.
std::vector<std::string> HintsTakeObjectiveBoxes();
DWORD HintsLastShireTalk();
// The last `count` messages of the history, oldest first (for the player's test marks).
std::vector<std::wstring> HintsRecent(size_t count);
// Mission skip for testing: the next script action the game runs is preceded by QUICKVICTORY
// (the mission ends as won and the campaign goes on). Log lines "skip: ...".
void HintsRequestVictory();
