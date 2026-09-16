/**
 * @file
 * @author ja2mod
 *
 * Added by the ja2mod fork on 2026-09-15; see CHANGES-ja2mod.md and Harness.h.
 *
 * The harness is a small state machine that GameLoop ticks once per frame. It
 * never runs game code of its own: it presses the same buttons a player would
 * (load the quick save, answer a message box, end the turn) and otherwise lets
 * the stock AI play. The player's mercs are handed to HandleSoldierAI exactly
 * like an AI team's soldiers, so the two sides fight the legacy tree against
 * the legacy tree while the situation exporter records every enemy decision.
 */

#include "../include/Harness.h"
#include "../include/NeuralHooks.h"
#include "../include/SituationExport.h"
#include "../include/tacnn_schema.h"
#include "../include/Plan.h" // the plan destructor, for HARNESS_NEURAL_ALL

#include "../../sgp/sgp.h"
#include "../../sgp/video.h"
#include "../../sgp/sgp_logger.h"
#include "../../sgp/random.h"
#include "../../Ja2/GameSettings.h"
#include "../../Ja2/gameloop.h"
#include "../../Ja2/jascreens.h"
#include "../../Ja2/screenids.h"
#include "../../Ja2/MainMenuScreen.h"
#include "../../Ja2/SaveLoadScreen.h"
#include "../../Ja2/MessageBoxScreen.h"
#include "../../Ja2/Fade Screen.h"
#include "../../Ja2/Sys Globals.h"
#include "../../Tactical/Overhead.h"
#include "../../Tactical/Overhead Types.h"
#include "../../Tactical/Interface Items.h"
#include "../../Tactical/Soldier Control.h"
#include "../../Tactical/TeamTurns.h"
#include "../../Tactical/Map Information.h"
#include "../../Tactical/opplist.h"
#include "../../TileEngine/Explosion Control.h"
#include "../../TileEngine/worlddef.h"
#include "../../TacticalAI/ai.h"
#include "../../TacticalAI/AIList.h"
#include "../../Strategic/strategicmap.h"
#include "../../Strategic/strategic.h"
#include "../../Strategic/Queen Command.h"
#include "../../Strategic/Map Screen Interface Bottom.h"
#include "../../Strategic/Game Clock.h"
#include "../../Utils/Timer Control.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <direct.h>
#include <windows.h>

extern BOOLEAN gfInMsgBox;
extern BOOLEAN gfInItemPickupMenu; // Tactical/Interface Items.cpp, not in its header
extern BOOLEAN gfFadeOut;    // Ja2/Fade Screen.cpp, not in its header
extern BOOLEAN gfPauseClock; // Utils/Timer Control.cpp, likewise
extern BOOLEAN gfGamePaused; // Strategic/Game Clock.cpp
extern BOOLEAN gfLogsEnabled; // TacticalAI/AIMain.cpp: the stock AI decision log, two file opens per line
extern BOOLEAN gfPauseAllAI;                   // Tactical/Overhead.cpp
extern BOOLEAN gfFadeOutDone;                  // Ja2/Fade Screen.cpp
extern BOOLEAN gfFadeInitialized;              // Ja2/Fade Screen.cpp
extern BOOLEAN gfSaveLoadScreenEntry;          // Ja2/SaveLoadScreen.cpp, the rest likewise
extern BOOLEAN gfSaveLoadScreenExit;
extern UINT32 guiSaveLoadExitScreen;
extern BOOLEAN gfSaveLoadScreenButtonsCreated;
extern BOOLEAN gfLoadGameUponEntry;
extern BOOLEAN gfStartedFadingOut;
extern INT32 gbSelectedSaveLocation;
extern BOOLEAN gbSaveGameArray[];

namespace tacnn
{
    namespace
    {
        enum State
        {
            HS_OFF,
            HS_MENU,      // waiting for the main menu, then pressing Load
            HS_LOADING,   // a load is in flight; waiting for a stable game screen with the new world
            HS_ARMING,    // one frame: seed, exporter folder, orders for the squad
            HS_BATTLE,    // driving the player's turns until the battle ends
            HS_RELOAD,    // battle recorded; waiting for a quiet frame to load again
            HS_DONE       // summary written; quitting
        };

        HarnessOptions g_opts;
        bool g_commandLine = false;   // NEURAL_HARNESS came from the command line, so we are on before the INI is read
        State g_state = HS_OFF;
        bool g_initialised = false;
        FILE* g_log = 0;

        int g_battle = 0;             // battles finished so far
        unsigned g_loadGeneration = 0;
        unsigned g_loadSeen = 0;      // generation at the time the current load was requested
        int g_stableFrames = 0;

        typedef std::chrono::steady_clock Clock;
        Clock::time_point g_stateSince;
        Clock::time_point g_heartbeat;
        double g_lapMs[HB_COUNT] = {0};     // frame profile since the last heartbeat
        unsigned g_lapCount[HB_COUNT] = {0}; // how often each nested section ran since the last heartbeat
        Clock::time_point g_lapStart;
        bool g_lapping = false;
        unsigned g_frames = 0;              // GameLoop calls since the harness started
        unsigned g_framesAtHeartbeat = 0;
        unsigned g_framesAtBattleStart = 0;
        Clock::time_point g_battleStart;
        Clock::time_point g_turnSince;
        UINT32 g_battleClockStart = 0;

        // per battle
        bool g_battleOpen = false;
        bool g_battleEnded = false;
        TacnnEpisodeEnd g_end;
        // the hooks' running totals at the start of the battle; the result reports the differences
        unsigned g_decisionsAtStart = 0;
        int g_damageDealtAtStart = 0;
        int g_damageTakenAtStart = 0;
        int g_playerTurns = 0;
        int g_enemyTurns = 0;
        double g_enemyTurnMs = 0.0;
        int g_lastTeam = -1;
        bool g_ourTurnArmed = false;
        unsigned g_situationsAtStart = 0;
        int g_boxesAnswered = 0;
        int g_menusClosed = 0;
        int g_handoffs = 0;
        int g_framesNoEnemy = 0;
        int g_episodes = 0;
        Clock::time_point g_lapsedAt;
        bool g_addedEnemy[TOTAL_SOLDIERS];      ///< enemies the top-up brought in this battle, by soldier id
        int g_startPlayer = 0;
        int g_startEnemy = 0;

        double MsSince(Clock::time_point t)
        {
            return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
        }

