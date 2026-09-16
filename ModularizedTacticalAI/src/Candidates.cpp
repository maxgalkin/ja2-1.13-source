/**
 * @file
 * @author ja2mod
 *
 * Added by the ja2mod fork on 2026-09-15; see CHANGES-ja2mod.md.
 *
 * The K = 48 hybrid candidate proposer of note 06 section 3.3 and the action
 * mask of section 3.1. Like BuildObservation.cpp this file includes nothing
 * from the engine: candidates are picked from the fine patch the observation
 * builder already scored and from the believed positions in FairInputs.
 */

#include "../include/BuildObservation.h"

#include <math.h>
#include <string.h>

namespace tacnn
{
    namespace
    {
        const int N = TACNN_PATCH;

        enum
        {
            C_KIND_UNIT = 0, C_KIND_COVER, C_KIND_ADVANCE, C_KIND_OTHER,
            C_DX, C_DY, C_DISTANCE, C_AP_COST, C_REACHABLE,
            C_COVER_ANY, C_COVER_SIGHT, C_COVER_PRONE, C_DANGER,
            C_ENEMIES_WITH_LOS, C_FRIENDS_NEAR, C_ENEMIES_NEAR, C_ON_ROOF,
            C_MY_REACH, C_THEIR_REACH,
            C_WATER_GAS, C_SMOKE, C_BOMB_NEAR, C_LIT,
            C_EXIT_DISTANCE, C_OBJECTIVE_DISTANCE, C_FLANK_ANGLE,
            C_LOS_TO_CENTROID, C_VALID,
            C_COUNT
        };
        static_assert(C_COUNT == TACNN_CANDIDATE_DIMS, "candidate feature enum matches the schema");

        inline float clamp01(float v)
        {
            if(v < 0.0f) return 0.0f;
            if(v > 1.0f) return 1.0f;
            return v;
        }

        inline bool hasTile(int32_t g) { return g != NO_TILE; }

        /// Chebyshev distance between two tiles given as coordinates (TileSpaces without the divisions).
        inline int spacesXY(int ax, int ay, int bx, int by)
        {
            int dx = ax > bx ? ax - bx : bx - ax;
            int dy = ay > by ? ay - by : by - ay;
            return dx > dy ? dx : dy;
        }

        /// Euclidean distance in whole tiles between two tiles given as coordinates (TileDistance likewise).
        inline int distanceXY(int ax, int ay, int bx, int by)
        {
            int dx = ax - bx, dy = ay - by;
            return static_cast<int>(sqrt(static_cast<double>(dx * dx + dy * dy)));
        }

        /**
         * Picks the tile candidates. The picking loops compare every fine
         * cell with every tile chosen so far, tens of thousands of distances
         * per decision, so the coordinates of the cells, the chosen tiles,
         * the believed enemies and the centroid are worked out once here and
         * the distances taken on those; the results are the ones TileSpaces
         * and TileDistance give.
         */
        struct Picker
        {
            const FairInputs& in;
            const BuildScratch& s;
            TacnnCandidateSet& out;
            int32_t chosen[TACNN_CANDIDATES];
            int chosenX[TACNN_CANDIDATES];
            int chosenY[TACNN_CANDIDATES];
            int chosenCount;
            int selfX, selfY;
            int cellX[N][N];                ///< coordinates of the fine cells; unset cells hold NO_TILE in the scratch
            int cellY[N][N];
            int enemyX[MAX_COVER_ENEMIES];
            int enemyY[MAX_COVER_ENEMIES];
            int centroidX, centroidY;

