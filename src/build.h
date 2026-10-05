#pragma once
#include "common.h"

bool ResolveBuildApi(const Section& text, const Section& rdata);
void ProcessBuild();
bool PlaceNearPlayer(double ahead, double side, double lift, double pos[3], double rot[4]);
bool GroundRay(uintptr_t zone, const double from[3], const double to[3], double hit[3]);

// Squadron 42 tab: drop one buildable without entering build mode. It is recorded
// in the base, so Undo / Clear base in the build section remove it too.
void Menu_RequestPlace(int index, bool inFront, float aheadMetres);
