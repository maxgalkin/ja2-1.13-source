/**
 * @file
 * @author ja2mod
 *
 * Added by the ja2mod fork on 2026-09-15; see CHANGES-ja2mod.md.
 */

#include "../include/SituationExport.h"
#include "../include/BeliefStore.h"

#include "../../Tactical/Soldier Control.h"
#include "../../Tactical/Soldier Profile.h"
#include "../../Tactical/Overhead.h"
#include "../../Tactical/Overhead Types.h"
#include "../../Tactical/opplist.h"
#include "../../Tactical/Items.h"
#include "../../Tactical/Item Types.h"
#include "../../Tactical/Weapons.h"
#include "../../Tactical/Animation Control.h"
#include "../../Tactical/Campaign.h"
#include "../../Tactical/Tactical Save.h"
#include "../../TacticalAI/ai.h"
#include "../../TileEngine/Isometric Utils.h"
#include "../../Strategic/strategicmap.h"
#include "../../Strategic/Campaign Types.h"
#include "../../Strategic/Game Clock.h"
#include "../../Ja2/GameSettings.h"
#include "sgp_logger.h"

#include <filesystem>
#include <stdio.h>
#include <string.h>
#include <time.h>

namespace tacnn
{
    namespace
    {
        const int FORMAT_VERSION = 1;
        const int HARNESS_NOBODY = 255;

        std::string g_directory;
        bool g_enabled = false;
        FILE* g_file = 0;
        unsigned g_written = 0;

        // The pending question: everything up to (not including) the closing brace.
        std::string g_pending;
        int g_pendingSoldier = -1;

        /// A very small JSON writer; enough for flat objects and integer arrays.
        class Json
        {
            public:
                Json(std::string& out) : out_(out), first_(true) { }

                void beginObject() { sep(); out_ += '{'; first_ = true; }
                void endObject() { out_ += '}'; first_ = false; }
                void beginArray() { sep(); out_ += '['; first_ = true; }
                void endArray() { out_ += ']'; first_ = false; }

                void key(const char* name)
                {
                    sep();
                    out_ += '"';
                    out_ += name;
                    out_ += "\":";
                    first_ = true;   // the value that follows takes no comma
                }
                void integer(long long value)
                {
                    sep();
                    char buffer[32];
                    snprintf(buffer, sizeof buffer, "%lld", value);
                    out_ += buffer;
                }
                void boolean(bool value) { sep(); out_ += value ? "true" : "false"; }
                void null() { sep(); out_ += "null"; }
                void string(const std::string& value)
                {
                    sep();
                    out_ += '"';
                    for(size_t i = 0; i < value.size(); ++i)
                    {
                        const unsigned char c = static_cast<unsigned char>(value[i]);
                        if(c == '"' || c == '\\') { out_ += '\\'; out_ += static_cast<char>(c); }
                        else if(c < 0x20 || c >= 0x7F) { out_ += '?'; }
                        else out_ += static_cast<char>(c);
                    }
                    out_ += '"';
                }

                template<class T>
                void integerArray(const T* values, int count)
                {
                    beginArray();
                    for(int i = 0; i < count; ++i)
                        integer(static_cast<long long>(values[i]));
                    endArray();
                }

            private:
                void sep()
                {
                    if(!first_)
                        out_ += ',';
                    first_ = false;
                }
                std::string& out_;
                bool first_;
        };

        std::string Narrow(const CHAR16* wide, size_t max)
        {
            std::string out;
            for(size_t i = 0; i < max && wide[i]; ++i)
                out += (wide[i] >= 0x20 && wide[i] < 0x7F) ? static_cast<char>(wide[i]) : '?';
            return out;
        }

        bool Exported(const SOLDIERTYPE* p)
        {
            return p && p->bActive && p->bInSector && !(p->flags.uiStatusFlags & SOLDIER_VEHICLE);
        }

        int StanceCode(const SOLDIERTYPE* p)
        {
            return gAnimControl[p->usAnimState].ubEndHeight;
        }

