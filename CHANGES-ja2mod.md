# Changes made by the ja2mod fork

This is a fork of [1dot13/source](https://github.com/1dot13/source) at commit
`d038ead5645a51b5989dff32670fdae00b47a09f` (2025-09-27), the revision that produced the
Jagged Alliance 2 v1.13 build installed on the machine this fork is developed against.

The upstream code is licensed under the Strategy First Inc. Source Code License
Agreement, a verbatim copy of which is in `SFI Source Code license agreement.txt`.
Clause 4 of that licence requires modified files to carry prominent notices, and that
the changes are dated. Every file below therefore carries a comment marked `ja2mod`
next to the change, and this table records what changed and when.

The fork exists to add a hook for a learned tactical policy to the enemy AI. It is not
distributed; it builds `ja2mod.exe`, which is deployed next to the stock `ja2.exe` by
`tools/ja2mod.py deploy` in the ja2mod repository.

## Changed and added files

| Date | File | Change |
| --- | --- | --- |
| 2026-09-12 | `CMakePresets.json` | Added. Committed configure presets (upstream leaves this file to the developer and gitignores it) so a fresh checkout configures without manual steps. |
| 2026-09-12 | `.gitignore` | Stopped ignoring `/CMakePresets.json`. |
| 2026-09-12 | `cmake/CopyUserPresetTemplate.cmake` | The missing-preset branch warns instead of raising `FATAL_ERROR`, so the first configure of a checkout is not fatal. |
| 2026-09-12 | `Ja2/GameVersion.cpp` → `Ja2/GameVersion.cpp.in` | Renamed. Upstream substitutes the `@Version@` and `@Build@` placeholders with `sed` in its GitHub Actions workflow, so a build run anywhere else ships an executable whose version reads `@Version@`. |
| 2026-09-12 | `Ja2/CMakeLists.txt` | Substitutes `@Version@`/`@Build@` from the `JA2MOD_VERSION` and `JA2MOD_BUILD` CMake variables with `configure_file`, and compiles the generated file. |
| 2026-09-12 | `Ja2/GameSettings.h` | Added `ubNeuralEliteFraction` to `GAME_EXTERNAL_OPTIONS` and the `NEURAL_AI_INDEX` constant. |
| 2026-09-12 | `Ja2/GameSettings.cpp` | Reads the `NEURAL_ELITE_FRACTION` key from the `Tactical Enemy Role Settings` section of `Ja2_Options.INI`. Default 0, range 0-100. |
| 2026-09-12 | `Tactical/Soldier Create.cpp` | Sets `bAIIndex = NEURAL_AI_INDEX` on that percentage of newly created enemy elites. |
| 2026-09-12 | `Tactical/Soldier Control.cpp` | `SOLDIERTYPE::DeleteSoldier` frees `ai_masterplan_`, so a reused soldier slot cannot inherit a plan built for the previous occupant's `bAIIndex`. |
| 2026-09-12 | `ModularizedTacticalAI/include/NeuralPlan.h` | Added. `CustomPolicy` interface, the active-policy accessors, and the `NeuralPlan` product. |
| 2026-09-12 | `ModularizedTacticalAI/src/NeuralPlan.cpp` | Added. The declining default policy and `NeuralPlan::execute`, which asks the policy and otherwise runs `LegacyAIPlan::execute`. |
| 2026-09-12 | `ModularizedTacticalAI/include/NeuralPlanFactory.h` | Added. The factory that produces `NeuralPlan`. |
| 2026-09-12 | `ModularizedTacticalAI/src/NeuralPlanFactory.cpp` | Added. Hands creatures, crows, armed vehicles and zombies to the legacy factory; everything else gets a `NeuralPlan`. |
| 2026-09-12 | `ModularizedTacticalAI/CMakeLists.txt` | Compiles the two new source files. |
| 2026-09-12 | `ModularizedTacticalAI/src/PlanFactoryLibrary.cpp` | Registers `NeuralPlanFactory`, so `AI.ini` can name it in a `Factory_<n>` slot. |

### The neural faction (`SOLDIER_CLASS_NEURAL`)

A fourth enemy army class whose soldiers run the neural plan factory, wear their own
uniform colours and draw from their own gun and item tables. It is a subset of the elite
class for every strategic count: sector and group bookkeeping keeps counting a neural
soldier as an elite and records how many of those elites are neural in a pair of shadow
counters, the same arrangement the turncoat feature uses. With `NEURAL_ELITE_FRACTION`
set to 0 no neural soldier is ever created and the game behaves exactly as stock.

| Date | File | Change |
| --- | --- | --- |
| 2026-09-12 | `Tactical/Soldier Control.h` | Added `SOLDIER_CLASS_NEURAL` after the last stock class, `UNIFORM_ENEMY_NEURAL` to the uniform enum, `SOLDIER_GUN_CHOICE_TABLE_SIZE` and `SOLDIER_CLASS_HAS_ITEM_TABLE`, and widened `SOLDIER_CLASS_ENEMY` from a range test to one that names the new class. |
| 2026-09-12 | `Tactical/Inventory Choosing.h` | The two class-indexed tables are sized by `SOLDIER_GUN_CHOICE_TABLE_SIZE` so the new class has a row of its own. |
| 2026-09-12 | `Tactical/Inventory Choosing.cpp` | Same resize for the definitions; the three clamp sites ask `SOLDIER_CLASS_HAS_ITEM_TABLE`; `GenerateRandomEquipment`'s assert and the equipment rating, item status and gasmask switches accept the new class at the elite values; the Rebel Command equipment modifier uses `SOLDIER_CLASS_ENEMY`. |
| 2026-09-12 | `Tactical/Items.cpp` | Two more clamp sites converted to `SOLDIER_CLASS_HAS_ITEM_TABLE`. |
| 2026-09-12 | `Strategic/Strategic Transport Groups.cpp` | Table resize; transport groups accept the new class and key their per-class map with it. |
| 2026-09-12 | `TacticalAI/AIUtils.cpp` | `CalcDifficultyModifier` gives the elite modifier; the elite alert-status rule applies; `CorpseEnemyTeam` recognises the new uniform, which is not adjacent to the three stock enemy ones. |
| 2026-09-12 | `Tactical/Soldier Create.cpp` | Palette case, elite experience level, the `TacticalCreateNeuralEnemy` factory, and the AI index assignment that replaces the temporary fraction tag added with the AI hook. |
| 2026-09-12 | `Tactical/Soldier Create.h` | Declares `TacticalCreateNeuralEnemy`. |
| 2026-09-12 | `Strategic/XML_UniformColors.cpp` | Parses a seventh `ENEMY_NEURAL` block. Unlike the six stock blocks it is optional: without one the class wears the elite uniform, so `ja2mod.exe` still starts against a stock `Data-1.13`. |
| 2026-09-12 | `Strategic/Campaign Types.h` | `SECTORINFO` and `UNDERGROUND_SECTORINFO` gain `ubNumElites_Neural` and `ubNeuralInBattle`, taken out of the existing padding so the structures, which savegames store as raw blobs, keep their size. |
| 2026-09-12 | `Strategic/Strategic Movement.h` | The same pair in `ENEMYGROUP`, likewise out of the padding. |
| 2026-09-12 | `Strategic/Queen Command.cpp` | `NeuralShareOfElites`, the single helper that turns `NEURAL_ELITE_FRACTION` into a count; the garrison, mobile-group and underground spawn paths use it; the map-placement census, the group-assignment switch and the three death-bookkeeping switches handle the new class. |
| 2026-09-12 | `Strategic/Queen Command.h` | Declares `NeuralShareOfElites`. |
| 2026-09-12 | `Tactical/Soldier Init List.cpp` | `AddSoldierInitListEnemyDefenceSoldiers` takes the neural count and retags that many elite placements through a wrapper around `AddPlacementToWorld`, so every slot count and running total in the allocator keeps its stock arithmetic. |
| 2026-09-12 | `Tactical/Soldier Init List.h` | The new parameter. |
| 2026-09-12 | `Tactical/Enemy Soldier Save.cpp` | Preserved placements of the new class count as elites; the two reinforcement calls pass the neural share of the newly arrived elites. |
| 2026-09-12 | `Strategic/Auto Resolve.cpp` | `CreateEnemies` spends the neural share off the front of the elite loop; `CalcClassBonusOrPenalty` returns the elite multiplier; the two kill-record switches tally a neural kill as an elite kill. |
| 2026-09-12 | `Strategic/Strategic Status.cpp` | `SoldierClassToRankIndex` maps the new class to the elite rank. |
| 2026-09-12 | `TacticalAI/AIMain.cpp` | A neural soldier joining a sector increments the elite counter and the shadow counter. |
| 2026-09-12 | `Tactical/Handle UI.cpp`, `Tactical/Merc Entering.cpp` | The same, for soldiers placed by the cheat key and by helicopter insertion. |
| 2026-09-12 | `Tactical/Civ Quotes.cpp` | Elite quotes and elite taunts. |
| 2026-09-12 | `Laptop/CampaignStats.cpp` | Filed under elites in the campaign statistics. |
| 2026-09-12 | `Tactical/Soldier Ani.cpp` | Counts as an elite kill on a merc's record. |
| 2026-09-12 | `Tactical/Overhead.cpp` | Held as an elite prisoner. |
| 2026-09-12 | `Strategic/Rebel Command.cpp` | Pays the elite bounty; without this a neural kill is an unknown kill and pays nothing. |
| 2026-09-12 | `Tactical/Weapons.cpp` | The four elite chance-to-hit bonus tests. |
| 2026-09-12 | `TileEngine/worlddef.cpp` | Map summaries count a neural placement as an elite one. |
| 2026-09-12 | `Tactical/opplist.cpp` | Debug label. |
| 2026-09-12 | `TacticalAI/DecideAction.cpp`, `Tactical/SoldierTooltips.cpp` | The debug class-name arrays are indexed by class and had to grow with the enum, otherwise reading a neural soldier's name runs off the end. |
| 2026-09-12 | `Tactical/LogicalBodyTypes/FilterDB.cpp` | The `SOLDIER_CLASS` enumeration list is declared with a count of `SOLDIER_CLASS_MAX`, so the new member had to be added to it. |
| 2026-09-12 | `Tactical/XML.h` | Filenames for the new gun and item tables. |
| 2026-09-12 | `Ja2/Init.cpp` | Loads those two tables. Unlike the stock tables the load is not fatal when the file is missing: the class then borrows the elite tables, so `ja2mod.exe` still starts against a stock `Data-1.13`. |
| 2026-09-12 | `Ja2/GameVersion.h` | `SAVE_GAME_VERSION` bumped to 186 (`NEURAL_FACTION_SHADOW_COUNTERS`). The structure sizes are unchanged, so an older save still loads; the bump only puts the difference on record. |

### Fair observation, decision log, sidecar, seeded AI dice, situation export

The observation a learned policy sees is built from a plain-data `FairInputs` record
that only the glue in `NeuralHooks.cpp` fills, from the deciding soldier's own state, his
team, his opponent list and a per-observer belief store that is written by the perception
hooks alone. `BuildObservation.cpp` and `Candidates.cpp` include no engine header, so no
opponent's `SOLDIERTYPE` is reachable from them. Everything is off unless one of the
`NEURAL_*` keys below is set; with all of them at their defaults the executable plays the
stock game and rolls the stock dice.

| Date | File | Change |
| --- | --- | --- |
| 2026-09-15 | `ModularizedTacticalAI/include/tacnn_schema.h` | Added, generated by `tools/tacrl/schema_gen.py` in the ja2mod repository from `tools/tacsim/obs/spec.py`. Packed observation, mask, candidate, action and log record structs, version and hash constants. |
| 2026-09-15 | `ModularizedTacticalAI/include/FairInputs.h` | Added. The plain-data inputs the observation is built from, and the `TerrainQuery` interface. |
| 2026-09-15 | `ModularizedTacticalAI/include/BeliefStore.h`, `src/BeliefStore.cpp` | Added. Per observer, per opponent record of what was seen, heard and suffered, parallel to `gsLastKnownOppLoc`; serialised into the savegame. |
| 2026-09-15 | `ModularizedTacticalAI/include/BuildObservation.h`, `src/BuildObservation.cpp` | Added. Observation builder and action mask; engine-free translation unit. |
| 2026-09-15 | `ModularizedTacticalAI/src/Candidates.cpp` | Added. The K=48 candidate proposer; engine-free. |
| 2026-09-15 | `ModularizedTacticalAI/include/DecisionLog.h`, `src/DecisionLog.cpp` | Added. Binary per-decision log with FIFO rotation (`NEURAL_LOG`, `NEURAL_LOG_DIR`, `NEURAL_LOG_CAP_MB`). |
| 2026-09-15 | `ModularizedTacticalAI/include/SidecarClient.h`, `src/SidecarClient.cpp` | Added. Named-pipe client for `\\.\pipe\ja2mod-policy` with fixed-layout framing, deadline, reconnect (`NEURAL_SIDECAR`, `NEURAL_SIDECAR_TIMEOUT_MS`). |
| 2026-09-15 | `ModularizedTacticalAI/include/AIRandom.h`, `src/AIRandom.cpp` | Added. Seeded `std::mt19937` stream for AI decisions (`NEURAL_AI_SEED`), scope marker and roll recorder. |
| 2026-09-15 | `ModularizedTacticalAI/include/SituationExport.h`, `src/SituationExport.cpp` | Added. JSON-lines export of the sector before every legacy decision (`NEURAL_EXPORT_SITUATIONS`, `NEURAL_EXPORT_DIR`), in the parity format of `tools/tacsim/CONTRACT.md`. |
| 2026-09-15 | `ModularizedTacticalAI/include/NeuralHooks.h`, `src/NeuralHooks.cpp` | Added. The engine glue: settings, perception hooks into the belief store, the fair-input filler, the sidecar policy, decision bookkeeping, save and load of the belief store. |
| 2026-09-15 | `ModularizedTacticalAI/CMakeLists.txt` | Compiles the eight new source files. |
| 2026-09-15 | `ModularizedTacticalAI/src/LegacyAIPlan.cpp` | `execute` opens a `LegacyDecisionScope`, which records the decision the tree makes on every return path. |
| 2026-09-15 | `ModularizedTacticalAI/src/NeuralPlan.cpp` | Calls `OnNeuralDecisionEnd` after the policy or the legacy tree has decided. |
| 2026-09-15 | `sgp/random.h` | `Random()` and `PreRandom()` hand the call to `AIRandomDraw()` while an `AIRandomScope` is open; otherwise unchanged. |
| 2026-09-15 | `TacticalAI/AIMain.cpp` | `HandleSoldierAI` and `StartNPCAI` open an `AIRandomScope` for non-player soldiers; `StartNPCAI` calls `OnStartNPCAI` before `RefreshAI`. |
| 2026-09-15 | `Tactical/opplist.cpp` | `ManSeesMan`, `HearNoise` and `NoticeUnseenAttacker` feed the belief store after `UpdatePersonal`; `InitOpponentKnowledgeSystem` resets it. |
| 2026-09-15 | `Tactical/Soldier Control.cpp` | `SoldierTakeDamage` reports the hit to the belief store and the decision log. |
| 2026-09-15 | `Tactical/Overhead.cpp` | `EnterCombatMode` and `ExitCombatMode` open and close the logged episode. |
| 2026-09-15 | `Tactical/TeamTurns.cpp` | `BeginTeamTurn` advances the belief store's turn counter. |
| 2026-09-15 | `Ja2/GameSettings.h`, `Ja2/GameSettings.cpp` | The nine `NEURAL_LOG*`, `NEURAL_SIDECAR*`, `NEURAL_AI_SEED`, `NEURAL_EXPORT*` and `NEURAL_CHEAT_AUDIT` keys in `Tactical Enemy Role Settings`. |
| 2026-09-15 | `Ja2/GameVersion.h` | `SAVE_GAME_VERSION` bumped to 187 (`BELIEF_STORE_IN_SAVE`). |
| 2026-09-15 | `Ja2/SaveLoadGame.cpp` | Writes the belief store after the rebel command data; reads it back for saves at or above 187 and starts empty for older ones. |

### The fast-forward battle harness (`NEURAL_HARNESS`)

A command-line mode that loads a save from the main menu on its own, lets the stock AI play
both sides of the battle with the clock and the frame rate running as fast as the machine
allows, records how each battle ended, reloads the save for the next one and quits with exit
code 0. Its purpose is to produce parity situations and battle statistics for the Python
simulator in the ja2mod repository, driven there by `tools/harness/run_battles.py`. The
switch is `NEURAL_HARNESS` in `[Ja2 Settings]` of `Ja2.ini`, normally given as
`-NEURAL_HARNESS=1` on the command line together with the `HARNESS_*` keys listed in
`ModularizedTacticalAI/include/Harness.h`. Every engine change below asks the harness first
(`HarnessActive`, `HarnessDrivesPlayerTeam`, `HarnessSkipsRender`, `gfHarnessFastForward`),
and all of those answer no while the key is absent, so a stock configuration plays the stock
game.

| Date | File | Change |
| --- | --- | --- |
| 2026-09-15 | `ModularizedTacticalAI/include/Harness.h`, `src/Harness.cpp` | Added. The harness state machine (menu → loading → arming → battle → reload → done), the option keys, the player squad driver (each merc handed to `StartNPCAI` in turn, `SEEKENEMY`/`AGGRESSIVE` orders, an optional enemy-position radar in the squad's public opplist, optional quiet turns), the enemy top-up through `AddEnemiesToBattle`, the alert override with knowledge wipe (yellow plants the squad's position as the enemy team's public noise every enemy turn until they notice the squad, 2026-09-16), forced turn mode, message-box auto-answer, the deadlock delay and per-battle timeouts, seeding, `results.jsonl` and `harness.log`; a per-frame time profile (`HarnessLapBegin`, `HarnessLap`, `HarnessProfileScope`) for the heartbeat (2026-09-16). Unless `HARNESS_AI_LOG=1`, start clears `gfLogsEnabled` (`TacticalAI/AIMain.cpp`, declared `extern` here, the file itself untouched): stock `DebugAI` opens, appends and closes `Logs\AI_Decisions.txt` and a per-soldier file for every line, about 35 lines and 150 ms per red or black decision, which was most of what a decision cost (2026-09-16). |
| 2026-09-15 | `ModularizedTacticalAI/CMakeLists.txt` | Compiles `Harness.cpp`. |
| 2026-09-15 | `ModularizedTacticalAI/src/NeuralHooks.cpp` | Forwards battle start and end, team turns and the loaded-save generation to the harness. |
| 2026-09-15 | `ModularizedTacticalAI/src/SituationExport.cpp` | The `opplists` block gains `team_aware`, one flag per team from `bAwareOfOpposition`, and every soldier's `ai` block gains `ai_difficulty`, his `SoldierDifficultyLevel()`; the Python replay needs the first to tell a red alert from a seek and the second to read the same row of `gbDiff` as the engine. |
| 2026-09-15 | `sgp/sgp.cpp` | `GetRuntimeSettings` reads `NEURAL_HARNESS` and the `HARNESS_*` keys from `[Ja2 Settings]` (so the existing `-KEY=VALUE` command-line override applies) and hands them to `HarnessSetOptions`. `HARNESS_AI_LOG` added 2026-09-16. (A frame batch in the `WM_TIMER` handler was tried and removed on 2026-09-16: the build runs in hi-speed clock mode, where the notify thread, not `WM_TIMER`, calls `GameLoop`, back to back while fast-forwarding.) |
| 2026-09-15 | `sgp/Random.cpp`, `sgp/random.h` | `SeedGameRandom`: reseeds the `NEW_RANDOM` generator and `srand` from the battle seed. |
| 2026-09-15 | `Ja2/GameSettings.h`, `Ja2/GameSettings.cpp` | `fNeuralHarness`, the `NEURAL_HARNESS` key of `Ja2_Options.INI` as the second way to switch the harness on. |
| 2026-09-15 | `Ja2/gameloop.cpp` | `GameLoop` calls `HarnessTick` before the screen handler; the frame's blit is skipped when the harness says so. |
| 2026-09-15 | `Ja2/gamescreen.cpp` | `MainGameScreenHandle` skips `RenderWorld` while the harness runs without `HARNESS_RENDER`; it calls `HarnessLapBegin` at its start and `HarnessLap` after each of its sections (2026-09-16), which the harness sums into a per-frame profile for its heartbeat and which do nothing while no run is on. |
| 2026-09-15 | `Ja2/MainMenuScreen.cpp`, `Ja2/mainmenuscreen.h` | `MainMenuAutoLoad`: presses Load for a slot from the main menu, the ALT+Load path with the slot chosen by the caller. |
| 2026-09-15 | `Ja2/SaveLoadScreen.cpp`, `Ja2/SaveLoadScreen.h` | `DoQuickLoadSlot` (`DoQuickLoad` for any slot, used to reload between battles) and `SaveLoadScreenAutoLoad` (confirms Load for a slot on an idle save/load screen). |
| 2026-09-15 | `Utils/Timer Control.cpp`, `Utils/Timer Control.h` | `gfHarnessFastForward`: a second fast-forward flag that `IsFastForwardMode` and `UpdateTimer` honour and that the places switching the stock flag off (player turn, dialogue, visible enemies) do not touch. |
| 2026-09-15 | `TacticalAI/AIMain.cpp` | The plan's `execute` is timed for the harness profile (2026-09-16). `HandleSoldierAI` no longer bails out for a player soldier while the harness drives the team; `SetNewSituation` marks the harness's mercs too, since the player's sighting halt (`HaultSoldierFromSighting`) cancels their shot and the AI would otherwise wait for it until the deadlock delay. |
| 2026-09-15 | `Tactical/Overhead.cpp` | `ExecuteOverhead` times `HandleSoldierAI` for the harness profile (2026-09-16). `InternalReduceAttackBusyCount`: while the harness drives the player's team the attacker is the merc under AI control, not the selected merc, and a player attacker is released with `FreeUpNPCFromAttacking` like an AI one; without both, a driven merc's shot never counted as finished and he stood aiming until the deadlock delay ended his turn. |
| 2026-09-15 | `Tactical/Soldier Ani.cpp` | The pending-action release at the end of a stationary animation (code 499) applies to the harness's mercs as well. |
| 2026-09-16 | `ModularizedTacticalAI/include/AIRandom.h`, `src/AIRandom.cpp` | `AIRandomUnrecorded`: an RAII scope inside which `AIRandomDraw` still draws from the AI stream but records nothing. |
| 2026-09-16 | `ModularizedTacticalAI/include/AIRandom.h`, `src/AIRandom.cpp`, `include/SituationExport.h`, `src/SituationExport.cpp`, `src/NeuralHooks.cpp` | The roll recorder also keeps the die of every recorded roll (`AIRandomRecordedRanges`), and `SituationComplete` writes them as `roll_ranges` next to `rolls`, so the Python replay can name the first draw at which it asked a different question than the engine. |
| 2026-09-16 | `ModularizedTacticalAI/src/SituationExport.cpp` | Every soldier's `weapon` block gains `weapon_scoped`, the engine's own `IsScoped` answer for the gun in hand (under NCTH: any scope, on the gun or its ammunition or attachments, magnifying more than 1x). The export does not carry attachments, so the Python replay could not reproduce the test that decides whether a soldier crouches before he shoots. |
| 2026-09-16 | `ModularizedTacticalAI/src/SituationExport.cpp` | Every soldier's record gains `sniper_trait` (the trait half of `AICheckIsSniper`, `gGameOptions.fNewTraitSystem && HAS_SKILL_TRAIT(SNIPER_NT)`) and `grenades`, the hand grenades in his pockets in slot order (`slot`, `item`, `status`, `count`; the static tests of `FindThrowableGrenade`: grenade class, toss cursor, no launcher), so the Python replay can run `CheckIfTossPossible` over the same pockets and throw the same dice. |
| 2026-09-16 | `Tactical/PATHAI.cpp` | `RandomSkipListLevel` rolls inside an `AIRandomUnrecorded` scope: the path finder draws one `Random(4)` per queue node for its skip list, dozens per search, and every AI decision that measures a path had them in its roll record, where the Python replay read them as the decision's own dice. Nothing changes for the game: the same numbers are drawn from the same stream, they are just not written down. |
| 2026-09-15 | `Tactical/TeamTurns.cpp` | `EndInterrupt` calls `HarnessOnInterruptEnded` when the player's team regains control, so a driven merc whose action an interrupt cut short decides afresh; `OnBeginTeamTurn` is called inside the team loop, after inactive teams are skipped, so it reports the team whose turn begins. |
| 2026-09-16 | `Tactical/Handle Items.cpp` | `HandleSoldierPickupItem` takes the AI branch (pick the item up, no menu) for a merc the harness drives, as it does for every non-player soldier. The item pickup menu waits for a mouse that never comes under the harness, and a second `InitializeItemPickupMenu` over a menu still up memsets its regions while they are linked, which closes `MSYS_RegList` into a cycle; the next `MSYS_RemoveRegion` (here `gTEAM_PanelRegion`, from `MSYS_DeleteRegionFromList`) then walks the list forever. Found on build 25 in battle 10 of the twenty-battle acceptance run with `cdb -pv` and a linker map. Stock play is unchanged: the condition is false while the harness is off. |
| 2026-09-16 | `ModularizedTacticalAI/src/Harness.cpp` | `CloseTacticalMenus`: during a battle any item pickup menu that opened anyway is closed with `RemoveItemPickupMenu` (what Cancel does) before the frame's game logic runs, counted in the result line as `menus`. |

### Embedded inference (`NEURAL_MODE = embedded`, `NEURAL_MODEL`)

The exported policy runs inside the executable. `NEURAL_MODEL` names an `.onnx` file under the
data folders (`AI\policy.onnx` by default, shipped by the ja2mod overlay as
`Data-ja2mod\AI\policy.onnx`); it is read through the VFS once, its weights are resolved by
their PyTorch parameter names, its `obs_schema` metadata is compared with `TACNN_SCHEMA_HASH`
of `tacnn_schema.h`, and a soldier tagged for `NeuralPlanFactory` then gets his action from a
fp32 forward pass of the network in about a millisecond, with no other process involved. A
missing, unreadable or mismatched model is logged in one line of `game_log.log` and the mode
drops to `off`, which is the legacy tree. `NEURAL_MODE` left empty keeps the meaning of the
older `NEURAL_SIDECAR` key.

| Date | File | Change |
| --- | --- | --- |
| 2026-09-16 | `ModularizedTacticalAI/include/TacnnInfer.h`, `src/TacnnInfer.cpp` | Added. A minimal ONNX protobuf reader (initializers and `metadata_props` only), the fp32 forward pass of the exported actor (convolutions, linear layers, group and layer normalisation, the entity transformer with attention pooling, the inbox read, FiLM, the GRU cell, the five heads), the greedy masked decode of `tacrl.model.policy.ActionHeads.sample(greedy=True)`, the team radio ring of `tools/tacrl/comms.py` and the per-soldier hidden-state store. No engine dependencies; `tools/test_tacrl_embedded.py` in the ja2mod repository compiles it with g++ against onnxruntime. |
| 2026-09-16 | `ModularizedTacticalAI/include/tacnn_schema.h` | Regenerated for schema 2: the `TacnnEmbedded` log record (what the network was fed and answered, with its microseconds), `TACNN_MODE_*`, `TACNN_SRC_EMBEDDED`, the model constants and the `tacnn_kind_compat` table. |
| 2026-09-16 | `ModularizedTacticalAI/include/DecisionLog.h`, `src/DecisionLog.cpp` | `writeEmbedded`: the embedded record follows its decision record in the log. |
| 2026-09-16 | `ModularizedTacticalAI/src/NeuralHooks.cpp` | `NEURAL_MODE` and `NEURAL_MODEL` applied; the model loaded through `FileOpen`/`FileRead` once per path with the schema check and the one-line fallback; the policy renamed `NeuralPolicy` and given the embedded branch (hidden state and inbox in, forward pass, state and ring updated, greedy decode, `ApplyPolicyAction`); the hidden store and the ring cleared at battle start; decision timing in microseconds; the embedded record written with the decision. An embedded answer the engine cannot carry out is handled as the training environment does (logged with `AI_ACTION_NONE`, the network asked again, the soldier's turn ended after three wasted answers in a row or twelve decisions) instead of falling to the legacy tree. |
| 2026-09-16 | `ModularizedTacticalAI/src/Candidates.cpp`, `include/FairInputs.h` | The action mask follows the Python mask of `tools/tacsim/obs/candidates.py` (T11): the stance head excludes the soldier's current stance, a teammate within one tile is not a seek target, and seeking or flanking needs `ap_min_move` plus the reserve `MAX_AP_CARRIED` (`SelfState::apSeekReserve`, filled from the APBP constants) as the legacy walk keeps back. |
| 2026-09-16 | `ModularizedTacticalAI/src/NeuralHooks.cpp` | Decision latency: a per-phase profile (fair inputs, observation, candidates, mask, forward pass, apply; the line-of-sight rays and the reach table inside them) summed per battle, printed by `DecisionProfileSummary()` into `game_log.log` at battle end and into the harness log; `LosCache`, a memo of the engine's line-of-sight answers for the current team turn, keyed by viewpoint (tile, level, target level and height, and for the soldier's own eyes a digest of his gear and condition) and the 64 by 64 block of the map the targets lie in, which the game minute, battle start, sector load and `OnWorldChanged()` empty (not the team turn: everything a ray depends on has a hook); the soldier's own rays computed as `SoldierTo3DLocationLineOfSightTest` does but with his sight range derived once per light level instead of once per tile (the engine rederives it from gear, traits and the light of the target tile for every call; with nobody standing on the tile only the light varies); the reach table's Dijkstra on a binary heap instead of a linear scan, and the table kept (`ReachMemo`) for the next decision of the same soldier from the same tile in the same movement mode, dropped when another soldier decides, a team turn begins or the map changes. Every answer is the one the engine gives. |
| 2026-09-16 | `ModularizedTacticalAI/include/NeuralHooks.h` | `OnWorldChanged()` and `DecisionProfileSummary()` declared. |
| 2026-09-16 | `TileEngine/structure.cpp` | `AddStructureToWorld`, `DeleteStructureFromWorld` and `InternalSwapStructureForPartner` (doors, windows, damaged partners) call `tacnn::OnWorldChanged()`: a structure that bends sight lines came, went or changed state. Person structures (`LEVELNODE_SOLDIER` / `STRUCTURE_PERSON`) are left out: a merc's structure is re-added on every animation frame, and neither the engine's sight rays nor the training environment treat soldiers as sight blockers. |
| 2026-09-16 | `TileEngine/Explosion Control.cpp` | `SpreadEffect` calls `tacnn::OnWorldChanged()` before smoke, gas, fire or light spreads. |
| 2026-09-16 | `TileEngine/SmokeEffects.cpp` | `AddSmokeEffectToTile` and `RemoveSmokeEffectFromTile`, the two writes of the smoke flags a sight ray reads, call `tacnn::OnWorldChanged()`. |
| 2026-09-16 | `ModularizedTacticalAI/src/Candidates.cpp` | The picker works on the coordinates of the fine cells, the chosen tiles, the believed enemies and their centroid, computed once per proposal, instead of dividing every tile by the map width for every comparison (tens of thousands per decision); the candidates are the same. |
| 2026-09-16 | `ModularizedTacticalAI/src/TacnnInfer.cpp` | Forward pass speed, same results: `Dot4` computes four output rows per pass over the input (linear layers, convolutions, the GRU gates); the entity transformer runs on the present rows only (padded rows are masked keys with zero weight and masked pooling terms, so the present rows come out identical); the key of an all-zero candidate row is computed once at load and reused for every empty slot. |
| 2026-09-16 | `TileEngine/lighting.cpp` | `LightSpriteCreate`, `LightSpriteDestroy`, `LightSpritePosition` and `LightSpritePower` call `tacnn::OnWorldChanged()`: the light a soldier sees by moved. |
| 2026-09-16 | `ModularizedTacticalAI/include/Harness.h`, `src/Harness.cpp` | `HARNESS_NEURAL_ALL` routes every enemy on the map through `NeuralPlanFactory` when a battle is armed (his plan is deleted so the factory builds the next one); `HARNESS_NEURAL_MODE` and `HARNESS_NEURAL_MODEL` override the INI keys for the run. |
| 2026-09-16 | `sgp/sgp.cpp` | Reads the three `HARNESS_NEURAL_*` keys. |
| 2026-09-16 | `Ja2/GameSettings.h`, `Ja2/GameSettings.cpp` | `szNeuralMode` and `szNeuralModel`, the `NEURAL_MODE` and `NEURAL_MODEL` keys of `Ja2_Options.INI`. |
| 2026-09-16 | `ModularizedTacticalAI/CMakeLists.txt` | Compiles `TacnnInfer.cpp`. |
| 2026-09-16 | `ModularizedTacticalAI/src/Candidates.cpp`, `src/BuildObservation.cpp` | Byte parity with the Python port after a side-by-side review: `my_reach` and `their_reach` are evaluated in double and rounded once on the store (two float roundings differed by an ulp, d = 1 over range 3); a teammate without a tile reports no line of sight instead of asking the terrain about `NO_TILE`; a negative level is written as 0 rather than 255. |

### The in-game feedback loop (`NEURAL_LOG_PLAYER`)

The decision log becomes the training data of the retraining workflow in the ja2mod repository
(`tools/tacrl/retrain.py`, `docs/features/neural-ai-retraining.md`): the player's own decisions
are behaviour-cloned into a league opponent, lost battles become starting states, and every
logged observation is checked against the Python builder. Three things were needed for that:
the log had to carry the player's side, every battle had to end with an `episode_end` record,
and the observations had to be byte-identical to the Python ones, which they were not.

| Date | File | Change |
| --- | --- | --- |
| 2026-09-16 | `Ja2/GameSettings.h`, `Ja2/GameSettings.cpp` | `fNeuralLogPlayer`, the `NEURAL_LOG_PLAYER` key of `Ja2_Options.INI` (default off): the log also records the decisions the AI makes for the player's soldiers. |
| 2026-09-16 | `ModularizedTacticalAI/src/NeuralHooks.cpp` | `Logged(p)`: with `NEURAL_LOG_PLAYER` the `LegacyDecisionScope` writes the decision record for a player soldier the AI decides for (a merc left under AI control, the harness's squad); the situation export stays enemy-only. Battle closure split into `CloseEpisodeRecord`, shared by `OnExitCombatMode` and the new public `CloseEpisode(outcome)` for a battle the engine did not end itself. `#undef NO_TILE` after the engine headers: `TileEngine/tiledef.h` defines `NO_TILE` as the tile engine's 64000 and the macro replaced every `tacnn::NO_TILE` (-1) in this file, so a believed enemy without a tile went into the observation as an entity 400 rows south of the map; `TileOrNone` maps any gridno outside `[0, WORLD_MAX)` to `NO_TILE` before it reaches the fair inputs. Found by `tools/tacrl/replay.py` (observation parity 63.5 percent before, 100 percent after). |
| 2026-09-16 | `ModularizedTacticalAI/include/NeuralHooks.h` | `CloseEpisode(UINT8 ubOutcome)` declared. |
| 2026-09-16 | `ModularizedTacticalAI/include/Harness.h`, `src/Harness.cpp` | `HARNESS_LOG_PLAYER` (default on) forces `NEURAL_LOG_PLAYER` for the run when a battle is armed, so the squad's decisions are logged as behaviour-cloning data; `FinishBattle` calls `CloseEpisode` with the harness verdict (`OutcomeCode`: `player_won`, `enemy_won`, otherwise `aborted`) for a battle it ended on the turn cap, the timeout or a stalemate, so the log has an `episode_end` for every battle. |
| 2026-09-16 | `sgp/sgp.cpp` | Reads `HARNESS_LOG_PLAYER`. |
