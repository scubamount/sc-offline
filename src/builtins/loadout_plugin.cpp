// loadout: the gear menu and the Squadron 42 outfits as a built-in plugin.
//
// loadout.equip and loadout.wear queue the same requests as the Player tab's Equip gear and the
// Squadron 42 tab's Wear SQ42 outfit, gated on the "loadout" capability (the game's loadout loader
// found). The built-in's tick subscription runs ProcessLoadout and ProcessOutfits, which read
// items.txt and outfits.txt once you're in the universe and equip on the game thread. The
// mechanics stay in loadout.cpp and outfits.cpp.
#include "builtins.h"
#include "tabs.h"
#include "../loadout.h"
#include "../menu.h"
#include "../outfits.h"
#include "../teleport.h"
#include <cstdio>

namespace {

bool g_ticking = false;   // the tick subscription owns ProcessLoadout and ProcessOutfits

const sco_plugin_info kInfo = {
    sizeof(sco_plugin_info), SCO_API_MAJOR, SCO_API_MINOR, "loadout", SCO_VERSION, "sc-offline",
};

constexpr const char* kCap = "loadout";

// Item names are entity classes (no spaces): split on spaces and commas.
sco_result Equip(const sco_arg* args, uint32_t, void*, char* reply, uint32_t size) {
    const char* items = args[0].v.s;
    if (!items || !*items || strlen(items) > 1023) {
        snprintf(reply, size, "Name the items from items.txt, separated by spaces or commas");
        return SCO_BAD_ARG;
    }
    if (Menu_GearCount(0) < 0) {   // asking starts the read of items.txt
        snprintf(reply, size, "The gear list loads once you're in the universe; try again in a moment");
        return SCO_FAILED;
    }
    int picks[Gear_SlotCount];
    for (int& p : picks) p = -1;   // the menu's None / Default / Matches weapon
    char list[1024];
    strncpy_s(list, items, _TRUNCATE);
    int count = 0;
    char* next = nullptr;
    for (char* item = strtok_s(list, " ,", &next); item; item = strtok_s(nullptr, " ,", &next)) {
        int slot = -1, index = -1;
        for (int s = 0; s < Gear_SlotCount && index < 0; ++s) {
            const int n = Menu_GearCount(s);
            for (int i = 0; i < n; ++i)
                if (_stricmp(Menu_GearName(s, i), item) == 0) { slot = s; index = i; break; }
        }
        if (index < 0) {
            snprintf(reply, size, "'%s' isn't in the gear list (items.txt)", item);
            return SCO_FAILED;
        }
        if (picks[slot] >= 0) {
            snprintf(reply, size, "'%s' and '%s' go in the same slot; name one", Menu_GearName(slot, picks[slot]), item);
            return SCO_BAD_ARG;
        }
        picks[slot] = index;
        ++count;
    }
    if (!count) {
        snprintf(reply, size, "Name the items from items.txt, separated by spaces or commas");
        return SCO_BAD_ARG;
    }
    Menu_RequestEquip(picks);
    snprintf(reply, size, "Equipping %d item%s; the rest of the slots are empty, as in the gear menu", count, count == 1 ? "" : "s");
    return SCO_OK;
}

sco_result Wear(const sco_arg* args, uint32_t, void*, char* reply, uint32_t size) {
    const char* outfit = args[0].v.s;
    if (!outfit || !*outfit) {
        snprintf(reply, size, "Name an outfit from outfits.txt");
        return SCO_BAD_ARG;
    }
    const int n = Menu_OutfitCount();   // asking starts the read of outfits.txt
    if (n < 0) {
        snprintf(reply, size, "The outfit list loads once you're in the universe; try again in a moment");
        return SCO_FAILED;
    }
    const int i = FindBuiltinName(n, Menu_OutfitName, outfit);
    if (i < 0) {
        snprintf(reply, size, "'%s' isn't in the outfit list (outfits.txt)", outfit);
        return SCO_FAILED;
    }
    Menu_RequestWearOutfit(i);
    snprintf(reply, size, "Wearing %s", Menu_OutfitName(i));
    return SCO_OK;
}

void OnTick(const char*, const void*, void*) {
    if (!g_tp.ok) return;
    ProcessLoadout();
    ProcessOutfits();
}

const sco_plugin_info* LoadoutQuery() { return &kInfo; }

sco_result LoadoutLoad(const sco_api* api, sco_plugin* self) {
    const sco_arg_def items[1] = { BuiltinArg("items", SCO_ARG_STRING, "Item names from items.txt, separated by spaces or commas") };
    const sco_arg_def outfit[1] = { BuiltinArg("outfit", SCO_ARG_STRING, "Outfit name, as in outfits.txt") };
    sco_result r = RegisterBuiltinCommand(api, self, kCap, "loadout.equip", "Equip gear",
        "Equips these items, at most one per slot, as the gear menu's Equip gear does", Equip, items, 1);
    if (r == SCO_OK) r = RegisterBuiltinCommand(api, self, kCap, "loadout.wear", "Wear SQ42 outfit",
        "Wears a Squadron 42 outfit, as the Squadron 42 tab does", Wear, outfit, 1);
    if (r == SCO_OK) r = api->subscribe(self, "tick", OnTick, nullptr);
    if (r != SCO_OK) return r;   // the host releases what was registered
    // Its page of the menu (and keys), through sco.ui; the menu shell draws it (tabs.h).
    RegisterPlayerTab(api, self);
    RegisterBuiltinTab(api, self, "loadout.sq42", "Squadron 42", kTabSq42, DrawSq42Tab);
    g_ticking = true;
    return SCO_OK;
}

// A crash leaves g_ticking set, so dllmain doesn't take the loadout tick back.
void LoadoutUnload() { g_ticking = false; }

}  // namespace

bool LoadoutBuiltinOwnsTick() { return g_ticking; }

const sco::plugins::Builtin kLoadoutBuiltin = { "loadout", LoadoutQuery, LoadoutLoad, LoadoutUnload };
