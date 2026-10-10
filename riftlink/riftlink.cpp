// riftlink: step 1 of the Elden Ring x Rift Apart bridge.
//
// Reads Ratchet's live position out of RiftApart.exe (read-only, no injection) and drives the
// Elden Ring bridge (erbridge_core, from minecraft-crossover-bridge) exactly the way the Minecraft
// mod did: it writes ErmcControl.hunterPos/hunterYawDeg with MOVE_HUNTER, so the hidden
// "stand-in" Tarnished is pinned wherever Ratchet is.
//
// Object chain (RiftApart.exe 3.0630.1.0, found by Codex's read-only research):
//   HeroLocal          vtable RVA 0x2f48188
//     +0x90 -> HeroPointerCache   vtable RVA 0x2f4c0b8
//       +0xE0 -> HeroMoverManager vtable RVA 0x2f5a328
//         +0xC8  float[3] position (Y up)
// Every vtable is checked against the executable's RTTI names before use, so a different
// Rift Apart build refuses to run instead of reading garbage.
//
// Keys (work from either game window):
//   PgUp  re-anchor: the Tarnished stays where it is, Ratchet's current spot becomes "here"
//   End   pause / resume the link (the Tarnished is released after one second)
//
// Config: riftlink.ini next to the exe.

#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <algorithm>
#include <time.h>
#include "bridge_protocol.h"
#include "rift_terrain.h"
#include "wgc.h"
#include <dwmapi.h>

static const uint32_t RVA_HERO_LOCAL = 0x2f48188;
static const uint32_t RVA_PTR_CACHE = 0x2f4c0b8;
static const uint32_t RVA_MOVER_MGR = 0x2f5a328;
static const uint32_t OFF_CACHE = 0x90;
static const uint32_t OFF_MOVER = 0xE0;
static const uint32_t OFF_POS = 0xC8;

// ------------------------------------------------------------------------------------------
// Config and log

struct Config {
    char runtimeDir[MAX_PATH];
    float scale = 1.0f;          // Elden Ring metres per Rift Apart unit
    int flipX = 0, flipZ = 0;    // mirror an axis
    int swapXZ = 0;
    float yawOffsetDeg = 0.0f;   // rotate Ratchet's movement before applying it in Elden Ring
    float facingOffsetDeg = 0.0f; // turn the Tarnished's facing (180 = it faced backwards)
    float heightScale = 1.0f;    // vertical movement multiplier
    int hideTarnished = 0;       // 1: hide the stand-in (for later, once Ratchet is drawn in ER)
    int cameraFollow = 1;        // Elden Ring's camera = Rift Apart's camera
    float camTargetHeight = 1.0f; // look this far above Ratchet's feet
    float camFovDeg = 0.0f;      // 0 = keep Elden Ring's field of view
    int keyTolerance = 60;       // overlay cut-out (corner mode): how close to the background colour counts as background
    int keyDark = 1;             // overlay cut-out: 1 = remove dark pixels (Ratchet in the dark), 0 = corner colour
    int darkCut = 8;             // dark mode: pixels up to this brightness (0-255) are removed
    int darkFull = 72;
    int keyAuto = 1;             // raise DarkCut to just above the measured background brightness           // ... and from this brightness up fully solid; in between, see-through (glows, smoke)
    int rateHz = 120;
    int terrain = 1;             // feed Elden Ring's ground to Rift Apart (riftbridge 0.2)
    int gridSize = 25;           // terrain grid: gridSize x gridSize points around the Tarnished
    float gridSpacing = 1.0f;    // metres between points (Rift Apart tiles are 1.2 m wide)
    float resampleDist = 2.5f;   // resample after the Tarnished moves this far
    float sinkDepth = 300.0f;    // Delete: drop Ratchet this far below Rift Apart's level (dark backdrop)
    int autoFov = 1;
    int healthBars = 1, barsMirror = 0, bossHp = 1200;  // [hud] HealthBars, BarsMirror, BossMinHp
    char bossName[64] = "";  // [hud] BossName (label over the boss bar)
    int padToRift = 1;           // [input] Controller: pass the controller to Rift Apart
    float damageScale = 1.0f;    // [combat] TakeDamage scale: Elden Ring hits on the stand-in -> Ratchet (0 = off)
    int minHp = 0;               // [combat] MinHealth: Ratchet's health never goes below this (0 = hits can kill)
    int showRunes = 1;           // [hud] ShowRunes: Ratchet's bolt counter shows the Tarnished's runes
    int camDelayMs = 20;         // delay Elden Ring's camera so it matches the overlay picture's age             // match Elden Ring's field of view to Rift Apart's
    int trueCamera = 1;          // use Rift Apart's real camera (position + view direction), not the look-at point
    int combat = 1;              // Ratchet's shots damage Elden Ring enemies
    float shotDamage = 3.0f, shotsPerSec = 5.0f, range = 60.0f, aimConeDeg = 12.0f;
    float meleeDamage = 6.0f, meleeRange = 3.0f;
    int fireKey = VK_LBUTTON, meleeKey = 'F';
    float projSpeed = 40.0f;     // shots land after distance / speed (metres per second), like real projectiles
    float blastDamage = 8.0f;    // [combat] ExplosionDamage: a bomb/grenade/rocket going off right on an enemy
    float brightness = 1.15f;    // [overlay] Brightness: Ratchet is lit by the dark under the level; this lifts him
    float gammaLift = 0.9f;     // [overlay] Gamma: below 1 brightens shadows more than highlights
    int matchSize = 1;           // [overlay] MatchSize: resize Rift Apart's window to Elden Ring's size (sharp, 1:1)
};
static Config g_cfg;
static FILE* g_log = nullptr;
static char g_exeDir[MAX_PATH];

static void logf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    SYSTEMTIME t;
    GetLocalTime(&t);
    printf("\r%-110s\r[%02d:%02d:%02d] %s\n", "", t.wHour, t.wMinute, t.wSecond, buf);
    if (g_log) {
        fprintf(g_log, "[%02d:%02d:%02d.%03d] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf);
        fflush(g_log);
    }
}

static void load_config() {
    char ini[MAX_PATH];
    snprintf(ini, sizeof(ini), "%s\\riftlink.ini", g_exeDir);
    char def[MAX_PATH];
    snprintf(def, sizeof(def), "%s\\runtime", g_exeDir);
    GetPrivateProfileStringA("bridge", "RuntimeDir", def, g_cfg.runtimeDir, MAX_PATH, ini);
    char v[64];
    auto f = [&](const char* sec, const char* key, float d) {
        char dd[32];
        snprintf(dd, sizeof(dd), "%g", d);
        GetPrivateProfileStringA(sec, key, dd, v, sizeof(v), ini);
        return (float)atof(v);
    };
    g_cfg.scale = f("mapping", "Scale", 1.0f);
    g_cfg.heightScale = f("mapping", "HeightScale", 1.0f);
    g_cfg.yawOffsetDeg = f("mapping", "YawOffsetDeg", 0.0f);
    g_cfg.facingOffsetDeg = f("mapping", "FacingOffsetDeg", 0.0f);
    g_cfg.flipX = GetPrivateProfileIntA("mapping", "FlipX", 0, ini);
    g_cfg.flipZ = GetPrivateProfileIntA("mapping", "FlipZ", 0, ini);
    g_cfg.swapXZ = GetPrivateProfileIntA("mapping", "SwapXZ", 0, ini);
    g_cfg.hideTarnished = GetPrivateProfileIntA("bridge", "HideTarnished", 0, ini);
    g_cfg.rateHz = GetPrivateProfileIntA("bridge", "RateHz", 120, ini);
    g_cfg.cameraFollow = GetPrivateProfileIntA("camera", "Follow", 1, ini);
    g_cfg.camTargetHeight = f("camera", "TargetHeight", 1.0f);
    g_cfg.camFovDeg = f("camera", "FovDeg", 0.0f);
    g_cfg.keyTolerance = GetPrivateProfileIntA("overlay", "KeyTolerance", 60, ini);
    g_cfg.keyDark = GetPrivateProfileIntA("overlay", "KeyDark", 1, ini);
    g_cfg.darkCut = GetPrivateProfileIntA("overlay", "DarkCut", 8, ini);
    g_cfg.darkFull = GetPrivateProfileIntA("overlay", "DarkFull", 72, ini);
    g_cfg.keyAuto = GetPrivateProfileIntA("overlay", "AutoCut", 1, ini);
    if (g_cfg.darkFull <= g_cfg.darkCut) g_cfg.darkFull = g_cfg.darkCut + 1;
    g_cfg.terrain = GetPrivateProfileIntA("terrain", "Enabled", 1, ini);
    g_cfg.gridSize = GetPrivateProfileIntA("terrain", "GridSize", 25, ini);
    g_cfg.gridSpacing = f("terrain", "Spacing", 1.0f);
    g_cfg.resampleDist = f("terrain", "ResampleDist", 2.5f);
    g_cfg.sinkDepth = f("terrain", "SinkDepth", 300.0f);
    g_cfg.trueCamera = GetPrivateProfileIntA("camera", "TrueCamera", 1, ini);
    g_cfg.autoFov = GetPrivateProfileIntA("camera", "AutoFov", 1, ini);
    g_cfg.camDelayMs = GetPrivateProfileIntA("camera", "DelayMs", 0, ini);
    g_cfg.showRunes = GetPrivateProfileIntA("hud", "ShowRunes", 1, ini);
    {
        char b[32];
        GetPrivateProfileStringA("combat", "TakeDamage", "0", b, sizeof(b), ini);
        g_cfg.damageScale = (float)atof(b);
        g_cfg.minHp = GetPrivateProfileIntA("combat", "MinHealth", 1, ini);
        g_cfg.padToRift = GetPrivateProfileIntA("input", "Controller", 1, ini);
    }
    g_cfg.healthBars = GetPrivateProfileIntA("hud", "HealthBars", 1, ini);
    g_cfg.barsMirror = GetPrivateProfileIntA("hud", "BarsMirror", 0, ini);
    g_cfg.bossHp = GetPrivateProfileIntA("hud", "BossMinHp", 1200, ini);
    GetPrivateProfileStringA("hud", "BossName", "", g_cfg.bossName, sizeof(g_cfg.bossName), ini);
    g_cfg.combat = GetPrivateProfileIntA("combat", "Enabled", 1, ini);
    g_cfg.shotDamage = f("combat", "ShotDamage", 3.0f);
    g_cfg.shotsPerSec = f("combat", "ShotsPerSecond", 5.0f);
    g_cfg.range = f("combat", "Range", 60.0f);
    g_cfg.aimConeDeg = f("combat", "AimConeDeg", 6.0f);
    g_cfg.meleeDamage = f("combat", "MeleeDamage", 6.0f);
    g_cfg.meleeRange = f("combat", "MeleeRange", 3.0f);
    g_cfg.fireKey = GetPrivateProfileIntA("combat", "FireKey", VK_LBUTTON, ini);
    g_cfg.meleeKey = GetPrivateProfileIntA("combat", "MeleeKey", 'F', ini);
    g_cfg.projSpeed = f("combat", "ProjectileSpeed", 40.0f);
    g_cfg.blastDamage = f("combat", "ExplosionDamage", 8.0f);
    g_cfg.brightness = f("overlay", "Brightness", 1.15f);
    g_cfg.gammaLift = f("overlay", "Gamma", 0.9f);
    g_cfg.matchSize = GetPrivateProfileIntA("overlay", "MatchSize", 1, ini);
    if (g_cfg.shotsPerSec < 0.5f) g_cfg.shotsPerSec = 0.5f;
    if (g_cfg.gridSize < 5) g_cfg.gridSize = 5;
    if (g_cfg.gridSize > 45) g_cfg.gridSize = 45;  // 45*45 = 2025 tiles <= RT_MAX_TILES
    if (g_cfg.gridSpacing < 0.3f) g_cfg.gridSpacing = 0.3f;
    if (g_cfg.rateHz < 20) g_cfg.rateHz = 20;
    if (g_cfg.rateHz > 240) g_cfg.rateHz = 240;
}

static uint64_t now_ms() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000ull) / 10000ull;
}

// ------------------------------------------------------------------------------------------
// Shared memory with erbridge_core (same file the Minecraft mod mapped)

static uint8_t* g_shm = nullptr;
static ErmcHeader* hdr() { return (ErmcHeader*)(g_shm + ERMC_OFF_HEADER); }
static ErmcGameState* est() { return (ErmcGameState*)(g_shm + ERMC_OFF_STATE); }
static ErmcControl* ctl() { return (ErmcControl*)(g_shm + ERMC_OFF_CONTROL); }

static bool shm_open() {
    CreateDirectoryA(g_cfg.runtimeDir, nullptr);
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\bridge.shm", g_cfg.runtimeDir);
    HANDLE f = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        logf("ERROR: can't open %s (%lu)", path, GetLastError());
        return false;
    }
    HANDLE m = CreateFileMappingA(f, nullptr, PAGE_READWRITE, 0, ERMC_SHM_SIZE, nullptr);
    if (!m) {
        logf("ERROR: CreateFileMapping failed (%lu)", GetLastError());
        return false;
    }
    g_shm = (uint8_t*)MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, ERMC_SHM_SIZE);
    if (!g_shm) {
        logf("ERROR: MapViewOfFile failed (%lu)", GetLastError());
        return false;
    }
    logf("bridge file: %s", path);
    return true;
}

// The bridge file outlives Elden Ring, so "ready" also needs Elden Ring's frame counter to be moving.
static bool host_ready() {
    if (hdr()->magic != ERMC_MAGIC || hdr()->version != ERMC_VERSION) return false;
    static uint64_t lastBeat = 0, lastChange = 0;
    uint64_t b = hdr()->hostHeartbeat, now = now_ms();
    if (b != lastBeat) lastBeat = b, lastChange = now;
    return now - lastChange < 3000;
}

static bool read_state(ErmcGameState* out) {
    ErmcGameState* s = est();
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = s->seq;
        MemoryBarrier();
        if (s1 & 1) {
            YieldProcessor();
            continue;
        }
        memcpy(out, (const void*)s, sizeof(*out));
        MemoryBarrier();
        if (s->seq == s1) return s1 != 0;
    }
    return false;
}

static uint64_t g_mcFrame = 0;
static float g_camUp[3] = {0, 1, 0};
static float g_fovLive = 0, g_erFov = 0;
static int g_camDelay = 0;  // ms, live-tunable with Ctrl+[ / Ctrl+]
static volatile float g_raAspect = 0;  // Rift Apart window width / height  // live FOV tuning; Elden Ring's own FOV from its state
static void write_control(uint32_t flags, const float pos[3], float yawDeg, const float* camPos = nullptr,
                          const float* camTgt = nullptr, float fov = 0) {
    ErmcControl* c = ctl();
    uint32_t s = c->seq;
    if (s & 1) s++;  // left odd by a crashed writer
    c->seq = s + 1;  // odd: writing
    MemoryBarrier();
    c->flags = flags;
    c->mcFrame = ++g_mcFrame;
    if (pos) memcpy(c->hunterPos, pos, 12);
    c->hunterYawDeg = yawDeg;
    if (camPos && camTgt) {
        memcpy(c->camPos, camPos, 12);
        memcpy(c->camTarget, camTgt, 12);
    } else {
        c->camPos[0] = c->camPos[1] = c->camPos[2] = 0;
        c->camTarget[0] = c->camTarget[1] = c->camTarget[2] = 0;
    }
    memcpy(c->camUp, g_camUp, 12);
    c->fovYDeg = fov;
    c->poseLag = 0;
    c->depthIndex = 0;
    c->lightGain = c->lightMin = c->fogStrength = 0;
    MemoryBarrier();
    c->seq = s + 2;
}

// ------------------------------------------------------------------------------------------
// Rift Apart process (read-only)

static HANDLE g_ra = nullptr;
static DWORD g_raPid = 0;
static uint64_t g_raBase = 0, g_raSize = 0;

static bool rd(uint64_t addr, void* out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(g_ra, (LPCVOID)addr, out, n, &got) && got == n;
}
static uint64_t rdq(uint64_t addr) {
    uint64_t v = 0;
    return rd(addr, &v, 8) ? v : 0;
}

static void terrain_close();
static void ra_close() {
    terrain_close();
    if (g_ra) CloseHandle(g_ra);
    g_ra = nullptr;
    g_raPid = 0;
    g_raBase = g_raSize = 0;
}

static bool ra_alive() {
    if (!g_ra) return false;
    DWORD code = 0;
    return GetExitCodeProcess(g_ra, &code) && code == STILL_ACTIVE;
}

static bool ra_open() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    for (BOOL ok = Process32First(snap, &pe); ok; ok = Process32Next(snap, &pe))
        if (_stricmp(pe.szExeFile, "RiftApart.exe") == 0) pid = pe.th32ProcessID;
    CloseHandle(snap);
    if (!pid) return false;
    HANDLE h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) {
        logf("Rift Apart found (pid %lu) but can't read it (%lu). Run riftlink as the same user as the game.", pid, GetLastError());
        return false;
    }
    HMODULE mods[1];
    DWORD need = 0;
    if (!EnumProcessModulesEx(h, mods, sizeof(mods), &need, LIST_MODULES_64BIT) || !need) {
        CloseHandle(h);
        return false;  // still starting
    }
    MODULEINFO mi;
    if (!GetModuleInformation(h, mods[0], &mi, sizeof(mi))) {
        CloseHandle(h);
        return false;
    }
    g_ra = h;
    g_raPid = pid;
    g_raBase = (uint64_t)mi.lpBaseOfDll;
    g_raSize = mi.SizeOfImage;
    return true;
}

// RTTI: vtable[-1] -> CompleteObjectLocator; +12 = TypeDescriptor RVA; name at TypeDescriptor+16.
static bool rtti_name(uint64_t vtable, char* out, size_t n) {
    uint64_t col = rdq(vtable - 8);
    if (col < g_raBase || col >= g_raBase + g_raSize) return false;
    uint32_t tdRva = 0;
    if (!rd(col + 12, &tdRva, 4) || tdRva >= g_raSize) return false;
    if (!rd(g_raBase + tdRva + 16, out, n - 1)) return false;
    out[n - 1] = 0;
    return true;
}

static bool check_build() {
    struct { uint32_t rva; const char* name; } want[] = {
        {RVA_HERO_LOCAL, ".?AVHeroLocal@Hero@@"},
        {RVA_PTR_CACHE, ".?AVHeroPointerCache@@"},
        {RVA_MOVER_MGR, ".?AVHeroMoverManager@Hero@@"},
    };
    for (auto& w : want) {
        char name[64];
        if (!rtti_name(g_raBase + w.rva, name, sizeof(name)) || strcmp(name, w.name) != 0) {
            logf("ERROR: this RiftApart.exe doesn't match (expected %s at RVA 0x%x). Was Rift Apart updated?",
                 w.name, w.rva);
            return false;
        }
    }
    return true;
}

struct Hero {
    uint64_t local = 0, cache = 0, mover = 0;
};