        void LogLine(const char* fmt, ...)
        {
            char buf[1024];
            va_list ap;
            va_start(ap, fmt);
            _vsnprintf(buf, sizeof buf - 1, fmt, ap);
            va_end(ap);
            buf[sizeof buf - 1] = 0;
            SGP_INFO() << "harness: " << buf << sgp::endl;
            if(g_log)
            {
                fprintf(g_log, "%s\n", buf);
                fflush(g_log);
            }
        }

        void MakeDir(const std::string& path)
        {
            std::string p = path;
            for(size_t i = 1; i <= p.size(); ++i)
            {
                if(i == p.size() || p[i] == '\\' || p[i] == '/')
                {
                    std::string part = p.substr(0, i);
                    if(part.size() == 2 && part[1] == ':')
                        continue;
                    _mkdir(part.c_str());
                }
            }
        }

        std::string BattleDir(int battle)
        {
            char buf[64];
            sprintf(buf, "%sbattle-%02d", g_opts.outDir.empty() ? "" : "\\", battle + 1);
            return g_opts.outDir + buf;
        }

        void SetState(State s)
        {
            g_state = s;
            g_stateSince = Clock::now();
            g_stableFrames = 0;
        }

        void Init()
        {
            g_initialised = true;
            MakeDir(g_opts.outDir);
            g_log = fopen((g_opts.outDir + "\\harness.log").c_str(), "a");
            LogLine("start slot=%d battles=%d seed=%u ff=%d ff_us=%d render=%d max_turns=%d timeout_s=%d deadlock_s=%d lapse_s=%d enemies=%d elites=%d edge=%d alert=%d force_tb=%d quiet_turns=%d radar=%d ai_log=%d neural_all=%d neural_mode=%s neural_model=%s log_player=%d out=%s",
                    g_opts.slot, g_opts.battles, g_opts.seed, g_opts.fastForward ? 1 : 0, g_opts.fastForwardMicros,
                    g_opts.render ? 1 : 0, g_opts.maxTurns, g_opts.timeoutSeconds, g_opts.deadlockSeconds, g_opts.lapseSeconds, g_opts.enemies,
                    g_opts.elites, g_opts.enemyEdge, g_opts.enemyAlert, g_opts.forceTurnMode ? 1 : 0, g_opts.quietTurns, g_opts.radar ? 1 : 0, g_opts.aiLog ? 1 : 0,
                    g_opts.neuralAll ? 1 : 0, g_opts.neuralMode.empty() ? "(ini)" : g_opts.neuralMode.c_str(),
                    g_opts.neuralModel.empty() ? "(ini)" : g_opts.neuralModel.c_str(), g_opts.logPlayer ? 1 : 0, g_opts.outDir.c_str());
            if(!g_opts.aiLog)
            {
                // the stock log costs two fopen/fclose per DebugAI line, which is most of what a decision costs
                gfLogsEnabled = FALSE;
            }
            gfHarnessFastForward = g_opts.fastForward ? TRUE : FALSE;
            if(g_opts.fastForward)
            {
                // the clock thread advances one 10 ms slice every fastForwardMicros of real time
                SetFastForwardPeriod(g_opts.fastForwardMicros);
                SetFastForwardMode(TRUE);
                gGameExternalOptions.gfVSync = FALSE;
            }
            SetState(HS_MENU);
        }

        void Quit(int code)
        {
            LogLine("quit code=%d after %d battles", code, g_battle);
            if(g_log)
            {
                fclose(g_log);
                g_log = 0;
            }
            gfHarnessFastForward = FALSE;
            gfProgramIsRunning = FALSE;
            if(ghWindow)
                PostMessage(ghWindow, WM_CLOSE, 0, 0);
        }

        // ---------------------------------------------------------------- game state helpers

        bool TurnBasedCombat()
        {
            return (gTacticalStatus.uiFlags & TURNBASED) && (gTacticalStatus.uiFlags & INCOMBAT);
        }

        bool GameScreenReady()
        {
            return guiCurrentScreen == GAME_SCREEN && guiPendingScreen == NO_PENDING_SCREEN && gfWorldLoaded
                && !gfFadeIn && !gfFadeOut && !fFirstTimeInGameScreen && !(gTacticalStatus.uiFlags & LOADING_SAVED_GAME);
        }

        bool Alive(const SOLDIERTYPE* p)
        {
            return p && p->bActive && p->bInSector && p->stats.bLife > 0 && !(p->flags.uiStatusFlags & SOLDIER_VEHICLE);
        }

        void CountSurvivors(TacnnEpisodeEnd& end)
        {
            end.survivors_enemy = end.survivors_player = end.survivors_militia = 0;
            for(int i = 0; i < TOTAL_SOLDIERS; ++i)
            {
                const SOLDIERTYPE* p = MercPtrs[i];
                if(!Alive(p))
                    continue;
                if(p->bTeam == ENEMY_TEAM && end.survivors_enemy < 255) ++end.survivors_enemy;
                else if(p->bTeam == OUR_TEAM && end.survivors_player < 255) ++end.survivors_player;
                else if(p->bTeam == MILITIA_TEAM && end.survivors_militia < 255) ++end.survivors_militia;
            }
        }

        const char* OutcomeName(int outcome)
        {
            switch(outcome)
            {
            case TACNN_OUTCOME_PLAYER_WON: return "player_won";
            case TACNN_OUTCOME_ENEMY_WON: return "enemy_won";
            default: return "unknown";
            }
        }

        // Message boxes stop the tactical loop until a button is pressed; press the button that keeps the game going.
        void AnswerMessageBox()
        {
            if(!gfInMsgBox || guiCurrentScreen != MSG_BOX_SCREEN || gMsgBox.bHandled != 0)
                return;
            const UINT32 f = gMsgBox.usFlags;
            INT8 answer;
            if(f & (MSG_BOX_FLAG_YESNO | MSG_BOX_FLAG_YESNOCONTRACT | MSG_BOX_FLAG_YESNOLIE))
                // "load anyway?", "end turn?" and similar want yes while loading; anything during a battle is declined
                answer = (g_state == HS_LOADING || g_state == HS_MENU || g_state == HS_RELOAD) ? MSG_BOX_RETURN_YES : MSG_BOX_RETURN_NO;
            else
                answer = MSG_BOX_RETURN_OK;
            gMsgBox.bHandled = answer;
            ++g_boxesAnswered;
            LogLine("message box flags=0x%x answered %d in state %d", f, answer, g_state);
        }