            Picker(const FairInputs& i, const BuildScratch& sc, TacnnCandidateSet& o)
                : in(i), s(sc), out(o), chosenCount(0), centroidX(0), centroidY(0)
            {
                const TerrainQuery& t = *in.terrain;
                TileXY(t, in.self.gridno, selfX, selfY);
                for(int row = 0; row < N; ++row)
                    for(int col = 0; col < N; ++col)
                    {
                        const int32_t g = s.fine[row][col].gridno;
                        if(hasTile(g))
                            TileXY(t, g, cellX[row][col], cellY[row][col]);
                        else
                            cellX[row][col] = cellY[row][col] = 0;
                    }
                for(int e = 0; e < s.coverEnemies && e < MAX_COVER_ENEMIES; ++e)
                    TileXY(t, s.coverEnemyGridno[e], enemyX[e], enemyY[e]);
                if(hasTile(s.centroidGridno))
                    TileXY(t, s.centroidGridno, centroidX, centroidY);
            }

            bool alreadyChosen(int32_t g) const
            {
                for(int i = 0; i < chosenCount; ++i)
                    if(chosen[i] == g)
                        return true;
                return false;
            }

            /// distance in tiles to the nearest tile already chosen (self counts)
            int spreadFrom(int32_t g) const
            {
                int x, y;
                TileXY(*in.terrain, g, x, y);
                return spreadFromXY(x, y);
            }

            int spreadFromXY(int x, int y) const
            {
                int best = spacesXY(selfX, selfY, x, y);
                for(int i = 0; i < chosenCount; ++i)
                {
                    int d = spacesXY(chosenX[i], chosenY[i], x, y);
                    if(d < best)
                        best = d;
                }
                return best;
            }

            /// distToNearestEnemy on coordinates
            int nearestEnemyFromXY(int x, int y) const
            {
                int best = 999;
                for(int i = 0; i < s.coverEnemies && i < MAX_COVER_ENEMIES; ++i)
                {
                    int d = distanceXY(x, y, enemyX[i], enemyY[i]);
                    if(d < best)
                        best = d;
                }
                return best;
            }

            void set(int slot, uint8_t kind, int32_t g, int8_t level, uint8_t unit)
            {
                out.kind[slot] = kind;
                out.gridno[slot] = g;
                out.level[slot] = level;
                out.unit_id[slot] = unit;
                if(hasTile(g) && chosenCount < TACNN_CANDIDATES)
                {
                    TileXY(*in.terrain, g, chosenX[chosenCount], chosenY[chosenCount]);
                    chosen[chosenCount++] = g;
                }
            }

            /// the cell of the fine patch that holds the tile, or null
            const PatchCell* cellOf(int32_t g) const
            {
                if(!hasTile(g))
                    return 0;
                int sx, sy, gx, gy, row, col;
                TileXY(*in.terrain, in.self.gridno, sx, sy);
                TileXY(*in.terrain, g, gx, gy);
                if(!OffsetToCell(s.quadrant, gx - sx, gy - sy, 1, row, col))
                    return 0;
                return &s.fine[row][col];
            }

            bool standable(const PatchCell& c) const
            {
                return hasTile(c.gridno) && c.gridno != in.self.gridno && c.passable > 0 && c.ap >= 0
                    && !c.occupied && c.gridno != in.self.blackListGridno;
            }

            /// fill `count` slots from `first` with the best-scoring standable cells, kept `minSpread` apart;
            /// the score sees the cell and its coordinates
            template<typename Score>
            void pickCells(int first, int count, uint8_t kind, int minSpread, Score score)
            {
                for(int k = 0; k < count; ++k)
                {
                    const PatchCell* best = 0;
                    long bestScore = -1000000L;
                    for(int row = 0; row < N; ++row)
                    {
                        for(int col = 0; col < N; ++col)
                        {
                            const PatchCell& c = s.fine[row][col];
                            if(!standable(c) || alreadyChosen(c.gridno))
                                continue;
                            const int cx = cellX[row][col], cy = cellY[row][col];
                            if(spreadFromXY(cx, cy) < minSpread)
                                continue;
                            long value = score(c, cx, cy);
                            if(value <= -1000000L)
                                continue;
                            if(value > bestScore)
                            {
                                bestScore = value;
                                best = &c;
                            }
                        }
                    }
                    if(!best)
                        break;
                    set(first + k, kind, best->gridno, in.self.level, TACNN_NO_UNIT);
                }
            }
        };

