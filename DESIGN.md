# Elden Ring x Rift Apart: Passthrough Design

> Play Elden Ring as Ratchet, using Rift Apart's real movement, weapons, gadgets and HUD, in the real Lands Between, fighting Elden Ring's enemies.

Status: v0.4 · 2026-10-06 · playable prototype (see §9 "As built" and §10) · design based on SkyCraft (chasmlol/SkyCraft) and minecraft-crossover-bridge (justbustin)

---

## 1. Core principle

**Neither game is rewritten. Both run at once, and a bridge connects them.**

| Elden Ring (host) runs | Rift Apart (guest) runs |
|---|---|
| World, terrain, dungeons, lighting, weather | Ratchet & Clank, animation, **movement** (double jump, Hoverboots, Phantom Dash, wall run, grind, swingshot) |
| Enemies, bosses, AI, quests, saves | Weapons, ammo, gadgets, weapon wheel, Ratchet's health and HUD |
| Final picture | Its own layer: Ratchet, his weapon, VFX, HUD |

The bridge only **translates** between them. If we ever re-code Ratchet's double jump inside Elden Ring, the design has gone wrong.

## 2. What already exists on this PC

| Piece | State |
|---|---|
| **Elden Ring host bridge**: `erbridge_core` (from justbustin/minecraft-crossover-bridge, ported to Windows in an earlier Codex task: `Documents\Codex\2026-10-03\can-you-make-a-mass-up\outputs\Minecraft-EldenRing-Windows`) | **Working with Minecraft as the guest.** It already handles: game-thread tasks, camera override, a hidden "stand-in" Tarnished that follows the guest player, terrain ray queries (`CSPhysWorld::CastRay`), enemy table and HP (`CSChrDataModule::ModifyHp`, `ChrIns::Kill`), death sync both ways, world-shift handling, the action key (doors, levers, Sites of Grace), and a D3D12 compositor with depth occlusion. Targets ER **2.7.1.0 (App 1.17.1)**, which matches the installed `eldenring.exe` |
| `SkyCraftEldenRing.dll` (`Documents\SkyCraft-EldenRing-Bridge-v0.0.2`) | An earlier heartbeat-only skeleton; superseded by `erbridge_core` |
| **Rift Apart research** (Codex, `Documents\Codex\2026-10-04\i-x20\outputs\Rift-Elden-Bridge`) | Read-only. RiftApart.exe **3.0630.1.0**. Hero object graph found by RTTI: `HeroLocal` → (+0x90) `HeroPointerCache` → `HeroHealth`, `GameInventoryManager`, `HeroWeaponManagerLocal`, `PhysicsComponent`, `HeroMoverManager`, `HeroMoverStandard`, plus `ClankPack`. **Position:** `HeroMoverManager +0xC8` (XYZ, Y up; `+0xFC` = next position, `+0x114` = step). Validated by stand-still and three jumps (~2.5 units high). Collision code traced around RVA `0x19dd810` / `0x19f0330`. Units and axis signs not yet calibrated. Addresses must be re-found every session (object moved once) |

**So the Elden Ring half is largely done. The work is the Rift Apart half: a DLL inside RiftApart.exe that does what the Minecraft Fabric mod did.**

## 3. Components

```
┌──────────── eldenring.exe ─────────────┐        ┌──────────── RiftApart.exe ──────────────────┐
│ erbridge_core.dll  (exists)             │        │ riftbridge.dll  (to build)                  │
│                                         │        │                                             │
│ terrain ray hits ───────────────────────┼──────▶ │ CollisionField → Havok bodies / queries     │
│ enemy table (pos, box, HP) ─────────────┼──────▶ │ proxy targets (invisible, lock-on, hittable)│
│ hits on stand-in ───────────────────────┼──────▶ │ HeroHealth damage                           │
│                                         │        │                                             │
│ stand-in Tarnished    ◀── hero pos/yaw ─┼─────── │ HeroMoverManager (real movement)            │
│ camera override       ◀── camera ───────┼─────── │ Camera2 (over-the-shoulder)                 │
│ ModifyHp / Kill       ◀── damage queue ─┼─────── │ weapon hits on proxies                      │
│ D3D12 compositor      ◀── frames.shm ───┼─────── │ Ratchet + weapon + VFX + HUD layer only     │
└─────────────────────────────────────────┘        └─────────────────────────────────────────────┘
        bridge.shm (8 MB, seqlocks) + frames.shm (triple-buffered layers)
```

`riftbridge.dll` speaks the **same shared-memory protocol** (`bridge_protocol.h`) as the Minecraft mod did. The host needs only small changes: Windows paths instead of the CrossOver `Z:\tmp\ermc` path (already done in the Windows port), unit scale, and new message types if needed.

## 4. Rift Apart side: riftbridge.dll

