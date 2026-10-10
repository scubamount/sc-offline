# Plugin UI: menu tabs, overlays and hotkeys

sc-offline's menu (**M**) is a shell over sco-core's `sco.ui` service ([sco-core docs/ui.md](https://github.com/scubamount/sco-core/blob/main/docs/ui.md)). Every feature tab is registered by its built-in plugin, and a plugin can add its own tab, overlay and hotkeys the same way. This page covers what sc-offline adds to sco-core's contract.

## Tabs

The menu shows the tabs registered through `sco.ui` by ascending `order`, ties in registration order, then sc-offline's own **Menu** tab, which is always last. A badge appears after the title, as in `UI probe (3)`. The built-ins' tabs and their orders:

| Order | Tab | Registered by | Id |
| ---: | --- | --- | --- |
| 100 | Player | `loadout` | `loadout.player` |
| 200 | Travel | `quantum` | `quantum.travel` |
| 300 | Vehicles | `spawn` | `spawn.vehicles` |
| 400 | Crew | `crew` | `crew.crew` |
| 500 | NPCs | `npc` | `npc.npcs` |
| 600 | Build | `build` | `build.build` |
| 700 | Squadron 42 | `loadout` | `loadout.sq42` |
| — | Menu | sc-offline | (not in `sco.ui`) |

To put a tab between two of these, give it an order in between. An order above 700 puts it after Squadron 42. `teleport` and `contracts` have no page of their own: their controls sit on the Player, Vehicles and Travel tabs, or they have no menu controls at all. The creative plugin has none either: the Player and Vehicles tabs' cheat checkboxes run its commands.

## Drawing: ImGui through sc-offline's frame

A draw callback gets `frame`, which is sc-offline's `ImGuiContext*`. It is valid **only during that call** and only on the game thread, the only thread sco-core runs draws on. The callback draws the tab's body inside the menu window, below the tab bar and above the status strip, and the window's scrolling child is already open. Overlays draw after the menu window, once per menu frame.

A plugin that draws with ImGui has to use **the same ImGui as sc-offline**:

1. Compile sc-offline's vendored ImGui into the plugin: `src/third_party/imgui/` at the sc-offline version you target (`imgui.cpp`, `imgui_draw.cpp`, `imgui_tables.cpp`, `imgui_widgets.cpp`, with its `imconfig.h` unchanged and no backends). This branch ships ImGui 1.92.8 WIP (`IMGUI_VERSION_NUM` 19277).
2. Include [`src/menu_imgui.h`](../src/menu_imgui.h), and call `sc_offline_imgui::Bind(frame)` first in every draw callback. It points the plugin's copy of ImGui at sc-offline's context and at the allocator sc-offline's ImGui uses (the process heap), so either side can free memory the other allocated, whichever C runtime each one links.
3. Use plain ImGui calls after that. Don't keep the context or any ImGui pointer past the call, and don't call `NewFrame`, `Render` or a backend.

```cpp
#include "sco_ui.h"
#include "menu_imgui.h"

static void DrawMyTab(void* frame, void*) {
    sc_offline_imgui::Bind(frame);
    ImGui::SeparatorText("My plugin");
    if (ImGui::Button("Wave")) { /* invoke a command */ }
}
// in sco_plugin_load: ui->register_tab(self, "myplugin.main", "My plugin", 900, DrawMyTab, NULL);
```

[`tools/test-plugins/ui_probe`](../tools/test-plugins/ui_probe/ui_probe.cpp) is a complete example; `CMakeLists.txt` builds it against the `imgui` library target. A different ImGui version or `imconfig.h` is undefined behaviour.

We chose to have plugins call ImGui directly rather than go through a C drawing shim: ImGui supports being shared across DLLs this way (context and allocator per module), a plugin gets every widget the built-ins use, and sc-offline doesn't have to keep a second API in step with ImGui. The cost is that a plugin must be rebuilt when sc-offline updates its ImGui.

**Crashes.** Each draw runs as a callout of its plugin under sco-core's crash guard. When a draw faults, sco-core disables that plugin and removes its tabs, badges, overlays and hotkeys. Around every draw, the menu stores ImGui's stack state and restores it afterwards (`ErrorRecoveryStoreState`), so a draw that faulted half-way, or forgot an `End*()`, doesn't break the rest of the frame. `mod.log` records `[menu] tab <id> (<title>) crashed; plugin '<id>' is disabled`, with sco-core's own crash line next to it.

**Where the frame is built.** The menu window and its Direct3D device live on the menu's own thread. Because `sco.ui` draws run on the game thread only, the game thread builds each ImGui frame: `NewFrame`, the shell, every tab and `Render`. The menu thread then renders it, and the two threads never use ImGui at the same time. While the game thread is busy (a loading screen), the menu keeps showing its last picture.

**Overlays.** sc-offline has no drawing surface over the game itself, only the menu window. Overlays therefore draw over the menu while it is open, not over the game while it is closed.

## Hotkeys

When a key is pressed while the game or the menu is in front (and the menu isn't typing into a text box), sc-offline builds the chord (`ctrl+alt+9`, `f6`). If the chord is bound, it runs the bound command through `sco::ui::Dispatch`, and `mod.log` shows `[hotkey] Ctrl+Alt+9 -> ui_probe.ping: OK "ui_probe ping 1"`. A plugin binds any free chord with `bind_hotkey`. The first plugin to bind a chord keeps it, and a later bind is refused with the holder's name.

| Key | Does | How |
| --- | --- | --- |
| **M** | Opens and closes the menu | Reserved by sc-offline |
| **F6** | `build.toggle` | Bound by the `build` built-in |
| **F7** | `teleport.save` | Bound by the `teleport` built-in |
| **F8** | `teleport.go` | Bound by the `teleport` built-in |
| **R**, **[**, **]**, **Backspace** | Build mode: rotate, reach closer, reach farther, undo | Reserved by sc-offline (used only while build mode is on) |
| **Caps Lock** (hold) | Quantum boost while held | Read by sc-offline's quantum code. It is a held key, not a chord, and the chord grammar has no Caps Lock |
| Left mouse button | Build mode: place | Read by build mode, not a chord |

Built-ins load before any plugin folder, so their bindings are in place before a plugin can ask for F6, F7 or F8. Binding a reserved chord is refused with `<chord> is reserved by the host`.
