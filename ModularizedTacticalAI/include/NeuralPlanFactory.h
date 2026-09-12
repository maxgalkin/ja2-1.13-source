/**
 * @file
 * @author ja2mod
 *
 * Added by the ja2mod fork; see CHANGES-ja2mod.md.
 */

#ifndef NEURAL_PLAN_FACTORY_H_
#define NEURAL_PLAN_FACTORY_H_

#include "AbstractPlanFactory.h"
#include "LegacyAIPlanFactory.h"
#include <string>

namespace AI
{
    namespace tactical
    {
        /**@class NeuralPlanFactory
         * @brief Concrete Factory. Produces the NeuralPlan for soldiers routed to a learned policy.
         *
         * Soldiers reach this factory through their bAIIndex, which AI.ini maps to a factory
         * name. The mod's AI.ini patch declares this factory in slot 11; the engine sets that
         * index on the soldiers that should use a policy.
         *
         * Body types the policy has no action space for -- creatures, bloodcats, crows, armed
         * vehicles and zombies -- are handed to the legacy factory, which builds their
         * specialised plans. Only ordinary humans get a NeuralPlan.
         */
        class NeuralPlanFactory : public AbstractPlanFactory
        {
            private:
                LegacyAIPlanFactory legacy_factory_;
            public:
                static std::string get_name() {return "NeuralPlanFactory";}
                virtual Plan* create_plan(SOLDIERTYPE* npc, const AIInputData& input);
                virtual void update_plan(SOLDIERTYPE* npc, const AIInputData& input);
        };
    }
}

#endif
