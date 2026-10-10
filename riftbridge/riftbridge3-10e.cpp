// riftbridge 0.6c (Rift Apart side: 0.6 + controller) of the Elden Ring x Rift Apart bridge). Loaded by RiftApart.exe as
// "faultrep.dll" (ReportFault passed through to the real Windows faultrep.dll).
//
// - Hooks HeroMoverManager::Update (per-frame, game thread).
// - Elden Ring ground: reads ground tiles that riftlink writes to riftbridge\terrain.shm (data only)
//   and keeps a pool of static Havok box bodies (hknpWorld::createBody/addBodies, then
//   setBodyTransform) on them, with the level ground's collision filter, so Ratchet's own movement
//   collides with Elden Ring's terrain.
// - Pause: switch Rift Apart's own static level collision off/on for Ratchet (swaps the level
//   bodies' collision filter with one his mover ignores; restored on the next press).
// - Delete / Shift+Delete: hide / show the level's zones (picture).
// - Home: test platform above Ratchet.
// - Play from the Elden Ring window: while Elden Ring is the active window, Rift Apart keeps
//   receiving keyboard/mouse (raw input with RIDEV_INPUTSINK) and believes it still has focus
//   (its GetForegroundWindow/GetActiveWindow/GetFocus imports answer with its own window, losing-focus
//   messages are held back, ClipCursor/SetCursorPos are skipped so the cursor stays free).
// - Rift Apart's own level collision switches off by itself once Elden Ring's ground has arrived.
// Log: riftbridge\rift-bridge3.log. Off switch: riftbridge\off.txt, or delete faultrep.dll.

#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include <algorithm>
#include <ctype.h>
#include "rift_terrain.h"
#include <unordered_map>
#include <unordered_set>

static char g_dir[MAX_PATH];
// Mouse turning of the camera, as a percentage: 35 by default (a little, for aiming), Home cycles
// 35 -> 100 -> 0 -> 35.
static volatile LONG g_mouseLook = 35;
static FILE* g_log = nullptr;
static uintptr_t g_base = 0;
static size_t g_size = 0;
static CRITICAL_SECTION g_lock;

static void logf(const char* fmt, ...) {
    if (!g_log) return;
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    SYSTEMTIME t;
    GetLocalTime(&t);
    EnterCriticalSection(&g_lock);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf);
    fflush(g_log);
    LeaveCriticalSection(&g_lock);
}
static void rawf(const char* s) {
    EnterCriticalSection(&g_lock);
    fputs(s, g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_lock);
}

typedef DWORD(WINAPI* ReportFault_t)(LPEXCEPTION_POINTERS, DWORD);
extern "C" __declspec(dllexport) DWORD WINAPI ReportFault(LPEXCEPTION_POINTERS pep, DWORD opt) {
    static ReportFault_t real = nullptr;
    if (!real) {
        char sys[MAX_PATH];
        GetSystemDirectoryA(sys, MAX_PATH);
        strcat(sys, "\\faultrep.dll");
        HMODULE m = LoadLibraryA(sys);
        if (m) real = (ReportFault_t)GetProcAddress(m, "ReportFault");
    }
    return real ? real(pep, opt) : 0;
}

// ---------------------------------------------------------------- memory helpers
static bool rd(uintptr_t a, void* out, size_t n) {
    SIZE_T got = 0;
    return a >= 0x10000 && ReadProcessMemory(GetCurrentProcess(), (LPCVOID)a, out, n, &got) && got == n;
}
static uintptr_t rdq(uintptr_t a) {
    uintptr_t v = 0;
    return rd(a, &v, 8) ? v : 0;
}
static uint32_t rdd(uintptr_t a) {
    uint32_t v = 0;
    return rd(a, &v, 4) ? v : 0;
}
static bool rtti(uintptr_t obj, std::string* out) {
    uintptr_t vt = rdq(obj);
    if (vt < g_base || vt >= g_base + g_size) return false;
    uintptr_t col = rdq(vt - 8);
    if (col < g_base || col >= g_base + g_size) return false;
    uint32_t td = 0;
    char name[200] = {};
    if (!rd(col + 12, &td, 4) || td >= g_size || !rd(g_base + td + 16, name, sizeof(name) - 1)) return false;
    if (strncmp(name, ".?A", 3) != 0) return false;
    *out = name;
    return true;
}
static bool plausible_float(uint32_t bits) {
    float f;
    memcpy(&f, &bits, 4);
    if (!isfinite(f) || f == 0.0f) return false;
    float a = fabsf(f);
    return a > 1e-4f && a < 1e6f;
}
static void dump(const char* title, uintptr_t obj, size_t bytes) {
    std::string nm;
    rtti(obj, &nm);
    logf("== %s @ %llx  %s", title, (unsigned long long)obj, nm.c_str());
    std::vector<uint64_t> q(bytes / 8);
    if (!rd(obj, q.data(), bytes)) {
        rawf("   (unreadable)\n");
        return;
    }
    std::string out;
    for (size_t i = 0; i < q.size(); i++) {
        uint64_t v = q[i];
        if (v == 0) continue;
        char line[512];
        int n = snprintf(line, sizeof(line), "  +%03zx %016llx", i * 8, (unsigned long long)v);
        std::string on;
        if (v >= g_base && v < g_base + g_size)
            n += snprintf(line + n, sizeof(line) - n, "  rva %llx", (unsigned long long)(v - g_base));
        else if (v > 0x10000 && v < 0x7FFFFFFFFFFFull && rtti(v, &on))
            n += snprintf(line + n, sizeof(line) - n, "  -> %s", on.c_str());
        else {
            uint32_t lo = (uint32_t)v, hi = (uint32_t)(v >> 32);
            if (plausible_float(lo) || plausible_float(hi)) {
                float a, b;
                memcpy(&a, &lo, 4);
                memcpy(&b, &hi, 4);
                n += snprintf(line + n, sizeof(line) - n, "  f %.4g %.4g", a, b);
            }
        }
        out += line;
        out += "\n";
    }
    rawf(out.c_str());
}

template <typename F>
static int scan_all(uintptr_t needle, F visit) {
    static uint8_t* buf = (uint8_t*)VirtualAlloc(nullptr, 4 << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    uintptr_t a = 0x10000;
    MEMORY_BASIC_INFORMATION mbi;
    int hits = 0;
    while (buf && a < 0x7FFFFFFF0000ull && VirtualQuery((LPCVOID)a, &mbi, sizeof(mbi))) {
        uintptr_t b = (uintptr_t)mbi.BaseAddress, e = b + mbi.RegionSize;
        DWORD pr = mbi.Protect & 0xFF;
        bool rw = pr == PAGE_READWRITE || pr == PAGE_EXECUTE_READWRITE || pr == PAGE_WRITECOPY;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && rw &&
            !(mbi.Protect & (PAGE_GUARD | PAGE_WRITECOMBINE | PAGE_NOCACHE)) && mbi.RegionSize <= (512ull << 20) &&
            !(b <= (uintptr_t)buf && (uintptr_t)buf < e)) {
            for (uintptr_t at = b; at < e; at += 4 << 20) {
                size_t n = (size_t)((e - at) < (4u << 20) ? (e - at) : (4u << 20));
                SIZE_T got = 0;
                ReadProcessMemory(GetCurrentProcess(), (LPCVOID)at, buf, n, &got);
                const uint64_t* q = (const uint64_t*)buf;
                for (size_t i = 0; i < got / 8; i++)
                    if (q[i] == needle) {
                        hits++;
                        if (visit(at + i * 8)) return hits;  // visitor returns true = stop
                    }
            }
        }
        a = e;
    }
    return hits;
}

// ---------------------------------------------------------------- game addresses (3.0630.1.0)
static const uint32_t RVA_VT_HERO = 0x2f48188, RVA_VT_CACHE = 0x2f4c0b8, RVA_VT_MOVER = 0x2f5a328;
static const uint32_t RVA_VT_WORLD = 0x4956af8, RVA_VT_PHYSSYS = 0x32e49b0;
static const uint32_t RVA_MOVER_UPDATE = 0x72faa0;     // HeroMoverManager::Update
static const uint32_t RVA_BOX_CFG_CTOR = 0x2933480;    // hknpConvexShape::BuildConfig::BuildConfig()
static const uint32_t RVA_CREATE_BOX = 0x2933fc0;      // createBoxShape(const hkVector4& half, float radius, const BuildConfig&)
static const uint32_t RVA_CREATE_BODY = 0x293efb0;     // hknpWorld::createBody(hknpBodyId* out, const hknpBodyCinfo&, ...)
static const uint32_t RVA_ADD_BODIES = 0x293e390;
static const uint32_t RVA_SET_TRANSFORM = 0x2939ea0;   // hknpWorld::setBodyTransform(bodyId, const hkTransform&, activation)
static const uint32_t RVA_ZONEVIS_MGR = 0x1923c00;     // returns the zone-visibility manager
static const uint32_t RVA_ZONEVIS_OP3 = 0x1923a00;     // (mgr, const u16* zones, int count): op 3; count 0 = all zones
static const uint32_t RVA_ZONEVIS_OP2 = 0x1924cf0;     // same with op 2 (ZoneVisibleAction uses it for event 0x29e19b1)      // hknpWorld::addBodies(const hknpBodyId*, int count, flags, activation)
static const uint8_t kMoverUpdateHead[16] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x18, 0x55,
                                             0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0x68, 0x98};

static volatile uintptr_t g_world = 0, g_mover = 0, g_moverLive = 0;
alignas(16) static uint8_t g_template[0xB0];
static volatile LONG g_haveTemplate = 0;
static volatile uint32_t g_levelFilter = 0;
static volatile uint64_t g_levelMat = 0;
// How our ground/wall/enemy bodies present themselves to Rift Apart's physics queries. Level bodies
// carry extra flag bits (upper 16 bits of body+0x68) that ours lacked; hitscan guns appear to need
// them. Variant 0 = filter and flags copied from the level's ground; Ctrl+Pause cycles the others.
struct BodyVariant { uint32_t filter, flagsHi; int count; uint32_t groundFilter; uint64_t tail; };  // tail: body+0x98 (1 = copy a level body's)
static volatile uint64_t g_levelTail = 0;  // body+0x98 of a level ground body (per-body id bits Insomniac's queries may check)
static std::vector<BodyVariant> g_variants;
static int g_variant = 0;
// Who owns a body (body+0x98): a hit resolves to an "object handle" from it. Level bodies resolve to their
// zone's world object; ours had none (handle 0), and thrown weapons skip hits without an owner (they
// still stop on birds and robots, which have one). Tail value 2 = use a level ground handle seen in a hit.
static volatile LONG g_worldHandle = 0;
static volatile LONG64 g_worldHandleTail = 0;
// Rift Apart's object handle table: handle = type (top byte) + index; valid while the entry has that type.
static bool handle_ok(uint32_t h) {
    uint32_t type = h >> 24, idx = h & 0xFFFFFF;
    if (!type) return false;
    int32_t cnt = 0;
    uintptr_t tab = 0;
    if (!rd(g_base + 0x6747cfc, &cnt, 4) || !rd(g_base + 0x6747ce0, &tab, 8) || !tab || (int32_t)idx >= cnt) return false;
    uint8_t t = 0;
    uintptr_t p = 0;
    return rd(tab + (uintptr_t)idx * 16 + 8, &t, 1) && t == type && rd(tab + (uintptr_t)idx * 16, &p, 8) && p;
}
// Level ground pieces seen in real hits (body, shape key, shape): any one still loaded can tell us a
// current owner handle again after the old one's zone streamed out.
struct GroundKey { uint32_t id, key; uint64_t shape; };
static GroundKey g_gk[512];
static volatile LONG g_gkN = 0;
static void apply_variant(uint32_t id, bool ground = false, bool proxy = false) {
    if (g_variants.empty() || !g_world) return;
    const BodyVariant& v = g_variants[(size_t)g_variant % g_variants.size()];
    uint8_t* b = (uint8_t*)rdq(g_world + 0x28) + (size_t)(id & 0xFFFFFF) * 0xC0;
    uint32_t f = *(volatile uint32_t*)(b + 0x68);
    *(volatile uint32_t*)(b + 0x68) = (f & 0xFFFFu) | (v.flagsHi & 0xFFFF0000u);
    *(volatile uint32_t*)(b + 0x6C) = ground && v.groundFilter ? v.groundFilter : v.filter;
    uint64_t tail = v.tail == 1 ? g_levelTail : v.tail;
    if (v.tail == 2) tail = !handle_ok((uint32_t)g_worldHandle) ? 0x00000000ffff0000ull : (uint64_t)(uint32_t)g_worldHandle;
    *(volatile uint64_t*)(b + 0x98) = tail;
}

// ---------------------------------------------------------------- hook plumbing
extern "C" {
uintptr_t g_tramp = 0;
void on_mover_update(uintptr_t self);
}
// Saves the volatile registers the original function may read (rcx/rdx/r8/r9, xmm0-5), calls
// on_mover_update(rcx), restores them and continues in the trampoline (original first 16 bytes,
// then a jump back to rva+16).
asm(".text\n.p2align 4\n.globl hook_entry\nhook_entry:\n"
    "  push %rcx\n  push %rdx\n  push %r8\n  push %r9\n  push %r10\n  push %r11\n"
    "  sub $0x88, %rsp\n"
    "  movdqu %xmm0, 0x20(%rsp)\n  movdqu %xmm1, 0x30(%rsp)\n  movdqu %xmm2, 0x40(%rsp)\n"
    "  movdqu %xmm3, 0x50(%rsp)\n  movdqu %xmm4, 0x60(%rsp)\n  movdqu %xmm5, 0x70(%rsp)\n"
    "  mov 0xb0(%rsp), %rcx\n"
    "  call on_mover_update\n"
    "  movdqu 0x20(%rsp), %xmm0\n  movdqu 0x30(%rsp), %xmm1\n  movdqu 0x40(%rsp), %xmm2\n"
    "  movdqu 0x50(%rsp), %xmm3\n  movdqu 0x60(%rsp), %xmm4\n  movdqu 0x70(%rsp), %xmm5\n"
    "  add $0x88, %rsp\n"
    "  pop %r11\n  pop %r10\n  pop %r9\n  pop %r8\n  pop %rdx\n  pop %rcx\n"
    "  jmpq *g_tramp(%rip)\n");
extern "C" void hook_entry();

static uint8_t* alloc_near(uintptr_t target) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity;
    for (uintptr_t d = 0x100000; d < 0x70000000; d += gran) {
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            uintptr_t a = (target + sgn * (intptr_t)d) & ~(gran - 1);
            void* p = VirtualAlloc((void*)a, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p) return (uint8_t*)p;
        }
    }
    return nullptr;
}

static bool install_hook() {
    uintptr_t fn = g_base + RVA_MOVER_UPDATE;
    uint8_t cur[16];
    if (!rd(fn, cur, 16) || memcmp(cur, kMoverUpdateHead, 16) != 0) {
        logf("hook: HeroMoverManager::Update doesn't start with the expected bytes; not hooking");
        return false;
    }
    uint8_t* page = alloc_near(fn);
    if (!page) {
        logf("hook: no memory near the game");
        return false;
    }
    // page+0: jmp [rip+0] -> hook_entry
    page[0] = 0xFF, page[1] = 0x25, *(uint32_t*)(page + 2) = 0;
    *(uint64_t*)(page + 6) = (uint64_t)&hook_entry;
    // page+0x20: trampoline = original 16 bytes + jmp [rip+0] -> fn+16
    uint8_t* tr = page + 0x20;
    memcpy(tr, kMoverUpdateHead, 16);
    tr[16] = 0xFF, tr[17] = 0x25, *(uint32_t*)(tr + 18) = 0;
    *(uint64_t*)(tr + 22) = fn + 16;
    g_tramp = (uintptr_t)tr;
    FlushInstructionCache(GetCurrentProcess(), page, 0x100);
    // Patch: one aligned 8-byte store = "jmp rel32 page" + original bytes 5..7 (never executed).
    int64_t rel = (int64_t)(uintptr_t)page - (int64_t)(fn + 5);
    if (rel < INT32_MIN || rel > INT32_MAX) {
        logf("hook: jump page out of range");
        return false;
    }
    uint8_t patch[8];
    memcpy(patch, kMoverUpdateHead, 8);
    patch[0] = 0xE9;
    int32_t r32 = (int32_t)rel;
    memcpy(patch + 1, &r32, 4);
    DWORD old;
    if (!VirtualProtect((void*)fn, 8, PAGE_EXECUTE_READWRITE, &old)) {
        logf("hook: VirtualProtect failed");
        return false;
    }
    InterlockedExchange64((volatile LONG64*)fn, *(LONG64*)patch);
    VirtualProtect((void*)fn, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)fn, 16);
    logf("hook: HeroMoverManager::Update hooked (page %p)", page);
    return true;
}

// ---------------------------------------------------------------- platform creation (game thread)
typedef void*(__fastcall* CfgCtor_t)(void* cfg);
typedef void*(__fastcall* CreateBox_t)(const float* half, float radius, const void* cfg);
typedef void*(__fastcall* CreateBody_t)(uintptr_t world, uint32_t* outId, const void* cinfo, uint64_t flags);
typedef void*(__fastcall* ZoneMgr_t)();
typedef void(__fastcall* ZoneOp_t)(void* mgr, const uint16_t* zones, int count);
typedef void(__fastcall* SetTransform_t)(uintptr_t world, uint32_t id, const float* xform, uint32_t activation);
typedef void(__fastcall* AddBodies_t)(uintptr_t world, const uint32_t* ids, int count, uint32_t flags, uint32_t activation);

static int g_platforms = 0;
static int g_trackFrames = 0;
static DWORD g_gameThread = 0;
static bool g_f7 = false, g_del = false;
static int g_maskLogFrames = 0;

// Zone system singleton (stored by its constructor at rva 0x68daef0). It holds bitmasks of 0x3C00
// zones (0x780 bytes each) starting at +0x20, +0x7a0, +0xf20, ... A function copies the +0x7a0 mask
// into +0x2d20 and sets the byte at +0xe4449, so +0x2d20 is the candidate "visible zones" mask and
// +0xe4449 its "changed" flag. Delete: save + clear +0x2d20. Shift+Delete: restore it.
static const uint32_t RVA_ZONE_SYSTEM_PTR = 0x68daef0;
static const uint32_t kMaskOffs[] = {0x20, 0x7a0, 0xf20, 0x16a0, 0x1e20, 0x25a0, 0x2d20, 0x34a0};
static uint8_t g_savedMask[0x780];
static bool g_haveSaved = false;

static int popcount_mask(uintptr_t at) {
    uint64_t q[0x780 / 8];
    if (!rd(at, q, sizeof(q))) return -1;
    int n = 0;
    for (uint64_t v : q) n += __builtin_popcountll(v);
    return n;
}

static void log_masks(const char* when) {
    uintptr_t zs = rdq(g_base + RVA_ZONE_SYSTEM_PTR);
    char line[512];
    int n = snprintf(line, sizeof(line), "zone masks %s (system %llx):", when, (unsigned long long)zs);
    for (uint32_t o : kMaskOffs) n += snprintf(line + n, sizeof(line) - n, " +%x=%d", o, popcount_mask(zs + o));
    uint8_t dirty = 0;
    rd(zs + 0xe4449, &dirty, 1);
    snprintf(line + n, sizeof(line) - n, " dirty=%u", dirty);
    logf("%s", line);
}

// Zone manager (RVA_ZONEVIS_MGR): bitmasks of 0x3C00 zones at +0x58 (loaded zones) and +0x7d8
// (zones hidden by op 3; op 2 takes them out of that set again, as ZoneVisibleAction does).
// Delete: hide every loaded zone (and keep hiding zones that stream in). Shift+Delete: show the ones
// we hid.
static std::vector<uint16_t> g_hiddenZones;
static bool g_hideOn = false;
static int mgr_count(uintptr_t mgr, uint32_t off, uint32_t bits) {
    int n = 0;
    for (uint32_t i = 0; i < bits / 64; i++) n += __builtin_popcountll(*(const uint64_t*)(mgr + off + i * 8));
    return n;
}
static bool zone_bit(uintptr_t mgr, uint32_t off, uint32_t z) {
    return (*(const uint64_t*)(mgr + off + (z >> 6) * 8) >> (z & 63)) & 1;
}
static uintptr_t zone_mgr() { return (uintptr_t)((ZoneMgr_t)(g_base + RVA_ZONEVIS_MGR))(); }
static void log_mgr(const char* when) {
    uintptr_t mgr = zone_mgr();
    if (!mgr) return;
    logf("zone manager %s (%llx): loaded %d, hidden %d, +f58 %d", when, (unsigned long long)mgr,
         mgr_count(mgr, 0x58, 0x3c00), mgr_count(mgr, 0x7d8, 0x3c00), mgr_count(mgr, 0xf58, 0x1000));
}
// Hide loaded zones that aren't hidden yet; returns how many.
static size_t hide_loaded_zones(uintptr_t mgr) {
    std::vector<uint16_t> list;
    for (uint32_t z = 0; z < 0x3c00; z++)
        if (zone_bit(mgr, 0x58, z) && !zone_bit(mgr, 0x7d8, z)) list.push_back((uint16_t)z);
    if (list.empty()) return 0;
    ((ZoneOp_t)(g_base + RVA_ZONEVIS_OP3))((void*)mgr, list.data(), (int)list.size());
    g_hiddenZones.insert(g_hiddenZones.end(), list.begin(), list.end());
    return list.size();
}
static bool ground_ready();
static void zone_visibility(bool restore) {
    if (!restore && !ground_ready()) {
        logf("zones: Delete ignored - Elden Ring's ground isn't under Ratchet (is riftlink.exe running and LINKED?). "
             "Hiding the level also removes its floor, so it waits for that ground.");
        return;
    }
    uintptr_t mgr = zone_mgr();
    if (!mgr) {
        logf("zones: no zone manager");
        return;
    }
    log_mgr("before");
    if (!restore) {
        g_hideOn = true;
        logf("zones: hiding %zu loaded zones (op 3)", hide_loaded_zones(mgr));
    } else {
        g_hideOn = false;
        if (!g_hiddenZones.empty())
            ((ZoneOp_t)(g_base + RVA_ZONEVIS_OP2))((void*)mgr, g_hiddenZones.data(), (int)g_hiddenZones.size());
        logf("zones: showing the %zu zones we hid (op 2)", g_hiddenZones.size());
        g_hiddenZones.clear();
    }
    g_maskLogFrames = 30;
}
static void zone_frame() {
    static uint32_t n = 0;
    if (!g_hideOn || ++n % 60) return;
    uintptr_t mgr = zone_mgr();
    if (!mgr) return;
    size_t k = hide_loaded_zones(mgr);
    if (k) logf("zones: %zu newly loaded zones hidden too", k);
}

