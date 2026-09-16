/**
 * @file
 * @author ja2mod
 *
 * The inputs the observation builder is allowed to see. Added by the ja2mod
 * fork on 2026-09-15; see CHANGES-ja2mod.md.
 *
 * This header is the fairness boundary of the neural AI. Everything in it is
 * plain data about the deciding soldier, his own team, and what he *believes*
 * about his opponents: where the engine's opponent list says they were last
 * seen or heard, and what the belief store recorded the last time he saw
 * them. No pointer to another soldier ever crosses this boundary, and the
 * translation units that consume it (BuildObservation.cpp, Candidates.cpp)
 * include nothing from the engine, so they cannot read an opponent's
 * SOLDIERTYPE even by accident. NeuralHooks.cpp fills these structures and
 * is the one place where the rule "only what the soldier could know" has to
 * be checked by reading the code.
 *
 * Terrain is shared knowledge, so the builder may ask questions about tiles
 * through the TerrainQuery interface. Those questions take grid numbers, not
 * soldiers, and their answers do not depend on who stands where.
 */

#ifndef TACNN_FAIR_INPUTS_H
#define TACNN_FAIR_INPUTS_H

#include <stdint.h>
#include "tacnn_schema.h"

namespace tacnn
{
    enum
    {
        MAX_TEAM = 32,
        MAX_ENEMIES = 64,
        MAX_NOISES = 8,
        MAX_WATCHED = 3,
        NO_TILE = -1
    };

    /// Stance codes shared by every structure below.
    enum Stance : uint8_t { STANCE_UNKNOWN = 0, STANCE_STAND = 1, STANCE_CROUCH = 2, STANCE_PRONE = 3 };

    /// Coarse weapon class, the same partition the legacy AIGunClass uses.
    enum GunClass : uint8_t { GUN_NONE = 0, GUN_PISTOL = 1, GUN_RIFLE = 2, GUN_HEAVY = 3 };

    /// Alert status, the engine's STATUS_GREEN .. STATUS_BLACK.
    enum Alert : uint8_t { ALERT_GREEN = 0, ALERT_YELLOW = 1, ALERT_RED = 2, ALERT_BLACK = 3 };

    /// Opponent-list knowledge values, the engine's HEARD_3_TURNS_AGO .. SEEN_3_TURNS_AGO.
    enum Knowledge : int8_t
    {
        KNOW_HEARD_3_TURNS_AGO = -4,
        KNOW_HEARD_2_TURNS_AGO = -3,
        KNOW_HEARD_LAST_TURN = -2,
        KNOW_HEARD_THIS_TURN = -1,
        KNOW_NOTHING = 0,
        KNOW_SEEN_CURRENTLY = 1,
        KNOW_SEEN_THIS_TURN = 2,
        KNOW_SEEN_LAST_TURN = 3,
        KNOW_SEEN_2_TURNS_AGO = 4,
        KNOW_SEEN_3_TURNS_AGO = 5
    };

