#pragma once
#include <cstdint>
#include <string>

void ScreensInit(const std::wstring& dumpDir);
// Called about every 100 ms from the mod thread: logs the screen stack when it changes.
void ScreensTick();
// Top screen object and its file name ("MainMenu.apt"); false when the shell is absent.
bool TopScreen(uintptr_t& screen, std::string& name, int& count);
// True when a screen with this file name ("Skirmish.apt") is anywhere on the shell stack.
bool ShellHasScreen(const char* file);
// Writes the shell and its screens' raw memory to dump-HHMMSS.txt (research key).
void ScreensDump();