static void mark_tile(uint32_t id);
static void make_platform(uintptr_t mover) {
    if (!g_world || !g_haveTemplate) {
        logf("Home: not ready yet (world %llx, template %ld)", (unsigned long long)g_world, g_haveTemplate);
        return;
    }
    if (g_platforms >= 5) {
        logf("Home: already made 5 platforms");
        return;
    }
    float p[3];
    memcpy(p, (void*)(mover + 0xC8), 12);
    logf("Home: making platform %d above Ratchet at %.3f %.3f %.3f", g_platforms + 1, p[0], p[1], p[2]);

    alignas(16) uint8_t cfg[0x80] = {};
    ((CfgCtor_t)(g_base + RVA_BOX_CFG_CTOR))(cfg);
    logf("Home: build config ready");
    alignas(16) float half[4] = {2.0f, 0.15f, 2.0f, 0.0f};
    void* shape = ((CreateBox_t)(g_base + RVA_CREATE_BOX))(half, 0.02f, cfg);
    std::string nm;
    rtti((uintptr_t)shape, &nm);
    logf("Home: box shape %p (%s)", shape, nm.c_str());
    if (!shape) return;

    alignas(16) uint8_t ci[0xC0] = {};
    memcpy(ci, g_template, 0xB0);
    *(void**)(ci + 0x00) = shape;
    *(uint32_t*)(ci + 0x08) &= ~0xFu;
    *(uint16_t*)(ci + 0x14) = 0;          // default material
    if (g_levelFilter) *(uint32_t*)(ci + 0x10) = g_levelFilter;  // collide like the level's ground
    *(uint64_t*)(ci + 0x18) = 0;          // name
    *(uint64_t*)(ci + 0x20) = 0;          // user data
    float pos[4] = {p[0], p[1] + 2.2f, p[2], 0.0f};
    float rot[4] = {0, 0, 0, 1};
    memcpy(ci + 0x30, pos, 16);
    memcpy(ci + 0x40, rot, 16);
    memset(ci + 0x50, 0, 0x20);           // velocities
    *(uint64_t*)(ci + 0x78) = 0;
    *(uint64_t*)(ci + 0x90) = 0;
    *(uint32_t*)(ci + 0x84) |= 0x00FFFFFFu;  // no reserved body id: allocate one
    ci[0x28] = 0;                            // motion type 0 = static (createBody gives it motion 0)
    logf("Home: cinfo flags %08x filter %08x quality %02x motion %02x mass %g mp %04x id %08x",
         *(uint32_t*)(ci + 0x08), *(uint32_t*)(ci + 0x10), ci[0x16], ci[0x28], *(float*)(ci + 0x70),
         *(uint16_t*)(ci + 0x80), *(uint32_t*)(ci + 0x84));
    uint32_t id = 0xFFFFFFFFu;
    ((CreateBody_t)(g_base + RVA_CREATE_BODY))(g_world, &id, ci, 0);
    logf("Home: createBody returned body id %08x", id);
    if ((id & 0xFFFFFF) != 0xFFFFFF) {
        ((AddBodies_t)(g_base + RVA_ADD_BODIES))(g_world, &id, 1, 0, 0);
        mark_tile(id);
        logf("Home: addBodies done");
    }
    g_platforms++;
    if ((id & 0xFFFFFF) != 0xFFFFFF) {
        uintptr_t bodies = rdq(g_world + 0x28);
        char t[64];
        snprintf(t, sizeof(t), "new body %u", id & 0xFFFFFF);
        dump(t, bodies + (uintptr_t)(id & 0xFFFFFF) * 0xC0, 0xC0);
    }
    g_trackFrames = 60 * 30;
}


// ---------------------------------------------------------------- Elden Ring ground tiles
static RtHeader* g_rt = nullptr;
static uint32_t g_rtGen = 0xFFFFFFFFu;
static RtTile g_rtTiles[RT_MAX_TILES];
static uint32_t g_rtCount = 0, g_rtApply = 0;
static bool g_rtPending = false;
struct TileSlot {
    uint32_t id;
    uint32_t key;
    float pos[3], n[3];
    uint32_t gen;  // generation that last used this slot
    int cls;       // size class (tile shape)
};
// Tile sizes: fine ground, far ground, walls... one box shape per size (half width in RtTile.half).
struct TileClass {
    float half;
    void* shape;
    float thick;  // half thickness: floors are thick (fast throwables can't skip through), walls thin
};
static std::vector<TileClass> g_classes;
static std::vector<TileSlot> g_slots;
static bool ground_ready();
static std::vector<uint8_t> g_isTile;     // by body index
static const float kTileThick = 0.5f, kFloorThick = 1.5f;  // half thickness: walls, floors

static void rt_open() {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\terrain.shm", g_dir);
    HANDLE f = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        logf("terrain: can't open %s (%lu)", path, GetLastError());
        return;
    }
    HANDLE m = CreateFileMappingA(f, nullptr, PAGE_READWRITE, 0, RT_FILE_SIZE, nullptr);
    if (!m) {
        logf("terrain: CreateFileMapping failed (%lu)", GetLastError());
        return;
    }
    g_rt = (RtHeader*)MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, RT_FILE_SIZE);
    if (g_rt) g_rtGen = g_rt->generation;  // whatever is in the file now is from an earlier session
    logf("terrain: feed %s mapped at %p", path, (void*)g_rt);
}

static volatile uint8_t g_tileMark[1 << 17];  // same as g_isTile, safe to read from any thread
static void mark_tile(uint32_t id) {
    uint32_t idx = id & 0xFFFFFF;
    if (idx < (1u << 17)) g_tileMark[idx] = 1;
    if (g_isTile.size() <= idx) g_isTile.resize(idx + 1024, 0);
    g_isTile[idx] = 1;
}

static bool tile_is_floor(const RtTile& t) {
    float nl = sqrtf(t.normal[0] * t.normal[0] + t.normal[1] * t.normal[1] + t.normal[2] * t.normal[2]);
    return nl > 0.1f && t.normal[1] / nl > 0.6f;
}
static int tile_class(float half, bool floor) {
    if (!(half > 0.2f && half < 6.0f)) half = 0.6f;
    half = roundf(half * 20.0f) / 20.0f;
    float thick = floor ? kFloorThick : kTileThick;
    for (size_t i = 0; i < g_classes.size(); i++)
        if (g_classes[i].half == half && g_classes[i].thick == thick) return (int)i;
    alignas(16) uint8_t cfg[0x80] = {};
    ((CfgCtor_t)(g_base + RVA_BOX_CFG_CTOR))(cfg);
    alignas(16) float he[4] = {half, thick, half, 0.0f};
    void* shape = ((CreateBox_t)(g_base + RVA_CREATE_BOX))(he, 0.02f, cfg);
    logf("terrain: %s tile shape %.2f m wide, %.1f m thick: %p", floor ? "floor" : "wall", half * 2, thick * 2, shape);
    if (!shape) return -1;
    g_classes.push_back({half, shape, thick});
    return (int)g_classes.size() - 1;
}

static uint32_t create_tile_body(void* shape) {
    alignas(16) uint8_t ci[0xC0] = {};
    memcpy(ci, g_template, 0xB0);
    *(void**)(ci + 0x00) = shape;
    *(uint32_t*)(ci + 0x08) &= ~0xFu;
    if (g_levelFilter) *(uint32_t*)(ci + 0x10) = g_levelFilter;
    *(uint16_t*)(ci + 0x14) = 0;
    *(uint64_t*)(ci + 0x18) = 0;
    *(uint64_t*)(ci + 0x20) = 0;
    float pos[4] = {0, -5000.0f, 0, 0}, rot[4] = {0, 0, 0, 1};
    memcpy(ci + 0x30, pos, 16);
    memcpy(ci + 0x40, rot, 16);
    memset(ci + 0x50, 0, 0x20);
    *(uint64_t*)(ci + 0x78) = 0;
    *(uint64_t*)(ci + 0x90) = 0;
    *(uint32_t*)(ci + 0x84) |= 0x00FFFFFFu;
    ci[0x28] = 0;  // static
    uint32_t id = 0xFFFFFFFFu;
    ((CreateBody_t)(g_base + RVA_CREATE_BODY))(g_world, &id, ci, 0);
    if ((id & 0xFFFFFF) == 0xFFFFFF) return id;
    ((AddBodies_t)(g_base + RVA_ADD_BODIES))(g_world, &id, 1, 0, 0);
    mark_tile(id);
    apply_variant(id, false, true);
    static bool dumped = false;
    if (!dumped) {
        dumped = true;
        dump("first Elden Ring body", rdq(g_world + 0x28) + (uintptr_t)(id & 0xFFFFFF) * 0xC0, 0xC0);
    }
    return id;
}

static void tile_transform(const RtTile& t, float* m /* 16 floats, 16-aligned */, float thick) {
    float n[3] = {t.normal[0], t.normal[1], t.normal[2]};
    float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (!(len > 0.1f)) n[0] = 0, n[1] = 1, n[2] = 0, len = 1;
    n[0] /= len, n[1] /= len, n[2] /= len;
    // x axis: perpendicular to n, as close to world X as possible
    float ax[3] = {1, 0, 0};
    if (fabsf(n[0]) > 0.9f) ax[0] = 0, ax[2] = 1;
    float d = ax[0] * n[0] + ax[1] * n[1] + ax[2] * n[2];
    float x[3] = {ax[0] - d * n[0], ax[1] - d * n[1], ax[2] - d * n[2]};
    float lx = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    x[0] /= lx, x[1] /= lx, x[2] /= lx;
    float z[3] = {x[1] * n[2] - x[2] * n[1], x[2] * n[0] - x[0] * n[2], x[0] * n[1] - x[1] * n[0]};  // x cross y
    float c[3] = {t.pos[0] - n[0] * thick, t.pos[1] - n[1] * thick, t.pos[2] - n[2] * thick};
    float out[16] = {x[0], x[1], x[2], 0, n[0], n[1], n[2], 0, z[0], z[1], z[2], 0, c[0], c[1], c[2], 1};
    memcpy(m, out, sizeof(out));
}

static bool g_terrainDead = false, g_autoOffDone = false;
static void toggle_level_collision();
static bool g_levelOff = false;
static bool world_still_ours() {
    // After a loading screen or rift the physics world may be rebuilt; then our tiles are gone and
    // touching the old world would crash. Check the world's vtable and that tile 0 still has our shape.
    if (rdq(g_world) != g_base + RVA_VT_WORLD) return false;
    if (!g_slots.empty()) {
        uintptr_t bodies = rdq(g_world + 0x28);
        uint64_t shape = rdq(bodies + (uintptr_t)(g_slots[0].id & 0xFFFFFF) * 0xC0 + 0x60);
        if (shape != (uint64_t)(uintptr_t)g_classes[g_slots[0].cls].shape) return false;
    }
    return true;
}

// Tiles are tied to Elden Ring grid cells (key in RtTile.flags): a cell that stays in range keeps its
// body untouched, so the ground under Ratchet never moves or disappears while the grid follows him.
// Only cells that left the range are recycled for the new ones, and all changes land in one frame.
static uint32_t g_rtAnchor = 0xFFFFFFFFu;

static void place_slot(TileSlot& sl, const RtTile& t) {
    alignas(16) float m[16];
    tile_transform(t, m, sl.cls >= 0 && sl.cls < (int)g_classes.size() ? g_classes[sl.cls].thick : kTileThick);
    ((SetTransform_t)(g_base + RVA_SET_TRANSFORM))(g_world, sl.id, m, 0);
    {
        float nl = sqrtf(t.normal[0] * t.normal[0] + t.normal[1] * t.normal[1] + t.normal[2] * t.normal[2]);
        apply_variant(sl.id, nl > 0.1f && t.normal[1] / nl > 0.6f);  // floor vs wall
    }
    sl.key = t.flags;
    memcpy(sl.pos, t.pos, 12);
    memcpy(sl.n, t.normal, 12);
}
static void park_slot(TileSlot& sl, size_t i) {
    alignas(16) float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -5000.0f - (float)i, 0, 1};
    ((SetTransform_t)(g_base + RVA_SET_TRANSFORM))(g_world, sl.id, m, 0);
    sl.key = 0xFFFFFFFFu;
}

static uint32_t g_lastBeat = 0;
static DWORD g_lastBeatTick = 0;
static bool feed_alive() { return g_lastBeatTick && GetTickCount() - g_lastBeatTick < 3000; }
static bool ground_ready() { return !g_slots.empty() && g_rtCount > 0 && !g_terrainDead && feed_alive(); }

static void terrain_frame() {
    if (!g_rt || !g_world || !g_haveTemplate || g_terrainDead) return;
    static uint32_t chk = 0;
    if (chk++ % 30 == 0 && !world_still_ours()) {
        g_terrainDead = true;
        logf("terrain: Rift Apart's physics world changed (loading screen / rift). Ground feed stopped: "
             "restart Rift Apart to get it back");
        return;
    }
    if (g_rt->reserved[1] != g_lastBeat) {  // riftlink's heartbeat: it's running and linked
        g_lastBeat = g_rt->reserved[1];
        g_lastBeatTick = GetTickCount();
    }
    if (g_rt->magic == RT_MAGIC && g_rt->version == RTER_VERSION && feed_alive()) {
        uint32_t s1 = g_rt->seq;
        if (!(s1 & 1) && g_rt->generation != g_rtGen) {
            uint32_t gen = g_rt->generation, cnt = g_rt->count, anchor = g_rt->reserved[0];
            if (cnt > RT_MAX_TILES) cnt = RT_MAX_TILES;
            memcpy(g_rtTiles, (const void*)g_rt->tiles, cnt * sizeof(RtTile));
            MemoryBarrier();
            if (g_rt->seq == s1) {
                g_rtGen = gen, g_rtCount = cnt, g_rtPending = true;
                if (anchor != g_rtAnchor) {  // re-anchored: every old tile is in the wrong place now
                    for (size_t i = 0; i < g_slots.size(); i++)
                        if (g_slots[i].key != 0xFFFFFFFFu) park_slot(g_slots[i], i);
                    g_rtAnchor = anchor;
                }
            }
        }
    } else if (g_rt->magic == RT_MAGIC && g_rt->version != RTER_VERSION) {
        static bool warned = false;
        if (!warned) logf("terrain: riftlink speaks feed version %u, this module %u: update riftlink.exe", g_rt->version, RTER_VERSION);
        warned = true;
    }
    if (!g_rtPending) return;
    // Size class of every tile; grow each class's pool first (body creation is the slow part: at
    // most 96 per frame).
    static int tcls[RT_MAX_TILES];
    std::vector<int> need, have;
    for (uint32_t t = 0; t < g_rtCount; t++) {
        tcls[t] = tile_class(g_rtTiles[t].half, tile_is_floor(g_rtTiles[t]));
        if (tcls[t] < 0) {
            g_rtPending = false;
            return;
        }
        if ((int)need.size() <= tcls[t]) need.resize(tcls[t] + 1, 0);
        need[tcls[t]]++;
    }
    have.assign(need.size(), 0);
    for (const TileSlot& sl : g_slots)
        if (sl.cls < (int)have.size()) have[sl.cls]++;
    int budget = 96;
    bool short_ = false;
    for (size_t c = 0; c < need.size(); c++)
        while (have[c] < need[c]) {
            if (budget-- <= 0) {
                short_ = true;
                break;
            }
            uint32_t id = create_tile_body(g_classes[c].shape);
            if ((id & 0xFFFFFF) == 0xFFFFFF) {
                logf("terrain: body creation failed at tile %zu; stopping", g_slots.size());
                g_rtPending = false;
                g_terrainDead = true;
                return;
            }
            g_slots.push_back({id, 0xFFFFFFFFu, {0, -5000, 0}, {0, 1, 0}, 0, (int)c});
            have[c]++;
        }
    if (short_) return;  // next frame
    std::unordered_map<uint64_t, uint32_t> byKey;
    byKey.reserve(g_slots.size() * 2);
    for (uint32_t i = 0; i < g_slots.size(); i++)
        if (g_slots[i].key != 0xFFFFFFFFu) byKey[((uint64_t)g_slots[i].key << 8) | (uint32_t)g_slots[i].cls] = i;
    std::vector<uint8_t> used(g_slots.size(), 0);
    std::vector<uint32_t> fresh;
    uint32_t moved = 0;
    for (uint32_t t = 0; t < g_rtCount; t++) {
        const RtTile& tile = g_rtTiles[t];
        auto it = byKey.find(((uint64_t)tile.flags << 8) | (uint32_t)tcls[t]);
        if (it == byKey.end() || used[it->second]) {
            fresh.push_back(t);
            continue;
        }
        TileSlot& sl = g_slots[it->second];
        used[it->second] = 1;
        float d = fabsf(sl.pos[0] - tile.pos[0]) + fabsf(sl.pos[1] - tile.pos[1]) + fabsf(sl.pos[2] - tile.pos[2]) +
                  fabsf(sl.n[0] - tile.normal[0]) + fabsf(sl.n[1] - tile.normal[1]) + fabsf(sl.n[2] - tile.normal[2]);
        if (d > 0.03f) {
            place_slot(sl, tile);
            moved++;
        }
    }
    // Cells that came into range take the slots (same size) of cells that left.
    std::vector<uint32_t> nextOf(g_classes.size(), 0);
    for (uint32_t t : fresh) {
        int c = tcls[t];
        uint32_t& n = nextOf[c];
        while (n < g_slots.size() && (used[n] || g_slots[n].cls != c)) n++;
        if (n >= g_slots.size()) continue;
        used[n] = 1;
        place_slot(g_slots[n], g_rtTiles[t]);
        moved++;
    }
    // Slots nothing wants any more: park them (stale wall pieces trapped Ratchet and blocked shots).
    for (uint32_t i = 0; i < g_slots.size(); i++)
        if (!used[i] && g_slots[i].key != 0xFFFFFFFFu) park_slot(g_slots[i], i);
    g_rtPending = false;
    static DWORD firstReady = 0;
    if (!g_autoOffDone && g_rtCount > 0 && !firstReady) firstReady = GetTickCount();
    // keep Rift Apart's own ground on until a hit on it told us who owns it (or 10 s at most)
    if (!g_autoOffDone && g_rtCount > 0 && true) {
        g_autoOffDone = true;
        if (!g_levelOff) {
            logf("Elden Ring's ground is in place: switching Rift Apart's own level collision off (Pause = back on)");
            toggle_level_collision();
        }
    }
    static uint32_t logged = 0;
    if (logged++ < 10 || logged % 100 == 0)
        logf("terrain: generation %u: %u cells, %u tiles placed or moved, %zu bodies", g_rtGen, g_rtCount, moved,
             g_slots.size());
}

// ---------------------------------------------------------------- Rift Apart level collision on/off
static bool g_pauseKey = false;
static std::vector<std::pair<uint32_t, uint32_t>> g_savedFilters;  // body index, original filter
static const uint32_t kIgnoredFilter = 0x0000800fu;                 // filter Ratchet's mover passed through

static size_t switch_off_new_level_bodies() {
    if (rdq(g_world) != g_base + RVA_VT_WORLD) return 0;
    uint8_t* bodies = (uint8_t*)rdq(g_world + 0x28);
    uint32_t cap = rdd(g_world + 0x30) & 0xFFFFFF;
    if (!bodies || cap == 0 || cap > 0x20000) return 0;
    size_t n = 0;
    for (uint32_t i = 0; i < cap; i++) {
        uint8_t* b = bodies + (size_t)i * 0xC0;
        if (!*(uint64_t*)(b + 0x60) || *(uint32_t*)(b + 0x40) != 0) continue;  // no shape / not static
        if (i < g_isTile.size() && g_isTile[i]) continue;                       // ours
        uint32_t f = *(uint32_t*)(b + 0x6C);
        if (f == kIgnoredFilter) continue;
        g_savedFilters.push_back({i, f});
        *(volatile uint32_t*)(b + 0x6C) = kIgnoredFilter;
        n++;
    }
    return n;
}

static void toggle_level_collision() {
    if (!g_world) {
        logf("level collision: world not found yet");
        return;
    }
    if (rdq(g_world) != g_base + RVA_VT_WORLD) {
        logf("level collision: physics world changed since start; restart Rift Apart");
        g_levelOff = false;
        g_savedFilters.clear();
        return;
    }
    uint8_t* bodies = (uint8_t*)rdq(g_world + 0x28);
    uint32_t cap = rdd(g_world + 0x30) & 0xFFFFFF;
    if (!bodies || cap == 0 || cap > 0x20000) return;
    if (!g_levelOff) {
        g_savedFilters.clear();
        switch_off_new_level_bodies();
        g_levelOff = true;
        logf("level collision OFF: %zu static Rift Apart bodies switched to filter %08x", g_savedFilters.size(),
             kIgnoredFilter);
    } else {
        for (auto& e : g_savedFilters) {
            uint8_t* b = bodies + (size_t)e.first * 0xC0;
            if (*(uint32_t*)(b + 0x6C) == kIgnoredFilter) *(volatile uint32_t*)(b + 0x6C) = e.second;
        }
        logf("level collision ON: %zu bodies restored", g_savedFilters.size());
        g_savedFilters.clear();
        g_levelOff = false;
    }
}

static void cycle_variant();
// ---------------------------------------------------------------- Elden Ring enemies as solid boxes
// (so rockets, grenades and bullets explode on them instead of flying through)
struct ProxySlot {
    uint32_t body;
    uint32_t shapeKey;
    uint32_t enemy;   // RtEnemy.id, 0 = free
    float c[3], yaw;
    bool parked;
};
static std::vector<ProxySlot> g_proxies;
static std::vector<std::pair<uint32_t, void*>> g_proxyShapes;