        // The tactical menus that wait for the mouse (the item pickup menu above all) are closed the way Cancel
        // would. HandleSoldierPickupItem no longer opens the pickup menu for a driven merc; this catches any
        // other way in, because a second InitializeItemPickupMenu over a menu still up re-links its regions
        // into MSYS_RegList as a cycle and the next MSYS_RemoveRegion never returns (build 25, battle 10 of 20).
        void CloseTacticalMenus()
        {
            if(!gfInItemPickupMenu)
                return;
            RemoveItemPickupMenu();
            ++g_menusClosed;
            LogLine("item pickup menu closed in state %d", g_state);
        }

        // A save made while paused loads paused, and a message box pauses the clock while it is up.
        void KeepClockRunning()
        {
            if(gfInMsgBox)
                return;
            if(gfPauseClock)
                PauseTime(FALSE);
            if(gfGamePaused && !TurnBasedCombat())
            {
                UnLockPauseState();
                UnPauseGame();
            }
        }

        // The player's squad fights on its own: seek-and-destroy orders, and each of them takes his turn from HandleSoldierAI.
        void OrderSquad()
        {
            int n = 0;
            for(int i = gTacticalStatus.Team[OUR_TEAM].bFirstID; i <= gTacticalStatus.Team[OUR_TEAM].bLastID; ++i)
            {
                SOLDIERTYPE* p = MercPtrs[i];
                if(!Alive(p))
                    continue;
                p->aiData.bOrders = SEEKENEMY;
                p->aiData.bAttitude = AGGRESSIVE;
                p->aiData.bAlertStatus = STATUS_RED;
                ++n;
            }
            gTacticalStatus.Team[OUR_TEAM].bAwareOfOpposition = TRUE;
            LogLine("squad of %d ordered to seek", n);
        }

        int CountTeam(INT8 team)
        {
            int n = 0;
            for(int i = gTacticalStatus.Team[team].bFirstID; i <= gTacticalStatus.Team[team].bLastID; ++i)
                if(Alive(MercPtrs[i]) && MercPtrs[i]->bTeam == team)
                    ++n;
            return n;
        }

        // HARNESS_ENEMY_ALERT: start the enemy team at a chosen alert status; below red they also forget the squad,
        // so their first decisions are the green and yellow ones a patrol makes before it notices anyone.
        // Yellow is "heard something, nobody in sight". DecideAlertStatus drops a yellow soldier back to green as
        // soon as MostImportantNoiseHeard has nothing for him, and the alert override wipes what he knew, so the
        // squad's centre is entered as the enemy team's public noise, as loud as the engine's loudest miscellaneous
        // noise (MAX_MISC_NOISE_DURATION, 12; combat decays it by three tenths a turn). It is planted again at the
        // start of every enemy turn until the team notices the squad, so a HARNESS_ENEMY_ALERT=1 battle yields
        // yellow decisions for as long as the squad keeps out of sight.
        void PlantSquadNoise()
        {
            INT32 sumX = 0, sumY = 0, n = 0;
            for(int i = gTacticalStatus.Team[OUR_TEAM].bFirstID; i <= gTacticalStatus.Team[OUR_TEAM].bLastID; ++i)
            {
                SOLDIERTYPE* p = MercPtrs[i];
                if(!Alive(p) || TileIsOutOfBounds(p->sGridNo))
                    continue;
                sumX += p->sGridNo % WORLD_COLS;
                sumY += p->sGridNo / WORLD_COLS;
                ++n;
            }
            if(n == 0)
                return;
            gsPublicNoiseGridNo[ENEMY_TEAM] = (sumY / n) * WORLD_COLS + (sumX / n);
            gubPublicNoiseVolume[ENEMY_TEAM] = MAX_MISC_NOISE_DURATION;
            gbPublicNoiseLevel[ENEMY_TEAM] = 0;
        }

        void SetEnemyAlert()
        {
            const int level = g_opts.enemyAlert;
            if(level < STATUS_GREEN || level > STATUS_BLACK)
                return;
            int changed = 0;
            for(int i = gTacticalStatus.Team[ENEMY_TEAM].bFirstID; i <= gTacticalStatus.Team[ENEMY_TEAM].bLastID; ++i)
            {
                SOLDIERTYPE* p = MercPtrs[i];
                if(!Alive(p))
                    continue;
                if(level < STATUS_RED)
                {
                    // forget the player's team: personal opplist, seen-opponents row, last known locations, and the
                    // noise and watched spot that reinforcements are handed on arrival (they point at the squad)
                    InitSoldierOppList(p);
                    p->aiData.bUnderFire = 0;
                    p->aiData.sNoiseGridno = NOWHERE;
                    p->aiData.ubNoiseVolume = 0;
                    p->bNoiseLevel = 0;
                    for(int w = 0; w < NUM_WATCHED_LOCS; ++w)
                    {
                        gsWatchedLoc[p->ubID][w] = NOWHERE;
                        gubWatchedLocPoints[p->ubID][w] = 0;
                        gfWatchedLocReset[p->ubID][w] = FALSE;
                    }
                }
                if(level < STATUS_RED && g_addedEnemy[p->ubID])
                {
                    // reinforcements arrive with seek-enemy orders; a garrison that suspects nothing stands guard or
                    // patrols, so the added men draw the orders a map would give them (seeded, like their edge)
                    static const INT8 garrison[4] = { ONGUARD, CLOSEPATROL, FARPATROL, SEEKENEMY };
                    p->aiData.bOrders = garrison[Random(4)];
                }
                p->aiData.bAlertStatus = (INT8)level;
                ++changed;
            }
            if(level < STATUS_RED)
            {
                gTacticalStatus.Team[ENEMY_TEAM].bAwareOfOpposition = FALSE;
                memset(gbPublicOpplist[ENEMY_TEAM], NOT_HEARD_OR_SEEN, sizeof gbPublicOpplist[ENEMY_TEAM]);
                gubPublicNoiseVolume[ENEMY_TEAM] = 0;
                gsPublicNoiseGridNo[ENEMY_TEAM] = NOWHERE;
                gbPublicNoiseLevel[ENEMY_TEAM] = 0;
                for(int i = 0; i < TOTAL_SOLDIERS; ++i)
                    gsPublicLastKnownOppLoc[ENEMY_TEAM][i] = NOWHERE;
            }
            if(level == STATUS_YELLOW)
                PlantSquadNoise();
            LogLine("enemy alert status set to %d on %d soldiers%s%s", level, changed,
                    level < STATUS_RED ? ", their knowledge of the squad wiped" : "",
                    level == STATUS_YELLOW ? ", the squad's position planted as their public noise" : "");
        }