    /// The deciding soldier, as he knows himself.
    struct SelfState
    {
        uint8_t id;
        uint8_t team;
        int32_t gridno;
        int8_t level;
        uint8_t facing;             ///< 0 north .. 7 north-west, clockwise
        uint8_t stance;             ///< Stance
        int16_t ap;
        int16_t apStart;            ///< action points at the start of the turn
        int16_t life;
        int16_t lifeMax;
        int16_t breath;
        int16_t bleeding;
        int8_t morale;              ///< 0 hopeless .. 4 fearless
        int8_t shock;
        uint8_t collapsed;
        uint8_t breathCollapsed;
        uint8_t gunClass;           ///< GunClass
        uint8_t gunRangeTiles;
        uint8_t roundsInGun;
        uint8_t magazineSize;
        uint8_t magazines;
        uint8_t scoped;
        uint8_t burstCapable;
        uint8_t autofireCapable;
        uint8_t maxAimClicks;
        int16_t apReady;
        int16_t apShoot;            ///< one un-aimed shot
        int16_t apBurst;
        int16_t apAuto;
        int16_t apReload;
        int16_t apThrow;
        int16_t apStand;            ///< to change into each stance from the current one
        int16_t apCrouch;
        int16_t apProne;
        int16_t apClimb;
        int16_t apMinMove;          ///< cheapest single step
        int16_t apSeekReserve;      ///< points the legacy walk-towards helper keeps back (MAX_AP_CARRIED); a seek or flank needs apMinMove plus this
        uint8_t grenades;
        uint8_t smokeGrenades;
        uint8_t medkits;
        uint8_t nightVision;
        uint8_t gasMask;
        uint8_t detonator;
        uint8_t knife;
        uint8_t throwingKnife;
        uint8_t launcher;
        uint8_t alert;              ///< Alert
        uint8_t orders;             ///< engine bOrders 0..7
        uint8_t attitude;           ///< engine bAttitude 0..5
        uint8_t experience;         ///< 1..10
        uint8_t marksmanship;       ///< 0..100
        uint8_t underFire;
        uint8_t canClimb;
        uint8_t inWater;
        uint8_t inGas;
        uint8_t inSmoke;
        uint8_t inLight;
        uint8_t turnBased;
        uint8_t stage;              ///< how many decisions this soldier already made this turn
        uint8_t role;               ///< TACNN_ROLE_*
        uint16_t turn;              ///< battle turn counter
        uint8_t teamAlive;
        uint8_t teamTotal;
        int32_t lastTargetGridno;   ///< where he last fired, NO_TILE if never
        int32_t blackListGridno;    ///< a tile the pathing code told him to avoid
        int32_t objectiveGridno;    ///< where his orders send him, NO_TILE if nowhere
        int8_t objectiveLevel;
    };

    /// A living member of the deciding soldier's own team.
    struct Teammate
    {
        uint8_t id;
        int32_t gridno;
        int8_t level;
        uint8_t stance;
        uint8_t lifeFrac;           ///< 0..255
        uint8_t gunClass;
        uint8_t alert;
        uint8_t underFire;
        uint8_t wounded;            ///< bleeding or below full life
        int32_t lastTargetGridno;   ///< NO_TILE if he has not fired
    };

    /**
     * An opponent as the deciding soldier believes him to be.
     *
     * The position and the knowledge codes come from the engine's opponent list
     * (personal and public), which is the same information the legacy AI uses.
     * The "seen*" fields come from the belief store and describe the opponent
     * at the moment he was last actually seen by this observer. cthSnap,
     * cthBest and losNow are only filled for an opponent who is seen right now;
     * for anyone else they are zero, because estimating a chance to hit against
     * a tile would let the engine peek at who really stands there.
     */
    struct BelievedEnemy
    {
        uint8_t id;
        int8_t knowledge;           ///< Knowledge, personal
        int8_t publicKnowledge;     ///< Knowledge, team
        int32_t gridno;             ///< personal last known tile, NO_TILE
        int8_t level;
        int32_t publicGridno;       ///< team last known tile, NO_TILE
        int8_t publicLevel;
        uint8_t turnsSinceFresh;    ///< 0..3 from the freshest of the two knowledge codes
        uint8_t seenStance;         ///< Stance at last sighting
        uint8_t seenGunClass;       ///< GunClass at last sighting
        uint8_t seenGunRange;       ///< tiles, at last sighting
        uint8_t seenLifeBand;       ///< 0 unknown, 1 dying .. 4 unhurt, at last sighting
        uint8_t attacksOnMe;        ///< times he attacked this observer, saturating
        uint8_t damageToMe;         ///< life points he took from this observer, saturating
        uint8_t damageByMe;         ///< life points this observer took from him, saturating
        uint8_t losNow;             ///< the observer sees him right now
        uint8_t watched;            ///< his tile is one of the observer's watched locations
        uint8_t turnsSinceLos;      ///< 0..3, turns since the observer last saw him
        int16_t cthSnap;            ///< chance to hit him un-aimed, percent, seen only
        int16_t cthBest;            ///< chance to hit him at full aim, percent, seen only
        int16_t apShot;             ///< action points of an un-aimed shot at him, seen only
    };