        bool EnsureOpen()
        {
            if(g_file)
                return true;
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(g_directory), ec);
            time_t now = time(0);
            struct tm* local = localtime(&now);
            char name[64];
            if(local)
                strftime(name, sizeof name, "situations-%Y%m%d-%H%M%S.jsonl", local);
            else
                snprintf(name, sizeof name, "situations-%lld.jsonl", static_cast<long long>(now));
            std::string path = g_directory + "/" + name;
            g_file = fopen(path.c_str(), "ab");
            if(!g_file)
                SGP_INFO() << "tacnn: cannot open situation export " << path << sgp::endl;
            return g_file != 0;
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

        void WriteSoldier(Json& json, const SOLDIERTYPE* p, int denseId, const int* denseOf)
        {
            json.beginObject();
            json.key("soldier_id"); json.integer(denseId);
            json.key("engine_id"); json.integer(p->ubID);
            json.key("team"); json.integer(p->bTeam);
            json.key("side"); json.integer(p->bSide);
            json.key("name"); json.string(Narrow(p->name, 10));
            json.key("profile");
            if(p->ubProfile == NO_PROFILE) json.null(); else json.integer(p->ubProfile);
            json.key("soldier_class"); json.integer(p->ubSoldierClass);
            json.key("gridno"); json.integer(p->sGridNo);
            json.key("level"); json.integer(p->pathing.bLevel);
            json.key("direction"); json.integer(p->ubDirection);
            json.key("anim_state"); json.integer(p->usAnimState);
            json.key("movement_mode"); json.integer(p->usUIMovementMode);
            json.key("tiles_moved"); json.integer(p->bTilesMoved);
            json.key("marksmanship"); json.integer(p->stats.bMarksmanship);
            json.key("dexterity"); json.integer(p->stats.bDexterity);
            json.key("agility"); json.integer(p->stats.bAgility);
            json.key("strength"); json.integer(p->stats.bStrength);
            json.key("wisdom"); json.integer(p->stats.bWisdom);
            json.key("leadership"); json.integer(p->stats.bLeadership);
            json.key("health"); json.integer(p->stats.bLifeMax);
            json.key("experience_level"); json.integer(p->stats.bExpLevel);
            json.key("body_type"); json.integer(p->ubBodyType);
            json.key("life"); json.integer(p->stats.bLife);
            json.key("life_max"); json.integer(p->stats.bLifeMax);
            json.key("bleeding"); json.integer(p->bBleeding);
            json.key("breath"); json.integer(p->bBreath);
            json.key("breath_max"); json.integer(p->bBreathMax);
            json.key("breath_red"); json.integer(p->sBreathRed);
            json.key("shock"); json.integer(p->aiData.bShock);
            json.key("collapsed"); json.boolean(p->bCollapsed != 0);
            json.key("gassed"); json.boolean((p->flags.uiStatusFlags & SOLDIER_GASSED) != 0);
            json.key("alive"); json.boolean(p->stats.bLife > 0);
            json.key("damage_resistance"); json.integer(0);
            json.key("blinded_counter"); json.integer(p->bBlindedCounter);
            json.key("deafened_counter"); json.integer(p->bDeafenedCounter);
            json.key("under_fire"); json.integer(p->aiData.bUnderFire);
            json.key("action_points"); json.integer(p->bActionPoints);
            json.key("initial_action_points"); json.integer(p->bInitialActionPoints);
            json.key("max_action_points"); json.integer(const_cast<SOLDIERTYPE*>(p)->CalcActionPoints());
            json.key("moved"); json.boolean(p->aiData.bMoved != 0);
            json.key("suppression_points"); json.integer(p->ubSuppressionPoints);
            json.key("aps_lost_to_suppression"); json.integer(p->ubAPsLostToSuppression);
            json.key("cowering"); json.boolean((p->flags.uiStatusFlags & SOLDIER_COWERING) != 0);
            json.key("alert_status"); json.integer(p->aiData.bAlertStatus);
            json.key("morale"); json.integer(p->aiData.bMorale);
            json.key("tactical_morale_mod"); json.integer(p->aiData.bTacticalMoraleMod);
            json.key("team_morale_mod"); json.integer(p->aiData.bTeamMoraleMod);
            json.key("strategic_morale_mod"); json.integer(p->aiData.bStrategicMoraleMod);
            json.key("campaign_progress"); json.integer(CurrentPlayerProgressPercentage());
            json.key("phlegmatic"); json.boolean(false);
            const OBJECTTYPE& gun = const_cast<SOLDIERTYPE*>(p)->inv[HANDPOS];
            const bool hasGun = gun.exists() && (Item[gun.usItem].usItemClass & IC_GUN);
            json.key("rounds_in_magazine"); json.integer(hasGun ? gun[0]->data.gun.ubGunShotsLeft : 0);
            json.key("magazines"); json.integer(hasGun ? CountMagazines(p, gun.usItem) : 0);
            json.key("night_ops"); json.integer(0);
            json.key("stealth_mode"); json.boolean(p->bStealthMode != 0);
            json.key("stealthy_reduction"); json.integer(0);
            json.key("squadleader_bonus"); json.integer(0);
            json.key("walkman"); json.boolean(false);
            json.key("hearing_bonus"); json.integer(0);
            json.key("carried_weight_percent"); json.integer(CalculateCarriedWeight(const_cast<SOLDIERTYPE*>(p)));
            json.key("difficulty"); json.integer(gGameOptions.ubDifficultyLevel);
            json.key("enemy_ap_bonus"); json.integer(0);
            json.key("weapon_index");
            if(hasGun) json.integer(gun.usItem); else json.null();
            json.key("weapon_ready"); json.boolean((gAnimControl[p->usAnimState].uiFlags & ANIM_FIREREADY) != 0);
            // the engine's own IsScoped: the export lists no attachments, so the replay could not tell a scoped rifle
            json.key("weapon_scoped"); json.boolean(hasGun && IsScoped(const_cast<OBJECTTYPE*>(&gun)));
            // the one trait the cascades ask about (AICheckIsSniper, AIUtils.cpp:5008)
            json.key("sniper_trait");
            json.boolean(gGameOptions.fNewTraitSystem && HAS_SKILL_TRAIT(const_cast<SOLDIERTYPE*>(p), SNIPER_NT));
            json.key("stance"); json.integer(StanceCode(p));
            // hand grenades, in slot order: what FindThrowableGrenade (Items.cpp:1892) would consider, so the
            // replay can run CheckIfTossPossible with the same pockets and throw the same dice
            json.key("grenades");
            json.beginArray();
            {
                SOLDIERTYPE* man = const_cast<SOLDIERTYPE*>(p);
                const int slots = static_cast<int>(man->inv.size());
                for(int slot = 0; slot < slots; ++slot)
                {
                    const OBJECTTYPE& obj = man->inv[slot];
                    if(!obj.exists())
                        continue;
                    const UINT16 item = obj.usItem;
                    if(!(Item[item].usItemClass & IC_GRENADE) || Item[item].ubCursor != TOSSCURS)
                        continue;
                    if(GetLauncherFromLaunchable(item) != NOTHING)
                        continue;
                    json.beginObject();
                    json.key("slot"); json.integer(slot);
                    json.key("item"); json.integer(item);
                    json.key("status"); json.integer(obj[0]->data.objectStatus);
                    json.key("count"); json.integer(obj.ubNumberOfObjects);
                    json.endObject();
                }
            }
            json.endArray();

            const STRUCT_AIData& ai = p->aiData;
            json.key("ai");
            json.beginObject();
            json.key("orders"); json.integer(ai.bOrders);
            json.key("attitude"); json.integer(ai.bAttitude);
            json.key("alert_status"); json.integer(ai.bAlertStatus);
            json.key("ai_morale"); json.integer(ai.bAIMorale);
            json.key("oppcnt"); json.integer(ai.bOppCnt);
            json.key("shock"); json.integer(ai.bShock);
            json.key("under_fire"); json.integer(ai.bUnderFire);
            json.key("last_attack_hit"); json.boolean(ai.bLastAttackHit != 0);
            json.key("new_situation"); json.integer(ai.bNewSituation);
            json.key("action"); json.integer(ai.bAction);
            json.key("last_action"); json.integer(ai.bLastAction);
            json.key("next_action"); json.integer(ai.bNextAction);
            json.key("action_data"); json.integer(ai.usActionData);
            json.key("patrol_grid"); json.integerArray(ai.sPatrolGrid, MAXPATROLGRIDS);
            json.key("next_patrol_point"); json.integer(ai.bNextPatrolPnt);
            json.key("noise_gridno"); json.integer(ai.sNoiseGridno);
            json.key("noise_volume"); json.integer(ai.ubNoiseVolume);
            json.key("noise_level"); json.integer(p->bNoiseLevel);
            json.key("blacklist"); json.integer(p->pathing.sBlackList);
            json.key("last_two_locations"); json.integerArray(p->sLastTwoLocations, 2);
            json.key("num_flanks"); json.integer(p->numFlanks);
            json.key("orig_dir"); json.integer(p->origDir);
            json.key("last_flank_left"); json.boolean(p->flags.lastFlankLeft != 0);
            json.key("last_flank_spot"); json.integer(p->lastFlankSpot);
            json.key("bypass_to_green"); json.integer(ai.bBypassToGreen);
            json.key("action_in_progress"); json.boolean(ai.bActionInProgress != 0);
            json.key("next_action_data"); json.integer(ai.usNextActionData);
            json.key("next_target_level"); json.integer(ai.bNextTargetLevel);
            json.key("sniper"); json.integer(p->sniper);
            json.key("aim_shot_location"); json.integer(p->bAimShotLocation);
            json.key("dominant_dir"); json.integer(ai.bDominantDir);
            // the harness keeps one watched location; give it the one with the most points
            int best = 0;
            for(int k = 1; k < NUM_WATCHED_LOCS; ++k)
                if(gubWatchedLocPoints[p->ubID][k] > gubWatchedLocPoints[p->ubID][best])
                    best = k;
            json.key("watched_loc"); json.integer(gsWatchedLoc[p->ubID][best]);
            json.key("watched_level"); json.integer(gbWatchedLocLevel[p->ubID][best]);
            json.key("watched_points"); json.integer(gubWatchedLocPoints[p->ubID][best]);
            json.key("raised_red_alert"); json.boolean((p->usSoldierFlagMask & SOLDIER_RAISED_REDALERT) != 0);
            // the gbDiff column this soldier's chances are read from: class, game difficulty, progress and distance
            // to the palace go into it, so it is not the game's difficulty setting and cannot be recomputed offline
            json.key("ai_difficulty"); json.integer(SoldierDifficultyLevel(const_cast<SOLDIERTYPE*>(p)));
            json.key("last_target"); json.integer(p->sLastTarget);
            json.key("previous_attacker_id");
            json.integer(p->ubPreviousAttackerID < TOTAL_SOLDIERS && denseOf[p->ubPreviousAttackerID] >= 0
                         ? denseOf[p->ubPreviousAttackerID] : HARNESS_NOBODY);
            json.key("next_to_previous_attacker_id");
            json.integer(p->ubNextToPreviousAttackerID < TOTAL_SOLDIERS && denseOf[p->ubNextToPreviousAttackerID] >= 0
                         ? denseOf[p->ubNextToPreviousAttackerID] : HARNESS_NOBODY);
            json.key("blinded_counter"); json.integer(p->bBlindedCounter);
            json.key("flanking"); json.boolean(const_cast<SOLDIERTYPE*>(p)->IsFlanking() != 0);
            json.key("moved"); json.boolean(ai.bMoved != 0);
            json.key("asleep"); json.boolean((ai.fAIFlags & AI_ASLEEP) != 0);
            json.key("check_schedule"); json.boolean((ai.fAIFlags & AI_CHECK_SCHEDULE) != 0);
            json.key("target_level"); json.integer(p->bTargetLevel);
            json.endObject();

            json.endObject();
        }