        // The engine leaves turn-based mode after two turns in which nobody saw anybody, and only turn-based decisions
        // are exported. The game's forced-turn-mode option (ToggleTurnMode in Turn Based Input.cpp) is the switch that
        // keeps combat going; a loaded save brings its own settings, so it is set again for every battle.
        void HoldTurnBased()
        {
            // without HARNESS_FF the run is the real-time reference: the stock option that fast-forwards unseen
            // enemies (off in a fresh install, on in many profiles) would blur what "real time" means, so it is off
            if(!g_opts.fastForward)
                gGameSettings.fOptions[TOPTION_AUTO_FAST_FORWARD_MODE] = FALSE;
            if(!g_opts.forceTurnMode)
                return;
            gGameSettings.fOptions[TOPTION_TOGGLE_TURN_MODE] = TRUE;
            if(!(gTacticalStatus.uiFlags & INCOMBAT))
            {
                EnterCombatMode(OUR_TEAM);
                LogLine("forced turn mode: combat opened from real time");
            }
        }

        // The save decides who is on the map; HARNESS_ENEMIES tops the enemy side up with reinforcements
        // that walk in from a map edge, through the same call the strategic layer uses mid-battle.
        // Runs after SeedGameRandom, so the edge drawn for a battle follows from its seed.
        void TopUpEnemies()
        {
            if(g_opts.enemies <= 0)
                return;
            const int have = CountTeam(ENEMY_TEAM);
            int add = g_opts.enemies - have;
            if(add <= 0)
            {
                LogLine("enemies on the map: %d, nothing to add", have);
                return;
            }
            const int elites = add < g_opts.elites ? add : g_opts.elites;
            const int troops = add - elites;
            static const UINT8 edges[4] = { INSERTION_CODE_NORTH, INSERTION_CODE_EAST, INSERTION_CODE_SOUTH, INSERTION_CODE_WEST };
            const int pick = g_opts.enemyEdge ? g_opts.enemyEdge - 1 : (int)Random(4);
            bool before[TOTAL_SOLDIERS];
            for(int i = 0; i < TOTAL_SOLDIERS; ++i)
                before[i] = Alive(MercPtrs[i]);
            AddEnemiesToBattle(NULL, edges[pick], 0, (UINT8)troops, (UINT8)elites, 0, 0, 0, TRUE);
            for(int i = 0; i < TOTAL_SOLDIERS; ++i)
                g_addedEnemy[i] = !before[i] && Alive(MercPtrs[i]) && MercPtrs[i]->bTeam == ENEMY_TEAM;
            LogLine("enemies on the map: %d, added %d troops and %d elites from edge %d, now %d", have, troops, elites, pick + 1,
                    CountTeam(ENEMY_TEAM));
        }

        // HARNESS_NEURAL_ALL: every enemy on the map decides through NeuralPlanFactory from now on. The plan a
        // soldier already holds was built for his old bAIIndex, so it goes; HandleSoldierAI builds the next one
        // from the new index the first time he decides. Saves made by this run keep the index, as the neural
        // class does, so they must not be loaded by the stock executable (see patch 080).
        void RouteEnemiesToNeural()
        {
            if(!g_opts.neuralAll)
                return;
            int routed = 0;
            for(int i = gTacticalStatus.Team[ENEMY_TEAM].bFirstID; i <= gTacticalStatus.Team[ENEMY_TEAM].bLastID; ++i)
            {
                SOLDIERTYPE* p = MercPtrs[i];
                if(!Alive(p))
                    continue;
                p->bAIIndex = NEURAL_AI_INDEX;
                if(p->ai_masterplan_)
                {
                    delete p->ai_masterplan_;
                    p->ai_masterplan_ = NULL;
                }
                ++routed;
            }
            LogLine("neural all: %d enemies routed to NeuralPlanFactory", routed);
        }

        // The squad is the enemy AI's opposition, not the subject: it may cheat. Each of its turns starts with every
        // enemy entered in its public opplist as heard, so seek-enemy mercs close in instead of wandering the map.
        void Radar()
        {
            if(!g_opts.radar)
                return;
            for(int i = gTacticalStatus.Team[ENEMY_TEAM].bFirstID; i <= gTacticalStatus.Team[ENEMY_TEAM].bLastID; ++i)
            {
                const SOLDIERTYPE* p = MercPtrs[i];
                if(!Alive(p) || gbPublicOpplist[OUR_TEAM][i] > NOT_HEARD_OR_SEEN)
                    continue; // someone sees him already
                gbPublicOpplist[OUR_TEAM][i] = HEARD_THIS_TURN;
                gsPublicLastKnownOppLoc[OUR_TEAM][i] = p->sGridNo;
                gbPublicLastKnownOppLevel[OUR_TEAM][i] = p->pathing.bLevel;
            }
        }

        void DrivePlayerTeam()
        {
            if(!TurnBasedCombat() || gfInMsgBox)
                return;
            if(gTacticalStatus.ubCurrentTeam != OUR_TEAM)
            {
                g_ourTurnArmed = false;
                return;
            }
            if(g_playerTurns <= g_opts.quietTurns)
            {
                // the squad holds still: nothing for the enemy to hear, so its patrols keep deciding as green
                if(!g_ourTurnArmed || MsSince(g_turnSince) > 200.0)
                {
                    if(!g_ourTurnArmed)
                        LogLine("turn %d: squad holds still", g_playerTurns);
                    g_ourTurnArmed = true;
                    g_turnSince = Clock::now();
                    EndTurn(OUR_TEAM + 1);
                }
                return;
            }
            for(int i = gTacticalStatus.Team[OUR_TEAM].bFirstID; i <= gTacticalStatus.Team[OUR_TEAM].bLastID; ++i)
            {
                const SOLDIERTYPE* p = MercPtrs[i];
                if(Alive(p) && (p->flags.uiStatusFlags & SOLDIER_UNDERAICONTROL))
                    return;
            }
            if(g_ourTurnArmed)
            {
                // the list ran dry and nobody ended the turn (an interrupt, or a soldier that could not act)
                if(MsSince(g_turnSince) < 200.0)
                    return;
                LogLine("turn %d: list exhausted, ending the turn", g_playerTurns);
                g_ourTurnArmed = false;
                EndTurn(OUR_TEAM + 1);
                return;
            }
            g_ourTurnArmed = true;
            g_turnSince = Clock::now();
            Radar();
            if(BuildAIListForTeam(OUR_TEAM))
            {
                const UINT8 id = RemoveFirstAIListEntry();
                if(id != NOBODY)
                {
                    StartNPCAI(MercPtrs[id]);
                    ++g_handoffs;
                    if(g_handoffs <= 12 || (g_handoffs % 50) == 0)
                        LogLine("turn %d: merc %d takes the turn (ap=%d life=%d)", g_playerTurns, (int)id,
                                (int)MercPtrs[id]->bActionPoints, (int)MercPtrs[id]->stats.bLife);
                    return;
                }
            }
            LogLine("turn %d: nobody left to act, ending the turn", g_playerTurns);
            EndTurn(OUR_TEAM + 1);
        }

