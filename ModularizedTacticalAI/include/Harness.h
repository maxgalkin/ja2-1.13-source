/**
 * @file
 * @author ja2mod
 *
 * Added by the ja2mod fork on 2026-09-15; see CHANGES-ja2mod.md.
 *
 * The fast-forward battle harness. When it is switched on (the NEURAL_HARNESS
 * key of Ja2_Options.ini, or -NEURAL_HARNESS=1 on the command line) the game
 * loads a save on its own from the main menu, lets the stock AI play both sides
 * of the battle with the clock running as fast as the frames allow, records
 * how the battle ended, reloads the same save for the next battle, and quits
 * with exit code 0 after the requested number of battles. Nothing here runs
 * while the harness is off; every hook asks HarnessActive() first.
 *
 * All harness keys are read from the [Ja2 Settings] section of Ja2.ini, which
 * the existing command-line override in sgp.cpp (-KEY=VALUE) can set:
 *
 *   NEURAL_HARNESS      1 turns the harness on for this process
 *   HARNESS_SLOT        save slot to load, 0 = QuickSave.sav of the profile (default 0)
 *   HARNESS_BATTLES     how many times the save is played (default 1)
 *   HARNESS_SEED        NEURAL_AI_SEED of the first battle; battle n uses seed + n (default 1)
 *   HARNESS_OUT         folder for results.jsonl, harness.log, the situation
 *                       exports and the decision log (default tacnn-harness)
 *   HARNESS_FF          0 keeps the stock clock for a speed baseline (default 1)
 *   HARNESS_FF_US       microseconds of real time per 10 ms of game clock while
 *                       fast-forwarding; the stock fast forward uses 1000 (10x),
 *                       the default 100 is 100x (default 100)
 *   HARNESS_RENDER      0 skips the world render and the screen blit (default 0)
 *   HARNESS_MAX_TURNS   a battle still running after this many player turns is
 *                       recorded as "turn_cap" and abandoned (default 40)
 *   HARNESS_TIMEOUT_S   wall-clock limit per battle, recorded as "timeout" (default 900)
 *   HARNESS_DEADLOCK_S  DEAD_LOCK_DELAY for the run: wall-clock seconds a soldier may
 *                       hold the AI before the engine ends his turn (default 5)
 *   HARNESS_LAPSE_S     when combat lapses into real time with both sides alive, both AIs
 *                       hunt for this many wall-clock seconds before the battle is recorded
 *                       as "stalemate" ("no_combat" if it never opened) (default 45)
 *   HARNESS_ENEMIES     top the enemy side up to this many soldiers at battle start with
 *                       reinforcements that walk in from a map edge, the way the strategic
 *                       layer sends them mid-battle (default 0 = the save as it is)
 *   HARNESS_ELITES      how many of the added soldiers are elites, the rest are troops (default 0)
 *   HARNESS_ENEMY_EDGE  map edge they enter from: 0 = drawn from the battle seed, 1 north,
 *                       2 east, 3 south, 4 west (default 0)
 *   HARNESS_ENEMY_ALERT every enemy's alert status when the battle is armed, 0 green to 3 black,
 *                       with their knowledge of the squad wiped below red; 1 also plants the
 *                       squad's position as the team's public noise, again every enemy turn
 *                       until they notice the squad (default -1 = untouched)
 *   HARNESS_FORCE_TB    0 lets combat lapse into real time as the stock game does (default 1)
 *   HARNESS_QUIET_TURNS the squad ends its first N turns without moving (default 0)
 *   HARNESS_RADAR       0 makes the squad hunt on its own knowledge (default 1)
 *   HARNESS_AI_LOG      1 keeps the stock AI decision log (Logs\AI_Decisions.txt), whose
 *                       file opens are most of what a decision costs (default 0)
 *   HARNESS_NEURAL_ALL  1 routes every enemy on the map through NeuralPlanFactory (bAIIndex
 *                       NEURAL_AI_INDEX) when the battle is armed, whatever his class, so a
 *                       run measures the policy on the whole enemy team (default 0)
 *   HARNESS_NEURAL_MODE overrides NEURAL_MODE of Ja2_Options.ini for the run: off, sidecar
 *                       or embedded (default empty = the INI's value)
 *   HARNESS_NEURAL_MODEL overrides NEURAL_MODEL likewise (default empty = the INI's value)
 *   HARNESS_LOG_PLAYER  1 records the squad's decisions in the decision log too (NEURAL_LOG_PLAYER
 *                       forced on), 0 leaves the INI's value (default 1)
 */