- **Loading:** a proxy DLL in the Rift Apart folder (e.g. `version.dll` or `dxgi.dll`) or an injector. Rift Apart has no anti-cheat; it's single-player. Ripped Apart's `mods` folder is separate and unaffected.
- **Finding objects each session:** scan for the `HeroLocal` vtable (RVA `0x2f48188`), validate it through RTTI, then follow `+0x90` to the pointer cache. Don't hard-code heap addresses.
- **Game thread:** hook a `HeroMoverManager` vtable slot (table at RVA `0x2f5a328`) or the main update, so reads and writes happen once per frame on the game thread.
- **Both engines use Havok's new physics (hknp).** Rift Apart's RTTI shows `hknpCharacterProxyInternals::QueryCollector` and `Physics::ighkCollisionQueryCommands::CastRayVsWorld`; Elden Ring uses Havok too. This gives two ways to feed Elden Ring's ground to Ratchet:
  - **A. Inject bodies:** add static boxes and triangle meshes from Elden Ring's ray hits into Rift Apart's hknp world near Ratchet. The character proxy then collides with them natively. This is the preferred option.
  - **B. Hook queries:** intercept the character proxy's query collector and cast-ray commands, and answer them from the Elden Ring field.
- **Void level:** Rift Apart's own level has to be out of the way. Either use a quiet, flat area and turn off its collision near Ratchet, or find a near-empty level such as a pocket dimension or the Clank rift puzzles. **Open question.**
- **Rendering only Ratchet:** turn off the level, sky and post-process in Rift Apart's renderer, then copy colour and depth to `frames.shm`. This is the hardest unknown: Rift Apart is D3D12 and DirectStorage, with no known render hooks.

## 5. Phases (each ends in a test you can see)

| # | Phase | Done when |
|---|---|---|
| 0a | **Telemetry** (mostly done) | Ratchet's position read reliably every frame, from inside the game, surviving reloads. Units and axes calibrated against Elden Ring metres |
| 0b | **Link** | `riftbridge.dll` writes Ratchet's position to `bridge.shm`. Running and jumping in Rift Apart moves the stand-in Tarnished in Elden Ring. Elden Ring's camera follows Ratchet's camera |
| 1 | **Ratchet's movement on Elden Ring ground** | Elden Ring collision is injected into Rift Apart (plan A or B). You can double jump, Hoverboot and Phantom Dash across Limgrave with Rift Apart's real physics: floor, wall, slope and ledge all behave |
| 2 | **See Ratchet** | Ratchet, his weapon and the Rift Apart HUD drawn in Elden Ring's frame. CPU copy first, depth later |
| 3 | **Combat** | Enemy proxies in Rift Apart. The Burst Pistol and Negotiator damage Elden Ring enemies; their hits damage Ratchet; deaths sync |
| 4 | **Polish** | Hidden Rift Apart window, auto-launch, loading and fast travel, saves, performance |

## 6. Risks

| Risk | Note |
|---|---|
| Rendering only Ratchet out of Rift Apart | No known hooks. Fallback: render Rift Apart normally with the level hidden and clear to a key colour, then composite without depth |
| Rift Apart's own level collision | It must be turned off near Ratchet, or Ratchet will hit invisible Rift Apart walls |
| Two AAA games at once | Both are heavy. The Rift Apart window can render at low resolution if it only draws Ratchet |
| Rift Apart updates | Patterns are pinned to RiftApart.exe 3.0630.1.0. Turn off Steam auto-update for Rift Apart and Elden Ring |
| Object addresses move | Re-find through vtable/RTTI every session (already seen once) |

## 7. Rules for any agent working on this
- Read-only research until a write is needed. Back up anything in a game folder before changing it.
- Elden Ring offline only (Easy Anti-Cheat off).
- Don't claim something works until it's been tested in-game. Key emulation, video overlays and model imports don't count as native integration.
- One agent changes files at a time. Claude and Codex must not both edit the bridge folders or game folders at once.

## 8. References
- SkyCraft: https://github.com/chasmlol/SkyCraft
- minecraft-crossover-bridge (Elden Ring host): https://github.com/justbustin/minecraft-crossover-bridge
- Ripped Apart (Rift Apart file tools): https://github.com/chaoticgd/ripped_apart
- fromsoftware-rs / libER (Elden Ring APIs): https://github.com/vswarte/fromsoftware-rs · https://github.com/Dasaav-dsv/libER

## 9. As built (2026-10-05): playable

Tested in-game by the user and confirmed playable. This is a working prototype, not the final design: the frames.shm compositor and renderer-level isolation from §4 were not used.