        void TrackTeamClock()
        {
            const int team = TurnBasedCombat() ? gTacticalStatus.ubCurrentTeam : -1;
            if(team != g_lastTeam)
            {
                if(g_lastTeam == ENEMY_TEAM)
                    g_enemyTurnMs += MsSince(g_turnSince);
                g_lastTeam = team;
                g_turnSince = Clock::now();
            }
        }

        void ResetBattleCounters()
        {
            memset(g_addedEnemy, 0, sizeof g_addedEnemy);
            g_battleOpen = false;
            g_battleEnded = false;
            memset(&g_end, 0, sizeof g_end);
            g_decisionsAtStart = DecisionsRecorded();
            g_damageDealtAtStart = DamageDealtRecorded();
            g_damageTakenAtStart = DamageTakenRecorded();
            g_playerTurns = 0;
            g_enemyTurns = 0;
            g_enemyTurnMs = 0.0;
            g_lastTeam = -1;
            g_ourTurnArmed = false;
            g_situationsAtStart = SituationExported();
            g_framesAtBattleStart = g_frames;
            g_boxesAnswered = 0;
            g_menusClosed = 0;
            g_handoffs = 0;
            g_framesNoEnemy = 0;
            g_episodes = 0;
            g_battleStart = Clock::now();
            g_lapsedAt = g_battleStart;
            g_turnSince = g_battleStart;
            g_battleClockStart = GetJA2Clock();
        }

        void WriteResult(const char* outcome)
        {
            if(g_lastTeam == ENEMY_TEAM)
                g_enemyTurnMs += MsSince(g_turnSince);
            const double wallMs = MsSince(g_battleStart);
            const UINT32 gameMs = GetJA2Clock() - g_battleClockStart;
            const unsigned situations = SituationExported() - g_situationsAtStart;
            FILE* f = fopen((g_opts.outDir + "\\results.jsonl").c_str(), "a");
            if(f)
            {
                fprintf(f,
                        "{\"battle\": %d, \"seed\": %u, \"map\": \"%s\", \"sector\": [%d, %d, %d], \"outcome\": \"%s\", "
                        "\"turns\": %d, \"enemy_turns\": %d, \"survivors\": {\"player\": %d, \"enemy\": %d, \"militia\": %d}, "
                        "\"damage_dealt\": %d, \"damage_taken\": %d, \"decisions\": %d, \"situations\": %u, "
                        "\"wall_ms\": %.0f, \"enemy_turn_ms\": %.0f, \"game_ms\": %u, \"frames\": %u, \"fast_forward\": %s, \"boxes\": %d, \"menus\": %d, "
                        "\"start\": {\"player\": %d, \"enemy\": %d}, \"episodes\": %d}\n",
                        g_battle + 1, g_opts.seed + (unsigned)g_battle, SituationMapName().c_str(),
                        (int)gWorldSectorX, (int)gWorldSectorY, (int)gbWorldSectorZ, outcome,
                        g_playerTurns, g_enemyTurns, (int)g_end.survivors_player, (int)g_end.survivors_enemy, (int)g_end.survivors_militia,
                        DamageDealtRecorded() - g_damageDealtAtStart, DamageTakenRecorded() - g_damageTakenAtStart,
                        (int)(DecisionsRecorded() - g_decisionsAtStart), situations,
                        wallMs, g_enemyTurnMs, gameMs, g_frames - g_framesAtBattleStart, g_opts.fastForward ? "true" : "false", g_boxesAnswered,
                        g_menusClosed, g_startPlayer, g_startEnemy, g_episodes);
                fclose(f);
            }
            LogLine("battle %d %s turns=%d survivors p=%d e=%d m=%d situations=%u wall_ms=%.0f enemy_turn_ms=%.0f game_ms=%u frames=%u",
                    g_battle + 1, outcome, g_playerTurns, (int)g_end.survivors_player, (int)g_end.survivors_enemy,
                    (int)g_end.survivors_militia, situations, wallMs, g_enemyTurnMs, gameMs, g_frames - g_framesAtBattleStart);
        }

        /// The log's outcome code for a battle the harness ends itself; the decided ones keep their verdict.
        UINT8 OutcomeCode(const char* outcome)
        {
            if(strcmp(outcome, "player_won") == 0) return TACNN_OUTCOME_PLAYER_WON;
            if(strcmp(outcome, "enemy_won") == 0) return TACNN_OUTCOME_ENEMY_WON;
            return TACNN_OUTCOME_ABORTED;
        }

        void FinishBattle(const char* outcome)
        {
            if(!g_battleEnded)
            {
                CountSurvivors(g_end);
                // the engine did not close the episode: give the log its episode_end with the harness's verdict
                CloseEpisode(OutcomeCode(outcome));
            }
            WriteResult(outcome);
            const std::string profile = DecisionProfileSummary();
            if(!profile.empty())
                LogLine("battle %d %s", g_battle + 1, profile.c_str());
            // the "your squad is dead" timer would drop the world under the next load
            SetCustomizableTimerCallbackAndDelay(0, NULL, TRUE);
            ++g_battle;
            if(g_battle >= g_opts.battles)
            {
                SetState(HS_DONE);
                return;
            }
            SetState(HS_RELOAD);
        }

        bool RequestLoad()
        {
            g_loadSeen = g_loadGeneration;
            if(guiCurrentScreen == MAINMENU_SCREEN)
                return MainMenuAutoLoad(g_opts.slot) != FALSE;
            if(guiCurrentScreen == GAME_SCREEN || guiCurrentScreen == MAP_SCREEN)
                return DoQuickLoadSlot(g_opts.slot) != FALSE;
            return false;
        }

