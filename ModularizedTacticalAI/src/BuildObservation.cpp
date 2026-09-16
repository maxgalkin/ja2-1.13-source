/**
 * @file
 * @author ja2mod
 *
 * Added by the ja2mod fork on 2026-09-15; see CHANGES-ja2mod.md.
 *
 * Builds the observation of tools/tacsim/obs/spec.py from a FairInputs. This
 * file deliberately includes no engine header: everything it knows about the
 * world arrives through FairInputs and the TerrainQuery callbacks, which take
 * tiles and never soldiers. Keep it that way; the cheat audit of the feature
 * note depends on it.
 */

#include "../include/BuildObservation.h"

#include <math.h>
#include <string.h>

namespace tacnn
{
    namespace
    {
        const int R = TACNN_PATCH_RADIUS;
        const int N = TACNN_PATCH;

        inline uint8_t byteOf(int v)
        {
            if(v < 0) return 0;
            if(v > 255) return 255;
            return static_cast<uint8_t>(v);
        }

        inline uint8_t maxByte(uint8_t a, uint8_t b) { return a > b ? a : b; }

        inline float clamp01(float v)
        {
            if(v < 0.0f) return 0.0f;
            if(v > 1.0f) return 1.0f;
            return v;
        }

        inline int floorDiv(int a, int b)
        {
            int q = a / b;
            if((a % b != 0) && ((a < 0) != (b < 0)))
                --q;
            return q;
        }

        inline bool hasTile(int32_t g) { return g != NO_TILE; }

        /// index into the fine patch of a tile, or false if it lies outside
        bool fineCellOf(const BuildScratch& s, const TerrainQuery& t, int32_t self, int32_t gridno, int& row, int& col)
        {
            if(!hasTile(gridno) || !hasTile(self))
                return false;
            int sx, sy, gx, gy;
            TileXY(t, self, sx, sy);
            TileXY(t, gridno, gx, gy);
            return OffsetToCell(s.quadrant, gx - sx, gy - sy, 1, row, col);
        }

        void paintFine(uint8_t (&plane)[TACNN_PATCH][TACNN_PATCH], const BuildScratch& s, const TerrainQuery& t,
                       int32_t self, int32_t gridno, uint8_t value)
        {
            int row, col;
            if(fineCellOf(s, t, self, gridno, row, col))
                plane[row][col] = maxByte(plane[row][col], value);
        }

        void paintMedium(uint8_t (&plane)[TACNN_PATCH][TACNN_PATCH], const BuildScratch& s, const TerrainQuery& t,
                         int32_t self, int32_t gridno, uint8_t value)
        {
            if(!hasTile(gridno) || !hasTile(self))
                return;
            int sx, sy, gx, gy, row, col;
            TileXY(t, self, sx, sy);
            TileXY(t, gridno, gx, gy);
            if(OffsetToCell(s.quadrant, gx - sx, gy - sy, TACNN_MEDIUM_SCALE, row, col))
                plane[row][col] = maxByte(plane[row][col], value);
        }

        void paintGlobal(uint8_t (&plane)[TACNN_GLOBAL][TACNN_GLOBAL], const TerrainQuery& t, int32_t gridno, uint8_t value, bool add)
        {
            if(!hasTile(gridno))
                return;
            int x, y;
            TileXY(t, gridno, x, y);
            int col = x / TACNN_GLOBAL_TILES_PER_CELL;
            int row = y / TACNN_GLOBAL_TILES_PER_CELL;
            if(col < 0 || col >= TACNN_GLOBAL || row < 0 || row >= TACNN_GLOBAL)
                return;
            if(add)
                plane[row][col] = byteOf(plane[row][col] + value);
            else
                plane[row][col] = maxByte(plane[row][col], value);
        }

        /// the believed tile of an enemy: personal if any, else public
        int32_t believedTile(const BelievedEnemy& e, int8_t& level)
        {
            if(e.knowledge != KNOW_NOTHING && hasTile(e.gridno)) { level = e.level; return e.gridno; }
            if(e.publicKnowledge != KNOW_NOTHING && hasTile(e.publicGridno)) { level = e.publicLevel; return e.publicGridno; }
            if(hasTile(e.gridno)) { level = e.level; return e.gridno; }
            level = 0;
            return NO_TILE;
        }

