# AI HANDOFF: Elden Ring x Ratchet & Clank: Rift Apart mod (+ Destiny 2 Project Sunrise notes)

> **For the next AI:** this file holds everything learned so far. Read §0 first, then §2 (the rules), before you touch anything.
> The source bundle `er-ratchet-handoff-src.zip` should come with this file. It contains the current sources, headers and the RE helper scripts.
> Written 2026-10-06, at the end of the project's first stretch. The user ended the mod work "for now".

---

## 0. TL;DR

- **What it is:** you play **Elden Ring** (host) as **Ratchet**. Two games run at once. Rift Apart (guest) provides Ratchet's real movement, weapons, gadgets and HUD. Elden Ring provides the world, the enemies and the final picture. A bridge translates between them. **Nothing is re-implemented.** If you find yourself writing Ratchet's double jump inside Elden Ring, you've gone wrong.
- **State:** **playable and tested by the user.**
  - Ratchet moves on Elden Ring ground with Rift Apart's own physics.
  - Ratchet is drawn over Elden Ring through a window-capture overlay.
  - Guns, melee, grenades and Shatterbombs damage Elden Ring enemies.
  - Throwables land near and far.
  - The Glove of Doom robots chase and hurt Elden Ring enemies.
  - Bolts mirror runes.
  - Ratchet-style enemy and boss health bars.
- **Current builds:**

  | Piece | Version | md5 |
  |---|---|---|
  | `faultrep.dll` (riftbridge) | **0.10f** | `0eea3c698ea3fc980aa31b751b2b0823` |
  | `riftlink.exe` | **0.27** | `362faa8397df07152937150dbe622f8d` |

  0.10f's ranged robots (Mr. Fungi, Sprinkler) were **untested** at hand-off. 0.10e is the last version the user confirmed.
- **Paused / open:**
  - Real "stand-in" Rift Apart actors at Elden Ring enemies, so robots target them natively.
  - Damage to Ratchet from Elden Ring hits. The user switched this off.
  - frames.shm compositor and depth (the overlay is used instead).

---

## 1. The user and how to work with them

- The user tests in-game and replies "done" plus what happened. They often attach screen recordings (.mp4).
- They prefer **large rounds**: bundle fixes into one build, not many small ones.
- They care about running out of usage, so be efficient: few round-trips, no needless exploration.
- They do the clicking themselves. **Don't use computer-use / screen control unless asked** ("its too slow ill do it").
- Their PC is reached through a linked-device bridge (file read/write in connected folders). Paths are on Windows: `D:\SteamLibrary\steamapps\common\...`.
- Ask questions with short multiple-choice cards when a symptom is vague. For example, graphics → "Too dark / Blocky / Wrong colours"; hits → "Misses count / Hits don't count".

## 2. Hard rules (carry these forward)

1. **Don't break aiming or shooting.** Every change to hits or collision was checked against this.
2. **Fixed-purpose code and data-only feeds only.** No script engines, no remote-code-execution or "run arbitrary command" surfaces between the processes. The shm files carry plain data.
3. **Back up any game file before replacing it** (pattern: `faultrep.dll.0.10e-backup`, `riftlink.exe.0.26-backup`, `riftlink.ini.0.25-backup`).
4. **Elden Ring offline only** (Easy Anti-Cheat off). The launcher starts it offline.
5. Don't claim something works until the user has tested it in-game.
6. Only one agent edits the bridge or game folders at a time.
7. Platform limit: writing into `.git` folders through the remote file tools is refused. Copy working files only.
8. Patterns are pinned to **RiftApart.exe 3.0630.1.0** and **eldenring.exe 2.7.1.0 (App 1.17.1)**. Steam auto-update should stay off for both games.

---

## 3. Architecture

