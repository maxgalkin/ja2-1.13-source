/**
 * @file
 * @author ja2mod
 *
 * Seam for a learned tactical policy. Added by the ja2mod fork; see
 * CHANGES-ja2mod.md.
 */

#ifndef NEURAL_PLAN_H_
#define NEURAL_PLAN_H_

#include "LegacyAIPlan.h"

class SOLDIERTYPE;

namespace AI
{
    namespace tactical
    {
        /**@class CustomPolicy
         * @brief Interface for an external decision maker that can take over a soldier's turn.
         *
         * A policy is asked once per decision. It either sets the soldier's aiData action
         * fields and reports that it did (true), or it declines (false) and leaves the
         * soldier untouched, in which case NeuralPlan runs the legacy decision tree.
         * Declining must have no observable effect, because it is the normal case whenever
         * no policy is loaded or the policy has nothing to say about the situation.
         *
         * The observation that a real policy needs is not built here yet; it arrives with
         * the observation builder and the sidecar transport.
         */
        class CustomPolicy
        {
            public:
                virtual ~CustomPolicy() { }
                /// Short name used in the game log, e.g. "decline".
                virtual const char* name() const = 0;
                /**@brief Decide what the soldier does this turn.
                 * @param npc The soldier being decided for.
                 * @param environment The same environment data the legacy plan receives.
                 * @return true iff the soldier's action fields were set; false to decline.
                 */
                virtual bool decide(SOLDIERTYPE* npc, PlanInputData& environment) = 0;
        };

        /// The policy every NeuralPlan consults. Never null; the built-in default declines.
        CustomPolicy* GetActivePolicy();

        /**@brief Install a policy. Passing null restores the declining default.
         *
         * The caller keeps ownership. There is no policy to install yet; the setter exists
         * so the transport can be added without touching this header again.
         */
        void SetActivePolicy(CustomPolicy* policy);

        /**@class NeuralPlan
         * @brief Concrete Product. Asks the active policy first, falls back to the legacy AI.
         *
         * Inheriting from LegacyAIPlan is what makes the fallback exact: when the policy
         * declines, LegacyAIPlan::execute runs unchanged, so a soldier routed through this
         * plan with the default policy behaves like any other soldier.
         */
        class NeuralPlan : public LegacyAIPlan
        {
            public:
                NeuralPlan(SOLDIERTYPE* npc);
                virtual void execute(PlanInputData& environment);
        };
    }
}

#endif
