/**
 * @file
 * @author ja2mod
 *
 * The engine-facing side of the neural AI instrumentation. Added by the
 * ja2mod fork on 2026-09-15; see CHANGES-ja2mod.md.
 *
 * Everything the engine calls is declared here and implemented in
 * NeuralHooks.cpp, which is the one translation unit allowed to read both
 * the engine's SOLDIERTYPE and the fair-input structures of FairInputs.h.
 * It records what a soldier perceives into the belief store, fills the
 * FairInputs a decision is built from, asks the sidecar, writes the decision
 * log and drives the AI random stream. Every hook is a no-op unless the
 * matching NEURAL_* key in Ja2_Options.INI turns it on, so a stock INI gives
 * stock behaviour.
 */

#ifndef TACNN_NEURAL_HOOKS_H
#define TACNN_NEURAL_HOOKS_H

#include <string>
#include "../../sgp/types.h"

class SOLDIERTYPE;

namespace tacnn
{
    /// Read gGameExternalOptions: open or close the log, the sidecar, the exporter, set the seed.
    /// Called lazily by every hook and again when a battle starts, so an edited INI is picked up.
    void ApplySettings();

    /// Battle lifecycle: EnterCombatMode / ExitCombatMode in Tactical/Overhead.cpp.
    void OnEnterCombatMode();
    void OnExitCombatMode();
    /// A save loaded in the middle of a battle never passed EnterCombatMode; open the episode now (battle harness).
    void EnsureBattleOpen();
    /// The battle harness ends a battle the engine did not (turn cap, timeout, stalemate, a side wiped out
    /// without ExitCombatMode): write the episode_end record with its verdict (a TACNN_OUTCOME_* value), so
    /// every logged battle carries its end. Nothing happens when no episode is open.
    void CloseEpisode(UINT8 ubOutcome);
    /// Running totals since the process started, for the battle harness: legacy decisions recorded,
    /// damage the enemy team dealt and took.  They never reset; read them as differences.
    unsigned DecisionsRecorded();
    int DamageDealtRecorded();
    int DamageTakenRecorded();
    /// Where the embedded decisions of the current battle spent their time, one printable line
    /// ("... fair 120/900 obs 3100/9000 ..." mean/max microseconds per phase); empty when none ran.
    /// The battle harness writes it into harness.log; ExitCombatMode logs it to game_log.log.
    std::string DecisionProfileSummary();
    /// A team's turn begins (Tactical/TeamTurns.cpp). The player's turn advances the belief store's turn counter.
    void OnBeginTeamTurn(UINT8 ubTeam);
    /// Tactical/opplist.cpp InitOpponentKnowledgeSystem: a new sector, forget everything.
    void OnInitOpponentKnowledge();
    /// Something that bends sight lines changed: a structure was added, swapped (doors) or destroyed,
    /// an explosion spread smoke, gas or light, a light came or went (TileEngine/structure.cpp,
    /// Explosion Control.cpp, lighting.cpp). Drops the line-of-sight memo the observation builder keeps
    /// for the turn. Cheap; safe to call often.
    void OnWorldChanged();

    /// Perception, from Tactical/opplist.cpp: what the observer legitimately learned.
    void OnManSeesMan(SOLDIERTYPE* pObserver, SOLDIERTYPE* pSeen, INT32 sGridNo, INT8 bLevel);
    void OnHearNoise(SOLDIERTYPE* pListener, UINT8 ubNoiseMaker, INT32 sGridNo, INT8 bLevel, UINT8 ubVolume, UINT8 ubNoiseType);
    void OnNoticeUnseenAttacker(SOLDIERTYPE* pDefender, SOLDIERTYPE* pAttacker);
    /// Tactical/Soldier Control.cpp SoldierTakeDamage, before the damage is applied.
    void OnSoldierTakeDamage(SOLDIERTYPE* pVictim, UINT8 ubAttacker, INT16 sLifeDeduct, INT16 sBreathLoss, UINT8 ubReason);

    /// TacticalAI/AIMain.cpp StartNPCAI, before RefreshAI: snapshot for the situation exporter, start recording dice.
    void OnStartNPCAI(SOLDIERTYPE* pSoldier);

    /// Called by NeuralPlan::execute after the policy or the legacy fallback decided; completes the log record.
    void OnNeuralDecisionEnd(SOLDIERTYPE* pSoldier, bool fPolicyDecided);

    /// Savegame: the belief store travels with the save from version BELIEF_STORE_IN_SAVE on.
    BOOLEAN SaveNeuralState(HWFILE hFile);
    BOOLEAN LoadNeuralState(HWFILE hFile, UINT32 uiSaveGameVersion);

    /// RAII guard around the body of LegacyAIPlan::execute; one line in that function.
    class LegacyDecisionScope
    {
        public:
            LegacyDecisionScope(SOLDIERTYPE* pSoldier, bool fTurnBased);
            ~LegacyDecisionScope();
        private:
            SOLDIERTYPE* soldier_;
            bool active_;
            LegacyDecisionScope(const LegacyDecisionScope&);
            LegacyDecisionScope& operator=(const LegacyDecisionScope&);
    };
}

#endif
