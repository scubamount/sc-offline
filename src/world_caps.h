#pragma once
// sco-core's world capabilities (sco/game/world.h): build mode, missions, cvars and the quantum
// boost take their addresses from these signature rows. Call after sco::ResolveAll.
#include "sco/caps.h"
#include "sco/game/world.h"
#include "sco/signatures.h"
#include <cstring>

// Sets the capability from its rows and returns whether it's ready.
inline bool WorldCapability(const char* name) {
    size_t n = 0;
    const sco::game::world::Capability* caps = sco::game::world::Capabilities(n);
    for (size_t i = 0; i < n; ++i)
        if (strcmp(caps[i].name, name) == 0) {
            sco::caps::SetFromSignatures(caps[i].name, caps[i].rows, caps[i].count);
            return sco::caps::Has(caps[i].name);
        }
    return false;
}
