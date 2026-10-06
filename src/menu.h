#pragma once
#include <windows.h>

struct MenuShip { char name[64]; int size; float length; };

int             Menu_ShipCount();
const MenuShip* Menu_Ships();
// How the spawner seats you after a ship spawns.
enum MenuSeatMode { SeatMode_None, SeatMode_Pilot, SeatMode_Named, SeatMode_PickLater };
struct MenuSpawnOptions {
    float height      = 20.0f;
    int   seatMode    = SeatMode_Pilot;
    char  seatName[48] = "";   // SeatMode_Named: words matched against the seat's entity name (e.g. "copilot", "turret left")
    bool  replaceNpc  = true;  // kick an NPC out of the seat you want instead of taking a different seat
    bool  flightReady = true;  // power the ship on once you're in a pilot seat
};
void            Menu_RequestSpawn(int index, const MenuSpawnOptions& options);

// Crew & seats panel. It works on the "target ship": the ship you spawned last, or the one you're in.
enum MenuSeatState { SeatState_Empty, SeatState_You, SeatState_Npc, SeatState_Taken };
struct MenuSeat { char name[64]; unsigned priority; int state; unsigned long long id; };
bool            Menu_SeatControlAvailable();
int             Menu_GetSeats(MenuSeat* out, int max, char* shipName, size_t shipNameLen); // -1 = no target ship
void            Menu_TargetShipImIn();
void            Menu_RequestSit(unsigned long long seatId, bool replaceNpc);
void            Menu_RequestKick(unsigned long long seatId);       // removes the NPC
void            Menu_RequestStandUp(unsigned long long seatId);    // you or the NPC get out of the seat
void            Menu_RequestStandAll();
void            Menu_RequestAddCrew(unsigned long long seatId, int npcIndex);
void            Menu_RequestFillCrew(int npcIndex);
void            Menu_RequestClearCrew();
void            Menu_RequestFlightReady();
void            Menu_GetStatus(char* out, size_t n);
void            SetMenuStatus(const char* fmt, ...);   // also logged; shown in the menu's status strip
void            Menu_SetNoclip(bool on, float speed);
void            Menu_SetNoclipSpeed(float speed);
void            Menu_SetGodMode(bool on);
void            Menu_SetInfiniteAmmo(bool on);
void            Menu_SetInfiniteShipAmmo(bool on);

enum MenuGearSlot { Gear_Undersuit, Gear_Helmet, Gear_Torso, Gear_Arms, Gear_Legs, Gear_Backpack,
                    Gear_Primary, Gear_Sidearm, Gear_Ammo, Gear_Grenade, Gear_SlotCount };
int             Menu_GearCount(int slot);
const char*     Menu_GearName(int slot, int index);
void            Menu_RequestEquip(const int picks[Gear_SlotCount]);
void            Menu_RunConsole(const char* cmd);
bool            Menu_ConsoleReady();   // the game's console was found

int             Menu_NpcCount();
const char*     Menu_NpcName(int index);
void            Menu_RequestNpc(int index, int count);
void            Menu_RequestClearNpcs();

int             Menu_BuildCount();
const char*     Menu_BuildName(int index);
const char*     Menu_BuildCategory(int index);
int             Menu_BuildCategoryOf(int index);
int             Menu_BuildCategoryCount();
const char*     Menu_BuildCategoryName(int category);
void            Menu_SetBuild(int index, float reach);
float           Menu_BuildReach();
void            Menu_ToggleBuildMode();
bool            Menu_BuildModeActive();
void            Menu_BuildUndo();
void            Menu_BuildClear();
int             Menu_BuildPlacedCount();

int             Menu_OutfitCount();
const char*     Menu_OutfitName(int index);
void            Menu_RequestWearOutfit(int index);
void            Menu_SetS42VisorHud(bool on);

void Menu_Start(HWND gameWindow);