        /// smaller is fresher: seen beats heard at equal age, then by turns
        int recencyRank(const BelievedEnemy& e)
        {
            int best = 100;
            const int8_t codes[2] = { e.knowledge, e.publicKnowledge };
            for(int i = 0; i < 2; ++i)
            {
                int8_t k = codes[i];
                if(k == KNOW_NOTHING)
                    continue;
                int rank = TurnsSince(k) * 2 + (k < 0 ? 1 : 0) + (i == 1 ? 10 : 0);
                if(rank < best)
                    best = rank;
            }
            return best;
        }

        void sortEnemies(const FairInputs& in, BuildScratch& s)
        {
            s.known = 0;
            int n = in.enemyCount;
            if(n > MAX_ENEMIES) n = MAX_ENEMIES;
            for(int i = 0; i < n; ++i)
                s.order[i] = i;
            const TerrainQuery& t = *in.terrain;
            // insertion sort; n is small
            for(int i = 1; i < n; ++i)
            {
                int idx = s.order[i];
                int j = i - 1;
                while(j >= 0)
                {
                    const BelievedEnemy& a = in.enemies[s.order[j]];
                    const BelievedEnemy& b = in.enemies[idx];
                    int ra = recencyRank(a), rb = recencyRank(b);
                    int8_t la, lb;
                    int32_t ga = believedTile(a, la), gb = believedTile(b, lb);
                    int da = hasTile(ga) ? TileDistance(t, in.self.gridno, ga) : 999;
                    int db = hasTile(gb) ? TileDistance(t, in.self.gridno, gb) : 999;
                    bool after = (ra > rb) || (ra == rb && da > db);
                    if(!after)
                        break;
                    s.order[j + 1] = s.order[j];
                    --j;
                }
                s.order[j + 1] = idx;
            }
            for(int i = 0; i < n; ++i)
            {
                int8_t level;
                if(hasTile(believedTile(in.enemies[s.order[i]], level)))
                    ++s.known;
                else
                    break;    // the sort puts enemies without a tile last
            }
        }

        // ------------------------------------------------------------------
        // the patches
        // ------------------------------------------------------------------

        struct TileChannels
        {
            uint8_t v[TACNN_PATCH_CHANNELS];
        };

        /// the terrain channels of one tile (0, 1, 4, 5, 14, 15) and its LOS-free extras
        void terrainChannels(const TerrainQuery& t, int32_t g, int8_t level, TileChannels& c)
        {
            memset(&c, 0, sizeof c);
            if(!hasTile(g))
                return;
            c.v[0] = t.passable(g);
            c.v[1] = byteOf(64 * t.structureHeight(g, level));
            c.v[4] = byteOf(t.groundHeight(g) + (t.roof(g) ? 128 : 0));
            uint8_t water = t.water(g);
            uint8_t gas = t.gasSmoke(g, level);
            c.v[5] = byteOf((water == 1 ? 64 : water == 2 ? 128 : 0) + ((gas & 6) ? 64 : 0));
            c.v[14] = t.corpse(g) ? 255 : 0;
            int kinds = ((gas & 1) ? 1 : 0) + ((gas & 2) ? 1 : 0) + ((gas & 4) ? 1 : 0);
            c.v[15] = byteOf(85 * kinds);
        }

        void coverAt(const TerrainQuery& t, const BuildScratch& s, int32_t g, int8_t level, uint8_t& sight, uint8_t& prone, uint8_t& exposed)
        {
            sight = prone = exposed = 0;
            if(!hasTile(g) || s.coverEnemies == 0)
                return;
            int noSight = 0, noProne = 0;
            for(int i = 0; i < s.coverEnemies; ++i)
            {
                bool seesStanding = t.standingAtSees(s.coverEnemyGridno[i], s.coverEnemyLevel[i], g, level, STANCE_STAND);
                bool seesProne = t.standingAtSees(s.coverEnemyGridno[i], s.coverEnemyLevel[i], g, level, STANCE_PRONE);
                if(!seesStanding) ++noSight;
                if(!seesProne) ++noProne;
                if(seesStanding) ++exposed;
            }
            sight = byteOf(255 * noSight / s.coverEnemies);
            prone = byteOf(255 * noProne / s.coverEnemies);
        }

