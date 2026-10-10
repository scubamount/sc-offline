// The titanlink built-in's menu tab: Titanfall (the link, pilot mode, your Titan). Registered through
// sco.ui by titanlink_plugin.cpp and drawn by the menu shell; every button runs the same work as the
// built-in's commands.
#include "titanlink.h"
#include "../tabs.h"
#include "../../menu_ui.h"
#include "../../third_party/imgui/imgui.h"
#include <cstdio>

void DrawTitanLinkTab(void*, void*) {
    static char reply[200] = "";
    const TlStatus& s = TlGetStatus();
    SectionHeading("Titanfall 2");
    if (!s.linkOpen)        ImGui::TextUnformatted("Link: closed");
    else if (!s.linked)     ImGui::TextUnformatted(s.launched ? "Link: open, waiting for Titanfall 2 to start" : "Link: open, waiting for Titanfall 2");
    else if (!s.inMatch)    ImGui::Text("Link: Titanfall 2 linked (%u ms), loading its match", s.peerAgeMs);
    else                    ImGui::Text("Link: Titanfall 2 in a match (%u ms)", s.peerAgeMs);
    if (s.frameStatus[0]) ImGui::Text("Picture: %s", s.frameStatus);
    if (s.peerLog[0]) ImGui::Text("Titanfall says: %s", s.peerLog);
    if (s.game[0]) ImGui::Text("Game folder: %s (%s on %s)", s.game, s.mode, s.map);

    SectionHeading("Pilot mode");
    if (PrimaryButton(s.pilot ? "Stop pilot mode (F9)" : s.wantPilot ? "Cancel (F9)" : "Start pilot mode (F9)")) {
        TlTogglePilot(reply, sizeof(reply));
        if (!s.pilot && s.wantPilot) MenuClose();
    }
    const float half = Columns(2);
    if (ImGui::Button("Call Titan (V)", ImVec2(half, 0))) TlCallTitan(reply, sizeof(reply));
    ImGui::SameLine();
    if (ImGui::Button(s.inTitan ? "Disembark (E)" : "Embark (E)", ImVec2(half, 0))) TlEmbark(reply, sizeof(reply));
    if (ImGui::Button("Close link", ImVec2(-1, 0))) TlUnlink(reply, sizeof(reply));
    if (reply[0]) ImGui::TextUnformatted(reply);

    if (s.inMatch) {
        SectionHeading("Your Titan");
        if (s.inTitan) ImGui::Text("Piloting it, health %.0f%%", s.titanHealth * 100.0f);
        else if (s.titanParked && s.titanDistance >= 0) ImGui::Text("Parked %.0f m away, health %.0f%%", s.titanDistance, s.titanHealth * 100.0f);
        else ImGui::TextUnformatted("None yet: call one with V");
        if (s.weapon[0]) ImGui::Text("Weapon: %s, clip %d", s.weapon, s.clip);
    }
    Hint("Needs Titanfall 2 with Northstar and the TitanLink plugin (docs/bridges.md). Set the game folder, map and mode in "
         "data\\titanlink.txt. Star Citizen still sees the keys you press in pilot mode.");
}
