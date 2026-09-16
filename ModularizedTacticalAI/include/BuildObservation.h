/**
 * @file
 * @author ja2mod
 *
 * The observation builder and the candidate proposer. Added by the ja2mod
 * fork on 2026-09-15; see CHANGES-ja2mod.md.
 *
 * Both are pure functions of a FairInputs: they include nothing from the
 * engine, so a translation unit that compiles them cannot reach an
 * opponent's SOLDIERTYPE. The layout they write is the generated
 * tacnn_schema.h; the meaning of every number is documented in
 * tools/tacsim/obs/spec.py of the ja2mod repository, which the Python side
 * follows.
 */

#ifndef TACNN_BUILD_OBSERVATION_H
#define TACNN_BUILD_OBSERVATION_H

#include "FairInputs.h"
#include "tacnn_schema.h"

namespace tacnn
{
    enum { MAX_COVER_ENEMIES = 4 };

    /// One tile of the fine patch, kept for the candidate proposer.
    struct PatchCell
    {
        int32_t gridno;         ///< NO_TILE when off the map
        int16_t ap;             ///< action points to get there, -1 unreachable
        uint8_t passable;       ///< channel 0 value
        uint8_t sightCover;     ///< channel 2 value
        uint8_t proneCover;     ///< channel 3 value
        uint8_t exposed;        ///< believed enemies with line of sight to a standing figure here
        uint8_t occupied;       ///< a known soldier stands here
    };

    /// What BuildObservation computed that ProposeCandidates reuses.
    struct BuildScratch
    {
        PatchCell fine[TACNN_PATCH][TACNN_PATCH];
        int coverEnemies;                           ///< how many believed enemies the cover channels used
        int32_t coverEnemyGridno[MAX_COVER_ENEMIES];
        int8_t coverEnemyLevel[MAX_COVER_ENEMIES];
        int order[MAX_ENEMIES];                     ///< enemy indices, freshest knowledge first
        int known;                                  ///< how many of `order` have a believed tile
        int32_t centroidGridno;                     ///< mean believed enemy position, NO_TILE if none
        int quadrant;                               ///< patch rotation, 0..3
    };

    // geometry helpers shared with the proposer and the glue
    int PatchQuadrant(uint8_t facing);
    void AheadRightToOffset(int quadrant, int u, int v, int& dx, int& dy);
    void CellToOffset(int quadrant, int row, int col, int scale, int& dx, int& dy);
    bool OffsetToCell(int quadrant, int dx, int dy, int scale, int& row, int& col);
    int32_t OffsetTile(const TerrainQuery& terrain, int32_t gridno, int dx, int dy);
    void TileXY(const TerrainQuery& terrain, int32_t gridno, int& x, int& y);
    int TileDistance(const TerrainQuery& terrain, int32_t a, int32_t b);       ///< engine PythSpacesAway
    int TileSpaces(const TerrainQuery& terrain, int32_t a, int32_t b);         ///< engine SpacesAway
    int DirectionTo(int dx, int dy);                                           ///< 0 north .. 7, clockwise
    uint8_t KnowledgeHeat(int8_t knowledge);
    uint8_t TurnsSince(int8_t knowledge);

    void BuildObservation(const FairInputs& in, TacnnObservation& obs, BuildScratch& scratch);
    void ProposeCandidates(const FairInputs& in, const BuildScratch& scratch, TacnnCandidateSet& cand);
    void BuildActionMask(const FairInputs& in, const TacnnCandidateSet& cand, TacnnActionMask& mask);

    /// The three above in order, with a private scratch.
    void BuildAll(const FairInputs& in, TacnnObservation& obs, TacnnCandidateSet& cand, TacnnActionMask& mask);
}

#endif