static bool hero_valid(const Hero& h) {
    if (!h.local) return false;
    if (rdq(h.local) != g_raBase + RVA_HERO_LOCAL) return false;
    if (rdq(h.local + OFF_CACHE) != h.cache || rdq(h.cache) != g_raBase + RVA_PTR_CACHE) return false;
    if (rdq(h.cache + OFF_MOVER) != h.mover || rdq(h.mover) != g_raBase + RVA_MOVER_MGR) return false;
    return true;
}

static bool hero_from_local(uint64_t local, Hero* out) {
    Hero h;
    h.local = local;
    h.cache = rdq(local + OFF_CACHE);
    if (!h.cache || rdq(h.cache) != g_raBase + RVA_PTR_CACHE) return false;
    h.mover = rdq(h.cache + OFF_MOVER);
    if (!h.mover || rdq(h.mover) != g_raBase + RVA_MOVER_MGR) return false;
    float p[3];
    if (!rd(h.mover + OFF_POS, p, 12)) return false;
    for (float v : p)
        if (!isfinite(v) || fabsf(v) > 1e6f) return false;
    *out = h;
    return true;
}

// Scan committed private read-write memory for HeroLocal's vtable pointer.
static bool find_hero(Hero* out) {
    const uint64_t needle = g_raBase + RVA_HERO_LOCAL;
    static uint8_t* buf = (uint8_t*)VirtualAlloc(nullptr, 4 << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    uint64_t addr = 0x10000, scanned = 0;
    DWORD t0 = GetTickCount();
    int candidates = 0;
    MEMORY_BASIC_INFORMATION mbi;
    while (addr < 0x7FFFFFFF0000ull && VirtualQueryEx(g_ra, (LPCVOID)addr, &mbi, sizeof(mbi))) {
        uint64_t base = (uint64_t)mbi.BaseAddress, end = base + mbi.RegionSize;
        DWORD prot = mbi.Protect & 0xFF;
        bool rw = prot == PAGE_READWRITE || prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READWRITE;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && rw && !(mbi.Protect & PAGE_GUARD)) {
            for (uint64_t at = base; at < end; at += 4 << 20) {
                size_t n = (size_t)((end - at) < (4u << 20) ? (end - at) : (4u << 20));
                SIZE_T got = 0;
                ReadProcessMemory(g_ra, (LPCVOID)at, buf, n, &got);
                scanned += got;
                const uint64_t* q = (const uint64_t*)buf;
                for (size_t i = 0; i < got / 8; i++) {
                    if (q[i] != needle) continue;
                    candidates++;
                    Hero h;
                    if (hero_from_local(at + i * 8, &h)) {
                        logf("found Ratchet: HeroLocal %llx, mover %llx (%d candidates, %llu MB scanned in %.1f s)",
                             (unsigned long long)h.local, (unsigned long long)h.mover, candidates,
                             (unsigned long long)(scanned >> 20), (GetTickCount() - t0) / 1000.0);
                        *out = h;
                        return true;
                    }
                }
            }
        }
        addr = end;
    }
    logf("Ratchet not found yet (%llu MB scanned, %d stale candidates). Load a save and stand in gameplay.",
         (unsigned long long)(scanned >> 20), candidates);
    return false;
}

// ------------------------------------------------------------------------------------------
// Mapping Rift Apart -> Elden Ring

static void map_delta(const float d[3], float out[3]) {
    float x = d[0], y = d[1], z = d[2];
    if (g_cfg.swapXZ) { float t = x; x = z; z = t; }
    if (g_cfg.flipX) x = -x;
    if (g_cfg.flipZ) z = -z;
    float a = g_cfg.yawOffsetDeg * 3.14159265f / 180.0f;
    float rx = x * cosf(a) - z * sinf(a);
    float rz = x * sinf(a) + z * cosf(a);
    out[0] = rx * g_cfg.scale;
    out[1] = y * g_cfg.scale * g_cfg.heightScale;
    out[2] = rz * g_cfg.scale;
}


// Inverse of map_delta: an Elden Ring offset -> Rift Apart offset. dir=true for directions (no scale).
static void unmap_delta(const float d[3], float out[3], bool dir) {
    float s = dir ? 1.0f : (g_cfg.scale != 0 ? g_cfg.scale : 1.0f);
    float sy = dir ? 1.0f : s * (g_cfg.heightScale != 0 ? g_cfg.heightScale : 1.0f);
    float rx = d[0] / s, y = d[1] / sy, rz = d[2] / s;
    float a = g_cfg.yawOffsetDeg * 3.14159265f / 180.0f;
    float x = rx * cosf(a) + rz * sinf(a);
    float z = -rx * sinf(a) + rz * cosf(a);
    if (g_cfg.flipZ) z = -z;
    if (g_cfg.flipX) x = -x;
    if (g_cfg.swapXZ) { float t = x; x = z; z = t; }
    out[0] = x, out[1] = y, out[2] = z;
}

// ------------------------------------------------------------------------------------------
// Elden Ring ground -> Rift Apart. Rays straight down through Elden Ring's collision around the
// Tarnished (the bridge's terrain ray service, run on Elden Ring's game thread), converted into
// Rift Apart coordinates and written as plain data to <Rift Apart>\riftbridge\terrain.shm, where
// the riftbridge module turns them into ground tiles for Ratchet.

static RtHeader* g_rt = nullptr;
static bool g_hurtQuiet = false;          // set by the main loop: sinking, paused, just (re)anchored
static uint64_t g_hurtQuietUntil = 0;
static volatile LONG g_erRunes = -1;      // Tarnished's runes (runes_thread)
static volatile DWORD g_erRunesTick = 0;
static HANDLE g_rtFile = nullptr, g_rtMap = nullptr;
static bool g_rtTried = false;

static void terrain_close() {
    if (g_rt) UnmapViewOfFile(g_rt);
    if (g_rtMap) CloseHandle(g_rtMap);
    if (g_rtFile && g_rtFile != INVALID_HANDLE_VALUE) CloseHandle(g_rtFile);
    g_rt = nullptr, g_rtMap = g_rtFile = nullptr, g_rtTried = false;
}

static bool terrain_open() {
    if (g_rt) return true;
    if (g_rtTried || !g_ra) return false;
    g_rtTried = true;
    char exe[MAX_PATH];
    DWORD n = MAX_PATH;
    if (!QueryFullProcessImageNameA(g_ra, 0, exe, &n)) {
        logf("terrain: can't find Rift Apart's folder (%lu)", GetLastError());
        return false;
    }
    char* sl = strrchr(exe, '\\');
    if (!sl) return false;
    *sl = 0;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\riftbridge", exe);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        logf("terrain: %s doesn't exist (is riftbridge's faultrep.dll installed in Rift Apart?): no ground feed", path);
        return false;
    }
    snprintf(path, sizeof(path), "%s\\riftbridge\\terrain.shm", exe);
    g_rtFile = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_rtFile == INVALID_HANDLE_VALUE) {
        logf("terrain: can't open %s (%lu)", path, GetLastError());
        g_rtFile = nullptr;
        return false;
    }
    g_rtMap = CreateFileMappingA(g_rtFile, nullptr, PAGE_READWRITE, 0, RT_FILE_SIZE, nullptr);
    g_rt = g_rtMap ? (RtHeader*)MapViewOfFile(g_rtMap, FILE_MAP_ALL_ACCESS, 0, 0, RT_FILE_SIZE) : nullptr;
    if (!g_rt) {
        logf("terrain: mapping %s failed (%lu)", path, GetLastError());
        return false;
    }
    g_rt->magic = RT_MAGIC;
    g_rt->version = RTER_VERSION;
    g_rt->writerPid = GetCurrentProcessId();
    logf("terrain: Elden Ring ground feed -> %s", path);
    return true;
}

struct TerrainJob {
    bool busy = false;
    uint32_t seq = 0, anchorId = 0;
    uint64_t sentMs = 0;
    float center[3] = {};
    float r0[3], e0[3];
    float yOffset = 0;
};
static TerrainJob g_tj;
static float g_tLastCenter[3] = {1e9f, 1e9f, 1e9f};
static uint64_t g_tLastMs = 0;
static uint32_t g_tAnchorDone = 0xFFFFFFFFu, g_tLogged = 0;
static float g_terrainYOffset = 0.0f;
static uint64_t g_tNoGroundMs = 0;  // last time the rays found no ground under the Tarnished  // extra Rift Apart height for the tiles (negative while sinking)

static ErmcRayHeader* rays_hdr() { return (ErmcRayHeader*)(g_shm + ERMC_OFF_RAYS); }

// Called every loop while linked. erPos = where the Tarnished is being placed.
// Far ground (coarse ring around the fine grid) and walls (horizontal rays), so Ratchet's shots and
// movement meet Elden Ring's ground further out and its walls, buildings and rocks.
static const int kFarN = 19;          // far grid: kFarN x kFarN points
static const float kFarS = 4.0f;      // ... this far apart
// Outer ring: very coarse ground out to ~150 m, so long throws (bombs, carpet bombs, drill dogs)
// still land on something far from Ratchet.
static const int kOutN = 25;
static const float kOutS = 12.0f;
static const int kWallDirs = 48;
static const float kWallH[4] = {0.9f, 1.8f, 2.7f, 3.6f};  // above the feet (higher pieces caught tree tops and blocked the camera)
static const float kWallLen = 30.0f;
struct TerrainExtra {
    float farCenter[3];
    uint8_t farSkip[kFarN * kFarN];
    int farRay[kFarN * kFarN];  // index of the first of the two rays, -1 if skipped
    float outCenter[3];
    int outRay[kOutN * kOutN];
    int wallRay0;
    float wallOrigin[3];
};
static TerrainExtra g_tx;
// Last ground grid (Elden Ring heights), so the camera can be kept above the ground.
static struct { bool valid; float center[3]; int G; float S; float hy[45 * 45]; uint8_t ok[45 * 45]; } g_ground;
static bool ground_height(float x, float z, float* out) {
    if (!g_ground.valid) return false;
    int G = g_ground.G;
    int gx = (int)lroundf((x - g_ground.center[0]) / g_ground.S) + G / 2, gz = (int)lroundf((z - g_ground.center[2]) / g_ground.S) + G / 2;
    if (gx < 0 || gz < 0 || gx >= G || gz >= G || !g_ground.ok[gz * G + gx]) return false;
    *out = g_ground.hy[gz * G + gx];
    return true;
}

static uint32_t hash_key(int a, int b, int c) {
    return ((uint32_t)a * 73856093u) ^ ((uint32_t)b * 19349663u) ^ ((uint32_t)c * 83492791u);
}

typedef DWORD(WINAPI* XInputGetState_t)(DWORD, void*);
static XInputGetState_t g_xinput = nullptr;
struct XPad {
    DWORD packet;
    WORD buttons;
    BYTE lt, rt;
    SHORT lx, ly, rx, ry;
};
static bool pad_state(XPad* out) {
    static bool tried = false;
    if (!tried) {
        tried = true;
        HMODULE h = LoadLibraryA("xinput1_4.dll");
        if (!h) h = LoadLibraryA("xinput9_1_0.dll");
        if (h) g_xinput = (XInputGetState_t)GetProcAddress(h, "XInputGetState");
    }
    if (!g_xinput) return false;
    for (DWORD i = 0; i < 4; i++)
        if (g_xinput(i, out) == 0) return true;
    return false;
}

