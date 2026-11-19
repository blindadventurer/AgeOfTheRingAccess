#pragma once
#include <string>

// Siege mode, run from the training menu: enemy hosts come on a timer, single-player practice. Each
// wave is announced 30 s ahead with who comes from where; it grows in units, in troop strength
// and from one host on one side to three on several; every fifth brings a hero. A wave
// beaten pays money. The best number of waves per difficulty stays in waves-record.txt.
void WavesInit(const std::wstring& modDir);
void WavesTick();  // every loop pass of the worker thread
bool WavesOn();
std::wstring WavesDifficultyName();
void WavesToggle();
void WavesNextDifficulty();
void WavesCallNow();  // the next wave at once; with the mode on it pays half as much again
void WavesSpeakStatus();
