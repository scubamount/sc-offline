#pragma once
#include "common.h"

// Location teleporter: a list of places grouped by star system (data\locations.txt plus whatever the
// in-game scan finds), and named saved spots (data\bookmarks.txt).

enum TravelKind   { Place_Planet, Place_Moon, Place_Other, Place_Minor };   // Minor: interiors and small zones, hidden by default
struct TravelPlace    { char system[32]; char name[64]; char entity[96]; double radius; int kind; char body[8]; };
struct TravelBookmark { char system[32]; char name[64]; };

void ProcessTravel(DWORD now);   // game thread

// Menu side (any thread)
int  Travel_GetPlaces(TravelPlace* out, int max);
int  Travel_PlacesVersion();                       // changes whenever the place list does
int  Travel_GetBookmarks(TravelBookmark* out, int max);
void Travel_CurrentSystem(char* out, size_t n);
bool Travel_Scanning(float& progress);
void Travel_RequestPlace(const TravelPlace& place, float altitude);
void Travel_RequestBookmark(int index);
void Travel_RequestSaveBookmark(const char* name);
void Travel_RequestDeleteBookmark(int index);
void Travel_RequestScan();