| Piece | What it is |
|---|---|
| **riftlink.exe** (`ELDEN RING\Game\riftbridge`) | External process. Reads Rift Apart memory (read-only). Drives the Elden Ring bridge through bridge.shm: the stand-in follows Ratchet, the camera follows Rift Apart's real camera, ray batches for terrain, damage queue. Writes the data-only feed terrain.shm. Also draws the overlay. Single instance; started by `Launch-Elden-Ring-Rift.cmd` |
| **faultrep.dll** (Rift Apart folder, proxy for ReportFault) | Inside Rift Apart. Hooks HeroMoverManager::Update (rva 0x72faa0) for per-frame game-thread work. Adds static hknp box bodies for Elden Ring ground, walls and enemy hitboxes. Hides the level's zones and collision. Infinite ammo uses the game's own flag (0x5166064). Bolt counter = Tarnished's runes. Input works from the Elden Ring window (IAT hooks + RIDEV_INPUTSINK); controller via keyboard/mouse stand-in (riftbridge\controller.ini) |
| **Overlay** | Windows Graphics Capture of the Rift Apart window, dark-keyed (Ratchet sinks 1000 m below the level into darkness: Delete), shown in a click-through layered window over Elden Ring. Ratchet-style enemy and boss health bars are drawn into it |

Key findings:
- **Camera:** the rendered camera is a ring of plain 4×4 matrix blocks, each 0x100 apart: axes at +0, position at +0x30, forward is axis 2. Always prefer that layout. Quaternion readings and the HeroCameraManager ideal eye give wrong pictures. Pick a copy in the ring by continuity, and force up to (0,1,0).
- **Body flags:** our bodies need the level flag bits `002b0000` (body+0x68). Without the flags, hitscan guns pass through. Filter `xxx8243f` on walls makes the camera bump into our bodies. Ctrl+Pause cycles the variants.
- **Bolts:** the GetCurrencyAction chain. Player slots `[0x68d3e78]` → handle→entity `0xf1a520` → component lookup key `0x60313c0` (`0xffb860`/`0xffca00`) → `0x86a660` → vf+0x198(`[0x60311a8]`) → vf+0x108 → +0x38.
- **Runes:** Elden Ring GameDataMan via pattern `48 8B 05 ?? ?? ?? ?? 48 85 C0 74 05 48 8B 40 58 C3 C3` → +0x08 PlayerGameData → runes at +0x6C, level at +0x68.
- **Boss names:** from the character model (cXXXX) table. Margit/Morgott and O'Neil/Niall are told apart by max HP.
- **Mapping:** Scale 1.25, FlipX=1.

## 10. Update (2026-10-06): throwables and explosions

Tested in-game: grenades, Shatterbombs, carpet bombs and Drillhounds land on Elden Ring ground near and far; Shatterbomb blasts took a 2169 HP enemy down in 8 bombs. faultrep.dll (riftbridge) 0.9f, riftlink 0.25.

| Finding | Detail |
|---|---|
| Thrown shots discard ownerless hits | Shot motions (GrenadeShotMotion etc.) look up the object behind each hit via `0x11ab4e0` (handle at result entry +8, top byte = type; table `[0x6747ce0]`, count `[0x6747cfc]`, lookup `0x12b64e0`). Our bodies have handle 0 (body+0x98 = 0), so the hit was skipped. Fix: `0x11ab4e0` is patched (one 8-byte atomic write, jmp to a trampoline) to return a static stand-in object (`+0x5c = 0x800`, "not an actor") for handle-0 hits; the next accessor `0x11ab290` returns no actor, so the shot takes its world-hit path. Works anywhere; no Rift Apart zone needed |
| Don't borrow level owners | Borrowing a level body's owner handle (zone object or actor) broke as zones streamed out, and crashed when re-resolving freed bodies (`0x11c31f0` on a stale body; shape vtable = hkBaseObject). That path is off |
| Ground reach | riftlink sends a fine grid (25×25 at 1 m), a far ring (19×19 at 4 m) and an outer ring (25×25 at 12 m, taller rays): about 150 m around the Tarnished, ~1430 tiles. Long throws used to land past the old ~38 m edge |
| Explosions → Elden Ring damage | Shots are entity-table actors with a `*ShotMotion*` component; actor position = `[record+0]+0x30`. riftbridge follows explosive shot types (Bouncer, CarpetBomber, Tunneler, Grenade, Homing…) every 10 ms; when one disappears, enemies within 4–5 m of its last position get a hit in terrain.shm (`hits[]` ring), and riftlink applies `ExplosionDamage` (8) × falloff through the bridge damage queue. Ignore origin positions and single >40 m jumps (spawn/death junk) |
| Mover pointer | Use the `self` of every HeroMoverManager::Update call, not the first one (the first can be a loading-time object reading 0,0,0) |
| Collision filter | `[13-bit group][7][layer 4][mask 12]`; vtable `0x32e50e0` slot 3 = pair filter (body+0x98 groups), slot 5 = by id, slot 6 = query vs body. Query classes xxxx1880/xxxx1900 (mask 0x80/0x100) reject our bodies and almost all level bodies too |
| Crash logging | faultrep.dll logs access violations (module+offset) to rift-bridge3.log. NRI.dll+412e1 / RiftApart.exe+16aeb23 at exit are the game shutting down |

