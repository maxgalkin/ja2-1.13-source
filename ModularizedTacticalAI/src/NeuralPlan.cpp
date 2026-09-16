/**
 * @file
 * @author ja2mod
 *
 * Added by the ja2mod fork; see CHANGES-ja2mod.md.
 */

#include "../include/NeuralPlan.h"

#include "../../Tactical/Soldier Control.h"      // SOLDIERTYPE
#include "sgp_logger.h"                          // SGP_INFO writes to game_log.log
#include "../include/NeuralHooks.h"              // ja2mod 2026-09-15: sidecar policy, decision log

namespace AI
{
    namespace tactical
    {
        namespace
        {
            /**@class DecliningPolicy
             * @brief The policy in force until a real one is installed: it never decides.
             *
             * With this policy every NeuralPlan falls through to the legacy decision tree,
             * so tagging a soldier for the neural factory changes nothing but the log.
             */
            class DecliningPolicy : public CustomPolicy
            {
                public:
                    virtual const char* name() const { return "decline"; }
                    virtual bool decide(SOLDIERTYPE*, PlanInputData&) { return false; }
            };

            DecliningPolicy g_declining_policy;
            CustomPolicy*   g_active_policy = &g_declining_policy;
        }

        CustomPolicy* GetActivePolicy()
        {
            return g_active_policy;
        }

        void SetActivePolicy(CustomPolicy* policy)
        {
            g_active_policy = policy ? policy : &g_declining_policy;
        }


        NeuralPlan::NeuralPlan(SOLDIERTYPE* npc)
            : LegacyAIPlan(npc)
        {
        }


        void NeuralPlan::execute(PlanInputData& environment)
        {
            SOLDIERTYPE* npc = get_npc();
            CustomPolicy* policy = GetActivePolicy();

            bool decided = policy->decide(npc, environment);
            if(!decided)
                LegacyAIPlan::execute(environment);
            tacnn::OnNeuralDecisionEnd(npc, decided);

            // One line per decision of a soldier routed to this factory. Soldiers that are
            // not routed here never reach this code, so the log of a game without any of
            // them is the stock log.
            SGP_INFO() << "NeuralPlan: soldier " << (int)npc->ubID
                       << " team " << (int)npc->bTeam
                       << " class " << (int)npc->ubSoldierClass
                       << " policy " << policy->name()
                       << " source " << (decided ? "policy" : "legacy")
                       << " action " << (int)npc->aiData.bAction
                       << " data " << (int)npc->aiData.usActionData
                       << sgp::endl;
        }
    }
}
