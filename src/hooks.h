#pragma once
#include "common.h"

extern bool g_hooksInstalled;

void InstallHooks(const Section& text);
void LogHooks();
bool HookFunction(uint8_t* target, size_t stolen, void* detour, void** original);
uint8_t* NearData(size_t n);
bool MakeCryString(void* out, const char* s);
void FreeCryString(void* s);
const char* RequestShipFromAtc(uint64_t atcEntity, uint64_t player, const char* shipClass);
