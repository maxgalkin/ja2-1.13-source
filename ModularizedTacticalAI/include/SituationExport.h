/**
 * @file
 * @author ja2mod
 *
 * Exporter of decision situations for the Python parity harness. Added by
 * the ja2mod fork on 2026-09-15; see CHANGES-ja2mod.md.
 *
 * When NEURAL_EXPORT_SITUATIONS is on, every turn-based legacy decision of
 * an enemy soldier is written as one JSON line in the format of
 * tools/tacsim/parity.py (`tools/tacsim/CONTRACT.md`, "The situation
 * file"): the whole sector as the engine sees it, the id of the deciding
 * soldier, the dice the decision rolled and the answer the legacy cascade
 * gave. The harness restores the record into its own port of the legacy AI
 * and compares the two answers.
 *
 * The question half is taken before the engine's RefreshAI for the first
 * decision of a soldier's turn (that is where the harness starts), and
 * right before the cascade for every further decision of the same turn;
 * those carry `stage` > 0 so the harness can skip its own refresh. Soldier
 * ids are renumbered densely in list order, because the harness numbers
 * soldiers by their position in the list; the engine id travels as
 * `engine_id`.
 *
 * This is a debugging dump and reads every soldier in the sector. It has
 * nothing to do with the observation the policy sees, which is built in
 * NeuralHooks.cpp from the fair inputs alone.
 */

#ifndef TACNN_SITUATION_EXPORT_H
#define TACNN_SITUATION_EXPORT_H

#include <stdint.h>
#include <string>
#include <vector>

class SOLDIERTYPE;

namespace tacnn
{
    /// Turn the exporter on with an output folder (relative to the working directory), or off.
    void SituationExportConfigure(const std::string& directory, bool enabled);
    bool SituationExportEnabled();

    /// Serialize the question half for this soldier now. Replaces any pending snapshot.
    void SituationSnapshot(SOLDIERTYPE* pSoldier, unsigned stage, bool beforeRefresh);
    /// True when a snapshot for this soldier is waiting for its answer.
    bool SituationPendingFor(const SOLDIERTYPE* pSoldier);
    /// Append the answer (the soldier's aiData action fields) and the rolls, write the line.
    void SituationComplete(SOLDIERTYPE* pSoldier, const std::vector<uint32_t>& rolls, const std::vector<uint32_t>& ranges);
    /// Forget a pending snapshot without writing it.
    void SituationDiscard();
    /// Lines written since the game started.
    unsigned SituationExported();
    /// The map name the harness loads, e.g. "c5" or "c5_b1"; lower case, no extension.
    std::string SituationMapName();
}

#endif