#ifndef JA2MOD_HARNESS_H
#define JA2MOD_HARNESS_H

#include "../../sgp/types.h"
#include <string>

struct TacnnEpisodeEnd;

namespace tacnn
{
    struct HarnessOptions
    {
        bool enabled;
        int slot;
        int battles;
        unsigned seed;
        std::string outDir;
        bool fastForward;
        int fastForwardMicros;
        bool render;
        int maxTurns;
        int timeoutSeconds;
        int deadlockSeconds;
        int lapseSeconds;
        int enemies;
        int elites;
        int enemyEdge;
        /// HARNESS_ENEMY_ALERT: -1 leaves the enemy team as the save and the top-up made it; 0..3 sets every enemy's
        /// alert status (green, yellow, red, black) when the battle is armed. Below red their knowledge of the player's
        /// team is wiped too, so the decisions before they notice anyone are green and yellow ones. Yellow needs a
        /// noise to stay yellow, so 1 plants the squad's position as the team's public noise every enemy turn until
        /// the team becomes aware of the squad.
        int enemyAlert;
        /// HARNESS_FORCE_TB (default on): keep the battle turn-based even while neither side knows where the other is,
        /// through the game's own forced-turn-mode option. Without it the engine drops to real time after two quiet
        /// turns, and the situation exporter only records turn-based decisions.
        bool forceTurnMode;
        /// HARNESS_QUIET_TURNS: the squad ends its first N turns without moving. Footsteps are what a green enemy
        /// team hears first, so with the squad silent its patrols keep deciding as green until one of them walks
        /// into a merc; 0 (default) lets the squad hunt from the first turn.
        int quietTurns;
        /// HARNESS_RADAR (default on): every one of the squad's turns starts with the enemy positions entered in the
        /// squad's public opplist as "heard this turn", so seek-enemy mercs walk towards the enemy instead of
        /// wandering. A cheat for the opposition only; the enemy's own knowledge is untouched.
        bool radar;
        /// HARNESS_AI_LOG (default off): keep the stock AI decision log (Logs\AI_Decisions.txt plus one file per
        /// soldier). Stock DebugAI opens, appends and closes both files for every one of the ~35 lines a red or
        /// black decision writes, about 4.5 ms of wall time per line on Windows, which is nearly the whole cost of a
        /// decision. Off, the harness clears gfLogsEnabled at start; the decisions themselves are untouched.
        bool aiLog;
        /// HARNESS_NEURAL_ALL (default off): when the battle is armed, every enemy soldier gets bAIIndex
        /// NEURAL_AI_INDEX and loses his plan, so NeuralPlanFactory builds the next one and the active policy
        /// (NEURAL_MODE) answers his decisions. Otherwise only the neural class, which a save rarely holds.
        bool neuralAll;
        /// HARNESS_NEURAL_MODE / HARNESS_NEURAL_MODEL: values written over the INI's NEURAL_MODE and
        /// NEURAL_MODEL before the settings are applied for a battle; empty leaves the INI's value.
        std::string neuralMode;
        std::string neuralModel;
        /// HARNESS_LOG_PLAYER (default on): the decision log also records the squad's decisions (NEURAL_LOG_PLAYER
        /// forced on for the run), the data the player-like opponent is cloned from. 0 leaves the INI's value.
        bool logPlayer;