        int distToNearestEnemy(const FairInputs& in, const BuildScratch& s, int32_t g)
        {
            int best = 999;
            for(int i = 0; i < s.coverEnemies; ++i)
            {
                int d = TileDistance(*in.terrain, g, s.coverEnemyGridno[i]);
                if(d < best)
                    best = d;
            }
            return best;
        }

        void features(const FairInputs& in, const BuildScratch& s, const Picker& p, int slot, TacnnCandidateSet& out)
        {
            float* f = out.features[slot];
            memset(f, 0, sizeof(float) * TACNN_CANDIDATE_DIMS);
            out.ap_cost[slot] = 255;
            const int32_t g = out.gridno[slot];
            const uint8_t kind = out.kind[slot];
            if(kind == TACNN_CAND_EMPTY || !hasTile(g))
                return;
            const TerrainQuery& t = *in.terrain;

            switch(kind)
            {
                case TACNN_CAND_ENEMY: case TACNN_CAND_TEAMMATE: f[C_KIND_UNIT] = 1.0f; break;
                case TACNN_CAND_COVER: case TACNN_CAND_RETREAT: f[C_KIND_COVER] = 1.0f; break;
                case TACNN_CAND_ADVANCE: case TACNN_CAND_FLANK: f[C_KIND_ADVANCE] = 1.0f; break;
                default: f[C_KIND_OTHER] = 1.0f; break;
            }

            int sx, sy, gx, gy;
            TileXY(t, in.self.gridno, sx, sy);
            TileXY(t, g, gx, gy);
            f[C_DX] = (gx - sx) / static_cast<float>(TACNN_PATCH_RADIUS * 4);
            f[C_DY] = (gy - sy) / static_cast<float>(TACNN_PATCH_RADIUS * 4);
            f[C_DISTANCE] = clamp01(TileDistance(t, in.self.gridno, g) / 56.0f);

            const PatchCell* c = p.cellOf(g);
            int16_t ap = c ? c->ap : (g == in.self.gridno ? 0 : -1);
            if(g == in.self.gridno)
                ap = 0;
            f[C_AP_COST] = ap >= 0 ? clamp01(ap / 100.0f) : 1.0f;
            f[C_REACHABLE] = ap >= 0 ? 1.0f : 0.0f;
            out.ap_cost[slot] = ap >= 0 ? (ap > 254 ? 254 : static_cast<uint8_t>(ap)) : 255;
            if(c)
            {
                f[C_COVER_SIGHT] = c->sightCover / 255.0f;
                f[C_COVER_PRONE] = c->proneCover / 255.0f;
                f[C_COVER_ANY] = (c->sightCover > c->proneCover ? c->sightCover : c->proneCover) / 255.0f;
                f[C_DANGER] = s.coverEnemies > 0 ? c->exposed / static_cast<float>(s.coverEnemies) : 0.0f;
                f[C_ENEMIES_WITH_LOS] = clamp01(c->exposed / 8.0f);
            }

            int friends = 0;
            for(int i = 0; i < in.teamCount && i < MAX_TEAM; ++i)
                if(TileSpaces(t, g, in.team[i].gridno) <= 3)
                    ++friends;
            f[C_FRIENDS_NEAR] = clamp01(friends / 8.0f);
            int enemies = 0;
            for(int i = 0; i < s.known && i < MAX_ENEMIES; ++i)
            {
                const BelievedEnemy& e = in.enemies[s.order[i]];
                int32_t eg = hasTile(e.gridno) ? e.gridno : e.publicGridno;
                if(hasTile(eg) && TileSpaces(t, g, eg) <= 5)
                    ++enemies;
            }
            f[C_ENEMIES_NEAR] = clamp01(enemies / 8.0f);
            f[C_ON_ROOF] = out.level[slot] > 0 ? 1.0f : 0.0f;

            // the Python port evaluates 1 - d / range in double and rounds once on the
            // store; two float roundings would differ from it by an ulp (d = 1, range = 3)
            int nearest = distToNearestEnemy(in, s, g);
            if(in.self.gunRangeTiles > 0 && nearest < 999)
                f[C_MY_REACH] = clamp01(static_cast<float>(1.0 - nearest / static_cast<double>(in.self.gunRangeTiles)));
            float their = 0.0f;
            for(int i = 0; i < s.known && i < MAX_ENEMIES; ++i)
            {
                const BelievedEnemy& e = in.enemies[s.order[i]];
                int32_t eg = hasTile(e.gridno) ? e.gridno : e.publicGridno;
                if(!hasTile(eg) || e.seenGunRange == 0)
                    continue;
                float reach = clamp01(static_cast<float>(1.0 - TileDistance(t, g, eg) / static_cast<double>(e.seenGunRange)));
                if(reach > their)
                    their = reach;
            }
            f[C_THEIR_REACH] = their;

            uint8_t water = t.water(g);
            uint8_t gas = t.gasSmoke(g, out.level[slot]);
            f[C_WATER_GAS] = (water ? 0.5f : 0.0f) + ((gas & 6) ? 0.5f : 0.0f);
            f[C_SMOKE] = (gas & 1) ? 1.0f : 0.0f;
            f[C_BOMB_NEAR] = t.bombNear(g) ? 1.0f : 0.0f;
            f[C_LIT] = t.lit(g, out.level[slot]) ? 1.0f : 0.0f;

            int edge = gx < gy ? gx : gy;
            if(t.cols() - 1 - gx < edge) edge = t.cols() - 1 - gx;
            if(t.rows() - 1 - gy < edge) edge = t.rows() - 1 - gy;
            f[C_EXIT_DISTANCE] = clamp01(edge / 80.0f);
            f[C_OBJECTIVE_DISTANCE] = hasTile(in.self.objectiveGridno)
                ? clamp01(TileDistance(t, g, in.self.objectiveGridno) / 56.0f) : 1.0f;

            if(hasTile(s.centroidGridno))
            {
                int cx, cy;
                TileXY(t, s.centroidGridno, cx, cy);
                double a1 = atan2(static_cast<double>(cy - sy), static_cast<double>(cx - sx));
                double a2 = atan2(static_cast<double>(cy - gy), static_cast<double>(cx - gx));
                double d = fabs(a1 - a2);
                if(d > 3.14159265358979323846) d = 2 * 3.14159265358979323846 - d;
                f[C_FLANK_ANGLE] = (g == s.centroidGridno) ? 0.0f : clamp01(static_cast<float>(d / 3.14159265358979323846));
                f[C_LOS_TO_CENTROID] = t.standingAtSees(g, out.level[slot], s.centroidGridno, s.coverEnemyLevel[0], STANCE_STAND) ? 1.0f : 0.0f;
            }
            f[C_VALID] = 1.0f;
        }
    }

