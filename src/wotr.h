#pragma once

// War of the Ring map mode (strategic map only). Regions, owners, armies and players are read
// from the running game, so the lists always belong to the scenario being played.
// On the map the arrows stay with the game's camera and the menu keys rest:
//   PageDown / PageUp        next / previous entry of the list
//   Ctrl+PageDown / PageUp   next / previous list: my armies, my regions, border, foreign, free,
//                            all, other players' armies near me, their marches, battles (the
//                            battle phase opens that one by itself)
// Spoken by itself: a report when each orders phase begins (treasury and its change, regions won
// and lost, new armies, other heroes that moved or stand at your borders) and the marches of
// other players' heroes when the armies walk (phase 1).
//   Home                     repeat; Shift+Home the region in detail with its neighbours
//   Ctrl+Home                camera to the region
//   End                      on a hero army: choose where it goes (PageDown neighbours, End sends);
//                            on your region: its build plots, End on an empty one chooses a
//                            building; on a foreign region: which of your nearby heroes attacks;
//                            on a region in battle (battle phase): choose real time, auto-resolve
//                            or retreat (PageDown), End sends it to the game.
//                            Apostrophe leaves any of these choices
//   Ctrl+Shift+End           camera to the region and a left click (the game's own panel)
//   Shift+End                turn and how much is yours
//   Ctrl+End twice           end the turn (the game's own end-phase call, the button as fallback)
//   ' (apostrophe, the Russian Э key)  buttons of the map on / off (menu keys: up, down, Enter)
// A dialog over the map (battle prompt, pause menu) takes the menu keys by itself.
void WotrInit(const std::wstring& modFolder);
void WotrKeys(bool gameFocused);
bool WotrActive();