```
┌──────── eldenring.exe ──────────┐   bridge.shm (8 MB)   ┌──── riftlink.exe (external) ────┐  terrain.shm  ┌──── RiftApart.exe ──────────────┐
│ erbridge_core (mods\ermc-preview│ ◀──────────────────▶ │ reads RA memory (read-only)      │ ────────────▶ │ faultrep.dll = riftbridge        │
│  \erbridge_core.bin, loaded by  │  state/control/rays/  │ drives ER: stand-in Tarnished,   │  data only:   │ hooks HeroMoverManager::Update    │
│  mods\ERMinecraftPreview.dll)   │  entities/damage      │ camera, ray batches, damage      │  ground tiles,│ static hknp box bodies = ER ground│
│ camera override, stand-in,      │                       │ overlay window (WGC capture of   │  enemy boxes, │ + enemy hitboxes; hides level;    │
│ CastRay, enemy table, ModifyHp  │                       │ RA window, dark-keyed)           │ ◀──────────── │ shot/robot tracking → hits[] ring │
└─────────────────────────────────┘                       └──────────────────────────────────┘  hits ring    └───────────────────────────────────┘
```

### 3.1 Elden Ring side: erbridge_core
This is a Windows port, from an earlier Codex task, of justbustin/minecraft-crossover-bridge. It was **built for Minecraft as the guest**, and we reuse it unchanged.
- It handles game-thread tasks, camera override, a hidden stand-in Tarnished that follows the guest, and terrain rays (`CSPhysWorld::CastRay`).
- It keeps an enemy table (position, box, HP) and applies damage (`CSChrDataModule::ModifyHp`, `ChrIns::Kill`).
- It also does death sync, world-shift handling, the action key (doors, levers, Graces), and a D3D12 compositor (unused here).
- Loader: `ELDEN RING\Game\mods\ERMinecraftPreview.dll`. It stays inactive unless the environment has `ERBRIDGE=1`. The core is `mods\ermc-preview\erbridge_core.bin`.
- Protocol is `bridge_protocol.h` (in the bundle): magic `0x434D484D` "MHMC", version 1, 8 MB. Offsets:

  | Offset | Block | Written by |
  |---|---|---|
  | `0x000000` | Header | — |
  | `0x000100` | State | ER |
  | `0x000800` | Control | guest |
  | `0x000A00` | Hunter events | — |
  | `0x001000` | Cmd mailbox | debug |
  | `0x100000` | Rays + hits | — |
  | `0x200000` | Entities | ER |
  | `0x280000` | Damage queue | guest |
  | `0x300000` | Passages | — |

  Seqlocks throughout.
- Control flags used: `OVERRIDE_CAMERA`, `MOVE_HUNTER` (the stand-in follows Ratchet), `HIDE_HUNTER`.
- The runtime dir is `ELDEN RING\Game\riftbridge\runtime`, set as `ERMC_RUNTIME_DIR` by `Launch-Elden-Ring-Rift.cmd`.

### 3.2 riftlink.exe (external; `ELDEN RING\Game\riftbridge\`)
- Single instance. Starts when the user launches `Launch-Elden-Ring-Rift.cmd`, which runs ER offline with `ERBRIDGE=1`.
- Reads Rift Apart memory read-only: hero position, camera, bolts, health.
- **Mapping:** Scale 1.25 (ER metres per RA metre), FlipX=1, FacingOffsetDeg=180.
- **Ground feed:** ER ray batches → `terrain.shm` tiles. There are three rings around the Tarnished:

  | Ring | Size | Spacing | Notes |
  |---|---|---|---|
  | Fine | 25×25 | 1 m | — |
  | Far | 19×19 | 4 m | — |
  | Outer | 25×25 | 12 m | rays from +70 m down to −90 m |

  That is about 150 m reach and ~1430 tiles. Before the outer ring, long throws landed past the ~38 m edge and fell through.
- **Combat:**
  - Instant-hit guns use an aim cone: the enemy nearest the view centre within `Range` 60 and `AimConeDeg` (now **6**; the ini had 12, edited to 6).
  - Real impacts and blasts come from riftbridge's `hits[]` ring (`blast_tick`).
    - Direct hits deal `ShotDamage` (1.5).
    - Blasts deal `ExplosionDamage` (8) × falloff.
  - The cone guess is skipped for 700 ms after `shotSpawns` changes, so a tracked projectile that misses doesn't count.
  - Damage is applied through the bridge damage queue `push_damage`. Amount is in "Minecraft units", converted with `er_damage(mc, maxHp)`. A basic soldier takes about 10 shots at 3.
