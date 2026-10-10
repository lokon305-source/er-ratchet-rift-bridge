// Terrain feed from riftlink (outside both games) to riftbridge.dll (inside Rift Apart).
// One file-backed mapping: <Rift Apart>\riftbridge\terrain.shm. Data only: ground/wall tiles and
// Elden Ring enemy hitboxes in Rift Apart world coordinates. riftlink writes (seqlocks), riftbridge reads.
#pragma once
#include <stdint.h>

#define RT_MAGIC 0x52455452u  /* "RTER" */
#define RTER_VERSION 4u
#define RT_MAX_TILES 2048u
#define RT_MAX_ENEMIES 64u

#pragma pack(push, 4)
typedef struct RtTile {
    float pos[3];      // top-surface point (where the Elden Ring ray hit), Rift Apart world space
    float normal[3];   // surface normal, Rift Apart world space
    float half;        // tile half-width (metres)
    uint32_t flags;    // v2: cell key
} RtTile;              // 32 bytes

typedef struct RtEnemy {
    float center[3];   // hitbox centre, Rift Apart world space
    float half[3];     // hitbox half size: x side, y up, z forward (Rift Apart metres)
    float yaw;         // radians: forward = (sin yaw, 0, cos yaw)
    uint32_t id;       // low bits of the Elden Ring entity id (stable while it lives)
} RtEnemy;             // 32 bytes

typedef struct RtHeader {
    uint32_t magic;
    uint32_t version;
    volatile uint32_t seq;   // odd while riftlink writes the tiles
    uint32_t count;          // tiles in use
    uint32_t generation;     // +1 every time the tile list changes
    uint32_t writerPid;
    uint32_t reserved[2];    // [0] anchor id, [1] heartbeat
    RtTile tiles[RT_MAX_TILES];
    volatile uint32_t enemySeq;  // odd while riftlink writes the enemies
    uint32_t enemyCount;
    RtEnemy enemies[RT_MAX_ENEMIES];
    volatile int32_t camLow;     // v4: 1 = the camera is down at the ground: don't let the mouse tilt it further up
    volatile uint32_t runesTag;  // RT_RUNES_TAG while erRunes is valid
    volatile int32_t erRunes;    // the Tarnished's runes held (shown as Ratchet's bolts)
    volatile uint32_t runesBeat; // +1 on every runes update
    volatile float heroDamage;   // total damage for Ratchet so far, as a fraction of his max health (monotonic)
    volatile uint32_t heroHits;  // hits so far (monotonic)
    volatile float heroMinHp;    // never take him below this (0 = hits can kill)
    volatile uint32_t damageTag; // RT_RUNES_TAG while the damage feed is on
    // controller as riftlink sees it (XInput pad 0..3, first connected), for Rift Apart while
    // Elden Ring has focus. Layout of XINPUT_GAMEPAD.
    volatile uint32_t padSeq;    // +1 on every write
    volatile uint32_t padOn;     // RT_RUNES_TAG while a pad is connected
    volatile uint32_t padPacket;
    volatile uint16_t padButtons;
    volatile uint8_t padLT, padRT;
    volatile int16_t padLX, padLY, padRX, padRY;
    // Impacts of Ratchet's explosive shots on Elden Ring enemies (riftbridge writes, riftlink applies
    // the damage). hits[] is a ring; hitWrite counts entries ever written.
    volatile uint32_t hitTag;    // RT_RUNES_TAG once riftbridge writes hits
    volatile uint32_t hitWrite;
    struct RtHit {
        uint32_t id;     // RtEnemy.id
        float factor;    // 0..1: 1 = direct hit, less towards the edge of the blast
        uint32_t kind;   // which shot type (index in riftbridge's list, for the log)
        float pos[3];    // where it went off, Rift Apart world space
    } hits[64];
    volatile uint32_t shotSpawns; // +1 whenever riftbridge sees Ratchet fire a shot it can follow to its impact
} RtHeader;
#define RT_HIT_DIRECT 0x80000000u   /* RtHit.kind flag: a direct hit (bullet, blade, bolt), not a blast */
#define RT_RUNES_TAG 0x454E5552u  /* "RUNE" */
#pragma pack(pop)

#define RT_FILE_SIZE ((uint32_t)sizeof(RtHeader))
