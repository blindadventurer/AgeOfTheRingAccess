#pragma once
#define WIN32_LEAN_AND_MEAN
#include <vector>
#include <windows.h>
#include <functional>
#include <memory>
#include <string>

// Runs mod code on the game's own thread. The game window's procedure is
// subclassed; a posted private message makes it run the queued jobs between two
// frames (SAGE pumps its messages on the thread that runs the game loop).
void GameThreadInit(HWND gameWindow);
void RunOnGameThread(std::function<void()> job);
// The same, and waits up to 2 s. What the job writes must outlive a timeout: pass shared_ptr.
void RunOnGameThreadAndWait(std::function<void()> job);

// Camera: TheTacticalView [0xDE447C], virtual lookAt(const Coord3D*) at vtable +0x54
// (Zero Hour View layout shifted by 8 bytes, as setWidth/getWidth/getHeight are;
// called with an object's position at 0x69E189 and 0x5DCBA8). Queued to the game thread.
// done, when given, is set once the call has run.
// unbound: lookAt skips the view's area limit (byte view+0x44, tested at 0x4859EC), which holds
// the camera in the north-east of the War of the Ring map (log 2026-11-04 23:40).
void CameraLookAt(float x, float y, float z, HANDLE done = nullptr, bool unbound = false);
// The point the camera looks at: view+0x0C/+0x10 (lookAt at 0x48CEA3 copies its
// argument there before recomputing the camera). 0, 0 when unreadable.
void CameraPosition(float& x, float& y);
// Camera height (zoom), the same call the mouse wheel ends in: the wheel loop at 0x83B21C calls
// TheTacticalView vt+0x134 (0x65E803, height * 0.96 - 1) once per notch, and that calls the
// setter vt+0x130 (0x48CBFE) with the height from vt+0x12C (view+0x40). The setter clamps to the
// limit object at view+0x24C8 (vt+0 lowest, vt+4 highest, also the default height: 0x48CDBD),
// and returns without a change while the camera runs a scripted move or is locked.
struct CameraHeight {
    bool ran = false;      // the job ran and found the functions
    float before = 0, after = 0, lowest = 0, highest = 0;
    bool fallback = false;  // the game's lowest read 0: the session's last good one was used
};
// closest: to the lowest height, else to the highest. Waits for the game thread.
void CameraToHeightLimit(bool closest, const std::shared_ptr<CameraHeight>& out);
// Selects an object the way the game's own "next unit" code does (0x69E13C): deselect all (UI
// vt+0x110), MSG_CREATE_SELECTED_GROUP (0x3E9) with bool 1 and the object id, selectDrawable
// (UI vt+0x108). No click: it reaches what stands behind a fortress. Queued to the game thread.
void SelectObjectDirect(uint32_t obj, HANDLE done, bool* ok);
// The same for several objects at once (put the selection back after the auto lever): one
// MSG_CREATE_SELECTED_GROUP with every id (Generals source: a bool "new group", then object ids;
// HYPOTHESIS for RotWK until a test), each drawable passed to selectDrawable. Queued, no wait.
void SelectObjectsDirect(const std::vector<uint32_t>& objs);
// War of the Ring: MSG_LW_SET_CONFLICT_RESOLUTION_METHOD (0x6A7, 0x9DF01E), what the battle
// prompt's buttons send: method 1 auto-resolve, 3 real time, 2 retreat (0x9E03D9, 0x9E042E,
// 0x9E0483; names in Worldbuilder), and the battle's id (battle +0x30).
void SendBattleMethod(int method, int battleId, HANDLE done, bool* ok);
// War of the Ring: MSG_LW_BUILD_BUILDING (0x6AA, 0x9FC126): region index, plot id, template id.
void SendBuild(int regionIndex, int plotId, int templateId, HANDLE done, bool* ok);
// War of the Ring: MSG_LW_QUEUE_UNIT (0x6AC, 0xA0C594, QueueUnitButton::OnLeftClicked): the
// building's id (building +0x18, found through 0x60EC2A) and the army's id (entry +0x4C).
void SendQueueUnit(int buildingId, int armyId, HANDLE done, bool* ok);
// What the same button asks first: the spawn nugget's canQueueUnit (vt+0x1C, thiscall, one int).
// can gets one byte per id. Queued to the game thread.
void AskCanQueue(uint32_t nugget, const int* ids, int count, char* can, HANDLE done, bool* ok);
// War of the Ring: MSG_LW_ARMY_MOVE_REQUEST (0x6A8) with the army's id, the goal region's index
// and a point in it, as the map's right click sends it (0x9E1588, read by the dispatcher through
// getArmyFromMessage; goal +0x14C, army id at army +0x20). Queued to the game thread.
void SendArmyMove(int armyId, int regionIndex, float x, float y, HANDLE done, bool* ok);
// War of the Ring: ends the local player's phase the way the game's own handler 0x97E41D does
// (disassembly 2026-11-15): TheLivingWorldLogic [0xDE4950]; if 0x441E4A(logic) (the mode is on)
// then 0x6B6BA1(logic), which refuses in phase 6 and later or when its check 0x6B6ACC says no,
// else sends MSG_LW_DONE_PHASE 0x6A6 with the turn +0xFC, the phase +0xF4 and the local player's
// id and marks logic +0x10A. Waits for the game thread. 1 sent, 0 refused by the game, -1 no logic,
// -2 the code differs (game update), -3 crash caught, -4 the mode is not on, -5 no answer.
int WotrEndPhase();
// Move order for the local player's selected units, as a right click on open ground gives it:
// a MSG_DO_MOVETO message with the point, appended to TheMessageStream [0xDE6398]. No mouse,
// so a cliff or rock drawn over the point cannot catch the click. done/ok as for CameraLookAt.
void MoveSelectedTo(float x, float y, float z, HANDLE done = nullptr, bool* ok = nullptr);
// The same with MSG_DO_ATTACKMOVETO (0x430): walk there and fight whatever comes in reach. The
// game's own attack-move (0x803E5E) sends it with the location alone, as the move.
void AttackMoveSelectedTo(float x, float y, float z, HANDLE done = nullptr, bool* ok = nullptr);
// The aimed ability the game waits a target for (a button pressed, the cursor in aim mode) is
// given to target (an object, or 0 for the place x, y, z) by the game's own
// issueSpecialPowerCommand 0x81E147, as a left click would, and the aim mode is ended.
// Result: 0x410/0x411/0x412 (message sent), 0 (the game refused the target), -1 (no ability
// waiting), -2 (an ability of a kind not handled: kind in *kind), -3 (crash caught).
int AimPendingAbility(uint32_t target, float x, float y, float z, int* kind);
// One object (not the selection) walks to a point: its AI's aiMoveToPosition (0x66C4CA), command
// source 0, as the game's own code calls it. Queued to the game thread; logs "order: object ... move to".
void MoveObjectTo(uint32_t obj, float x, float y, float z);
// The special power of a CommandButton, used by source (an object of the local player, selected or
// not) at a place: issueSpecialPowerCommand 0x81E147 with that source, which puts the source's id into
// the message (0x81E192, 0x81E215). Waits for the game thread. 0x410-0x412 sent, 0 refused,
// -1 no power on the button, -3 crash caught, -5 no answer.
int IssuePowerFrom(uint32_t button, uint32_t source, float x, float y, float z);
// Sends one object along a waypoint path of the map exactly, as the mission scripts' own
// NAMED_FOLLOW_WAYPOINTS_EXACT does, from the path's waypoint nearest to (x, y). The script
// paths go where ordinary orders do not (the dwarves' escape path in Council of Elrond).
void FollowWaypointPath(uint32_t obj, float x, float y, float z, const std::string& label, HANDLE done = nullptr,
                        bool* ok = nullptr);