        void Arm()
        {
            const unsigned seed = g_opts.seed + (unsigned)g_battle;
            gGameExternalOptions.uiNeuralAISeed = seed;
            gGameExternalOptions.fNeuralExportSituations = TRUE;
            strncpy(gGameExternalOptions.szNeuralExportDir, BattleDir(g_battle).c_str(), sizeof gGameExternalOptions.szNeuralExportDir - 1);
            gGameExternalOptions.szNeuralExportDir[sizeof gGameExternalOptions.szNeuralExportDir - 1] = 0;
            gGameExternalOptions.fNeuralLog = TRUE;
            if(g_opts.logPlayer)
                gGameExternalOptions.fNeuralLogPlayer = TRUE; // the squad's decisions are the behaviour-cloning data
            strncpy(gGameExternalOptions.szNeuralLogDir, (g_opts.outDir + "\\tacnn-log").c_str(), sizeof gGameExternalOptions.szNeuralLogDir - 1);
            gGameExternalOptions.szNeuralLogDir[sizeof gGameExternalOptions.szNeuralLogDir - 1] = 0;
            if(!g_opts.neuralMode.empty())
            {
                strncpy(gGameExternalOptions.szNeuralMode, g_opts.neuralMode.c_str(), sizeof gGameExternalOptions.szNeuralMode - 1);
                gGameExternalOptions.szNeuralMode[sizeof gGameExternalOptions.szNeuralMode - 1] = 0;
            }
            if(!g_opts.neuralModel.empty())
            {
                strncpy(gGameExternalOptions.szNeuralModel, g_opts.neuralModel.c_str(), sizeof gGameExternalOptions.szNeuralModel - 1);
                gGameExternalOptions.szNeuralModel[sizeof gGameExternalOptions.szNeuralModel - 1] = 0;
            }
            ApplySettings();
            // a soldier whose action never completes holds the AI until DEAD_LOCK_DELAY wall-clock seconds pass
            gGameExternalOptions.gubDeadLockDelay = (UINT8)g_opts.deadlockSeconds;
            SeedGameRandom(seed);
            ResetBattleCounters();
            KeepClockRunning();
            OrderSquad();
            TopUpEnemies();
            RouteEnemiesToNeural();
            SetEnemyAlert();
            HoldTurnBased();
            g_startPlayer = CountTeam(OUR_TEAM);
            g_startEnemy = CountTeam(ENEMY_TEAM);
            EnsureBattleOpen();
            if(TurnBasedCombat() && gTacticalStatus.ubCurrentTeam == OUR_TEAM)
                g_playerTurns = 1; // the turn the save was made in; BeginTeamTurn counts the rest
            LogLine("battle %d armed seed=%u combat=%d team=%d player=%d enemy=%d export=%s", g_battle + 1, seed, TurnBasedCombat() ? 1 : 0,
                    (int)gTacticalStatus.ubCurrentTeam, g_startPlayer, g_startEnemy, gGameExternalOptions.szNeuralExportDir);
            SetState(HS_BATTLE);
        }

        void TickBattle()
        {
            TrackTeamClock();
            CloseTacticalMenus();
            KeepClockRunning();
            if(g_battleEnded)
            {
                FinishBattle(OutcomeName(g_end.outcome));
                return;
            }
            TacnnEpisodeEnd now;
            memset(&now, 0, sizeof now);
            CountSurvivors(now);
            if(now.survivors_player == 0 && now.survivors_militia == 0)
            {
                g_end = now;
                g_end.outcome = TACNN_OUTCOME_ENEMY_WON;
                FinishBattle("enemy_won");
                return;
            }
            if(now.survivors_enemy == 0)
            {
                // a few frames of grace so ExitCombatMode can report the episode first
                if(++g_framesNoEnemy > 30)
                {
                    g_end = now;
                    g_end.outcome = TACNN_OUTCOME_PLAYER_WON;
                    FinishBattle("player_won");
                    return;
                }
            }
            else
                g_framesNoEnemy = 0;
            if(g_playerTurns > g_opts.maxTurns)
            {
                g_end = now;
                FinishBattle("turn_cap");
                return;
            }
            if(MsSince(g_battleStart) > 1000.0 * g_opts.timeoutSeconds)
            {
                g_end = now;
                FinishBattle("timeout");
                return;
            }
            if(!TurnBasedCombat())
            {
                // real time: nobody sees anybody, both sides hunt under their AI until combat resumes;
                // a save that never opens combat, or a hunt that never finds anyone, ends here
                if(MsSince(g_lapsedAt) > 1000.0 * g_opts.lapseSeconds)
                {
                    g_end = now;
                    FinishBattle(g_episodes ? "stalemate" : "no_combat");
                }
                return;
            }
            DrivePlayerTeam();
        }
    }

    // -------------------------------------------------------------------- public

    void HarnessSetOptions(const HarnessOptions& options)
    {
        g_opts = options;
        g_commandLine = options.enabled;
        if(g_opts.outDir.empty())
            g_opts.outDir = "tacnn-harness";
        if(g_opts.battles < 1)
            g_opts.battles = 1;
        if(g_opts.maxTurns < 1)
            g_opts.maxTurns = 1;
        if(g_opts.timeoutSeconds < 10)
            g_opts.timeoutSeconds = 10;
        if(g_opts.slot < 0)
            g_opts.slot = 0;
        if(g_opts.fastForwardMicros < 1)
            g_opts.fastForwardMicros = 1;
        if(g_opts.fastForwardMicros > 10000)
            g_opts.fastForwardMicros = 10000;
        if(g_opts.deadlockSeconds < 1)
            g_opts.deadlockSeconds = 1;
        if(g_opts.deadlockSeconds > 50)
            g_opts.deadlockSeconds = 50;
        if(g_opts.lapseSeconds < 5)
            g_opts.lapseSeconds = 5;
        if(g_opts.quietTurns < 0)
            g_opts.quietTurns = 0;
        if(g_opts.quietTurns >= g_opts.maxTurns)
            g_opts.quietTurns = g_opts.maxTurns - 1;
        if(g_opts.enemies < 0)
            g_opts.enemies = 0;
        if(g_opts.enemies > 32)
            g_opts.enemies = 32;
        if(g_opts.elites < 0)
            g_opts.elites = 0;
        if(g_opts.enemyEdge < 0 || g_opts.enemyEdge > 4)
            g_opts.enemyEdge = 0;
        if(g_opts.enemyAlert > STATUS_BLACK)
            g_opts.enemyAlert = STATUS_BLACK;
    }

    bool HarnessActive()
    {
        return g_commandLine || gGameExternalOptions.fNeuralHarness != FALSE;
    }

    bool HarnessDrivesPlayerTeam()
    {
        return g_state == HS_BATTLE || g_state == HS_ARMING;
    }

