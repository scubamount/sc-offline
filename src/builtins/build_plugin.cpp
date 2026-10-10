// build: build mode as a built-in plugin.
//
// build.toggle, build.undo, build.clear and build.place queue the same requests as the Build tab
// and the Squadron 42 tab's Spawn list, gated on the "build" capability (the free camera found
// and the spawner ready). The built-in binds F6 to build.toggle and registers the Build tab through
// sco.ui (build_ui.cpp). Its tick subscription runs ProcessBuild, which reads build mode's own keys
// as before: Backspace undoes, [ ] change reach, R rotates, the left mouse button places. The
// mechanics stay in build.cpp.
#include "builtins.h"
#include "tabs.h"
#include "../build.h"
#include "../menu.h"
#include "../teleport.h"
#include "../version.h"
#include <sc_entities.h>
#include <cstddef>
#include <cstdio>

namespace {

bool g_ticking = false;   // the tick subscription owns ProcessBuild

const sco_plugin_info kInfo = {
    sizeof(sco_plugin_info), SCO_API_MAJOR, SCO_API_MINOR, "build", SCO_VERSION, "sc-offline",
};

constexpr const char* kCap = "build";
constexpr double kMaxAhead = 100.0;   // metres in front of you

sco_result Toggle(const sco_arg*, uint32_t, void*, char* reply, uint32_t size) {
    const bool active = Menu_BuildModeActive();
    Menu_ToggleBuildMode();
    snprintf(reply, size, "Turning build mode %s", active ? "off" : "on");
    return SCO_OK;
}

sco_result Undo(const sco_arg*, uint32_t, void*, char* reply, uint32_t size) {
    if (Menu_BuildPlacedCount() == 0) {
        snprintf(reply, size, "Nothing placed to undo");
        return SCO_FAILED;
    }
    Menu_BuildUndo();
    snprintf(reply, size, "Removing the last thing you placed");
    return SCO_OK;
}

sco_result Clear(const sco_arg*, uint32_t, void*, char* reply, uint32_t size) {
    const int placed = Menu_BuildPlacedCount();
    if (placed == 0) {
        snprintf(reply, size, "Nothing placed to clear");
        return SCO_FAILED;
    }
    Menu_BuildClear();
    snprintf(reply, size, "Clearing the base (%d objects)", placed);
    return SCO_OK;
}

sco_result Place(const sco_arg* args, uint32_t, void*, char* reply, uint32_t size) {
    const char* thing = args[0].v.s;
    const double ahead = args[1].v.f;
    if (!thing || !*thing) {
        snprintf(reply, size, "Name a buildable from buildables.txt");
        return SCO_BAD_ARG;
    }
    if (!(ahead >= 0.0 && ahead <= kMaxAhead)) {
        snprintf(reply, size, "Ahead must be 0 (at your feet) to %.0f m", kMaxAhead);
        return SCO_BAD_ARG;
    }
    const int n = Menu_BuildCount();   // asking starts the read of buildables.txt
    if (n < 0) {
        snprintf(reply, size, "The build list loads once you're in the universe; try again in a moment");
        return SCO_FAILED;
    }
    const int i = FindBuiltinName(n, Menu_BuildName, thing);
    if (i < 0) {
        snprintf(reply, size, "'%s' isn't in the build list (buildables.txt)", thing);
        return SCO_FAILED;
    }
    Menu_RequestPlace(i, ahead > 0.0, static_cast<float>(ahead));
    if (ahead > 0.0) snprintf(reply, size, "Placing %s %.0f m in front of you", Menu_BuildName(i), ahead);
    else             snprintf(reply, size, "Placing %s at your feet", Menu_BuildName(i));
    return SCO_OK;
}

void OnTick(const char*, const void*, void*) {
    if (g_tp.ok) ProcessBuild();
}

const sco_plugin_info* BuildQuery() { return &kInfo; }

sco_result BuildLoad(const sco_api* api, sco_plugin* self) {
    const sco_arg_def place[2] = {
        BuiltinArg("object", SCO_ARG_STRING, "A buildable, as in buildables.txt"),
        BuiltinArg("ahead", SCO_ARG_FLOAT, "Metres in front of you (0 = at your feet, up to 100)"),
    };
    sco_result r = RegisterBuiltinCommand(api, self, kCap, "build.toggle", "Build mode",
        "Turns build mode on or off (F6)", Toggle);
    if (r == SCO_OK) r = RegisterBuiltinCommand(api, self, kCap, "build.undo", "Undo",
        "Removes the last object you placed (Backspace in build mode)", Undo);
    if (r == SCO_OK) r = RegisterBuiltinCommand(api, self, kCap, "build.clear", "Clear base",
        "Removes everything you placed", Clear);
    if (r == SCO_OK) r = RegisterBuiltinCommand(api, self, kCap, "build.place", "Place object",
        "Places one buildable without entering build mode; it joins the base, so undo and clear remove it", Place, place, 2);
    if (r == SCO_OK) r = api->subscribe(self, "tick", OnTick, nullptr);
    if (r != SCO_OK) return r;   // the host releases what was registered
    // Props go through game.entities (sc_entities.h, published by sco-core's game pack); without
    // it, or when its capabilities are off on this game build, build mode uses the spawner as before.
    const sc_entities_v1* ent = nullptr;
    if (api->size > offsetof(sco_api, query_service) &&
        api->query_service(SC_ENTITIES_NAME, SC_ENTITIES_VERSION_1_0, reinterpret_cast<const void**>(&ent)) == SCO_OK && ent &&
        ent->size > offsetof(sc_entities_v1, last_error))
        BuildUseEntities(ent, self);
    else
        Log("[build] game.entities isn't available: props are placed through the spawner");
    // The ground ray with nothing skipped and the camera go through game.world (sc_world.h, the same game
    // pack); without it, or when its capabilities are off on this game build, build mode reads the game
    // through sco-core's build rows as before. The aim ray that skips the preview stays on the rows.
    const sc_world_v1* world = nullptr;
    if (api->size > offsetof(sco_api, query_service) &&
        api->query_service(SC_WORLD_NAME, SC_WORLD_VERSION_1_0, reinterpret_cast<const void**>(&world)) == SCO_OK && world &&
        world->size > offsetof(sc_world_v1, camera))
        BuildUseWorld(world, self);
    else
        Log("[build] game.world isn't available: the ground ray and the camera come from sco-core's build rows");
    // Its page of the menu (and keys), through sco.ui; the menu shell draws it (tabs.h).
    RegisterBuiltinTab(api, self, "build.build", "Build", kTabBuild, DrawBuildTab);
    BindBuiltinHotkey(api, self, "f6", "build.toggle");
    g_ticking = true;
    return SCO_OK;
}

// A crash leaves g_ticking set, so dllmain doesn't take ProcessBuild back.
void BuildUnload() {
    g_ticking = false;
    BuildUseWorld(nullptr, nullptr);
    BuildUseEntities(nullptr, nullptr);   // the host despawns what this plugin still owns through game.entities (the preview; placed props were kept)
}

}  // namespace

bool BuildBuiltinOwnsTick() { return g_ticking; }

const sco::plugins::Builtin kBuildBuiltin = { "build", BuildQuery, BuildLoad, BuildUnload };