    /// A noise the soldier or his team remembers.
    struct NoiseMemory
    {
        int32_t gridno;
        int8_t level;
        uint8_t volume;             ///< engine noise volume, 0..15
        uint8_t type;               ///< engine NOISE_*
        uint8_t turnsAgo;
        uint8_t isPublic;
    };

    /**
     * Terrain questions the builder may ask. Implemented in NeuralHooks.cpp
     * over the engine's map data; every method takes tiles, not soldiers, and
     * must answer the same whether or not anyone stands on the tile.
     */
    class TerrainQuery
    {
        public:
            virtual ~TerrainQuery() { }
            virtual int cols() const = 0;
            virtual int rows() const = 0;
            /// Channel 0 encoding: 0 blocked, else 255 - 8 * min(cost, 25).
            virtual uint8_t passable(int32_t gridno) const = 0;
            /// Height in levels of the tallest structure on the tile, 0 open.
            virtual uint8_t structureHeight(int32_t gridno, int8_t level) const = 0;
            /// The map's ground height byte.
            virtual uint8_t groundHeight(int32_t gridno) const = 0;
            virtual bool roof(int32_t gridno) const = 0;
            /// 0 dry, 1 shallow water, 2 deep water.
            virtual uint8_t water(int32_t gridno) const = 0;
            /// Bit 0 smoke, bit 1 tear or mustard gas, bit 2 any other gas.
            virtual uint8_t gasSmoke(int32_t gridno, int8_t level) const = 0;
            virtual bool corpse(int32_t gridno) const = 0;
            virtual bool lit(int32_t gridno, int8_t level) const = 0;
            virtual bool bombNear(int32_t gridno) const = 0;
            /// Line of sight from the deciding soldier, as he stands now, to the tile.
            virtual bool selfSees(int32_t gridno, int8_t level) const = 0;
            /// Would a standing figure at `from` see a figure of `toStance` at `to`.
            virtual bool standingAtSees(int32_t from, int8_t fromLevel, int32_t to, int8_t toLevel, uint8_t toStance) const = 0;
            /// Action points to reach the tile this turn, -1 if not reachable.
            virtual int16_t apToReach(int32_t gridno, int8_t level) const = 0;
            /// A friendly or seen soldier stands there (unseen opponents do not count).
            virtual bool knownOccupied(int32_t gridno, int8_t level) const = 0;
    };

    /// Everything the builder gets.
    struct FairInputs
    {
        SelfState self;
        Teammate team[MAX_TEAM];
        int teamCount;
        BelievedEnemy enemies[MAX_ENEMIES];
        int enemyCount;
        NoiseMemory noises[MAX_NOISES];
        int noiseCount;
        int32_t watched[MAX_WATCHED];
        int8_t watchedLevel[MAX_WATCHED];
        int watchedCount;
        const TerrainQuery* terrain;
    };

    /// Zero every field and mark every tile slot as NO_TILE; call before filling.
    inline void ClearFairInputs(FairInputs& in)
    {
        unsigned char* bytes = reinterpret_cast<unsigned char*>(&in);
        for(unsigned i = 0; i < sizeof(FairInputs); ++i)
            bytes[i] = 0;
        in.self.gridno = NO_TILE;
        in.self.lastTargetGridno = NO_TILE;
        in.self.blackListGridno = NO_TILE;
        in.self.objectiveGridno = NO_TILE;
        for(int i = 0; i < MAX_WATCHED; ++i)
            in.watched[i] = NO_TILE;
    }
}

#endif