static uint32_t proxy_shape_key(const float* h) {
    int a = (int)lroundf(fminf(fmaxf(h[0], 0.2f), 6.0f) * 10), b = (int)lroundf(fminf(fmaxf(h[1], 0.2f), 6.0f) * 10),
        c = (int)lroundf(fminf(fmaxf(h[2], 0.2f), 6.0f) * 10);
    return (uint32_t)(a | (b << 8) | (c << 16));
}
static void* proxy_shape(uint32_t key) {
    for (auto& e : g_proxyShapes)
        if (e.first == key) return e.second;
    alignas(16) uint8_t cfg[0x80] = {};
    ((CfgCtor_t)(g_base + RVA_BOX_CFG_CTOR))(cfg);
    alignas(16) float he[4] = {(key & 255) / 10.0f, ((key >> 8) & 255) / 10.0f, ((key >> 16) & 255) / 10.0f, 0};
    void* sh = ((CreateBox_t)(g_base + RVA_CREATE_BOX))(he, 0.02f, cfg);
    if (sh) g_proxyShapes.push_back({key, sh});
    return sh;
}
static void proxy_place(ProxySlot& p, const float* c, float yaw) {
    float cy = cosf(yaw), sy = sinf(yaw);
    alignas(16) float m[16] = {cy, 0, -sy, 0, 0, 1, 0, 0, sy, 0, cy, 0, c[0], c[1], c[2], 1};
    ((SetTransform_t)(g_base + RVA_SET_TRANSFORM))(g_world, p.body, m, 0);
    memcpy(p.c, c, 12);
    p.yaw = yaw;
    p.parked = false;
}
static void proxy_park(ProxySlot& p, size_t i) {
    if (p.parked) return;
    alignas(16) float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -6000.0f - (float)i * 3, 0, 1};
    ((SetTransform_t)(g_base + RVA_SET_TRANSFORM))(g_world, p.body, m, 0);
    p.parked = true;
    p.enemy = 0;
}
static void enemies_frame() {
    if (!g_rt || !g_world || !g_haveTemplate || g_terrainDead || !feed_alive()) return;
    RtEnemy list[RT_MAX_ENEMIES];
    uint32_t n = 0, s1 = g_rt->enemySeq;
    if (s1 & 1) return;
    MemoryBarrier();
    n = g_rt->enemyCount;
    if (n > RT_MAX_ENEMIES) n = RT_MAX_ENEMIES;
    memcpy(list, (const void*)g_rt->enemies, n * sizeof(RtEnemy));
    MemoryBarrier();
    if (g_rt->enemySeq != s1) return;
    std::vector<uint8_t> used(g_proxies.size(), 0);
    int created = 0;
    static uint32_t logged = 0;
    for (uint32_t i = 0; i < n; i++) {
        const RtEnemy& e = list[i];
        uint32_t key = proxy_shape_key(e.half);
        int slot = -1;
        for (size_t k = 0; k < g_proxies.size(); k++)
            if (!used[k] && g_proxies[k].enemy == e.id && g_proxies[k].shapeKey == key) slot = (int)k;
        if (slot < 0)
            for (size_t k = 0; k < g_proxies.size(); k++)
                if (!used[k] && g_proxies[k].shapeKey == key && (g_proxies[k].enemy == 0 || g_proxies[k].parked)) slot = (int)k;
        if (slot < 0 && created < 8) {
            void* sh = proxy_shape(key);
            if (!sh) continue;
            uint32_t id = create_tile_body(sh);
            if ((id & 0xFFFFFF) == 0xFFFFFF) continue;
            g_proxies.push_back({id, key, 0, {0, 0, 0}, 0, true});
            used.push_back(0);
            slot = (int)g_proxies.size() - 1;
            created++;
            if (logged++ < 10) logf("enemies: solid box %.1fx%.1fx%.1f m for an Elden Ring enemy", e.half[0] * 2, e.half[1] * 2, e.half[2] * 2);
        }
        if (slot < 0) continue;
        ProxySlot& p = g_proxies[slot];
        used[slot] = 1;
        bool moved = p.parked || p.enemy != e.id || fabsf(p.c[0] - e.center[0]) + fabsf(p.c[1] - e.center[1]) + fabsf(p.c[2] - e.center[2]) > 0.01f ||
                     fabsf(p.yaw - e.yaw) > 0.01f;
        p.enemy = e.id;
        if (moved) proxy_place(p, e.center, e.yaw);
    }
    for (size_t k = 0; k < g_proxies.size(); k++)
        if (!used[k]) proxy_park(g_proxies[k], k);
}

static void save_variant() {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\bodyvariant10.txt", g_dir);
    FILE* f = fopen(path, "w");
    if (f) fprintf(f, "%d\n", g_variant), fclose(f);
}
static void cycle_variant() {
    if (g_variants.empty() || !g_world || rdq(g_world) != g_base + RVA_VT_WORLD) return;
    g_variant = (g_variant + 1) % (int)g_variants.size();
    size_t n = 0;
    for (auto& sl : g_slots) apply_variant(sl.id, sl.key != 0xFFFFFFFFu && sl.n[1] > 0.6f), n++;
    for (auto& p : g_proxies) apply_variant(p.body, false, true), n++;
    const BodyVariant& v = g_variants[g_variant];
    logf("guns: body variant %d of %zu: filter %08x flags %08x +98 %016llx (%s) on %zu Elden Ring bodies", g_variant,
         g_variants.size(), v.filter, v.flagsHi, (unsigned long long)v.tail,
         g_variant == 0 ? "throwables land (0.9c)" : g_variant == 1 ? "borrowed level owner (0.9b)" :
         g_variant == 2 ? "0.8b" : g_variant == 3 ? "0.7 filters" : "older", n);
    save_variant();
}

// ---------------------------------------------------------------- infinite ammo
// Learns which numbers in Ratchet's weapon and inventory objects go down while the fire button is
// held (and never otherwise), then keeps them at their highest value. Off with riftbridge\noammo.txt.
struct AmmoField {
    uintptr_t addr, obj, vt;
    int32_t maxSeen;
    int score, bad;
    bool on;
};
static std::vector<uintptr_t> g_ammoObjs;
static std::vector<int32_t> g_ammoPrev;
static std::unordered_map<uintptr_t, AmmoField> g_ammo;
static uintptr_t g_heroCache = 0;
static bool g_ammoOff = false;
typedef DWORD(WINAPI* XGetState_t)(DWORD, void*);
static XGetState_t g_xget = nullptr;

static bool fire_held() {
    if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) return true;
    static bool tried = false;
    if (!tried) {
        tried = true;
        HMODULE h = LoadLibraryA("xinput1_4.dll");
        if (h) g_xget = (XGetState_t)GetProcAddress(h, "XInputGetState");
    }
    if (g_xget) {
        struct { DWORD pkt; WORD btn; BYTE lt, rt; SHORT lx, ly, rx, ry; } st;
        for (DWORD i = 0; i < 4; i++)
            if (g_xget(i, &st) == 0 && st.rt > 120) return true;
    }
    return false;
}

static const int kAmmoInts = 0x200;  // 2 KB per object
static void ammo_objects() {
    g_ammoObjs.clear();
    if (!g_heroCache) return;
    // Objects reachable from Ratchet's pointer cache, two levels deep (weapons, inventory, their parts).
    std::vector<std::pair<uintptr_t, int>> q = {{g_heroCache, 0}};
    for (size_t qi = 0; qi < q.size() && g_ammoObjs.size() < 400; qi++) {
        uint64_t v8[0x80];
        if (!rd(q[qi].first, v8, sizeof(v8))) continue;
        if (q[qi].second > 0) g_ammoObjs.push_back(q[qi].first);
        if (q[qi].second >= 2) continue;
        for (int i = 1; i < 0x80; i++) {
            uintptr_t v = v8[i];
            if (v < 0x10000 || v > 0x7FFFFFFFFFF0ull || (v & 7) || (v >= g_base && v < g_base + g_size)) continue;
            uintptr_t vt = rdq(v);
            if (vt < g_base || vt >= g_base + g_size) continue;  // objects with a vtable only
            bool dup = false;
            for (auto& e : q) dup = dup || e.first == v;
            if (!dup) q.push_back({v, q[qi].second + 1});
        }
    }
    g_ammoPrev.assign(g_ammoObjs.size() * kAmmoInts, 0);
    for (size_t o = 0; o < g_ammoObjs.size(); o++) rd(g_ammoObjs[o], &g_ammoPrev[o * kAmmoInts], kAmmoInts * 4);
    static bool logged = false;
    if (!logged) logf("ammo: watching %zu of Ratchet's objects for ammo counts", g_ammoObjs.size()), logged = true;
}

// Rift Apart's own infinite-ammo switch: the byte the game's "Set Infinite Ammo" script action sets
// (read by the weapons' has-infinite-ammo check). Kept on.
static const uint32_t RVA_INFINITE_AMMO = 0x5166064;

// ---------------------------------------------------------------- Elden Ring runes as Ratchet's bolts
// The same lookups Rift Apart's GetCurrencyAction makes (player slot -> entity -> inventory
// component -> bolt counter), then the Tarnished's runes (from riftlink, terrain.shm) are written
// into the counter. Off with riftbridge\nobolts.txt or ShowRunes=0 in riftlink.ini.
static const uint32_t RVA_PLAYER_SLOTS = 0x68d3e78;   // -> 16 player slots, handle at +28 + 20*i
static const uint32_t RVA_HANDLE_TO_ENTITY = 0xf1a520;
static const uint32_t RVA_MAP_FIND_A = 0xffb860, RVA_MAP_FIND_B = 0xffca00;
static const uint32_t RVA_INV_KEY = 0x60313c0;        // component type key (address)
static const uint32_t RVA_GET_INVENTORY = 0x86a660;
static const uint32_t RVA_BOLTS_ARG = 0x60311a8;      // argument GetCurrency passes to the +0x198 call
static bool code_ptr(uintptr_t p) {
    MEMORY_BASIC_INFORMATION mb;
    return p && VirtualQuery((void*)p, &mb, sizeof(mb)) && mb.State == MEM_COMMIT &&
           (mb.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE));
}
static bool obj_ok(uintptr_t o) {  // readable heap object whose vtable points into RiftApart.exe code
    uint64_t vt = 0;
    if (!o || !rd(o, &vt, 8)) return false;
    return vt > g_base && vt < g_base + g_size;
}
static int32_t* find_bolts() {
    uintptr_t slots = 0;
    if (!rd(g_base + RVA_PLAYER_SLOTS, &slots, 8) || !slots) return nullptr;
    for (int i = 0; i < 16; i++) {
        uint32_t h = 0;
        if (!rd(slots + 28 + 20 * i, &h, 4) || !h) continue;
        uintptr_t ent = ((uintptr_t(*)(const uint32_t*))(g_base + RVA_HANDLE_TO_ENTITY))(&h);
        if (!ent) continue;
        uint16_t n = 0;
        if (!rd(ent + 0x88, &n, 2)) continue;
        uintptr_t key = g_base + RVA_INV_KEY, comp;
        if (n) comp = ((uintptr_t(*)(uintptr_t, uintptr_t))(g_base + RVA_MAP_FIND_A))(ent + 0x80, key);
        else comp = ((uintptr_t(*)(uintptr_t, uintptr_t))(g_base + RVA_MAP_FIND_B))(ent + 0x58, key);
        if (!comp) continue;
        uintptr_t inv = ((uintptr_t(*)(uintptr_t))(g_base + RVA_GET_INVENTORY))(comp);
        if (!obj_ok(inv)) continue;
        uintptr_t vt = rdq(inv), f198 = rdq(vt + 0x198);
        if (!code_ptr(f198)) continue;
        uintptr_t q = ((uintptr_t(*)(uintptr_t, uintptr_t))f198)(inv, rdq(g_base + RVA_BOLTS_ARG));
        if (!obj_ok(q)) continue;
        uintptr_t f108 = rdq(rdq(q) + 0x108);
        if (!code_ptr(f108)) continue;
        uintptr_t st = ((uintptr_t(*)(uintptr_t))f108)(q);
        int32_t v;
        if (!st || !rd(st + 0x38, &v, 4)) continue;
        return (int32_t*)(st + 0x38);
    }
    return nullptr;
}
static bool g_boltsOff = false;
static void bolts_frame() {
    static uint32_t frame = 0;
    if (g_boltsOff || ++frame % 15 != 0 || !g_rt || !feed_alive()) return;
    if (g_rt->runesTag != RT_RUNES_TAG) return;
    int32_t runes = g_rt->erRunes;
    if (runes < 0) return;
    if (runes > 9999999) runes = 9999999;
    static int failures = 0;
    if (failures > 20) return;
    int32_t* b = find_bolts();
    static bool logged = false;
    if (!b) {
        if (++failures == 21) logf("bolts: Ratchet's bolt counter not found; runes not shown");
        return;
    }
    if (!logged) {
        logged = true;
        if (*b < 0 || *b > 99999999) {
            logf("bolts: counter candidate %p holds %d, which doesn't look like bolts; runes not shown", (void*)b, *b);
            g_boltsOff = true;
            return;
        }
        logf("bolts: Ratchet's bolt counter at %p holds %d; from now on it shows the Tarnished's runes (%d)", (void*)b, *b, runes);
    }
    if (*b != runes) *(volatile int32_t*)b = runes;
}

static void ammo_frame() {
    static uint32_t frame = 0;
    if (g_ammoOff) return;
    if (frame++ % 30) return;
    volatile uint8_t* flag = (volatile uint8_t*)(g_base + RVA_INFINITE_AMMO);
    if (*flag != 1) {
        *flag = 1;
        static int n = 0;
        if (n++ < 5) logf("ammo: Rift Apart's infinite ammo switched on");
    }
}

static void ammo_frame_learning_unused() {
    static uint32_t frame = 0;
    static DWORD lastFire = 0, lastRefresh = 0;
    if (g_ammoOff || !g_heroCache) return;
    DWORD now = GetTickCount();
    if (fire_held()) lastFire = now;
    if (++frame % 4) return;
    if (g_ammoObjs.empty() || now - lastRefresh > 8000) {
        // Progress report while nothing is learned yet: the best candidates so far.
        int on = 0;
        for (auto& kv : g_ammo) on += kv.second.on;
        static DWORD lastReport = 0;
        if (!on && !g_ammo.empty() && now - lastReport > 20000) {
            lastReport = now;
            std::vector<const AmmoField*> v;
            for (auto& kv : g_ammo) v.push_back(&kv.second);
            std::sort(v.begin(), v.end(), [](const AmmoField* x, const AmmoField* y) { return x->score > y->score; });
            for (size_t i = 0; i < v.size() && i < 5; i++)
                logf("ammo: candidate %llx: went down %d times while firing, %d times otherwise (max %d)", (unsigned long long)v[i]->addr,
                     v[i]->score, v[i]->bad, v[i]->maxSeen);
        }
        lastRefresh = now;
        ammo_objects();
        return;
    }
    bool firing = lastFire && now - lastFire < 1500;
    static std::vector<int32_t> cur;
    cur.resize(g_ammoPrev.size());
    for (size_t o = 0; o < g_ammoObjs.size(); o++) {
        int32_t* c = &cur[o * kAmmoInts];
        if (!rd(g_ammoObjs[o], c, kAmmoInts * 4)) continue;
        const int32_t* p = &g_ammoPrev[o * kAmmoInts];
        for (int i = 2; i < kAmmoInts; i++) {  // skip the vtable
            if (c[i] == p[i]) continue;
            uintptr_t addr = g_ammoObjs[o] + i * 4;
            if (c[i] < p[i] && p[i] - c[i] <= 10 && c[i] >= 0 && p[i] <= 999) {  // ammo: small counts, small steps
                AmmoField& f = g_ammo[addr];
                if (!f.addr) f.addr = addr, f.obj = g_ammoObjs[o], f.vt = (uintptr_t)(uint32_t)c[0] | ((uintptr_t)(uint32_t)c[1] << 32), f.maxSeen = p[i];
                if (firing) f.score++;
                else f.bad++;
                if (!f.on && f.score >= 4 && f.bad * 4 <= f.score) {
                    f.on = true;
                    logf("ammo: kept full: object %llx +%x (was %d, now %d)", (unsigned long long)g_ammoObjs[o], i * 4, f.maxSeen, c[i]);
                }
            } else if (c[i] > p[i]) {
                auto it = g_ammo.find(addr);
                if (it != g_ammo.end() && c[i] > it->second.maxSeen && c[i] <= 999) it->second.maxSeen = c[i];
            }
        }
    }
    for (auto& kv : g_ammo) {  // refill
        AmmoField& f = kv.second;
        if (!f.on || f.bad * 4 > f.score) {
            if (f.on) f.on = false, logf("ammo: stopped keeping %llx full (it also changes when not firing)", (unsigned long long)f.addr);
            continue;
        }
        if (rdq(f.obj) != f.vt) {  // the object went away (weapon swap, loading): forget the field
            f.on = false, f.bad = 1000;
            continue;
        }
        int32_t v;
        if (rd(f.addr, &v, 4) && v >= 0 && v < f.maxSeen) {
            *(volatile int32_t*)f.addr = f.maxSeen;
            size_t idx = 0;
            for (size_t o = 0; o < g_ammoObjs.size(); o++)
                if (f.addr >= g_ammoObjs[o] && f.addr < g_ammoObjs[o] + kAmmoInts * 4) idx = o * kAmmoInts + (f.addr - g_ammoObjs[o]) / 4, cur[idx] = f.maxSeen;
        }
    }
    g_ammoPrev.swap(cur);
}


// ---------------------------------------------------------------- F8: map Rift Apart's actors (diagnostic)
// Step 1 of Elden Ring enemies as Rift Apart stand-ins: learn where an actor's position lives and
// which actors have health. Logs only; changes nothing.
static const uint32_t RVA_ENT_TABLE = 0x6748978, RVA_ENT_COUNT = 0x6748994;
static void comp_list(uintptr_t rec, std::vector<std::pair<std::string, uintptr_t>>* out) {
    uintptr_t tab = rdq(rec + 0x80);
    uint16_t n = 0;
    rd(rec + 0x88, &n, 2);
    if (tab && n && n <= 1024)
        for (uint32_t k = 0; k < n; k++) {
            uintptr_t v = rdq(tab + k * 16 + 8);
            std::string nm;
            if (v && rtti(v, &nm)) out->push_back({nm, v});
        }
    uintptr_t arr = rdq(rec + 0x68);
    uint16_t m = 0;
    rd(rec + 0x70, &m, 2);
    if (arr && m && m <= 1024)
        for (uint32_t k = 0; k < m; k++) {
            uintptr_t v = rdq(arr + k * 16);
            std::string nm;
            if (v && rtti(v, &nm)) out->push_back({nm, v});
        }
}
static void actor_survey() {
    float rp[3];
    if (!g_mover || !rd(g_mover + 0xC8, rp, 12)) return;
    uintptr_t table = rdq(g_base + RVA_ENT_TABLE);
    uint32_t count = rdd(g_base + RVA_ENT_COUNT);
    logf("actors: survey at Ratchet %.2f %.2f %.2f; entity table %p, %u slots", rp[0], rp[1], rp[2], (void*)table, count);
    if (!table || !count || count > 0x100000) return;
    // the hero's record (player slot 0)
    uintptr_t slots = rdq(g_base + 0x68d3e78);
    uint32_t h = slots ? rdd(slots + 28) : 0;
    uintptr_t heroRec = h ? table + (uintptr_t)(h & 0xFFFFF) * 0xC0 : 0;
    logf("actors: hero handle %08x record %p", h, (void*)heroRec);
    int posOff = -1, posPtr = -1;  // position: record+posOff, or [record+posPtr]+posOff
    if (heroRec) {
        dump("hero record", heroRec, 0xC0);
        std::vector<std::pair<std::string, uintptr_t>> cl;
        comp_list(heroRec, &cl);
        for (auto& c : cl) logf("   hero component %s @%p", c.first.c_str(), (void*)c.second);
        auto near3 = [&](const float* f) {
            return fabsf(f[0] - rp[0]) < 0.6f && fabsf(f[1] - rp[1]) < 2.0f && fabsf(f[2] - rp[2]) < 0.6f;
        };
        uint8_t buf[0x800];
        if (rd(heroRec, buf, 0xC0))
            for (int o = 0; o + 12 <= 0xC0; o += 4)
                if (near3((float*)(buf + o))) logf("   position candidate: record+%x", o), posOff < 0 ? (posOff = o) : 0;
        for (int p = 0; p < 0xC0; p += 8) {
            uintptr_t q = rdq(heroRec + p);
            if (!q || (q & 7) || !rd(q, buf, sizeof(buf))) continue;
            for (int o = 0; o + 12 <= (int)sizeof(buf); o += 4)
                if (near3((float*)(buf + o))) {
                    std::string nm;
                    rtti(q, &nm);
                    logf("   position candidate: [record+%x]+%x (%s)", p, o, nm.c_str());
                    if (posOff < 0 && posPtr < 0) posPtr = p, posOff = o;
                }
        }
    }
    // actors with health
    int shown = 0, total = 0;
    for (uint32_t i = 0; i < count && shown < 120; i++) {
        uintptr_t rec = table + (uintptr_t)i * 0xC0;
        uint16_t gen = 0;
        if (!rd(rec + 8, &gen, 2) || !gen) continue;
        std::vector<std::pair<std::string, uintptr_t>> cl;
        comp_list(rec, &cl);
        for (auto& c : cl) {
            if (c.first.find("Health") == std::string::npos || c.first.find("Bar") != std::string::npos) continue;
            total++;
            float hp[2] = {0, 0}, pos[3] = {0, 0, 0};
            rd(c.second + 0x80, hp, 8);
            uintptr_t pb = posPtr >= 0 ? rdq(rec + posPtr) : rec;
            if (posOff >= 0 && pb) rd(pb + posOff, pos, 12);
            float dx = pos[0] - rp[0], dz = pos[2] - rp[2];
            std::string all;
            for (auto& d : cl)
                if (all.size() < 300) all += d.first.substr(4, d.first.size() > 6 ? d.first.size() - 6 : 0) + " ";
            logf("   actor %u (handle %08x) %s hp %.0f/%.0f at %.1f %.1f %.1f (%.0f m): %s", i, (gen << 20) | i,
                 c.first.c_str(), hp[1], hp[0], pos[0], pos[1], pos[2], sqrtf(dx * dx + dz * dz), all.c_str());
            shown++;
            break;
        }
    }
    logf("actors: %d actors with health (%d listed)", total, shown);
}

