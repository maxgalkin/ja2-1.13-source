/**
 * @file
 * @author ja2mod
 *
 * Added by the ja2mod fork on 2026-09-15; see CHANGES-ja2mod.md.
 *
 * This file is the fairness seam of the neural AI: it is the only place that
 * reads the engine's soldiers *and* writes the FairInputs the observation is
 * built from. The rule for every line in FillFairInputs is "only what this
 * soldier could know": his own state, his team, terrain, and what the
 * engine's opponent list and the belief store say about opponents. The one
 * engine call that looks at who stands on a tile (WhoIsThere2) is wrapped in
 * knownOccupied(), which only admits teammates and currently seen opponents.
 */

#include "../include/NeuralHooks.h"
#include "../include/FairInputs.h"
#include "../include/BeliefStore.h"
#include "../include/BuildObservation.h"
#include "../include/DecisionLog.h"
#include "../include/SidecarClient.h"
#include "../include/TacnnInfer.h"
#include "../include/AIRandom.h"
#include "../include/SituationExport.h"
#include "../include/NeuralPlan.h"
#include "../include/Harness.h"

#include "../../Tactical/Soldier Control.h"
#include "../../Tactical/Soldier macros.h"
#include "../../Tactical/Overhead.h"
#include "../../Tactical/Overhead Types.h"
#include "../../Tactical/opplist.h"
#include "../../Tactical/Points.h"
#include "../../Tactical/Items.h"
#include "../../Tactical/Item Types.h"
#include "../../Tactical/Weapons.h"
#include "../../Tactical/LOS.h"
#include "../../Tactical/Animation Control.h"
#include "../../Tactical/PATHAI.H"
#include "../../Tactical/Rotting Corpses.h"
#include "../../TacticalAI/ai.h"
#include "../../TacticalAI/AIInternals.h"
#include "../../TileEngine/worlddef.h"
#include "../../TileEngine/worldman.h"
#include "../../TileEngine/Isometric Utils.h"
#include "../../TileEngine/lighting.h"
#include "../../TileEngine/environment.h"
#include "../../TileEngine/structure.h"
#include "../../Strategic/strategicmap.h"
#include "../../Strategic/Game Clock.h"
#include "../../Ja2/GameSettings.h"
#include "../../Ja2/GameVersion.h"
#include "../../sgp/random.h"
#include "../../sgp/FileMan.h"
#include "sgp_logger.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>
#include <string>
#include <vector>
#include <algorithm>
#include <functional>

/// Tactical/LOS.cpp defines the ray march every sight test wraps but does not declare it in a header.
INT32 LineOfSightTest(FLOAT dStartX, FLOAT dStartY, FLOAT dStartZ, FLOAT dEndX, FLOAT dEndY, FLOAT dEndZ,
                      int iTileSightLimit, INT8 bAware, BOOLEAN fSmell, INT32* psWindowGridNo, bool adjustForSight, bool cthCalc);

// TileEngine/tiledef.h (reached through worlddef.h above) defines NO_TILE as the tile engine's 64000. Every
// NO_TILE in this file means tacnn::NO_TILE (-1), the "no tile" of the fair inputs that BuildObservation.cpp,
// Candidates.cpp and the Python builder share; with the macro in force the enemy rows of the observation
// carried a believed tile of 64000, an entity 400 rows south of the map (found by tools/tacrl/replay.py).
#ifdef NO_TILE
#undef NO_TILE
#endif

namespace tacnn
{
    namespace
    {
        // ------------------------------------------------------------------ settings

        struct Settings
        {
            bool applied;
            bool log;
            bool logPlayer;             ///< NEURAL_LOG_PLAYER: the AI's decisions for the player's soldiers go in the log too
            std::string logDir;
            uint64_t logCapBytes;
            bool sidecar;
            unsigned sidecarTimeoutMs;
            uint32_t seed;
            bool exportSituations;
            std::string exportDir;
            bool cheatAudit;
            int mode;                   ///< TACNN_MODE_*: who answers a tagged soldier's decision
            std::string modelPath;      ///< the VFS path of the .onnx, "AI\\<NEURAL_MODEL>"
        };
        Settings g_settings = { false, false, false, std::string(), 0, false, 50, 0, false, std::string(), false,
                                TACNN_MODE_OFF, std::string() };

        // ------------------------------------------------------------------ the embedded policy

        PolicyModel g_model;
        HiddenStore g_hidden;
        CommsRing g_ring(160);
        std::string g_modelLoadedFrom;      ///< the path the loaded model came from
        std::string g_modelFailedFor;       ///< the path whose load failed and was logged; not retried

        bool EqualsNoCase(const char* a, const char* b)
        {
            for(; *a && *b; ++a, ++b)
                if(tolower((unsigned char)*a) != tolower((unsigned char)*b))
                    return false;
            return *a == *b;
        }

        int ParseMode(const char* text, bool sidecarKey)
        {
            if(!text || !*text)
                return sidecarKey ? TACNN_MODE_SIDECAR : TACNN_MODE_OFF;
            if(EqualsNoCase(text, "embedded")) return TACNN_MODE_EMBEDDED;
            if(EqualsNoCase(text, "sidecar")) return TACNN_MODE_SIDECAR;
            if(EqualsNoCase(text, "off")) return TACNN_MODE_OFF;
            return -1;
        }

        const char* ModeName(int mode)
        {
            return mode == TACNN_MODE_EMBEDDED ? "embedded" : mode == TACNN_MODE_SIDECAR ? "sidecar" : "off";
        }

        /**
         * Load NEURAL_MODEL through the VFS, once per path. A file that is
         * missing, not a model or exported against another schema is logged
         * once and the mode drops to off, so the tagged soldiers decide
         * through the legacy tree; the same path is not tried again until
         * the setting changes.
         */
        void EnsureModelLoaded()
        {
            if(g_settings.mode != TACNN_MODE_EMBEDDED)
                return;
            if(g_model.loaded() && g_modelLoadedFrom == g_settings.modelPath)
                return;
            if(g_modelFailedFor == g_settings.modelPath)
            {
                g_settings.mode = TACNN_MODE_OFF;
                return;
            }
            g_model.unload();
            g_modelLoadedFrom.clear();
            std::string error;
            std::vector<uint8_t> bytes;
            HWFILE hFile = FileOpen(const_cast<STR>(g_settings.modelPath.c_str()), FILE_ACCESS_READ, FALSE);
            if(!hFile)
                error = "file not found in the data folders";
            else
            {
                const UINT32 size = FileGetSize(hFile);
                UINT32 read = 0;
                if(size == 0 || size > 64u * 1024u * 1024u)
                    error = "file is empty or over 64 MB";
                else
                {
                    bytes.resize(size);
                    if(!FileRead(hFile, &bytes[0], size, &read) || read != size)
                        error = "read failed";
                }
                FileClose(hFile);
            }
            if(error.empty())
            {
                const int64_t started = NowUs();
                if(g_model.load(&bytes[0], bytes.size(), TACNN_SCHEMA_HASH))
                {
                    const ModelInfo& info = g_model.info();
                    g_modelLoadedFrom = g_settings.modelPath;
                    char hash[16];
                    snprintf(hash, sizeof hash, "0x%08X", info.schemaHash);
                    SGP_INFO() << "tacnn: embedded model " << g_settings.modelPath << " loaded: generation "
                               << info.generation << ", schema " << hash << ", " << info.paramCount << " weights, "
                               << info.trainedSteps << " training steps, " << (unsigned)(bytes.size() / 1024) << " KB in "
                               << (int)((NowUs() - started) / 1000) << " ms" << sgp::endl;
                    return;
                }
                error = g_model.error();
            }
            g_modelFailedFor = g_settings.modelPath;
            g_settings.mode = TACNN_MODE_OFF;
            SGP_INFO() << "tacnn: embedded model " << g_settings.modelPath << " unavailable (" << error
                       << "); tagged soldiers decide through the legacy AI" << sgp::endl;
        }

        // ------------------------------------------------------------------ battle bookkeeping

        bool g_inBattle = false;
        uint32_t g_battleLo = 0;
        uint32_t g_battleHi = 0;
        uint32_t g_decisionIndex = 0;
        uint16_t g_decisionsInBattle = 0;
        int32_t g_damageDealt = 0;     // by the enemy team
        int32_t g_damageTaken = 0;     // by the enemy team
        // running totals since the process started; the battle harness reads them as differences
        uint32_t g_decisionsEver = 0;
        int32_t g_damageDealtEver = 0;
        int32_t g_damageTakenEver = 0;
        uint8_t g_stage[TOTAL_SOLDIERS];

        /// The decision being made right now: built before the plan runs, written after.
        struct Pending
        {
            bool active;
            uint8_t soldier;
            bool exportPending;             ///< a situation snapshot waits for this decision's answer
            int64_t startedUs;
            TacnnDecision head;
            TacnnObservation obs;
            TacnnActionMask mask;
            TacnnCandidateSet cand;
            TacnnPolicyAction action;
            bool hasEmbedded;               ///< the in-process network ran for this decision
            TacnnEmbedded embedded;         ///< what it was fed and what it answered
        };
        Pending g_pending;
        BuildScratch g_scratch;
        FairInputs g_inputs;

        /**
         * Where a decision's time goes, summed over the battle and written to
         * the log when it ends. The first six buckets are the phases of a
         * decision and add up to its elapsed time; the rest are the expensive
         * terrain questions inside those phases (line-of-sight rays and the
         * reach table), with how many the engine actually computed against
         * how many the builders asked, the difference being cache hits.
         */
        struct Profile
        {
            enum { FAIR, OBS, CAND, MASK, INFER, APPLY, PHASES, LOS_SELF = PHASES, LOS_COVER, REACH, COUNT };
            int64_t sum[COUNT];
            int64_t max[COUNT];
            int64_t calls[COUNT];       ///< engine computations in the bucket
            int64_t asked[COUNT];       ///< questions the builders asked (computations plus cache hits)
            uint32_t n;
            void clear() { memset(this, 0, sizeof *this); }
            void add(int bucket, int64_t us)
            {
                sum[bucket] += us;
                ++calls[bucket];
                if(us > max[bucket])
                    max[bucket] = us;
            }
        };
        Profile g_profile;
        const char* const PROFILE_NAMES[Profile::COUNT] = { "fair", "obs", "cand", "mask", "infer", "apply", "los_self", "los_cover", "reach" };

        bool TurnBasedCombat()
        {
            return (gTacticalStatus.uiFlags & TURNBASED) && (gTacticalStatus.uiFlags & INCOMBAT);
        }

        bool Instrumented(const SOLDIERTYPE* p)
        {
            return p && p->bActive && p->bInSector && p->bTeam == ENEMY_TEAM
                && !(p->flags.uiStatusFlags & SOLDIER_VEHICLE);
        }

        /**
         * Whose legacy decisions the log records: every instrumented enemy, and
         * with NEURAL_LOG_PLAYER the player's own soldiers while the AI decides
         * for them (a merc left under AI control, the squad under the battle
         * harness). The situation export and the cheat audit stay enemy-only.
         */
        bool Logged(const SOLDIERTYPE* p)
        {
            if(Instrumented(p))
                return true;
            return g_settings.logPlayer && p && p->bActive && p->bInSector && p->bTeam == OUR_TEAM
                && !(p->flags.uiStatusFlags & SOLDIER_VEHICLE);
        }

        uint32_t GameRandomFallback(uint32_t range)
        {
            if(gGameExternalOptions.fNewRandom)
                return NewRandom(range);
            return GetRndNum(range);
        }

        void EnsureApplied()
        {
            if(!g_settings.applied)
                ApplySettings();
        }

        // ------------------------------------------------------------------ small translations

        uint8_t StanceOf(const SOLDIERTYPE* p)
        {
            switch(gAnimControl[p->usAnimState].ubEndHeight)
            {
                case ANIM_STAND:  return STANCE_STAND;
                case ANIM_CROUCH: return STANCE_CROUCH;
                case ANIM_PRONE:  return STANCE_PRONE;
            }
            return STANCE_UNKNOWN;
        }

