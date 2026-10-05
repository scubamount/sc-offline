#pragma once
#include "common.h"

#include <string>

bool ResolveLoadoutApi(const Section& text, const Section& rdata);
void ProcessLoadout();

// Shared with the outfit menu: same loader, same file handling.
bool        LoadoutApiReady();
std::string LoadoutItem(const char* port, const char* name, const std::string& children = std::string());
void        EquipLoadoutXml(const std::string& xml, const char* tag);
