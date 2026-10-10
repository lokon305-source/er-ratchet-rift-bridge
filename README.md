# Elden Ring × Ratchet & Clank: Rift Apart

Play **Elden Ring** as **Ratchet**. Both games run at once: Rift Apart provides
Ratchet's real movement, weapons, gadgets and HUD; Elden Ring provides the world,
enemies and final picture. A bridge translates between them and draws Ratchet over
Elden Ring with a window-capture overlay.

**Start with [HANDOFF.md](HANDOFF.md)**: what works, architecture, install layout,
reverse-engineering notes, version history and open items. [DESIGN.md](DESIGN.md) is
the original design document.

## Status

- Playable. Movement on Elden Ring ground, guns, melee, grenades, Shatterbombs and
  Glove of Doom robots damage Elden Ring enemies; bolts mirror runes; Ratchet-style
  health bars.
- `riftbridge3-10f.cpp` (latest) adds ranged robots (Mr. Fungi, Topiary Sprinkler) and
  more chaser types. **Untested.** `riftbridge3-10e.cpp` is the last confirmed-good build.

## Layout

| Path | What |
|---|---|
| `riftbridge/` | `faultrep.dll` loaded inside Rift Apart (10f current, 10e confirmed) |
| `riftlink/` | External link + overlay (`riftlink.exe`), example config, launcher |
| `erbridge/include/` | Shared-memory protocol with the Elden Ring side |
| `shared/` | Terrain shared-memory layout |
| `re/` | Reverse-engineering helpers |

## Requirements

- Your own copies of **Elden Ring** and **Ratchet & Clank: Rift Apart** (3.630.1.0).
  No game files are included in this repository.
- Build steps and install layout: HANDOFF.md §4 and §8.

## Play offline

This mod reads and writes live game memory. Run Elden Ring **offline**; Easy
Anti-Cheat can ban accounts that play online with modified games.