        void buildFine(const FairInputs& in, TacnnObservation& obs, BuildScratch& s)
        {
            const TerrainQuery& t = *in.terrain;
            const int32_t self = in.self.gridno;
            for(int row = 0; row < N; ++row)
            {
                for(int col = 0; col < N; ++col)
                {
                    PatchCell& cell = s.fine[row][col];
                    int dx, dy;
                    CellToOffset(s.quadrant, row, col, 1, dx, dy);
                    int32_t g = OffsetTile(t, self, dx, dy);
                    cell.gridno = g;
                    cell.ap = -1;
                    cell.passable = cell.sightCover = cell.proneCover = cell.exposed = cell.occupied = 0;
                    if(!hasTile(g))
                        continue;
                    TileChannels c;
                    terrainChannels(t, g, in.self.level, c);
                    cell.passable = c.v[0];
                    cell.occupied = t.knownOccupied(g, in.self.level) ? 1 : 0;
                    cell.ap = t.apToReach(g, in.self.level);
                    if(cell.passable > 0 || g == self)
                        coverAt(t, s, g, in.self.level, cell.sightCover, cell.proneCover, cell.exposed);
                    c.v[2] = cell.sightCover;
                    c.v[3] = cell.proneCover;
                    c.v[6] = (g == self || t.selfSees(g, in.self.level)) ? 255 : 0;
                    for(int ch = 0; ch < TACNN_PATCH_CHANNELS; ++ch)
                        obs.fine[ch][row][col] = c.v[ch];
                }
            }
        }

        void buildMedium(const FairInputs& in, TacnnObservation& obs, BuildScratch& s)
        {
            const TerrainQuery& t = *in.terrain;
            const int32_t self = in.self.gridno;
            for(int row = 0; row < N; ++row)
            {
                for(int col = 0; col < N; ++col)
                {
                    TileChannels pooled;
                    memset(&pooled, 0, sizeof pooled);
                    int32_t first = NO_TILE;
                    const int u0 = (R - row) * TACNN_MEDIUM_SCALE;
                    const int v0 = (col - R) * TACNN_MEDIUM_SCALE;
                    for(int i = 0; i < TACNN_MEDIUM_SCALE; ++i)
                    {
                        for(int j = 0; j < TACNN_MEDIUM_SCALE; ++j)
                        {
                            int tx, ty;
                            AheadRightToOffset(s.quadrant, u0 + i, v0 + j, tx, ty);
                            int32_t g = OffsetTile(t, self, tx, ty);
                            if(!hasTile(g))
                                continue;
                            if(i == 0 && j == 0)
                                first = g;
                            TileChannels c;
                            terrainChannels(t, g, in.self.level, c);
                            for(int ch = 0; ch < TACNN_PATCH_CHANNELS; ++ch)
                                pooled.v[ch] = maxByte(pooled.v[ch], c.v[ch]);
                        }
                    }
                    if(hasTile(first))
                    {
                        uint8_t sight, prone, exposed;
                        if(t.passable(first) > 0 || first == self)
                            coverAt(t, s, first, in.self.level, sight, prone, exposed);
                        else
                            sight = prone = exposed = 0;
                        pooled.v[2] = sight;
                        pooled.v[3] = prone;
                        pooled.v[6] = (first == self || t.selfSees(first, in.self.level)) ? 255 : 0;
                    }
                    for(int ch = 0; ch < TACNN_PATCH_CHANNELS; ++ch)
                        obs.medium[ch][row][col] = pooled.v[ch];
                }
            }
        }

        /// channels 7-13 are painted from the lists rather than per tile
        void paintKnowledge(const FairInputs& in, TacnnObservation& obs, const BuildScratch& s)
        {
            const TerrainQuery& t = *in.terrain;
            const int32_t self = in.self.gridno;

            for(int i = 0; i < in.teamCount && i < MAX_TEAM; ++i)
            {
                const Teammate& m = in.team[i];
                paintFine(obs.fine[7], s, t, self, m.gridno, 255);
                paintMedium(obs.medium[7], s, t, self, m.gridno, 255);
                if(hasTile(m.lastTargetGridno))
                {
                    paintFine(obs.fine[8], s, t, self, m.lastTargetGridno, 255);
                    paintMedium(obs.medium[8], s, t, self, m.lastTargetGridno, 255);
                }
            }
            if(hasTile(in.self.lastTargetGridno))
            {
                paintFine(obs.fine[8], s, t, self, in.self.lastTargetGridno, 255);
                paintMedium(obs.medium[8], s, t, self, in.self.lastTargetGridno, 255);
            }

            for(int i = 0; i < in.enemyCount && i < MAX_ENEMIES; ++i)
            {
                const BelievedEnemy& e = in.enemies[i];
                if(e.knowledge != KNOW_NOTHING && hasTile(e.gridno))
                {
                    int ch = e.knowledge > 0 ? 9 : 10;
                    uint8_t heat = KnowledgeHeat(e.knowledge);
                    uint8_t age = byteOf(85 * TurnsSince(e.knowledge));
                    paintFine(obs.fine[ch], s, t, self, e.gridno, heat);
                    paintMedium(obs.medium[ch], s, t, self, e.gridno, heat);
                    paintFine(obs.fine[12], s, t, self, e.gridno, age);
                    paintMedium(obs.medium[12], s, t, self, e.gridno, age);
                }
                if(e.publicKnowledge != KNOW_NOTHING && hasTile(e.publicGridno))
                {
                    uint8_t heat = KnowledgeHeat(e.publicKnowledge);
                    uint8_t age = byteOf(85 * TurnsSince(e.publicKnowledge));
                    paintFine(obs.fine[11], s, t, self, e.publicGridno, heat);
                    paintMedium(obs.medium[11], s, t, self, e.publicGridno, heat);
                    paintFine(obs.fine[12], s, t, self, e.publicGridno, age);
                    paintMedium(obs.medium[12], s, t, self, e.publicGridno, age);
                }
            }

            for(int i = 0; i < in.noiseCount && i < MAX_NOISES; ++i)
            {
                const NoiseMemory& n = in.noises[i];
                uint8_t value = byteOf(16 * n.volume);
                paintFine(obs.fine[13], s, t, self, n.gridno, value);
                paintMedium(obs.medium[13], s, t, self, n.gridno, value);
            }
        }