static void terrain_tick(uint64_t now, uint32_t anchorId, const float r0[3], const float e0[3], const float erPos[3]) {
    if (!g_cfg.terrain || !terrain_open()) return;
    g_rt->reserved[1] = g_rt->reserved[1] + 1;  // heartbeat: Rift Apart only trusts a live feed
    {  // controller -> Rift Apart (it only sees the pad itself while its own window has focus)
        XPad xp;
        bool ok = g_cfg.padToRift && pad_state(&xp);
        if (ok) {
            g_rt->padPacket = xp.packet, g_rt->padButtons = xp.buttons, g_rt->padLT = xp.lt, g_rt->padRT = xp.rt;
            g_rt->padLX = xp.lx, g_rt->padLY = xp.ly, g_rt->padRX = xp.rx, g_rt->padRY = xp.ry;
        }
        g_rt->padOn = ok ? RT_RUNES_TAG : 0;
        g_rt->padSeq = g_rt->padSeq + 1;
        static bool logged = false;
        if (ok && !logged) logged = true, logf("controller: found; passing it to Rift Apart while Elden Ring has focus");
    }
    {  // Elden Ring hits on the stand-in Tarnished -> damage for Ratchet (fraction of max health)
        static bool have = false;
        static uint32_t lastHits = 0;
        static float lastTotal = 0, frac = 0;
        static uint32_t hits = 0;
        const ErmcHunterEvents* ev = (const ErmcHunterEvents*)(g_shm + ERMC_OFF_HUNTER);
        uint32_t s1 = ev->seq;
        if (!(s1 & 1)) {
            MemoryBarrier();
            uint32_t hc = ev->hitCount;
            float tot = ev->totalDamage, mx = ev->hunterMaxHp, last = ev->lastDamage;
            MemoryBarrier();
            if (ev->seq == s1) {
                static DWORD lastApplied = 0;
                DWORD tnow = GetTickCount();
                if (!have) have = true, lastHits = hc, lastTotal = tot;
                else if (hc != lastHits) {
                    float d = tot - lastTotal;
                    // Not every Tarnished HP loss is an enemy's hit: the hidden stand-in can be put into a
                    // wall or the air during sinks, re-anchors and loading screens (falls, kill planes:
                    // the game takes it to 1 HP), and some hits arrive as a burst of ticks.
                    const char* why = nullptr;
                    if (g_hurtQuiet) why = "sinking / re-anchoring / paused";
                    else if (mx > 0 && last >= mx - 2) why = "a fall or the game's kill zone, not an enemy";
                    else if (tnow - lastApplied < 400) why = "same hit (burst)";
                    if (d > 0 && mx > 0 && g_cfg.damageScale > 0 && !why) {
                        float f = d / mx * g_cfg.damageScale;
                        if (f > 0.35f) f = 0.35f;
                        frac += f;
                        hits += hc - lastHits;
                        lastApplied = tnow;
                        logf("hurt: Elden Ring hit the Tarnished for %.0f of %.0f HP -> Ratchet loses %.0f%% of his health",
                             last, mx, f * 100);
                    } else if (d > 0 && why) {
                        static int skipped = 0;
                        if (skipped++ < 30) logf("hurt: ignored %.0f HP lost by the Tarnished (%s)", last, why);
                    }
                    lastHits = hc, lastTotal = tot;
                }
            }
        }
        g_rt->heroDamage = frac;
        g_rt->heroHits = hits;
        g_rt->heroMinHp = (float)g_cfg.minHp;
        g_rt->damageTag = g_cfg.damageScale > 0 ? RT_RUNES_TAG : 0;
    }
    if (g_erRunes >= 0 && GetTickCount() - g_erRunesTick < 3000) {
        g_rt->erRunes = g_erRunes;
        g_rt->runesTag = g_cfg.showRunes ? RT_RUNES_TAG : 0;
        g_rt->runesBeat = g_rt->runesBeat + 1;
    } else {
        g_rt->runesTag = 0;
    }
    ErmcRayHeader* h = rays_hdr();
    const int G = g_cfg.gridSize, N = G * G;
    const float S = g_cfg.gridSpacing;
    if (g_tj.busy) {
        if (h->respSeq != g_tj.seq) {
            if (now - g_tj.sentMs > 4000) {
                logf("terrain: Elden Ring didn't answer the ground rays in 4 s; trying again");
                g_tj.busy = false;
            }
            return;
        }
        g_tj.busy = false;
        MemoryBarrier();
        if (g_tj.anchorId != anchorId) return;  // re-anchored meanwhile: stale
        const ErmcRayHit* hits = (const ErmcRayHit*)((uint8_t*)h + ERMC_RAYS_OFF_HITS);
        static float hy[45 * 45], fy[kFarN * kFarN], oy[kOutN * kOutN];
        static uint8_t ok[45 * 45], fok[kFarN * kFarN], ook[kOutN * kOutN];
        int nOk = 0;
        auto pick = [&](int r, float* out) {  // ground ray pair: at/below the feet, else uphill
            const ErmcRayHit& lo = hits[r];
            const ErmcRayHit& hi = hits[r + 1];
            if (lo.hit && isfinite(lo.pos[1])) return *out = lo.pos[1], true;
            if (hi.hit && isfinite(hi.pos[1])) return *out = hi.pos[1], true;
            return false;
        };
        for (int i = 0; i < N; i++) nOk += (ok[i] = pick(i * 2, &hy[i]) ? 1 : 0);
        for (int i = 0; i < kFarN * kFarN; i++) fok[i] = g_tx.farRay[i] >= 0 && pick(g_tx.farRay[i], &fy[i]);
        for (int i = 0; i < kOutN * kOutN; i++) ook[i] = g_tx.outRay[i] >= 0 && pick(g_tx.outRay[i], &oy[i]);
        if (nOk == 0) g_tNoGroundMs = now;
        if (nOk == 0) {  // no ground below (falling, or under the map): keep the last ground
            if (g_tLogged++ < 5 || g_tLogged % 100 == 0) logf("terrain: no Elden Ring ground found under the Tarnished; keeping the last tiles");
            return;
        }
        // Ground height near an Elden Ring point (fine grid, else far grid), for telling walls from slopes.
        auto groundAt = [&](float x, float z, float* out) {
            int gx = (int)lroundf((x - g_tj.center[0]) / S) + G / 2, gz = (int)lroundf((z - g_tj.center[2]) / S) + G / 2;
            if (gx >= 0 && gz >= 0 && gx < G && gz < G && ok[gz * G + gx]) return *out = hy[gz * G + gx], true;
            int fx = (int)lroundf((x - g_tx.farCenter[0]) / kFarS) + kFarN / 2, fz = (int)lroundf((z - g_tx.farCenter[2]) / kFarS) + kFarN / 2;
            if (fx >= 0 && fz >= 0 && fx < kFarN && fz < kFarN && fok[fz * kFarN + fx]) return *out = fy[fz * kFarN + fx], true;
            return false;
        };
        g_ground.valid = true, g_ground.G = G, g_ground.S = S;
        memcpy(g_ground.center, g_tj.center, 12);
        memcpy(g_ground.hy, hy, sizeof(float) * N);
        memcpy(g_ground.ok, ok, N);
        RtTile* tiles = (RtTile*)g_rt->tiles;
        uint32_t seq = g_rt->seq;
        g_rt->seq = seq + 1;  // odd: writing
        MemoryBarrier();
        uint32_t cnt = 0;
        auto emit = [&](const float pe[3], const float ne[3], float half, uint32_t key) {
            if (cnt >= RT_MAX_TILES) return;
            float d[3] = {pe[0] - g_tj.e0[0], pe[1] - g_tj.e0[1], pe[2] - g_tj.e0[2]}, ra[3], nr[3];
            unmap_delta(d, ra, false);
            unmap_delta(ne, nr, true);
            float ll = sqrtf(nr[0] * nr[0] + nr[1] * nr[1] + nr[2] * nr[2]);
            RtTile& t = tiles[cnt++];
            for (int k = 0; k < 3; k++) t.pos[k] = g_tj.r0[k] + ra[k], t.normal[k] = nr[k] / ll;
            t.pos[1] += g_tj.yOffset;
            t.half = half / (g_cfg.scale > 0.1f ? g_cfg.scale : 1.0f);  // Elden Ring metres -> Rift Apart
            t.flags = key;
        };
        // Slope normal of a height grid from its neighbours (steps over 1.5 m are ledges, not slopes).
        auto slope = [&](const float* hv, const uint8_t* okv, int n, float sp, int gx, int gz, float* ne) {
            int i = gz * n + gx;
            auto hAt = [&](int x, int z, float* out) {
                if (x < 0 || z < 0 || x >= n || z >= n || !okv[z * n + x]) return false;
                float v = hv[z * n + x];
                if (fabsf(v - hv[i]) > 1.5f) return false;
                *out = v;
                return true;
            };
            float a, b, dx = 0, dz = 0;
            bool l = hAt(gx - 1, gz, &a), r = hAt(gx + 1, gz, &b);
            if (l && r) dx = (b - a) / (2 * sp); else if (r) dx = (b - hv[i]) / sp; else if (l) dx = (hv[i] - a) / sp;
            l = hAt(gx, gz - 1, &a), r = hAt(gx, gz + 1, &b);
            if (l && r) dz = (b - a) / (2 * sp); else if (r) dz = (b - hv[i]) / sp; else if (l) dz = (hv[i] - a) / sp;
            float nl = sqrtf(dx * dx + 1.0f + dz * dz);
            ne[0] = -dx / nl, ne[1] = 1.0f / nl, ne[2] = -dz / nl;
        };
        for (int gz = 0; gz < G; gz++)
            for (int gx = 0; gx < G; gx++) {
                int i = gz * G + gx;
                if (!ok[i]) continue;
                float ne[3];
                slope(hy, ok, G, S, gx, gz, ne);
                float pe[3] = {g_tj.center[0] + (gx - G / 2) * S, hy[i], g_tj.center[2] + (gz - G / 2) * S};
                int ix = (int)lroundf(pe[0] / S), iz = (int)lroundf(pe[2] / S);
                emit(pe, ne, S * 0.6f, ((uint32_t)(uint16_t)ix << 16) | (uint32_t)(uint16_t)iz);
            }
        int nFar = 0, nWall = 0;
        for (int fz = 0; fz < kFarN; fz++)
            for (int fx = 0; fx < kFarN; fx++) {
                int i = fz * kFarN + fx;
                if (!fok[i]) continue;
                float ne[3];
                slope(fy, fok, kFarN, kFarS, fx, fz, ne);
                float pe[3] = {g_tx.farCenter[0] + (fx - kFarN / 2) * kFarS, fy[i], g_tx.farCenter[2] + (fz - kFarN / 2) * kFarS};
                emit(pe, ne, kFarS * 0.6f, hash_key(1, (int)lroundf(pe[0] / kFarS), (int)lroundf(pe[2] / kFarS)));
                nFar++;
            }
        for (int oz = 0; oz < kOutN; oz++)
            for (int ox = 0; ox < kOutN; ox++) {
                int i = oz * kOutN + ox;
                if (!ook[i]) continue;
                float ne[3];
                slope(oy, ook, kOutN, kOutS, ox, oz, ne);
                float pe[3] = {g_tx.outCenter[0] + (ox - kOutN / 2) * kOutS, oy[i], g_tx.outCenter[2] + (oz - kOutN / 2) * kOutS};
                emit(pe, ne, kOutS * 0.55f, hash_key(3, (int)lroundf(pe[0] / kOutS), (int)lroundf(pe[2] / kOutS)));
                nFar++;
            }
        // Walls: each piece faces along the wall (from its neighbours' hits), sized to the gap to its
        // neighbours, so pieces don't stick out across corridors or doorways.
        for (int hI = 0; hI < 4; hI++) {
            bool valid[kWallDirs];
            for (int dI = 0; dI < kWallDirs; dI++) {
                const ErmcRayHit& w = hits[g_tx.wallRay0 + hI * kWallDirs + dI];
                float g;
                bool haveG = groundAt(w.pos[0], w.pos[2], &g);
                valid[dI] = w.hit && isfinite(w.pos[0] + w.pos[1] + w.pos[2]) &&
                            !(haveG && w.pos[1] - g < 0.6f) &&  // rising ground isn't a wall
                            !(haveG && w.pos[1] - g > 3.0f);    // branches / overhangs high above the ground: skip
            }
            for (int dI = 0; dI < kWallDirs; dI++) {
                if (!valid[dI]) continue;
                const float* pc = hits[g_tx.wallRay0 + hI * kWallDirs + dI].pos;
                int l = (dI + kWallDirs - 1) % kWallDirs, r = (dI + 1) % kWallDirs;
                const float* pl = valid[l] ? hits[g_tx.wallRay0 + hI * kWallDirs + l].pos : nullptr;
                const float* pr = valid[r] ? hits[g_tx.wallRay0 + hI * kWallDirs + r].pos : nullptr;
                auto dist2 = [](const float* a, const float* b) {
                    float x = a[0] - b[0], z = a[2] - b[2];
                    return sqrtf(x * x + z * z);
                };
                if (pl && dist2(pl, pc) > 4.0f) pl = nullptr;
                if (pr && dist2(pr, pc) > 4.0f) pr = nullptr;
                float a = dI * 6.2831853f / kWallDirs;
                float ne[3] = {-sinf(a), 0, -cosf(a)};
                if (pl || pr) {
                    const float* p0 = pl ? pl : pc;
                    const float* p1 = pr ? pr : pc;
                    float tx = p1[0] - p0[0], tz = p1[2] - p0[2], tl = sqrtf(tx * tx + tz * tz);
                    if (tl > 0.05f) {
                        float nx = tz / tl, nz = -tx / tl;
                        if (nx * (g_tx.wallOrigin[0] - pc[0]) + nz * (g_tx.wallOrigin[2] - pc[2]) < 0) nx = -nx, nz = -nz;
                        ne[0] = nx, ne[2] = nz;
                    }
                }
                float gap = fmaxf(pl ? dist2(pl, pc) : 0.0f, pr ? dist2(pr, pc) : 0.0f);
                if (gap <= 0) gap = 2.0f * dist2(pc, g_tx.wallOrigin) * 0.0654f;  // angular spacing at this distance
                float want = gap * 0.6f;
                float half = want <= 0.3f ? 0.3f : want <= 0.5f ? 0.5f : want <= 1.0f ? 1.0f : 2.0f;
                emit(pc, ne, half, hash_key(2, (int)lroundf(pc[0] * 2), (int)lroundf(pc[1]) * 4096 + (int)lroundf(pc[2] * 2)));
                nWall++;
            }
        }
        g_rt->count = cnt;
        g_rt->reserved[0] = g_tj.anchorId;
        g_rt->generation = g_rt->generation + 1;
        MemoryBarrier();
        g_rt->seq = seq + 2;
        g_tAnchorDone = anchorId;
        if (g_tLogged++ < 5 || g_tLogged % 100 == 0)
            logf("terrain: %d of %d ground points, %d far ground, %d wall pieces; %u tiles sent to Rift Apart (round trip %llu ms)",
                 nOk, N, nFar, nWall, cnt, (unsigned long long)(now - g_tj.sentMs));
        return;
    }
    float dx = erPos[0] - g_tLastCenter[0], dy = erPos[1] - g_tLastCenter[1], dz = erPos[2] - g_tLastCenter[2];
    bool due = anchorId != g_tAnchorDone || dx * dx + dz * dz > g_cfg.resampleDist * g_cfg.resampleDist ||
               fabsf(dy) > 3.0f || now - g_tLastMs > 3000;
    if (!due) return;
    if (h->reqSeq != h->respSeq) return;  // someone else's batch still running
    ErmcRay* rays = (ErmcRay*)((uint8_t*)h + ERMC_RAYS_OFF_RAYS);
    int nr = 0;
    auto groundPair = [&](float x, float z, float y) {
        ErmcRay& lo = rays[nr++];
        ErmcRay& hi = rays[nr++];
        lo.start[0] = lo.end[0] = hi.start[0] = hi.end[0] = x;
        lo.start[2] = lo.end[2] = hi.start[2] = hi.end[2] = z;
        lo.start[1] = y + 2.5f, lo.end[1] = y - 30.0f;
        hi.start[1] = y + 15.0f, hi.end[1] = y + 2.5f;
    };
    // Grid fixed to the world (cells at multiples of the spacing), so a cell keeps its tile in Rift
    // Apart while the grid follows the Tarnished.
    float snapped[3] = {roundf(erPos[0] / S) * S, erPos[1], roundf(erPos[2] / S) * S};
    for (int gz = 0; gz < G; gz++)
        for (int gx = 0; gx < G; gx++) groundPair(snapped[0] + (gx - G / 2) * S, snapped[2] + (gz - G / 2) * S, erPos[1]);
    // Far ring: coarse cells outside the fine grid.
    float fc[3] = {roundf(erPos[0] / kFarS) * kFarS, erPos[1], roundf(erPos[2] / kFarS) * kFarS};
    float inner = (G / 2) * S + kFarS * 0.5f;
    for (int fz = 0; fz < kFarN; fz++)
        for (int fx = 0; fx < kFarN; fx++) {
            int i = fz * kFarN + fx;
            float x = fc[0] + (fx - kFarN / 2) * kFarS, z = fc[2] + (fz - kFarN / 2) * kFarS;
            if (fabsf(x - snapped[0]) < inner && fabsf(z - snapped[2]) < inner) {
                g_tx.farRay[i] = -1;
                continue;
            }
            g_tx.farRay[i] = nr;
            groundPair(x, z, erPos[1]);
        }
    memcpy(g_tx.farCenter, fc, 12);
    // Outer ring: very coarse cells outside the far ring, with taller rays (hills further out).
    float oc[3] = {roundf(erPos[0] / kOutS) * kOutS, erPos[1], roundf(erPos[2] / kOutS) * kOutS};
    float farEdge = (kFarN / 2) * kFarS + kOutS * 0.5f;
    for (int oz = 0; oz < kOutN; oz++)
        for (int ox = 0; ox < kOutN; ox++) {
            int i = oz * kOutN + ox;
            float x = oc[0] + (ox - kOutN / 2) * kOutS, z = oc[2] + (oz - kOutN / 2) * kOutS;
            if (fabsf(x - fc[0]) < farEdge && fabsf(z - fc[2]) < farEdge) {
                g_tx.outRay[i] = -1;
                continue;
            }
            g_tx.outRay[i] = nr;
            ErmcRay& lo = rays[nr++];
            ErmcRay& hi = rays[nr++];
            lo.start[0] = lo.end[0] = hi.start[0] = hi.end[0] = x;
            lo.start[2] = lo.end[2] = hi.start[2] = hi.end[2] = z;
            lo.start[1] = erPos[1] + 2.5f, lo.end[1] = erPos[1] - 90.0f;
            hi.start[1] = erPos[1] + 70.0f, hi.end[1] = erPos[1] + 2.5f;
        }
    memcpy(g_tx.outCenter, oc, 12);
    // Walls: horizontal rays all around at a few heights.
    g_tx.wallRay0 = nr;
    memcpy(g_tx.wallOrigin, erPos, 12);
    for (int hI = 0; hI < 4; hI++)
        for (int dI = 0; dI < kWallDirs; dI++) {
            float a = dI * 6.2831853f / kWallDirs, dxw = sinf(a), dzw = cosf(a);
            ErmcRay& r = rays[nr++];
            r.start[0] = erPos[0] + dxw * 0.5f, r.start[1] = erPos[1] + kWallH[hI], r.start[2] = erPos[2] + dzw * 0.5f;
            r.end[0] = erPos[0] + dxw * kWallLen, r.end[1] = r.start[1], r.end[2] = erPos[2] + dzw * kWallLen;
        }
    h->count = (uint32_t)nr;
    h->flags = 0;
    h->processed = 0;
    MemoryBarrier();
    g_tj.seq = h->reqSeq + 1;
    g_tj.busy = true;
    g_tj.sentMs = now;
    g_tj.anchorId = anchorId;
    memcpy(g_tj.center, snapped, 12);
    memcpy(g_tj.r0, r0, 12);
    memcpy(g_tj.e0, e0, 12);
    g_tj.yOffset = g_terrainYOffset;
    memcpy(g_tLastCenter, erPos, 12);
    g_tLastMs = now;
    h->reqSeq = g_tj.seq;
}


// ------------------------------------------------------------------------------------------
// Rift Apart's real camera. HeroCameraManager's +0x1F8 is Ratchet's feet (where the camera looks
// roughly), not the centre of the view, so the picture didn't line up. Find the camera object's
// world matrix (3 orthonormal axes + translation next to the manager's eye point) once, among the
// Camera2 objects reachable from the camera manager, then read it every tick.

struct CamProbe {
    uint64_t obj = 0, vt = 0;
    uint32_t posOff = 0, rotOff = 0, stride = 16;
    int quat = 0;            // 1: rotation stored as a quaternion at rotOff
    int fwd = 2, fwdSign = 1, up = 1;
    bool hasVt = false;      // the block starts with a vtable (else it's a plain matrix block)
    uint64_t ringBase = 0;   // plain blocks come in a ring of per-frame copies: base, count, stride
    int ringCount = 0;
    uint32_t ringStride = 0;
    int fovOff = -1;         // Rift Apart's field of view (radians, horizontal) next to near/far, if found
    uint64_t nextTry = 0;
    int tries = 0;
    bool proper = false;     // the known-good layout: plain 4x4 matrix block, axes +0, position +0x30, forward = axis 2
};
static CamProbe g_cam;
static int g_camVtCount = -1;
static float g_raFovH = 0;  // Rift Apart's horizontal field of view (radians), when found  // cached camera-class instances (-1 = scan again)

static bool unit3(const float* v) {
    float l = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    return isfinite(l) && fabsf(l - 1.0f) < 0.01f;
}
static float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// Quaternion (x,y,z,w) -> the three rotated basis vectors (rows of axes[9]).
static void quat_axes(const float* q, float* ax) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    ax[0] = 1 - 2 * (y * y + z * z), ax[1] = 2 * (x * y + z * w), ax[2] = 2 * (x * z - y * w);
    ax[3] = 2 * (x * y - z * w), ax[4] = 1 - 2 * (x * x + z * z), ax[5] = 2 * (y * z + x * w);
    ax[6] = 2 * (x * z + y * w), ax[7] = 2 * (y * z - x * w), ax[8] = 1 - 2 * (x * x + y * y);
}

// Read the rotation of a candidate as 3 axes; false if it isn't a rotation (any more).
static bool cand_axes(const uint8_t* buf, size_t n, uint32_t rotOff, uint32_t stride, int quat, float* ax) {
    if (quat) {
        if (rotOff + 16 > n) return false;
        const float* q = (const float*)(buf + rotOff);
        float l = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
        if (!isfinite(l) || fabsf(l - 1.0f) > 0.002f) return false;
        quat_axes(q, ax);
        return true;
    }
    if (rotOff + stride * 2 + 12 > n) return false;
    for (int i = 0; i < 3; i++) memcpy(ax + i * 3, buf + rotOff + i * stride, 12);
    if (!unit3(ax) || !unit3(ax + 3) || !unit3(ax + 6)) return false;
    return fabsf(dot3(ax, ax + 3)) < 0.01f && fabsf(dot3(ax, ax + 6)) < 0.01f && fabsf(dot3(ax + 3, ax + 6)) < 0.01f;
}

// Instances of a few camera classes, by vtable, in one pass over Rift Apart's memory.
static int scan_vtables(const uint64_t* vts, int nvt, uint64_t* out, int maxOut) {
    static uint8_t* buf = (uint8_t*)VirtualAlloc(nullptr, 4 << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    int n = 0;
    uint64_t addr = 0x10000;
    MEMORY_BASIC_INFORMATION mbi;
    while (n < maxOut && addr < 0x7FFFFFFF0000ull && VirtualQueryEx(g_ra, (LPCVOID)addr, &mbi, sizeof(mbi))) {
        uint64_t base = (uint64_t)mbi.BaseAddress, end = base + mbi.RegionSize;
        DWORD prot = mbi.Protect & 0xFF;
        bool rw = prot == PAGE_READWRITE || prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READWRITE;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && rw && !(mbi.Protect & (PAGE_GUARD | PAGE_WRITECOMBINE | PAGE_NOCACHE))) {
            for (uint64_t at = base; at < end && n < maxOut; at += 4 << 20) {
                size_t len = (size_t)((end - at) < (4u << 20) ? (end - at) : (4u << 20));
                SIZE_T got = 0;
                ReadProcessMemory(g_ra, (LPCVOID)at, buf, len, &got);
                const uint64_t* q = (const uint64_t*)buf;
                for (size_t i = 0; i < got / 8 && n < maxOut; i++)
                    for (int k = 0; k < nvt; k++)
                        if (q[i] == vts[k]) out[n++] = at + i * 8;
            }
        }
        addr = end;
    }
    return n;
}