Open:
- Robots (Glove of Doom agents, Mr. Fungi, Drillhounds) need Rift Apart actors to chase: invisible stand-in actors at each Elden Ring enemy, with their damage forwarded.
- Direct-hit weapons still use riftlink's aim cone + travel delay, not real impacts.
- Damage to Ratchet from Elden Ring hits: set aside by the user (off).

## 11. Research notes (2026-10-06): robots and targets

- Decompiler: radare2 6.2.4 + r2ghidra (Ghidra decompiler) built from git in the container (`/home/claude/tools`); `re/dec.sh <rva>...` decompiles single functions of RiftApart.exe in ~10 s each without whole-program analysis (2 cores / 7 GB is too small for full Ghidra analysis; GitHub release downloads and apt are blocked by the proxy, git clone works).
- Ratchet's robots are `GameBotDoom` (Glove of Doom) and `GameBotMrFunGuy` (Mr. Fungi) actors with `LogicalOwner`; Drillhounds are `ShotMotionTunneler(/Puppy)` shots. 0.10b steers them toward Elden Ring enemies (actor transform `[record+0]+0x30` plus matching copies in their parts) with contact damage and blast-on-expiry; tested working for the Glove of Doom.
- Targeting: `DoomBotTargeting` (derives `TieredAggroTargeting`, vtable 0x2ea7a10) iterates awareness entries (entry +0xa0 = position, +0x6c = actor handle, state byte > 2 = aware); entries whose handle no longer resolves are dropped, so targets must be real actors.
- `TargetData@Target` (vtable 0x4625338) registers in a global target registry at 0x580eed0 (slot index at +0x1a0, spatial grid 0x580fb20, per-team list chosen by +0x48, flag lists by bits of +0x4c); its position is at +0x180.
- Stand-in idea under test: give an existing hidden Rift Apart actor that has TargetData + Health (e.g. a crate) an enemy team and move it onto an Elden Ring enemy, then forward its health loss. 0.10c logs every TargetData (team, flags, slot, position) at start and when robots are out.

## 12. Final polish (2026-10-06): tested working

Current builds: faultrep.dll (riftbridge) **0.10e**, riftlink **0.27**. The user confirmed "works better now".

- **Sharper effects:** riftlink resizes Rift Apart's window so its inside matches Elden Ring's client size exactly (was 1923×1057 → 1920×1080 resampled every frame). `[overlay] MatchSize=1`; it can't resize a maximized window. Rift Apart runs at ~40 fps beside Elden Ring; effect quality otherwise comes from Rift Apart's own graphics settings (upscaler on Quality, no dynamic resolution).
- **Brightness:** the overlay lifts Ratchet's colours (levels from 0.6 × the cut-out threshold, then gamma 0.9 and gain 1.15). Ctrl+F9 / Ctrl+F10 adjust it, saved as `[overlay] Brightness`.
- **Real hits:** riftbridge counts a direct hit when a non-explosive tracked shot ends within 1.2 m of an Elden Ring enemy box (`RT_HIT_DIRECT` in `hits[]`, damage = `ShotDamage`). It bumps `shotSpawns` for every tracked shot. riftlink skips its aim-cone guess for 0.7 s after a spawn, so misses no longer count. Instant-hit guns still use the cone, narrowed to `AimConeDeg=6`.
- **Robots:** stand-in research is paused. Actors in hidden zones drop out of the target registry (TargetData slot -1), so borrowing a hidden crate would mean re-registering it. Steering (0.10b+) remains.

## 13. Last build of this project (2026-10-06): riftbridge 0.10f

Robot helpers, all by steering and simulated hits (no Rift Apart stand-in actors):
- **Chasers** (moved onto the nearest Elden Ring enemy within 90 m, at 20 m/s for bots and 24 m/s for shots; contact hit every 0.6 s; blast when they expire): Glove of Doom bots (`GameBotDoom`), Drillhounds and pups (`ShotMotionTunneler*`), Mr. Fungi's mortars (`ShotMotion*MrFunGuy`), homing, Warmonger, rocket and missile shots.
- **Ranged** (stay put and hit the nearest enemy in reach): Mr. Fungi (22 m, every 1 s, factor 0.45) and the Topiary Sprinkler (10 m, every 1.5 s, factor 0.25).
- Only actors spawned after the first full scan and owned by Ratchet (`LogicalOwner`) count; weapon actors are excluded. Untested in-game at hand-off; 0.10e is backed up next to it.
