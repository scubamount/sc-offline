# Changelog

## 0.9.0-rc1

First release candidate of the seats, crew, travel and menu update. Debugging and analysis code
from development is removed, and the release build is optimized.

### Menu
- New tabbed menu: Player, Travel, Vehicles, Crew, NPCs, Build and Menu.
- Dark green theme, Bahnschrift font (Segoe UI fallback), and a status line at the bottom that
  shows what the mod just did.
- Optional background image: save `data\menu_background.png` (or `.jpg`). Darkness and image
  position are set in the Menu tab. The file is ignored by git.
- The version shows in the title bar, the Menu tab and the first line of `mod.log`.

### Ships and seats
- Choose where you board a spawned ship: pilot seat, a seat by name, pick after it spawns, or none.
- Optionally remove the NPC in the seat you want instead of taking another seat.
- Crew tab: every seat on the ship and who is in it (you, NPC, empty). Sit here, Stand up,
  Remove NPC and Add NPC per seat; Fill empty seats, All NPCs stand up and Remove all NPCs.
- Power on now sends the game's own Flight Ready event to the pilot dashboard, including on ships
  whose dashboard is a separate part (F8C, Moth...). It falls back to pressing R only if needed.
- Infinite ship ammo: refills the magazines of the ship you're in. Every magazine on the ship is
  also topped up twice a second, for energy weapons that drain another way.

### Travel
- Travel tab with places grouped by star system: planets, moons, Lagrange points, comm arrays,
  jump points and landing zones, for Stanton, Pyro and Nyx.
- Scan the game for places finds everything loaded in a few seconds. Interiors and small zones are
  hidden unless you ask for them.
- Named saved spots, grouped by system, stored in `data\bookmarks.txt`. F7/F8 still work.
- Teleports refuse to cross star systems instead of leaving you stuck in empty space.

### NPCs and building
- Removing NPCs, kicking crew, build mode's undo and Clear base now work. The game wants an entity
  handle, not an id. NPCs the game refuses to delete offline are taken out of their seat and moved
  far out of range instead.
- Build mode previews prefabs with a flag while you move the camera and the real building when you
  hold still. Clicking keeps the previewed building.

### Build
- Release x64 is built with full optimization, link-time code generation and the static C runtime,
  so the DLL runs without the Visual C++ redistributable.

### Needs testing before the final release
These were built after the last in-game test and haven't been confirmed yet:
- The energy weapon top-up (does an energy weapon still run dry with infinite ship ammo on?).
- Proper NPC deletion through the entity system's handle lookup. If it doesn't take, the
  send-far-away fallback (confirmed working) still removes them from the world.
- Stand up and All NPCs stand up.
- The real-building prefab preview in build mode.
- Pyro and Nyx system names after a fresh place scan.

### Known limitations
- NPCs added to seats sit there but don't fly the ship or operate turrets.
- Power on uses Flight Ready, so some systems (engines, weapons) may start off on some ships.
  Per-system power options are planned.
- Places in Pyro arrive in orbit until their radii are added to `data\locations.txt`.
