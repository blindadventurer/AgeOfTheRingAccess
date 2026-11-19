// A saved game with a horde in an alternate formation (Porcupine and kin) cannot be loaded by the
// game: its formation class differs from the main one. This hook lets the load read past that data.
#pragma once

void SaveFixInstallHook();  // on the game thread: no load can be mid-run