- **HUD:** Ratchet-style enemy and boss bars are drawn in the overlay. Boss names come from the cXXXX model table. Margit/Morgott and O'Neil/Niall share a model and are told apart by max HP. Runes are shown as bolts.
- **Overlay:**
  - Windows Graphics Capture of the RA window, shown in a click-through layered window over ER.
  - **Dark key:** Ratchet is sunk 1000 m below RA's level (Delete) into darkness, so alpha = brightness.
    - `DarkCut` 8 = transparent at or below this brightness.
    - `DarkFull` 72 = solid at or above this.
    - `AutoCut` measures the background.
  - **Brightness LUT:** levels from 0.6 × cut, then gamma 0.9 and gain 1.15. Ctrl+F9 / Ctrl+F10 adjust it, saved as `[overlay] Brightness`.
  - **`MatchSize=1`** resizes RA's window client to exactly ER's client size (1920×1080), so the copy is 1:1. This fixed "blocky" effects: they had been resampled from 1923×1057. It can't resize a maximized window, and logs that.
  - RA runs at ~40 fps beside ER. For quality, use RA's own settings: upscaler on Quality, no dynamic resolution.

**Hotkeys (riftlink):**

| Key | Action |
|---|---|
| End | Pause / resume (releases the Tarnished) |
| Insert | Overlay mode |
| Page Down | Toggle camera follow |
| Ctrl+PgUp / Ctrl+PgDn | Field of view |
| Page Up | Re-anchor |
| Ctrl+[ / Ctrl+] | Camera delay (ms) |
| Delete | Sink Ratchet 1000 m into the dark; again = deeper |
| Ctrl+F9 / Ctrl+F10 | Brightness |

**Build** (llvm-mingw at `/opt/llvm-mingw-20250924-ucrt-ubuntu-22.04-x86_64/bin`):
```
cd riftlink
x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -I src -I ../erbridge/include -o riftlink-0.27.exe src/riftlink.cpp src/wgc.o \
  -luser32 -lgdi32 -ldwmapi -ld3d11 -ldxgi -lwindowsapp -lole32 -lkernel32 -lpsapi
```
`-lkernel32` must come before `-lpsapi`, or `K32EnumProcessModulesEx` fails to link. `wgc.o` is built from `wgc.c` (Windows Graphics Capture glue).

**riftlink.ini keys:**

| Section | Keys |
|---|---|
| `[bridge]` | RuntimeDir, HideTarnished=1, RateHz=120 |
| `[mapping]` | Scale=1.25, HeightScale, FlipX=1, FlipZ, SwapXZ, YawOffsetDeg, FacingOffsetDeg=180 |
| `[camera]` | Follow=1, TrueCamera=1, AutoFov=1, TargetHeight, FovDeg=0, DelayMs |
| `[overlay]` | KeyTolerance, KeyDark=1, DarkCut=8, DarkFull=72, AutoCut=1, MatchSize=1, Brightness |
| `[terrain]` | Enabled=1, GridSize=25, Spacing=1.0, ResampleDist=2.5, SinkDepth=1000 |
| `[combat]` | Enabled, ShotDamage=1.5, ShotsPerSecond=5, MeleeDamage=6, Range=60, AimConeDeg=6, MeleeRange=3, FireKey=1, MeleeKey=70, ProjectileSpeed=40, ExplosionDamage=8, TakeDamage (off), MinHealth |
| `[hud]` | HealthBars, BarsMirror, BossName, BossMinHp, ShowRunes |
| `[input]` | Controller |