static void cam_probe(uint64_t cm, const float eye[3], const float tgt[3], uint64_t now) {
    if (now < g_cam.nextTry) return;
    g_cam.tries++;
    g_cam.nextTry = now + (g_cam.tries < 3 ? 3000 : 20000);
    float dir[3] = {tgt[0] - eye[0], tgt[1] - eye[1], tgt[2] - eye[2]};
    float dl = sqrtf(dot3(dir, dir));
    if (dl < 0.1f) return;
    for (float& v : dir) v /= dl;
    DWORD t0 = GetTickCount();
    // Candidates: objects reachable from the camera manager (2 levels) + every instance of the
    // Camera2 camera classes.
    static uint64_t cand[4000];
    int cn = 0;
    cand[cn++] = cm;
    for (int lvl = 0, from = 0; lvl < 2; lvl++) {
        int to = cn;
        for (int c = from; c < to && cn < 3000; c++) {
            uint64_t q[0x100];
            if (!rd(cand[c], q, sizeof(q))) continue;
            for (int i = 1; i < 0x100 && cn < 3000; i++) {
                uint64_t v = q[i];
                if (v < 0x10000 || v > 0x7FFFFFFFFFF0ull || (v & 7) || (v >= g_raBase && v < g_raBase + g_raSize)) continue;
                bool seen = false;
                for (int j = 0; j < cn && !seen; j++) seen = cand[j] == v;
                if (!seen) cand[cn++] = v;
            }
        }
        from = to;
    }
    const uint64_t vts[] = {g_raBase + 0x2ee3258, g_raBase + 0x2ee2a58, g_raBase + 0x2ee3fa8, g_raBase + 0x32c0d50,
                            g_raBase + 0x45dbcf0};
    // The full-memory pass takes many seconds: in the background, once per Ratchet; meanwhile the
    // objects next to the camera manager are tried on their own.
    static uint64_t vtObjs[512];
    static volatile LONG scanning = 0;
    static uint64_t scanVts[5];
    if (g_camVtCount < 0 && !scanning) {
        memcpy(scanVts, vts, sizeof(scanVts));
        InterlockedExchange(&scanning, 1);
        CreateThread(nullptr, 0, [](void*) -> DWORD {
            DWORD t = GetTickCount();
            int n = scan_vtables(scanVts, 5, vtObjs, 512);
            logf("camera: %d camera-class objects in Rift Apart (background search, %lu ms)", n, GetTickCount() - t);
            g_camVtCount = n;
            InterlockedExchange(&scanning, 0);
            return 0;
        }, nullptr, 0, nullptr);
    }
    int nvtObj = g_camVtCount < 0 ? 0 : g_camVtCount < 4000 - cn ? g_camVtCount : 4000 - cn;
    int firstVt = cn;
    memcpy(cand + cn, vtObjs, nvtObj * sizeof(uint64_t));
    cn += nvtObj;
    static uint8_t buf[0x1000];
    float bestScore = 0;
    CamProbe best;
    int shown = 0, dumped = 0;
    char nm[96];
    for (int c = 0; c < cn; c++) {
        SIZE_T got = 0;
        if (!ReadProcessMemory(g_ra, (LPCVOID)cand[c], buf, sizeof(buf), &got) || got < 0x100) continue;
        uint64_t vt = *(uint64_t*)buf;
        nm[0] = 0;
        if (!(vt >= g_raBase && vt < g_raBase + g_raSize && rtti_name(vt, nm, sizeof(nm)))) strcpy(nm, "?");
        if (c >= firstVt && dumped < 4) {  // raw floats of a few camera objects, for later analysis
            dumped++;
            logf("camera dump %s @%llx:", nm, (unsigned long long)cand[c]);
            for (uint32_t o = 0; o < 0x600 && o < got; o += 0x40) {
                char line[400];
                int k = snprintf(line, sizeof(line), "  +%03x", o);
                for (int j = 0; j < 16; j++) {
                    float f;
                    memcpy(&f, buf + o + j * 4, 4);
                    k += snprintf(line + k, sizeof(line) - k, (isfinite(f) && fabsf(f) < 1e5f && (fabsf(f) > 1e-4f || f == 0)) ? " %.3f" : " .", f);
                }
                logf("%s", line);
            }
        }
        for (uint32_t p = 0; p + 12 <= got; p += 4) {
            const float* t = (const float*)(buf + p);
            float dx = t[0] - eye[0], dy = t[1] - eye[1], dz = t[2] - eye[2];
            float d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (!(d < 1.0f)) continue;
            for (int quat = 0; quat < 2; quat++)
                for (uint32_t stride = 12; stride <= 16; stride += 4) {
                    if (quat && stride == 12) continue;
                    for (int r = -0xC0; r <= 0xC0; r += 4) {
                        if ((int)p + r < 0) continue;
                        uint32_t ro = p + r;
                        float ax[9];
                        if (!cand_axes(buf, got, ro, stride, quat, ax)) continue;
                        float dots[3] = {dot3(ax, dir), dot3(ax + 3, dir), dot3(ax + 6, dir)};
                        int k = 0;
                        for (int i = 1; i < 3; i++) if (fabsf(dots[i]) > fabsf(dots[k])) k = i;
                        float score = fabsf(dots[k]) - d * 0.2f;
                        if (fabsf(dots[k]) < 0.75f) continue;
                        // The rendered camera is a plain 4x4 matrix (axes at +0, position at +0x30, looking
                        // down axis 2). Other readings of the same numbers (quaternions, the manager's ideal
                        // eye) sometimes score a little higher but turn the picture wrong: always prefer it.
                        bool proper = !quat && stride == 16 && p == ro + 0x30 && k == 2 && strcmp(nm, "?") == 0;
                        if (proper) score += 2.0f;
                        if (shown < 25) {
                            shown++;
                            logf("camera candidate %s @%llx pos +%x (%.2f m) %s +%x: forward axis %d dot %.3f", nm,
                                 (unsigned long long)cand[c], p, d, quat ? "quat" : stride == 16 ? "axes16" : "axes12", ro, k,
                                 dots[k]);
                        }
                        if (score > bestScore) {
                            bestScore = score;
                            best = CamProbe();
                            best.obj = cand[c], best.vt = vt, best.posOff = p, best.rotOff = ro, best.stride = stride;
                            best.hasVt = strcmp(nm, "?") != 0;
                            best.fovOff = -1;
                            for (uint32_t fo = ro; fo + 12 <= got && fo < ro + 0x200; fo += 4) {  // near, far, fov
                                const float* t3 = (const float*)(buf + fo);
                                if (t3[0] >= 0.01f && t3[0] <= 1.0f && t3[1] >= 100.0f && t3[1] <= 100000.0f && t3[2] > 0.3f &&
                                    t3[2] < 2.6f) {
                                    best.fovOff = (int)fo + 8;
                                    break;
                                }
                            }
                            best.quat = quat, best.fwd = k, best.fwdSign = dots[k] >= 0 ? 1 : -1;
                            best.proper = proper;
                            const float wup[3] = {0, 1, 0};
                            int u = -1;
                            for (int i = 0; i < 3; i++)
                                if (i != k && (u < 0 || fabsf(dot3(ax + i * 3, wup)) > fabsf(dot3(ax + u * 3, wup)))) u = i;
                            best.up = u;
                        }
                    }
                }
        }
    }
    if (best.obj && !best.hasVt) {
        // Rift Apart keeps a ring of per-frame camera copies; a fixed one is often a few frames old.
        // Find the ring (same layout every 0x100 bytes) and pick the newest copy on every read.
        uint32_t st = 0x100;
        auto validAt = [&](uint64_t a) {
            uint8_t b[0x100];
            float ax[9];
            return rd(a, b, sizeof(b)) && best.rotOff + 48 <= sizeof(b) && cand_axes(b, sizeof(b), best.rotOff, best.stride, best.quat, ax);
        };
        uint64_t lo = best.obj;
        while (best.obj - lo < st * 32 && validAt(lo - st)) lo -= st;
        int cnt = 0;
        while (cnt < 64 && validAt(lo + (uint64_t)cnt * st)) cnt++;
        if (cnt > 1) {
            best.ringBase = lo, best.ringCount = cnt, best.ringStride = st;
            logf("camera: the real camera is a ring of %d per-frame copies; always using the newest", cnt);
        }
    }
    if (best.obj) {
        int tries = g_cam.tries;
        g_cam = best;
        g_cam.tries = tries;
        g_cam.nextTry = now + 3000;
        if (!best.proper && tries <= 3)
            logf("camera: this is a stand-in (Rift Apart's rendered camera matrix not found yet); looking again every 3 s");
        rtti_name(best.vt, nm, sizeof(nm));
        if (!best.hasVt) strcpy(nm, "matrix block");
        logf("camera: using Rift Apart's real camera: %s @%llx, position +%x, %s +%x, forward axis %d%s, field of view %s (%d objects, %lu ms)",
             nm, (unsigned long long)best.obj, best.posOff, best.quat ? "quaternion" : "axes", best.rotOff, best.fwd,
             best.fwdSign > 0 ? "" : " reversed", best.fovOff >= 0 ? "found" : "not found", cn, GetTickCount() - t0);
    } else {
        logf("camera: real camera not found among %d objects (%d camera-class objects); using the look-at point (%lu ms)", cn,
             nvtObj, GetTickCount() - t0);
    }
}

// Real camera position + forward + up, in Rift Apart space.
static float g_prevCamFwd[3] = {0, 0, 0}, g_prevCamPos[3] = {0, 0, 0};
static bool cam_read(float pos[3], float fwd[3], float up[3], const float* eyeHint = nullptr, const float* tgtHint = nullptr) {
    if (!g_cam.obj) return false;
    if (g_cam.ringCount > 1 && eyeHint) {
        // The ring holds every view Rift Apart renders this frame (main camera, reflections, shadows,
        // copies turned 90 degrees). The main camera: looks towards Ratchet from within a few metres,
        // and moves smoothly from last frame (position and direction). The camera manager's eye is
        // only the starting point: when the camera bumps into something, the real one is elsewhere.
        const float* rat = tgtHint;
        uint64_t bestA = 0;
        float bestS = -1e9f;
        bool havePrev = dot3(g_prevCamFwd, g_prevCamFwd) > 0.5f;
        for (int i = 0; i < g_cam.ringCount; i++) {
            uint64_t a = g_cam.ringBase + (uint64_t)i * g_cam.ringStride;
            uint8_t b[0x100];
            float ax[9];
            if (!rd(a, b, sizeof(b)) || !cand_axes(b, sizeof(b), g_cam.rotOff, g_cam.stride, g_cam.quat, ax)) continue;
            const float* t = (const float*)(b + g_cam.posOff);
            float f[3] = {ax[g_cam.fwd * 3] * g_cam.fwdSign, ax[g_cam.fwd * 3 + 1] * g_cam.fwdSign, ax[g_cam.fwd * 3 + 2] * g_cam.fwdSign};
            float sc = 0;
            if (rat) {
                float v[3] = {rat[0] - t[0], rat[1] + 0.8f - t[1], rat[2] - t[2]};
                float l = sqrtf(dot3(v, v));
                if (l < 0.2f || l > 15.0f) continue;  // not around Ratchet
                float dr = dot3(f, v) / l;
                if (dr < 0.3f) continue;             // not looking at him
                sc += dr * 2.0f;
            }
            if (havePrev) {
                float dp = fabsf(t[0] - g_prevCamPos[0]) + fabsf(t[1] - g_prevCamPos[1]) + fabsf(t[2] - g_prevCamPos[2]);
                sc += dot3(f, g_prevCamFwd) * 3.0f - dp * 0.5f;
            } else {
                sc -= fabsf(t[0] - eyeHint[0]) + fabsf(t[1] - eyeHint[1]) + fabsf(t[2] - eyeHint[2]);
            }
            if (sc > bestS) bestS = sc, bestA = a;
        }
        if (bestA) g_cam.obj = bestA;
    }
    uint8_t buf[0x1000];
    uint32_t lo = g_cam.posOff < g_cam.rotOff ? g_cam.posOff : g_cam.rotOff;
    uint32_t hi = (g_cam.posOff + 12 > g_cam.rotOff + 48 ? g_cam.posOff + 12 : g_cam.rotOff + 48);
    if (hi > sizeof(buf)) hi = sizeof(buf);
    float ax[9];
    if (g_cam.fovOff >= 0 && (uint32_t)g_cam.fovOff + 4 > hi) hi = g_cam.fovOff + 4 <= (int)sizeof(buf) ? g_cam.fovOff + 4 : hi;
    if ((g_cam.hasVt && rdq(g_cam.obj) != g_cam.vt) || !rd(g_cam.obj, buf, hi) ||
        !cand_axes(buf, hi, g_cam.rotOff, g_cam.stride, g_cam.quat, ax)) {
        logf("camera: lost Rift Apart's camera object; looking for it again");
        g_cam = CamProbe();
        return false;
    }
    (void)lo;
    const float* t = (const float*)(buf + g_cam.posOff);
    for (int i = 0; i < 3; i++) {
        pos[i] = t[i];
        fwd[i] = ax[g_cam.fwd * 3 + i] * g_cam.fwdSign;
        up[i] = ax[g_cam.up * 3 + i];
    }
    if (up[1] < 0) up[0] = -up[0], up[1] = -up[1], up[2] = -up[2];
    memcpy(g_prevCamFwd, fwd, 12);
    memcpy(g_prevCamPos, pos, 12);
    up[0] = 0, up[1] = 1, up[2] = 0;  // no roll: Rift Apart's camera never tilts sideways
    g_raFovH = 0;
    if (g_cam.fovOff >= 0 && (uint32_t)g_cam.fovOff + 4 <= hi) {
        float f;
        memcpy(&f, buf + g_cam.fovOff, 4);
        if (f > 0.3f && f < 2.6f) g_raFovH = f;
    }
    return isfinite(pos[0] + pos[1] + pos[2]);
}

// ------------------------------------------------------------------------------------------
// Combat: while the fire button is held (mouse or pad trigger), Ratchet's shots hit the Elden
// Ring enemy closest to the centre of the view; melee hits the nearest enemy in front of him.
// Damage goes through the bridge's damage queue (same path the Minecraft side used).

static ErmcEntityTable* ents() { return (ErmcEntityTable*)(g_shm + ERMC_OFF_ENTITIES); }
static ErmcDamageQueue* dmgq() { return (ErmcDamageQueue*)(g_shm + ERMC_OFF_DAMAGE); }

static bool copy_entities(ErmcEntity* out, uint32_t* count) {
    ErmcEntityTable* t = ents();
    for (int tries = 0; tries < 4; tries++) {
        uint32_t s1 = t->seq;
        if (s1 & 1) { Sleep(0); continue; }
        MemoryBarrier();
        uint32_t n = t->count;
        if (n > ERMC_MAX_ENTITIES) n = ERMC_MAX_ENTITIES;
        memcpy(out, (const void*)t->entities, n * sizeof(ErmcEntity));
        MemoryBarrier();
        if (t->seq == s1) {
            *count = n;
            return true;
        }
    }
    return false;
}

static void push_damage(const ErmcEntity& e, float amount) {
    ErmcDamageQueue* q = dmgq();
    uint32_t w = q->write;
    ErmcDamage& d = q->ring[w % ERMC_DAMAGE_RING];
    d.id = e.id;
    d.amount = amount;
    memcpy(d.hitPos, e.boxCenter, 12);
    d.flags = ERMC_DAMAGE_OUTWARD;
    d.reserved = 0;
    MemoryBarrier();
    q->write = w + 1;
}

// Explosions: riftbridge (inside Rift Apart) follows Ratchet's bombs, grenades and rockets and reports
// which Elden Ring enemies were inside a blast (terrain.shm hits ring); they take damage here.
static void blast_tick() {
    if (!g_rt || g_rt->hitTag != RT_RUNES_TAG || g_cfg.blastDamage <= 0) return;
    static uint32_t seen = 0;
    static bool init = false;
    uint32_t w = g_rt->hitWrite;
    if (!init) {
        seen = w, init = true;
        logf("combat: explosions from Rift Apart hurt Elden Ring enemies (ExplosionDamage %.1f)", g_cfg.blastDamage);
        return;
    }
    if (w == seen) return;
    if (w - seen > 64) seen = w - 64;
    static ErmcEntity list[ERMC_MAX_ENTITIES];
    uint32_t n = 0;
    if (!copy_entities(list, &n)) return;
    static int logged = 0;
    for (; seen != w; seen++) {
        RtHeader::RtHit h;
        memcpy(&h, (const void*)&g_rt->hits[seen % 64], sizeof(h));
        if (!(h.factor > 0 && h.factor <= 1.0f)) continue;
        for (uint32_t i = 0; i < n; i++) {
            const ErmcEntity& e = list[i];
            if ((uint32_t)(e.id >> 3) != h.id || (e.flags & 1) || e.hp <= 0) continue;
            bool direct = (h.kind & RT_HIT_DIRECT) != 0;
            float amt = (direct ? g_cfg.shotDamage : g_cfg.blastDamage) * h.factor;
            push_damage(e, amt);
            if (logged++ < 80) logf("combat: %s %s for %.1f (HP %.0f/%.0f)", direct ? "direct hit on" : "explosion hit", e.name, amt, e.hp, e.maxHp);
            break;
        }
    }
}

struct PendingHit { uint64_t due, id; float amount; };
static PendingHit g_pending[64];
static int g_pendingN = 0;
// eye/fwd: Elden Ring camera; feet: the Tarnished; yawDeg: its facing (bridge convention).
static void combat_tick(uint64_t now, const float eye[3], const float fwd[3], const float feet[3], float yawDeg,
                        bool haveAim) {
    if (!g_cfg.combat) return;
    DWORD fg = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &fg);
    bool ours = fg && (fg == hdr()->hostPid || fg == g_raPid);
    XPad pad = {};
    bool havePad = pad_state(&pad);
    bool fire = ours && g_cfg.fireKey && (GetAsyncKeyState(g_cfg.fireKey) & 0x8000);
    bool melee = ours && g_cfg.meleeKey && (GetAsyncKeyState(g_cfg.meleeKey) & 0x8000);
    if (havePad) fire = fire || pad.rt > 120, melee = melee || (pad.buttons & 0x4000);  // RT, X (Square)
    static uint64_t nextShot = 0;
    static bool meleeWas = false;
    bool meleeNow = melee && !meleeWas;
    meleeWas = melee;
    bool shotNow = fire && now >= nextShot;
    if (g_pendingN) {  // shots in flight that have arrived
        static ErmcEntity cur[ERMC_MAX_ENTITIES];
        uint32_t cn = 0;
        bool haveList = copy_entities(cur, &cn);
        for (int k = 0; k < g_pendingN;) {
            if (g_pending[k].due > now) {
                k++;
                continue;
            }
            if (haveList)
                for (uint32_t i = 0; i < cn; i++)
                    if (cur[i].id == g_pending[k].id && !(cur[i].flags & 1) && cur[i].hp > 0) {
                        push_damage(cur[i], g_pending[k].amount);
                        break;
                    }
            g_pending[k] = g_pending[--g_pendingN];
        }
    }
    // Weapons whose shots riftbridge can follow report their real impacts (blast_tick); the aim-cone
    // guess is only for the rest (instant-hit guns), so misses no longer count.
    static uint32_t lastSpawns = 0;
    static uint64_t realShotsUntil = 0;
    if (g_rt && g_rt->hitTag == RT_RUNES_TAG) {
        uint32_t sp = g_rt->shotSpawns;
        if (sp != lastSpawns) lastSpawns = sp, realShotsUntil = now + 700;
    }
    if (shotNow && now < realShotsUntil) shotNow = false, nextShot = now + (uint64_t)(1000.0f / g_cfg.shotsPerSec);
    if (!shotNow && !meleeNow) return;
    if (shotNow) nextShot = now + (uint64_t)(1000.0f / g_cfg.shotsPerSec);
    static ErmcEntity list[ERMC_MAX_ENTITIES];
    uint32_t n = 0;
    if (!copy_entities(list, &n) || n == 0) return;
    static uint32_t hitsLogged = 0;
    if (shotNow && haveAim) {
        int bestI = -1;
        float bestScore = 1e9f;
        for (uint32_t i = 0; i < n; i++) {
            const ErmcEntity& e = list[i];
            if ((e.flags & 1) || e.hp <= 0 || e.kind == ERMC_ENT_OTHER) continue;
            float v[3] = {e.boxCenter[0] - eye[0], e.boxCenter[1] - eye[1], e.boxCenter[2] - eye[2]};
            float along = dot3(v, fwd);
            if (along < 0.5f || along > g_cfg.range + 6.0f) continue;
            float dist = sqrtf(dot3(v, v));
            float perp = sqrtf(fmaxf(0.0f, dist * dist - along * along));
            float r = fmaxf(e.boxHalf[0], fmaxf(e.boxHalf[1], e.boxHalf[2])) + 0.5f;
            float ang = atan2f(fmaxf(0.0f, perp - r), along) * 180.0f / 3.14159265f;
            if (ang > g_cfg.aimConeDeg) continue;
            float score = ang + along * 0.05f;
            if (score < bestScore) bestScore = score, bestI = (int)i;
        }
        if (bestI >= 0) {
            // The shot lands after it has travelled there (if the enemy is still alive then).
            const ErmcEntity& e = list[bestI];
            float v[3] = {e.boxCenter[0] - eye[0], e.boxCenter[1] - eye[1], e.boxCenter[2] - eye[2]};
            float dist = sqrtf(dot3(v, v));
            uint64_t due = now + (g_cfg.projSpeed > 1 ? (uint64_t)(dist / g_cfg.projSpeed * 1000.0f) : 0);
            if (g_pendingN < 64) g_pending[g_pendingN++] = {due, e.id, g_cfg.shotDamage};
            if (hitsLogged++ < 30 || hitsLogged % 50 == 0)
                logf("combat: shot at %s, %.0f m (HP %.0f/%.0f)", e.name, dist, e.hp, e.maxHp);
        }
    }
    if (meleeNow) {
        float a = yawDeg * 3.14159265f / 180.0f;
        float face[3] = {-sinf(a), 0, -cosf(a)};
        int bestI = -1;
        float bestD = 1e9f;
        for (uint32_t i = 0; i < n; i++) {
            const ErmcEntity& e = list[i];
            if ((e.flags & 1) || e.hp <= 0 || e.kind == ERMC_ENT_OTHER) continue;
            float v[3] = {e.boxCenter[0] - feet[0], 0, e.boxCenter[2] - feet[2]};
            float d = sqrtf(dot3(v, v));
            float r = fmaxf(e.boxHalf[0], e.boxHalf[2]);
            if (d - r > g_cfg.meleeRange || fabsf(e.boxCenter[1] - feet[1]) > 3.0f) continue;
            if (d > 0.3f && dot3(v, face) / d < -0.2f) continue;  // behind him (facing is approximate)
            if (d < bestD) bestD = d, bestI = (int)i;
        }
        if (bestI >= 0) {
            push_damage(list[bestI], g_cfg.meleeDamage);
            if (hitsLogged++ < 30 || hitsLogged % 50 == 0)
                logf("combat: melee hit %s (HP %.0f/%.0f)", list[bestI].name, list[bestI].hp, list[bestI].maxHp);
        }
    }
}

