#pragma once

// Battle reports of enemies, for a player who cannot see who comes (player's idea, 2026-11-08).
// Roles come from KindOf in the mod's object INI (horde templates carry them: MordorPikemanHorde
// PIKE, GondorArcherHorde ARCHER, RohanRohirrimHorde CAVALRY), in this order: HERO, SIEGEENGINE,
// MONSTER (BIG_MONSTER, TROLL, HORDE_MONSTER), CAVALRY with ARCHER (horse archers), CAVALRY,
// PIKE, ARCHER, INFANTRY. Only enemies in the clear of the shroud are told of.
//
// Automatic report: enemies that come within 70 m of the player's own objects are spoken once,
// the nearest group (within 40 m of each other) with the own object they near, distance and
// direction, then the count of the others. Again only after they went farther than 105 m.
// Keys (battle, outside the command panel mode and menus over the battle):
//   ,  (comma, the Russian Б)  summary: enemies seen within 150 m, by role, nearest of each
//   Ctrl+,                     next role as the target, with how many and the nearest
//   Shift+,                    the selected units attack the nearest seen enemy of that role
//                              (no role chosen: the nearest seen enemy); cavalry on pikes asks
//                              for a second press
//   Ctrl+Shift+,               automatic reports on / off
void TacticsTick();  // every 100 ms
void TacticsKeys(bool gameFocused);
