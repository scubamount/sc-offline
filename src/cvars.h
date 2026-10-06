#pragma once
#include "common.h"

void ResolveCVarsApi(const Section& text, const Section& rdata);
void ProcessCVars();
bool SetCVarNow(const char* name, float value);
bool GetCVarNow(const char* name, float& value);

// Squadron 42 tab settings: four game cvars behind one checkbox each.
int         Menu_S42SettingCount();
const char* Menu_S42SettingLabel(int index);
const char* Menu_S42SettingTip(int index);
bool        Menu_S42SettingOn(int index);      // cached; refreshed on the game thread
bool        Menu_S42SettingKnown(int index);   // false until the game thread has read the cvar
void        Menu_RequestS42Setting(int index, bool on);