// ------------------------------------------------------------------------------------------
// Camera learning (one minute, in the background, once the real camera is found): which numbers
// in Rift Apart's camera objects follow the camera's turning (yaw) and tilting (pitch). Those are
// what Elden Ring's camera will drive next. Deltas are correlated, so wrapping angles don't matter.
static volatile LONG g_learnStarted = 0;
static DWORD WINAPI learn_thread(void*) {
    const uint64_t vts[] = {g_raBase + 0x45ae910, g_raBase + 0x45b14e0, g_raBase + 0x2ee6d60, g_raBase + 0x45aebb0,
                            g_raBase + 0x2f58808, g_raBase + 0x2ee3258, g_raBase + 0x2ee3fa8, g_raBase + 0x2ee2d80,
                            g_raBase + 0x2ee36b0};
    static uint64_t objs[400];
    int n = scan_vtables(vts, 9, objs, 400);
    CamProbe cam = g_cam;
    if (n <= 0 || !cam.obj) return 0;
    const int F = 0x400 / 4;  // floats per object
    struct Acc { double sx, sy, sxx, syy, sxy, sp, spp, sxp; int cnt; };
    std::vector<Acc> acc((size_t)n * F);
    for (auto& a : acc) memset(&a, 0, sizeof(a));
    std::vector<float> prev((size_t)n * F), cur((size_t)n * F);
    char nm[96];
    logf("camera learning: %d camera objects; turn the camera left/right and up/down now and then during the next minute", n);
    float prevYaw = 0, prevPitch = 0;
    bool havePrev = false;
    int used = 0;
    for (int it = 0; it < 1200; it++) {
        Sleep(50);
        uint8_t mb[0x200];
        float ax[9];
        if (!rd(cam.obj, mb, sizeof(mb)) || !cand_axes(mb, sizeof(mb), cam.rotOff, cam.stride, cam.quat, ax)) continue;
        float f[3] = {ax[cam.fwd * 3] * cam.fwdSign, ax[cam.fwd * 3 + 1] * cam.fwdSign, ax[cam.fwd * 3 + 2] * cam.fwdSign};
        float yaw = atan2f(f[0], f[2]), pitch = asinf(fmaxf(-1.0f, fminf(1.0f, f[1])));
        for (int o = 0; o < n; o++)
            if (!rd(objs[o], &cur[(size_t)o * F], F * 4)) memset(&cur[(size_t)o * F], 0, F * 4);
        if (havePrev) {
            float dy = yaw - prevYaw;
            if (dy > 3.14159265f) dy -= 6.2831853f;
            if (dy < -3.14159265f) dy += 6.2831853f;
            float dp = pitch - prevPitch;
            if (fabsf(dy) > 0.002f || fabsf(dp) > 0.002f) {
                used++;
                for (size_t k = 0; k < acc.size(); k++) {
                    float a = cur[k], b = prev[k];
                    if (!isfinite(a) || !isfinite(b) || fabsf(a) > 1e5f) continue;
                    float dx = a - b;
                    if (dx > 3.14159265f && dx < 6.3f) dx -= 6.2831853f;
                    if (dx < -3.14159265f && dx > -6.3f) dx += 6.2831853f;
                    Acc& c = acc[k];
                    c.sx += dx, c.sy += dy, c.sxx += (double)dx * dx, c.syy += (double)dy * dy, c.sxy += (double)dx * dy;
                    c.sp += dp, c.spp += (double)dp * dp, c.sxp += (double)dx * dp, c.cnt++;
                }
            }
        }
        prev.swap(cur);
        prevYaw = yaw, prevPitch = pitch, havePrev = true;
    }
    if (used < 30) {
        logf("camera learning: the camera hardly moved (%d moving samples); nothing learned", used);
        return 0;
    }
    auto report = [&](bool pitchMode) {
        struct R { double r, k; int o, off; };
        std::vector<R> best;
        for (int o = 0; o < n; o++)
            for (int fI = 0; fI < F; fI++) {
                const Acc& c = acc[(size_t)o * F + fI];
                if (c.cnt < 30) continue;
                double N = c.cnt;
                double sy = pitchMode ? c.sp : c.sy, syy = pitchMode ? c.spp : c.syy, sxy = pitchMode ? c.sxp : c.sxy;
                double vx = c.sxx - c.sx * c.sx / N, vy = syy - sy * sy / N, cxy = sxy - c.sx * sy / N;
                if (vx < 1e-9 || vy < 1e-9) continue;
                double r = cxy / sqrt(vx * vy);
                if (fabs(r) < 0.9) continue;
                best.push_back({r, cxy / vy, o, fI * 4});
            }
        std::sort(best.begin(), best.end(), [](const R& a, const R& b) { return fabs(a.r) > fabs(b.r); });
        for (size_t i = 0; i < best.size() && i < 12; i++) {
            uint64_t vt = rdq(objs[best[i].o]);
            if (!rtti_name(vt, nm, sizeof(nm))) strcpy(nm, "?");
            logf("camera learning: %s field %s +%x (object %llx): correlation %.3f, moves %.3f per radian", pitchMode ? "PITCH" : "YAW",
                 nm, best[i].off, (unsigned long long)objs[best[i].o], best[i].r, best[i].k);
        }
        if (best.empty()) logf("camera learning: no %s field found", pitchMode ? "pitch" : "yaw");
    };
    report(false);
    report(true);
    logf("camera learning: done (%d moving samples)", used);
    return 0;
}

// Elden Ring enemies -> Rift Apart: their hitboxes as solid boxes, so Ratchet's rockets, grenades and
// bullets explode on them (and he bumps into them) instead of flying through.
static void enemies_tick(const float r0[3], const float e0[3], const float tarnished[3]) {
    if (!g_rt || !g_cfg.terrain) return;
    static ErmcEntity list[ERMC_MAX_ENTITIES];
    uint32_t n = 0;
    if (!copy_entities(list, &n)) return;
    RtEnemy out[RT_MAX_ENEMIES];
    uint32_t cnt = 0;
    float s = g_cfg.scale != 0 ? g_cfg.scale : 1.0f, sy = s * (g_cfg.heightScale != 0 ? g_cfg.heightScale : 1.0f);
    for (uint32_t i = 0; i < n && cnt < RT_MAX_ENEMIES; i++) {
        const ErmcEntity& e = list[i];
        if ((e.flags & 1) || e.hp <= 0 || e.kind == ERMC_ENT_OTHER) continue;
        float dx = e.boxCenter[0] - tarnished[0], dz = e.boxCenter[2] - tarnished[2];
        if (dx * dx + dz * dz > 70.0f * 70.0f) continue;
        float d[3] = {e.boxCenter[0] - e0[0], e.boxCenter[1] - e0[1], e.boxCenter[2] - e0[2]}, ra[3];
        unmap_delta(d, ra, false);
        float fwd[3] = {0, 0, 1};
        if (!(e.flags & 2)) {
            float x = e.quat[0], y = e.quat[1], z = e.quat[2], w = e.quat[3];
            fwd[0] = 2 * (x * z + w * y), fwd[1] = 2 * (y * z - w * x), fwd[2] = 1 - 2 * (x * x + y * y);
        }
        float fr[3];
        unmap_delta(fwd, fr, true);
        RtEnemy& o = out[cnt++];
        for (int k = 0; k < 3; k++) o.center[k] = r0[k] + ra[k];
        o.center[1] += g_terrainYOffset;
        // Elden Ring's hit capsules are slim; Rift Apart's bombs and slow shots need a fuller target
        // (box a bit wider and taller than the capsule, never thinner than 1 m across / 1.4 m tall).
        o.half[0] = fmaxf(e.boxHalf[0] * 1.4f, 0.5f) / s, o.half[1] = fmaxf(e.boxHalf[1] * 1.15f, 0.7f) / sy;
        o.half[2] = fmaxf(e.boxHalf[2] * 1.4f, 0.5f) / s;
        o.yaw = atan2f(fr[0], fr[2]);
        o.id = (uint32_t)(e.id >> 3);
    }
    uint32_t q = g_rt->enemySeq;
    g_rt->enemySeq = q + 1;
    MemoryBarrier();
    memcpy((void*)g_rt->enemies, out, cnt * sizeof(RtEnemy));
    g_rt->enemyCount = cnt;
    MemoryBarrier();
    g_rt->enemySeq = q + 2;
}

static bool key_pressed(int vk, bool* last) {
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool edge = down && !*last;
    *last = down;
    return edge;
}


// ------------------------------------------------------------------------------------------
// Capture test (Insert): grab Rift Apart's window with PrintWindow(PW_RENDERFULLCONTENT) into
// riftbridge\capture-N.bmp, and time 20 grabs, to see whether the 3D picture and HUD come through
// and how fast this path is.

struct FindWin { DWORD pid; HWND best; long area; };
static BOOL CALLBACK find_win_cb(HWND h, LPARAM lp) {
    FindWin* f = (FindWin*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != f->pid || !IsWindowVisible(h)) return TRUE;
    RECT r;
    GetClientRect(h, &r);
    long a = (r.right - r.left) * (r.bottom - r.top);
    if (a > f->area) f->area = a, f->best = h;
    return TRUE;
}

static bool grab(HWND h, int w, int ht, HDC mem) {
    return PrintWindow(h, mem, 2 /* PW_RENDERFULLCONTENT */) != 0;
    (void)w; (void)ht;
}

static void capture_test(int n) {
    if (!g_raPid) {
        logf("capture: Rift Apart isn't running");
        return;
    }
    FindWin f = {g_raPid, nullptr, 0};
    EnumWindows(find_win_cb, (LPARAM)&f);
    if (!f.best) {
        logf("capture: no Rift Apart window found");
        return;
    }
    RECT r;
    GetClientRect(f.best, &r);
    int w = r.right - r.left, h = r.bottom - r.top;
    HDC wdc = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(wdc);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = h;  // bottom-up
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    LARGE_INTEGER f0, t0, t1;
    QueryPerformanceFrequency(&f0);
    QueryPerformanceCounter(&t0);
    bool ok = grab(f.best, w, h, mem);
    QueryPerformanceCounter(&t1);
    double first = (t1.QuadPart - t0.QuadPart) * 1000.0 / f0.QuadPart;
    // Is the picture black (capture failed silently)?
    uint64_t sum = 0;
    const uint32_t* px = (const uint32_t*)bits;
    for (int i = 0; i < w * h; i += 97) sum += (px[i] & 0xFF) + ((px[i] >> 8) & 0xFF) + ((px[i] >> 16) & 0xFF);
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\capture-%d.bmp", g_exeDir, n);
    FILE* o = fopen(path, "wb");
    if (o) {
        BITMAPFILEHEADER fh = {};
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
        fh.bfSize = fh.bfOffBits + (DWORD)w * h * 4;
        fwrite(&fh, sizeof(fh), 1, o);
        fwrite(&bi.bmiHeader, sizeof(bi.bmiHeader), 1, o);
        fwrite(bits, 4, (size_t)w * h, o);
        fclose(o);
    }
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < 20; i++) grab(f.best, w, h, mem);
    QueryPerformanceCounter(&t1);
    double avg = (t1.QuadPart - t0.QuadPart) * 1000.0 / f0.QuadPart / 20.0;
    logf("capture %d: %s, window %dx%d, first grab %.1f ms, average %.1f ms (%.0f per second), brightness %llu -> %s",
         n, ok ? "ok" : "PrintWindow failed", w, h, first, avg, avg > 0 ? 1000.0 / avg : 0.0,
         (unsigned long long)sum, path);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, wdc);
}

// ------------------------------------------------------------------------------------------
// Ratchet overlay: a transparent, click-through, always-on-top window over Elden Ring's client
// area showing Rift Apart's picture (PrintWindow capture, works while Rift Apart is behind other
// windows). Insert cycles: off -> see-through full picture (for alignment) -> cut-out -> off.
// Cut-out keeps pixels that differ from the background colour sampled at the picture's top-left
// corner; it's meant for when Rift Apart's level is hidden (riftbridge test: Delete).

static volatile LONG g_ovMode = 0;  // 0 off, 1 see-through, 2 cut-out
static const char* kOvNames[] = {"off", "see-through (alignment check)", "cut-out (Ratchet + HUD only)"};

struct Dib {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    uint32_t* px = nullptr;
    int w = 0, h = 0;
    void make(int nw, int nh) {
        if (nw == w && nh == h && dc) return;
        free();
        HDC sdc = GetDC(nullptr);
        dc = CreateCompatibleDC(sdc);
        ReleaseDC(nullptr, sdc);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = nw;
        bi.bmiHeader.biHeight = -nh;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&px, nullptr, 0);
        old = SelectObject(dc, bmp);
        w = nw, h = nh;
    }
    void free() {
        if (dc) {
            SelectObject(dc, old);
            DeleteObject(bmp);
            DeleteDC(dc);
        }
        dc = nullptr, bmp = nullptr, px = nullptr, w = h = 0;
    }
};

static bool er_client_rect(RECT* r) {
    ErmcGameState st;
    if (g_shm && host_ready() && read_state(&st) && (st.flags & ERMC_STATE_WINDOW_VALID) && st.winW > 0 && st.winH > 0) {
        r->left = st.winX, r->top = st.winY, r->right = st.winX + st.winW, r->bottom = st.winY + st.winH;
        return true;
    }
    return false;
}

// Where Rift Apart's client area sits inside the captured window image (title bar / borders).
static void client_offset(HWND h, int* ox, int* oy, int* cw, int* ch) {
    RECT frame, rc;
    POINT o = {0, 0};
    GetClientRect(h, &rc);
    ClientToScreen(h, &o);
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) GetWindowRect(h, &frame);
    *ox = o.x - frame.left, *oy = o.y - frame.top, *cw = rc.right, *ch = rc.bottom;
}

// ---------------------------------------------------------------- Ratchet-style health bars
// Drawn into the overlay on top of Elden Ring: a small bar over every hurt (or close) enemy, and a
// wide boss bar at the top for big, high-HP enemies in a fight. Positions come from Elden Ring's
// entity table and camera (bridge.shm). Off: HealthBars=0 in [hud].
static inline void blend_px(uint32_t* d, uint32_t rgb, int a) {  // a 0..255, straight colour over (premultiplied) dst
    if (a <= 0) return;
    uint32_t o = *d;
    int oa = o >> 24, orr = (o >> 16) & 255, og = (o >> 8) & 255, ob = o & 255;
    int r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
    int ia = 255 - a;
    int na = a + oa * ia / 255;
    int nr = (r * a + orr * ia) / 255, ng = (g * a + og * ia) / 255, nb = (b * a + ob * ia) / 255;
    *d = ((uint32_t)na << 24) | ((uint32_t)nr << 16) | ((uint32_t)ng << 8) | (uint32_t)nb;
}
static void fill_rect(uint32_t* px, int W, int H, int x, int y, int w, int h, uint32_t rgb, int a, int slant = 0) {
    // slant > 0: parallelogram leaning right by `slant` px over the height (Rift Apart's bars are slanted)
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= H) continue;
        int off = slant ? slant * (h - 1 - j) / (h > 1 ? h - 1 : 1) : 0;
        for (int i = 0; i < w; i++) {
            int xx = x + i + off;
            if (xx < 0 || xx >= W) continue;
            blend_px(px + (size_t)yy * W + xx, rgb, a);
        }
    }
}
static uint32_t lerp_rgb(uint32_t a, uint32_t b, float t) {
    auto ch = [&](int s) { return (uint32_t)(((a >> s) & 255) + (((int)((b >> s) & 255) - (int)((a >> s) & 255)) * t)) & 255; };
    return (ch(16) << 16) | (ch(8) << 8) | ch(0);
}
static void bar_fill(uint32_t* px, int W, int H, int x, int y, int w, int h, int slant, uint32_t c0, uint32_t c1, int a) {
    for (int i = 0; i < w; i++) fill_rect(px, W, H, x + i, y, 1, h, lerp_rgb(c0, c1, w > 1 ? (float)i / (w - 1) : 0), a, slant);
}