    bool HarnessSkipsRender()
    {
        return g_state != HS_OFF && g_state != HS_DONE && !g_opts.render;
    }

    void HarnessLapBegin()
    {
        if(g_state == HS_OFF || g_state == HS_DONE)
            return;
        g_lapStart = Clock::now();
        g_lapping = true;
    }

    void HarnessLap(int bucket)
    {
        if(!g_lapping || bucket < 0 || bucket >= HB_COUNT)
            return;
        const Clock::time_point now = Clock::now();
        g_lapMs[bucket] += std::chrono::duration<double, std::milli>(now - g_lapStart).count();
        g_lapStart = now;
    }

    void HarnessProfileAdd(int bucket, double ms)
    {
        if(bucket >= 0 && bucket < HB_COUNT)
            g_lapMs[bucket] += ms;
    }

    HarnessProfileScope::HarnessProfileScope(int bucket)
        : bucket_(bucket), active_(g_lapping), start_(0)
    {
        if(!active_)
            return;
        start_ = Clock::now().time_since_epoch().count();
        if(bucket_ >= 0 && bucket_ < HB_COUNT)
            ++g_lapCount[bucket_];
    }

    HarnessProfileScope::~HarnessProfileScope()
    {
        if(!active_)
            return;
        const Clock::duration elapsed = Clock::now().time_since_epoch() - Clock::duration(start_);
        HarnessProfileAdd(bucket_, std::chrono::duration<double, std::milli>(elapsed).count());
    }

    bool HarnessTick()
    {
        if(!HarnessActive())
            return false;
        if(!g_initialised)
            Init();
        if(g_state == HS_DONE)
        {
            if(gfProgramIsRunning)
                Quit(0);
            return false;
        }
        if(g_opts.fastForward && !gfHarnessFastForward)
            gfHarnessFastForward = TRUE;
        ++g_frames;
        AnswerMessageBox();

        const double inState = MsSince(g_stateSince);
        const double sinceHeartbeat = MsSince(g_heartbeat);
        if(sinceHeartbeat > 5000.0)
        {
            g_heartbeat = Clock::now();
            LogLine("state=%d screen=%u pending=%u msgbox=%d fade=%d/%d world=%d first=%d flags=0x%x team=%d turns=%d clock=%u fps=%.0f",
                    g_state, guiCurrentScreen, guiPendingScreen, gfInMsgBox ? 1 : 0, gfFadeIn ? 1 : 0, gfFadeOut ? 1 : 0,
                    gfWorldLoaded ? 1 : 0, fFirstTimeInGameScreen ? 1 : 0, gTacticalStatus.uiFlags, (int)gTacticalStatus.ubCurrentTeam,
                    g_playerTurns, GetJA2Clock(), (g_frames - g_framesAtHeartbeat) * 1000.0 / sinceHeartbeat);
            const unsigned frames = g_frames - g_framesAtHeartbeat;
            g_framesAtHeartbeat = g_frames;
            if(frames > 0 && g_state == HS_BATTLE)
            {
                LogLine("  frame: ms/frame overhead=%.2f (ai=%.2f decide=%.2f in %u decisions, %.1f ms each) ui=%.2f messages=%.2f world=%.2f interface=%.2f radar=%.2f overlays=%.2f rest=%.2f",
                        g_lapMs[HB_OVERHEAD] / frames, g_lapMs[HB_AI] / frames, g_lapMs[HB_DECIDE] / frames, g_lapCount[HB_DECIDE],
                        g_lapCount[HB_DECIDE] ? g_lapMs[HB_DECIDE] / g_lapCount[HB_DECIDE] : 0.0, g_lapMs[HB_UI] / frames,
                        g_lapMs[HB_MESSAGES] / frames, g_lapMs[HB_WORLD] / frames, g_lapMs[HB_INTERFACE] / frames,
                        g_lapMs[HB_RADAR] / frames, g_lapMs[HB_OVERLAYS] / frames, g_lapMs[HB_REST] / frames);
            }
            for(int b = 0; b < HB_COUNT; ++b)
            {
                g_lapMs[b] = 0;
                g_lapCount[b] = 0;
            }
            if(g_state == HS_BATTLE && TurnBasedCombat())
            {
                LogLine("  battle: pauseAI=%d paused=%d pauseClock=%d abc=%d outOfTurn=%d sighting=%d explq=%d/%d fadeInit=%d ourArmed=%d",
                        gfPauseAllAI ? 1 : 0, gfGamePaused ? 1 : 0, gfPauseClock ? 1 : 0, (int)gTacticalStatus.ubAttackBusyCount,
                        (int)gubOutOfTurnPersons, gTacticalStatus.fEnemySightingOnTheirTurn ? 1 : 0, (int)gubElementsOnExplosionQueue,
                        gfExplosionQueueActive ? 1 : 0, gfFadeInitialized ? 1 : 0, g_ourTurnArmed ? 1 : 0);
                for(int i = gTacticalStatus.Team[OUR_TEAM].bFirstID; i <= gTacticalStatus.Team[OUR_TEAM].bLastID; ++i)
                {
                    const SOLDIERTYPE* p = MercPtrs[i];
                    if(!p || !p->bActive)
                        continue;
                    LogLine("  merc %d: life=%d ap=%d insector=%d anim=%d aictl=%d moved=%d action=%d turn=%d newsit=%d orders=%d alert=%d",
                            i, (int)p->stats.bLife, (int)p->bActionPoints, (int)p->bInSector, (int)p->usAnimState,
                            (p->flags.uiStatusFlags & SOLDIER_UNDERAICONTROL) ? 1 : 0, (int)p->aiData.bMoved, (int)p->aiData.bAction,
                            p->flags.fTurnInProgress ? 1 : 0, (int)p->aiData.bNewSituation, (int)p->aiData.bOrders, (int)p->aiData.bAlertStatus);
                }
            }
            if(guiCurrentScreen == SAVE_LOAD_SCREEN)
                LogLine("  save/load: entry=%d exit=%d exitScreen=%u buttons=%d uponEntry=%d fadingOut=%d selected=%d lastSlot=%d slotOk=%d fadeOutDone=%d fadeInit=%d",
                        gfSaveLoadScreenEntry ? 1 : 0, gfSaveLoadScreenExit ? 1 : 0, guiSaveLoadExitScreen,
                        gfSaveLoadScreenButtonsCreated ? 1 : 0, gfLoadGameUponEntry ? 1 : 0, gfStartedFadingOut ? 1 : 0,
                        gbSelectedSaveLocation, (int)gGameSettings.bLastSavedGameSlot,
                        (g_opts.slot >= 0 && g_opts.slot < NUM_SAVE_GAMES) ? (gbSaveGameArray[g_opts.slot] ? 1 : 0) : -1,
                        gfFadeOutDone ? 1 : 0, gfFadeInitialized ? 1 : 0);
        }
        switch(g_state)
        {
        case HS_MENU:
            if(guiCurrentScreen == MAINMENU_SCREEN || (GameScreenReady() && !gfInMsgBox))
            {
                if(RequestLoad())
                {
                    LogLine("load requested from screen %u", guiCurrentScreen);
                    SetState(HS_LOADING);
                }
            }
            else if(inState > 180000.0)
            {
                LogLine("no main menu after %.0f ms, giving up", inState);
                SetState(HS_DONE);
            }
            break;

        case HS_LOADING:
            if(g_loadGeneration != g_loadSeen && GameScreenReady() && !gfInMsgBox)
            {
                if(++g_stableFrames >= 3)
                {
                    LogLine("world loaded: sector %d,%d,%d combat=%d", (int)gWorldSectorX, (int)gWorldSectorY, (int)gbWorldSectorZ,
                            TurnBasedCombat() ? 1 : 0);
                    SetState(HS_ARMING);
                }
            }
            else if(g_loadGeneration != g_loadSeen && guiCurrentScreen == MAP_SCREEN && guiPendingScreen == NO_PENDING_SCREEN
                    && gfWorldLoaded && !gfFadeIn && !gfFadeOut && !gfInMsgBox)
            {
                // a save made from the map screen comes back there; press the tactical button
                if(++g_stableFrames == 3)
                {
                    LogLine("map screen after load, leaving for the sector");
                    RequestTriggerExitFromMapscreen(MAP_EXIT_TO_TACTICAL);
                }
                else if(g_stableFrames > 600)
                    g_stableFrames = 0;
            }
            else
            {
                g_stableFrames = 0;
                if(g_loadGeneration == g_loadSeen && guiCurrentScreen == SAVE_LOAD_SCREEN && !gfInMsgBox && inState > 500.0)
                {
                    // the main menu's Load opened the save/load screen but the load did not start there;
                    // pick the slot and confirm like the player would
                    if(SaveLoadScreenAutoLoad(g_opts.slot))
                        LogLine("save/load screen idle, confirmed slot %d", g_opts.slot);
                }
                if(inState > 180000.0)
                {
                    LogLine("load did not finish after %.0f ms (screen %u, generation %u/%u), giving up", inState,
                            guiCurrentScreen, g_loadGeneration, g_loadSeen);
                    SetState(HS_DONE);
                }
            }
            break;

        case HS_ARMING:
            Arm();
            break;

        case HS_BATTLE:
            if(guiCurrentScreen == GAME_SCREEN || guiCurrentScreen == MSG_BOX_SCREEN)
                TickBattle();
            else if(inState > 1000.0 * g_opts.timeoutSeconds)
            {
                LogLine("left the game screen (%u) and never came back", guiCurrentScreen);
                CountSurvivors(g_end);
                FinishBattle("aborted");
            }
            break;

        case HS_RELOAD:
            if(!gfInMsgBox && !gfFadeIn && !gfFadeOut && guiPendingScreen == NO_PENDING_SCREEN)
            {
                if(RequestLoad())
                {
                    LogLine("reload requested for battle %d", g_battle + 1);
                    SetState(HS_LOADING);
                }
                else if(inState > 60000.0)
                {
                    LogLine("cannot reload from screen %u, giving up", guiCurrentScreen);
                    SetState(HS_DONE);
                }
            }
            break;

        default:
            break;
        }
        return HarnessSkipsRender();
    }

