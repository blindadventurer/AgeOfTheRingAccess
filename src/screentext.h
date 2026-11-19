#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>

// Text the game holds for the screen, read from TheDisplayStringManager [0xDE4518]: every
// DisplayString alive sits on its linked list (Zero Hour design; the head and next offsets are
// found in game, log line "screentext: list ..."). APT texts are expected there too
// (WorldBuilder names DAptDisplayString::SetText, AptAddDisplayString): HYPOTHESIS until the
// log shows a message box text. New texts are logged and kept; an open message box reads its
// new texts aloud.
// Keys (game focused): ; (the Russian Ж key) older text, Shift+; newer text.
void ScreenTextTick();
// Texts first seen at or after this GetTickCount moment, oldest first.
std::vector<std::wstring> ScreenTextSince(DWORD when);
// Texts alive right now that start with prefix (empty: all), each once.
std::vector<std::wstring> ScreenTextNow(const wchar_t* prefix);
void ScreenTextKeys(bool gameFocused);