// Elden Ring character model (cXXXX) -> boss name, for the boss bar. Names follow the in-game boss
// titles; models shared by several bosses are told apart by max HP where that's reliable.
struct BossName { int model; const char* name; };
static const BossName kBossNames[] = {
    {2030, "Rennala, Queen of the Full Moon"}, {2031, "Rennala, Queen of the Full Moon"}, {2050, "Ranni the Witch"},
    {2100, "Black Knife Assassin"}, {2110, "Beast Clergyman"}, {2120, "Malenia, Blade of Miquella"},
    {2190, "Radagon of the Golden Order"}, {2200, "Elden Beast"}, {2500, "Crucible Knight"},
    {3100, "Bell Bearing Hunter"}, {3150, "Night's Cavalry"}, {3181, "Red Wolf of Radagon"},
    {3250, "Draconic Tree Sentinel"}, {3251, "Tree Sentinel"}, {6251, "Tree Sentinel"}, {3252, "Royal Knight Loretta"},
    {3300, "Nox Swordstress"}, {3320, "Mimic Tear"}, {3350, "Crystalian"}, {3400, "Grave Warden Duelist"},
    {3451, "Scaly Misbegotten"}, {3460, "Leonine Misbegotten"}, {3560, "Godskin Apostle"}, {3570, "Godskin Noble"},
    {3600, "Alabaster Lord"}, {3664, "Cemetery Shade"}, {3800, "Cleanrot Knight"}, {3810, "Kindred of Rot"},
    {3970, "Beastman of Farum Azula"}, {4020, "Royal Revenant"}, {4120, "Demi-Human Chief"},
    {4130, "Demi-Human Queen"}, {4140, "Spirit-Caller Snail"}, {4260, "Erdtree Burial Watchdog"},
    {4270, "Lion Guardian"}, {4290, "Bloodhound Knight"}, {4340, "Mad Pumpkin Head"}, {4341, "Mad Pumpkin Head"},
    {4470, "Abductor Virgin"}, {4480, "Miranda the Blighted Bloom"}, {4492, "Knight of the Great Jar"},
    {4500, "Flying Dragon"}, {4501, "Decaying Ekzykes"}, {4502, "Glintstone Dragon"},
    {4503, "Borealis the Freezing Fog"}, {4504, "Elder Dragon Greyoll"}, {4510, "Ancient Dragon"},
    {4511, "Lichdragon Fortissax"}, {4520, "Dragonlord Placidusax"}, {4600, "Troll"}, {4602, "Snowfield Troll"},
    {4603, "Stonedigger Troll"}, {4620, "Astel, Naturalborn of the Void"}, {4630, "Runebear"},
    {4640, "Ulcerated Tree Spirit"}, {4650, "Dragonkin Soldier"}, {4660, "Guardian Golem"},
    {4670, "Ancestor Spirit"}, {4680, "Fallingstar Beast"}, {4690, "Grafted Scion"},
    {4710, "Rykard, Lord of Blasphemy"}, {4720, "Godfrey, First Elden Lord"}, {4721, "Hoarah Loux, Warrior"},
    {4730, "Starscourge Radahn"}, {4750, "Godrick the Grafted"}, {4760, "Fire Giant"}, {4770, "Valiant Gargoyle"},
    {4800, "Mohg, Lord of Blood"}, {4801, "Mohg, the Omen"}, {4810, "Erdtree Avatar"}, {4811, "Putrid Avatar"},
    {4820, "Omenkiller"}, {4910, "Magma Wyrm"}, {4911, "Great Wyrm Theodorix"}, {4950, "Tibia Mariner"},
    {4980, "Death Rite Bird"}, {6260, "Death Rite Bird"}, {6310, "Fallingstar Beast"},
    {5000, "Commander Gaius"}, {5020, "Putrescent Knight"}, {5030, "Romina, Saint of the Bud"},
    {5040, "Curseblade Labirith"}, {5050, "Midra, Lord of Frenzied Flame"}, {5051, "Midra, Lord of Frenzied Flame"},
    {5070, "Death Knight"}, {5120, "Bayle the Dread"}, {5130, "Messmer the Impaler"}, {5131, "Messmer the Impaler"},
    {5132, "Messmer the Impaler"}, {5170, "Furnace Golem"}, {5200, "Metyr, Mother of Fingers"},
    {5210, "Divine Beast Dancing Lion"}, {5220, "Promised Consort Radahn"}, {5230, "Scadutree Avatar"},
    {5300, "Rellana, Twin Moon Knight"}, {5312, "Jori, Elder Inquisitor"}, {5330, "St. Trina"},
    {5370, "Ancient Dragon Senessax"}, {5580, "Jagged Peak Drake"}, {5800, "Crucible Knight Devonia"},
    {5810, "Demi-Human Swordmaster Onze"}, {5820, "Great Red Bear"}, {5840, "Black Knight"},
    {5860, "Ghostflame Dragon"}, {5910, "Hornsent Grandam"},
};
static int entity_model(const ErmcEntity& e) {
    int m = -1;
    if (e.name[0] == 'c' && sscanf(e.name + 1, "%d", &m) == 1) return m;
    return -1;
}
// nullptr: not a named boss model
static const char* boss_name(const ErmcEntity& e) {
    int m = entity_model(e);
    if (m == 2130) return e.maxHp < 7000 ? "Margit, the Fell Omen" : "Morgott, the Omen King";
    if (m == 3050) return e.maxHp < 5000 ? "Commander O'Neil" : "Commander Niall";
    if (m == 4500 && e.maxHp > 6000) return "Flying Dragon";
    for (const BossName& b : kBossNames)
        if (b.model == m) return b.name;
    return nullptr;
}

struct BarMem { uint64_t id; float shown, chip; DWORD lastHit, seen; };
static std::vector<BarMem> g_barMem;
static Dib g_txt;
static void draw_text(uint32_t* px, int W, int H, int x, int y, const char* s, int size, uint32_t rgb) {
    int tw = size * (int)strlen(s), th = size + size / 2;
    if (tw < 8) return;
    g_txt.make(tw, th);
    memset(g_txt.px, 0, (size_t)tw * th * 4);
    static HFONT font = nullptr;
    static int fontSize = 0;
    if (!font || fontSize != size) {
        if (font) DeleteObject(font);
        font = CreateFontA(-size, 0, 0, 0, FW_BLACK, TRUE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           ANTIALIASED_QUALITY, DEFAULT_PITCH, "Segoe UI Black");
        fontSize = size;
    }
    HGDIOBJ of = SelectObject(g_txt.dc, font);
    SetBkMode(g_txt.dc, TRANSPARENT);
    SetTextColor(g_txt.dc, RGB(255, 255, 255));
    TextOutA(g_txt.dc, 0, 0, s, (int)strlen(s));
    SelectObject(g_txt.dc, of);
    GdiFlush();
    for (int j = 0; j < th; j++)
        for (int i = 0; i < tw; i++) {
            uint32_t c = g_txt.px[(size_t)j * tw + i];
            int a = (c >> 8) & 255;
            int xx = x + i, yy = y + j;
            if (a && xx >= 0 && yy >= 0 && xx < W && yy < H) {
                if (xx + 2 < W && yy + 2 < H) blend_px(px + (size_t)(yy + 2) * W + xx + 2, 0x000000, a * 3 / 5);  // drop shadow
                blend_px(px + (size_t)yy * W + xx, rgb, a);
            }
        }
}
static void draw_health_bars(uint32_t* px, int W, int H) {
    if (!g_cfg.healthBars || !g_shm || !host_ready()) return;
    ErmcGameState st;
    if (!read_state(&st) || !(st.flags & ERMC_STATE_CAMERA_VALID)) return;
    static ErmcEntity list[ERMC_MAX_ENTITIES];
    uint32_t n = 0;
    if (!copy_entities(list, &n)) return;
    float f[3] = {st.camTarget[0] - st.camPos[0], st.camTarget[1] - st.camPos[1], st.camTarget[2] - st.camPos[2]};
    float fl = sqrtf(dot3(f, f));
    if (!(fl > 1e-4f)) return;
    for (float& v : f) v /= fl;
    float up[3] = {0, 1, 0};
    // Elden Ring is left-handed: right = up x forward (BarsMirror=1 flips it)
    float r[3] = {up[1] * f[2] - up[2] * f[1], up[2] * f[0] - up[0] * f[2], up[0] * f[1] - up[1] * f[0]};
    if (g_cfg.barsMirror) r[0] = -r[0], r[1] = -r[1], r[2] = -r[2];
    float rl = sqrtf(dot3(r, r));
    if (!(rl > 1e-4f)) return;
    for (float& v : r) v /= rl;
    float u[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};
    if (u[1] < 0) u[0] = -u[0], u[1] = -u[1], u[2] = -u[2];
    float fov = (st.fovYDeg > 5 && st.fovYDeg < 150 ? st.fovYDeg : 48.0f) * 3.14159265f / 180.0f;
    float ty = tanf(fov * 0.5f), aspect = (float)W / H;
    float k = H / 1080.0f;
    DWORD now = GetTickCount();
    // boss: the biggest-HP large enemy that's in the fight (hurt or near)
    int boss = -1;
    for (uint32_t i = 0; i < n; i++) {
        const ErmcEntity& e = list[i];
        if ((e.flags & 1) || e.hp <= 0 || e.kind == ERMC_ENT_OTHER) continue;
        bool named = boss_name(e) != nullptr;
        if (!(named ? e.maxHp >= 600 : (e.kind == ERMC_ENT_LARGE_MONSTER && e.maxHp >= g_cfg.bossHp))) continue;
        float d[3] = {e.boxCenter[0] - st.camPos[0], e.boxCenter[1] - st.camPos[1], e.boxCenter[2] - st.camPos[2]};
        if (dot3(d, d) > 60.0f * 60.0f) continue;
        if (e.hp >= e.maxHp && dot3(d, d) > 25.0f * 25.0f) continue;
        if (boss < 0 || (named && !boss_name(list[boss])) || ((named == (boss_name(list[boss]) != nullptr)) && e.maxHp > list[boss].maxHp)) boss = (int)i;
    }
    auto mem_for = [&](const ErmcEntity& e) -> BarMem& {
        for (auto& m : g_barMem)
            if (m.id == e.id) {
                m.seen = now;
                return m;
            }
        float fr = e.maxHp > 0 ? e.hp / e.maxHp : 0;
        g_barMem.push_back({e.id, fr, fr, 0, now});
        return g_barMem.back();
    };
    auto update_mem = [&](BarMem& m, float frac) {
        if (frac < m.shown - 0.0005f) m.lastHit = now;
        m.shown = frac;
        if (m.chip < frac) m.chip = frac;
        if (now - m.lastHit > 450 && m.chip > frac) m.chip = fmaxf(frac, m.chip - 0.012f);
    };
    for (uint32_t i = 0; i < n; i++) {
        const ErmcEntity& e = list[i];
        if ((e.flags & 1) || e.hp <= 0 || e.kind == ERMC_ENT_OTHER || e.maxHp <= 0 || (int)i == boss) continue;
        float top[3] = {e.boxCenter[0], e.boxCenter[1] + e.boxHalf[1] + 0.35f, e.boxCenter[2]};
        float d[3] = {top[0] - st.camPos[0], top[1] - st.camPos[1], top[2] - st.camPos[2]};
        float z = dot3(d, f);
        float dist = sqrtf(dot3(d, d));
        if (z < 0.8f || dist > 45.0f) continue;
        float frac = e.hp / e.maxHp;
        BarMem& m = mem_for(e);
        update_mem(m, frac);
        if (frac >= 0.999f && dist > 14.0f && now - m.lastHit > 6000) continue;  // untouched and far: no bar
        int sx = (int)(W * 0.5f * (1 + dot3(d, r) / (z * ty * aspect)));
        int sy = (int)(H * 0.5f * (1 - dot3(d, u) / (z * ty)));
        float sc = fminf(1.25f, fmaxf(0.6f, 9.0f / fmaxf(dist, 1.0f)));
        int bw = (int)(130 * k * sc), bh = (int)fmaxf(5, 11 * k * sc), sl = bh / 2;
        int x = sx - bw / 2, y = sy - bh;
        if (x + bw < 0 || x > W || y + bh < 0 || y > H) continue;
        fill_rect(px, W, H, x - 3, y - 3, bw + 6, bh + 6, 0x08121e, 210, sl);      // dark frame
        fill_rect(px, W, H, x - 1, y - 1, bw + 2, bh + 2, 0xffffff, 60, sl);       // thin light rim
        fill_rect(px, W, H, x, y, bw, bh, 0x1b2735, 235, sl);                        // empty track
        int cw = (int)(bw * m.chip), fw = (int)(bw * frac);
        if (cw > fw) fill_rect(px, W, H, x + fw, y, cw - fw, bh, 0xfff2b0, 235, sl);  // recent damage chip
        bar_fill(px, W, H, x, y, fw, bh, sl, 0xff2a3d, 0xff8a1e, 255);
        fill_rect(px, W, H, x, y, fw, (bh + 3) / 4, 0xffffff, 70, sl);              // shine
    }
    if (boss >= 0) {
        const ErmcEntity& e = list[boss];
        float frac = e.hp / e.maxHp;
        BarMem& m = mem_for(e);
        update_mem(m, frac);
        int bw = (int)(W * 0.42f), bh = (int)(20 * k), sl = (int)(12 * k);
        int x = (W - bw) / 2, y = (int)(H * 0.085f);
        fill_rect(px, W, H, x - 6, y - 6, bw + 12, bh + 12, 0x07101c, 215, sl);
        fill_rect(px, W, H, x - 3, y - 3, bw + 6, bh + 6, 0x2ff3e0, 230, sl);       // teal frame
        fill_rect(px, W, H, x - 1, y - 1, bw + 2, bh + 2, 0x07101c, 255, sl);
        fill_rect(px, W, H, x, y, bw, bh, 0x1a1530, 240, sl);
        int cw = (int)(bw * m.chip), fw = (int)(bw * frac);
        if (cw > fw) fill_rect(px, W, H, x + fw, y, cw - fw, bh, 0xffe9a8, 240, sl);
        bar_fill(px, W, H, x, y, fw, bh, sl, 0x8a3dff, 0xff3fa4, 255);
        fill_rect(px, W, H, x, y, fw, bh / 4, 0xffffff, 80, sl);
        for (int t = 1; t < 10; t++) fill_rect(px, W, H, x + bw * t / 10, y + bh / 2, (int)fmaxf(1, 2 * k), bh - bh / 2, 0x07101c, 120, 0);  // ticks
        char label[96];
        const char* bn = boss_name(e);
        if (g_cfg.bossName[0]) snprintf(label, sizeof(label), "%s", g_cfg.bossName);
        else snprintf(label, sizeof(label), "%s", bn ? bn : "BOSS");
        for (char* c = label; *c; c++) *c = (char)toupper((unsigned char)*c);  // Rift Apart's boss titles are capitals
        static int lastModel = -2;
        if (entity_model(e) != lastModel) {
            lastModel = entity_model(e);
            logf("boss bar: %s (model %s, %d HP)", label, e.name, (int)e.maxHp);
        }
        draw_text(px, W, H, x + sl, y - (int)(34 * k), label, (int)(26 * k), 0xffffff);
        char hp[48];
        snprintf(hp, sizeof(hp), "%d / %d", (int)e.hp, (int)e.maxHp);
        draw_text(px, W, H, x + bw - (int)(150 * k), y + bh + (int)(6 * k), hp, (int)(16 * k), 0x9ff7ee);
    }
    for (size_t i = 0; i < g_barMem.size();)
        if (now - g_barMem[i].seen > 10000) g_barMem.erase(g_barMem.begin() + i);
        else i++;
}

