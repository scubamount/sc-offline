#pragma once
#include "common.h"

void ResolveMissionsApi(const Section& text, const Section& rdata);
void ProcessMissions();
bool StartMissionNearPlayer(const char* missionId, const char* name, float minM, float maxM);
bool HaveScript(const char* path);
