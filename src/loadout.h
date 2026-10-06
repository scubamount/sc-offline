#pragma once
#include "common.h"

#include <string>

bool ResolveLoadoutApi(const Section& text, const Section& rdata);
void ProcessLoadout();

// Shared with the outfit menu: same loader, same file handling.
bool        LoadoutApiReady();
std::string LoadoutItem(const char* port, const char* name, const std::string& children = std::string());
void        EquipLoadoutXml(const std::string& xml, const char* tag);

// The player's head subtree; null fields take the original's defaults (see HeadItem).
struct HeadParts {
    const char* head = nullptr;
    const char* eyes = nullptr;
    const char* teeth = nullptr;
    const char* eyebrow = nullptr;
    const char* hair = nullptr;
    const char* hairColor = nullptr;
    std::string accessories;
};
std::string HeadItem(const HeadParts& parts);
std::string MobiGlasItem();