        void buildGlobal(const FairInputs& in, TacnnObservation& obs)
        {
            const TerrainQuery& t = *in.terrain;
            const int cols = t.cols(), rows = t.rows();
            const int step = TACNN_GLOBAL_TILES_PER_CELL;
            for(int row = 0; row < TACNN_GLOBAL; ++row)
            {
                for(int col = 0; col < TACNN_GLOBAL; ++col)
                {
                    int passable = 0, open = 0, samples = 0;
                    for(int sy = 1; sy < step; sy += 4)
                    {
                        for(int sx = 1; sx < step; sx += 4)
                        {
                            int x = col * step + sx, y = row * step + sy;
                            if(x >= cols || y >= rows)
                                continue;
                            int32_t g = y * cols + x;
                            ++samples;
                            if(t.passable(g) > 0) ++passable;
                            if(t.structureHeight(g, 0) == 0) ++open;
                        }
                    }
                    obs.global_map[0][row][col] = samples ? byteOf(255 * passable / samples) : 0;
                    obs.global_map[1][row][col] = samples ? byteOf(255 * open / samples) : 0;
                }
            }
            paintGlobal(obs.global_map[2], t, in.self.gridno, 255, false);
            for(int i = 0; i < in.teamCount && i < MAX_TEAM; ++i)
                paintGlobal(obs.global_map[2], t, in.team[i].gridno, 64, true);
            for(int i = 0; i < in.enemyCount && i < MAX_ENEMIES; ++i)
            {
                const BelievedEnemy& e = in.enemies[i];
                if(e.knowledge != KNOW_NOTHING)
                    paintGlobal(obs.global_map[3], t, e.gridno, KnowledgeHeat(e.knowledge), false);
                if(e.publicKnowledge != KNOW_NOTHING)
                    paintGlobal(obs.global_map[3], t, e.publicGridno, KnowledgeHeat(e.publicKnowledge), false);
            }
            for(int i = 0; i < in.noiseCount && i < MAX_NOISES; ++i)
                paintGlobal(obs.global_map[4], t, in.noises[i].gridno, byteOf(16 * in.noises[i].volume), false);
            paintGlobal(obs.global_map[5], t, in.self.objectiveGridno, 255, false);
        }

        // ------------------------------------------------------------------
        // entities and self
        // ------------------------------------------------------------------

        enum
        {
            F_IS_SELF = 0, F_IS_TEAMMATE, F_IS_ENEMY, F_DX, F_DY, F_DISTANCE,
            F_BEARING_0, F_ON_ROOF = F_BEARING_0 + 8,
            F_KNOW_0, F_KNOW_PERSONAL = F_KNOW_0 + 10,
            F_LOS_NOW, F_CTH_SNAP, F_CTH_BEST, F_SHOT_AP,
            F_HP_FRAC, F_STANCE, F_GUN_NONE, F_GUN_PISTOL, F_GUN_RIFLE, F_GUN_HEAVY, F_GUN_RANGE,
            F_FIRED_AT_ME, F_TURNS_SINCE_LOS, F_WATCHED,
            F_COUNT
        };
        static_assert(F_COUNT == TACNN_ENTITY_DIMS, "entity feature enum matches the schema");

