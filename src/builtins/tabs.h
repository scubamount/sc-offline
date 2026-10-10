#pragma once
// The built-ins' menu tabs and hotkeys, through sco-core's sco.ui service (include/sco_ui.h).
//
// Each built-in that owns a page of the menu registers it in its load function; the menu shell
// (menu.cpp) lists sco.ui's tabs by order and calls each draw function on the game thread, with
// sc-offline's ImGui context as the frame (the built-ins share it with menu.cpp: same module, same
// ImGui). The draw functions live in <feature>_ui.cpp next to the built-in.
//
// A built-in loads without its tab or hotkey when sco.ui refuses it (logged): its commands still
// work through invoke.
#include "sco_api.h"
#include "sco_ui.h"
#include <cstddef>
#include <cstdio>

// The menu's tab order (sco.ui order). The Menu tab is the product's own and comes after every
// registered tab; a plugin's tab picks any order (900 sits between Squadron 42 and Menu).
constexpr int32_t kTabPlayer   = 100;   // loadout.player
constexpr int32_t kTabTravel   = 200;   // quantum.travel
constexpr int32_t kTabVehicles = 300;   // spawn.vehicles
constexpr int32_t kTabCrew     = 400;   // crew.crew
constexpr int32_t kTabNpcs     = 500;   // npc.npcs
constexpr int32_t kTabBuild    = 600;   // build.build
constexpr int32_t kTabMultiplayer = 650;   // multiplayer.session
constexpr int32_t kTabSq42     = 700;   // loadout.sq42
constexpr int32_t kTabTitanLink = 800;  // titanlink.titanfall (optional bridge, SCO_BRIDGE_TITANLINK)
constexpr int32_t kTabVoxel     = 810;  // voxel_bridge.voxel (optional bridge, SCO_BRIDGE_VOXEL)

// Draw functions (sco_ui_draw_fn), in <feature>_ui.cpp.
void DrawPlayerTab(void* frame, void* ctx);     // loadout_ui.cpp
void DrawSq42Tab(void* frame, void* ctx);       // loadout_ui.cpp
void DrawTravelTab(void* frame, void* ctx);     // quantum_ui.cpp
void DrawVehiclesTab(void* frame, void* ctx);   // spawn_ui.cpp
void DrawCrewTab(void* frame, void* ctx);       // crew_ui.cpp
void CrewTabTick(uint32_t now);                 // crew_ui.cpp: the Crew tab's seat actions (spawner.cpp's ProcessCrew)
void DrawNpcsTab(void* frame, void* ctx);       // npc_ui.cpp
void DrawBuildTab(void* frame, void* ctx);      // build_ui.cpp
void DrawMultiplayerTab(void* frame, void* ctx);   // multiplayer_ui.cpp
void DrawTitanLinkTab(void* frame, void* ctx);  // titanlink/titanlink_ui.cpp (optional)
void DrawVoxelTab(void* frame, void* ctx);      // voxel_bridge/voxel_ui.cpp (optional)

// The NPC picker the NPCs and Crew tabs share (npc_ui.cpp): a search box and a combo over
// npcs.txt. False while the list isn't there (a hint is shown instead). g_npcPick is the pick.
bool NpcPicker();
extern int g_npcPick;

// sco.ui from the host, or nullptr when it doesn't publish it.
inline const sco_ui_v1* BuiltinUi(const sco_api* api) {
    const sco_ui_v1* ui = nullptr;
    if (api->size <= offsetof(sco_api, query_service) ||
        api->query_service(SCO_UI_NAME, SCO_UI_VERSION_1_0, reinterpret_cast<const void**>(&ui)) != SCO_OK)
        return nullptr;
    return ui;
}

// Logs a refused sco.ui call with the host's reason ("f6 is bound by 'x' to x.go").
inline void LogUiRefusal(const sco_api* api, sco_plugin* self, const sco_ui_v1* ui, const char* what, sco_result r) {
    char why[160] = "";
    uint32_t n = sizeof(why);
    if (ui) ui->last_error(self, why, &n);
    char line[256];
    snprintf(line, sizeof(line), "%s refused (%d): %s", what, static_cast<int>(r), why[0] ? why : "sco.ui isn't published");
    api->log(self, SCO_LOG_WARN, line);
}

// Registers one of a built-in's menu tabs. A refusal is logged; the built-in loads without it.
inline void RegisterBuiltinTab(const sco_api* api, sco_plugin* self, const char* id, const char* title, int32_t order,
                               sco_ui_draw_fn draw) {
    const sco_ui_v1* ui = BuiltinUi(api);
    const sco_result r = ui ? ui->register_tab(self, id, title, order, draw, nullptr) : SCO_NOT_FOUND;
    if (r != SCO_OK) {
        char what[96];
        snprintf(what, sizeof(what), "menu tab %s", id);
        LogUiRefusal(api, self, ui, what, r);
    }
}

// Binds one of sc-offline's own keys to a built-in's command (the menu shell dispatches it). A
// refusal is logged; the command still works from the menu and through invoke.
inline void BindBuiltinHotkey(const sco_api* api, sco_plugin* self, const char* chord, const char* command) {
    const sco_ui_v1* ui = BuiltinUi(api);
    const sco_result r = ui ? ui->bind_hotkey(self, chord, command, nullptr, 0) : SCO_NOT_FOUND;
    if (r != SCO_OK) {
        char what[96];
        snprintf(what, sizeof(what), "hotkey %s -> %s", chord, command);
        LogUiRefusal(api, self, ui, what, r);
    }
}