// The selected units attack the object, friend or not, by its id: no mouse, so nothing can miss.
void ForceAttackObject(uint32_t obj, float x, float y, float z, HANDLE done = nullptr, bool* ok = nullptr);
// The selected units attack an enemy by its id, when the game says at least one of them can.
// False: not an enemy, or none can: the target gets no order.
bool AttackObject(uint32_t target, const std::vector<uint32_t>& selection);
// The selected units enter the object (a garrison, a hiding bush), as a right click on it orders:
// MSG_ENTER (0x42C) with object id 0 (the selection) and the container's id, the game's translator
// at 0x81D846. False when the message could not be made.
bool EnterObject(uint32_t container);
// Puts one object at a point, as the mission scripts' UNIT_TELEPORT_TO_WAYPOINT does.
void TeleportObject(uint32_t obj, float x, float y, float z, HANDLE done = nullptr);

// Spell store (disassembly 2026-11-08). The store object is [0xDE8AD4] (AptSpellStore::OnInitialized
// stores it there); its button N's CommandButton is at store +0x2A8 + (N-1)*8 (OnBttnSpell 0x8235A5
// reads the number after "Spell", minus 1), and the button's sciences are the vector at
// CommandButton +0xA4..+0xA8. A click on a spell only marks it (store +0x288); the marks are bought
// when Accept (OnBttnClose 0x82289A) closes the store, so a close by Enter loses them.
// Sciences of store button slot (1-based), read while the store is open. Empty when unreadable.
std::vector<int> SpellStoreSciences(int slot);
struct SciencePurchase {
    bool ran = false;     // the job ran and found the local player
    int science = -1;     // the one chosen (first not owned, prerequisites met, affordable)
    int cost = -1, points = -1;
    bool owned = false;   // every science of the button is owned already
    bool noPrereq = false, tooDear = false;
    bool sent = false;    // MSG_PURCHASE_SCIENCE appended
};
// Buys like the control bar's PURCHASE_SCIENCE button (0x94161D): for the local player
// [[0xDE4928]+0x10] it skips owned sciences (player 0x6AC207), unmet prerequisites (ScienceStore
// [0xDE3B20] 0x5FED05 with player+4) and costs (0x5FEC64) above the points at player +0x24, then
// sends MSG_PURCHASE_SCIENCE (0x414, entry 0x2A of the switch at 0x7117C4) with player +0x54 and the
// science. Waits for the game thread.
void PurchaseScience(const std::vector<int>& sciences, const std::shared_ptr<SciencePurchase>& out);
// The CommandButton of store button slot (1-based) while the store is open; 0 when unreadable. Its
// name AsciiString is at +0x10 (commands.cpp WindowButton).
uint32_t SpellStoreButton(int slot);
// Chooses the spell of store button slot as the store's own click does and presses Accept, so the
// store buys it as it closes (gamethread.cpp has the disassembly). Waits for the game thread.
struct StoreChoice {
    enum { kNone = 0, kChosen, kOwned, kPending, kNoPrereq, kTooDear, kClosing, kNoStore, kCrash };
    int result = kNone;
    int science = -1, cost = -1, points = -1;
    bool forced = false;  // Accept refused, the flag and the close done by the mod
};
void StoreChooseAndAccept(int slot, const std::shared_ptr<StoreChoice>& out);
// Does the local player own each science (1/0, -1 unread), and the points left (-1 unread). Waits.
std::vector<int> SciencesOwned(const std::vector<int>& sciences, int& points);