    void HarnessOnBattleStart()
    {
        if(g_state != HS_BATTLE && g_state != HS_ARMING)
            return;
        g_battleOpen = true;
        g_battleEnded = false;
        ++g_episodes;
        LogLine("battle %d: combat episode %d begins after %.0f ms", g_battle + 1, g_episodes, MsSince(g_battleStart));
    }

    void HarnessOnBattleEnd(const TacnnEpisodeEnd& end)
    {
        if(g_state != HS_BATTLE || !g_battleOpen)
            return;
        g_battleOpen = false;
        if(end.outcome == TACNN_OUTCOME_PLAYER_WON || end.outcome == TACNN_OUTCOME_ENEMY_WON)
        {
            g_end = end;
            g_battleEnded = true;
            return;
        }
        // combat lapsed into real time with both sides alive; the battle goes on until it resumes or the lapse runs out
        g_lapsedAt = Clock::now();
        LogLine("battle %d: combat episode %d lapsed (player %d, enemy %d alive), hunting in real time", g_battle + 1, g_episodes,
                (int)end.survivors_player, (int)end.survivors_enemy);
    }

    void HarnessOnBeginTeamTurn(UINT8 ubTeam)
    {
        if(g_state != HS_BATTLE)
            return;
        if(ubTeam == OUR_TEAM)
        {
            ++g_playerTurns;
            g_ourTurnArmed = false;
        }
        else if(ubTeam == ENEMY_TEAM)
        {
            ++g_enemyTurns;
            // runs before this turn's DecayPublicOpplist, which takes the planted noise down to 8
            if(g_opts.enemyAlert == STATUS_YELLOW && !gTacticalStatus.Team[ENEMY_TEAM].bAwareOfOpposition)
                PlantSquadNoise();
        }
    }

    void HarnessOnSavedGameLoaded()
    {
        ++g_loadGeneration;
    }

    void HarnessOnInterruptEnded()
    {
        if(g_state != HS_BATTLE || !HarnessDrivesPlayerTeam())
            return;
        for(int i = gTacticalStatus.Team[OUR_TEAM].bFirstID; i <= gTacticalStatus.Team[OUR_TEAM].bLastID; ++i)
        {
            SOLDIERTYPE* p = MercPtrs[i];
            if(!Alive(p) || !(p->flags.uiStatusFlags & SOLDIER_UNDERAICONTROL) || p->aiData.bAction == AI_ACTION_NONE)
                continue;
            // the engine restarts an AI team after an interrupt through StartNPCAI; the player's team only gets its UI back
            LogLine("turn %d: interrupt over, merc %d drops action %d", g_playerTurns, i, (int)p->aiData.bAction);
            CancelAIAction(p, FORCE);
        }
    }
}