// ---------------------------------------------------------------- shots: explosions on Elden Ring enemies
// Rift Apart's explosions only hurt Rift Apart actors, and Elden Ring enemies are plain boxes here.
// So: every Ratchet shot is an actor; this thread follows the explosive ones (grenades, bombs and
// their split-off bomblets, rockets...) and when one is gone, the place it was last seen is where
// it went off. Elden Ring enemies within the blast get a hit in terrain.shm; riftlink turns it into
// Elden Ring damage. Also logs every new kind of actor once (for the robots next).
static volatile LONG g_posPtr = -2, g_posOff = -1;  // actor position: [record+ptr]+off (ptr -1: record+off)
static bool actor_pos(uintptr_t rec, float* p) {
    LONG ptr = g_posPtr, off = g_posOff;
    if (off < 0 || ptr == -2) return false;
    uintptr_t b = ptr >= 0 ? rdq(rec + ptr) : rec;
    return b && rd(b + off, p, 12) && isfinite(p[0]) && isfinite(p[1]) && isfinite(p[2]) && fabsf(p[0]) < 1e6f &&
           fabsf(p[1]) < 1e6f && fabsf(p[2]) < 1e6f;
}
struct PosCand { int ptr, off, fails, passes; };
static std::vector<PosCand> g_posCands;
static float g_posStart[3];
static void learn_actor_pos(uintptr_t heroRec, const float* rp) {
    if (!(isfinite(rp[0]) && isfinite(rp[1]) && isfinite(rp[2]))) return;
    {   // known layout (checked in earlier sessions): the actor's transform at [record+0]+0x30
        static int ok = 0, bad = 0;
        float f[3];
        uintptr_t a = rdq(heroRec);
        if (a && rd(a + 0x30, f, 12) && fabsf(f[0] - rp[0]) < 2.0f && fabsf(f[1] - rp[1]) < 3.0f && fabsf(f[2] - rp[2]) < 2.0f) ok++;
        else bad++;
        if (ok >= 5) {
            g_posOff = 0x30;
            InterlockedExchange(&g_posPtr, 0);
            logf("shots: actor position is at [record+0]+30 (the actor's transform; matched Ratchet %d times)", ok);
            return;
        }
        if (bad < 50) return;  // give it ~10 s before falling back to searching
    }
    auto near3 = [&](const float* f) { return fabsf(f[0] - rp[0]) < 0.6f && fabsf(f[1] - rp[1]) < 2.0f && fabsf(f[2] - rp[2]) < 0.6f; };
    uint8_t buf[0x800];
    if (g_posCands.empty()) {
        memcpy(g_posStart, rp, 12);
        if (rd(heroRec, buf, 0xC0))
            for (int o = 0; o + 12 <= 0xC0; o += 4)
                if (near3((float*)(buf + o))) g_posCands.push_back({-1, o, 0, 0});
        for (int p = 0; p < 0xC0; p += 8) {
            uintptr_t q = rdq(heroRec + p);
            if (!q || (q & 7) || !rd(q, buf, sizeof(buf))) continue;
            for (int o = 0; o + 12 <= (int)sizeof(buf); o += 4)
                if (near3((float*)(buf + o))) g_posCands.push_back({p, o, 0, 0});
        }
        if (g_posCands.empty()) return;
        logf("shots: %zu places that might hold an actor's position; checking them while Ratchet moves", g_posCands.size());
        return;
    }
    float d = fabsf(rp[0] - g_posStart[0]) + fabsf(rp[2] - g_posStart[2]);
    auto close = [&](const float* f) { return fabsf(f[0] - rp[0]) < 2.0f && fabsf(f[1] - rp[1]) < 3.0f && fabsf(f[2] - rp[2]) < 2.0f; };
    for (auto& c : g_posCands) {
        float f[3];
        uintptr_t b = c.ptr >= 0 ? rdq(heroRec + c.ptr) : heroRec;
        if (!b || !rd(b + c.off, f, 12) || !close(f)) c.fails++;
        else if (d > 1.0f) c.passes++;
    }
    static DWORD lastNote = 0;
    if (GetTickCount() - lastNote > 15000) {
        lastNote = GetTickCount();
        std::string st;
        for (auto& c : g_posCands) {
            char b[48];
            snprintf(b, sizeof(b), " %x/%x:%d+%d-", c.ptr & 0xFFFF, c.off, c.passes, c.fails);
            st += b;
        }
        logf("shots: still learning where actors keep their position (Ratchet moved %.0f m):%s", d, st.c_str());
    }
    bool allBad = true;
    for (auto& c : g_posCands) allBad = allBad && c.fails > 60 && c.fails > c.passes * 4;
    if (allBad) {  // picked up while the level was still loading: start over
        g_posCands.clear();
        return;
    }
    if (d < 4.0f) return;
    for (auto& c : g_posCands)
        if (c.passes >= 5 && c.fails * 8 <= c.passes) {
            g_posOff = c.off;
            InterlockedExchange(&g_posPtr, c.ptr);
            logf("shots: actor position is at %s+%x (followed Ratchet %d times)", c.ptr >= 0 ? "[record+ptr]" : "record", c.off, c.passes);
            if (c.ptr >= 0) logf("shots:   (ptr = record+%x)", c.ptr);
            return;
        }
}
struct ShotKind { std::string name; bool explosive; float radius; int seen, hits; };
static std::vector<ShotKind> g_shotKinds;
struct TrackedShot { uint32_t handle; uintptr_t rec; int kind; float last[3]; DWORD lastSeen; bool havePos; DWORD born; float startY; int rejects; };
static std::vector<TrackedShot> g_shots;
static std::unordered_map<uintptr_t, std::string> g_vtName;
static std::unordered_map<uint32_t, int> g_handleKind;  // handle -> shot kind (-1 = not a shot)
static std::unordered_set<std::string> g_seenSigs;
static const std::string& vt_name(uintptr_t obj) {
    static const std::string none;
    uintptr_t vt = rdq(obj);
    if (!vt) return none;
    auto it = g_vtName.find(vt);
    if (it != g_vtName.end()) return it->second;
    std::string nm;
    rtti(obj, &nm);
    return g_vtName[vt] = nm;
}
// ---- robots: Ratchet's helpers that hunt on their own (Glove of Doom agents, Drillhounds and their
// pups, homing rockets...). They look for Rift Apart creatures, and Elden Ring enemies aren't any, so
// they wander. riftbridge steers them: every frame each one is moved toward the nearest Elden Ring
// enemy; once there it deals contact damage, and when it's gone (blew up / timed out) its blast counts.
struct Robot {
    uint32_t handle;
    uintptr_t rec, actor;
    std::string name;
    bool isShot;                      // shots already get their blast from the shot tracker
    std::vector<uintptr_t> posCopies; // other places in its parts that hold the same position
    float last[3];
    bool havePos;
    DWORD born, lastBite;
    uint32_t target;
};
static CRITICAL_SECTION g_robotLock;
static std::vector<Robot> g_robotNew;  // found by the scan thread, taken over by the game thread
static bool g_scanBaseline = false;    // first full scan done: later actors are new spawns
static std::unordered_set<std::string> g_spawnSigs;
static bool robot_name(const std::string& s) {
    static const char* words[] = {"GameBot", "DoomBot", "BotDoom", "GloveOfDoom", "Tunneler", "FunGuy", "Fungi", "Drone",
                                  "Minion", "Puppy", "Homing", "Corkscrew", "Seeker", "Swarm", "Glitch"};
    for (const char* w : words)
        if (s.find(w) != std::string::npos) return true;
    return false;
}

static int classify_actor(uintptr_t rec, uint32_t handle) {
    std::vector<std::string> names;
    uintptr_t tab = rdq(rec + 0x80);
    uint16_t n = 0;
    rd(rec + 0x88, &n, 2);
    if (tab && n && n <= 512)
        for (uint32_t k = 0; k < n; k++) {
            uintptr_t v = rdq(tab + k * 16 + 8);
            if (v) names.push_back(vt_name(v));
        }
    uintptr_t arr = rdq(rec + 0x68);
    uint16_t m = 0;
    rd(rec + 0x70, &m, 2);
    if (arr && m && m <= 512)
        for (uint32_t k = 0; k < m; k++) {
            uintptr_t v = rdq(arr + k * 16);
            if (v) names.push_back(vt_name(v));
        }
    std::string motion, sig;
    bool health = false;
    for (auto& nm : names) {
        if (nm.empty()) continue;
        std::string s = nm.size() > 6 ? nm.substr(4, nm.size() - 6) : nm;
        if (s.find("ShotMotion") != std::string::npos && motion.empty()) motion = s;
        if (s.find("Health") != std::string::npos && s.find("Bar") == std::string::npos) health = true;
        if (sig.size() < 400) sig += s + " ";
    }
    if (g_scanBaseline && !sig.empty()) {
        bool owned = sig.find("LogicalOwner") != std::string::npos;
        bool weapon = sig.find("Weapon") != std::string::npos && sig.find("ShotMotion") == std::string::npos;
        bool robot = !weapon && owned && robot_name(sig);  // only Ratchet's own (LogicalOwner)
        if ((owned || robot) && g_spawnSigs.size() < 60 && !g_spawnSigs.count(sig)) {
            g_spawnSigs.insert(sig);
            logf("robots: new %s%s: %s", robot ? "ROBOT " : "", owned ? "(Ratchet's)" : "", sig.c_str());
        }
        if (robot) {
            Robot r = {};
            r.handle = handle, r.rec = rec, r.actor = rdq(rec);
            r.isShot = !motion.empty();
            size_t sp = sig.find(' ');
            for (auto& nm : names)  // name it after its most telling part
                if (!nm.empty() && robot_name(nm)) { r.name = nm.size() > 6 ? nm.substr(4, nm.size() - 6) : nm; break; }
            if (r.name.empty()) r.name = sig.substr(0, sp);
            float p[3];
            if (actor_pos(rec, p) && fabsf(p[0]) + fabsf(p[1]) + fabsf(p[2]) >= 1.0f) {
                memcpy(r.last, p, 12), r.havePos = true;
                // the same position kept inside its parts (motion / mover state): steer those too
                std::vector<uintptr_t> parts;
                uintptr_t tab2 = rdq(rec + 0x80);
                uint16_t n2 = 0;
                rd(rec + 0x88, &n2, 2);
                if (tab2 && n2 && n2 <= 512)
                    for (uint32_t k = 0; k < n2; k++) if (uintptr_t v = rdq(tab2 + k * 16 + 8)) parts.push_back(v);
                uintptr_t arr2 = rdq(rec + 0x68);
                uint16_t m2 = 0;
                rd(rec + 0x70, &m2, 2);
                if (arr2 && m2 && m2 <= 512)
                    for (uint32_t k = 0; k < m2; k++) if (uintptr_t v = rdq(arr2 + k * 16)) parts.push_back(v);
                for (uintptr_t part : parts) {
                    static float buf[0x400];
                    int nf = rd(part, buf, 0x1000) ? 0x400 : rd(part, buf, 0x600) ? 0x180 : 0;
                    for (int o = 2; o + 3 <= nf && r.posCopies.size() < 16; o++)
                        if (fabsf(buf[o] - p[0]) < 0.05f && fabsf(buf[o + 1] - p[1]) < 0.05f && fabsf(buf[o + 2] - p[2]) < 0.05f)
                            r.posCopies.push_back(part + o * 4);
                }
            }
            r.born = GetTickCount();
            EnterCriticalSection(&g_robotLock);
            if (g_robotNew.size() < 64) g_robotNew.push_back(r);
            LeaveCriticalSection(&g_robotLock);
        }
    }
    int kind = -1;
    if (!motion.empty()) {
        for (size_t k = 0; k < g_shotKinds.size(); k++)
            if (g_shotKinds[k].name == motion) kind = (int)k;
        if (kind < 0) {
            auto has = [&](const char* w) { return motion.find(w) != std::string::npos; };
            // Explosive shots only; bullets, blades, bolts and pellets hit what the crosshair is on
            // (riftlink's aim), so they'd count twice here.
            bool expl = has("Grenade") || has("Bouncer") || has("CarpetBomb") || has("Tunneler") || has("Bomb") ||
                        has("Mortar") || has("Rocket") || has("Missile") || has("Homing") || has("Corkscrew") ||
                        has("Arc") || has("Gravity") || has("Explo") || has("Mine") || has("Warmonger") || has("Dispersing");
            float r = has("CarpetBomb") || has("Bouncer") || has("Grenade") ? 5.0f : has("Tunneler") ? 3.5f : 4.0f;
            g_shotKinds.push_back({motion, expl, r, 0, 0});
            kind = (int)g_shotKinds.size() - 1;
            logf("shots: new shot type %d: %s (%s, blast %.1f m) - parts: %s", kind, motion.c_str(),
                 expl ? "explosive" : "bullet/beam, riftlink's aim handles it", r, sig.c_str());
        }
        g_shotKinds[kind].seen++;
    } else if (g_seenSigs.size() < 80 && !sig.empty() && !g_seenSigs.count(sig)) {
        g_seenSigs.insert(sig);
        float p[3] = {0, 0, 0};
        actor_pos(rec, p);
        logf("actors: new kind %08x%s at %.1f %.1f %.1f: %s", handle, health ? " (has health)" : "", p[0], p[1], p[2], sig.c_str());
    }
    return kind;
}
// Elden Ring enemies as riftlink last wrote them (Rift Apart space).
static uint32_t read_enemies(RtEnemy* list) {
    if (!g_rt) return 0;
    for (int tries = 0; tries < 4; tries++) {
        uint32_t s1 = g_rt->enemySeq;
        if (s1 & 1) continue;
        MemoryBarrier();
        uint32_t n = g_rt->enemyCount;
        if (n > RT_MAX_ENEMIES) n = RT_MAX_ENEMIES;
        memcpy(list, (const void*)g_rt->enemies, n * sizeof(RtEnemy));
        MemoryBarrier();
        if (g_rt->enemySeq == s1) return n;
    }
    return 0;
}
// distance from a point to an enemy's box (0 inside)
static float box_dist(const RtEnemy& e, const float* p) {
    float d[3] = {p[0] - e.center[0], p[1] - e.center[1], p[2] - e.center[2]};
    float c = cosf(e.yaw), sn = sinf(e.yaw);
    float lx = d[0] * c - d[2] * sn, lz = d[0] * sn + d[2] * c, ly = d[1];
    float ox = fmaxf(0.0f, fabsf(lx) - e.half[0]), oy = fmaxf(0.0f, fabsf(ly) - e.half[1]), oz = fmaxf(0.0f, fabsf(lz) - e.half[2]);
    return sqrtf(ox * ox + oy * oy + oz * oz);
}
static CRITICAL_SECTION g_hitLock;
static volatile LONG g_hitLockInit = 0;
static void push_hit(uint32_t id, float factor, uint32_t kind, const float* pos) {
    if (!g_rt) return;
    if (InterlockedCompareExchange(&g_hitLockInit, 1, 0) == 0) InitializeCriticalSection(&g_hitLock), InterlockedExchange(&g_hitLockInit, 2);
    while (g_hitLockInit != 2) Sleep(0);
    EnterCriticalSection(&g_hitLock);
    uint32_t w = g_rt->hitWrite;
    auto& h = g_rt->hits[w % 64];
    h.id = id, h.factor = factor, h.kind = kind;
    memcpy(h.pos, pos, 12);
    MemoryBarrier();
    g_rt->hitTag = RT_RUNES_TAG;
    g_rt->hitWrite = w + 1;
    LeaveCriticalSection(&g_hitLock);
}

static void shot_went_off(const TrackedShot& s) {
    if (!s.havePos || s.kind < 0 || !g_rt || !feed_alive()) return;
    ShotKind& k = g_shotKinds[s.kind];
    float rp[3] = {0, 0, 0};
    if (g_moverLive) rd(g_moverLive + 0xC8, rp, 12);
    static int logged = 0;
    // Elden Ring enemies (Rift Apart space, same boxes as the solid stand-ins)
    RtEnemy list[RT_MAX_ENEMIES];
    uint32_t n = 0;
    for (int tries = 0; tries < 4; tries++) {
        uint32_t s1 = g_rt->enemySeq;
        if (s1 & 1) continue;
        MemoryBarrier();
        n = g_rt->enemyCount;
        if (n > RT_MAX_ENEMIES) n = RT_MAX_ENEMIES;
        memcpy(list, (const void*)g_rt->enemies, n * sizeof(RtEnemy));
        MemoryBarrier();
        if (g_rt->enemySeq == s1) break;
        n = 0;
    }
    int hit = 0;
    float dxr = s.last[0] - rp[0], dzr = s.last[2] - rp[2];
    if (!k.explosive && dxr * dxr + dzr * dzr < 250.0f * 250.0f) {
        // a bullet, blade or bolt that ended on an Elden Ring enemy's box: a real direct hit
        int best = -1;
        float bestD = 1.2f;
        for (uint32_t i = 0; i < n; i++) {
            float d = box_dist(list[i], s.last);
            if (d < bestD) bestD = d, best = (int)i;
        }
        if (best >= 0) push_hit(list[best].id, 1.0f, (uint32_t)s.kind | RT_HIT_DIRECT, s.last), hit++, k.hits++;
    }
    if (k.explosive && dxr * dxr + dzr * dzr < 250.0f * 250.0f)
        for (uint32_t i = 0; i < n; i++) {
            const RtEnemy& e = list[i];
            float d[3] = {s.last[0] - e.center[0], s.last[1] - e.center[1], s.last[2] - e.center[2]};
            float c = cosf(e.yaw), sn = sinf(e.yaw);
            float lx = d[0] * c - d[2] * sn, lz = d[0] * sn + d[2] * c, ly = d[1];  // into the box's frame
            float ox = fmaxf(0.0f, fabsf(lx) - e.half[0]), oy = fmaxf(0.0f, fabsf(ly) - e.half[1]), oz = fmaxf(0.0f, fabsf(lz) - e.half[2]);
            float dist = sqrtf(ox * ox + oy * oy + oz * oz);
            if (dist > k.radius) continue;
            float f = 1.0f - 0.6f * dist / k.radius;
            push_hit(e.id, f, (uint32_t)s.kind, s.last);
            hit++, k.hits++;
        }
    float dx = s.last[0] - rp[0], dz = s.last[2] - rp[2];
    if (k.explosive ? (logged < 120 || (hit && logged < 300)) : (hit && logged < 300))
        logged++, logf("shots: %s went off %.1f m from Ratchet, %.1f m %s his feet, after %.1f s%s: %d Elden Ring enemies in the blast (Ratchet at %.0f %.0f %.0f)",
                       k.name.c_str(), sqrtf(dx * dx + dz * dz), fabsf(s.last[1] - rp[1]), s.last[1] < rp[1] ? "below" : "above",
                       (GetTickCount() - s.born) / 1000.0f, k.explosive ? "" : " (not explosive)", hit, rp[0], rp[1], rp[2]);
}
static volatile LONG g_tdSurvey = 1;  // 1 = do one now
static const char* volatile g_tdWhy = "start";
static void targetdata_survey(const char* why);
static DWORD WINAPI shots_thread(void*) {
    logf("shots: watching Ratchet's shots for explosions near Elden Ring enemies");
    DWORD lastScan = 0;
    for (;;) {
        Sleep(10);
        uintptr_t mv = g_moverLive;
        if (!mv) continue;
        float rp[3];
        if (!rd(mv + 0xC8, rp, 12) || fabsf(rp[0]) + fabsf(rp[1]) + fabsf(rp[2]) < 1.0f) continue;
        uintptr_t table = rdq(g_base + RVA_ENT_TABLE);
        uint32_t count = rdd(g_base + RVA_ENT_COUNT);
        if (!table || !count || count > 0x100000) continue;
        if (g_posPtr == -2) {
            uintptr_t slots = rdq(g_base + 0x68d3e78);
            uint32_t h = slots ? rdd(slots + 28) : 0;
            if (h) learn_actor_pos(table + (uintptr_t)(h & 0xFFFFF) * 0xC0, rp);
            Sleep(200);
            continue;
        }
        DWORD now = GetTickCount();
        // follow the shots we know (every 10 ms); gone -> it went off where it was last seen
        for (size_t i = 0; i < g_shots.size();) {
            TrackedShot& s = g_shots[i];
            uint16_t gen = 0;
            rd(s.rec + 8, &gen, 2);
            bool alive = gen && (uint32_t)((gen << 20) | ((s.handle) & 0xFFFFF)) == s.handle;
            float p[3];
            if (alive && actor_pos(s.rec, p)) {
                float jump = fabsf(p[0] - s.last[0]) + fabsf(p[1] - s.last[1]) + fabsf(p[2] - s.last[2]);
                bool origin = fabsf(p[0]) + fabsf(p[1]) + fabsf(p[2]) < 1.0f;  // not placed yet / already cleared
                if (!origin) {
                    // a dying shot's position can jump to junk: ignore single big jumps, but follow a real move
                    if (!s.havePos || jump < 40.0f || ++s.rejects >= 4) {
                        if (!s.havePos) s.startY = p[1];
                        memcpy(s.last, p, 12), s.havePos = true, s.lastSeen = now, s.rejects = 0;
                    }
                }
                i++;
                continue;
            }
            if (!alive) {
                shot_went_off(s);
                g_handleKind.erase(s.handle);
                g_shots[i] = g_shots.back();
                g_shots.pop_back();
                continue;
            }
            i++;
        }
        if (now - lastScan < 60) continue;
        lastScan = now;
        // find new actors (new shots appear here within ~60 ms of being fired)
        for (uint32_t i = 0; i < count; i++) {
            uintptr_t rec = table + (uintptr_t)i * 0xC0;
            uint16_t gen = 0;
            if (!rd(rec + 8, &gen, 2) || !gen) continue;
            uint32_t handle = ((uint32_t)gen << 20) | i;
            if (g_handleKind.count(handle)) continue;
            int kind = classify_actor(rec, handle);
            g_handleKind[handle] = kind;
            if (kind >= 0) {
                TrackedShot s = {handle, rec, kind, {0, 0, 0}, now, false, now, 0, 0};
                float p[3];
                if (actor_pos(rec, p) && fabsf(p[0]) + fabsf(p[1]) + fabsf(p[2]) >= 1.0f) memcpy(s.last, p, 12), s.havePos = true, s.startY = p[1];
                g_shots.push_back(s);
                if (g_rt) g_rt->shotSpawns = g_rt->shotSpawns + 1;  // riftlink: real impacts will follow
            }
        }
        if (!g_scanBaseline) g_scanBaseline = true, logf("robots: %zu actors known; new robots from now on get steered to Elden Ring enemies", g_handleKind.size());
        if (InterlockedExchange(&g_tdSurvey, 0)) targetdata_survey(g_tdWhy);
        if (g_handleKind.size() > 200000) g_handleKind.clear();
    }
}

// Research (read-only): every actor's TargetData (Rift Apart's "can be targeted" part): team at +0x48,
// flags at +0x4c, position at +0x180, registry slot at +0x1a0. Robots pick targets of a hostile team
// from this registry; knowing the teams is the next step toward real stand-ins for Elden Ring enemies.
static void targetdata_survey(const char* why) {
    uintptr_t table = rdq(g_base + RVA_ENT_TABLE);
    uint32_t count = rdd(g_base + RVA_ENT_COUNT);
    if (!table || !count || count > 0x100000) return;
    float rp[3] = {0, 0, 0};
    if (g_moverLive) rd(g_moverLive + 0xC8, rp, 12);
    int shown = 0, total = 0;
    logf("targets (%s): survey of TargetData parts, Ratchet at %.0f %.0f %.0f", why, rp[0], rp[1], rp[2]);
    for (uint32_t i = 0; i < count; i++) {
        uintptr_t rec = table + (uintptr_t)i * 0xC0;
        uint16_t gen = 0;
        if (!rd(rec + 8, &gen, 2) || !gen) continue;
        std::vector<uintptr_t> parts;
        uintptr_t tab = rdq(rec + 0x80);
        uint16_t n = 0;
        rd(rec + 0x88, &n, 2);
        if (tab && n && n <= 512)
            for (uint32_t k = 0; k < n; k++) if (uintptr_t v = rdq(tab + k * 16 + 8)) parts.push_back(v);
        uintptr_t arr = rdq(rec + 0x68);
        uint16_t m = 0;
        rd(rec + 0x70, &m, 2);
        if (arr && m && m <= 512)
            for (uint32_t k = 0; k < m; k++) if (uintptr_t v = rdq(arr + k * 16)) parts.push_back(v);
        uintptr_t td = 0;
        std::string who;
        for (uintptr_t p : parts) {
            const std::string& nm = vt_name(p);
            if (nm.find("TargetData@Target") != std::string::npos) td = p;
            else if (who.size() < 90 && nm.size() > 6 && nm.find("Conduit") == std::string::npos && nm.find("Sound") == std::string::npos &&
                     nm.find("EmergentVO") == std::string::npos)
                who += nm.substr(4, nm.size() - 6) + " ";
        }
        if (!td) continue;
        total++;
        int16_t slot0 = -1;
        rd(td + 0x1a0, &slot0, 2);
        // one line per kind of actor, but every registered (active) target and every robot
        static std::unordered_set<std::string> kinds;
        bool special = slot0 >= 0 || who.find("GameBot") != std::string::npos || who.find("Hero") != std::string::npos;
        if (!special && kinds.count(who)) continue;
        kinds.insert(who);
        if (shown >= 120) continue;
        uint32_t f[8];
        float pos[3] = {0, 0, 0};
        int16_t slot = -1;
        rd(td + 0x40, f, sizeof(f));
        uint32_t g[8];
        rd(td + 0x160, g, sizeof(g));
        rd(td + 0x180, pos, 12);
        rd(td + 0x1a0, &slot, 2);
        float dx = pos[0] - rp[0], dz = pos[2] - rp[2];
        logf("   target %08x team %u flags %08x [+40 %08x %08x +50 %08x %08x %08x %08x +160 %08x %08x %08x %08x %08x %08x %08x %08x] slot %d at %.0f %.0f %.0f (%.0f m): %s",
             ((uint32_t)gen << 20) | i, f[2], f[3], f[0], f[1], f[4], f[5], f[6], f[7], g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7], slot, pos[0], pos[1], pos[2],
             sqrtf(dx * dx + dz * dz), who.c_str());
        shown++;
    }
    logf("targets: %d actors can be targeted (%d listed)", total, shown);
}

