#pragma once
#include "common.h"

void ResolveCVarsApi(const Section& text, const Section& rdata);
void ProcessCVars();
bool RunConsoleNow(const char* cmd);
bool SetCVarNow(const char* name, float value);