        template<class Row>
        void WriteMatrix(Json& json, const char* name, const SOLDIERTYPE* const* soldiers, int count,
                         const Row& row)
        {
            json.key(name);
            json.beginArray();
            for(int i = 0; i < count; ++i)
            {
                json.beginArray();
                for(int j = 0; j < count; ++j)
                    json.integer(row(soldiers[i]->ubID, soldiers[j]->ubID));
                json.endArray();
            }
            json.endArray();
        }

        template<class Row>
        void WriteTeamMatrix(Json& json, const char* name, const SOLDIERTYPE* const* soldiers, int count,
                             const Row& row)
        {
            json.key(name);
            json.beginArray();
            for(int team = 0; team < MAXTEAMS; ++team)
            {
                json.beginArray();
                for(int j = 0; j < count; ++j)
                    json.integer(row(team, soldiers[j]->ubID));
                json.endArray();
            }
            json.endArray();
        }
    }

    void SituationExportConfigure(const std::string& directory, bool enabled)
    {
        if(g_file && (!enabled || directory != g_directory))
        {
            fclose(g_file);
            g_file = 0;
        }
        g_directory = directory.empty() ? std::string("tacnn-situations") : directory;
        g_enabled = enabled;
    }

    bool SituationExportEnabled()
    {
        return g_enabled;
    }

