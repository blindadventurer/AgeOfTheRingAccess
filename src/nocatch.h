// Eternal stealth: while on, the map scripts that notice the player's heroes stay false.
#pragma once
#include <string>

void NoCatchInit(const std::wstring& modDir);
void NoCatchInstallHook();  // on the game thread: the evaluator cannot be mid-run
bool NoCatchOn();
void NoCatchToggle();  // speaks the result; the choice survives a restart
