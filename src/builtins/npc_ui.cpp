// The npc built-in's menu tab: NPCs, and the NPC picker the Crew tab shares. Registered through
// sco.ui (RegisterNpcsTab, called by npc_plugin.cpp) and drawn by the menu shell. The buttons run
// the built-in's npc.spawn and npc.clear commands through sco_api's invoke.
#include "tabs.h"
#include "../menu.h"
#include "../menu_ui.h"
#include "../third_party/imgui/imgui.h"
#include <cstdio>
#include <cstring>

static const sco_api* g_api = nullptr;
static sco_plugin*    g_self = nullptr;

// The command's reply (a refusal, or "Spawning ...") in the menu's status strip, which also logs it.
static void ShowReply(sco_result, const char* reply, void*) { SetMenuStatus("%s", reply ? reply : ""); }

static void InvokeNpcCommand(const char* name, const sco_arg* args, uint32_t nargs) {
    if (g_api && g_self) g_api->invoke(g_self, name, args, nargs, ShowReply, nullptr);
}

void RegisterNpcsTab(const sco_api* api, sco_plugin* self) {
    g_api = api;
    g_self = self;
    RegisterBuiltinTab(api, self, "npc.npcs", "NPCs", kTabNpcs, DrawNpcsTab);
}

// --- NPC picker, shared by the NPCs and Crew tabs -------------------------------------------

int g_npcPick = 0;

bool NpcPicker() {
    static char filter[64] = "";
    const int npcs = Menu_NpcCount();
    if (npcs < 0) { Hint("Loading NPCs (you need to be in the universe)..."); return false; }
    if (npcs == 0) { Hint("No NPCs found. Check data\\npcs.txt."); return false; }
    if (g_npcPick >= npcs) g_npcPick = 0;
    if (SearchBox("##npcFilter", "Search NPCs", filter, sizeof(filter)))
        for (int i = 0; i < npcs; ++i)
            if (MatchesFilter(Menu_NpcName(i), filter)) { g_npcPick = i; break; }
    char preview[128];
    PrettyBuildName(preview, sizeof(preview), Menu_NpcName(g_npcPick));
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##npc", preview, ImGuiComboFlags_HeightLarge)) {
        for (int i = 0; i < npcs; ++i) {
            const char* name = Menu_NpcName(i);
            if (!MatchesFilter(name, filter)) continue;
            char label[160];
            PrettyBuildName(label, sizeof(label) - 16, name);
            snprintf(label + strlen(label), 16, "##n%d", i);
            if (ImGui::Selectable(label, i == g_npcPick)) g_npcPick = i;
            ImGui::SetItemTooltip("%s", name);
            if (i == g_npcPick) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return true;
}

void DrawNpcsTab(void*, void*) {
    SectionHeading("Spawn NPCs");
    if (!NpcPicker()) return;
    static int howMany = 1;
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderInt("##howMany", &howMany, 1, 10, howMany == 1 ? "1 NPC" : "%d NPCs");
    if (PrimaryButton("Spawn in front of me")) {
        sco_arg args[2] = {};
        args[0].type = SCO_ARG_STRING;
        args[0].v.s = Menu_NpcName(g_npcPick);
        args[1].type = SCO_ARG_INT;
        args[1].v.i = howMany;
        InvokeNpcCommand("npc.spawn", args, 2);
        MenuClose();
    }
    if (ImGui::Button("Remove spawned NPCs", ImVec2(-1, 0))) {
        InvokeNpcCommand("npc.clear", nullptr, 0);
        Menu_RequestClearNpcs();   // the crew's NPCs, which the spawner tracks in npc.cpp
    }
    Hint("Removes every NPC this menu has spawned, crew included.");
}
