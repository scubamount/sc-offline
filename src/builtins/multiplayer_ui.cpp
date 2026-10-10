// The multiplayer built-in's menu tab: host, join or leave a session, and who's in it. Registered
// through sco.ui by multiplayer_plugin.cpp and drawn by the menu shell.
#include "multiplayer.h"
#include "tabs.h"
#include "../menu_ui.h"
#include "../third_party/imgui/imgui.h"
#include <cstdio>

namespace {

char g_message[192] = "";   // the last Host / Join answer

void Answer(const char* err, const char* ok) { snprintf(g_message, sizeof(g_message), "%s", err ? err : ok); }

void SessionForm(MpSettings& s) {
    SectionHeading("You");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##mpName", "Your name, as the others see it", s.name, sizeof(s.name));
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##mpPass", "Session passphrase (at least 8 characters)", s.passphrase, sizeof(s.passphrase),
                             ImGuiInputTextFlags_Password);
    ImGui::Checkbox("Remember the passphrase on this PC", &s.rememberPass);
    ImGui::SetItemTooltip("Kept unencrypted in data\\storage\\multiplayer.db. Off: you type it each time.");

    SectionHeading("Host a session");
    ImGui::SetNextItemWidth(Columns(2));
    ImGui::InputInt("UDP port##mpHostPort", &s.hostPort, 0);
    if (PrimaryButton("Host")) Answer(Mp_Host(), "Hosting: give the others your LAN address, the port and the passphrase.");

    SectionHeading("Join a session");
    ImGui::SetNextItemWidth(Columns(2));
    ImGui::InputTextWithHint("##mpAddr", "Host's address (192.168.1.20)", s.joinAddress, sizeof(s.joinAddress));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputInt("##mpJoinPort", &s.joinPort, 0);
    if (PrimaryButton("Join")) Answer(Mp_Join(), "Joining...");
}

void PeerList() {
    const int n = Mp_PeerCount();
    if (!ImGui::BeginTable("##mpPeers", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("Player");
    ImGui::TableSetupColumn("Seen as");
    ImGui::TableSetupColumn("Ship");
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();
    for (int i = 0; i < n; ++i) {
        MpPeerView v;
        if (!Mp_Peer(i, v)) continue;
        ImGui::PushID(i);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%s", v.name);
        ImGui::SetItemTooltip("player %llu, session id %016llx", static_cast<unsigned long long>(v.peer),
                              static_cast<unsigned long long>(v.sessionId));
        ImGui::TableNextColumn();
        ImGui::TextWrapped("%s", v.avatar);
        ImGui::TableNextColumn();
        ImGui::TextWrapped("%s", v.ship[0] ? v.ship : "-");
        ImGui::TableNextColumn();
        if (!v.self && ImGui::SmallButton("Go to")) Answer(Mp_GoTo(v.peer), "Teleported.");
        ImGui::PopID();
    }
    ImGui::EndTable();
}

}  // namespace

void DrawMultiplayerTab(void*, void*) {
    if (!Mp_Enabled()) {
        Hint("Multiplayer isn't available (multiplayer = off in sc-offline.ini, or sco.net is missing; see mod.log).");
        return;
    }
    MpSettings& s = Mp_Settings();
    SectionHeading("Session");
    ImGui::Text("%s", Mp_StateText());
    if (Mp_LastReason()[0]) ImGui::TextColored(kMuted, "Last session ended: %s", Mp_LastReason());
    if (g_message[0]) ImGui::TextWrapped("%s", g_message);
    if (Mp_InSession()) {
        if (ImGui::Button("Leave", ImVec2(-1, 0))) { Mp_Leave(); g_message[0] = 0; }
        PeerList();
        if (!Mp_GhostsReady()) Hint("Ghosts are off: this game build lacks spawn.entities 1.2 or teleport.spatial.");
    } else {
        SessionForm(s);
    }

    SectionHeading("Others' ghosts");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##mpAvatar", "NPC class others see you as", s.avatarClass, sizeof(s.avatarClass));
    ImGui::SetItemTooltip("A human class from npcs.txt. Another player sees you as it when their npcs.txt has it,\n"
                          "otherwise as their own choice.");
    ImGui::Checkbox("Show other players' ships", &s.showShips);
    if (ImGui::Button("Save settings", ImVec2(-1, 0))) Mp_SaveSettings();
    Hint("LAN only (and multiplayer_allow in sc-offline.ini for a VPN). Messages are signed with the passphrase but not "
         "encrypted. Ghosts are visual only: no shared physics, damage, missions or inventory.");
    if (Mp_AllowText()[0]) ImGui::TextColored(kMuted, "Also allowed: %s", Mp_AllowText());
}
