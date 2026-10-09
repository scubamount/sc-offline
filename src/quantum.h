#pragma once
#include "common.h"

void ResolveQuantumApi(const Section& text, const Section& rdata);
void LogQuantum();
bool QuantumDriveReady();   // the Gladius' new drive is patched in (LogQuantum's first line)
bool QuantumBoostReady();   // quantum boost works (LogQuantum's second line)
void ProcessQuantum();
