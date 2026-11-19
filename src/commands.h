#pragma once

// Command panel mode (battle only). The buttons of the selected unit or building (its
// template's CommandSet in commandset.ini: abilities, training, upgrades, buildings) and of
// the player's spell book, read from the mod's INI, spoken with name, hotkey, cost, target
// and autocast. A button is pressed with its own hotkey (the letter after '&' in its label,
// sent as a key press); spell book buttons, which have none, with a click on the palantir's
// SpellBookUI button. An ability that needs a target offers the nearest fitting one.
// Keys:
//   '  (apostrophe, the Russian Э key)  mode on / off; while choosing a target: cancel; inside a
//                                menu button's list: back
//   PageDown / PageUp            next / previous button (while aiming: next / previous target)
//   Ctrl+PageDown / PageUp       the selected object's buttons / the spell book
//   Home                         repeat; Shift+Home: what the button does (its tooltip)
//   End                          press the button (while aiming: use it on the target). A menu
//                                button (a fortress's Heroes, Fortress Upgrades) opens its own
//                                list; its entries are pressed on the palantir's ring
//   Shift+End                    autocast on / off (a right click on the palantir ring button)
// The scanner keys are off while the mode is on. Arrows keep moving the camera: the game
// reads them itself.
void CommandsKeys(bool gameFocused);
bool CommandsActive();