    void ProposeCandidates(const FairInputs& in, const BuildScratch& s, TacnnCandidateSet& cand)
    {
        memset(&cand, 0, sizeof cand);
        for(int k = 0; k < TACNN_CANDIDATES; ++k)
        {
            cand.gridno[k] = NO_TILE;
            cand.unit_id[k] = TACNN_NO_UNIT;
            cand.ap_cost[k] = 255;
        }
        if(!in.terrain || !hasTile(in.self.gridno))
            return;

        Picker p(in, s, cand);
        const TerrainQuery& t = *in.terrain;
        const int32_t self = in.self.gridno;

        // 0-11 known enemies, freshest first
        for(int i = 0; i < s.known && i < 12; ++i)
        {
            const BelievedEnemy& e = in.enemies[s.order[i]];
            int32_t g = hasTile(e.gridno) && e.knowledge != KNOW_NOTHING ? e.gridno : e.publicGridno;
            int8_t level = hasTile(e.gridno) && e.knowledge != KNOW_NOTHING ? e.level : e.publicLevel;
            if(!hasTile(g)) { g = e.gridno; level = e.level; }
            p.set(i, TACNN_CAND_ENEMY, g, level, e.id);
        }

        // 12-15 nearest teammates
        {
            int used[MAX_TEAM];
            memset(used, 0, sizeof used);
            for(int k = 0; k < 4; ++k)
            {
                int best = -1, bestD = 100000;
                for(int i = 0; i < in.teamCount && i < MAX_TEAM; ++i)
                {
                    if(used[i] || !hasTile(in.team[i].gridno))
                        continue;
                    int d = TileDistance(t, self, in.team[i].gridno);
                    if(d < bestD) { bestD = d; best = i; }
                }
                if(best < 0)
                    break;
                used[best] = 1;
                p.set(12 + k, TACNN_CAND_TEAMMATE, in.team[best].gridno, in.team[best].level, in.team[best].id);
            }
        }

        const bool enemiesKnown = s.coverEnemies > 0;
        const int distNow = enemiesKnown ? distToNearestEnemy(in, s, self) : 999;
        const int centroidNow = hasTile(s.centroidGridno) ? TileDistance(t, self, s.centroidGridno) : 999;

        // 16-23 cover: hide from the believed enemies, cheaply
        p.pickCells(16, 8, TACNN_CAND_COVER, 2, [&](const PatchCell& c, int, int) -> long {
            if(!enemiesKnown)
                return static_cast<long>(c.passable) / 8 - c.ap;       // no one to hide from: nearby standable tiles
            return 2L * c.proneCover + c.sightCover - 40L * c.exposed - c.ap / 4;
        });

        // 24-29 advance: closer to the nearest believed enemy, not naked
        if(enemiesKnown)
        {
            p.pickCells(24, 6, TACNN_CAND_ADVANCE, 2, [&](const PatchCell& c, int cx, int cy) -> long {
                int gain = distNow - p.nearestEnemyFromXY(cx, cy);
                if(gain < 2)
                    return -1000000L;
                return 10L * gain + c.sightCover / 4 - 20L * c.exposed;
            });
        }

        // 30-33 retreat: away from the enemy centroid, out of sight
        if(hasTile(s.centroidGridno))
        {
            p.pickCells(30, 4, TACNN_CAND_RETREAT, 2, [&](const PatchCell& c, int cx, int cy) -> long {
                int gain = distanceXY(cx, cy, p.centroidX, p.centroidY) - centroidNow;
                if(gain < 1)
                    return -1000000L;
                return 10L * gain + c.sightCover / 2 + c.proneCover / 2 - 30L * c.exposed;
            });
        }

        // 34-37 noises and heard-only enemies
        {
            int k = 0;
            for(int i = 0; i < in.noiseCount && i < MAX_NOISES && k < 4; ++i)
            {
                if(!hasTile(in.noises[i].gridno) || p.alreadyChosen(in.noises[i].gridno))
                    continue;
                p.set(34 + k, TACNN_CAND_NOISE, in.noises[i].gridno, in.noises[i].level, TACNN_NO_UNIT);
                ++k;
            }
            for(int i = 0; i < in.enemyCount && i < MAX_ENEMIES && k < 4; ++i)
            {
                const BelievedEnemy& e = in.enemies[i];
                if(e.knowledge >= KNOW_NOTHING || !hasTile(e.gridno) || p.alreadyChosen(e.gridno))
                    continue;
                p.set(34 + k, TACNN_CAND_NOISE, e.gridno, e.level, e.id);
                ++k;
            }
        }

        // 38-43 farthest-point samples of the reachable set
        p.pickCells(38, 6, TACNN_CAND_SAMPLE, 1, [&](const PatchCell& c, int cx, int cy) -> long {
            return static_cast<long>(p.spreadFromXY(cx, cy)) * 100 - c.ap / 10;
        });

        // 44-45 watched locations
        for(int i = 0, k = 0; i < in.watchedCount && i < MAX_WATCHED && k < 2; ++i)
        {
            if(!hasTile(in.watched[i]))
                continue;
            p.set(44 + k, TACNN_CAND_WATCHED, in.watched[i], in.watchedLevel[i], TACNN_NO_UNIT);
            ++k;
        }

        // 46 here, 47 the objective
        p.set(46, TACNN_CAND_SELF, self, in.self.level, in.self.id);
        if(hasTile(in.self.objectiveGridno))
            p.set(47, TACNN_CAND_OBJECTIVE, in.self.objectiveGridno, in.self.objectiveLevel, TACNN_NO_UNIT);

        for(int k = 0; k < TACNN_CANDIDATES; ++k)
            features(in, s, p, k, cand);
    }

