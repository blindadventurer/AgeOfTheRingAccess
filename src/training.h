#pragma once

// Training menu for learning the game, single player only. In battle Ctrl+Shift+F9 opens and
// closes it; PageDown / PageUp move, Home repeats, End carries the item out: money, command
// points, immortal own units, and the siege mode of waves.cpp. Ctrl+Shift+F8 tells how the siege
// stands. It stays closed for the whole launch once a LAN or online screen was seen, where
// changing the game would break the match for the others.
bool TrainingActive();
void TrainingKeys(bool gameFocused);  // every loop pass
