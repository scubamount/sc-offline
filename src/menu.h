#pragma once
#include <windows.h>

struct MenuShip { char name[64]; int size; float length; };

int             Menu_ShipCount();
const MenuShip* Menu_Ships();
void            Menu_RequestSpawn(int index, float heightAboveMe, bool sitInPilotSeat, bool flightReady);
void            Menu_RequestDaymar(int index);
void            Menu_GetStatus(char* out, size_t n);
void            Menu_SetNoclip(bool on, float speed);
void            Menu_SetNoclipSpeed(float speed);
void            Menu_SetGodMode(bool on);
void            Menu_SetInfiniteAmmo(bool on);

enum MenuGearSlot { Gear_Undersuit, Gear_Helmet, Gear_Torso, Gear_Arms, Gear_Legs, Gear_Backpack,
                    Gear_Primary, Gear_Sidearm, Gear_Ammo, Gear_Grenade, Gear_SlotCount };
int             Menu_GearCount(int slot);
const char*     Menu_GearName(int slot, int index);
void            Menu_RequestEquip(const int picks[Gear_SlotCount]);
void            Menu_RunConsole(const char* cmd);

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
void Menu_Start(HWND gameWindow);