### 3.3 riftbridge = faultrep.dll (inside Rift Apart; `Ratchet & Clank - Rift Apart\faultrep.dll`)
- Proxy DLL: it exports `ReportFault` and passes it through to the real `System32\faultrep.dll`. RA has no anti-cheat.
- **Off switch:** `riftbridge\off.txt`, or delete `faultrep.dll`.
- Log: `riftbridge\rift-bridge3.log`. A crash logger (vectored exception handler) logs module+offset.
- Hooks `HeroMoverManager::Update` (RVA `0x72faa0`; head bytes are checked first, then a trampoline is used) for per-frame game-thread work.
- **Input from the ER window:**
  - IAT hooks make RA believe it is focused (`GetForegroundWindow`, `GetActiveWindow` and `GetFocus` answer with RA's own window).
  - Raw input uses `RIDEV_INPUTSINK`.
  - Losing-focus messages are held back, and `ClipCursor` / `SetCursorPos` are skipped.
  - A controller is turned into keyboard and mouse input via `riftbridge\controller.ini` (Y=`WHEELUP|MMB` tap/hold syntax).
- **ER ground:** a pool of static hknp box bodies placed on the terrain.shm tiles (`createBody` / `addBodies`, then `setBodyTransform`). `tile_class` half-extent must be under 6.
  - **Body flags** `002b0000` at body+0x68 (the level's flag bits) are required, or hitscan guns pass through.
  - Filter `xxx8243f` on walls makes the camera bump into our bodies.
  - Ctrl+Pause cycles the body variants (`bodyvariant10.txt`). Variant 0 = no owner + hit-object fix. This is the one in use.
- **Hides RA's level:** zone visibility ops (Delete / Shift+Delete). RA's own static collision switches off by itself once ER ground arrives (Pause toggles it manually).
- Infinite ammo uses the game's own flag at `0x5166064`. The bolt counter is set to the Tarnished's runes.
- **Shots thread:**
  - Scans the entity table every 60 ms and follows shots every 10 ms.
  - Writes hits into terrain.shm `hits[64]` and bumps `shotSpawns`.
- **Robots** are steered and given simulated hits (§5.4).
- Hotkeys: F9 status line (also every 5 s), F8 actor survey (research), Home test platform / mouse-camera speed, Pause, Ctrl+Pause, Delete.

**Build:**
```
cd riftbridge
x86_64-w64-mingw32-g++ -std=c++17 -O2 -shared -static -I src -o v03/faultrep-0.10f.dll src/riftbridge3-10f.cpp -luser32 -lpsapi
```
This reproduces the shipped size, 737,792 bytes. `src/rift_terrain.h` must match `riftlink/src/rift_terrain.h`; the master copy is `shared/`.

### 3.4 terrain.shm (`rift_terrain.h`, RTER_VERSION 4)
riftlink writes:
- the header;
- ground tiles (position, half-extents, class);
- enemy boxes (id, centre, half-size; used for direct and blast hits);
- the anchor id. All tiles move when this changes.

riftbridge writes, after the pad fields:
```c
volatile uint32_t hitTag; volatile uint32_t hitWrite;
struct RtHit { uint32_t id; float factor; uint32_t kind; float pos[3]; } hits[64];   // ring
volatile uint32_t shotSpawns;
#define RT_HIT_DIRECT 0x80000000u   // kind bit: direct impact (ShotDamage) vs blast (ExplosionDamage x factor)
```

---

## 4. Install layout on the user's PC (with backups)

| Path | Contents |
|---|---|
| `D:\SteamLibrary\steamapps\common\ELDEN RING\Game\riftbridge\` | `riftlink.exe` (0.27) plus backups `riftlink.exe.0.23/.0.24/.0.25/.0.26-backup`, `riftlink.ini` plus `riftlink.ini.0.25-backup`, `Launch-Elden-Ring-Rift.cmd`, `README.txt`, `riftlink.log`, `runtime\` (bridge.shm lives here) |
| `ELDEN RING\Game\mods\` | `ERMinecraftPreview.dll` (loader; a `.ratchetmode-off` copy is re-enabled by the launcher), `ermc-preview\erbridge_core.bin` |
| `D:\SteamLibrary\steamapps\common\Ratchet & Clank - Rift Apart\` | `faultrep.dll` (0.10f) |
| `...Rift Apart\riftbridge\` | Backups `faultrep.dll` 0.7d, 0.8a, 0.8d, 0.9b, 0.9f, 0.10e; `controller.ini`, `bodyvariant10.txt`, `terrain.shm`, `rift-bridge3.log` |

**Launch order:**
1. Steam running.
2. Run `Launch-Elden-Ring-Rift.cmd` (ER offline) and load the save.
3. Start Rift Apart. faultrep.dll loads automatically.
4. riftlink starts and links. Wait for "LINKED" and the first terrain line.
5. Press **Delete** to sink Ratchet into the dark and play from the ER window.

Earlier work (Codex), for reference only:
- `Documents\Codex\2026-10-03\can-you-make-a-mass-up\outputs\Minecraft-EldenRing-Windows` — the erbridge Windows port.
- `Documents\Codex\2026-10-04\i-x20\outputs\Rift-Elden-Bridge` — the first RA research.
- `Documents\SkyCraft-EldenRing-Bridge-v0.0.2` — superseded.

---

## 5. Rift Apart reverse-engineering facts (RiftApart.exe 3.0630.1.0, image base 0x140000000; all RVAs)

### 5.1 Hero, camera, inventory

**Hero chain:** `HeroLocal` (vtable `0x2f48188`) → +0x90 `HeroPointerCache` (vtable `0x2f4c0b8`). From the cache: `HeroHealth`, `GameInventoryManager`, `HeroWeaponManagerLocal`, `PhysicsComponent`, `HeroMoverManager` (vtable `0x2f5a328`), `HeroMoverStandard`, `ClankPack`.
- Re-find through vtable/RTTI every session. Heap objects move.

**Position:** `HeroMoverManager + 0xC8` (XYZ, Y up). +0xFC = next position, +0x114 = step.
- **Use the `self` of every Update call** (`g_moverLive`). The first-call mover can be a loading-time object reading 0,0,0.

**Camera:** the rendered camera is a ring of plain 4×4 matrix blocks, 0x100 apart.
- Axes at +0, position at +0x30, forward = axis 2.
- Pick a copy by continuity and force up = (0,1,0).
- Quaternion readings and the HeroCameraManager "ideal eye" give wrong pictures.

**Bolts (GetCurrencyAction chain):**
1. Player slots `[0x68d3e78]`: 16 slots, handle at +28 + 20·i.
2. Handle → entity: `0xf1a520`.
3. Component lookup with key `0x60313c0` (map find `0xffb860` / `0xffca00`).
4. `0x86a660` (GetInventory).
5. vf+0x198(`[0x60311a8]`).
6. vf+0x108.
7. Value at +0x38.

**Other RVAs:**

| RVA | What |
|---|---|
| `0x5166064` | Infinite-ammo flag |
| `0x68daef0` | Zone system pointer |
| `0x1923c00` | Zone visibility manager |
| `0x1923a00` | Zone op 3 (mgr, u16* zones, count; 0 = all) |
| `0x1924cf0` | Zone op 2 |
| `0x4956af8` | World vtable |
| `0x32e49b0` | Physics system vtable |

### 5.2 Havok (hknp) bodies

| RVA | Function |
|---|---|
| `0x2933480` | `hknpConvexShape::BuildConfig` ctor |
| `0x2933fc0` | `createBoxShape(half, radius, cfg)` |
| `0x293efb0` | `hknpWorld::createBody(out, cinfo, ...)` |
| `0x293e390` | `addBodies` |
| `0x2939ea0` | `setBodyTransform(id, xf, activation)` |

**InsomniacCollisionFilter** (vtable `0x32e50e0`):
- Slot 3 `0x1198430` = filterBodyPairs (groups at body+0x98).
- Slot 4 = shape-key query.
- Slot 5 `0x1198900` = by body id.
- Slot 6 `0x11988d0` = query vs body.

Filter word layout = `[13-bit group][7][layer 4][mask 12]`. Query classes `xxxx1880` / `xxxx1900` (mask 0x80 / 0x100) reject our bodies and almost all level bodies too, so they are not the cause of anything.

**IgCollisionQueryHitCollector** (vtable `0x32e81a8`): addHit is slot 4, `0x11946f0`; body id at hit+0x48. The cached resolver is `0x11b1290(0x5324a00, out, &bodyId, key)`.

**Body owner** (`0x11b9890`):
- If the low dword of body+0x98 has `& 0xffff0000 == 0xffff0000`, it is a zone index (low16) → `0x13d2920(0x54a8a30, idx)`.
- Otherwise the low dword is an actor handle (dynamic bodies use `0x02xxxxxx`).

### 5.3 The fix that made throwables land (key discovery)
Shot motions (GrenadeShotMotion etc.) process each hit-result entry with three accessors:
- `0x11ab3a0` — flags at entry+4; mask 0x100.
- **`0x11ab4e0`** — handle at entry+8 (top byte = type) → object via `0x12b64e0(rcx = &handle, edx = idx, r8d = type)`.
  - Handle table `[0x6747ce0]`: 16-byte entries (ptr, type byte at +8). Count at `[0x6747cfc]`.
- `0x11ab290` — actor. Returns null if the object has `[+0x5c] & 0x800`.

Our bodies have handle 0, so **grenades silently discarded every hit on our ground**.

**The fix:**
- Patch `0x11ab4e0` with one **8-byte atomic write** (expected original bytes `48 63 51 08 48 8b 01 48`).
- The patch is a rel32 jmp to a nearby trampoline (`FF 25` absolute jmp) → `hk_hit_object`.
- For handle-0 hits, `hk_hit_object` returns a static stand-in object with `+0x5c = 0x800` ("not an actor"). The shot then takes its normal world-hit path.

This works anywhere and needs no RA zone. Calling `0x12b64e0` with garbage in r8 returns "object 0", so read the table directly (`handle_ok`).

**What failed first (don't repeat):**
- Changing the filter group (0.8a) and group-none (0.8b) didn't help.
- Borrowing a level body's owner handle (0.8d–0.9b) broke as zones streamed out.
- Re-resolving freed bodies crashed at `RiftApart+0x11b963c`: `0x11c31f0` on a stale body, shape vtable = hkBaseObject.

### 5.4 Entities, shots, robots

**Entity table** `[0x6748978]`, count `[0x6748994]`:
- Records are 0xC0 bytes; generation at +8; `handle = (gen << 20) | i`.
- Components: hash map at rec+0x80 (count +0x88, values at +k·16+8) and list at rec+0x68 (count +0x70).
- **Actor position = `[record+0]+0x30`** (actor transform). 0.10a's auto-learner wrongly picked `[rec+0x60]+0x180`, which is hero-only and starts as NaN, so this is hardcoded after 5 matches against the hero.

**Shots** are actors with a `*ShotMotion*` component:
- `ShotMotionTunneler` / `TunnelerPuppy` (Drillhound)
- `LightningRod`
- `Ricochet`
- `BuzzBladesShotMotion`
- `ShotMotionBouncer` (Shatterbomb)
- `ShotMotionProtoShotgun` (Enforcer)
- `ShotMotionCarpetBomber`
- `ShotMotionMrFunGuy`
- `ShotMotionHomingLobMrFunGuy`

Ratchet's things carry `LogicalOwner`.

**Shot tracking:**
- Ignore origin (0,0,0) samples. Ignore single >40 m jumps (spawn/death junk), but accept the jump after 4 rejects.
- **Explosive** = the name contains any of: Grenade, Bouncer, CarpetBomb, Tunneler, Bomb, Mortar, Rocket, Missile, Homing, Corkscrew, Arc, Gravity, Explo, Mine, Warmonger, Dispersing.
- When an explosive shot disappears, it blasts at its last position. Radius: 5 m for CarpetBomb/Bouncer/Grenade, 3.5 m for Tunneler, 4 m otherwise. Falloff applies; only within 250 m of Ratchet.
- When a non-explosive shot ends within 1.2 m of an enemy box, it is a direct hit (`RT_HIT_DIRECT`).
- Result: a Shatterbomb series killed a 2169 HP enemy in 8 bombs.

**Robots (0.10f):**
- **Classes:** `GameBotDoom` (Glove of Doom), `GameBotMrFunGuy` (Mr. Fungi), `TopiarySprinkler`.
- **Name words:** GameBot, DoomBot, BotDoom, GloveOfDoom, Tunneler, FunGuy, Fungi, Drone, Minion, Puppy, Homing, Corkscrew, Seeker, Swarm, Glitch, TopiarySprinkler, Warmonger, Missile, Rocket.
- **Which count:** only spawns after the first full scan, with LogicalOwner, and not a Weapon.
- **Chasers:**
  - Steered toward the nearest ER enemy within 90 m, at 20 m/s (bots) or 24 m/s (shots).
  - Steering writes the actor transform plus up to 16 matching position copies found in their components.
  - Contact hit every 0.6 s: factor 0.35 for bots, 0.25 for shots.
  - Blast on expiry: non-shots, within 4 m.
- **Ranged** (stay put):
  - Mr. Fungi: 22 m, every 1 s, factor 0.45.
  - Sprinkler: 10 m, every 1.5 s, factor 0.25.
- `robots_frame()` runs on the game thread.
- Tested: Glove of Doom works. Mr. Fungi and Sprinkler were untested.

### 5.5 Native targeting research (paused; read before trying "real stand-in actors")
- **DoomBotTargeting** (vtable `0x2ea7a10`, derived from `TieredAggroTargeting`) iterates awareness entries:
  - +0xa0 = position;
  - +0x6c = actor handle;
  - state byte > 2 = aware.
  - Entries whose handle doesn't resolve are dropped, so **targets must be real actors**.
- **TargetData@Target** (vtable `0x4625338`) registers in the global registry `0x580eed0`:
  - slot index at +0x1a0;
  - spatial grid `0x580fb20`;
  - per-team list chosen by +0x48;
  - flag lists from the bits of +0x4c;
  - position at +0x180 (`0x1b4efc0` returns arg+0x180).
  - Activate = vf7 `0x1b4e3a0`; deactivate = vf8 `0x1b4e3d0`.
- Observed values:
  - crates: team 0, flags 0x40000;
  - breakables: flags 0x10000;
  - NPCs ("LookAtHero Dialog"): team 7.
  - Actors in hidden zones are **unregistered** (slot −1). Borrowing a hidden crate as a stand-in would mean re-registering it (vf7) and finding the enemy team value. Team semantics are still unclear.
- Other classes:

  | Class | Vtable |
  |---|---|
  | DoomBotAwareness | `0x2ea70e0` |
  | AwarenessDB | `0x45966a0` |
  | SimpleAllegiance | `0x46251d0` |
  | Faction | `0x312ce90` |
  | MrFunGuyAwareness | `0x300cbf0` |

### 5.6 Elden Ring facts used
- **Runes:** GameDataMan via pattern `48 8B 05 ?? ?? ?? ?? 48 85 C0 74 05 48 8B 40 58 C3 C3` → +0x08 PlayerGameData → runes +0x6C, level +0x68.
- Everything else goes through erbridge_core (CastRay, ModifyHp, Kill, camera, stand-in).

### 5.7 Crashes seen
- `NRI.dll+412e1` and `RiftApart.exe+16aeb23` at exit are the game shutting down. They are harmless.
- `RiftApart+0x11b963c` came from re-resolving stale bodies. That code is now disabled.

---

## 6. Version history (short)

| Ver | What changed / learned |
|---|---|
| riftbridge 0.2–0.5 | Mover hook, terrain.shm, static box bodies; Ratchet walks on ER ground |
| 0.6c–0.6j | Play from the ER window (IAT focus hooks, raw input sink), controller → keys |
| 0.7–0.7d | Body flags `002b0000` (hitscan hits our bodies), level collision auto-off, infinite ammo, bolts = runes |
| 0.8a–0.8d | Throwables falling through: filter/group experiments (no help), borrowed owners (stale) |
| 0.9a–0.9b | Borrow refresh → crash on stale bodies |
| **0.9c** | **Hit-object accessor patch → throwables land** |
| 0.9d–0.9f | Shot tracking + blast damage, `g_moverLive`, origin/jump filters; refresh off for variant 0 |
| 0.10a–0.10b | Robots: name words, position = `[rec+0]+0x30`, steering + contact damage (Glove of Doom works) |
| 0.10c–0.10d | TargetData survey (research logging) |
| **0.10e** | Direct hits via real impacts, `shotSpawns` (user: "works better now") |
| **0.10f** | All robot helpers: ranged Mr. Fungi / Sprinkler, more chaser types |

| Ver | What changed / learned |
|---|---|
| riftlink 0.22–0.24 | Camera ring, bars, boss names |
| 0.25 | Far ring, blast damage |
| 0.26 | Outer ring (±150 m) |
| **0.27** | Cone skip after spawns, AimConeDeg 6, brightness LUT, MatchSize |

## 7. Open items / ideas for the next AI
1. Get the user's test result for 0.10f's ranged robots (Mr. Fungi, Sprinkler). If they are bad, 0.10e is backed up.
2. **Native robot targeting:** re-register a TargetData (vf7) on a borrowed actor and set an enemy team. Or create a minimal actor. Move it onto each ER enemy, and forward its health loss to `push_damage`. Research is in §5.5.
3. Damage to Ratchet from ER hits (`combat.TakeDamage`, `MinHealth` exist). The user had it off.
4. Real compositor (frames.shm + depth), instead of the window-capture overlay.
5. Hidden RA window, auto-launch, fast travel and loading handling.

---

## 8. Tooling and environment lessons

- **Cross-compile:** llvm-mingw `x86_64-w64-mingw32-g++` (Linux container → Windows x64 PE).
- **Decompiler:** radare2 6.2.4 + r2ghidra, built from git in `/home/claude/tools`. Only the x86 processor is enabled in `ghidra-processors.txt`.
  - `re/dec.sh <rva>...` decompiles single functions in ~10 s each, with no whole-program analysis (the container has 2 cores and 7 GB; full Ghidra analysis is too big).
  - Script:
    ```sh
    export PATH=/home/claude/tools/r2/bin:$PATH LD_LIBRARY_PATH=/home/claude/tools/r2/lib
    cmds=""; for a in "$@"; do cmds="$cmds s $((0x140000000 + $a)); af; echo ===== $a; pdg;"; done
    cd "$(dirname "$0")" && timeout 900 r2 -q -e bin.cache=true -e scr.color=0 -c "$cmds" RiftApart.exe
    ```
  - `re/pe.py` (Python, in the bundle) provides PE helpers: `dis`, `xrefs`, `callers`, `func_of`, `primary_func`, `rtti_vtables`, `vtable_for_col`. It reads MSVC RTTI from RiftApart.exe. `cols.pkl` is a cache.
- **Network:** the container proxy allows git clone and PyPI. GitHub *release downloads* and apt return 403. Build from source instead.
- **User's helper repos** (cloned and reviewed, not installed): `github.com/rehan-remade/universal-modder`, `github.com/morluto/rea`.
- **Logs to ask for:** `Rift Apart\riftbridge\rift-bridge3.log`, `ELDEN RING\Game\riftbridge\riftlink.log`. Long lines are cut at 4096; long reports use `rawf`.

---

## 9. Second topic: Destiny 2 "Project Sunrise" (started 2026-10-06)

- **Guide:** https://projectsunrise.dev/guides/installing/. It uses DepotDownloader to fetch an old Destiny 2 build:
  - App **1085660**;
  - depot **1085661**, manifest **7180122903232116872**;
  - depot **1085662**, manifest **2210332166360342287**.

  Command shape:
  ```
  DepotDownloader -app 1085660 -depot <depot> -manifest <manifest> -username <steam user> -remember-password -dir D:\Destiny
  ```
  The user enters their own Steam login and Steam Guard code themselves. The account must own Destiny 2.
- **Install folder:** `D:\Destiny\bin\x64`.
- **Done:** the SunriseMissions scripts (`github.com/stanuwu/SunriseMissions`, commit f317a32) were copied into `D:\Destiny\bin\x64\Sunrise\scripts`: 63 files. `.git` was left out because of the platform limit, so `git pull` won't work there. Re-clone on the PC to update.
- **Problem seen:** `bin\x64` held only 4 DLLs (`GFSDK_Aftermath_lib`, `oo2core_3_win64`, `steam_api64`, `steamnetworkingsockets`) and **no `destiny2.exe`**. The depot download looks incomplete: re-run both depots fully.
- **Remaining steps for the user:** finish the downloads, then swap in the Sunrise `steam_api64.dll` as the guide says (back up the original first).

---

## 10. Source bundle contents (`er-ratchet-handoff-src.zip`)
- `riftbridge/riftbridge3-10f.cpp`, `riftbridge/riftbridge3-10e.cpp` — RA DLL, current and last-confirmed.
- `riftlink/riftlink.cpp`, `wgc.c`, `wgc.h` — external link and overlay.
- `shared/rift_terrain.h` — terrain.shm layout.
- `erbridge/include/bridge_protocol.h` — bridge.shm layout.
- `re/dec.sh`, `re/pe.py` — RE helpers. RiftApart.exe itself is not included; take it from the game folder.
- `riftbridge/controller.ini`, `riftlink/riftlink.ini` (example), `Launch-Elden-Ring-Rift.cmd`.
- `DESIGN.md` — the original design doc with dated as-built sections.