static DWORD WINAPI overlay_thread(void*) {
    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "RiftlinkOverlay";
    RegisterClassA(&wc);
    HWND ov = CreateWindowExA(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                              wc.lpszClassName, "Ratchet overlay", WS_POPUP, 0, 0, 16, 16, nullptr, nullptr,
                              wc.hInstance, nullptr);
    Dib cap, out;
    bool shown = false, wgcOk = false, wgcFailed = false;
    HWND wgcFor = nullptr;
    uint64_t frames = 0, lastReport = GetTickCount64();
    std::vector<int> xmap;
    for (;;) {
        MSG m;
        while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageA(&m);
        LONG mode = g_ovMode;
        RECT er;
        HWND ra = nullptr;
        if (mode != 0 && g_raPid) {
            FindWin f = {g_raPid, nullptr, 0};
            EnumWindows(find_win_cb, (LPARAM)&f);
            ra = f.best;
        }
        if (mode == 0 || !ra || !er_client_rect(&er)) {
            if (shown) ShowWindow(ov, SW_HIDE), shown = false;
            if (wgcOk && mode == 0) wgc_stop(), wgcOk = false, wgcFor = nullptr;
            Sleep(50);
            continue;
        }
        int ew = er.right - er.left, eh = er.bottom - er.top;
        if (ew < 16 || eh < 16) {
            Sleep(50);
            continue;
        }
        // Rift Apart's picture: Windows Graphics Capture (fast, works behind other windows), else PrintWindow.
        const uint32_t* src = nullptr;
        int sw = 0, sh = 0, sx0 = 0, sy0 = 0, rw = 0, rh = 0;
        if (!wgcFailed && (ra != wgcFor || !wgcOk)) {
            char err[160] = {};
            wgcOk = wgc_start(ra, err, sizeof(err)) != 0;
            wgcFor = ra;
            if (!wgcOk) {
                wgcFailed = true;
                logf("overlay: fast capture unavailable (%s); using the slower PrintWindow", err);
            } else {
                logf("overlay: fast capture (Windows Graphics Capture) of Rift Apart's window");
            }
        }
        if (wgcOk) {
            if (!wgc_frame(&src, &sw, &sh)) {
                Sleep(2);  // no new frame yet
                continue;
            }
            int cw, ch;
            client_offset(ra, &sx0, &sy0, &cw, &ch);
            if (sx0 < 0 || sy0 < 0 || sx0 + cw > sw || sy0 + ch > sh) sx0 = sy0 = 0, cw = sw, ch = sh;
            rw = cw, rh = ch;
        } else {
            RECT rc;
            GetClientRect(ra, &rc);
            rw = rc.right, rh = rc.bottom;
            if (rw < 16 || rh < 16) {
                Sleep(50);
                continue;
            }
            cap.make(rw, rh);
            PrintWindow(ra, cap.dc, 2 /* PW_RENDERFULLCONTENT */);
            src = cap.px, sw = rw, sh = rh;
        }
        // Rift Apart's picture must be the same size as Elden Ring's, or every frame gets resampled (soft,
        // blocky effects). Resize Rift Apart's window so its inside matches exactly ([overlay] MatchSize).
        {
            static DWORD lastFix = 0;
            static int fixes = 0;
            if (g_cfg.matchSize && (rw != ew || rh != eh) && GetTickCount() - lastFix > 3000 && fixes < 5) {
                lastFix = GetTickCount();
                LONG st = GetWindowLongA(ra, GWL_STYLE), ex = GetWindowLongA(ra, GWL_EXSTYLE);
                if (!(st & WS_MAXIMIZE)) {
                    RECT want = {0, 0, ew, eh};
                    AdjustWindowRectEx(&want, (DWORD)st, FALSE, (DWORD)ex);
                    SetWindowPos(ra, nullptr, 0, 0, want.right - want.left, want.bottom - want.top,
                                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                    fixes++;
                    logf("overlay: resized Rift Apart's window inside from %dx%d to %dx%d (same as Elden Ring: no resampling)", rw, rh, ew, eh);
                } else if (fixes++ == 0) {
                    logf("overlay: Rift Apart is maximized at %dx%d; un-maximize it so its picture can match Elden Ring's %dx%d", rw, rh, ew, eh);
                }
            }
        }
        DWORD t0 = GetTickCount();
        out.make(ew, eh);
        // Fit Rift Apart's picture into Elden Ring's client area, same centre, keep aspect (crop).
        double s = (double)ew / rw > (double)eh / rh ? (double)ew / rw : (double)eh / rh;
        int vw = (int)(ew / s), vh = (int)(eh / s);
        int bx = sx0 + (rw - vw) / 2, by = sy0 + (rh - vh) / 2;
        // Bilinear scaling (16.16 fixed point) into Elden Ring's size.
        if ((int)xmap.size() != ew) xmap.resize(ew);
        static std::vector<int> xfrac;
        if ((int)xfrac.size() != ew) xfrac.resize(ew);
        for (int x = 0; x < ew; x++) {
            double fx = bx + (x + 0.5) * vw / ew - 0.5;
            if (fx < 0) fx = 0;
            int ix = (int)fx;
            if (ix > sw - 2) ix = sw - 2;
            xmap[x] = ix, xfrac[x] = (int)((fx - ix) * 256);
        }
        uint32_t* p = out.px;
        // Background level: Rift Apart's "dark" isn't pure black (fog, ambient light). Measure it on the
        // picture's edges (middle of each side, away from the HUD corners) and cut just above it, so no
        // grey haze is laid over Elden Ring.
        static float bgEma = 0;
        {
            std::vector<int> v;
            v.reserve(256);
            for (int k = 0; k < 64; k++) {
                int yy = sh * (30 + k * 40 / 64) / 100, xx = sw * (30 + k * 40 / 64) / 100;
                const uint32_t pts[4] = {src[(size_t)yy * sw + sw / 50], src[(size_t)yy * sw + sw - 1 - sw / 50],
                                         src[(size_t)(sh / 30) * sw + xx], src[(size_t)(sh - 1 - sh / 30) * sw + xx]};
                for (uint32_t c : pts) {
                    int r = (c >> 16) & 255, gg = (c >> 8) & 255, b = c & 255;
                    v.push_back(r > gg ? (r > b ? r : b) : (gg > b ? gg : b));
                }
            }
            std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
            float bg = (float)v[v.size() / 2];
            bgEma = bgEma <= 0 ? bg : bgEma * 0.9f + bg * 0.1f;
        }
        int cut = g_cfg.darkCut, full = g_cfg.darkFull;
        if (g_cfg.keyAuto && bgEma + 6 > cut) {
            int shift = (int)(bgEma + 6) - cut;
            cut += shift, full += shift;
            if (full > 250) full = 250;
            if (cut >= full) cut = full - 1;
        }
        // Ratchet is rendered in darkness (sunk under the level), so lift his colours: levels from just
        // under the cut-out threshold, then gamma and gain. Ctrl+F9 / Ctrl+F10 = darker / brighter.
        {
            static bool k9 = false, k10 = false;
            bool ctl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            bool d9 = ctl && (GetAsyncKeyState(VK_F9) & 0x8000), d10 = ctl && (GetAsyncKeyState(VK_F10) & 0x8000);
            if ((d9 && !k9) || (d10 && !k10)) {
                g_cfg.brightness = fminf(3.0f, fmaxf(0.5f, g_cfg.brightness + (d10 ? 0.1f : -0.1f)));
                char ini[MAX_PATH], v[32];
                snprintf(ini, sizeof(ini), "%s\\riftlink.ini", g_exeDir);
                snprintf(v, sizeof(v), "%.2f", g_cfg.brightness);
                WritePrivateProfileStringA("overlay", "Brightness", v, ini);
                logf("overlay: Ratchet brightness %.2f (Ctrl+F9 darker, Ctrl+F10 brighter; saved)", g_cfg.brightness);
            }
            k9 = d9, k10 = d10;
        }
        static uint8_t lut[256];
        {
            float lo = cut * 0.6f, span = 255.0f - lo;
            for (int i = 0; i < 256; i++) {
                float t = (i - lo) / span;
                t = t <= 0 ? 0 : powf(t, g_cfg.gammaLift) * g_cfg.brightness;
                lut[i] = (uint8_t)(t >= 1.0f ? 255 : t * 255.0f);
            }
        }
        uint32_t kr = 0, kg = 0, kb = 0;
        if (mode == 2 && !g_cfg.keyDark) {  // corner mode: background = average of the top-left 16x16
            for (int y = 0; y < 16; y++)
                for (int x = 0; x < 16; x++) {
                    uint32_t c = src[(size_t)(by + (int)((y + 0.5) * vh / eh)) * sw + xmap[x]];
                    kr += (c >> 16) & 255, kg += (c >> 8) & 255, kb += c & 255;
                }
            kr /= 256, kg /= 256, kb /= 256;
        }
        for (int y = 0; y < eh; y++) {
            double fy = by + (y + 0.5) * vh / eh - 0.5;
            if (fy < 0) fy = 0;
            int iy = (int)fy;
            if (iy > sh - 2) iy = sh - 2;
            int wy = (int)((fy - iy) * 256);
            const uint32_t* r0p = src + (size_t)iy * sw;
            const uint32_t* r1p = r0p + sw;
            uint32_t* o = p + (size_t)y * ew;
            for (int x = 0; x < ew; x++) {
                int ix = xmap[x], wx = xfrac[x];
                uint32_t c00 = r0p[ix], c01 = r0p[ix + 1], c10 = r1p[ix], c11 = r1p[ix + 1];
                auto lerp = [&](int sh8) {
                    int a0 = (c00 >> sh8) & 255, a1 = (c01 >> sh8) & 255, b0 = (c10 >> sh8) & 255, b1 = (c11 >> sh8) & 255;
                    int top = a0 * 256 + (a1 - a0) * wx, bot = b0 * 256 + (b1 - b0) * wx;
                    return (uint32_t)((top * 256 + (bot - top) * wy) >> 16);
                };
                uint32_t r = lerp(16), gg = lerp(8), b = lerp(0);
                uint32_t a;
                if (mode == 1) {
                    a = 178;
                    r = r * a / 255, gg = gg * a / 255, b = b * a / 255;
                } else if (g_cfg.keyDark) {
                    // Brightness -> coverage, colour kept: glows, sparks and smoke add their light on top of
                    // Elden Ring instead of turning into dark, jagged patches.
                    uint32_t mx = r > gg ? (r > b ? r : b) : (gg > b ? gg : b);
                    a = mx <= (uint32_t)cut ? 0 : mx >= (uint32_t)full ? 255 : (mx - cut) * 255 / (full - cut);
                    if (a == 0) r = gg = b = 0;
                    else r = lut[r], gg = lut[gg], b = lut[b];
                } else {
                    int d = abs((int)r - (int)kr) + abs((int)gg - (int)kg) + abs((int)b - (int)kb);
                    a = d <= g_cfg.keyTolerance ? 0 : 255;
                    if (!a) r = gg = b = 0;
                }
                o[x] = (a << 24) | (r << 16) | (gg << 8) | b;
            }
        }
        if (mode == 2) draw_health_bars(p, ew, eh);
        POINT dst = {er.left, er.top}, src0 = {0, 0};
        SIZE sz = {ew, eh};
        BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        HDC sdc = GetDC(nullptr);
        UpdateLayeredWindow(ov, sdc, &dst, &sz, out.dc, &src0, 0, &bf, ULW_ALPHA);
        ReleaseDC(nullptr, sdc);
        if (!shown) {
            ShowWindow(ov, SW_SHOWNOACTIVATE);
            SetWindowPos(ov, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            shown = true;
        }
        frames++;
        if (GetTickCount64() - lastReport > 10000) {
            logf("overlay: %.0f fps, Rift Apart %dx%d -> Elden Ring %dx%d (%s)", frames / ((GetTickCount64() - lastReport) / 1000.0),
                 rw, rh, ew, eh, wgcOk ? "fast capture" : "PrintWindow");
            frames = 0;
            lastReport = GetTickCount64();
        }
        if (!wgcOk) {
            DWORD took = GetTickCount() - t0;
            if (took < 16) Sleep(16 - took);
        }
    }
    return 0;
}

static volatile LONG g_linkedForExit = 0;
static BOOL WINAPI on_console_close(DWORD) {
    // Hand the Tarnished back right away instead of waiting for the one-second timeout.
    if (g_shm && g_linkedForExit) write_control(0, nullptr, 0);
    return FALSE;
}

// ---------------------------------------------------------------- Elden Ring runes -> Rift Apart bolts
// Reads (read-only) the Tarnished's runes held: GameDataMan -> PlayerGameData (+0x08) -> runes (+0x6C),
// level at +0x68. GameDataMan is found once by its well-known code pattern in eldenring.exe.
// The value goes into terrain.shm; riftbridge shows it as Ratchet's bolts. Off: ShowRunes=0 in [hud].
static DWORD WINAPI runes_thread(void*) {
    HANDLE er = nullptr;
    uint64_t gdmSlot = 0;
    int lastLogged = -1, fails = 0;
    for (;; Sleep(500)) {
        if (er) {
            DWORD code = 0;
            if (!GetExitCodeProcess(er, &code) || code != STILL_ACTIVE) CloseHandle(er), er = nullptr, gdmSlot = 0;
        }
        if (!er) {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snap == INVALID_HANDLE_VALUE) continue;
            PROCESSENTRY32 pe;
            pe.dwSize = sizeof(pe);
            DWORD pid = 0;
            for (BOOL ok = Process32First(snap, &pe); ok; ok = Process32Next(snap, &pe))
                if (_stricmp(pe.szExeFile, "eldenring.exe") == 0) pid = pe.th32ProcessID;
            CloseHandle(snap);
            if (!pid) continue;
            er = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
            if (!er) continue;
        }
        auto erd = [&](uint64_t a, void* o, size_t n) {
            SIZE_T got = 0;
            return ReadProcessMemory(er, (LPCVOID)a, o, n, &got) && got == n;
        };
        if (!gdmSlot) {
            if (fails > 5) continue;
            HMODULE mod = nullptr;
            DWORD need = 0;
            MODULEINFO mi = {};
            if (!EnumProcessModulesEx(er, &mod, sizeof(mod), &need, LIST_MODULES_64BIT) || !mod ||
                !GetModuleInformation(er, mod, &mi, sizeof(mi)))
                continue;
            // 48 8B 05 ?? ?? ?? ?? 48 85 C0 74 05 48 8B 40 58 C3 C3
            const uint8_t pat[] = {0x48, 0x8B, 0x05, 0, 0, 0, 0, 0x48, 0x85, 0xC0, 0x74, 0x05, 0x48, 0x8B, 0x40, 0x58, 0xC3, 0xC3};
            const bool wild[] = {0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
            uint64_t base = (uint64_t)mod, size = mi.SizeOfImage;
            std::vector<uint8_t> chunk(1 << 20);
            int found = 0;
            for (uint64_t o = 0x1000; o < size && !gdmSlot; o += chunk.size() - sizeof(pat)) {
                size_t n = (size_t)std::min<uint64_t>(chunk.size(), size - o);
                SIZE_T got = 0;
                if (!ReadProcessMemory(er, (LPCVOID)(base + o), chunk.data(), n, &got) || got < sizeof(pat)) continue;
                for (size_t i = 0; i + sizeof(pat) <= got; i++) {
                    if (chunk[i] != 0x48 || chunk[i + 1] != 0x8B || chunk[i + 2] != 0x05) continue;
                    size_t k = 3;
                    while (k < sizeof(pat) && (wild[k] || chunk[i + k] == pat[k])) k++;
                    if (k < sizeof(pat)) continue;
                    int32_t disp;
                    memcpy(&disp, &chunk[i + 3], 4);
                    gdmSlot = base + o + i + 7 + (int64_t)disp;
                    found++;
                    break;
                }
            }
            if (!gdmSlot) {
                if (++fails == 3) logf("runes: Elden Ring's GameDataMan not found; bolts stay Rift Apart's own");
                continue;
            }
            logf("runes: Elden Ring GameDataMan at eldenring.exe+%llx", (unsigned long long)(gdmSlot - base));
        }
        uint64_t gdm = 0, pgd = 0;
        int32_t lvlRunes[2] = {0, 0};
        if (!erd(gdmSlot, &gdm, 8) || !gdm || !erd(gdm + 0x08, &pgd, 8) || !pgd || !erd(pgd + 0x68, lvlRunes, 8)) continue;
        if (lvlRunes[0] < 1 || lvlRunes[0] > 713 || lvlRunes[1] < 0 || lvlRunes[1] > 999999999) continue;
        InterlockedExchange(&g_erRunes, lvlRunes[1]);
        g_erRunesTick = GetTickCount();
        if (lastLogged < 0 || (lastLogged != lvlRunes[1] && fails < 1000)) {
            if (lastLogged < 0) logf("runes: the Tarnished (level %d) holds %d runes -> shown as Ratchet's bolts%s", lvlRunes[0], lvlRunes[1],
                                     g_cfg.showRunes ? "" : " (off: ShowRunes=0)");
            lastLogged = lvlRunes[1];
        }
    }
    return 0;
}

int main() {
    GetModuleFileNameA(nullptr, g_exeDir, MAX_PATH);
    char* slash = strrchr(g_exeDir, '\\');
    if (slash) *slash = 0;
    char logPath[MAX_PATH];
    snprintf(logPath, sizeof(logPath), "%s\\riftlink.log", g_exeDir);
    g_log = fopen(logPath, "a");
    SetConsoleTitleA("Rift Apart -> Elden Ring link (step 1)");
    timeBeginPeriod(1);
    // Only one riftlink at a time: two would fight over the Tarnished and the ground feed.
    CreateMutexA(nullptr, TRUE, "Local\\riftlink-single-instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        printf("riftlink is already running in another window. This one closes in 5 seconds.\n");
        Sleep(5000);
        return 0;
    }
    load_config();
    g_fovLive = g_cfg.camFovDeg;
    g_camDelay = g_cfg.camDelayMs;
    logf("riftlink 0.27 starting. Terrain feed %s (%dx%d, %.1f m). Scale %.3f, flip X/Z %d/%d, swap %d, yaw offset %.0f, facing offset %.0f, rate %d Hz",
         g_cfg.terrain ? "on" : "off", g_cfg.gridSize, g_cfg.gridSize, g_cfg.gridSpacing, g_cfg.scale, g_cfg.flipX, g_cfg.flipZ, g_cfg.swapXZ, g_cfg.yawOffsetDeg, g_cfg.facingOffsetDeg, g_cfg.rateHz);
    if (!shm_open()) {
        logf("Press Enter to exit.");
        getchar();
        return 1;
    }
    SetConsoleCtrlHandler(on_console_close, TRUE);
    CreateThread(nullptr, 0, overlay_thread, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, runes_thread, nullptr, 0, nullptr);
    hdr()->mcPid = GetCurrentProcessId();
    hdr()->mcStartMs = now_ms();

    Hero hero;
    bool buildOk = false, anchored = false, paused = false, wasLinked = false;
    bool kPgUp = false, kEnd = false, kPgDn = false, kIns = false, kDel = false;
    // Delete (with the level hidden in Rift Apart): sink. The ground tiles go SinkDepth below, Ratchet
    // falls onto them in the dark under the level while the Tarnished and camera hold still, then
    // the link re-anchors where he landed.
    int sinkPhase = 0;  // 0 none, 1 falling
    bool sunk = false;
    uint64_t sinkStart = 0;
    float sinkTargetY = 0, sinkPrevY = 0, holdEr[3] = {}, holdCamE[3] = {}, holdCamT[3] = {};
    bool holdHaveCam = false;
    int sinkStill = 0;
    bool autoFovOn = false;
    float lastAutoFov = 0;
    // Catch: if Ratchet drops more than 25 m in 2 s (fell through, or linked while falling), hold the
    // Tarnished where it last stood and put the ground under Ratchet, like a sink.
    float ySamples[8] = {};
    int ySampleN = 0;
    uint64_t lastSampleMs = 0;
    float groundEr[3] = {}, groundCamE[3] = {}, groundCamT[3] = {};
    bool groundHaveCam = false, haveGroundEr = false;
    int captures = 0;
    bool camFollow = g_cfg.cameraFollow != 0;
    uint64_t camMgrVt = 0;  // HeroCameraManager vtable, checked once by RTTI name
    uint64_t lastMoveMs = 0, lastHintMs = 0;
    float lastRp[3] = {};
    float lastRpY = 0, lastCamE[3] = {}, lastCamT[3] = {};
    bool lastHaveCam = false;
    float r0[3] = {}, e0[3] = {}, lastEr[3] = {}, yawDeg = 0;
    float yawRef[3] = {};
    uint32_t lastLife = 0, anchorId = 0;
    uint64_t lastSearch = 0, lastStatus = 0, lastValidate = 0, lastWaitMsg = 0;
    const DWORD period = 1000 / g_cfg.rateHz;

    for (;;) {
        Sleep(period);
        uint64_t now = now_ms();
        hdr()->mcHeartbeat = hdr()->mcHeartbeat + 1;

        if (key_pressed(VK_END, &kEnd)) {
            paused = !paused;
            logf(paused ? "PAUSED (End): the Tarnished is released" : "RESUMED (End)");
            if (!paused) anchored = false;
            if (sinkPhase) sinkPhase = 0, g_terrainYOffset = 0;
        }
        if (key_pressed(VK_INSERT, &kIns)) {
            if (GetAsyncKeyState(VK_CONTROL) & 0x8000) {
                capture_test(++captures);
            } else {
                LONG m = (g_ovMode + 1) % 3;
                InterlockedExchange(&g_ovMode, m);
                logf("overlay: %s (Insert)", kOvNames[m]);
            }
        }
        if (key_pressed(VK_NEXT, &kPgDn)) {
            if (GetAsyncKeyState(VK_CONTROL) & 0x8000) {
                g_fovLive = (g_fovLive > 0 ? g_fovLive : g_erFov > 0 ? g_erFov : 50.0f) - 2.0f;
                if (g_fovLive < 10) g_fovLive = 10;
                logf("camera: Elden Ring field of view %.0f degrees (Ctrl+PgUp/PgDn; put FovDeg=%.0f in riftlink.ini to keep it)", g_fovLive, g_fovLive);
            } else {
                camFollow = !camFollow;
                logf(camFollow ? "camera: Elden Ring follows Rift Apart's camera (Page Down)"
                               : "camera: Elden Ring's own camera (Page Down)");
            }
        }
        {
            static bool kLb = false, kRb = false;
            bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            if (key_pressed(VK_OEM_4, &kLb) && ctrl) {
                g_camDelay = g_camDelay >= 10 ? g_camDelay - 10 : 0;
                logf("camera: Elden Ring camera delay %d ms (Ctrl+[ / Ctrl+]; put DelayMs=%d in riftlink.ini to keep it)", g_camDelay, g_camDelay);
            }
            if (key_pressed(VK_OEM_6, &kRb) && ctrl) {
                g_camDelay = g_camDelay + 10 <= 200 ? g_camDelay + 10 : 200;
                logf("camera: Elden Ring camera delay %d ms (Ctrl+[ / Ctrl+]; put DelayMs=%d in riftlink.ini to keep it)", g_camDelay, g_camDelay);
            }
        }
        if (key_pressed(VK_DELETE, &kDel) && !(GetAsyncKeyState(VK_SHIFT) & 0x8000)) {
            if (!g_cfg.terrain || g_cfg.sinkDepth <= 0) {
            } else if (sinkPhase) {
                logf("sink: Ratchet is still falling; press Delete again after he lands to go deeper");
            } else if (!anchored || g_tAnchorDone != anchorId) {
                logf("sink: wait until LINKED and the first terrain line, then press Delete again");
            } else {
                sinkPhase = 1;
                sinkStart = now_ms();
                sinkStill = 0;
                memcpy(holdEr, lastEr, 12);
                sinkTargetY = lastRpY - g_cfg.sinkDepth;
                sinkPrevY = lastRpY;
                holdHaveCam = lastHaveCam;
                memcpy(holdCamE, lastCamE, 12);
                memcpy(holdCamT, lastCamT, 12);
                g_terrainYOffset = -g_cfg.sinkDepth;
                anchorId++;  // all tiles move: resample now
                logf(sunk ? "sink: dropping Ratchet another %.0f m (the Tarnished waits)"
                          : "sink: dropping Ratchet %.0f m below the level (the Tarnished waits)", g_cfg.sinkDepth);
            }
        }
        if (key_pressed(VK_PRIOR, &kPgUp)) {
            if (GetAsyncKeyState(VK_CONTROL) & 0x8000) {
                g_fovLive = (g_fovLive > 0 ? g_fovLive : g_erFov > 0 ? g_erFov : 50.0f) + 2.0f;
                logf("camera: Elden Ring field of view %.0f degrees (Ctrl+PgUp/PgDn; put FovDeg=%.0f in riftlink.ini to keep it)", g_fovLive, g_fovLive);
            } else {
                anchored = false;
                logf("re-anchor (Page Up)");
            }
        }

        // Rift Apart side
        if (!ra_alive()) {
            if (g_ra) {
                logf("Rift Apart closed");
                ra_close();
                hero = Hero();
                buildOk = false;
                anchored = false;
            }
            if (now - lastSearch > 2000) {
                lastSearch = now;
                if (ra_open()) {
                    logf("Rift Apart running (pid %lu, base %llx)", g_raPid, (unsigned long long)g_raBase);
                    buildOk = check_build();
                    if (buildOk) logf("RiftApart.exe build matches");
                } else if (now - lastWaitMsg > 15000) {
                    lastWaitMsg = now;
                    logf("waiting for Rift Apart...");
                }
            }
        }
        if (!g_ra || !buildOk) continue;

        if (now - lastValidate > 250 && hero.local) {
            lastValidate = now;
            if (!hero_valid(hero)) {
                logf("Ratchet's object changed (loading screen, rift or death); searching again");
                hero = Hero();
                anchored = false;
                sinkPhase = 0, sunk = false, g_terrainYOffset = 0;
                g_cam = CamProbe();
                g_camVtCount = -1;
                g_prevCamFwd[0] = g_prevCamFwd[1] = g_prevCamFwd[2] = 0;
            }
        }
        if (!hero.local) {
            if (now - lastSearch > 3000) {
                lastSearch = now;
                find_hero(&hero);
            }
            continue;
        }
        float rp[3];
        if (!rd(hero.mover + OFF_POS, rp, 12) || !isfinite(rp[0]) || !isfinite(rp[1]) || !isfinite(rp[2])) {
            hero = Hero();
            anchored = false;
            continue;
        }

        // Elden Ring side
        if (!host_ready()) {
            if (now - lastWaitMsg > 15000) {
                lastWaitMsg = now;
                logf("waiting for Elden Ring with the bridge: it isn't running (or was started from Steam). "
                     "Close it and start it with Launch-Elden-Ring-Rift.cmd.");
            }
            continue;
        }
        ErmcGameState st;
        bool haveState = read_state(&st);
        bool erAlive = haveState && (st.flags & ERMC_STATE_PLAYER_VALID) && !(st.flags & ERMC_STATE_PLAYER_DEAD) &&
                       !(st.flags & ERMC_STATE_HOST_BUSY);
        uint32_t life = hdr()->hostLife;
        if (life != lastLife) {
            lastLife = life;
            if (anchored) logf("Elden Ring moved the Tarnished (loading, grace, respawn): re-anchoring there");
            anchored = false;
        }
        if (!erAlive && !paused && now - lastWaitMsg > 15000) {
            lastWaitMsg = now;
            logf("Elden Ring is running but the Tarnished isn't usable yet (title screen, loading, menu or dead): "
                 "load your save and stand in the world");
        }
        if (paused || !erAlive) {
            if (wasLinked) {
                write_control(0, nullptr, yawDeg);
                logf(paused ? "link paused" : "Elden Ring busy (loading/dead/menu): link waiting");
                wasLinked = false;
            }
            anchored = false;
            continue;
        }
        if (!anchored) {
            memcpy(r0, rp, 12);
            memcpy(e0, st.playerPos, 12);
            memcpy(yawRef, st.playerPos, 12);
            // Keep the Tarnished's current facing until Ratchet moves: Elden Ring yaw theta from the
            // quaternion, bridge yaw = theta - 180 degrees (see stand_in in game.cpp).
            float theta = 2.0f * atan2f(st.playerQuat[1], st.playerQuat[3]);
            yawDeg = (theta - 3.14159265f) * 180.0f / 3.14159265f;
            anchored = true;
            anchorId++;
            ySampleN = 0;
            if (sinkPhase == 0) memcpy(groundEr, st.playerPos, 12), haveGroundEr = true, groundHaveCam = false;
            g_hurtQuietUntil = now + 4000;
            logf("anchored: Ratchet (%.2f %.2f %.2f) = Tarnished (%.2f %.2f %.2f)", rp[0], rp[1], rp[2], e0[0], e0[1],
                 e0[2]);
        }
        float d[3] = {rp[0] - r0[0], rp[1] - r0[1], rp[2] - r0[2]}, m[3];
        map_delta(d, m);
        float er[3] = {e0[0] + m[0], e0[1] + m[1], e0[2] + m[2]};

        // Face the way Ratchet moves (Elden Ring forward = (-sin y, 0, -cos y) in the bridge's convention).
        float fx = er[0] - yawRef[0], fz = er[2] - yawRef[2];
        if (fx * fx + fz * fz > 0.04f) {  // 20 cm of travel
            yawDeg = atan2f(-fx, -fz) * 180.0f / 3.14159265f + g_cfg.facingOffsetDeg;
            memcpy(yawRef, er, 12);
        }
        uint32_t flags = ERMC_CTRL_MOVE_HUNTER | (g_cfg.hideTarnished ? ERMC_CTRL_HIDE_HUNTER : 0);
        // Camera: HeroCameraManager (pointer cache +0x100) holds the look-at point (+0x1F8, Ratchet's
        // feet) and the camera eye (+0x208), in Rift Apart world space. Mapped like Ratchet himself.
        float camE[3], camT[3];
        bool haveCam = false;
        if (camFollow) {
            uint64_t cm = rdq(hero.cache + 0x100);
            uint64_t vt = cm ? rdq(cm) : 0;
            if (vt && !camMgrVt) {
                char nm[64];
                if (rtti_name(vt, nm, sizeof(nm)) && !strcmp(nm, ".?AVHeroCameraManager@Hero@@")) {
                    camMgrVt = vt;
                    logf("camera: found Rift Apart's camera manager");
                }
            }
            float raw[6];
            if (vt && vt == camMgrVt && rd(cm + 0x1F8, raw, 12) && rd(cm + 0x208, raw + 3, 12)) {
                bool ok = true;
                for (float v : raw) ok = ok && isfinite(v);
                float dx = raw[3] - raw[0], dy = raw[4] - raw[1], dz = raw[5] - raw[2];
                if (ok && dx * dx + dy * dy + dz * dz < 40.0f * 40.0f && dx * dx + dy * dy + dz * dz > 0.01f) {
                    float cp[3], cf[3], cu[3];
                    if (g_cfg.trueCamera && (!g_cam.obj || (!g_cam.proper && now >= g_cam.nextTry))) cam_probe(cm, raw + 3, raw, now);
                    if (g_cfg.trueCamera && g_cam.obj && !g_learnStarted) {
                        InterlockedExchange(&g_learnStarted, 1);
                        CreateThread(nullptr, 0, learn_thread, nullptr, 0, nullptr);
                    }
                    if (g_cfg.trueCamera && cam_read(cp, cf, cu, raw + 3, raw)) {
                        // Real camera: same eye, same view direction, same up.
                        float de[3] = {cp[0] - r0[0], cp[1] - r0[1], cp[2] - r0[2]}, me[3], mf[3], mu[3];
                        map_delta(de, me);
                        map_delta(cf, mf);
                        map_delta(cu, mu);
                        float lf = sqrtf(dot3(mf, mf)), lu = sqrtf(dot3(mu, mu));
                        for (int i = 0; i < 3; i++) {
                            camE[i] = e0[i] + me[i];
                            camT[i] = camE[i] + mf[i] / lf * 10.0f;
                            g_camUp[i] = mu[i] / lu;
                        }
                        // Looking up pulls Rift Apart's camera low; never let Elden Ring's go under its ground
                        // (that showed the world from below: Ratchet seemed to fly).
                        float gh;
                        bool low = false;
                        if (sinkPhase == 0 && ground_height(camE[0], camE[2], &gh)) {
                            // Close to the ground: tell Rift Apart to stop the mouse tilting the camera any further
                            // up (looking up lowers it); only nudge Elden Ring's camera if it's actually under.
                            low = camE[1] < gh + 0.9f;
                            if (camE[1] < gh + 0.15f) {
                                float lift = gh + 0.15f - camE[1];
                                camE[1] += lift, camT[1] += lift;
                            }
                        }
                        if (g_rt) g_rt->camLow = low ? 1 : 0;
                        haveCam = true;
                    } else {
                        float de[3] = {raw[3] - r0[0], raw[4] - r0[1], raw[5] - r0[2]}, dt[3] = {raw[0] - r0[0], raw[1] - r0[1], raw[2] - r0[2]};
                        float me[3], mt[3];
                        map_delta(de, me);
                        map_delta(dt, mt);
                        for (int i = 0; i < 3; i++) camE[i] = e0[i] + me[i], camT[i] = e0[i] + mt[i];
                        camT[1] += g_cfg.camTargetHeight;
                        g_camUp[0] = 0, g_camUp[1] = 1, g_camUp[2] = 0;
                        haveCam = true;
                    }
                }
            }
        }
        lastRpY = rp[1];
        static uint64_t lastAspectMs = 0;
        if (now - lastAspectMs > 1000) {  // Rift Apart's picture shape, for the field of view
            lastAspectMs = now;
            FindWin fw = {g_raPid, nullptr, 0};
            EnumWindows(find_win_cb, (LPARAM)&fw);
            RECT rc;
            if (fw.best && GetClientRect(fw.best, &rc) && rc.bottom > 0) g_raAspect = (float)rc.right / rc.bottom;
        }
        if (haveCam) memcpy(lastCamE, camE, 12), memcpy(lastCamT, camT, 12);
        lastHaveCam = haveCam;
        if (now - lastSampleMs >= 250) {  // Ratchet's height every 250 ms, last 2 s
            lastSampleMs = now;
            memmove(ySamples + 1, ySamples, sizeof(float) * 7);
            ySamples[0] = rp[1];
            if (ySampleN < 8) ySampleN++;
            if (ySampleN >= 3 && fabsf(ySamples[0] - ySamples[2]) < 1.0f && sinkPhase == 0) {  // steady: on the ground
                memcpy(groundEr, er, 12), haveGroundEr = true;
                groundHaveCam = haveCam;
                if (haveCam) memcpy(groundCamE, camE, 12), memcpy(groundCamT, camT, 12);
            }
            // Only when Elden Ring has no ground under the Tarnished either (a real cliff jump does).
            if (sinkPhase == 0 && haveGroundEr && ySampleN == 8 && ySamples[7] - ySamples[0] > 25.0f &&
                g_tNoGroundMs && now - g_tNoGroundMs < 3000) {
                sinkPhase = 1;
                sinkStart = now;
                sinkStill = 0;
                memcpy(holdEr, groundEr, 12);
                holdHaveCam = groundHaveCam;
                memcpy(holdCamE, groundCamE, 12);
                memcpy(holdCamT, groundCamT, 12);
                sinkTargetY = rp[1] - 80.0f;
                sinkPrevY = rp[1];
                float hs = g_cfg.scale * g_cfg.heightScale;
                float groundRa = r0[1] + (holdEr[1] - e0[1]) / (hs != 0 ? hs : 1.0f);
                g_terrainYOffset = sinkTargetY - groundRa;
                anchorId++;
                logf("catch: Ratchet is falling (y %.1f); ground goes under him at y %.1f, the Tarnished waits", rp[1],
                     sinkTargetY);
            }
        }
        if (sinkPhase == 1) {
            // Hold the Tarnished and Elden Ring's camera while Ratchet falls.
            memcpy(er, holdEr, 12);
            haveCam = holdHaveCam;
            memcpy(camE, holdCamE, 12);
            memcpy(camT, holdCamT, 12);
            bool still = fabsf(rp[1] - sinkPrevY) < 0.05f;
            sinkPrevY = rp[1];
            sinkStill = (still && rp[1] < sinkTargetY + 2.0f) ? sinkStill + 1 : 0;
            bool timeout = now - sinkStart > 10000 + (uint64_t)(fabsf(sinkPrevY - sinkTargetY) > 0 ? g_cfg.sinkDepth * 1000.0f / 15.0f : 30000.0f);
            if (sinkStill > g_cfg.rateHz / 3 || timeout) {
                sinkPhase = 0;
                sunk = !timeout || rp[1] < sinkTargetY + 50.0f;
                g_terrainYOffset = 0;
                anchored = false;  // re-anchor: Ratchet here = the Tarnished where it waited
                logf(timeout ? "sink: Ratchet didn't land in time (y %.1f, floor at %.1f); re-anchoring where he is"
                             : "sink: Ratchet landed at y %.1f (floor at %.1f); re-anchoring", rp[1], sinkTargetY);
            }
        }
        // Camera delay: the overlay shows Rift Apart's picture a little late (capture + copy), so play
        // Elden Ring's camera from the same moment, or Ratchet slides across the world while turning.
        if (haveCam && sinkPhase == 0) {
            struct CamRec { uint64_t t; float e[3], tg[3], up[3]; };
            static CamRec hist[256];
            static int hHead = 0;
            CamRec& r = hist[hHead++ & 255];
            r.t = now;
            memcpy(r.e, camE, 12), memcpy(r.tg, camT, 12), memcpy(r.up, g_camUp, 12);
            if (g_camDelay > 0) {
                for (int k = 1; k < 256; k++) {
                    const CamRec& c = hist[(hHead - 1 - k) & 255];
                    if (!c.t || c.t > now) break;
                    if (now - c.t >= (uint64_t)g_camDelay) {
                        memcpy(camE, c.e, 12), memcpy(camT, c.tg, 12), memcpy(g_camUp, c.up, 12);
                        break;
                    }
                }
            }
        }
        if (haveCam) flags |= ERMC_CTRL_OVERRIDE_CAMERA;
        if (st.fovYDeg > 5 && st.fovYDeg < 170 && g_fovLive <= 0 && !autoFovOn) g_erFov = st.fovYDeg;
        float fovOut = g_fovLive;
        if (fovOut <= 0 && g_cfg.autoFov && g_raFovH > 0 && g_raAspect > 0.2f) {
            // Rift Apart's value is horizontal; Elden Ring wants vertical (same picture shape: Rift Apart's).
            fovOut = 2.0f * atanf(tanf(g_raFovH * 0.5f) / g_raAspect) * 180.0f / 3.14159265f;
            if (!autoFovOn || fabsf(fovOut - lastAutoFov) > 1.0f)
                logf("camera: field of view matched: Rift Apart %.1f deg wide -> Elden Ring %.1f deg tall (Elden Ring's own: %.1f)",
                     g_raFovH * 180.0f / 3.14159265f, fovOut, g_erFov);
            autoFovOn = true, lastAutoFov = fovOut;
            g_erFov = fovOut;  // starting point for Ctrl+PgUp / Ctrl+PgDn
        }
        write_control(flags, er, yawDeg, haveCam ? camE : nullptr, haveCam ? camT : nullptr, fovOut);
        {
            float aim[3] = {camT[0] - camE[0], camT[1] - camE[1], camT[2] - camE[2]};
            float al = haveCam ? sqrtf(dot3(aim, aim)) : 0;
            if (al > 0.01f) for (float& v : aim) v /= al;
            combat_tick(now, camE, aim, er, yawDeg, al > 0.01f);
            blast_tick();
        }
        memcpy(lastEr, er, 12);
        g_hurtQuiet = sinkPhase != 0 || paused || now < g_hurtQuietUntil;
        terrain_tick(now, anchorId, r0, e0, er);
        enemies_tick(r0, e0, er);
        if (!wasLinked) {
            logf("LINKED: the Tarnished now follows Ratchet");
            wasLinked = true;
            g_linkedForExit = 1;
        }
        if (fabsf(rp[0] - lastRp[0]) + fabsf(rp[1] - lastRp[1]) + fabsf(rp[2] - lastRp[2]) > 0.01f) {
            lastMoveMs = now;
            memcpy(lastRp, rp, 12);
        }
        if (now - lastMoveMs > 8000 && now - lastHintMs > 20000) {
            lastHintMs = now;
            DWORD fgPid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &fgPid);
            if (fgPid && fgPid != g_raPid && fgPid != hdr()->hostPid)
                logf("hint: Ratchet hasn't moved. Click into the Elden Ring (or Rift Apart) window and move him.");
        }
        if (now - lastStatus > 200) {
            lastStatus = now;
            printf("\rRatchet %8.2f %8.2f %8.2f  ->  Tarnished %8.2f %8.2f %8.2f  yaw %5.0f   ", rp[0], rp[1], rp[2],
                   er[0], er[1], er[2], yawDeg);
            fflush(stdout);
        }
    }
}