        void geometry(const TerrainQuery& t, int32_t self, int32_t other, float* f)
        {
            if(!hasTile(self) || !hasTile(other))
                return;
            int sx, sy, ox, oy;
            TileXY(t, self, sx, sy);
            TileXY(t, other, ox, oy);
            int dx = ox - sx, dy = oy - sy;
            f[F_DX] = dx / static_cast<float>(TACNN_PATCH_RADIUS * 4);
            f[F_DY] = dy / static_cast<float>(TACNN_PATCH_RADIUS * 4);
            f[F_DISTANCE] = clamp01(TileDistance(t, self, other) / 56.0f);
            if(dx != 0 || dy != 0)
                f[F_BEARING_0 + DirectionTo(dx, dy)] = 1.0f;
        }

        void gunOneHot(uint8_t gunClass, float* f)
        {
            switch(gunClass)
            {
                case GUN_PISTOL: f[F_GUN_PISTOL] = 1.0f; break;
                case GUN_RIFLE: f[F_GUN_RIFLE] = 1.0f; break;
                case GUN_HEAVY: f[F_GUN_HEAVY] = 1.0f; break;
                default: f[F_GUN_NONE] = 1.0f; break;
            }
        }

        void buildEntities(const FairInputs& in, TacnnObservation& obs, const BuildScratch& s)
        {
            const TerrainQuery& t = *in.terrain;
            int slot = 0;

            {
                float* f = obs.entities[slot];
                f[F_IS_SELF] = 1.0f;
                f[F_BEARING_0 + (in.self.facing & 7)] = 1.0f;
                f[F_ON_ROOF] = in.self.level > 0 ? 1.0f : 0.0f;
                f[F_KNOW_0 + (KNOW_SEEN_CURRENTLY + 4)] = 1.0f;
                f[F_KNOW_PERSONAL] = 1.0f;
                f[F_LOS_NOW] = 1.0f;
                f[F_HP_FRAC] = in.self.lifeMax > 0 ? clamp01(in.self.life / static_cast<float>(in.self.lifeMax)) : 0.0f;
                f[F_STANCE] = in.self.stance / 3.0f;
                gunOneHot(in.self.gunClass, f);
                f[F_GUN_RANGE] = clamp01(in.self.gunRangeTiles / 56.0f);
                obs.entity_mask[slot] = 1;
                obs.entity_id[slot] = in.self.id;
                ++slot;
            }

            int enemySlots = 0;
            for(int i = 0; i < in.enemyCount && i < MAX_ENEMIES && slot < TACNN_ENTITIES && enemySlots < 12; ++i)
            {
                const BelievedEnemy& e = in.enemies[s.order[i]];
                int8_t level;
                int32_t g = believedTile(e, level);
                if(!hasTile(g))
                    break;
                float* f = obs.entities[slot];
                f[F_IS_ENEMY] = 1.0f;
                geometry(t, in.self.gridno, g, f);
                f[F_ON_ROOF] = level > 0 ? 1.0f : 0.0f;
                bool personal = e.knowledge != KNOW_NOTHING
                    && (e.publicKnowledge == KNOW_NOTHING || TurnsSince(e.knowledge) <= TurnsSince(e.publicKnowledge));
                int8_t code = personal ? e.knowledge : e.publicKnowledge;
                f[F_KNOW_0 + (code + 4)] = 1.0f;
                f[F_KNOW_PERSONAL] = personal ? 1.0f : 0.0f;
                f[F_LOS_NOW] = e.losNow ? 1.0f : 0.0f;
                f[F_CTH_SNAP] = clamp01(e.cthSnap / 100.0f);
                f[F_CTH_BEST] = clamp01(e.cthBest / 100.0f);
                f[F_SHOT_AP] = clamp01(e.apShot / 100.0f);
                f[F_HP_FRAC] = e.seenLifeBand / 4.0f;
                f[F_STANCE] = e.seenStance / 3.0f;
                gunOneHot(e.seenGunClass, f);
                f[F_GUN_RANGE] = clamp01(e.seenGunRange / 56.0f);
                f[F_FIRED_AT_ME] = clamp01(e.attacksOnMe / 8.0f);
                f[F_TURNS_SINCE_LOS] = clamp01(e.turnsSinceLos / 3.0f);
                f[F_WATCHED] = e.watched ? 1.0f : 0.0f;
                obs.entity_mask[slot] = 1;
                obs.entity_id[slot] = e.id;
                ++slot;
                ++enemySlots;
            }

            // teammates, nearest first
            int order[MAX_TEAM];
            int n = in.teamCount < MAX_TEAM ? in.teamCount : MAX_TEAM;
            for(int i = 0; i < n; ++i) order[i] = i;
            for(int i = 1; i < n; ++i)
            {
                int idx = order[i], j = i - 1;
                int d = TileDistance(t, in.self.gridno, in.team[idx].gridno);
                while(j >= 0 && TileDistance(t, in.self.gridno, in.team[order[j]].gridno) > d)
                {
                    order[j + 1] = order[j];
                    --j;
                }
                order[j + 1] = idx;
            }
            for(int i = 0; i < n && slot < TACNN_ENTITIES; ++i)
            {
                const Teammate& m = in.team[order[i]];
                float* f = obs.entities[slot];
                f[F_IS_TEAMMATE] = 1.0f;
                geometry(t, in.self.gridno, m.gridno, f);
                f[F_ON_ROOF] = m.level > 0 ? 1.0f : 0.0f;
                f[F_KNOW_0 + (KNOW_SEEN_CURRENTLY + 4)] = 1.0f;
                f[F_KNOW_PERSONAL] = 1.0f;
                f[F_LOS_NOW] = hasTile(m.gridno) && t.selfSees(m.gridno, m.level) ? 1.0f : 0.0f;
                f[F_HP_FRAC] = m.lifeFrac / 255.0f;
                f[F_STANCE] = m.stance / 3.0f;
                gunOneHot(m.gunClass, f);
                f[F_FIRED_AT_ME] = m.underFire ? 1.0f : 0.0f;
                obs.entity_mask[slot] = 1;
                obs.entity_id[slot] = m.id;
                ++slot;
            }
            for(; slot < TACNN_ENTITIES; ++slot)
            {
                obs.entity_mask[slot] = 0;
                obs.entity_id[slot] = TACNN_NO_UNIT;
            }
        }