        uint8_t GunClassOf(UINT16 usItem)
        {
            if(usItem == NOTHING || !(Item[usItem].usItemClass & IC_GUN))
                return GUN_NONE;
            switch(Weapon[usItem].ubWeaponClass)
            {
                case HANDGUNCLASS:
                case SMGCLASS:     return GUN_PISTOL;
                case RIFLECLASS:
                case SHOTGUNCLASS: return GUN_RIFLE;
                case MGCLASS:      return GUN_HEAVY;
            }
            return GUN_NONE;
        }

        uint8_t GunRangeTiles(SOLDIERTYPE* p)
        {
            OBJECTTYPE& gun = p->inv[HANDPOS];
            if(!gun.exists() || !(Item[gun.usItem].usItemClass & IC_GUN))
                return 0;
            int tiles = GunRange(&gun, p) / CELL_X_SIZE;
            return static_cast<uint8_t>(tiles < 0 ? 0 : tiles > 255 ? 255 : tiles);
        }

        uint8_t LifeBand(int life, int lifeMax)
        {
            if(lifeMax <= 0 || life <= 0)
                return 1;
            const int pct = life * 100 / lifeMax;
            if(pct < 20) return 1;
            if(pct < 50) return 2;
            if(pct < 85) return 3;
            return 4;
        }