        HarnessOptions()
            : enabled(false), slot(0), battles(1), seed(1), outDir("tacnn-harness"),
              fastForward(true), fastForwardMicros(100), render(false), maxTurns(40), timeoutSeconds(900),
              deadlockSeconds(5), lapseSeconds(45), enemies(0), elites(0), enemyEdge(0), enemyAlert(-1),
              forceTurnMode(true), quietTurns(0), radar(true), aiLog(false), neuralAll(false), logPlayer(true) {}
    };

    /// Called once from GetRuntimeSettings with the command-line-merged [Ja2 Settings] keys.
    void HarnessSetOptions(const HarnessOptions& options);

    /// True when the harness runs this process (command line, or NEURAL_HARNESS in the INI once that is read).
    bool HarnessActive();

    /// Once per frame from GameLoop, before the screen handler. Returns true when the frame's screen blit can be skipped.
    bool HarnessTick();

    /// AIMain.cpp: let HandleSoldierAI drive player soldiers like any AI team.
    bool HarnessDrivesPlayerTeam();

    /// gamescreen.cpp: leave the world unrendered this frame.
    bool HarnessSkipsRender();

    /// Where a tactical frame's time goes, for the heartbeat. MainGameScreenHandle calls HarnessLapBegin at
    /// its start and HarnessLap(bucket) after each of its sections; the harness adds the time since the
    /// previous lap to the bucket and logs the per-frame averages every heartbeat. No-ops while no run is on.
    enum HarnessBucket
    {
        HB_OVERHEAD,    // environment, physics, bullets, ExecuteOverhead (animation and AI)
        HB_UI,          // HandleTacticalUI, dialogue, game events
        HB_MESSAGES,    // top messages, interface set-up, scrolling
        HB_WORLD,       // RenderWorld (skipped without HARNESS_RENDER)
        HB_INTERFACE,   // RenderTopmostTacticalInterface: the panel, buttons, overlays above soldiers
        HB_RADAR,       // RenderRadarScreen, ResetInterface
        HB_OVERLAYS,    // ExecuteVideoOverlays
        HB_REST,        // dialogue, fast help, dirty rects, fades
        HB_AI,          // inside HB_OVERHEAD: HandleSoldierAI for every soldier
        HB_DECIDE,      // inside HB_AI: the decision itself (the plan's execute)
        HB_COUNT
    };
    void HarnessLapBegin();
    void HarnessLap(int bucket);
    void HarnessProfileAdd(int bucket, double ms);

    /// RAII: adds the scope's lifetime to a bucket. For sections nested inside a lap.
    class HarnessProfileScope
    {
        public:
            explicit HarnessProfileScope(int bucket);
            ~HarnessProfileScope();
        private:
            int bucket_;
            bool active_;
            long long start_;
            HarnessProfileScope(const HarnessProfileScope&);
            HarnessProfileScope& operator=(const HarnessProfileScope&);
    };

    /// NeuralHooks.cpp: a battle (EnterCombatMode, or a save loaded mid-battle) has begun.
    void HarnessOnBattleStart();

    /// NeuralHooks.cpp: ExitCombatMode ran; the summary is what the decision log records.
    void HarnessOnBattleEnd(const TacnnEpisodeEnd& end);

    /// NeuralHooks.cpp: BeginTeamTurn ran for this team.
    void HarnessOnBeginTeamTurn(UINT8 ubTeam);
    /// Tactical/TeamTurns.cpp EndInterrupt, when the player's team regains control: an interrupt cut a driven
    /// merc's action short, and nothing in the engine tells his AI, so the harness cancels it and he decides afresh.
    void HarnessOnInterruptEnded();

    /// NeuralHooks.cpp: LoadSavedGame reached the belief store, so a new world is in.
    void HarnessOnSavedGameLoaded();
}

#endif