    std::string SituationMapName()
    {
        char buffer[32];
        const int x = gWorldSectorX;
        const int y = gWorldSectorY;
        const int z = gbWorldSectorZ;
        if(x < 1 || x > 16 || y < 1 || y > 16)
            return std::string("unknown");
        snprintf(buffer, sizeof buffer, "%c%d", static_cast<char>('a' + (y - 1)), x);
        std::string name(buffer);
        if(z != 0)
        {
            snprintf(buffer, sizeof buffer, "_b%d", z);
            name += buffer;
        }
        if(GetSectorFlagStatus(static_cast<INT16>(x), static_cast<INT16>(y), static_cast<UINT8>(z), SF_USE_ALTERNATE_MAP))
            name += "_a";
        return name;
    }

    void SituationSnapshot(SOLDIERTYPE* pSoldier, unsigned stage, bool beforeRefresh)
    {
        if(!g_enabled || !pSoldier)
            return;

        const SOLDIERTYPE* soldiers[TOTAL_SOLDIERS];
        int denseOf[TOTAL_SOLDIERS];
        int count = 0;
        for(int i = 0; i < TOTAL_SOLDIERS; ++i)
        {
            denseOf[i] = -1;
            const SOLDIERTYPE* p = MercPtrs[i];
            if(Exported(p))
            {
                denseOf[i] = count;
                soldiers[count++] = p;
            }
        }
        if(denseOf[pSoldier->ubID] < 0)
            return;

        g_pending.clear();
        g_pending.reserve(65536);
        Json json(g_pending);
        json.beginObject();
        json.key("version"); json.integer(FORMAT_VERSION);
        json.key("source"); json.string("engine");
        json.key("map"); json.string(SituationMapName());
        json.key("sector_x"); json.integer(gWorldSectorX);
        json.key("sector_y"); json.integer(gWorldSectorY);
        json.key("sector_z"); json.integer(gbWorldSectorZ);
        json.key("turn"); json.integer(GetBeliefStore().turn());
        json.key("minute_of_day"); json.integer(GetWorldMinutesInDay());
        json.key("underground"); json.boolean(gbWorldSectorZ != 0);
        json.key("current_team"); json.integer(gTacticalStatus.ubCurrentTeam);
        json.key("difficulty"); json.integer(gGameOptions.ubDifficultyLevel);
        json.key("stage"); json.integer(stage);
        json.key("before_refresh"); json.boolean(beforeRefresh);
        json.key("soldier"); json.integer(denseOf[pSoldier->ubID]);
        json.key("soldier_engine_id"); json.integer(pSoldier->ubID);

        json.key("soldiers");
        json.beginArray();
        for(int i = 0; i < count; ++i)
            WriteSoldier(json, soldiers[i], i, denseOf);
        json.endArray();

        json.key("opplists");
        json.beginObject();
        WriteMatrix(json, "personal", soldiers, count,
                    [](UINT8 a, UINT8 b) { return (long long)MercPtrs[a]->aiData.bOppList[b]; });
        WriteMatrix(json, "last_known_loc", soldiers, count,
                    [](UINT8 a, UINT8 b) { return (long long)gsLastKnownOppLoc[a][b]; });
        WriteMatrix(json, "last_known_level", soldiers, count,
                    [](UINT8 a, UINT8 b) { return (long long)gbLastKnownOppLevel[a][b]; });
        WriteMatrix(json, "seen_opponents", soldiers, count,
                    [](UINT8 a, UINT8 b) { return (long long)gbSeenOpponents[a][b]; });
        WriteTeamMatrix(json, "public", soldiers, count,
                        [](int team, UINT8 b) { return (long long)gbPublicOpplist[team][b]; });
        WriteTeamMatrix(json, "public_last_known_loc", soldiers, count,
                        [](int team, UINT8 b) { return (long long)gsPublicLastKnownOppLoc[team][b]; });
        WriteTeamMatrix(json, "public_last_known_level", soldiers, count,
                        [](int team, UINT8 b) { return (long long)gbPublicLastKnownOppLevel[team][b]; });
        json.key("public_noise_volume"); json.integerArray(gubPublicNoiseVolume, MAXTEAMS);
        json.key("public_noise_gridno"); json.integerArray(gsPublicNoiseGridNo, MAXTEAMS);
        json.key("public_noise_level"); json.integerArray(gbPublicNoiseLevel, MAXTEAMS);
        // bAwareOfOpposition per team: the flag that latches a team out of green and yellow
        json.key("team_aware");
        json.beginArray();
        for(int t = 0; t < MAXTEAMS; ++t)
            json.integer(gTacticalStatus.Team[t].bAwareOfOpposition ? 1 : 0);
        json.endArray();
        json.key("opp_count");
        json.beginArray();
        for(int i = 0; i < count; ++i)
            json.integer(soldiers[i]->aiData.bOppCnt);
        json.endArray();
        json.key("new_opp_count");
        json.beginArray();
        for(int i = 0; i < count; ++i)
            json.integer(soldiers[i]->bNewOppCnt);
        json.endArray();
        json.endObject();

        g_pendingSoldier = pSoldier->ubID;
    }

