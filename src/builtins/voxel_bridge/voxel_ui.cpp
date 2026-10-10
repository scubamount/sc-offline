// The voxel_bridge built-in's menu tab: Voxel (the link, the building area, the crates). Registered
// through sco.ui by voxel_plugin.cpp and drawn by the menu shell; every button runs the same work as
// the built-in's commands.
#include "voxel.h"
#include "../tabs.h"
#include "../../menu_ui.h"
#include "../../third_party/imgui/imgui.h"
#include <cstdio>

void DrawVoxelTab(void*, void*) {
    static char reply[200] = "";
    const VxStatus& s = VxGetStatus();
    SectionHeading("Voxel game");
    if (!s.linkOpen)     ImGui::TextUnformatted("Link: closed");
    else if (!s.linked)  ImGui::TextUnformatted("Link: open, waiting for the voxel game");
    else                 ImGui::Text("Link: voxel game %s (%u ms)", s.inWorld ? "in its world" : "linked, no world loaded", s.peerAgeMs);
    if (s.peerStatus[0]) ImGui::Text("Voxel game: %s", s.peerStatus);
    if (s.peerLog[0]) ImGui::Text("Last message: %s", s.peerLog);
    if (PrimaryButton(s.linkOpen ? "Close the bridge (Ctrl+F9)" : "Open the bridge (Ctrl+F9)")) VxToggle(reply, sizeof(reply));

    SectionHeading("Building area");
    if (!s.anchored) ImGui::TextUnformatted("None yet: it is made where you stand once the bridge is open");
    else ImGui::Text("In %s%s; you're at block %.0f %.0f %.0f", s.zone[0] ? s.zone : "your zone", s.paused ? " (you're outside it)" : "",
                     s.feet[0], s.feet[1], s.feet[2]);
    if (ImGui::Button("Move the area here", ImVec2(-1, 0))) VxAnchor(reply, sizeof(reply));
    ImGui::Text("Crates: %zu (%zu still to spawn)", s.crates, s.pending);
    if (s.dropped) ImGui::Text("%u solid blocks over the crate limit aren't shown", s.dropped);
    ImGui::Text("Blocks are %s, %.2f m", s.block, s.blockSize);
    if (reply[0]) ImGui::TextUnformatted(reply);
    Hint("Needs the voxel game with its sc-offline mod (docs/bridges.md). The crate class and size are set in data\\voxel_bridge.txt.");
}