// Game thread, every frame: steer the robots.
static std::vector<Robot> g_robots;
static void robots_frame() {
    if (!g_rt) return;
    EnterCriticalSection(&g_robotLock);
    for (auto& r : g_robotNew) {
        g_robots.push_back(r);
        logf("robots: %s %08x joins the hunt (%zu position copies)", r.name.c_str(), r.handle, r.posCopies.size());
        static int surveys = 0;
        if (surveys < 3 && r.name.find("GameBot") != std::string::npos) surveys++, g_tdWhy = "Glove of Doom / Mr. Fungi robots out", InterlockedExchange(&g_tdSurvey, 1);
    }
    g_robotNew.clear();
    LeaveCriticalSection(&g_robotLock);
    if (g_robots.empty()) return;
    static DWORD lastT = GetTickCount();
    DWORD now = GetTickCount();
    float dt = (now - lastT) / 1000.0f;
    lastT = now;
    if (dt > 0.1f) dt = 0.1f;
    RtEnemy list[RT_MAX_ENEMIES];
    uint32_t n = feed_alive() ? read_enemies(list) : 0;
    for (size_t i = 0; i < g_robots.size();) {
        Robot& r = g_robots[i];
        uint16_t gen = 0;
        rd(r.rec + 8, &gen, 2);
        bool alive = gen && (((uint32_t)gen << 20) | (r.handle & 0xFFFFF)) == r.handle && rdq(r.rec) == r.actor;
        if (!alive) {
            int hit = 0;
            if (!r.isShot && r.havePos)  // its own blast (shots get theirs from the shot tracker)
                for (uint32_t k = 0; k < n; k++) {
                    float d = box_dist(list[k], r.last);
                    if (d < 4.0f) push_hit(list[k].id, 1.0f - 0.6f * d / 4.0f, 900, r.last), hit++;
                }
            logf("robots: %s %08x done after %.1f s%s", r.name.c_str(), r.handle, (now - r.born) / 1000.0f,
                 hit ? " (its blast caught Elden Ring enemies)" : "");
            g_robots[i] = g_robots.back();
            g_robots.pop_back();
            continue;
        }
        i++;
        float p[3];
        if (!actor_pos(r.rec, p) || fabsf(p[0]) + fabsf(p[1]) + fabsf(p[2]) < 1.0f) continue;
        memcpy(r.last, p, 12), r.havePos = true;
        if (!n || now - r.born < 250) continue;  // let it leave Ratchet's hand first
        int best = -1;
        float bestD = 70.0f;
        for (uint32_t k = 0; k < n; k++) {
            float d = box_dist(list[k], p);
            if (list[k].id == r.target) d -= 5.0f;  // stick with the one it's after
            if (d < bestD) bestD = d, best = (int)k;
        }
        if (best < 0) continue;
        const RtEnemy& e = list[best];
        if (r.target != e.id) {
            r.target = e.id;
            logf("robots: %s %08x goes after Elden Ring enemy %u (%.0f m)", r.name.c_str(), r.handle, e.id, bestD);
        }
        float aim[3] = {e.center[0], e.center[1] - e.half[1] * 0.3f, e.center[2]};
        float v[3] = {aim[0] - p[0], aim[1] - p[1], aim[2] - p[2]};
        float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        float surf = box_dist(e, p);
        if (surf < 1.2f) {  // there: bite / slash / zap
            if (now - r.lastBite > 600) {
                r.lastBite = now;
                push_hit(e.id, r.isShot ? 0.25f : 0.35f, 901, p);
            }
            continue;
        }
        float step = fminf(len - 0.8f, 14.0f * dt);  // ~14 m/s, stops at the enemy's side
        if (step <= 0 || len < 0.01f) continue;
        float np[3] = {p[0] + v[0] / len * step, p[1] + v[1] / len * step, p[2] + v[2] / len * step};
        LONG pp = g_posPtr, po = g_posOff;
        uintptr_t a = pp >= 0 ? rdq(r.rec + pp) : r.rec;
        DWORD old = 0;
        if (a && po >= 0) {
            float chk[3];
            if (rd(a + po, chk, 12))  // (the actor's own transform: where actor_pos read it)
                *(volatile float*)(a + po) = np[0], *(volatile float*)(a + po + 4) = np[1], *(volatile float*)(a + po + 8) = np[2];
        }
        for (uintptr_t c : r.posCopies) {
            float f[3];
            if (rd(c, f, 12) && fabsf(f[0] - p[0]) < 1.0f && fabsf(f[1] - p[1]) < 1.0f && fabsf(f[2] - p[2]) < 1.0f)
                *(volatile float*)c = np[0], *(volatile float*)(c + 4) = np[1], *(volatile float*)(c + 8) = np[2];
        }
        (void)old;
    }
}


// ---------------------------------------------------------------- collision query diagnostics
// Every physics query (gun rays, grenade sweeps, camera...) asks Rift Apart's collision filter
// (InsomniacCollisionFilter, vtable slot 6: query vs body) whether it may hit a body. This counts,
// per query filter value, how often our Elden Ring pieces and the level's own bodies are tested and
// accepted, and logs the table every few seconds while it changes. Read-only: the answer is the game's.
static const uint32_t RVA_VT_IGFILTER = 0x32e50e0;
typedef bool (*QF_t)(void*, void*, void*, void*);
static QF_t real_QF = nullptr;
struct QfStat { volatile LONG key; volatile LONG ourT, ourP, lvlT, lvlP, hitOur, hitOth; };
static const int QF_N = 512;
static QfStat g_qf[QF_N];
static DWORD g_qfTls = TLS_OUT_OF_INDEXES;
static volatile LONG g_qfDirty = 0;
static bool hk_QF(void* self, void* a, void* q, void* body) {
    bool r = real_QF(self, a, q, body);
    if (!q || !body || !g_world) return r;
    uint32_t qf = *(uint32_t*)((uint8_t*)q + 4);
    uintptr_t bodies = *(uintptr_t*)(g_world + 0x28);
    uintptr_t b = (uintptr_t)body;
    if (b < bodies) return r;
    size_t idx = (b - bodies) / 0xC0;
    if (idx >= (*(uint32_t*)(g_world + 0x30) & 0xFFFFFF) || idx >= (1u << 17)) return r;
    bool ours = g_tileMark[idx] != 0;
    bool level = !ours;  // any other body
    for (int i = 0; i < QF_N; i++) {
        LONG k = g_qf[i].key;
        if (k == 0) {
            if (InterlockedCompareExchange(&g_qf[i].key, (LONG)qf, 0) != 0 && g_qf[i].key != (LONG)qf) continue;
            k = (LONG)qf;
        }
        if (k != (LONG)qf) continue;
        if (ours) InterlockedIncrement(&g_qf[i].ourT), r ? InterlockedIncrement(&g_qf[i].ourP) : 0;
        else InterlockedIncrement(&g_qf[i].lvlT), r ? InterlockedIncrement(&g_qf[i].lvlP) : 0;
        if (ours) InterlockedExchange(&g_qfDirty, 1);
        if (g_qfTls != TLS_OUT_OF_INDEXES) TlsSetValue(g_qfTls, (void*)(uintptr_t)(i + 1));
        break;
    }
    return r;
}
// IgCollisionQueryHitCollector::addHit (vtable slot 4): a query actually hit a body. Attributed to the
// query filter last tested on this thread (the filter runs just before the hit test of that body).
typedef void (*AddHit_t)(void*, void*);
static AddHit_t real_AddHit = nullptr;
typedef void* (*ResolveHit_t)(void* cache, void* out, const uint32_t* bodyId, uint32_t key);
static volatile LONG g_whBody = 0, g_whFilter = 0;
static void hk_AddHit(void* coll, void* hit) {
    bool borrow = !g_variants.empty() && g_variants[(size_t)g_variant % g_variants.size()].tail == 2;
    if (borrow && hit && g_world) {
        uint32_t id = *(uint32_t*)((uint8_t*)hit + 0x48) & 0xFFFFFF;
        uintptr_t bodies = rdq(g_world + 0x28);
        if (id != 0xFFFFFF && bodies && id < (*(uint32_t*)(g_world + 0x30) & 0xFFFFFF) && !(id < (1u << 17) && g_tileMark[id])) {
            uintptr_t b = bodies + (uintptr_t)id * 0xC0;
            uint32_t f = *(uint32_t*)(b + 0x6C), lo = *(uint32_t*)(b + 0x98);
            if (*(uint32_t*)(b + 0x40) == 0 && (f & 0x7FFFFu) == 0x0243fu && (lo & 0xFFFF0000u) == 0xFFFF0000u) {
                uint32_t key = *(uint32_t*)((uint8_t*)hit + 0x50);
                bool dup = false;
                LONG n = g_gkN < 512 ? g_gkN : 512;
                for (LONG i = 0; i < n && !dup; i++) dup = g_gk[i].id == id && g_gk[i].shape == *(uint64_t*)(b + 0x60);
                if (!dup) {
                    LONG i = InterlockedIncrement(&g_gkN) - 1;
                    g_gk[i % 512] = {id, key, *(uint64_t*)(b + 0x60)};  // ring: newest pieces replace the oldest
                }
            }
        }
    }
    if (borrow && hit && !g_worldHandle && g_world) {
        // learn the owner handle of Rift Apart's own ground from a real hit on it (same call addHit makes)
        uint32_t id = *(uint32_t*)((uint8_t*)hit + 0x48) & 0xFFFFFF;
        uintptr_t bodies = rdq(g_world + 0x28);
        if (id != 0xFFFFFF && bodies && id < (*(uint32_t*)(g_world + 0x30) & 0xFFFFFF) && !(id < (1u << 17) && g_tileMark[id])) {
            uintptr_t b = bodies + (uintptr_t)id * 0xC0;
            uint32_t f = *(uint32_t*)(b + 0x6C);
            uint32_t lo = *(uint32_t*)(b + 0x98);
            if (*(uint32_t*)(b + 0x40) == 0 && (f & 0x7FFFFu) == 0x0243fu && (lo & 0xFFFF0000u) == 0xFFFF0000u) {
                alignas(16) uint8_t out[64] = {};
                ((ResolveHit_t)(g_base + 0x11b1290))((void*)(g_base + 0x5324a00), out, (const uint32_t*)((uint8_t*)hit + 0x48),
                                                      *(uint32_t*)((uint8_t*)hit + 0x50));
                uint32_t h = *(uint32_t*)out;
                if (handle_ok(h) && InterlockedCompareExchange(&g_worldHandle, (LONG)h, 0) == 0)
                    g_whBody = (LONG)id, g_whFilter = (LONG)f, g_worldHandleTail = *(LONG64*)(b + 0x98);
            }
        }
    }
    if (hit && g_qfTls != TLS_OUT_OF_INDEXES) {
        uintptr_t i = (uintptr_t)TlsGetValue(g_qfTls);
        uint32_t id = *(uint32_t*)((uint8_t*)hit + 0x48) & 0xFFFFFF;
        if (i && id != 0xFFFFFF) {
            bool ours = id < (1u << 17) && g_tileMark[id];
            InterlockedIncrement(ours ? &g_qf[i - 1].hitOur : &g_qf[i - 1].hitOth);
        }
    }
    real_AddHit(coll, hit);
}
static void qf_install() {
    static bool done = false;
    if (done || !g_world) return;
    done = true;
    void** vt = (void**)(g_base + RVA_VT_IGFILTER);
    // which filter the world uses
    for (int o = 0; o < 0x400; o += 8) {
        uintptr_t p = rdq(g_world + o);
        if (p && rdq(p) == (uintptr_t)vt) logf("queries: world+%x is Rift Apart's collision filter", o);
    }
    DWORD old;
    if (false && VirtualProtect(&vt[6], 8, PAGE_READWRITE, &old)) {  // query statistics: off (done)
        real_QF = (QF_t)vt[6];
        InterlockedExchangePointer(&vt[6], (void*)hk_QF);
        VirtualProtect(&vt[6], 8, old, &old);
        logf("queries: watching which physics queries may hit the Elden Ring ground (diagnostic)");
    }
    g_qfTls = TlsAlloc();
    void** hv = (void**)(g_base + 0x32e81a8);  // IgCollisionQueryHitCollector
    if (VirtualProtect(&hv[4], 8, PAGE_READWRITE, &old)) {
        real_AddHit = (AddHit_t)hv[4];
        InterlockedExchangePointer(&hv[4], (void*)hk_AddHit);
        VirtualProtect(&hv[4], 8, old, &old);
        logf("queries: counting query hits on the Elden Ring ground (diagnostic)");
    }
}
static void qf_report() {
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (now - last < 3000) return;
    last = now;
    static char line[16384];
    int n = snprintf(line, sizeof(line), "queries (filter: ours tested/passed/HIT | others tested/passed/hit):");
    int used = 0;
    for (int i = 0; i < QF_N && g_qf[i].key; i++) {
        used++;
        QfStat& q = g_qf[i];
        LONG ot = InterlockedExchange(&q.ourT, 0), op = InterlockedExchange(&q.ourP, 0);
        LONG lt = InterlockedExchange(&q.lvlT, 0), lp = InterlockedExchange(&q.lvlP, 0);
        LONG ho = InterlockedExchange(&q.hitOur, 0), hh = InterlockedExchange(&q.hitOth, 0);
        if (!ot) continue;
        if (n < (int)sizeof(line) - 80)
            n += snprintf(line + n, sizeof(line) - n, " %08lx: %ld/%ld/%ld | %ld/%ld/%ld;", (unsigned long)q.key, ot, op, ho, lt, lp, hh);
    }
    if (!InterlockedExchange(&g_qfDirty, 0)) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char head[64];
    snprintf(head, sizeof(head), "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    if (n > (int)sizeof(line) - 40) n = (int)sizeof(line) - 40;
    snprintf(line + n, sizeof(line) - n, " [%d kinds]\n", used);
    if (g_log) rawf(head), rawf(line);
}

// ---------------------------------------------------------------- body pair diagnostics
// Thrown objects (grenades, bombs) are moving physics bodies, not queries: they touch the ground only
// if Havok's broadphase pairs them with a ground body and Rift Apart's pair filter (vtable slot 3,
// filterBodyPairs) keeps the pair. This counts every pair that involves one of our pieces, per kind of
// "other" body, and how many the game kept. Read-only.
typedef int (*FBP_t)(void*, void*, uint32_t*, int);
static FBP_t real_FBP = nullptr;
typedef bool (*BPE_t)(void*, int, uint32_t, uint32_t);
static BPE_t real_BPE = nullptr;
struct PairStat { volatile LONG key; volatile LONG t, p; volatile LONG64 tail; volatile LONG flags, motion; };
static PairStat g_ps[48];
static volatile LONG g_fbpCalls = 0, g_fbpPairs = 0, g_bpeT = 0, g_bpeP = 0, g_psDirty = 0;
static inline uintptr_t pair_bodies() {
    uintptr_t w = rdq(g_base + 0x53355f0);
    w = w ? rdq(w + 0x18) : 0;
    return w ? rdq(w + 0x28) : 0;
}
static void ps_add(uintptr_t other, bool kept) {
    LONG key = *(volatile LONG*)(other + 0x6C);
    for (int i = 0; i < 48; i++) {
        LONG k = g_ps[i].key;
        if (k == 0) {
            if (InterlockedCompareExchange(&g_ps[i].key, key, 0) != 0 && g_ps[i].key != key) continue;
            k = key;
        }
        if (k != key) continue;
        InterlockedIncrement(&g_ps[i].t);
        if (kept) InterlockedIncrement(&g_ps[i].p);
        g_ps[i].tail = *(volatile LONG64*)(other + 0x98);
        g_ps[i].flags = *(volatile LONG*)(other + 0x68);
        g_ps[i].motion = *(volatile LONG*)(other + 0x78);
        InterlockedExchange(&g_psDirty, 1);
        return;
    }
}
static int hk_FBP(void* self, void* ctx, uint32_t* pairs, int n) {
    InterlockedIncrement(&g_fbpCalls);
    InterlockedExchangeAdd(&g_fbpPairs, n);
    uint64_t mine[128];
    int nm = 0;
    uintptr_t bodies = n > 0 && pairs ? pair_bodies() : 0;
    if (bodies)
        for (int i = 0; i < n && nm < 128; i++) {
            uint32_t a = pairs[2 * i] & 0xFFFFFF, b = pairs[2 * i + 1] & 0xFFFFFF;
            bool oa = a < (1u << 17) && g_tileMark[a], ob = b < (1u << 17) && g_tileMark[b];
            if (oa != ob) mine[nm++] = ((uint64_t*)pairs)[i];
        }
    int m = real_FBP(self, ctx, pairs, n);
    for (int k = 0; k < nm; k++) {
        bool kept = false;
        for (int i = 0; i < m && !kept; i++) kept = ((uint64_t*)pairs)[i] == mine[k];
        uint32_t a = (uint32_t)mine[k] & 0xFFFFFF, b = (uint32_t)(mine[k] >> 32) & 0xFFFFFF;
        uint32_t other = g_tileMark[a] ? b : a;
        ps_add(bodies + (uintptr_t)other * 0xC0, kept);
    }
    return m;
}
static bool hk_BPE(void* self, int qt, uint32_t a, uint32_t b) {
    bool r = real_BPE(self, qt, a, b);
    uint32_t ia = a & 0xFFFFFF, ib = b & 0xFFFFFF;
    if ((ia < (1u << 17) && g_tileMark[ia]) != (ib < (1u << 17) && g_tileMark[ib])) {
        InterlockedIncrement(&g_bpeT);
        if (r) InterlockedIncrement(&g_bpeP);
        InterlockedExchange(&g_psDirty, 1);
    }
    return r;
}
static void pairs_install() {
    static bool done = false;
    if (done || !g_world) return;
    done = true;
    void** vt = (void**)(g_base + RVA_VT_IGFILTER);
    uintptr_t pb = pair_bodies();
    logf("pairs: filter's body array %llx, our world's %llx%s", (unsigned long long)pb, (unsigned long long)rdq(g_world + 0x28),
         pb == rdq(g_world + 0x28) ? " (same world)" : " (DIFFERENT world)");
    DWORD old;
    if (VirtualProtect(&vt[3], 0x20, PAGE_READWRITE, &old)) {
        real_FBP = (FBP_t)vt[3];
        real_BPE = (BPE_t)vt[5];
        InterlockedExchangePointer(&vt[3], (void*)hk_FBP);
        InterlockedExchangePointer(&vt[5], (void*)hk_BPE);
        VirtualProtect(&vt[3], 0x20, old, &old);
        logf("pairs: watching which moving bodies touch the Elden Ring ground (diagnostic)");
    }
}
static void pairs_report() {
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (now - last < 4000) return;
    last = now;
    LONG calls = InterlockedExchange(&g_fbpCalls, 0), np = InterlockedExchange(&g_fbpPairs, 0);
    if (!InterlockedExchange(&g_psDirty, 0)) {
        static int quiet = 0;
        if (calls && ++quiet % 4 == 0) logf("pairs: %ld pair checks, %ld pairs, none with our ground", calls, np);
        return;
    }
    char line[2048];
    int n = snprintf(line, sizeof(line), "pairs: %ld checks, %ld pairs; by id %ld/%ld; with our ground (other body filter: tested/kept +98 flags +78):",
                     calls, np, InterlockedExchange(&g_bpeT, 0), InterlockedExchange(&g_bpeP, 0));
    for (int i = 0; i < 48 && g_ps[i].key; i++) {
        PairStat& q = g_ps[i];
        LONG t = InterlockedExchange(&q.t, 0), p = InterlockedExchange(&q.p, 0);
        if (!t) continue;
        if (n < (int)sizeof(line) - 90)
            n += snprintf(line + n, sizeof(line) - n, " %08lx: %ld/%ld %016llx %08lx %08lx;", (unsigned long)q.key, t, p,
                          (unsigned long long)q.tail, (unsigned long)q.flags, (unsigned long)q.motion);
    }
    logf("%s", line);
}

// ---------------------------------------------------------------- throwables: hits on ownerless pieces
// Grenades, bombs and other thrown shots look up the object behind each hit (0x11ab4e0, a tiny
// accessor used only by shot motions) and ignore hits with none. Our Elden Ring pieces have no owner
// (handle 0). This replacement answers such hits with a stand-in "world" object, so the shot treats
// the hit like Rift Apart's own ground: the next accessor still finds no actor behind it.
alignas(64) static uint8_t g_worldStandIn[0x1000];
typedef void* (*HandleLookup_t)(void*, uint32_t, uint32_t);
static volatile LONG g_hoCalls = 0, g_hoStandIn = 0;
extern "C" void* hk_hit_object(uintptr_t iter) {
    InterlockedIncrement(&g_hoCalls);
    uintptr_t results = *(uintptr_t*)(*(uintptr_t*)iter);
    int32_t i = *(int32_t*)(iter + 8);
    uint8_t* e = (uint8_t*)results + 8 + (intptr_t)i * 64;
    uint32_t type = e[3];
    if (type) return ((HandleLookup_t)(g_base + 0x12b64e0))(e, *(uint32_t*)e & 0xFFFFFF, type);
    if (g_variant != 0) return nullptr;
    InterlockedIncrement(&g_hoStandIn);
    return (void*)g_worldStandIn;
}
// Crashes: log where (the game's own crash reporter takes over afterwards).
static LONG CALLBACK crash_logger(EXCEPTION_POINTERS* ep) {
    static volatile LONG n = 0;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&n) > 6) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t ip = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    HMODULE me = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ip, &me);
    char mod[MAX_PATH] = "?";
    if (me) GetModuleFileNameA(me, mod, sizeof(mod));
    const char* base = strrchr(mod, '\\');
    CONTEXT* c = ep->ContextRecord;
    logf("EXCEPTION %08lx at %s+%llx (thread %lu) reading/writing %llx; rcx %llx rdx %llx r8 %llx rax %llx; ret %llx",
         code, base ? base + 1 : mod, (unsigned long long)(ip - (uintptr_t)me), GetCurrentThreadId(),
         (unsigned long long)(ep->ExceptionRecord->NumberParameters > 1 ? ep->ExceptionRecord->ExceptionInformation[1] : 0),
         (unsigned long long)c->Rcx, (unsigned long long)c->Rdx, (unsigned long long)c->R8, (unsigned long long)c->Rax,
         (unsigned long long)(*(uintptr_t*)c->Rsp - g_base));
    return EXCEPTION_CONTINUE_SEARCH;
}
static void hitobj_install() {
    static bool done = false;
    if (done) return;
    done = true;
    *(uint32_t*)(g_worldStandIn + 0x5c) = 0x800;  // "not an actor"
    AddVectoredExceptionHandler(0, crash_logger);
    uint8_t* fn = (uint8_t*)(g_base + 0x11ab4e0);
    static const uint8_t expect[] = {0x48, 0x63, 0x51, 0x08, 0x48, 0x8b, 0x01, 0x48};
    if (memcmp(fn, expect, sizeof(expect)) != 0) {
        logf("throwables: accessor 0x11ab4e0 looks different (game updated?); hit fix off");
        return;
    }
    uint8_t* tr = (uint8_t*)alloc_near((uintptr_t)fn);
    if (!tr) {
        logf("throwables: no memory near the game for the hit fix");
        return;
    }
    tr[0] = 0xFF, tr[1] = 0x25, *(uint32_t*)(tr + 2) = 0, *(uint64_t*)(tr + 6) = (uint64_t)(uintptr_t)hk_hit_object;
    int64_t rel = (int64_t)(tr - (fn + 5));
    uint64_t q = *(uint64_t*)fn;
    uint8_t nb[8];
    memcpy(nb, &q, 8);
    nb[0] = 0xE9;
    *(int32_t*)(nb + 1) = (int32_t)rel;
    uint64_t nq;
    memcpy(&nq, nb, 8);
    DWORD old;
    if (VirtualProtect(fn, 8, PAGE_EXECUTE_READWRITE, &old)) {
        InterlockedExchange64((volatile LONG64*)fn, (LONG64)nq);  // one 8-byte write: never half-patched
        VirtualProtect(fn, 8, old, &old);
        FlushInstructionCache(GetCurrentProcess(), fn, 8);
        logf("throwables: hits on the Elden Ring ground now count for grenades, bombs and rockets (no Rift Apart zone needed)");
    }
}