        enum
        {
            S_AP = 0, S_AP_FRAC, S_HP, S_HP_MAX, S_BREATH, S_BLEEDING,
            S_STAND, S_CROUCH, S_PRONE,
            S_FACE_0, S_ON_ROOF = S_FACE_0 + 8,
            S_MORALE_0, S_SHOCK = S_MORALE_0 + 5, S_COLLAPSED, S_BREATH_COLLAPSED,
            S_GUN_NONE, S_GUN_PISTOL, S_GUN_RIFLE, S_GUN_HEAVY, S_ROUNDS, S_MAGS,
            S_SCOPED, S_AUTOFIRE, S_BURST, S_GUN_RANGE, S_AP_READY, S_AP_RELOAD,
            S_GRENADES, S_SMOKE_GAS, S_MEDKITS, S_NVG, S_GAS_MASK, S_DETONATOR,
            S_ALERT_0, S_TURN = S_ALERT_0 + 4, S_TEAM_TURN_FRAC,
            S_ORDERS_0, S_COUNT = S_ORDERS_0 + 6
        };
        static_assert(S_COUNT == TACNN_SELF_DIMS, "self feature enum matches the schema");

        void buildSelf(const FairInputs& in, TacnnObservation& obs)
        {
            const SelfState& me = in.self;
            float* f = obs.self_vec;
            f[S_AP] = clamp01(me.ap / 100.0f);
            f[S_AP_FRAC] = me.apStart > 0 ? clamp01(me.ap / static_cast<float>(me.apStart)) : 0.0f;
            f[S_HP] = clamp01(me.life / 100.0f);
            f[S_HP_MAX] = clamp01(me.lifeMax / 100.0f);
            f[S_BREATH] = clamp01(me.breath / 100.0f);
            f[S_BLEEDING] = clamp01(me.bleeding / 20.0f);
            if(me.stance == STANCE_STAND) f[S_STAND] = 1.0f;
            else if(me.stance == STANCE_CROUCH) f[S_CROUCH] = 1.0f;
            else if(me.stance == STANCE_PRONE) f[S_PRONE] = 1.0f;
            f[S_FACE_0 + (me.facing & 7)] = 1.0f;
            f[S_ON_ROOF] = me.level > 0 ? 1.0f : 0.0f;
            int morale = me.morale < 0 ? 0 : me.morale > 4 ? 4 : me.morale;
            f[S_MORALE_0 + morale] = 1.0f;
            f[S_SHOCK] = clamp01(me.shock / 20.0f);
            f[S_COLLAPSED] = me.collapsed ? 1.0f : 0.0f;
            f[S_BREATH_COLLAPSED] = me.breathCollapsed ? 1.0f : 0.0f;
            switch(me.gunClass)
            {
                case GUN_PISTOL: f[S_GUN_PISTOL] = 1.0f; break;
                case GUN_RIFLE: f[S_GUN_RIFLE] = 1.0f; break;
                case GUN_HEAVY: f[S_GUN_HEAVY] = 1.0f; break;
                default: f[S_GUN_NONE] = 1.0f; break;
            }
            f[S_ROUNDS] = me.magazineSize > 0 ? clamp01(me.roundsInGun / static_cast<float>(me.magazineSize)) : 0.0f;
            f[S_MAGS] = clamp01(me.magazines / 6.0f);
            f[S_SCOPED] = me.scoped ? 1.0f : 0.0f;
            f[S_AUTOFIRE] = me.autofireCapable ? 1.0f : 0.0f;
            f[S_BURST] = me.burstCapable ? 1.0f : 0.0f;
            f[S_GUN_RANGE] = clamp01(me.gunRangeTiles / 56.0f);
            f[S_AP_READY] = clamp01(me.apReady / 100.0f);
            f[S_AP_RELOAD] = clamp01(me.apReload / 100.0f);
            f[S_GRENADES] = clamp01(me.grenades / 4.0f);
            f[S_SMOKE_GAS] = clamp01(me.smokeGrenades / 4.0f);
            f[S_MEDKITS] = clamp01(me.medkits / 2.0f);
            f[S_NVG] = me.nightVision ? 1.0f : 0.0f;
            f[S_GAS_MASK] = me.gasMask ? 1.0f : 0.0f;
            f[S_DETONATOR] = me.detonator ? 1.0f : 0.0f;
            f[S_ALERT_0 + (me.alert & 3)] = 1.0f;
            f[S_TURN] = clamp01(me.turn / 30.0f);
            f[S_TEAM_TURN_FRAC] = me.teamTotal > 0 ? clamp01(me.teamAlive / static_cast<float>(me.teamTotal)) : 0.0f;
            int orders = me.orders <= 4 ? me.orders : 5;
            f[S_ORDERS_0 + orders] = 1.0f;

            obs.role = me.role;
            obs.stage = me.stage;
            obs.facing = me.facing;
            obs.level = static_cast<uint8_t>(me.level > 0 ? me.level : 0);
            obs.gridno = me.gridno;

            int attitude = me.attitude < 6 ? me.attitude : 5;
            obs.personality[attitude] = 1.0f;
            obs.personality[6] = clamp01(me.experience / 10.0f);
            obs.personality[7] = clamp01(me.marksmanship / 100.0f);
        }

