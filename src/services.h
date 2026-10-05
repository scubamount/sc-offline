#pragma once
#include "common.h"

using HubFn = uintptr_t(__fastcall*)(uintptr_t service);

uint8_t* FindServicesObject(const Section& text);
uintptr_t StandInHub();
bool SwapHubSlot(HubFn hub, HubFn* real);

void ResolveHangarsApi(const Section& text, const Section& rdata);