static void wgi_report();
extern "C" void on_mover_update(uintptr_t self) {
    wgi_report();
    if (!g_gameThread) {
        g_gameThread = GetCurrentThreadId();
        g_mover = self;
        logf("hook: first call on thread %lu, mover %llx", g_gameThread, (unsigned long long)self);
    }
    if (GetCurrentThreadId() == g_gameThread) g_moverLive = self;
    bool down = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
    if (down && !g_f7 && GetCurrentThreadId() == g_gameThread) {
        LONG v = g_mouseLook == 35 ? 100 : g_mouseLook == 100 ? 0 : 35;
        InterlockedExchange(&g_mouseLook, v);
        logf("camera: mouse turns the camera at %ld%% (Home: 35%% -> 100%% -> off)", v);
    }
    g_f7 = down;
    bool del = (GetAsyncKeyState(VK_DELETE) & 0x8000) != 0;
    if (del && !g_del && GetCurrentThreadId() == g_gameThread)
        zone_visibility((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);
    g_del = del;
    {
        static bool f8 = false;
        bool d8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        static DWORD autoAt = GetTickCount() + 20000;
        static bool autoDone = true;  // enemy stand-ins on hold: no automatic survey
        if (!autoDone && GetTickCount() > autoAt && GetCurrentThreadId() == g_gameThread) autoDone = true, actor_survey();
        if (d8 && !f8 && GetCurrentThreadId() == g_gameThread) actor_survey();
        f8 = d8;
    }
    bool pz = (GetAsyncKeyState(VK_PAUSE) & 0x8000) != 0;
    if (pz && !g_pauseKey && GetCurrentThreadId() == g_gameThread) {
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) cycle_variant();
        else toggle_level_collision();
    }
    g_pauseKey = pz;
    if (GetCurrentThreadId() == g_gameThread) {
        if (g_haveTemplate) qf_install(), hitobj_install();
        qf_report();
        {   // every 5 s (and on F9): where Ratchet is, and whether throwables' hits find the Elden Ring ground
            static DWORD last = 0;
            static bool f9 = false;
            bool d9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
            bool mark = d9 && !f9;
            f9 = d9;
            if (mark || GetTickCount() - last > 5000) {
                last = GetTickCount();
                float p[3] = {0, 0, 0};
                memcpy(p, (void*)(self + 0xC8), 12);
                LONG c = InterlockedExchange(&g_hoCalls, 0), si = InterlockedExchange(&g_hoStandIn, 0);
                if (mark || c) logf("%sstatus: Ratchet at %.0f %.0f %.0f; throwable hit checks %ld (%ld on the Elden Ring ground); %zu shots flying",
                                    mark ? "F9 MARK " : "", p[0], p[1], p[2], c, si, g_shots.size());
            }
        }
        {
            static bool shotsStarted = false;
            if (!shotsStarted && g_haveTemplate)
                shotsStarted = true, InitializeCriticalSection(&g_robotLock), CreateThread(nullptr, 0, shots_thread, nullptr, 0, nullptr);
            if (shotsStarted) robots_frame();
        }
        {
            // The ground's owner handle belongs to one Rift Apart level zone; zones stream in and out as
            // Ratchet travels, so check it twice a second and take a fresh one from a loaded piece if needed.
            static uint32_t applied = 0;
            static DWORD lastCheck = 0, lastLost = 0;
            DWORD nowT = GetTickCount();
            if (nowT - lastCheck > 500 && !g_variants.empty() && g_variants[(size_t)g_variant % g_variants.size()].tail == 2 &&
                g_world && rdq(g_world) == g_base + RVA_VT_WORLD) {  // (only for the old "borrowed owner" setup)
                lastCheck = nowT;
                uint32_t h = (uint32_t)g_worldHandle;
                if (h && !handle_ok(h)) {
                    uintptr_t bodies = rdq(g_world + 0x28);
                    uint32_t fresh = 0;
                    LONG n = g_gkN < 512 ? g_gkN : 512;
                    typedef void* (*Res2_t)(void* out, const uint32_t* bodyId, const uint32_t* key);
                    for (LONG i = 0; i < n && !fresh; i++) {
                        GroundKey k = g_gk[i];
                        if (!bodies || rdq(bodies + (uintptr_t)k.id * 0xC0 + 0x60) != k.shape) continue;  // gone
                        alignas(16) uint8_t out[64] = {};
                        ((Res2_t)(g_base + 0x11c31f0))(out, &k.id, &k.key);
                        uint32_t c = *(uint32_t*)out;
                        if (handle_ok(c)) fresh = c;
                    }
                    if (fresh) {
                        InterlockedExchange(&g_worldHandle, (LONG)fresh);
                        logf("grenades: the ground's owner %08x streamed out; now %08x", h, fresh);
                    } else if (nowT - lastLost > 20000) {
                        lastLost = nowT;
                        logf("grenades: the ground's owner %08x streamed out and none of %ld known level pieces is loaded", h, n);
                    }
                }
                h = (uint32_t)g_worldHandle;
                if (h != applied && handle_ok(h)) {
                    applied = h;
                    logf("grenades: Elden Ring ground owned by %08x (level body %ld, filter %08lx; %ld level pieces known)", h, g_whBody,
                         (unsigned long)g_whFilter, (long)g_gkN);
                    size_t n = 0;
                for (auto& sl : g_slots) apply_variant(sl.id, sl.key != 0xFFFFFFFFu && sl.n[1] > 0.6f), n++;
                for (auto& p : g_proxies) apply_variant(p.body, false, true), n++;
                    if (g_variants[(size_t)g_variant % g_variants.size()].tail == 2)
                        logf("grenades: %zu Elden Ring pieces now belong to it too", n);
                }
            }
        }
        terrain_frame();
        zone_frame();
        enemies_frame();
        ammo_frame();
        bolts_frame();
        static uint32_t frame = 0;
        if (g_levelOff && ++frame % 120 == 0) {  // level pieces streamed in since: switch them off too
            size_t n = switch_off_new_level_bodies();
            if (n) logf("level collision OFF: %zu newly loaded bodies switched too", n);
        }
    }
    if (g_maskLogFrames > 0 && --g_maskLogFrames == 0) log_mgr("half a second later");
    if (g_trackFrames > 0) {
        if (g_trackFrames % 30 == 0) {
            float p[3], n[3];
            memcpy(p, (void*)(self + 0xC8), 12);
            uintptr_t std_ = 0;
            (void)std_;
            logf("Ratchet %.3f %.3f %.3f", p[0], p[1], p[2]);
            (void)n;
        }
        g_trackFrames--;
    }
}

// ---------------------------------------------------------------- background work

// ---------------------------------------------------------------- play from the Elden Ring window
static HWND g_raHwnd = nullptr;
static WNDPROC g_origProc = nullptr;
static bool g_procUnicode = true;
static volatile LONG g_remote = 0;  // 1 while Elden Ring is the active window
static volatile LONG g_spoof = 0;   // 1 while any other window than Rift Apart's is active
static volatile LONG g_sinkInputs = 0, g_sinkDropped = 0;
static const UINT WM_RB_ACTIVATE = WM_APP + 0x5251, WM_RB_DEACTIVATE = WM_APP + 0x5252;

typedef HWND(WINAPI* HwndFn_t)();
typedef BOOL(WINAPI* ClipCursor_t)(const RECT*);
typedef BOOL(WINAPI* SetCursorPos_t)(int, int);
typedef BOOL(WINAPI* RegRaw_t)(PCRAWINPUTDEVICE, UINT, UINT);
static HwndFn_t real_GetForegroundWindow = GetForegroundWindow, real_GetActiveWindow = GetActiveWindow,
                real_GetFocus = GetFocus;
static ClipCursor_t real_ClipCursor = ClipCursor;
static SetCursorPos_t real_SetCursorPos = SetCursorPos;
static RegRaw_t real_RegisterRawInputDevices = RegisterRawInputDevices;
typedef UINT(WINAPI* GetRawInputData_t)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
static GetRawInputData_t real_GetRawInputData = GetRawInputData;
static bool fake_raw(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT* ret);
static UINT WINAPI hk_GetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT hdrSize) {
    UINT fr;
    if (fake_raw(h, cmd, data, size, &fr)) return fr;
    UINT r = real_GetRawInputData(h, cmd, data, size, hdrSize);
    if (cmd == RID_INPUT && data && r != (UINT)-1 && r >= sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE)) {
        RAWINPUT* ri = (RAWINPUT*)data;
        if (ri->header.dwType == RIM_TYPEMOUSE && !(ri->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
            // Scale the movement (keep the remainders so slow moves still add up); buttons and wheel pass.
            static long remX = 0, remY = 0;
            long pct = g_mouseLook;
            long dy = ri->data.mouse.lLastY;
            if (dy < 0 && g_rt && g_rt->camLow) dy = 0;  // camera down at the ground: no further tilting up
            long x = ri->data.mouse.lLastX * pct + remX, y = dy * pct + remY;
            ri->data.mouse.lLastX = x / 100, ri->data.mouse.lLastY = y / 100;
            remX = x % 100, remY = y % 100;
        }
    }
    return r;
}

static bool pid_is_elden_ring(DWORD pid) {
    static thread_local DWORD cachedPid = 0;
    static thread_local bool cached = false;
    if (pid == cachedPid) return cached;
    bool er = false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) {
        char img[MAX_PATH];
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameA(h, 0, img, &n)) {
            const char* b = strrchr(img, '\\');
            er = _stricmp(b ? b + 1 : img, "eldenring.exe") == 0;
        }
        CloseHandle(h);
    }
    cachedPid = pid, cached = er;
    return er;
}

static bool er_is_foreground() {
    HWND fg = real_GetForegroundWindow();
    DWORD pid = 0;
    if (!fg || !GetWindowThreadProcessId(fg, &pid)) return false;
    return pid_is_elden_ring(pid);
}

static HWND WINAPI hk_GetForegroundWindow() { return g_spoof && g_raHwnd ? g_raHwnd : real_GetForegroundWindow(); }
static HWND WINAPI hk_GetActiveWindow() { return g_spoof && g_raHwnd ? g_raHwnd : real_GetActiveWindow(); }
static HWND WINAPI hk_GetFocus() { return g_spoof && g_raHwnd ? g_raHwnd : real_GetFocus(); }
static BOOL WINAPI hk_ClipCursor(const RECT* r) { return g_spoof ? TRUE : real_ClipCursor(r); }
static BOOL WINAPI hk_SetCursorPos(int x, int y) { return g_spoof ? TRUE : real_SetCursorPos(x, y); }
static BOOL WINAPI hk_RegisterRawInputDevices(PCRAWINPUTDEVICE devs, UINT n, UINT sz) {
    // Keyboard and mouse: also deliver while another window is active (filtered in the window
    // procedure below, so it only counts while Elden Ring is the active window).
    RAWINPUTDEVICE copy[16];
    if (!devs || n == 0 || n > 16 || sz != sizeof(RAWINPUTDEVICE)) return real_RegisterRawInputDevices(devs, n, sz);
    memcpy(copy, devs, n * sizeof(RAWINPUTDEVICE));
    for (UINT i = 0; i < n; i++) {
        RAWINPUTDEVICE& d = copy[i];
        logf("input: Rift Apart registers raw input page %x usage %x flags %lx window %p", d.usUsagePage, d.usUsage,
             (unsigned long)d.dwFlags, (void*)d.hwndTarget);
        bool kbm = d.usUsagePage == 1 && (d.usUsage == 2 || d.usUsage == 6);
        // game controllers (joystick 4, gamepad 5, multi-axis 8) read as raw HID input: keep them coming
        // while Elden Ring is in front too
        bool pad = d.usUsagePage == 1 && (d.usUsage == 4 || d.usUsage == 5 || d.usUsage == 8);
        if ((kbm || pad) && !(d.dwFlags & RIDEV_REMOVE)) {
            if (!d.hwndTarget && pad && g_raHwnd) d.hwndTarget = g_raHwnd;
            if (d.hwndTarget) {
                d.dwFlags |= RIDEV_INPUTSINK;
                if (!g_raHwnd && kbm) g_raHwnd = d.hwndTarget;
            }
        }
    }
    BOOL ok = real_RegisterRawInputDevices(copy, n, sz);
    if (!ok) ok = real_RegisterRawInputDevices(devs, n, sz);  // fall back to the game's own request
    return ok;
}

static bool patch_iat(const char* dll, const char* fn, void* hook, void** realOut) {
    uint8_t* base = (uint8_t*)GetModuleHandleA(nullptr);
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
        if (_stricmp((char*)(base + imp->Name), dll) != 0) continue;
        auto* names = (IMAGE_THUNK_DATA64*)(base + imp->OriginalFirstThunk);
        auto* slots = (IMAGE_THUNK_DATA64*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto* ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((char*)ibn->Name, fn) != 0) continue;
            DWORD old;
            if (!VirtualProtect(&slots->u1.Function, 8, PAGE_READWRITE, &old)) return false;
            if (realOut) *realOut = (void*)slots->u1.Function;
            InterlockedExchangePointer((void**)&slots->u1.Function, hook);
            VirtualProtect(&slots->u1.Function, 8, old, &old);
            return true;
        }
    }
    return false;
}

static bool StrStrIA_safe(const char* hay, const char* needle) {
    size_t n = strlen(needle);
    for (const char* p = hay; *p; p++)
        if (_strnicmp(p, needle, n) == 0) return true;
    return false;
}

// ---------------------------------------------------------------- controller from riftlink
// Rift Apart looks up XInputGetState / XInputGetCapabilities with GetProcAddress. While Elden Ring
// has focus, Steam and Windows only give the controller to the focused game, so these answer for
// pad 0 with what riftlink reads from the controller (terrain.shm, data only).
typedef DWORD(WINAPI* XGS_t)(DWORD, void*);
static XGS_t real_XInputGetState = nullptr, real_XInputGetStateEx = nullptr, real_XInputGetCaps = nullptr;
static FARPROC(WINAPI* real_GetProcAddress)(HMODULE, LPCSTR) = nullptr;
static DWORD g_padSeqSeen = 0, g_padSeqTick = 0;
static bool pad_feed() {  // fresh controller data from riftlink, and Elden Ring (not Rift Apart) has focus
    if (!g_rt || !g_spoof || g_rt->padOn != RT_RUNES_TAG) return false;
    DWORD sq = g_rt->padSeq, now = GetTickCount();
    if (sq != g_padSeqSeen) g_padSeqSeen = sq, g_padSeqTick = now;
    return now - g_padSeqTick < 500;
}
static DWORD fill_pad(DWORD idx, void* out) {
    struct St { DWORD pkt; WORD btn; BYTE lt, rt; SHORT lx, ly, rx, ry; }* st = (St*)out;
    if (idx != 0) return ERROR_DEVICE_NOT_CONNECTED;
    st->pkt = g_rt->padPacket, st->btn = g_rt->padButtons, st->lt = g_rt->padLT, st->rt = g_rt->padRT;
    st->lx = g_rt->padLX, st->ly = g_rt->padLY, st->rx = g_rt->padRX, st->ry = g_rt->padRY;
    static bool logged = false;
    if (!logged) logged = true, logf("controller: Rift Apart reads the controller through riftlink while Elden Ring has focus");
    return ERROR_SUCCESS;
}
static volatile LONG g_xiCalls[4], g_xiOk[4], g_xiFed;
static void xi_report() {  // every 5 s: what Rift Apart asked for and what it got (diagnostics)
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (now - last < 5000) return;
    last = now;
    LONG c0 = InterlockedExchange(&g_xiCalls[0], 0), c1 = InterlockedExchange(&g_xiCalls[1], 0),
         c2 = InterlockedExchange(&g_xiCalls[2], 0), c3 = InterlockedExchange(&g_xiCalls[3], 0);
    LONG o0 = InterlockedExchange(&g_xiOk[0], 0), fed = InterlockedExchange(&g_xiFed, 0);
    InterlockedExchange(&g_xiOk[1], 0), InterlockedExchange(&g_xiOk[2], 0), InterlockedExchange(&g_xiOk[3], 0);
    bool feedOn = g_rt && g_rt->padOn == RT_RUNES_TAG;
    logf("controller: Rift Apart asked XInput %ld/%ld/%ld/%ld times (pads 0-3), pad 0 answered %ld, from riftlink %ld; "
         "Elden Ring active %ld, riftlink controller %s",
         c0, c1, c2, c3, o0, fed, g_spoof, feedOn ? "on" : "off");
}
static DWORD WINAPI hk_XInputGetState(DWORD idx, void* out) {
    if (idx < 4) InterlockedIncrement(&g_xiCalls[idx]);
    DWORD r;
    if (out && pad_feed()) {
        r = fill_pad(idx, out);
        if (r == 0) InterlockedIncrement(&g_xiFed);
    } else {
        r = real_XInputGetState ? real_XInputGetState(idx, out) : ERROR_DEVICE_NOT_CONNECTED;
    }
    if (r == 0 && idx < 4) InterlockedIncrement(&g_xiOk[idx]);
    return r;
}
static DWORD WINAPI hk_XInputGetStateEx(DWORD idx, void* out) {
    if (out && pad_feed()) return fill_pad(idx, out);
    return real_XInputGetStateEx ? real_XInputGetStateEx(idx, out) : ERROR_DEVICE_NOT_CONNECTED;
}
static DWORD WINAPI hk_XInputGetCapabilities(DWORD idx, DWORD flags, void* caps) {
    if (caps && idx == 0 && pad_feed()) {
        uint8_t* c = (uint8_t*)caps;  // XINPUT_CAPABILITIES: type, subtype, flags, gamepad (all buttons/axes), vibration
        memset(c, 0, 20);
        c[0] = 1, c[1] = 1;  // XINPUT_DEVTYPE_GAMEPAD, XINPUT_DEVSUBTYPE_GAMEPAD
        *(WORD*)(c + 4) = 0xF3FF, c[6] = c[7] = 0xFF;
        *(SHORT*)(c + 8) = *(SHORT*)(c + 10) = *(SHORT*)(c + 12) = *(SHORT*)(c + 14) = (SHORT)0xFFC0;
        *(WORD*)(c + 16) = *(WORD*)(c + 18) = 0xFF;
        return ERROR_SUCCESS;
    }
    typedef DWORD(WINAPI * C_t)(DWORD, DWORD, void*);
    return real_XInputGetCaps ? ((C_t)real_XInputGetCaps)(idx, flags, caps) : ERROR_DEVICE_NOT_CONNECTED;
}
static FARPROC WINAPI hk_GetProcAddress(HMODULE m, LPCSTR name) {
    FARPROC r = real_GetProcAddress(m, name);
    if (!r) return r;
    if ((uintptr_t)name <= 0xFFFF) {
        if ((uintptr_t)name == 100) {  // XInputGetStateEx (ordinal 100)
            char path[MAX_PATH];
            if (GetModuleFileNameA(m, path, MAX_PATH) && StrStrIA_safe(path, "xinput")) {
                real_XInputGetStateEx = (XGS_t)r;
                return (FARPROC)hk_XInputGetStateEx;
            }
        }
        return r;
    }
    if (strstr(name, "XInput") || strstr(name, "HidD_") || strstr(name, "Gaming"))
        logf("controller: Rift Apart looked up %s (from %p)", name, __builtin_return_address(0));
    if (!strcmp(name, "XInputGetState")) {
        real_XInputGetState = (XGS_t)r;
        logf("controller: Rift Apart's XInputGetState goes through riftbridge");
        return (FARPROC)hk_XInputGetState;
    }
    if (!strcmp(name, "XInputGetCapabilities")) {
        real_XInputGetCaps = (XGS_t)r;
        return (FARPROC)hk_XInputGetCapabilities;
    }
    return r;
}