        void chooseCoverEnemies(const FairInputs& in, BuildScratch& s)
        {
            s.coverEnemies = 0;
            int gx = 0, gy = 0, n = 0;
            const TerrainQuery& t = *in.terrain;
            for(int i = 0; i < s.known && i < MAX_ENEMIES; ++i)
            {
                const BelievedEnemy& e = in.enemies[s.order[i]];
                int8_t level;
                int32_t g = believedTile(e, level);
                if(!hasTile(g))
                    continue;
                int x, y;
                TileXY(t, g, x, y);
                gx += x; gy += y; ++n;
                if(s.coverEnemies < MAX_COVER_ENEMIES)
                {
                    s.coverEnemyGridno[s.coverEnemies] = g;
                    s.coverEnemyLevel[s.coverEnemies] = level;
                    ++s.coverEnemies;
                }
            }
            s.centroidGridno = NO_TILE;
            if(n > 0)
            {
                int cx = gx / n, cy = gy / n;
                s.centroidGridno = cy * t.cols() + cx;
            }
        }
    }

    // ----------------------------------------------------------------------
    // geometry helpers
    // ----------------------------------------------------------------------

    int PatchQuadrant(uint8_t facing)
    {
        return ((facing + 1) / 2) & 3;
    }

    void AheadRightToOffset(int quadrant, int u, int v, int& dx, int& dy)
    {
        switch(quadrant & 3)
        {
            case 0: dx = v; dy = -u; break;
            case 1: dx = u; dy = v; break;
            case 2: dx = -v; dy = u; break;
            default: dx = -u; dy = -v; break;
        }
    }

    void CellToOffset(int quadrant, int row, int col, int scale, int& dx, int& dy)
    {
        AheadRightToOffset(quadrant, (R - row) * scale, (col - R) * scale, dx, dy);
    }

    bool OffsetToCell(int quadrant, int dx, int dy, int scale, int& row, int& col)
    {
        int u, v;
        switch(quadrant & 3)
        {
            case 0: v = dx; u = -dy; break;
            case 1: u = dx; v = dy; break;
            case 2: v = -dx; u = dy; break;
            default: u = -dx; v = -dy; break;
        }
        row = R - floorDiv(u, scale);
        col = R + floorDiv(v, scale);
        return row >= 0 && row < N && col >= 0 && col < N;
    }

