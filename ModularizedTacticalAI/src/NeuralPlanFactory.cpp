/**
 * @file
 * @author ja2mod
 *
 * Added by the ja2mod fork; see CHANGES-ja2mod.md.
 */

#include "../include/NeuralPlanFactory.h"
#include "../include/NeuralPlan.h"

#include "../../TacticalAI/AIInternals.h"        // DEBUGAIMSG
#include "../../Tactical/Soldier Control.h"      // SOLDIERTYPE
#include "../../Tactical/Animation Data.h"       // BLOODCAT, CROW
#include "Soldier macros.h"                      // ARMED_VEHICLE

namespace AI
{
    namespace tactical
    {
        Plan* NeuralPlanFactory::create_plan(SOLDIERTYPE* npc, const AIInputData& input)
        {
            DEBUGAIMSG("Neural planning for "<<(int)npc->ubID);

            // Creatures, crows, armed vehicles and zombies need the specialised plans the
            // legacy factory builds for them; a policy has no action space for those.
            if((npc->flags.uiStatusFlags & SOLDIER_MONSTER) || npc->ubBodyType == BLOODCAT ||
               npc->ubBodyType == CROW || ARMED_VEHICLE(npc) || npc->IsZombie())
            {
                return legacy_factory_.create_plan(npc, input);
            }

            return new NeuralPlan(npc);
        }


        void NeuralPlanFactory::update_plan(SOLDIERTYPE* npc, const AIInputData& input)
        {
            DEBUGAIMSG("Neural update called for "<<(int)npc->ubID<<" event: "<<input);
            if(!npc->ai_masterplan_)
                npc->ai_masterplan_ = create_plan(npc, input);
        }
    }
}