// ---------------------------------------------------------------- controller via Windows.Gaming.Input
// Rift Apart reads controllers through Windows.Gaming.Input (RawGameController / Gamepad), which
// only gives input to the focused program. The reading methods are patched in the shared vtables
// of those objects; while Elden Ring is active they answer with the controller riftlink reads.
struct WGuid { uint32_t a; uint16_t b, c; uint8_t d[8]; };
static const WGuid IID_IGamepadStatics = {0x8BBCE529, 0xD49C, 0x39E9, {0x95, 0x60, 0xE4, 0x7D, 0xDE, 0x96, 0xB7, 0xC8}};
static const WGuid IID_IRawGameControllerStatics = {0xEB8D0792, 0xE95A, 0x4B19, {0xAF, 0xC7, 0x0A, 0x59, 0xF8, 0xBF, 0x75, 0x9E}};
typedef HRESULT(WINAPI* GpRead_t)(void* self, void* reading);
typedef HRESULT(WINAPI* RawRead_t)(void* self, UINT32 nb, uint8_t* b, UINT32 ns, int32_t* sw, UINT32 na, double* ax, UINT64* ts);
static GpRead_t real_GpRead = nullptr;
static RawRead_t real_RawRead = nullptr;
static volatile LONG g_gpCalls = 0, g_rawCalls = 0, g_gpFed = 0, g_rawFed = 0;
struct GpReading { UINT64 ts; uint32_t buttons; uint32_t pad; double lt, rt, lx, ly, rx, ry; };
static double stick(int16_t v) { return v < 0 ? v / 32768.0 : v / 32767.0; }
static HRESULT WINAPI hk_GpRead(void* self, void* out) {
    InterlockedIncrement(&g_gpCalls);
    HRESULT hr = real_GpRead(self, out);
    if (out && pad_feed()) {
        GpReading* r = (GpReading*)out;
        uint16_t x = g_rt->padButtons;
        uint32_t b = 0;
        if (x & 0x0010) b |= 1;     // Start -> Menu
        if (x & 0x0020) b |= 2;     // Back -> View
        if (x & 0x1000) b |= 4;     // A
        if (x & 0x2000) b |= 8;     // B
        if (x & 0x4000) b |= 16;    // X
        if (x & 0x8000) b |= 32;    // Y
        if (x & 0x0001) b |= 64;    // dpad up
        if (x & 0x0002) b |= 128;   // down
        if (x & 0x0004) b |= 256;   // left
        if (x & 0x0008) b |= 512;   // right
        if (x & 0x0100) b |= 1024;  // LB
        if (x & 0x0200) b |= 2048;  // RB
        if (x & 0x0040) b |= 4096;  // left stick press
        if (x & 0x0080) b |= 8192;  // right stick press
        r->buttons = b;
        r->lt = g_rt->padLT / 255.0, r->rt = g_rt->padRT / 255.0;
        r->lx = stick(g_rt->padLX), r->ly = stick(g_rt->padLY), r->rx = stick(g_rt->padRX), r->ry = stick(g_rt->padRY);
        if (!r->ts) {
            LARGE_INTEGER c, f;
            QueryPerformanceCounter(&c), QueryPerformanceFrequency(&f);
            r->ts = (UINT64)(c.QuadPart / (f.QuadPart / 1000000));
        }
        InterlockedIncrement(&g_gpFed);
        return 0;
    }
    return hr;
}
// Xbox controllers as raw controllers: buttons A B X Y LB RB View Menu LS RS, one switch (d-pad),
// axes LX LY RX RY LT RT in 0..1 (sticks centred at 0.5, Y down).
static HRESULT WINAPI hk_RawRead(void* self, UINT32 nb, uint8_t* bt, UINT32 ns, int32_t* sw, UINT32 na, double* ax, UINT64* ts) {
    InterlockedIncrement(&g_rawCalls);
    HRESULT hr = real_RawRead(self, nb, bt, ns, sw, na, ax, ts);
    if (pad_feed() && nb >= 10 && na >= 6 && bt && ax) {
        uint16_t x = g_rt->padButtons;
        const uint16_t map[10] = {0x1000, 0x2000, 0x4000, 0x8000, 0x0100, 0x0200, 0x0020, 0x0010, 0x0040, 0x0080};
        for (UINT32 i = 0; i < nb; i++) bt[i] = i < 10 ? ((x & map[i]) != 0) : 0;
        if (ns >= 1 && sw) {
            bool u = x & 1, d = x & 2, l = x & 4, r = x & 8;
            sw[0] = u && r ? 2 : d && r ? 4 : d && l ? 6 : u && l ? 8 : u ? 1 : r ? 3 : d ? 5 : l ? 7 : 0;
        }
        ax[0] = (g_rt->padLX + 32768) / 65535.0, ax[1] = 1.0 - (g_rt->padLY + 32768) / 65535.0;
        ax[2] = (g_rt->padRX + 32768) / 65535.0, ax[3] = 1.0 - (g_rt->padRY + 32768) / 65535.0;
        ax[4] = g_rt->padLT / 255.0, ax[5] = g_rt->padRT / 255.0;
        InterlockedIncrement(&g_rawFed);
        return 0;
    }
    return hr;
}
static bool patch_slot(void* obj, int slot, void* hook, void** realOut) {
    void** vt = *(void***)obj;
    if (vt[slot] == hook) return true;
    DWORD old;
    if (!VirtualProtect(&vt[slot], 8, PAGE_EXECUTE_READWRITE, &old)) return false;
    *realOut = vt[slot];
    InterlockedExchangePointer(&vt[slot], hook);
    VirtualProtect(&vt[slot], 8, old, &old);
    return true;
}
static DWORD WINAPI wgi_thread(void*) {
    HMODULE cb = LoadLibraryA("combase.dll");
    auto RoInit = (HRESULT(WINAPI*)(int))GetProcAddress(cb, "RoInitialize");
    auto RoGetAF = (HRESULT(WINAPI*)(void*, const WGuid*, void**))GetProcAddress(cb, "RoGetActivationFactory");
    auto MkStr = (HRESULT(WINAPI*)(const wchar_t*, UINT32, void**))GetProcAddress(cb, "WindowsCreateString");
    if (!RoInit || !RoGetAF || !MkStr) {
        logf("controller: Windows.Gaming.Input functions not found");
        return 0;
    }
    RoInit(1);
    bool gp = false, raw = false;
    for (int tries = 0; tries < 600 && !(gp && raw); tries++) {
        Sleep(tries ? 2000 : 8000);
        struct { const wchar_t* cls; const WGuid* iid; int slot; void* hook; void** real; bool* done; const char* nm; } t[2] = {
            {L"Windows.Gaming.Input.Gamepad", &IID_IGamepadStatics, 8, (void*)hk_GpRead, (void**)&real_GpRead, &gp, "Gamepad"},
            {L"Windows.Gaming.Input.RawGameController", &IID_IRawGameControllerStatics, 13, (void*)hk_RawRead, (void**)&real_RawRead, &raw, "RawGameController"}};
        for (auto& e : t) {
            if (*e.done) continue;
            void* hs = nullptr;
            void* statics = nullptr;
            if (MkStr(e.cls, (UINT32)wcslen(e.cls), &hs) != 0) continue;
            HRESULT hr = RoGetAF(hs, e.iid, &statics);
            if (hr != 0 || !statics) {
                if (tries == 0) logf("controller: %s statics unavailable (%08lx)", e.nm, (unsigned long)hr);
                continue;
            }
            void** svt = *(void***)statics;
            void* vec = nullptr;
            hr = ((HRESULT(WINAPI*)(void*, void**))svt[10])(statics, &vec);  // get_Gamepads / get_RawGameControllers
            if (hr == 0 && vec) {
                void** vvt = *(void***)vec;
                UINT32 n = 0;
                ((HRESULT(WINAPI*)(void*, UINT32*))vvt[7])(vec, &n);  // get_Size
                if (n > 0) {
                    void* c = nullptr;
                    if (((HRESULT(WINAPI*)(void*, UINT32, void**))vvt[6])(vec, 0, &c) == 0 && c) {  // GetAt(0)
                        *e.done = patch_slot(c, e.slot, e.hook, e.real);
                        logf("controller: %s reading %s (%u connected)", e.nm, *e.done ? "now passes through riftbridge" : "could not be patched", n);
                    }
                } else if (tries % 15 == 0) {
                    logf("controller: no %s connected yet", e.nm);
                }
            }
        }
    }
    return 0;
}
static void wgi_report() {
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (now - last < 5000) return;
    last = now;
    LONG gc = InterlockedExchange(&g_gpCalls, 0), rc = InterlockedExchange(&g_rawCalls, 0);
    LONG gf = InterlockedExchange(&g_gpFed, 0), rf = InterlockedExchange(&g_rawFed, 0);
    if (gc || rc) logf("controller: Rift Apart read Gamepad %ld times (%ld from riftlink), RawGameController %ld times (%ld from riftlink)", gc, gf, rc, rf);
}


// ---------------------------------------------------------------- controller as keyboard + mouse
// Rift Apart reads the controller in a way that stops while another window is in front, but its
// keyboard/mouse input keeps working from the Elden Ring window. So while Elden Ring is active the
// controller (read by riftlink) is turned into keyboard keys and mouse movement for Rift Apart:
// left stick = W A S D, right stick = mouse look, buttons/triggers = keys and mouse buttons as set
// in riftbridge\controller.ini. Synthetic raw input: a WM_INPUT with our own handle, answered by
// the GetRawInputData hook.
static const uintptr_t kFakeTag = 0x7AD00000;
struct FakeEv { RAWINPUT ri; };
static FakeEv g_fake[256];
static volatile LONG g_fakeNext = 0;
static bool fake_raw(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT* ret) {
    uintptr_t v = (uintptr_t)h;
    if ((v & 0xFFFFFF00u) != kFakeTag || !size) return false;
    const RAWINPUT& ri = g_fake[v & 0xFF].ri;
    UINT need = cmd == RID_HEADER ? sizeof(RAWINPUTHEADER) : ri.header.dwSize;
    if (!data) {
        *size = need;
        *ret = 0;
        return true;
    }
    if (*size < need) {
        *size = need;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        *ret = (UINT)-1;
        return true;
    }
    memcpy(data, &ri, need);
    *ret = need;
    return true;
}
static void post_fake(const RAWINPUT& ri) {
    if (!g_raHwnd) return;
    LONG i = InterlockedIncrement(&g_fakeNext) & 0xFF;
    g_fake[i].ri = ri;
    PostMessageW(g_raHwnd, WM_INPUT, RIM_INPUT, (LPARAM)(kFakeTag | (uintptr_t)i));
}
static void fake_key(UINT vk, bool down) {
    RAWINPUT ri = {};
    ri.header.dwType = RIM_TYPEKEYBOARD;
    ri.header.dwSize = sizeof(RAWINPUTHEADER) + sizeof(RAWKEYBOARD);
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    ri.data.keyboard.MakeCode = (USHORT)sc;
    ri.data.keyboard.Flags = down ? RI_KEY_MAKE : RI_KEY_BREAK;
    if (vk == VK_RCONTROL || vk == VK_RMENU || vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT ||
        vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT)
        ri.data.keyboard.Flags |= RI_KEY_E0;
    ri.data.keyboard.VKey = (USHORT)vk;
    ri.data.keyboard.Message = down ? WM_KEYDOWN : WM_KEYUP;
    post_fake(ri);
}
static void fake_mouse(LONG dx, LONG dy, USHORT btnFlags, SHORT wheel) {
    RAWINPUT ri = {};
    ri.header.dwType = RIM_TYPEMOUSE;
    ri.header.dwSize = sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE);
    ri.data.mouse.usFlags = MOUSE_MOVE_RELATIVE;
    ri.data.mouse.usButtonFlags = btnFlags;
    ri.data.mouse.usButtonData = (USHORT)wheel;
    ri.data.mouse.lLastX = dx, ri.data.mouse.lLastY = dy;
    post_fake(ri);
}
// "keys" in controller.ini: a virtual-key name; mouse buttons and the wheel are special.
enum { KM_NONE = 0, KM_LMB = 0x10001, KM_RMB, KM_MMB, KM_WHEELUP, KM_WHEELDOWN };
static int key_from_name(const char* n) {
    struct { const char* n; int vk; } t[] = {
        {"SPACE", VK_SPACE}, {"LSHIFT", VK_LSHIFT}, {"SHIFT", VK_LSHIFT}, {"RSHIFT", VK_RSHIFT}, {"LCTRL", VK_LCONTROL},
        {"CTRL", VK_LCONTROL}, {"RCTRL", VK_RCONTROL}, {"LALT", VK_LMENU}, {"ALT", VK_LMENU}, {"TAB", VK_TAB},
        {"ESC", VK_ESCAPE}, {"ESCAPE", VK_ESCAPE}, {"ENTER", VK_RETURN}, {"BACKSPACE", VK_BACK}, {"CAPSLOCK", VK_CAPITAL},
        {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT}, {"LMB", KM_LMB}, {"RMB", KM_RMB},
        {"MMB", KM_MMB}, {"WHEELUP", KM_WHEELUP}, {"WHEELDOWN", KM_WHEELDOWN}, {"NONE", KM_NONE}};
    for (auto& e : t)
        if (!_stricmp(n, e.n)) return e.vk;
    if (strlen(n) == 1 && isalnum((unsigned char)n[0])) return toupper((unsigned char)n[0]);
    if ((n[0] == 'F' || n[0] == 'f') && atoi(n + 1) >= 1 && atoi(n + 1) <= 12) return VK_F1 + atoi(n + 1) - 1;
    return KM_NONE;
}
struct PadBind { const char* name; uint16_t bit; int trig; const char* def; int key; int holdKey; };
static PadBind g_binds[] = {
    // defaults follow Rift Apart's PC keys: jump Space, crouch C, melee F, quick select Q (hold),
    // Phantom Dash Left Ctrl, interact E, aim right mouse, fire left mouse, Hoverboots Left Shift
    {"A", 0x1000, 0, "SPACE", 0},        {"B", 0x2000, 0, "C", 0},           {"X", 0x4000, 0, "F", 0},
    {"Y", 0x8000, 0, "WHEELUP|MMB", 0},            {"LB", 0x0100, 0, "LSHIFT", 0},      {"RB", 0x0200, 0, "E", 0},
    {"LT", 0, 1, "RMB", 0},              {"RT", 0, 2, "LMB", 0},             {"LS", 0x0040, 0, "LCTRL", 0},
    {"RS", 0x0080, 0, "NONE", 0},           {"Start", 0x0010, 0, "ESC", 0},     {"Back", 0x0020, 0, "TAB", 0},
    {"DPadUp", 0x0001, 0, "1", 0},       {"DPadRight", 0x0008, 0, "2", 0},   {"DPadDown", 0x0002, 0, "3", 0},
    {"DPadLeft", 0x0004, 0, "4", 0},
};
static int g_lookSpeed = 1600, g_padKeysOn = 1;
static void load_pad_binds() {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\controller.ini", g_dir);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        if (FILE* f = fopen(path, "w")) {
            fprintf(f, "; Controller -> Rift Apart while you play from the Elden Ring window.\n"
                       "; Rift Apart's own controller reading stops when its window isn't in front, so the\n"
                       "; controller is turned into keyboard keys and mouse moves. Set each button to the key\n"
                       "; Rift Apart uses for that action (Rift Apart: Settings > Controls > Keyboard).\n"
                       "; Keys: letters, digits, SPACE LSHIFT LCTRL LALT TAB ESC ENTER UP DOWN LEFT RIGHT F1-F12,\n"
                       ";       LMB RMB MMB WHEELUP WHEELDOWN, NONE. Left stick = W A S D, right stick = mouse look.\n"
                       "[controller]\nEnabled=1\nLookSpeed=1600\n[buttons]\n");
            for (auto& b : g_binds) fprintf(f, "%s=%s\n", b.name, b.def);
            fclose(f);
        }
    }
    g_padKeysOn = GetPrivateProfileIntA("controller", "Enabled", 1, path);
    g_lookSpeed = GetPrivateProfileIntA("controller", "LookSpeed", 1600, path);
    for (auto& b : g_binds) {
        char v[32];
        GetPrivateProfileStringA("buttons", b.name, b.def, v, sizeof(v), path);
        // "TAP|HOLD": a quick press sends TAP, holding the button longer holds HOLD down
        char* bar = strchr(v, '|');
        if (bar) {
            *bar = 0;
            b.key = key_from_name(v), b.holdKey = key_from_name(bar + 1);
        } else {
            b.key = key_from_name(v), b.holdKey = -1;
        }
    }
    logf("controller: keyboard/mouse stand-in %s (riftbridge\\controller.ini), look speed %d", g_padKeysOn ? "on" : "off", g_lookSpeed);
}
static void press(int key, bool down, bool* state) {
    if (*state == down || key == KM_NONE) return;
    *state = down;
    switch (key) {
    case KM_LMB: fake_mouse(0, 0, down ? RI_MOUSE_LEFT_BUTTON_DOWN : RI_MOUSE_LEFT_BUTTON_UP, 0); break;
    case KM_RMB: fake_mouse(0, 0, down ? RI_MOUSE_RIGHT_BUTTON_DOWN : RI_MOUSE_RIGHT_BUTTON_UP, 0); break;
    case KM_MMB: fake_mouse(0, 0, down ? RI_MOUSE_MIDDLE_BUTTON_DOWN : RI_MOUSE_MIDDLE_BUTTON_UP, 0); break;
    case KM_WHEELUP: if (down) fake_mouse(0, 0, RI_MOUSE_WHEEL, 120); break;
    case KM_WHEELDOWN: if (down) fake_mouse(0, 0, RI_MOUSE_WHEEL, -120); break;
    default: fake_key((UINT)key, down);
    }
}
static DWORD WINAPI padkeys_thread(void*) {
    while (!g_dir[0]) Sleep(100);
    load_pad_binds();
    bool held[32] = {}, mv[4] = {}, holdOn[32] = {};
    DWORD downAt[32] = {};
    const int kMoveKeys[4] = {'W', 'A', 'S', 'D'};
    double accX = 0, accY = 0;
    bool active = false;
    LARGE_INTEGER f, last;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&last);
    for (;;) {
        Sleep(8);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double dt = (double)(now.QuadPart - last.QuadPart) / f.QuadPart;
        last = now;
        bool on = g_padKeysOn && g_remote && pad_feed();
        if (!on) {
            if (active) {  // release everything we hold
                for (size_t i = 0; i < sizeof(g_binds) / sizeof(g_binds[0]); i++) {
                    press(g_binds[i].key, false, &held[i]);
                    if (g_binds[i].holdKey >= 0) press(g_binds[i].holdKey, false, &holdOn[i]);
                    downAt[i] = 0;
                }
                for (int k = 0; k < 4; k++) press(kMoveKeys[k], false, &mv[k]);
                active = false;
            }
            continue;
        }
        if (!active) active = true, logf("controller: driving Rift Apart as keyboard + mouse while Elden Ring is in front");
        uint16_t bt = g_rt->padButtons;
        uint8_t lt = g_rt->padLT, rt = g_rt->padRT;
        for (size_t i = 0; i < sizeof(g_binds) / sizeof(g_binds[0]); i++) {
            const PadBind& b = g_binds[i];
            bool d = b.trig == 1 ? lt > 80 : b.trig == 2 ? rt > 80 : (bt & b.bit) != 0;
            if (b.holdKey < 0) {
                press(b.key, d, &held[i]);
                continue;
            }
            // tap / hold button
            DWORD tnow = GetTickCount();
            if (d && !downAt[i]) downAt[i] = tnow;
            if (d && !holdOn[i] && tnow - downAt[i] >= 250) press(b.holdKey, true, &holdOn[i]);
            if (!d && downAt[i]) {
                if (holdOn[i]) press(b.holdKey, false, &holdOn[i]);
                else {  // quick tap
                    bool t = false;
                    press(b.key, true, &t);
                    press(b.key, false, &t);
                }
                downAt[i] = 0;
            }
        }
        double lx = stick(g_rt->padLX), ly = stick(g_rt->padLY);
        press('W', ly > 0.35, &mv[0]);
        press('A', lx < -0.35, &mv[1]);
        press('S', ly < -0.35, &mv[2]);
        press('D', lx > 0.35, &mv[3]);
        double rx = stick(g_rt->padRX), ry = stick(g_rt->padRY);
        auto shape = [](double v) {  // dead zone, then a curve: fine aim near the centre
            double a = fabs(v);
            if (a < 0.15) return 0.0;
            a = (a - 0.15) / 0.85;
            return (v < 0 ? -1 : 1) * a * a;
        };
        accX += shape(rx) * g_lookSpeed * dt;
        accY += -shape(ry) * g_lookSpeed * dt;
        LONG dx = (LONG)accX, dy = (LONG)accY;
        if (dx || dy) {
            accX -= dx, accY -= dy;
            fake_mouse(dx, dy, 0, 0);
        }
    }
    return 0;
}

static void install_input_hooks() {
    int ok = 0;
    ok += patch_iat("USER32.dll", "GetForegroundWindow", (void*)hk_GetForegroundWindow, (void**)&real_GetForegroundWindow);
    ok += patch_iat("USER32.dll", "GetActiveWindow", (void*)hk_GetActiveWindow, (void**)&real_GetActiveWindow);
    ok += patch_iat("USER32.dll", "GetFocus", (void*)hk_GetFocus, (void**)&real_GetFocus);
    ok += patch_iat("USER32.dll", "ClipCursor", (void*)hk_ClipCursor, (void**)&real_ClipCursor);
    ok += patch_iat("USER32.dll", "SetCursorPos", (void*)hk_SetCursorPos, (void**)&real_SetCursorPos);
    ok += patch_iat("USER32.dll", "GetRawInputData", (void*)hk_GetRawInputData, (void**)&real_GetRawInputData);
    ok += patch_iat("USER32.dll", "RegisterRawInputDevices", (void*)hk_RegisterRawInputDevices,
                    (void**)&real_RegisterRawInputDevices);
    ok += patch_iat("KERNEL32.dll", "GetProcAddress", (void*)hk_GetProcAddress, (void**)&real_GetProcAddress);
    logf("input: %d of 8 window/input imports redirected", ok);
    CreateThread(nullptr, 0, wgi_thread, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, padkeys_thread, nullptr, 0, nullptr);
}

static LRESULT call_orig(HWND h, UINT m, WPARAM w, LPARAM l) {
    return g_procUnicode ? CallWindowProcW(g_origProc, h, m, w, l) : CallWindowProcA(g_origProc, h, m, w, l);
}