    int32_t OffsetTile(const TerrainQuery& terrain, int32_t gridno, int dx, int dy)
    {
        if(!hasTile(gridno))
            return NO_TILE;
        int cols = terrain.cols(), rows = terrain.rows();
        int x = gridno % cols + dx;
        int y = gridno / cols + dy;
        if(x < 0 || y < 0 || x >= cols || y >= rows)
            return NO_TILE;
        return y * cols + x;
    }

    void TileXY(const TerrainQuery& terrain, int32_t gridno, int& x, int& y)
    {
        int cols = terrain.cols();
        x = gridno % cols;
        y = gridno / cols;
    }

    int TileDistance(const TerrainQuery& terrain, int32_t a, int32_t b)
    {
        if(!hasTile(a) || !hasTile(b))
            return 999;
        int ax, ay, bx, by;
        TileXY(terrain, a, ax, ay);
        TileXY(terrain, b, bx, by);
        int dx = ax - bx, dy = ay - by;
        return static_cast<int>(sqrt(static_cast<double>(dx * dx + dy * dy)));
    }

    int TileSpaces(const TerrainQuery& terrain, int32_t a, int32_t b)
    {
        if(!hasTile(a) || !hasTile(b))
            return 999;
        int ax, ay, bx, by;
        TileXY(terrain, a, ax, ay);
        TileXY(terrain, b, bx, by);
        int dx = ax > bx ? ax - bx : bx - ax;
        int dy = ay > by ? ay - by : by - ay;
        return dx > dy ? dx : dy;
    }

    int DirectionTo(int dx, int dy)
    {
        // 0 is north (dy < 0), turning clockwise; sectors of 45 degrees
        double angle = atan2(static_cast<double>(dx), static_cast<double>(-dy));   // 0 north, +pi/2 east
        int sector = static_cast<int>(floor(angle / (3.14159265358979323846 / 4.0) + 0.5));
        return ((sector % 8) + 8) % 8;
    }

    uint8_t KnowledgeHeat(int8_t knowledge)
    {
        switch(knowledge)
        {
            case KNOW_SEEN_CURRENTLY: return 255;
            case KNOW_SEEN_THIS_TURN: case KNOW_HEARD_THIS_TURN: return 200;
            case KNOW_SEEN_LAST_TURN: case KNOW_HEARD_LAST_TURN: return 150;
            case KNOW_SEEN_2_TURNS_AGO: case KNOW_HEARD_2_TURNS_AGO: return 100;
            case KNOW_SEEN_3_TURNS_AGO: case KNOW_HEARD_3_TURNS_AGO: return 50;
            default: return 0;
        }
    }

    uint8_t TurnsSince(int8_t knowledge)
    {
        switch(knowledge)
        {
            case KNOW_SEEN_CURRENTLY: case KNOW_SEEN_THIS_TURN: case KNOW_HEARD_THIS_TURN: return 0;
            case KNOW_SEEN_LAST_TURN: case KNOW_HEARD_LAST_TURN: return 1;
            case KNOW_SEEN_2_TURNS_AGO: case KNOW_HEARD_2_TURNS_AGO: return 2;
            case KNOW_SEEN_3_TURNS_AGO: case KNOW_HEARD_3_TURNS_AGO: return 3;
            default: return 3;
        }
    }

    // ----------------------------------------------------------------------
    // entry points
    // ----------------------------------------------------------------------

    void BuildObservation(const FairInputs& in, TacnnObservation& obs, BuildScratch& scratch)
    {
        memset(&obs, 0, sizeof obs);
        memset(&scratch, 0, sizeof scratch);
        scratch.quadrant = PatchQuadrant(in.self.facing);
        scratch.centroidGridno = NO_TILE;
        if(!in.terrain)
            return;
        sortEnemies(in, scratch);
        chooseCoverEnemies(in, scratch);
        buildFine(in, obs, scratch);
        buildMedium(in, obs, scratch);
        paintKnowledge(in, obs, scratch);
        buildGlobal(in, obs);
        buildEntities(in, obs, scratch);
        buildSelf(in, obs);
    }

    void BuildAll(const FairInputs& in, TacnnObservation& obs, TacnnCandidateSet& cand, TacnnActionMask& mask)
    {
        static BuildScratch scratch;
        BuildObservation(in, obs, scratch);
        ProposeCandidates(in, scratch, cand);
        BuildActionMask(in, cand, mask);
    }
}