        uint8_t Clamp255(int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }
        int16_t ClampAP(int v) { return static_cast<int16_t>(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

        /// A gridno the observation may use, or NO_TILE. The engine's knowledge tables hold NOWHERE (-1) for
        /// "nothing", but a loaded save or a stale entry can also carry the tile engine's NO_TILE (64000) or any
        /// other value outside the sector; the Python builder only ever sees real tiles, so those become NO_TILE
        /// here rather than an entity standing 400 rows south of the map (found by tools/tacrl/replay.py).
        int32_t TileOrNone(INT32 g)
        {
            return (g >= 0 && g < static_cast<INT32>(WORLD_MAX)) ? g : NO_TILE;
        }

        /// 0..3 turns since a knowledge code was fresh; 3 for nothing.
        uint8_t TurnsSinceCode(INT8 code)
        {
            if(code >= SEEN_CURRENTLY)
            {
                const int t = code - SEEN_THIS_TURN;
                return static_cast<uint8_t>(t < 0 ? 0 : t > 3 ? 3 : t);
            }
            if(code <= HEARD_THIS_TURN)
            {
                const int t = HEARD_THIS_TURN - code;
                return static_cast<uint8_t>(t > 3 ? 3 : t);
            }
            return 3;
        }

        int CountClass(const SOLDIERTYPE* p, UINT32 itemClass)
        {
            int count = 0;
            for(int slot = 0; slot < NUM_INV_SLOTS; ++slot)
            {
                const OBJECTTYPE& obj = const_cast<SOLDIERTYPE*>(p)->inv[slot];
                if(obj.exists() && (Item[obj.usItem].usItemClass & itemClass))
                    count += obj.ubNumberOfObjects;
            }
            return count;
        }

        int CountMagazines(const SOLDIERTYPE* p, UINT16 usGun)
        {
            if(usGun == NOTHING || !(Item[usGun].usItemClass & IC_GUN))
                return 0;
            int count = 0;
            for(int slot = 0; slot < NUM_INV_SLOTS; ++slot)
            {
                const OBJECTTYPE& obj = const_cast<SOLDIERTYPE*>(p)->inv[slot];
                if(!obj.exists() || !(Item[obj.usItem].usItemClass & IC_AMMO))
                    continue;
                if(Magazine[Item[obj.usItem].ubClassIndex].ubCalibre == Weapon[usGun].ubCalibre)
                    count += obj.ubNumberOfObjects;
            }
            return count;
        }

        int CountSmokeGrenades(const SOLDIERTYPE* p)
        {
            int count = 0;
            for(int slot = 0; slot < NUM_INV_SLOTS; ++slot)
            {
                const OBJECTTYPE& obj = const_cast<SOLDIERTYPE*>(p)->inv[slot];
                if(obj.exists() && (Item[obj.usItem].usItemClass & IC_GRENADE)
                   && Explosive[Item[obj.usItem].ubClassIndex].ubType == EXPLOSV_SMOKE)
                    count += obj.ubNumberOfObjects;
            }
            return count;
        }

        uint8_t RoleOf(const SOLDIERTYPE* p)
        {
            if(p->aiData.bOrders == SNIPER)
                return TACNN_ROLE_SNIPER;
            const OBJECTTYPE& gun = const_cast<SOLDIERTYPE*>(p)->inv[HANDPOS];
            if(gun.exists() && (Item[gun.usItem].usItemClass & IC_GUN) && Weapon[gun.usItem].ubWeaponClass == MGCLASS)
                return TACNN_ROLE_MACHINEGUNNER;
            if(FindObjClass(const_cast<SOLDIERTYPE*>(p), IC_LAUNCHER) != NO_SLOT)
                return TACNN_ROLE_MORTAR_OR_GL;
            if(FindObjClass(const_cast<SOLDIERTYPE*>(p), IC_MEDKIT) != NO_SLOT)
                return TACNN_ROLE_MEDIC;
            if(gun.exists() && (Item[gun.usItem].usItemClass & IC_GUN) && IsScoped(const_cast<OBJECTTYPE*>(&gun)))
                return TACNN_ROLE_MARKSMAN;
            return TACNN_ROLE_RIFLEMAN;
        }

        // ------------------------------------------------------------------ line-of-sight memo

        /**
         * Memo of the engine's line-of-sight answers for the current team
         * turn. A ray's answer depends on the map, its structures, smoke and
         * light, none of which changes while one team acts except through
         * the events that call OnWorldChanged() (doors, structures added or
         * destroyed, explosions, lights); each of those, every team turn,
         * every game minute and every battle start empties the memo.
         *
         * One entry holds the rays of one viewpoint (tile, level and, for a
         * soldier's own eyes, whose eyes) to the tiles of one 64 by 64 block
         * of the map at one target level and height, as two bitmaps:
         * answered, and seen. Maps run up to 2000 tiles a side, so the map
         * is not one bitmap; a soldier's patches touch at most four blocks.
         *
         * The observation builders ask the same rays over and over: the
         * cover channels ask each believed enemy's view of every patch tile
         * for every decision of every soldier of the team, and the fine and
         * medium patches overlap. The memo turns all but the first of those
         * into a bit test, with the same answer the engine would give.
         */
        class LosCache
        {
            public:
                enum { ENTRIES = 256, SLOTS = 4096, BLOCK_SHIFT = 6, BLOCK = 1 << BLOCK_SHIFT, WORDS = BLOCK * BLOCK / 32 };
                enum { CUBE_TARGET = 100 };  ///< height marker of a ray to the first cube of the target tile

                struct Key
                {
                    int32_t from;           ///< the viewpoint tile
                    uint32_t viewer;        ///< 0 for a fixed standing eye; else a digest of the looking soldier's sight
                    uint16_t block;         ///< which 64 by 64 block of the map the targets lie in
                    int8_t fromLevel;
                    int8_t toLevel;
                    int8_t height;          ///< the target height: a STANCE_* or CUBE_TARGET
                    int8_t pad[3];

                    bool operator==(const Key& o) const
                    {
                        return from == o.from && viewer == o.viewer && block == o.block && fromLevel == o.fromLevel
                            && toLevel == o.toLevel && height == o.height;
                    }
                };

                struct Entry
                {
                    Key key;
                    bool used;
                    uint32_t lastUsed;
                    uint32_t answered;      ///< rays stored, the entry's worth when one must go
                    uint32_t known[WORDS];
                    uint32_t seen[WORDS];
                };

                LosCache() { clear(); }

                void clear()
                {
                    for(int i = 0; i < ENTRIES; ++i)
                        entries_[i].used = false;
                    for(int i = 0; i < SLOTS; ++i)
                        slots_[i] = NULL;
                    tick_ = 0;
                }

                /// The block a tile lies in and its bit within the block's bitmaps.
                static void Locate(int32_t gridno, uint16_t& block, int& bit)
                {
                    const int x = gridno % WORLD_COLS;
                    const int y = gridno / WORLD_COLS;
                    block = static_cast<uint16_t>(((y >> BLOCK_SHIFT) << 5) | (x >> BLOCK_SHIFT));   // 2000 tiles = 32 blocks a side
                    bit = ((y & (BLOCK - 1)) << BLOCK_SHIFT) | (x & (BLOCK - 1));
                }

                /**
                 * The entry of this viewpoint and block, made if missing. When all entries are taken the
                 * one with the fewest answered rays goes, oldest first among equals: the candidate
                 * features ask one ray from each of 48 tiles per decision, and those single-ray entries
                 * must not push out the viewpoints holding hundreds of rays of a patch.
                 */
                Entry* entry(const Key& key)
                {
                    // two hint slots before the scan; the scan's result takes the emptier or older of them
                    const uint32_t h = Hash(key);
                    Entry*& first = slots_[h & (SLOTS - 1)];
                    Entry*& second = slots_[(h + 1) & (SLOTS - 1)];
                    if(first && first->used && first->key == key)
                    {
                        first->lastUsed = ++tick_;
                        return first;
                    }
                    if(second && second->used && second->key == key)
                    {
                        second->lastUsed = ++tick_;
                        return second;
                    }
                    Entry* found = NULL;
                    Entry* victim = NULL;
                    for(int i = 0; i < ENTRIES; ++i)
                    {
                        Entry& e = entries_[i];
                        if(e.used && e.key == key)
                        {
                            found = &e;
                            break;
                        }
                        if(!e.used)
                        {
                            if(!victim || victim->used)
                                victim = &e;
                        }
                        else if(!victim || (victim->used && (e.answered < victim->answered
                                                             || (e.answered == victim->answered && e.lastUsed < victim->lastUsed))))
                            victim = &e;
                    }
                    if(!found)
                    {
                        found = victim;
                        found->key = key;
                        found->used = true;
                        found->answered = 0;
                        memset(found->known, 0, sizeof found->known);
                        memset(found->seen, 0, sizeof found->seen);
                    }
                    found->lastUsed = ++tick_;
                    Entry*& hint = (!first || !first->used) ? first
                                 : (!second || !second->used) ? second
                                 : (first->lastUsed <= second->lastUsed) ? first : second;
                    hint = found;
                    return found;
                }

                /// -1 when the ray was never computed, else 0 or 1
                static int Lookup(const Entry& e, int bit)
                {
                    const uint32_t mask = 1u << (bit & 31);
                    if(!(e.known[bit >> 5] & mask))
                        return -1;
                    return (e.seen[bit >> 5] & mask) ? 1 : 0;
                }

                static void Store(Entry& e, int bit, bool seen)
                {
                    const uint32_t mask = 1u << (bit & 31);
                    if(!(e.known[bit >> 5] & mask))
                        ++e.answered;
                    e.known[bit >> 5] |= mask;
                    if(seen)
                        e.seen[bit >> 5] |= mask;
                    else
                        e.seen[bit >> 5] &= ~mask;
                }

            private:
                static uint32_t Hash(const Key& k)
                {
                    uint32_t h = (uint32_t)k.from * 2654435761u;
                    h ^= k.viewer * 40503u;
                    h ^= (uint32_t)k.block * 2246822519u;
                    h ^= (uint32_t)(uint8_t)k.fromLevel << 3;
                    h ^= (uint32_t)(uint8_t)k.toLevel << 9;
                    h ^= (uint32_t)(uint8_t)k.height << 15;
                    return h ^ (h >> 16);
                }

                Entry entries_[ENTRIES];
                Entry* slots_[SLOTS];   ///< hints into entries_, checked before the scan
                uint32_t tick_;
        };
        LosCache g_los;
        UINT32 g_losMinute = 0xFFFFFFFFu;   ///< the game minute the memo was last known good for

        /**
         * The last reach table built, kept for the next decision of the same
         * soldier from the same tile in the same movement mode: a soldier who
         * changed stance, fired or wasted an answer asks again without having
         * moved, and the table (his own movement costs over the window, with
         * teammates and seen opponents as obstacles) has not changed for him.
         * Another soldier's decision replaces it, and it goes with the
         * line-of-sight memo whenever that is emptied.
         */
        struct ReachMemo
        {
            enum { R = TACNN_PATCH_RADIUS + 2, W = 2 * R + 1 };
            bool valid;
            uint8_t soldier;
            int32_t gridno;
            int8_t level;
            uint16_t mode;
            int16_t table[W * W];

            ReachMemo() : valid(false), soldier(0), gridno(0), level(0), mode(0) {}
        };
        ReachMemo g_reach;

        /// Forget every memo of the map: the line-of-sight answers and the reach table.
        void ClearTerrainMemos()
        {
            g_los.clear();
            g_reach.valid = false;
        }

        /// Empty the memos when the game clock moved (light and smoke change with it).
        void FreshenLosCache()
        {
            const UINT32 minute = GetWorldTotalMin();
            if(minute != g_losMinute)
            {
                ClearTerrainMemos();
                g_losMinute = minute;
            }
        }

        // ------------------------------------------------------------------ terrain

        /**
         * Terrain questions over the engine's map. Every answer depends on
         * tiles only, except knownOccupied() and the reach table, which
         * consult positions but only of teammates and currently seen
         * opponents.
         *
         * The two line-of-sight questions give exactly the engine's answers
         * (SoldierTo3DLocationLineOfSightTest with the first cube of the
         * target tile, and LocationToLocationLineOfSightTest between standing
         * eyes and the target height) but compute them cheaper: rays go
         * through the turn-scoped memo above, and the soldier's own sight
         * range, which the engine derives afresh for every tile from his
         * gear, traits and the light there, is derived once per light level.
         */
        class EngineTerrain : public TerrainQuery
        {
            public:
                enum { REACH_R = TACNN_PATCH_RADIUS + 2, REACH_W = 2 * REACH_R + 1 };

                explicit EngineTerrain(SOLDIERTYPE* me)
                    : me_(me), reachBuilt_(false), eyesReady_(false), viewer_(0), startZ_(0.0f), thermal_(false), last_(NULL)
                {
                    for(int i = 0; i < 2; ++i)
                        for(int l = 0; l < LIGHT_LEVELS; ++l)
                            sightLimit_[i][l] = -1;
                }

                virtual int cols() const { return WORLD_COLS; }
                virtual int rows() const { return WORLD_ROWS; }

                virtual uint8_t passable(int32_t gridno) const
                {
                    if(!Valid(gridno))
                        return 0;
                    const int level = me_->pathing.bLevel > 0 ? 1 : 0;
                    int best = 255;
                    for(int dir = 0; dir < MAXDIR; ++dir)
                    {
                        const int cost = gubWorldMovementCosts[gridno][dir][level];
                        if(cost < best)
                            best = cost;
                    }
                    if(best >= TRAVELCOST_BLOCKED && best != TRAVELCOST_DOOR)
                        return 0;
                    if(best >= TRAVELCOST_DOOR_CLOSED_HERE)
                        best = 25;   // a door: passable, but the slowest thing that is
                    else
                        best /= 10;  // TRAVELCOST_FLAT is 10 per tile
                    if(best > 25)
                        best = 25;
                    return static_cast<uint8_t>(255 - 8 * best);
                }

                virtual uint8_t structureHeight(int32_t gridno, int8_t level) const
                {
                    if(!Valid(gridno))
                        return 0;
                    const INT8 h = GetTallestStructureHeight(gridno, level > 0);
                    return static_cast<uint8_t>(h < 0 ? 0 : h);
                }

                virtual uint8_t groundHeight(int32_t gridno) const
                {
                    return Valid(gridno) ? gpWorldLevelData[gridno].sHeight : 0;
                }

                virtual bool roof(int32_t gridno) const
                {
                    return Valid(gridno) && FlatRoofAboveGridNo(gridno);
                }

                virtual uint8_t water(int32_t gridno) const
                {
                    if(!Valid(gridno))
                        return 0;
                    if(DeepWater(gridno, 0))
                        return 2;
                    return Water(gridno, 0) ? 1 : 0;
                }

                virtual uint8_t gasSmoke(int32_t gridno, int8_t level) const
                {
                    if(!Valid(gridno))
                        return 0;
                    const UINT16 flags = gpWorldLevelData[gridno].ubExtFlags[level > 0 ? 1 : 0];
                    uint8_t out = 0;
                    if(flags & MAPELEMENT_EXT_SMOKE)
                        out |= 1;
                    if(flags & (MAPELEMENT_EXT_TEARGAS | MAPELEMENT_EXT_MUSTARDGAS))
                        out |= 2;
                    if(flags & (MAPELEMENT_EXT_CREATUREGAS | MAPELEMENT_EXT_BURNABLEGAS))
                        out |= 4;
                    return out;
                }

                virtual bool corpse(int32_t gridno) const
                {
                    return Valid(gridno) && GetCorpseAtGridNo(gridno, 0) != NULL;
                }

                virtual bool lit(int32_t gridno, int8_t level) const
                {
                    return Valid(gridno) && LightTrueLevel(gridno, level) < NORMAL_LIGHTLEVEL_NIGHT;
                }

                virtual bool bombNear(int32_t gridno) const
                {
                    if(!Valid(gridno))
                        return false;
                    const UINT16 mine = me_->bTeam == OUR_TEAM ? MAPELEMENT_PLAYER_MINE_PRESENT : MAPELEMENT_ENEMY_MINE_PRESENT;
                    return (gpWorldLevelData[gridno].uiFlags & mine) != 0;
                }

                virtual bool selfSees(int32_t gridno, int8_t level) const
                {
                    if(!Valid(gridno))
                        return false;
                    ++g_profile.asked[Profile::LOS_SELF];
                    EngineTerrain* self = const_cast<EngineTerrain*>(this);
                    if(!eyesReady_)
                        self->prepareEyes();
                    LosCache::Key key;
                    memset(&key, 0, sizeof key);
                    key.from = me_->sGridNo;
                    key.viewer = viewer_;
                    key.fromLevel = me_->pathing.bLevel;
                    key.toLevel = level;
                    key.height = LosCache::CUBE_TARGET;
                    int bit;
                    LosCache::Locate(gridno, key.block, bit);
                    LosCache::Entry* e = self->memo(key);
                    const int known = LosCache::Lookup(*e, bit);
                    if(known >= 0)
                        return known == 1;
                    const int64_t t0 = NowUs();
                    const bool seen = self->castOwnRay(gridno, level);
                    g_profile.add(Profile::LOS_SELF, NowUs() - t0);
                    LosCache::Store(*e, bit, seen);
                    return seen;
                }

                virtual bool standingAtSees(int32_t from, int8_t fromLevel, int32_t to, int8_t toLevel, uint8_t toStance) const
                {
                    if(!Valid(from) || !Valid(to))
                        return false;
                    ++g_profile.asked[Profile::LOS_COVER];
                    LosCache::Key key;
                    memset(&key, 0, sizeof key);
                    key.from = from;
                    key.viewer = 0;
                    key.fromLevel = fromLevel;
                    key.toLevel = toLevel;
                    key.height = static_cast<int8_t>(toStance);
                    int bit;
                    LosCache::Locate(to, key.block, bit);
                    LosCache::Entry* e = const_cast<EngineTerrain*>(this)->memo(key);
                    const int known = LosCache::Lookup(*e, bit);
                    if(known >= 0)
                        return known == 1;
                    const int64_t t0 = NowUs();
                    const FLOAT endPos = toStance == STANCE_PRONE ? PRONE_LOS_POS
                                       : toStance == STANCE_CROUCH ? CROUCHED_LOS_POS : STANDING_LOS_POS;
                    const bool seen = LocationToLocationLineOfSightTest(from, fromLevel, to, toLevel, TRUE, CALC_FROM_ALL_DIRS,
                                                                        STANDING_LOS_POS, endPos) != 0;
                    g_profile.add(Profile::LOS_COVER, NowUs() - t0);
                    LosCache::Store(*e, bit, seen);
                    return seen;
                }

                virtual int16_t apToReach(int32_t gridno, int8_t level) const
                {
                    if(!Valid(gridno) || level != me_->pathing.bLevel)
                        return -1;
                    ++g_profile.asked[Profile::REACH];
                    if(!reachBuilt_)
                        const_cast<EngineTerrain*>(this)->buildReach();
                    int dx, dy;
                    Offset(gridno, dx, dy);
                    if(dx < -REACH_R || dx > REACH_R || dy < -REACH_R || dy > REACH_R)
                        return -1;
                    return reach_[(dy + REACH_R) * REACH_W + (dx + REACH_R)];
                }

                virtual bool knownOccupied(int32_t gridno, int8_t level) const
                {
                    if(!Valid(gridno))
                        return false;
                    const UINT8 id = WhoIsThere2(gridno, level);
                    if(id >= TOTAL_SOLDIERS || id == me_->ubID)
                        return false;
                    const SOLDIERTYPE* other = MercPtrs[id];
                    if(!other)
                        return false;
                    if(other->bSide == me_->bSide)
                        return true;
                    return me_->aiData.bOppList[id] == SEEN_CURRENTLY;
                }

            private:
                static bool Valid(int32_t gridno)
                {
                    return gridno >= 0 && gridno < WORLD_MAX;
                }

                void Offset(int32_t gridno, int& dx, int& dy) const
                {
                    dx = (gridno % WORLD_COLS) - (me_->sGridNo % WORLD_COLS);
                    dy = (gridno / WORLD_COLS) - (me_->sGridNo / WORLD_COLS);
                }

                /// The memo entry of a viewpoint and block; the last one asked for is checked first.
                LosCache::Entry* memo(const LosCache::Key& key)
                {
                    if(last_ && last_->used && last_->key == key)
                        return last_;
                    last_ = g_los.entry(key);
                    return last_;
                }

                /// What the soldier's own rays depend on besides the tiles: eye height, gear, condition.
                void prepareEyes()
                {
                    eyesReady_ = true;
                    startZ_ = 0.0f;
                    CalculateSoldierZPos(me_, LOS_POS, &startZ_);
                    thermal_ = HasThermalOptics(me_) != FALSE;
                    uint32_t h = 2166136261u;
                    const uint32_t parts[] = {
                        me_->ubID,
                        gAnimControl[me_->usAnimState].ubEndHeight,
                        me_->inv[HELMETPOS].usItem, me_->inv[HEAD1POS].usItem, me_->inv[HEAD2POS].usItem,
                        me_->inv[HANDPOS].usItem, me_->inv[SECONDHANDPOS].usItem,
                        (uint32_t)(me_->bBlindedCounter > 0), (uint32_t)(me_->bCollapsed != 0), (uint32_t)(me_->bBreath == 0),
                        (uint32_t)(uint8_t)me_->aiData.bShock, (uint32_t)(me_->flags.uiStatusFlags & (SOLDIER_MONSTER | SOLDIER_ROBOT | SOLDIER_VEHICLE)),
                        (uint32_t)thermal_ };
                    for(size_t i = 0; i < sizeof parts / sizeof parts[0]; ++i)
                        h = (h ^ parts[i]) * 16777619u;
                    viewer_ = h ? h : 1;   // 0 marks the fixed standing eye of a cover ray
                }

                /// How far the soldier sees towards a tile: the engine derives it per tile from his gear, traits
                /// and the light there; with nobody standing on the tile only the light varies, so one answer
                /// per light level and level serves every tile.
                int sightLimit(int32_t gridno, int8_t level)
                {
                    if(ARMED_VEHICLE(me_) || WhoIsThere2(gridno, level) != NOBODY)
                        return me_->GetMaxDistanceVisible(gridno, level, CALC_FROM_ALL_DIRS);
                    const int light = LightTrueLevel(gridno, level);
                    if(light < 0 || light >= LIGHT_LEVELS)
                        return me_->GetMaxDistanceVisible(gridno, level, CALC_FROM_ALL_DIRS);
                    int& memo = sightLimit_[level > 0 ? 1 : 0][light];
                    if(memo < 0)
                        memo = me_->GetMaxDistanceVisible(gridno, level, CALC_FROM_ALL_DIRS);
                    return memo;
                }

                /// SoldierTo3DLocationLineOfSightTest(me, gridno, level, 1, TRUE, CALC_FROM_ALL_DIRS), with the
                /// parts that do not depend on the tile computed once per decision.
                bool castOwnRay(int32_t gridno, int8_t level)
                {
                    FLOAT endZ = ((FLOAT)(1 + level * PROFILE_Z_SIZE) - 0.5f) * HEIGHT_UNITS_PER_INDEX;
                    endZ += CONVERT_PIXELS_TO_HEIGHTUNITS(gpWorldLevelData[gridno].sHeight);
                    const int limit = sightLimit(gridno, level);
                    INT16 sX, sY, sX2, sY2;
                    ConvertGridNoToCenterCellXY(me_->sGridNo, &sX, &sY);
                    ConvertGridNoToCenterCellXY(gridno, &sX2, &sY2);
                    return LineOfSightTest((FLOAT)sX, (FLOAT)sY, startZ_, (FLOAT)sX2, (FLOAT)sY2, endZ, limit, TRUE,
                                           thermal_ ? TRUE : FALSE, NULL, true, false) != 0;
                }

                /// Dijkstra over the window around the soldier with the engine's per-step cost,
                /// unless the memo still holds this soldier's table from this tile.
                void buildReach()
                {
                    reachBuilt_ = true;
                    static_assert(ReachMemo::W == REACH_W, "reach memo and window agree");
                    const UINT16 mode = static_cast<UINT16>(me_->usUIMovementMode);
                    const int8_t level = me_->pathing.bLevel;
                    if(g_reach.valid && g_reach.soldier == me_->ubID && g_reach.gridno == me_->sGridNo
                       && g_reach.level == level && g_reach.mode == mode)
                    {
                        memcpy(reach_, g_reach.table, sizeof reach_);
                        return;
                    }
                    const int64_t t0 = NowUs();
                    const int n = REACH_W * REACH_W;
                    bool done[REACH_W * REACH_W];
                    for(int i = 0; i < n; ++i)
                    {
                        reach_[i] = -1;
                        done[i] = false;
                    }
                    const int center = REACH_R * REACH_W + REACH_R;
                    reach_[center] = 0;
                    const int startX = me_->sGridNo % WORLD_COLS;
                    const int startY = me_->sGridNo / WORLD_COLS;
                    static const int stepX[MAXDIR] = { 0, 1, 1, 1, 0, -1, -1, -1 };
                    static const int stepY[MAXDIR] = { -1, -1, 0, 1, 1, 1, 0, -1 };

                    // a binary heap of (cost << 16 | cell), smallest cost first; stale entries are skipped
                    static std::vector<uint32_t> heap;
                    heap.clear();
                    heap.push_back((uint32_t)center);
                    while(!heap.empty())
                    {
                        std::pop_heap(heap.begin(), heap.end(), std::greater<uint32_t>());
                        const uint32_t packed = heap.back();
                        heap.pop_back();
                        const int best = (int)(packed & 0xFFFF);
                        const int cost = (int)(packed >> 16);
                        if(done[best] || cost != reach_[best])
                            continue;
                        done[best] = true;
                        const int cx = best % REACH_W - REACH_R;
                        const int cy = best / REACH_W - REACH_R;
                        for(int dir = 0; dir < MAXDIR; ++dir)
                        {
                            const int nx = cx + stepX[dir];
                            const int ny = cy + stepY[dir];
                            if(nx < -REACH_R || nx > REACH_R || ny < -REACH_R || ny > REACH_R)
                                continue;
                            const int wx = startX + nx;
                            const int wy = startY + ny;
                            if(wx < 0 || wy < 0 || wx >= WORLD_COLS || wy >= WORLD_ROWS)
                                continue;
                            const int32_t ng = wy * WORLD_COLS + wx;
                            const int ni = (ny + REACH_R) * REACH_W + (nx + REACH_R);
                            if(done[ni] || knownOccupied(ng, level))
                                continue;
                            const INT16 step = ActionPointCost(me_, ng, static_cast<INT8>(dir), mode);
                            if(step <= 0 || step >= 100)
                                continue;
                            const int total = reach_[best] + step;
                            if(total > 32000)
                                continue;
                            if(reach_[ni] < 0 || total < reach_[ni])
                            {
                                reach_[ni] = static_cast<int16_t>(total);
                                heap.push_back(((uint32_t)total << 16) | (uint32_t)ni);
                                std::push_heap(heap.begin(), heap.end(), std::greater<uint32_t>());
                            }
                        }
                    }
                    g_reach.valid = true;
                    g_reach.soldier = me_->ubID;
                    g_reach.gridno = me_->sGridNo;
                    g_reach.level = level;
                    g_reach.mode = mode;
                    memcpy(g_reach.table, reach_, sizeof reach_);
                    g_profile.add(Profile::REACH, NowUs() - t0);
                }

                enum { LIGHT_LEVELS = 32 };

                SOLDIERTYPE* me_;
                bool reachBuilt_;
                int16_t reach_[REACH_W * REACH_W];
                bool eyesReady_;
                uint32_t viewer_;
                FLOAT startZ_;
                bool thermal_;
                int sightLimit_[2][LIGHT_LEVELS];
                LosCache::Entry* last_;
        };

        // ------------------------------------------------------------------ fair inputs

        void FillFairInputs(SOLDIERTYPE* me, FairInputs& in, EngineTerrain& terrain)
        {
            ClearFairInputs(in);
            in.terrain = &terrain;
            const BeliefStore& beliefs = GetBeliefStore();

            SelfState& s = in.self;
            s.id = me->ubID;
            s.team = static_cast<uint8_t>(me->bTeam);
            s.gridno = me->sGridNo;
            s.level = me->pathing.bLevel;
            s.facing = me->ubDirection;
            s.stance = StanceOf(me);
            s.ap = me->bActionPoints;
            s.apStart = me->bInitialActionPoints;
            s.life = me->stats.bLife;
            s.lifeMax = me->stats.bLifeMax;
            s.breath = me->bBreath;
            s.bleeding = me->bBleeding;
            s.morale = me->aiData.bAIMorale;
            s.shock = me->aiData.bShock;
            s.collapsed = me->bCollapsed ? 1 : 0;
            s.breathCollapsed = me->bBreathCollapsed ? 1 : 0;

            const INT32 ahead = NewGridNo(me->sGridNo, DirectionInc(me->ubDirection));
            OBJECTTYPE& gun = me->inv[HANDPOS];
            const bool hasGun = gun.exists() && (Item[gun.usItem].usItemClass & IC_GUN);
            if(hasGun)
            {
                s.gunClass = GunClassOf(gun.usItem);
                s.gunRangeTiles = GunRangeTiles(me);
                s.roundsInGun = gun[0]->data.gun.ubGunShotsLeft;
                s.magazineSize = Clamp255(GetMagSize(&gun));
                s.magazines = Clamp255(CountMagazines(me, gun.usItem));
                s.scoped = IsScoped(&gun) ? 1 : 0;
                s.burstCapable = IsGunBurstCapable(&gun, FALSE, me) ? 1 : 0;
                s.autofireCapable = IsGunAutofireCapable(&gun) ? 1 : 0;
                const int aim = AllowedAimingLevels(me, ahead);
                s.maxAimClicks = static_cast<uint8_t>(aim > TACNN_AIM_LEVELS - 1 ? TACNN_AIM_LEVELS - 1 : aim);
                s.apShoot = ClampAP(MinAPsToAttack(me, ahead, FALSE, 0));
                if(s.burstCapable)
                    s.apBurst = ClampAP(s.apShoot + CalcAPsToBurst(me->CalcActionPoints(), &gun, me));
                if(s.autofireCapable)
                    s.apAuto = ClampAP(s.apShoot + CalcAPsToAutofire(me->CalcActionPoints(), &gun, 3, me));
                s.apReload = ClampAP(APBPConstants[AP_RELOAD_GUN]);
                s.apReady = ClampAP(GetAPsToReadyWeapon(me, me->usAnimState));
            }
            s.apThrow = ClampAP(MinAPsToThrow(me, ahead, FALSE));
            s.apStand = ClampAP(GetAPsToChangeStance(me, ANIM_STAND));
            s.apCrouch = ClampAP(GetAPsToChangeStance(me, ANIM_CROUCH));
            s.apProne = ClampAP(GetAPsToChangeStance(me, ANIM_PRONE));
            s.apClimb = ClampAP(GetAPsToClimbRoof(me, me->pathing.bLevel > 0));
            s.apMinMove = ClampAP(MinPtsToMove(me));
            s.apSeekReserve = ClampAP(APBPConstants[MAX_AP_CARRIED]);
            s.grenades = Clamp255(CountClass(me, IC_GRENADE));
            s.smokeGrenades = Clamp255(CountSmokeGrenades(me));
            s.medkits = FindObjClass(me, IC_MEDKIT) != NO_SLOT ? 1 : 0;
            s.knife = FindObjClass(me, IC_BLADE) != NO_SLOT ? 1 : 0;
            s.throwingKnife = FindObjClass(me, IC_THROWING_KNIFE) != NO_SLOT ? 1 : 0;
            s.launcher = (FindObjClass(me, IC_LAUNCHER) != NO_SLOT || FindRocketLauncherOrCannon(me) != NO_SLOT) ? 1 : 0;
            s.detonator = FindObjClass(me, IC_BOMB) != NO_SLOT ? 1 : 0;
            s.alert = static_cast<uint8_t>(me->aiData.bAlertStatus);
            s.orders = static_cast<uint8_t>(me->aiData.bOrders);
            s.attitude = static_cast<uint8_t>(me->aiData.bAttitude);
            s.experience = static_cast<uint8_t>(me->stats.bExpLevel);
            s.marksmanship = static_cast<uint8_t>(me->stats.bMarksmanship);
            s.underFire = me->aiData.bUnderFire ? 1 : 0;
            s.canClimb = (me->pathing.bLevel > 0
                          || FindDirectionForClimbing(me, me->sGridNo, me->pathing.bLevel) != DIRECTION_IRRELEVANT) ? 1 : 0;
            s.inWater = terrain.water(me->sGridNo) ? 1 : 0;
            s.inGas = (terrain.gasSmoke(me->sGridNo, me->pathing.bLevel) & 6) ? 1 : 0;
            s.inSmoke = (terrain.gasSmoke(me->sGridNo, me->pathing.bLevel) & 1) ? 1 : 0;
            s.inLight = terrain.lit(me->sGridNo, me->pathing.bLevel) ? 1 : 0;
            s.turnBased = (gTacticalStatus.uiFlags & TURNBASED) ? 1 : 0;
            s.stage = g_stage[me->ubID];
            s.role = RoleOf(me);
            s.turn = static_cast<uint16_t>(beliefs.turn() > 65535 ? 65535 : beliefs.turn());
            s.lastTargetGridno = TileOrNone(me->sLastTarget);
            s.blackListGridno = TileOrNone(me->pathing.sBlackList);
            if((me->aiData.bOrders == POINTPATROL || me->aiData.bOrders == RNDPTPATROL) && me->aiData.bPatrolCnt > 0)
            {
                int point = me->aiData.bNextPatrolPnt;
                if(point < 0 || point >= me->aiData.bPatrolCnt || point >= MAXPATROLGRIDS)
                    point = 0;
                const INT32 g = me->aiData.sPatrolGrid[point];
                s.objectiveGridno = TileOrNone(g);
                s.objectiveLevel = 0;
            }

            // own team
            const TacticalTeamType& team = gTacticalStatus.Team[me->bTeam];
            for(int i = team.bFirstID; i <= team.bLastID && i < TOTAL_SOLDIERS; ++i)
            {
                SOLDIERTYPE* p = MercPtrs[i];
                if(!p || !p->bActive || !p->bInSector || (p->flags.uiStatusFlags & SOLDIER_VEHICLE))
                    continue;
                ++s.teamTotal;
                if(p->stats.bLife <= 0)
                    continue;
                ++s.teamAlive;
                if(p == me || in.teamCount >= MAX_TEAM)
                    continue;
                Teammate& t = in.team[in.teamCount++];
                t.id = p->ubID;
                t.gridno = p->sGridNo;
                t.level = p->pathing.bLevel;
                t.stance = StanceOf(p);
                t.lifeFrac = Clamp255(p->stats.bLifeMax > 0 ? p->stats.bLife * 255 / p->stats.bLifeMax : 0);
                t.gunClass = GunClassOf(p->inv[HANDPOS].exists() ? p->inv[HANDPOS].usItem : NOTHING);
                t.alert = static_cast<uint8_t>(p->aiData.bAlertStatus);
                t.underFire = p->aiData.bUnderFire ? 1 : 0;
                t.wounded = (p->bBleeding > 0 || p->stats.bLife < p->stats.bLifeMax) ? 1 : 0;
                t.lastTargetGridno = TileOrNone(p->sLastTarget);
            }

            // believed opponents: only through the opponent list and the belief store
            for(int i = 0; i < TOTAL_SOLDIERS && in.enemyCount < MAX_ENEMIES; ++i)
            {
                const SOLDIERTYPE* p = MercPtrs[i];
                if(!p || !p->bActive || !p->bInSector || i == me->ubID)
                    continue;
                if(p->bSide == me->bSide || p->aiData.bNeutral)
                    continue;
                const INT8 personal = me->aiData.bOppList[i];
                const INT8 pub = gbPublicOpplist[me->bTeam][i];
                const BeliefRecord& rec = beliefs.get(me->ubID, static_cast<uint8_t>(i));
                if(personal == NOT_HEARD_OR_SEEN && pub == NOT_HEARD_OR_SEEN && rec.seenTurn == 0 && rec.heardTurn == 0)
                    continue;

                BelievedEnemy& e = in.enemies[in.enemyCount++];
                e.id = static_cast<uint8_t>(i);
                e.knowledge = personal;
                e.publicKnowledge = pub;
                const INT32 loc = gsLastKnownOppLoc[me->ubID][i];
                e.gridno = personal != NOT_HEARD_OR_SEEN ? TileOrNone(loc) : NO_TILE;
                e.level = gbLastKnownOppLevel[me->ubID][i];
                const INT32 pubLoc = gsPublicLastKnownOppLoc[me->bTeam][i];
                e.publicGridno = pub != NOT_HEARD_OR_SEEN ? TileOrNone(pubLoc) : NO_TILE;
                e.publicLevel = gbPublicLastKnownOppLevel[me->bTeam][i];
                if(e.gridno == NO_TILE && e.publicGridno == NO_TILE)
                {
                    // nothing current in the opponent list: fall back on the belief store's last sighting or noise
                    if(rec.seenTurn != 0 && TileOrNone(rec.seenGridno) != NO_TILE)
                    {
                        e.gridno = rec.seenGridno;
                        e.level = rec.seenLevel;
                    }
                    else if(rec.heardTurn != 0 && TileOrNone(rec.heardGridno) != NO_TILE)
                    {
                        e.gridno = rec.heardGridno;
                        e.level = rec.heardLevel;
                    }
                }
                const uint8_t sincePersonal = TurnsSinceCode(personal);
                const uint8_t sincePublic = TurnsSinceCode(pub);
                e.turnsSinceFresh = sincePersonal < sincePublic ? sincePersonal : sincePublic;
                e.seenStance = rec.seenStance;
                e.seenGunClass = rec.seenGunClass;
                e.seenGunRange = rec.seenGunRange;
                e.seenLifeBand = rec.seenLifeBand;
                e.attacksOnMe = rec.attacksOnMe;
                e.damageToMe = rec.damageToMe;
                e.damageByMe = rec.damageByMe;
                e.losNow = personal == SEEN_CURRENTLY ? 1 : 0;
                for(int k = 0; k < NUM_WATCHED_LOCS; ++k)
                    if(e.gridno != NO_TILE && gsWatchedLoc[me->ubID][k] == e.gridno)
                        e.watched = 1;
                e.turnsSinceLos = personal >= SEEN_CURRENTLY ? TurnsSinceCode(personal) : 3;
                if(e.losNow && hasGun && e.gridno != NO_TILE)
                {
                    const UINT32 snap = AICalcChanceToHitGun(me, e.gridno, 0, AIM_SHOT_TORSO, e.level, me->usAnimState);
                    const UINT32 best = AICalcChanceToHitGun(me, e.gridno, s.maxAimClicks, AIM_SHOT_TORSO, e.level, me->usAnimState);
                    e.cthSnap = static_cast<int16_t>(snap > 100 ? 100 : snap);
                    e.cthBest = static_cast<int16_t>(best > 100 ? 100 : best);
                    e.apShot = ClampAP(MinAPsToAttack(me, e.gridno, TRUE, 0));
                }
            }

            // noises
            if(TileOrNone(me->aiData.sNoiseGridno) != NO_TILE && in.noiseCount < MAX_NOISES)
            {
                NoiseMemory& n = in.noises[in.noiseCount++];
                n.gridno = me->aiData.sNoiseGridno;
                n.level = me->bNoiseLevel;
                n.volume = me->aiData.ubNoiseVolume;
                n.type = 0;
                n.turnsAgo = 0;
                n.isPublic = 0;
            }
            if(TileOrNone(gsPublicNoiseGridNo[me->bTeam]) != NO_TILE && in.noiseCount < MAX_NOISES)
            {
                NoiseMemory& n = in.noises[in.noiseCount++];
                n.gridno = gsPublicNoiseGridNo[me->bTeam];
                n.level = gbPublicNoiseLevel[me->bTeam];
                n.volume = gubPublicNoiseVolume[me->bTeam];
                n.type = 0;
                n.turnsAgo = 0;
                n.isPublic = 1;
            }

            // watched locations
            for(int k = 0; k < NUM_WATCHED_LOCS && in.watchedCount < MAX_WATCHED; ++k)
            {
                if(gsWatchedLoc[me->ubID][k] == NOWHERE)
                    continue;
                in.watched[in.watchedCount] = gsWatchedLoc[me->ubID][k];
                in.watchedLevel[in.watchedCount] = gbWatchedLocLevel[me->ubID][k];
                ++in.watchedCount;
            }
        }

        // ------------------------------------------------------------------ decisions

        void FillHead(SOLDIERTYPE* me, TacnnDecision& head, uint8_t source)
        {
            memset(&head, 0, sizeof head);
            head.battle_lo = g_battleLo;
            head.battle_hi = g_battleHi;
            head.decision_index = g_decisionIndex;
            const uint32_t turn = GetBeliefStore().turn();
            head.turn = static_cast<uint16_t>(turn > 65535 ? 65535 : turn);
            head.team = static_cast<uint8_t>(me->bTeam);
            head.soldier_id = me->ubID;
            head.source = source;
            head.action_points = me->bActionPoints;
        }

        /**
         * The poison test of the cheat audit (NEURAL_CHEAT_AUDIT): change the
         * life of every opponent the soldier does not currently see, build
         * the observation again, put the life back, and compare. A builder
         * that only reads what the soldier may know produces the same bytes;
         * the first differing byte, if any, is named in the log.
         */
        void CheatAudit(SOLDIERTYPE* me)
        {
            static TacnnObservation obs;
            static TacnnCandidateSet cand;
            static TacnnActionMask mask;
            static FairInputs inputs;
            static BuildScratch scratch;
            static unsigned identical = 0, differing = 0;

            INT16 saved[TOTAL_SOLDIERS];
            bool poisoned[TOTAL_SOLDIERS];
            int count = 0;
            for(int i = 0; i < TOTAL_SOLDIERS; ++i)
            {
                poisoned[i] = false;
                SOLDIERTYPE* p = MercPtrs[i];
                if(!p || !p->bActive || !p->bInSector || i == me->ubID || p->bSide == me->bSide)
                    continue;
                if(me->aiData.bOppList[i] == SEEN_CURRENTLY)
                    continue;
                saved[i] = p->stats.bLife;
                p->stats.bLife = static_cast<INT8>(p->stats.bLife > 1 ? p->stats.bLife - 1 : p->stats.bLife + 1);
                poisoned[i] = true;
                ++count;
            }

            EngineTerrain terrain(me);
            FillFairInputs(me, inputs, terrain);
            BuildObservation(inputs, obs, scratch);
            ProposeCandidates(inputs, scratch, cand);
            BuildActionMask(inputs, cand, mask);
            inputs.terrain = 0;

            for(int i = 0; i < TOTAL_SOLDIERS; ++i)
                if(poisoned[i])
                    MercPtrs[i]->stats.bLife = static_cast<INT8>(saved[i]);

            const unsigned char* a[3] = { reinterpret_cast<const unsigned char*>(&g_pending.obs),
                                          reinterpret_cast<const unsigned char*>(&g_pending.cand),
                                          reinterpret_cast<const unsigned char*>(&g_pending.mask) };
            const unsigned char* b[3] = { reinterpret_cast<const unsigned char*>(&obs),
                                          reinterpret_cast<const unsigned char*>(&cand),
                                          reinterpret_cast<const unsigned char*>(&mask) };
            const size_t sizes[3] = { sizeof obs, sizeof cand, sizeof mask };
            const char* const names[3] = { "observation", "candidates", "mask" };
            for(int part = 0; part < 3; ++part)
            {
                for(size_t byte = 0; byte < sizes[part]; ++byte)
                {
                    if(a[part][byte] == b[part][byte])
                        continue;
                    ++differing;
                    SGP_INFO() << "tacnn: cheat audit decision " << g_decisionIndex << " soldier " << (int)me->ubID
                               << ": " << names[part] << " DIFFERS at byte " << byte << " after poisoning " << count
                               << " unseen opponents (" << identical << " identical, " << differing << " differing so far)"
                               << sgp::endl;
                    return;
                }
            }
            ++identical;
            SGP_INFO() << "tacnn: cheat audit decision " << g_decisionIndex << " soldier " << (int)me->ubID
                       << ": identical after poisoning " << count << " unseen opponents (" << identical
                       << " identical, " << differing << " differing so far)" << sgp::endl;
        }

        /// Build the observation for the soldier and park it as the pending decision.
        /// Build the fair observation, the candidates and the mask and park them until EndDecision writes
        /// them. The cheat audit runs for enemies only (audit = false for a player soldier being logged).
        void BeginDecision(SOLDIERTYPE* me, bool audit = true)
        {
            if(g_pending.active && g_pending.soldier == me->ubID)
                return;
            g_pending.active = true;
            g_pending.soldier = me->ubID;
            g_pending.startedUs = NowUs();
            g_pending.hasEmbedded = false;
            memset(&g_pending.action, 0, sizeof g_pending.action);
            FillHead(me, g_pending.head, TACNN_SRC_LEGACY);

            {
                FreshenLosCache();
                EngineTerrain terrain(me);
                int64_t t0 = g_pending.startedUs;
                FillFairInputs(me, g_inputs, terrain);
                int64_t t1 = NowUs();
                g_profile.add(Profile::FAIR, t1 - t0);
                BuildObservation(g_inputs, g_pending.obs, g_scratch);
                t0 = NowUs();
                g_profile.add(Profile::OBS, t0 - t1);
                ProposeCandidates(g_inputs, g_scratch, g_pending.cand);
                t1 = NowUs();
                g_profile.add(Profile::CAND, t1 - t0);
                BuildActionMask(g_inputs, g_pending.cand, g_pending.mask);
                g_profile.add(Profile::MASK, NowUs() - t1);
                ++g_profile.n;
                // the terrain object dies here; nothing in the pending record refers to it
                g_inputs.terrain = 0;
            }
            if(audit && g_settings.cheatAudit)
                CheatAudit(me);
        }

        /// Write the pending record with what the soldier is about to do.
        void EndDecision(SOLDIERTYPE* me, uint8_t source)
        {
            if(!g_pending.active || g_pending.soldier != me->ubID)
                return;
            g_pending.active = false;
            TacnnDecision& head = g_pending.head;
            head.source = source;
            head.engine_action = me->aiData.bAction;
            head.engine_action_data = me->aiData.usActionData;
            head.target_level = me->bTargetLevel;
            head.aim_time = static_cast<int8_t>(me->aiData.bAimTime > 127 ? 127 : me->aiData.bAimTime);
            const int64_t elapsed = NowUs() - g_pending.startedUs;
            head.elapsed_us = static_cast<uint16_t>(elapsed < 0 ? 0 : elapsed > 65535 ? 65535 : elapsed);
            ++g_decisionIndex;
            ++g_decisionsEver;
            if(g_decisionsInBattle < 65535)
                ++g_decisionsInBattle;

            DecisionLog& log = GetDecisionLog();
            if(log.isOpen())
            {
                log.writeDecision(head, g_pending.obs, g_pending.mask, g_pending.cand, g_pending.action);
                if(g_pending.hasEmbedded)
                {
                    g_pending.embedded.decision_index = head.decision_index;
                    log.writeEmbedded(g_pending.embedded);
                }
            }
            g_pending.hasEmbedded = false;
        }

        /// One more decision made by this soldier this turn, whichever path produced it.
        void CountStage(const SOLDIERTYPE* me)
        {
            if(me->ubID < TOTAL_SOLDIERS && g_stage[me->ubID] < 255)
                ++g_stage[me->ubID];
        }

        /// Translate a policy answer into the soldier's aiData fields. False means "could not, run legacy".
        bool ApplyPolicyAction(SOLDIERTYPE* me, const TacnnCandidateSet& cand, const TacnnActionMask& mask,
                               const TacnnPolicyAction& action)
        {
            if(action.type >= TACNN_ACTION_TYPES || !mask.type[action.type])
                return false;
            const int slot = action.target_slot < TACNN_CANDIDATES ? action.target_slot : 0;
            const INT32 target = cand.gridno[slot];
            const INT8 targetLevel = cand.level[slot];
            const bool haveTarget = cand.kind[slot] != TACNN_CAND_EMPTY && target >= 0 && target < WORLD_MAX
                                    && mask.target[slot];
            const int aim = action.aim < TACNN_AIM_LEVELS ? action.aim : 0;
            const int mode = action.mode < TACNN_FIRE_MODES ? action.mode : 0;
            const int stance = action.stance < TACNN_STANCES ? action.stance : 0;

            STRUCT_AIData& ai = me->aiData;
            ai.bAimTime = 0;
            me->bDoBurst = 0;
            me->bDoAutofire = 0;

            switch(action.type)
            {
                case TACNN_ACT_END_TURN:
                case TACNN_ACT_HOLD:
                    ai.bAction = AI_ACTION_END_TURN;
                    ai.usActionData = NOWHERE;
                    return true;

                case TACNN_ACT_MOVE_RUN:
                case TACNN_ACT_MOVE_WALK:
                case TACNN_ACT_MOVE_SWAT:
                case TACNN_ACT_TAKE_COVER:
                case TACNN_ACT_SEEK_OPPONENT:
                case TACNN_ACT_SEEK_NOISE:
                case TACNN_ACT_SEEK_FRIEND:
                case TACNN_ACT_FLANK_LEFT:
                case TACNN_ACT_FLANK_RIGHT:
                case TACNN_ACT_WITHDRAW:
                case TACNN_ACT_RUN_AWAY:
                    if(!haveTarget || target == me->sGridNo)
                        return false;
                    switch(action.type)
                    {
                        case TACNN_ACT_MOVE_RUN:       ai.bAction = AI_ACTION_RUN; break;
                        case TACNN_ACT_MOVE_WALK:      ai.bAction = AI_ACTION_WALK; break;
                        case TACNN_ACT_TAKE_COVER:
                        case TACNN_ACT_MOVE_SWAT:      ai.bAction = AI_ACTION_TAKE_COVER; break;
                        case TACNN_ACT_SEEK_OPPONENT:  ai.bAction = AI_ACTION_SEEK_OPPONENT; break;
                        case TACNN_ACT_SEEK_NOISE:     ai.bAction = AI_ACTION_SEEK_NOISE; break;
                        case TACNN_ACT_SEEK_FRIEND:    ai.bAction = AI_ACTION_SEEK_FRIEND; break;
                        case TACNN_ACT_WITHDRAW:       ai.bAction = AI_ACTION_WITHDRAW; break;
                        case TACNN_ACT_RUN_AWAY:       ai.bAction = AI_ACTION_RUN_AWAY; break;
                        // the legacy flank manoeuvre carries state the policy does not set; walk to the flank tile instead
                        default:                       ai.bAction = AI_ACTION_WALK; break;
                    }
                    ai.usActionData = target;
                    me->bTargetLevel = targetLevel;
                    return true;

                case TACNN_ACT_FIRE_GUN:
                case TACNN_ACT_SUPPRESS:
                {
                    if(!haveTarget)
                        return false;
                    OBJECTTYPE& gun = me->inv[HANDPOS];
                    if(!gun.exists() || !(Item[gun.usItem].usItemClass & IC_GUN) || gun[0]->data.gun.ubGunShotsLeft == 0)
                        return false;
                    ai.bAction = AI_ACTION_FIRE_GUN;
                    ai.usActionData = target;
                    me->bTargetLevel = targetLevel;
                    ai.bAimTime = static_cast<INT16>(mask.aim[aim] ? aim : 0);
                    int fireMode = mode;
                    if(action.type == TACNN_ACT_SUPPRESS)
                        fireMode = mask.mode[2] ? 2 : mask.mode[1] ? 1 : 0;
                    if(fireMode == 1 && mask.mode[1])
                        me->bDoBurst = 1;
                    else if(fireMode == 2 && mask.mode[2])
                    {
                        me->bDoBurst = 1;
                        me->bDoAutofire = 3;
                    }
                    return true;
                }

                case TACNN_ACT_TOSS_PROJECTILE:
                {
                    if(!haveTarget)
                        return false;
                    const INT8 slotIn = FindThrowableGrenade(me);
                    if(slotIn == NO_SLOT)
                        return false;
                    if(slotIn != HANDPOS)
                        RearrangePocket(me, HANDPOS, slotIn, TEMPORARILY);
                    ai.bAction = AI_ACTION_TOSS_PROJECTILE;
                    ai.usActionData = target;
                    me->bTargetLevel = targetLevel;
                    return true;
                }

                case TACNN_ACT_THROW_KNIFE:
                {
                    if(!haveTarget)
                        return false;
                    const INT8 slotIn = FindObjClass(me, IC_THROWING_KNIFE);
                    if(slotIn == NO_SLOT)
                        return false;
                    if(slotIn != HANDPOS)
                        RearrangePocket(me, HANDPOS, slotIn, TEMPORARILY);
                    ai.bAction = AI_ACTION_THROW_KNIFE;
                    ai.usActionData = target;
                    me->bTargetLevel = targetLevel;
                    return true;
                }

                case TACNN_ACT_KNIFE_STAB:
                {
                    if(!haveTarget || SpacesAway(me->sGridNo, target) > 1)
                        return false;
                    const INT8 slotIn = FindObjClass(me, IC_BLADE);
                    if(slotIn == NO_SLOT)
                        return false;
                    if(slotIn != HANDPOS)
                        RearrangePocket(me, HANDPOS, slotIn, TEMPORARILY);
                    ai.bAction = AI_ACTION_KNIFE_STAB;
                    ai.usActionData = target;
                    me->bTargetLevel = targetLevel;
                    return true;
                }

                case TACNN_ACT_RELOAD:
                {
                    OBJECTTYPE& gun = me->inv[HANDPOS];
                    if(!gun.exists() || !(Item[gun.usItem].usItemClass & IC_GUN))
                        return false;
                    if(FindAmmoToReload(me, HANDPOS, NO_SLOT) == NO_SLOT)
                        return false;
                    ai.bAction = AI_ACTION_RELOAD_GUN;
                    ai.usActionData = HANDPOS;
                    return true;
                }

                case TACNN_ACT_CHANGE_STANCE:
                {
                    if(!mask.stance[stance])
                        return false;
                    const INT8 height = stance == 0 ? ANIM_STAND : stance == 1 ? ANIM_CROUCH : ANIM_PRONE;
                    if(gAnimControl[me->usAnimState].ubEndHeight == height)
                        return false;
                    ai.bAction = AI_ACTION_CHANGE_STANCE;
                    ai.usActionData = height;
                    return true;
                }

                case TACNN_ACT_CHANGE_FACING:
                {
                    if(!haveTarget || target == me->sGridNo)
                        return false;
                    const UINT8 dir = GetDirectionFromGridNo(target, me);
                    if(dir == me->ubDirection || dir >= NUM_WORLD_DIRECTIONS)
                        return false;
                    ai.bAction = AI_ACTION_CHANGE_FACING;
                    ai.usActionData = dir;
                    return true;
                }

                case TACNN_ACT_RED_ALERT:
                    ai.bAction = AI_ACTION_RED_ALERT;
                    ai.usActionData = NOWHERE;
                    return true;

                case TACNN_ACT_DOCTOR:
                    if(FindObjClass(me, IC_MEDKIT) == NO_SLOT)
                        return false;
                    ai.bAction = AI_ACTION_DOCTOR;
                    ai.usActionData = NOWHERE;
                    return true;

                case TACNN_ACT_CLIMB:
                    if(FindDirectionForClimbing(me, me->sGridNo, me->pathing.bLevel) == DIRECTION_IRRELEVANT)
                        return false;
                    ai.bAction = AI_ACTION_CLIMB_ROOF;
                    ai.usActionData = me->sGridNo;
                    return true;
            }
            return false;
        }

        /**
         * The in-process network decides: the recurrent state and inbox
         * are gathered, the forward pass runs, the state and the ring are
         * updated with its answer, the heads are decoded greedily under the
         * masks, and everything the parity tool needs is parked for the
         * embedded log record, which EndDecision writes whether or not the
         * engine could carry the answer out.
         */
        void EmbeddedDecide(SOLDIERTYPE* npc, TacnnPolicyAction& answer)
        {
            TacnnEmbedded& rec = g_pending.embedded;
            memset(&rec, 0, sizeof rec);
            rec.battle_lo = g_battleLo;
            rec.battle_hi = g_battleHi;
            rec.decision_index = g_decisionIndex;
            rec.soldier_id = npc->ubID;

            InferInputs in;
            g_hidden.get(npc->ubID, in.h_in);
            g_ring.inboxFor(npc->ubID, g_pending.obs.gridno, in);
            memcpy(rec.h_in, in.h_in, sizeof rec.h_in);
            memcpy(rec.inbox_msg, in.inbox_msg, sizeof rec.inbox_msg);
            memcpy(rec.inbox_sig, in.inbox_sig, sizeof rec.inbox_sig);
            for(int s = 0; s < TACNN_INBOX; ++s)
                rec.inbox_mask[s] = in.inbox_mask[s] > 0.5f ? 1 : 0;

            InferOutputs out;
            const int64_t started = NowUs();
            g_model.infer(g_pending.obs, g_pending.cand, in, out);
            const int64_t took = NowUs() - started;
            g_profile.add(Profile::INFER, took);
            rec.infer_us = static_cast<uint16_t>(took < 0 ? 0 : took > 65535 ? 65535 : took);

            g_hidden.set(npc->ubID, out.h_out);
            g_ring.push(npc->ubID, g_pending.obs.gridno, out.message, out.signature);
            memcpy(rec.type_logits, out.type_logits, sizeof rec.type_logits);
            memcpy(rec.target_logits, out.target_logits, sizeof rec.target_logits);
            memcpy(rec.aim_logits, out.aim_logits, sizeof rec.aim_logits);
            memcpy(rec.mode_logits, out.mode_logits, sizeof rec.mode_logits);
            memcpy(rec.stance_logits, out.stance_logits, sizeof rec.stance_logits);
            memcpy(rec.message, out.message, sizeof rec.message);
            memcpy(rec.signature, out.signature, sizeof rec.signature);
            memcpy(rec.h_out, out.h_out, sizeof rec.h_out);
            g_pending.hasEmbedded = true;

            DecodeGreedy(out, g_pending.mask, g_pending.cand, answer);
            // the record says what the network chose even when the engine could not carry it out
            g_pending.action = answer;
        }

        /**
         * The rules of the training environment (tools/tacsim/envs/tactical_env.py,
         * DecisionEnv._execute and _waste) for an answer the world cannot carry
         * out: it is a wasted decision, the soldier is asked again, and his turn
         * ends after three wasted answers in a row or DECISION_CAP decisions in
         * the turn. The policy learnt under those rules, so the engine keeps
         * them rather than handing a wasted answer to the legacy tree, and every
         * wasted decision is logged like any other, with AI_ACTION_NONE as the
         * engine action. Returns true with the soldier's aiData set.
         */
        const int DECISION_CAP = 12;        ///< DecisionEnv(decision_cap=12)
        const int WASTE_STREAK_CAP = 3;     ///< DecisionEnv._waste ends the turn at the third in a row
        uint8_t g_wasteStreak[TOTAL_SOLDIERS];

        bool EmbeddedTurn(SOLDIERTYPE* npc)
        {
            for(;;)
            {
                TacnnPolicyAction answer;
                EmbeddedDecide(npc, answer);
                const int64_t started = NowUs();
                const bool applied = ApplyPolicyAction(npc, g_pending.cand, g_pending.mask, answer);
                g_profile.add(Profile::APPLY, NowUs() - started);
                if(applied)
                {
                    g_wasteStreak[npc->ubID] = 0;
                    return true;
                }
                npc->aiData.bAction = AI_ACTION_NONE;
                npc->aiData.usActionData = NOWHERE;
                EndDecision(npc, TACNN_SRC_EMBEDDED);
                CountStage(npc);
                if(g_wasteStreak[npc->ubID] < 255)
                    ++g_wasteStreak[npc->ubID];
                if(g_wasteStreak[npc->ubID] >= WASTE_STREAK_CAP || g_stage[npc->ubID] >= DECISION_CAP)
                {
                    npc->aiData.bAction = AI_ACTION_END_TURN;
                    npc->aiData.usActionData = NOWHERE;
                    return true;
                }
                BeginDecision(npc);
            }
        }

        /**
         * The policy NeuralPlan consults: builds the fair observation, asks
         * the network (in process or over the sidecar pipe, by NEURAL_MODE),
         * applies the answer. Declines (so the legacy cascade runs) whenever
         * the mode is off, the sidecar is unreachable, late, or answers
         * "decline", or the answer is not applicable; the decision is logged
         * either way.
         */
        class NeuralPolicy : public AI::tactical::CustomPolicy
        {
            public:
                virtual const char* name() const { return g_settings.mode == TACNN_MODE_OFF ? "log" : ModeName(g_settings.mode); }

                virtual bool decide(SOLDIERTYPE* npc, AI::tactical::PlanInputData& environment)
                {
                    EnsureApplied();
                    if(!environment.turn_based() || !Instrumented(npc) || !TurnBasedCombat())
                        return false;
                    if(g_settings.mode == TACNN_MODE_OFF && !g_settings.log && !g_settings.cheatAudit)
                        return false;

                    if(g_settings.mode == TACNN_MODE_EMBEDDED && g_model.loaded() && g_stage[npc->ubID] >= DECISION_CAP)
                    {
                        // DecisionEnv._advance: at the cap the soldier's turn ends without another decision
                        npc->aiData.bAction = AI_ACTION_END_TURN;
                        npc->aiData.usActionData = NOWHERE;
                        return true;
                    }
                    BeginDecision(npc);
                    if(g_settings.mode == TACNN_MODE_EMBEDDED)
                        return g_model.loaded() && EmbeddedTurn(npc);
                    if(g_settings.mode != TACNN_MODE_SIDECAR)
                        return false;

                    TacnnSidecarRequest request;
                    memset(&request, 0, sizeof request);
                    request.battle_lo = g_battleLo;
                    request.battle_hi = g_battleHi;
                    request.decision_index = g_decisionIndex;
                    request.turn = g_pending.head.turn;
                    request.team = static_cast<uint8_t>(npc->bTeam);
                    request.soldier_id = npc->ubID;

                    SidecarClient& client = GetSidecarClient();
                    TacnnPolicyAction answer;
                    memset(&answer, 0, sizeof answer);
                    const bool got = client.request(request, g_pending.obs, g_pending.mask, g_pending.cand, answer);
                    if(!got)
                    {
                        if(!client.declined())
                            SGP_INFO() << "tacnn: sidecar unavailable (" << client.lastError()
                                       << "), soldier " << (int)npc->ubID << " falls back to legacy" << sgp::endl;
                        return false;
                    }
                    if(!ApplyPolicyAction(npc, g_pending.cand, g_pending.mask, answer))
                    {
                        SGP_INFO() << "tacnn: sidecar action " << (int)answer.type << " slot " << (int)answer.target_slot
                                   << " not applicable for soldier " << (int)npc->ubID << ", legacy decides" << sgp::endl;
                        return false;
                    }
                    g_pending.action = answer;
                    return true;
                }
        };
        NeuralPolicy g_policy;

        void FillMapName(uint8_t* out, size_t size)
        {
            const std::string name = SituationMapName();
            memset(out, 0, size);
            memcpy(out, name.data(), name.size() < size - 1 ? name.size() : size - 1);
        }

        void NewBattleId()
        {
            static uint32_t counter = 0;
            g_battleLo = static_cast<uint32_t>(time(0));
            g_battleHi = ++counter;
        }
    }

    // ---------------------------------------------------------------------- public hooks

    void ApplySettings()
    {
        g_settings.applied = true;
        const GAME_EXTERNAL_OPTIONS& o = gGameExternalOptions;
        g_settings.log = o.fNeuralLog != FALSE;
        g_settings.logPlayer = o.fNeuralLogPlayer != FALSE;
        g_settings.logDir = o.szNeuralLogDir[0] ? o.szNeuralLogDir : "tacnn-log";
        g_settings.logCapBytes = static_cast<uint64_t>(o.uiNeuralLogCapMB) * 1024u * 1024u;
        g_settings.sidecar = o.fNeuralSidecar != FALSE;
        g_settings.sidecarTimeoutMs = o.uiNeuralSidecarTimeoutMs ? o.uiNeuralSidecarTimeoutMs : 50;
        g_settings.seed = o.uiNeuralAISeed;
        g_settings.exportSituations = o.fNeuralExportSituations != FALSE;
        g_settings.exportDir = o.szNeuralExportDir[0] ? o.szNeuralExportDir : "tacnn-situations";
        g_settings.cheatAudit = o.fNeuralCheatAudit != FALSE;
        const int mode = ParseMode(o.szNeuralMode, g_settings.sidecar);
        if(mode < 0)
        {
            static std::string warned;
            if(warned != o.szNeuralMode)
            {
                warned = o.szNeuralMode;
                SGP_INFO() << "tacnn: NEURAL_MODE '" << o.szNeuralMode << "' is not off, sidecar or embedded; off" << sgp::endl;
            }
            g_settings.mode = TACNN_MODE_OFF;
        }
        else
            g_settings.mode = mode;
        g_settings.sidecar = g_settings.mode == TACNN_MODE_SIDECAR;
        {
            std::string model = o.szNeuralModel[0] ? o.szNeuralModel : "policy.onnx";
            if(model.find('\\') == std::string::npos && model.find('/') == std::string::npos)
                model = "AI\\" + model;
            g_settings.modelPath = model;
        }
        EnsureModelLoaded();

        AIRandomSetFallback(&GameRandomFallback);
        if(AIRandomSeed() != g_settings.seed)
            AIRandomSetSeed(g_settings.seed);

        DecisionLog& log = GetDecisionLog();
        if(g_settings.log)
        {
            if(!log.isOpen() || log.directory() != g_settings.logDir)
            {
                log.close();
                if(!log.open(g_settings.logDir, g_settings.logCapBytes, 0))
                    SGP_INFO() << "tacnn: cannot open decision log in " << g_settings.logDir << sgp::endl;
            }
        }
        else if(log.isOpen())
            log.close();

        SidecarClient& client = GetSidecarClient();
#ifdef _WIN32
        client.configure("\\\\.\\pipe\\ja2mod-policy", g_settings.sidecarTimeoutMs);
#else
        // the loopback port tools/tacsim/sidecar_server.py listens on where there are no named pipes
        client.configure("tcp:48213", g_settings.sidecarTimeoutMs);
#endif
        if(!g_settings.sidecar)
            client.disconnect();

        SituationExportConfigure(g_settings.exportDir, g_settings.exportSituations);

        AI::tactical::SetActivePolicy((g_settings.mode != TACNN_MODE_OFF || g_settings.log || g_settings.cheatAudit) ? &g_policy : 0);
    }

    void OnEnterCombatMode()
    {
        ApplySettings();
        g_inBattle = true;
        NewBattleId();
        g_decisionIndex = 0;
        g_decisionsInBattle = 0;
        g_damageDealt = 0;
        g_damageTaken = 0;
        memset(g_stage, 0, sizeof g_stage);
        memset(g_wasteStreak, 0, sizeof g_wasteStreak);
        g_profile.clear();
        ClearTerrainMemos();
        g_pending.active = false;
        g_pending.hasEmbedded = false;
        AIRandomReset();
        // the network starts every battle with a blank memory and a silent radio
        g_hidden.clear();
        g_ring.clear();
        g_ring.setColumns(WORLD_COLS);

        DecisionLog& log = GetDecisionLog();
        if(log.isOpen())
        {
            TacnnBattleStart start;
            memset(&start, 0, sizeof start);
            start.battle_lo = g_battleLo;
            start.battle_hi = g_battleHi;
            start.sector_x = gWorldSectorX;
            start.sector_y = gWorldSectorY;
            start.sector_z = gbWorldSectorZ;
            start.difficulty = gGameOptions.ubDifficultyLevel;
            const uint32_t turn = GetBeliefStore().turn();
            start.turn = static_cast<uint16_t>(turn > 65535 ? 65535 : turn);
            start.ai_seed = g_settings.seed;
            FillMapName(start.map_name, sizeof start.map_name);
            log.writeBattleStart(start);
        }
        SGP_INFO() << "tacnn: battle " << g_battleHi << " starts, log " << (log.isOpen() ? "on" : "off")
                   << " mode " << ModeName(g_settings.mode)
                   << (g_settings.mode == TACNN_MODE_EMBEDDED ? " (" + g_model.info().generation + ")" : std::string())
                   << " export " << (g_settings.exportSituations ? "on" : "off")
                   << " seed " << g_settings.seed << sgp::endl;
        HarnessOnBattleStart();
    }

    void EnsureBattleOpen()
    {
        if(!g_inBattle && (gTacticalStatus.uiFlags & INCOMBAT))
            OnEnterCombatMode();
    }

    unsigned DecisionsRecorded()
    {
        return g_decisionsEver;
    }

    int DamageDealtRecorded()
    {
        return g_damageDealtEver;
    }

    int DamageTakenRecorded()
    {
        return g_damageTakenEver;
    }

    /**
     * Close the current episode: count the survivors, settle the outcome (the
     * caller's verdict when it has one, otherwise from who is left standing),
     * write the episode_end record and the decision profile line.
     */
    static TacnnEpisodeEnd CloseEpisodeRecord(bool forced, uint8_t forcedOutcome)
    {
        g_inBattle = false;
        g_pending.active = false;
        SituationDiscard();
        AIRandomRecordStop();

        TacnnEpisodeEnd end;
        memset(&end, 0, sizeof end);
        end.battle_lo = g_battleLo;
        end.battle_hi = g_battleHi;
        const uint32_t turn = GetBeliefStore().turn();
        end.turns = static_cast<uint16_t>(turn > 65535 ? 65535 : turn);
        for(int i = 0; i < TOTAL_SOLDIERS; ++i)
        {
            const SOLDIERTYPE* p = MercPtrs[i];
            if(!p || !p->bActive || !p->bInSector || p->stats.bLife <= 0 || (p->flags.uiStatusFlags & SOLDIER_VEHICLE))
                continue;
            if(p->bTeam == ENEMY_TEAM && end.survivors_enemy < 255) ++end.survivors_enemy;
            else if(p->bTeam == OUR_TEAM && end.survivors_player < 255) ++end.survivors_player;
            else if(p->bTeam == MILITIA_TEAM && end.survivors_militia < 255) ++end.survivors_militia;
        }
        if(forced)
            end.outcome = forcedOutcome;
        else if(end.survivors_enemy == 0 && (end.survivors_player > 0 || end.survivors_militia > 0))
            end.outcome = TACNN_OUTCOME_PLAYER_WON;
        else if(end.survivors_player == 0 && end.survivors_militia == 0 && end.survivors_enemy > 0)
            end.outcome = TACNN_OUTCOME_ENEMY_WON;
        else
            end.outcome = TACNN_OUTCOME_UNKNOWN;
        end.decisions = g_decisionsInBattle;
        end.damage_dealt = g_damageDealt;
        end.damage_taken = g_damageTaken;

        DecisionLog& log = GetDecisionLog();
        if(log.isOpen())
        {
            log.writeEpisodeEnd(end);
            log.flush();
        }
        const std::string profile = DecisionProfileSummary();
        if(!profile.empty())
        {
            const std::string line = "tacnn: battle " + std::to_string((long long)g_battleHi) + " " + profile;
            SGP_INFO() << line << sgp::endl;
        }
        return end;
    }

    void OnExitCombatMode()
    {
        if(!g_inBattle)
            return;
        const TacnnEpisodeEnd end = CloseEpisodeRecord(false, TACNN_OUTCOME_UNKNOWN);
        HarnessOnBattleEnd(end);
    }

    void CloseEpisode(UINT8 ubOutcome)
    {
        if(!g_inBattle)
            return;
        CloseEpisodeRecord(true, ubOutcome);
    }

    std::string DecisionProfileSummary()
    {
        if(g_profile.n == 0)
            return std::string();
        // mean and worst microseconds of each phase of a decision, for the latency budget
        std::string line = "decision profile over " + std::to_string((long long)g_profile.n) + " decisions (mean/max us):";
        for(int b = 0; b < Profile::PHASES; ++b)
            line += std::string(" ") + PROFILE_NAMES[b] + " " + std::to_string((long long)(g_profile.sum[b] / g_profile.n))
                  + "/" + std::to_string((long long)g_profile.max[b]);
        // the terrain questions: total microseconds per decision, computed/asked per decision
        line += "; per decision";
        for(int b = Profile::PHASES; b < Profile::COUNT; ++b)
            line += std::string(" ") + PROFILE_NAMES[b] + " " + std::to_string((long long)(g_profile.sum[b] / g_profile.n))
                  + "us " + std::to_string((long long)(g_profile.calls[b] / g_profile.n)) + "/"
                  + std::to_string((long long)(g_profile.asked[b] / g_profile.n));
        return line;
    }

    void OnBeginTeamTurn(UINT8 ubTeam)
    {
        EnsureApplied();
        if(ubTeam == gbPlayerNum)
            GetBeliefStore().beginTurn();
        if(ubTeam == ENEMY_TEAM)
            memset(g_stage, 0, sizeof g_stage);
        // the line-of-sight memo stays across turns: everything a ray depends on (structures, doors,
        // smoke, light, the game minute) empties it as it changes. The reach table also counts
        // teammates and seen opponents as obstacles, and they move between turns
        g_reach.valid = false;
        HarnessOnBeginTeamTurn(ubTeam);
    }

    void OnInitOpponentKnowledge()
    {
        GetBeliefStore().reset();
        g_pending.active = false;
        ClearTerrainMemos();
        SituationDiscard();
    }

    void OnWorldChanged()
    {
        ClearTerrainMemos();
    }

    void OnManSeesMan(SOLDIERTYPE* pObserver, SOLDIERTYPE* pSeen, INT32 sGridNo, INT8 bLevel)
    {
        if(!pObserver || !pSeen || pObserver->ubID >= TOTAL_SOLDIERS || pSeen->ubID >= TOTAL_SOLDIERS)
            return;
        // what a sighting legitimately tells the observer: where, how he stands, what he carries, how hurt he looks
        const UINT16 usItem = pSeen->inv[HANDPOS].exists() ? pSeen->inv[HANDPOS].usItem : NOTHING;
        GetBeliefStore().recordSighting(pObserver->ubID, pSeen->ubID, sGridNo, bLevel, StanceOf(pSeen),
                                        GunClassOf(usItem), GunRangeTiles(pSeen),
                                        LifeBand(pSeen->stats.bLife, pSeen->stats.bLifeMax));
    }

    void OnHearNoise(SOLDIERTYPE* pListener, UINT8 ubNoiseMaker, INT32 sGridNo, INT8 bLevel, UINT8 ubVolume, UINT8 ubNoiseType)
    {
        if(!pListener || pListener->ubID >= TOTAL_SOLDIERS || ubNoiseMaker >= TOTAL_SOLDIERS)
            return;
        GetBeliefStore().recordHearing(pListener->ubID, ubNoiseMaker, sGridNo, bLevel, ubVolume, ubNoiseType);
    }

    void OnNoticeUnseenAttacker(SOLDIERTYPE* pDefender, SOLDIERTYPE* pAttacker)
    {
        if(!pDefender || !pAttacker || pDefender->ubID >= TOTAL_SOLDIERS || pAttacker->ubID >= TOTAL_SOLDIERS)
            return;
        // the attack itself is the noise: the victim learns roughly where the shot came from
        GetBeliefStore().recordHearing(pDefender->ubID, pAttacker->ubID, pAttacker->sGridNo, pAttacker->pathing.bLevel,
                                       15, NOISE_GUNFIRE);
    }

    void OnSoldierTakeDamage(SOLDIERTYPE* pVictim, UINT8 ubAttacker, INT16 sLifeDeduct, INT16 sBreathLoss, UINT8 ubReason)
    {
        if(!pVictim || pVictim->ubID >= TOTAL_SOLDIERS)
            return;
        const SOLDIERTYPE* attacker = ubAttacker < TOTAL_SOLDIERS ? MercPtrs[ubAttacker] : 0;
        if(attacker && attacker != pVictim && sLifeDeduct > 0)
            GetBeliefStore().recordAttack(ubAttacker, pVictim->ubID, sLifeDeduct);

        if(!g_inBattle)
            return;
        if(attacker && attacker->bTeam == ENEMY_TEAM && pVictim->bTeam != ENEMY_TEAM)
        {
            g_damageDealt += sLifeDeduct;
            g_damageDealtEver += sLifeDeduct;
        }
        if(pVictim->bTeam == ENEMY_TEAM && (!attacker || attacker->bTeam != ENEMY_TEAM))
        {
            g_damageTaken += sLifeDeduct;
            g_damageTakenEver += sLifeDeduct;
        }

        DecisionLog& log = GetDecisionLog();
        if(!log.isOpen())
            return;
        TacnnOutcome outcome;
        memset(&outcome, 0, sizeof outcome);
        outcome.battle_lo = g_battleLo;
        outcome.battle_hi = g_battleHi;
        outcome.decision_index = g_decisionIndex;
        const uint32_t turn = GetBeliefStore().turn();
        outcome.turn = static_cast<uint16_t>(turn > 65535 ? 65535 : turn);
        outcome.attacker = attacker ? ubAttacker : TACNN_NO_UNIT;
        outcome.victim = pVictim->ubID;
        outcome.attacker_team = attacker ? static_cast<uint8_t>(attacker->bTeam) : 255;
        outcome.victim_team = static_cast<uint8_t>(pVictim->bTeam);
        outcome.victim_died = (pVictim->stats.bLife - sLifeDeduct) <= 0 ? 1 : 0;
        outcome.reason = ubReason;
        outcome.damage = sLifeDeduct;
        outcome.breath = sBreathLoss;
        log.writeOutcome(outcome);
    }

    void OnStartNPCAI(SOLDIERTYPE* pSoldier)
    {
        EnsureApplied();
        if(!pSoldier || pSoldier->ubID >= TOTAL_SOLDIERS)
            return;
        g_stage[pSoldier->ubID] = 0;
        g_wasteStreak[pSoldier->ubID] = 0;
        if(!g_settings.exportSituations || !Instrumented(pSoldier) || !TurnBasedCombat())
            return;
        // the harness starts at RefreshAI, so the question is asked before it runs
        SituationSnapshot(pSoldier, 0, true);
        AIRandomRecordBegin();
    }

    void OnNeuralDecisionEnd(SOLDIERTYPE* pSoldier, bool fPolicyDecided)
    {
        if(!pSoldier)
            return;
        if(fPolicyDecided)
        {
            EndDecision(pSoldier, g_settings.mode == TACNN_MODE_EMBEDDED ? TACNN_SRC_EMBEDDED : TACNN_SRC_SIDECAR);
            // the situation file is a record of legacy answers; a snapshot waiting for one is dropped
            SituationDiscard();
            AIRandomRecordStop();
            CountStage(pSoldier);
        }
        // when the policy declined, LegacyDecisionScope inside LegacyAIPlan::execute has already written the record
    }

    BOOLEAN SaveNeuralState(HWFILE hFile)
    {
        std::vector<uint8_t> blob;
        GetBeliefStore().serialize(blob);
        UINT32 size = static_cast<UINT32>(blob.size());
        UINT32 written = 0;
        if(!FileWrite(hFile, &size, sizeof size, &written) || written != sizeof size)
            return FALSE;
        if(size == 0)
            return TRUE;
        if(!FileWrite(hFile, &blob[0], size, &written) || written != size)
            return FALSE;
        return TRUE;
    }

    BOOLEAN LoadNeuralState(HWFILE hFile, UINT32 uiSaveGameVersion)
    {
        EnsureApplied();
        AIRandomReset();
        g_inBattle = false;
        HarnessOnSavedGameLoaded();
        BeliefStore& store = GetBeliefStore();
        if(uiSaveGameVersion < BELIEF_STORE_IN_SAVE)
        {
            store.reset();
            return TRUE;
        }
        UINT32 size = 0;
        UINT32 read = 0;
        if(!FileRead(hFile, &size, sizeof size, &read) || read != sizeof size)
            return FALSE;
        if(size > 64u * 1024u * 1024u)
            return FALSE;
        std::vector<uint8_t> blob(size);
        if(size > 0 && (!FileRead(hFile, &blob[0], size, &read) || read != size))
            return FALSE;
        if(size == 0 || !store.deserialize(&blob[0], size))
            store.reset();
        return TRUE;
    }

    // ---------------------------------------------------------------------- legacy decision scope

    LegacyDecisionScope::LegacyDecisionScope(SOLDIERTYPE* pSoldier, bool fTurnBased)
        : soldier_(pSoldier), active_(false)
    {
        EnsureApplied();
        if(!fTurnBased || !Logged(pSoldier) || !TurnBasedCombat())
            return;
        active_ = true;
        const bool enemy = Instrumented(pSoldier);
        if(g_settings.log || (enemy && g_settings.cheatAudit))
            BeginDecision(pSoldier, enemy);
        if(enemy && g_settings.exportSituations && !SituationPendingFor(pSoldier))
        {
            // a further decision in the same turn: RefreshAI is behind us, say so
            SituationSnapshot(pSoldier, g_stage[pSoldier->ubID], false);
            AIRandomRecordBegin();
        }
    }

    LegacyDecisionScope::~LegacyDecisionScope()
    {
        if(!active_)
            return;
        if(SituationPendingFor(soldier_))
        {
            SituationComplete(soldier_, AIRandomRecorded(), AIRandomRecordedRanges());
            AIRandomRecordStop();
        }
        EndDecision(soldier_, TACNN_SRC_LEGACY);
        CountStage(soldier_);
    }
}