static LRESULT CALLBACK sub_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_RB_ACTIVATE:  // entering Elden Ring: make sure Rift Apart considers itself active
        call_orig(h, WM_ACTIVATEAPP, TRUE, 0);
        call_orig(h, WM_ACTIVATE, WA_ACTIVE, 0);
        call_orig(h, WM_SETFOCUS, 0, 0);
        return 0;
    case WM_RB_DEACTIVATE:  // left Elden Ring for some other program: let Rift Apart know now
        call_orig(h, WM_KILLFOCUS, 0, 0);
        call_orig(h, WM_ACTIVATE, WA_INACTIVE, 0);
        call_orig(h, WM_ACTIVATEAPP, FALSE, 0);
        return 0;
    case WM_ACTIVATEAPP:
        if (!w) return 0;  // never tell Rift Apart it lost focus: it would pause itself
        break;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE) return 0;
        break;
    case WM_KILLFOCUS:
        return 0;
    case WM_NCACTIVATE:
        if (!w) return TRUE;
        break;
    case WM_INPUT:
        {
            static volatile LONG hidSeen[2] = {0, 0};
            int bg = GET_RAWINPUT_CODE_WPARAM(w) == RIM_INPUTSINK;
            RAWINPUTHEADER rh;
            UINT sz = sizeof(rh);
            if (hidSeen[bg] < 2 && real_GetRawInputData((HRAWINPUT)l, RID_HEADER, &rh, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
                rh.dwType == RIM_TYPEHID) {
                InterlockedIncrement(&hidSeen[bg]);
                logf("input: controller raw input arrives (%s)", GET_RAWINPUT_CODE_WPARAM(w) == RIM_INPUTSINK ? "behind Elden Ring" : "Rift Apart in front");
            }
        }
        // Input that arrived while Rift Apart wasn't the active window: only while Elden Ring is.
        if (GET_RAWINPUT_CODE_WPARAM(w) == RIM_INPUTSINK) {
            if (!g_remote) {
                InterlockedIncrement(&g_sinkDropped);
                return DefWindowProcW(h, m, w, l);
            }
            InterlockedIncrement(&g_sinkInputs);
        }
        break;
    }
    return call_orig(h, m, w, l);
}

struct FindRa {
    DWORD pid;
    HWND best;
    long area;
};
static BOOL CALLBACK find_ra_cb(HWND h, LPARAM lp) {
    auto* f = (FindRa*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != f->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    char cls[32] = {};
    GetClassNameA(h, cls, sizeof(cls));
    if (strcmp(cls, "GameNxApp") != 0) return TRUE;  // the game's main window class
    RECT r;
    GetClientRect(h, &r);
    long a = (r.right - r.left) * (r.bottom - r.top) + 1;
    if (a > f->area) f->area = a, f->best = h;
    return TRUE;
}

static DWORD WINAPI input_thread(void*) {
    bool wasRemote = false;
    uint32_t tick = 0;
    for (;;) {
        Sleep(50);
        if (!g_raHwnd || !IsWindow(g_raHwnd)) {
            FindRa f = {GetCurrentProcessId(), nullptr, 0};
            EnumWindows(find_ra_cb, (LPARAM)&f);
            if (f.best != g_raHwnd) g_origProc = nullptr;
            g_raHwnd = f.best;
            if (!g_raHwnd) continue;
        }
        if (!g_origProc) {
            g_procUnicode = IsWindowUnicode(g_raHwnd) != 0;
            LONG_PTR prev = g_procUnicode ? SetWindowLongPtrW(g_raHwnd, GWLP_WNDPROC, (LONG_PTR)sub_proc)
                                          : SetWindowLongPtrA(g_raHwnd, GWLP_WNDPROC, (LONG_PTR)sub_proc);
            g_origProc = (WNDPROC)prev;
            char cls[64] = {};
            GetClassNameA(g_raHwnd, cls, sizeof(cls));
            logf("input: Rift Apart window %p (%s) ready for input from the Elden Ring window", (void*)g_raHwnd, cls);
            // Keyboard + mouse to this window even while Elden Ring is active (the game registered
            // them before we got here, without that flag).
            RAWINPUTDEVICE d[2] = {{1, 6, RIDEV_INPUTSINK, g_raHwnd}, {1, 2, RIDEV_INPUTSINK, g_raHwnd}};
            if (!real_RegisterRawInputDevices(d, 2, sizeof(RAWINPUTDEVICE)))
                logf("input: raw input registration failed (%lu)", GetLastError());
            // game controllers too (joystick, gamepad): raw HID reports keep arriving behind Elden Ring
            RAWINPUTDEVICE g[2] = {{1, 4, RIDEV_INPUTSINK, g_raHwnd}, {1, 5, RIDEV_INPUTSINK, g_raHwnd}};
            if (!real_RegisterRawInputDevices(g, 2, sizeof(RAWINPUTDEVICE)))
                logf("input: controller raw input registration failed (%lu)", GetLastError());
            else
                logf("input: controllers registered for background raw input");
        }
        HWND fgNow = real_GetForegroundWindow();
        InterlockedExchange(&g_spoof, fgNow && fgNow != g_raHwnd ? 1 : 0);
        bool remote = er_is_foreground();
        if (++tick % 100 == 0 && (g_sinkInputs || g_sinkDropped)) {  // every 5 s
            logf("input: %ld keyboard/mouse events passed to Rift Apart from the Elden Ring window, %ld ignored (other programs)",
                 (long)InterlockedExchange(&g_sinkInputs, 0), (long)InterlockedExchange(&g_sinkDropped, 0));
        }
        if (remote != wasRemote) {
            if (remote) {
                InterlockedExchange(&g_remote, 1);
                PostMessageW(g_raHwnd, WM_RB_ACTIVATE, 0, 0);
                logf("input: Elden Ring is the active window -> keyboard, mouse and pad go to Ratchet");
            } else {
                InterlockedExchange(&g_remote, 0);
                HWND fg = real_GetForegroundWindow();
                logf(fg == g_raHwnd ? "input: back in the Rift Apart window"
                                    : "input: another program is active; Rift Apart ignores keyboard/mouse (it keeps running)");
            }
            wasRemote = remote;
        }
    }
}

static DWORD WINAPI main_thread(void*) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\rift-bridge3.log", g_dir);
    g_log = fopen(path, "w");
    MODULEINFO mi = {};
    GetModuleInformation(GetCurrentProcess(), GetModuleHandleA(nullptr), &mi, sizeof(mi));
    g_base = (uintptr_t)mi.lpBaseOfDll;
    g_size = mi.SizeOfImage;
    logf("riftbridge 0.10e, pid %lu, base %llx", GetCurrentProcessId(), (unsigned long long)g_base);
    install_input_hooks();
    CreateThread(nullptr, 0, input_thread, nullptr, 0, nullptr);
    uintptr_t col = rdq(g_base + RVA_VT_HERO - 8);
    uint32_t td = 0;
    char name[64] = {};
    if (!(col >= g_base && col < g_base + g_size && rd(col + 12, &td, 4) && rd(g_base + td + 16, name, 63) &&
          !strcmp(name, ".?AVHeroLocal@Hero@@"))) {
        logf("RiftApart.exe build does NOT match: stopping");
        return 0;
    }
    uintptr_t hero = 0;
    for (int i = 0; !hero && i < 400; i++) {
        if (i % 12 == 0) logf("waiting for Ratchet (load a save)...");
        scan_all(g_base + RVA_VT_HERO, [&](uintptr_t h) {
            uintptr_t c = rdq(h + 0x90), m = rdq(c + 0xE0);
            if (rdq(c) == g_base + RVA_VT_CACHE && rdq(m) == g_base + RVA_VT_MOVER) {
                hero = h;
                return true;
            }
            return false;
        });
        if (!hero) Sleep(5000);
    }
    if (!hero) return 0;
    uintptr_t cache = rdq(hero + 0x90), mover = rdq(cache + 0xE0);
    g_heroCache = cache;
    {
        char na[MAX_PATH];
        snprintf(na, sizeof(na), "%s\\noammo.txt", g_dir);
        g_ammoOff = GetFileAttributesA(na) != INVALID_FILE_ATTRIBUTES;
        logf(g_ammoOff ? "ammo: infinite ammo off (noammo.txt)" : "ammo: infinite ammo on (Rift Apart's own switch)");
        snprintf(na, sizeof(na), "%s\\nobolts.txt", g_dir);
        g_boltsOff = GetFileAttributesA(na) != INVALID_FILE_ATTRIBUTES;
        if (g_boltsOff) logf("bolts: Rift Apart's own bolts (nobolts.txt)");
    }
    logf("Ratchet found: hero %llx mover %llx", (unsigned long long)hero, (unsigned long long)mover);

    install_hook();

    // B. Havok world + static template, in one memory pass: collect the level's physics systems,
    // take the world most of them point to (a real hknpWorld points to itself at +0x18).
    std::vector<uintptr_t> systems;
    {
        // Fast path: walk pointers out from Ratchet's objects (his physics component leads to a
        // physics system), then take the neighbouring systems in the same array (0x60 apart).
        DWORD t0 = GetTickCount();
        uintptr_t first = 0;
        std::vector<std::pair<uintptr_t, int>> queue = {{cache, 0}, {hero, 0}, {mover, 0}};
        std::unordered_map<uintptr_t, bool> seen;
        const uintptr_t vt = g_base + RVA_VT_PHYSSYS;
        for (size_t qi = 0; qi < queue.size() && !first && queue.size() < 40000; qi++) {
            uintptr_t obj = queue[qi].first;
            int depth = queue[qi].second;
            uint64_t q[0x80];
            if (!rd(obj, q, sizeof(q))) continue;
            for (int i = 0; i < 0x80 && !first; i++) {
                uintptr_t v = q[i];
                if (v < 0x10000 || v > 0x7FFFFFFFFFF0ull || (v & 7) || (v >= g_base && v < g_base + g_size)) continue;
                if (seen.count(v)) continue;
                seen[v] = true;
                if (rdq(v) == vt) first = v;
                else if (depth < 3) queue.push_back({v, depth + 1});
            }
        }
        if (first) {
            // Systems sit in arrays with occasional gaps: walk both ways in 16-byte steps, stopping
            // after 1 KB without one.
            uintptr_t lo = first, gap = 0;
            for (uintptr_t a = first - 0x10; gap < 0x400 && first - a < 0x100000; a -= 0x10, gap += 0x10)
                if (rdq(a) == vt) lo = a, gap = 0;
            gap = 0;
            for (uintptr_t a = lo; gap < 0x400 && systems.size() < 4000; a += 0x10, gap += 0x10)
                if (rdq(a) == vt && rdq(a + 0x20) && rdq(rdq(a + 0x20)) == g_base + RVA_VT_WORLD) systems.push_back(a), gap = 0;
            if (systems.empty()) systems.push_back(first);
            logf("physics systems: %zu found next to Ratchet's (%zu objects walked, %lu ms)", systems.size(), queue.size(),
                 GetTickCount() - t0);
        } else {
            logf("physics systems: not reachable from Ratchet (%zu objects walked); scanning memory", queue.size());
        }
    }
    if (systems.size() < 8) {
        DWORD t0 = GetTickCount();
        scan_all(g_base + RVA_VT_PHYSSYS, [&](uintptr_t sys) {
            systems.push_back(sys);
            return systems.size() >= 256;
        });
        logf("%zu physics systems found by scanning (%lu ms)", systems.size(), GetTickCount() - t0);
    }
    {
        std::vector<std::pair<uintptr_t, int>> worlds;
        for (uintptr_t sys : systems) {
            uintptr_t w = rdq(sys + 0x20);
            if (!w || rdq(w) != g_base + RVA_VT_WORLD || rdq(w + 0x18) != w) continue;
            bool found = false;
            for (auto& e : worlds)
                if (e.first == w) e.second++, found = true;
            if (!found) worlds.push_back({w, 1});
        }
        int bestN = 0;
        for (auto& e : worlds) {
            logf("   world %llx used by %d systems", (unsigned long long)e.first, e.second);
            if (e.second > bestN) bestN = e.second, g_world = e.first;
        }
    }
    logf("hknpWorld %llx", (unsigned long long)g_world);
    int motionHist[256] = {}, logged = 0;
    for (uintptr_t sys : systems) {
        uintptr_t data = rdq(sys + 0x18);
        uintptr_t cinfos = rdq(data + 0x38);
        uint32_t count = rdd(data + 0x40);
        if (rdq(sys + 0x20) != g_world || !cinfos || count == 0 || count > 20000) continue;
        for (uint32_t i = 0; i < count && i < 64; i++) {
            uint8_t c[0xB0];
            if (!rd(cinfos + (uintptr_t)i * 0xB0, c, 0xB0)) break;
            uint64_t shape = *(uint64_t*)c;
            uint32_t flags = *(uint32_t*)(c + 8);
            if (!shape || (flags >> 14) & 1) continue;
            motionHist[c[0x28]]++;
            if (logged < 12) {
                logged++;
                std::string sn;
                rtti(shape, &sn);
                logf("cinfo: shape %llx %s flags %08x filter %08x mat %04x qual %02x motion %02x mass %g mp %04x "
                     "id %08x +88 %08x +8c %g +a0 %08x userdata %llx",
                     (unsigned long long)shape, sn.c_str(), flags, *(uint32_t*)(c + 0x10), *(uint16_t*)(c + 0x14),
                     c[0x16], c[0x28], *(float*)(c + 0x70), *(uint16_t*)(c + 0x80), *(uint32_t*)(c + 0x84),
                     *(uint32_t*)(c + 0x88), *(float*)(c + 0x8c), *(uint32_t*)(c + 0xa0),
                     (unsigned long long)*(uint64_t*)(c + 0x20));
            }
        }
    }
    int best = -1;
    for (int m = 0; m < 256; m++) {
        if (motionHist[m]) logf("   motion %02x: %d bodies", m, motionHist[m]);
        if (motionHist[m] && (best < 0 || motionHist[m] > motionHist[best])) best = m;
    }
    for (uintptr_t sys : systems) {
        if (g_haveTemplate) break;
        uintptr_t data = rdq(sys + 0x18);
        uintptr_t cinfos = rdq(data + 0x38);
        uint32_t count = rdd(data + 0x40);
        if (rdq(sys + 0x20) != g_world || !cinfos || count == 0 || count > 20000) continue;
        for (uint32_t i = 0; i < count && i < 64; i++) {
            uint8_t c[0xB0];
            if (!rd(cinfos + (uintptr_t)i * 0xB0, c, 0xB0)) break;
            if (!*(uint64_t*)c || ((*(uint32_t*)(c + 8)) >> 14) & 1) continue;
            if (c[0x28] == best) {
                memcpy(g_template, c, 0xB0);
                InterlockedExchange(&g_haveTemplate, 1);
                logf("template: system %llx cinfo %u (motion %02x)", (unsigned long long)sys, i, best);
                break;
            }
        }
    }
    // Collision filter of the level's static ground: most common filter among static bodies
    // (motion id at +0x40 == 0) in the world's body array.
    if (g_world) {
        uintptr_t bodies = rdq(g_world + 0x28);
        uint32_t cap = rdd(g_world + 0x30) & 0xFFFFFF;
        if (cap > 0x20000) cap = 0x20000;
        std::vector<uint8_t> all((size_t)cap * 0xC0);
        if (bodies && cap && rd(bodies, all.data(), all.size())) {
            std::vector<std::pair<uint32_t, int>> hist;
            int statics = 0, dynamics = 0;
            for (uint32_t i = 0; i < cap; i++) {
                const uint8_t* b = &all[(size_t)i * 0xC0];
                uint64_t shape = *(const uint64_t*)(b + 0x60);
                if (!shape) continue;
                uint32_t motion = *(const uint32_t*)(b + 0x40), filter = *(const uint32_t*)(b + 0x6C);
                if (motion != 0) {
                    dynamics++;
                    continue;
                }
                statics++;
                bool f = false;
                for (auto& e : hist)
                    if (e.first == filter) e.second++, f = true;
                if (!f) hist.push_back({filter, 1});
            }
            logf("world bodies: %d static, %d moving (capacity %u)", statics, dynamics, cap);
            int bestN = 0;
            for (auto& e : hist) {
                if (e.second >= 5) logf("   static filter %08x: %d bodies", e.first, e.second);
                if (e.second > bestN) bestN = e.second, g_levelFilter = e.first;
            }
            int shown = 0;
            for (uint32_t i = 0; i < cap && shown < 3; i++) {
                const uint8_t* b = &all[(size_t)i * 0xC0];
                if (*(const uint64_t*)(b + 0x60) && *(const uint32_t*)(b + 0x40) == 0 &&
                    *(const uint32_t*)(b + 0x6C) == g_levelFilter) {
                    char t[64];
                    snprintf(t, sizeof(t), "level static body %u", i);
                    dump(t, bodies + (uintptr_t)i * 0xC0, 0xC0);
                    std::string sn;
                    rtti(*(const uint64_t*)(b + 0x60), &sn);
                    logf("   its shape: %s", sn.c_str());
                    if (!shown) g_levelMat = *(const uint64_t*)(b + 0x78), g_levelTail = *(const uint64_t*)(b + 0x98);
                    shown++;
                }
            }
            logf("level ground filter: %08x, body tail %016llx", g_levelFilter, (unsigned long long)g_levelTail);
            {   // what +0x98 looks like across the level's static bodies
                std::vector<std::pair<uint64_t, int>> th;
                for (uint32_t i = 0; i < cap; i++) {
                    const uint8_t* b = &all[(size_t)i * 0xC0];
                    if (!*(const uint64_t*)(b + 0x60) || *(const uint32_t*)(b + 0x40) != 0) continue;
                    uint64_t v = *(const uint64_t*)(b + 0x98) & 0xFFFFFFFFFFFF0000ull;
                    bool f = false;
                    for (auto& e : th)
                        if (e.first == v) e.second++, f = true;
                    if (!f && th.size() < 32) th.push_back({v, 1});
                }
                for (auto& e : th) logf("   static body +98 (high bits) %016llx: %d bodies", (unsigned long long)e.first, e.second);
            }
            // (filter, upper flag bits) pairs among the level's static bodies
            std::vector<BodyVariant> pairs;
            for (uint32_t i = 0; i < cap; i++) {
                const uint8_t* b = &all[(size_t)i * 0xC0];
                if (!*(const uint64_t*)(b + 0x60) || *(const uint32_t*)(b + 0x40) != 0) continue;
                uint32_t fl = *(const uint32_t*)(b + 0x6C), hi = *(const uint32_t*)(b + 0x68) & 0xFFFF0000u;
                if (fl == 0x0000800fu) continue;
                bool f = false;
                for (auto& e : pairs)
                    if (e.filter == fl && e.flagsHi == hi) e.count++, f = true;
                if (!f) pairs.push_back({fl, hi, 1});
            }
            std::sort(pairs.begin(), pairs.end(), [](const BodyVariant& a, const BodyVariant& c) { return a.count > c.count; });
            for (size_t k = 0; k < pairs.size() && k < 12; k++)
                logf("   static body type: filter %08x flags %08x: %d bodies", pairs[k].filter, pairs[k].flagsHi, pairs[k].count);
            // Fixed choices (the level's own pick varies between sessions):
            //  0: filter fff8263f (camera was fine with it) + the level's flag bits (guns hit)
            //  1: filter fff8243f + flag bits (0.5 build: guns hit, camera bumped into things)
            //  2: filter fff8263f, no flag bits (0.4 build: camera fine, some guns passed through)
            //  0: floor tiles use the level's ground filter fff8243f (grenades and other throwables land and
            //     go off on it), walls and enemy boxes keep fff8263f (the camera passes them)
            //  0 also copies body+0x98 from a level ground body (the one field still different from the
            //  level's own bodies; grenade arcs ignored our floor without it)
            // Collision filter = [13-bit group id][7 bits][layer 4][mask 12]. Our pieces had group 0x1fff
            // ("special"), which thrown weapons' queries can switch off while they fly (that's why
            // grenades fell through). Give them the group id of a real level ground piece instead.
            uint32_t idHi = 0;
            int idN = 0;
            for (auto& e : pairs)
                if ((e.filter & 0x7FFFFu) == 0x0243fu && (e.filter >> 19) != 0x1fffu && (e.filter >> 19) != 0 && e.count > idN)
                    idN = e.count, idHi = e.filter & 0xFFF80000u;
            if (!idHi)
                for (auto& e : pairs)
                    if (((e.filter >> 12) & 0xF) == 2 && (e.filter >> 19) != 0x1fffu && (e.filter >> 19) != 0 && e.count > idN)
                        idN = e.count, idHi = e.filter & 0xFFF80000u;
            logf("guns: level ground group id for our pieces: %03x (%d level bodies use it)", idHi >> 19, idN);
            // body+0x98 bits 16-31 = "object group": two bodies of the same group never touch (Rift Apart's
            // pair filter, vtable slot 3). Ours had 0 there (= group 0) where level bodies have ffff (= none),
            // so a thrown object in group 0 skipped our ground. Mark ours "no group" like the level's.
            uint32_t fW = idHi ? idHi | 0x263fu : 0xfff8263fu, fG = idHi ? idHi | 0x243fu : 0xfff8243fu;
            g_variants.push_back({fW, 0x002b0000u, 0, fG, 0});  // no owner + hit-object fix (below)
            g_variants.push_back({fW, 0x002b0000u, 0, fG, 2});  // 0.9b: borrowed level owner
            g_variants.push_back({fW, 0x002b0000u, 0, fG, 0x00000000ffff0000ull});
            g_variants.push_back({0xfff8263fu, 0x002b0000u, 0, 0xfff8243fu, 0});
            g_variants.push_back({0xfff8263fu, 0x002b0000u, 0, 0, 0});
            g_variants.push_back({0xfff8243fu, 0x002b0000u, 0, 0, 0});
            g_variants.push_back({0xfff8263fu, 0, 0, 0, 0});
            char vp[MAX_PATH];
            snprintf(vp, sizeof(vp), "%s\\bodyvariant10.txt", g_dir);
            if (FILE* vf = fopen(vp, "r")) {
                int v = 0;
                if (fscanf(vf, "%d", &v) == 1 && v >= 0 && v < (int)g_variants.size()) g_variant = v;
                fclose(vf);
            }
            logf("guns: our bodies use variant %d of %zu: filter %08x flags %08x +98 %016llx (Ctrl+Pause cycles)", g_variant,
                 g_variants.size(), g_variants[g_variant].filter, g_variants[g_variant].flagsHi,
                 (unsigned long long)g_variants[g_variant].tail);
        }
    }
    log_mgr("at start");
    rt_open();
    if (!g_haveTemplate || !g_world) logf("no world/template found; F7 will do nothing");
    else logf("READY: Elden Ring ground feed active. Pause = Rift Apart level collision off/on, Ctrl+Pause = next gun-collision variant, Delete / Shift+Delete = hide / show level, Home = mouse camera 35%/100%/off");

    
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(self);
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char* s = strrchr(exe, '\\');
    if (!s || _stricmp(s + 1, "RiftApart.exe") != 0) return TRUE;
    *s = 0;
    snprintf(g_dir, sizeof(g_dir), "%s\\riftbridge", exe);
    CreateDirectoryA(g_dir, nullptr);
    char dis[MAX_PATH];
    snprintf(dis, sizeof(dis), "%s\\off.txt", g_dir);
    if (GetFileAttributesA(dis) != INVALID_FILE_ATTRIBUTES) return TRUE;
    InitializeCriticalSection(&g_lock);
    CreateThread(nullptr, 0, main_thread, nullptr, 0, nullptr);
    return TRUE;
}