    void BuildActionMask(const FairInputs& in, const TacnnCandidateSet& cand, TacnnActionMask& mask)
    {
        memset(&mask, 0, sizeof mask);
        const SelfState& me = in.self;

        bool anyReachable = false, anyCover = false, anyAdvance = false, anyRetreat = false;
        bool anyNoise = false, anyTeammate = false, anyEnemy = false, anySeen = false, adjacentSeen = false;
        bool woundedTeammateAdjacent = false;
        for(int k = 0; k < TACNN_CANDIDATES; ++k)
        {
            mask.target[k] = cand.kind[k] != TACNN_CAND_EMPTY ? 1 : 0;
            bool reachable = cand.ap_cost[k] != 255 && cand.ap_cost[k] <= me.ap;
            switch(cand.kind[k])
            {
                case TACNN_CAND_COVER: anyCover |= reachable; anyReachable |= reachable; break;
                case TACNN_CAND_ADVANCE: anyAdvance |= reachable; anyReachable |= reachable; break;
                case TACNN_CAND_RETREAT: anyRetreat |= reachable; anyReachable |= reachable; break;
                case TACNN_CAND_SAMPLE: anyReachable |= reachable; break;
                case TACNN_CAND_NOISE: anyNoise = true; break;
                case TACNN_CAND_TEAMMATE:
                    // a teammate standing next to the soldier is already reached, so there is
                    // nothing to seek there and the slot is closed (tools/tacsim/candidates.py)
                    if(in.terrain && TileSpaces(*in.terrain, me.gridno, cand.gridno[k]) <= 1)
                        mask.target[k] = 0;
                    else
                        anyTeammate = true;
                    break;
                case TACNN_CAND_ENEMY: anyEnemy = true; break;
                default: break;
            }
        }
        for(int i = 0; i < in.enemyCount && i < MAX_ENEMIES; ++i)
        {
            const BelievedEnemy& e = in.enemies[i];
            if(e.losNow)
            {
                anySeen = true;
                if(in.terrain && TileSpaces(*in.terrain, me.gridno, e.gridno) <= 1)
                    adjacentSeen = true;
            }
        }
        for(int i = 0; i < in.teamCount && i < MAX_TEAM; ++i)
            if(in.team[i].wounded && in.terrain && TileSpaces(*in.terrain, me.gridno, in.team[i].gridno) <= 1)
                woundedTeammateAdjacent = true;

        const bool hasGun = me.gunClass != GUN_NONE;
        const bool canMove = anyReachable && me.ap >= me.apMinMove && !me.collapsed;
        const bool mobile = !me.collapsed && me.ap > 0;

        mask.type[TACNN_ACT_END_TURN] = 1;
        mask.type[TACNN_ACT_HOLD] = 1;
        mask.type[TACNN_ACT_MOVE_RUN] = canMove;
        mask.type[TACNN_ACT_MOVE_WALK] = canMove;
        mask.type[TACNN_ACT_MOVE_SWAT] = canMove;
        mask.type[TACNN_ACT_TAKE_COVER] = canMove && anyCover;
        // moves aimed at a unit go through the legacy walk-towards helper, which will not take a
        // step that leaves fewer than its reserve behind
        const bool canSeek = mobile && me.ap >= me.apMinMove + me.apSeekReserve;
        mask.type[TACNN_ACT_SEEK_OPPONENT] = canSeek && anyEnemy;
        mask.type[TACNN_ACT_SEEK_NOISE] = mobile && anyNoise && me.ap >= me.apMinMove;
        mask.type[TACNN_ACT_SEEK_FRIEND] = canSeek && anyTeammate;
        mask.type[TACNN_ACT_FLANK_LEFT] = canSeek && anyEnemy && anyReachable && me.ap >= 2 * me.apMinMove;
        mask.type[TACNN_ACT_FLANK_RIGHT] = mask.type[TACNN_ACT_FLANK_LEFT];
        mask.type[TACNN_ACT_WITHDRAW] = canMove && anyRetreat;
        mask.type[TACNN_ACT_RUN_AWAY] = canMove && anyRetreat;
        mask.type[TACNN_ACT_FIRE_GUN] = hasGun && me.roundsInGun > 0 && anySeen && me.ap >= me.apShoot && !me.collapsed;
        mask.type[TACNN_ACT_SUPPRESS] = hasGun && (me.burstCapable || me.autofireCapable) && me.roundsInGun >= 3
            && anyEnemy && me.ap >= me.apBurst && !me.collapsed;
        mask.type[TACNN_ACT_TOSS_PROJECTILE] = (me.grenades > 0 || me.smokeGrenades > 0 || me.launcher)
            && anyEnemy && me.ap >= me.apThrow && !me.collapsed;
        mask.type[TACNN_ACT_THROW_KNIFE] = me.throwingKnife && anySeen && me.ap >= me.apThrow && !me.collapsed;
        mask.type[TACNN_ACT_KNIFE_STAB] = me.knife && adjacentSeen && me.ap >= me.apShoot && !me.collapsed;
        mask.type[TACNN_ACT_RELOAD] = hasGun && me.magazines > 0 && me.roundsInGun < me.magazineSize && me.ap >= me.apReload;
        {
            int16_t cheapest = me.apStand;
            if(me.apCrouch < cheapest) cheapest = me.apCrouch;
            if(me.apProne < cheapest) cheapest = me.apProne;
            mask.type[TACNN_ACT_CHANGE_STANCE] = !me.collapsed && me.ap >= cheapest;
        }
        mask.type[TACNN_ACT_CHANGE_FACING] = me.ap >= 1 && !me.collapsed;
        mask.type[TACNN_ACT_RED_ALERT] = me.alert < ALERT_RED && (anyEnemy || anyNoise);
        mask.type[TACNN_ACT_DOCTOR] = me.medkits > 0 && woundedTeammateAdjacent && me.ap >= me.apShoot;
        mask.type[TACNN_ACT_CLIMB] = me.canClimb && me.ap >= me.apClimb && !me.collapsed;

        for(int a = 0; a < TACNN_AIM_LEVELS; ++a)
            mask.aim[a] = (a <= me.maxAimClicks && me.ap >= me.apShoot + a) ? 1 : 0;
        if(!mask.aim[0])
            mask.aim[0] = 1;   // the un-aimed shot is always the fallback parameter
        mask.mode[0] = 1;
        mask.mode[1] = me.burstCapable && me.roundsInGun >= 2 && me.ap >= me.apBurst;
        mask.mode[2] = me.autofireCapable && me.roundsInGun >= 3 && me.ap >= me.apAuto;
        // the stance head only serves CHANGE_STANCE, so the stance the soldier is already in is not a choice
        mask.stance[0] = me.stance != STANCE_STAND && me.ap >= me.apStand;
        mask.stance[1] = me.stance != STANCE_CROUCH && me.ap >= me.apCrouch;
        mask.stance[2] = me.stance != STANCE_PRONE && me.ap >= me.apProne;
        if(me.collapsed)
        {
            mask.stance[0] = mask.stance[1] = 0;
            mask.stance[2] = me.stance != STANCE_PRONE;
        }
        const bool anyStance = mask.stance[0] || mask.stance[1] || mask.stance[2];
        mask.type[TACNN_ACT_CHANGE_STANCE] = !me.collapsed && anyStance;
        if(!anyStance)
            mask.stance[0] = 1;   // the head always has a legal parameter, even when the type is closed
    }
}