    bool SituationPendingFor(const SOLDIERTYPE* pSoldier)
    {
        return pSoldier && g_pendingSoldier == pSoldier->ubID && !g_pending.empty();
    }

    void SituationComplete(SOLDIERTYPE* pSoldier, const std::vector<uint32_t>& rolls, const std::vector<uint32_t>& ranges)
    {
        if(!SituationPendingFor(pSoldier))
            return;
        if(!EnsureOpen())
        {
            SituationDiscard();
            return;
        }
        Json json(g_pending);
        // the object is still open; the writer's comma state is fresh, so add the separator by hand
        g_pending += ',';
        json.key("rolls");
        json.beginArray();
        for(size_t i = 0; i < rolls.size(); ++i)
            json.integer(rolls[i]);
        json.endArray();
        // the die of each roll, so a replay can tell at which draw it parted from the engine
        json.key("roll_ranges");
        json.beginArray();
        for(size_t i = 0; i < ranges.size() && i < rolls.size(); ++i)
            json.integer(ranges[i]);
        json.endArray();
        json.key("expected");
        json.beginObject();
        json.key("action"); json.integer(pSoldier->aiData.bAction);
        json.key("action_name"); json.string(std::string());
        json.key("action_data"); json.integer(pSoldier->aiData.usActionData);
        json.key("target_level"); json.integer(pSoldier->bTargetLevel);
        json.key("aim_time"); json.integer(pSoldier->aiData.bAimTime);
        json.key("rule"); json.string("engine");
        json.endObject();
        g_pending += "}\n";
        fwrite(g_pending.data(), 1, g_pending.size(), g_file);
        fflush(g_file);
        ++g_written;
        SituationDiscard();
    }

    void SituationDiscard()
    {
        g_pending.clear();
        g_pendingSoldier = -1;
    }

    unsigned SituationExported()
    {
        return g_written;
    }
}
