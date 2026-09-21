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

### Covert blades

A blade carrying the `Covert` item flag is an assassination weapon for a Covert Ops
soldier: the trait's melee chance-to-hit bonus applies to it, and against a target that
does not have the attacker in view (attacked from behind or unaware, blinded or down) it
rolls the garotte's instakill. A disguised soldier who kills such a target from behind
keeps the disguise: the victim is not turned around before the blow, does not recognise
the attacker mid-stab, and does not scream. A victim who is still on his feet afterwards
uncovers the attacker at once. Nothing in the savegame format changes.

| Date | File | Change |
| --- | --- | --- |
| 2026-09-15 | `Tactical/Weapons.cpp` | `MeleeTargetSeesAttacker` (from the `SOLDIER_BACK_ATTACK`/`SOLDIER_SNEAK_ATTACK` flags, collapse and blindness) replaces the "can the target see us" line-of-sight test of the garotte code in `CalcChanceHTH` and `HTHImpact`, which was taken from the attacker's side and therefore always true for an adjacent target. `CalcChanceHTH`: a Covert-flagged blade adds `COVERT_MELEE_CTH_BONUS` per Covert Ops level in the stab branch. `HTHImpact`: a Covert-flagged blade rolls the garotte instakill when the target does not see the attacker; the weapon-status scaling of the roll no longer collapses to zero for any status below 100 (integer division). |
| 2026-09-15 | `Tactical/Soldier Control.cpp` | Helpers `IsDisguised`, `IsCovertMeleeWeapon`, `IsDisguisedBackAttack`, `VictimUncoversDisguisedAttacker`. `EVENT_SoldierBeginBladeAttack` no longer turns the victim to face a disguised attacker who strikes from behind. `RecognizeAsCombatant` extends the punch-attack exemption to the four blade animations. `EVENT_SoldierGotHit`: no scream when a disguised attacker kills from behind with a covert melee weapon; a victim who stays on his feet after a melee hit from a disguised attacker uncovers him. |

### In-battle counter hygiene (stock bug fix)

A sector's and a mobile enemy group's `ub*InBattle` counters record how many of its soldiers
stand on the loaded tactical map. Stock 1.13 raises them when a map is entered and clears them
when it is left, but only for the groups still positioned in the battle sector. A group that
was counted into the battle while already walking out of the sector kept moving on the
strategic layer, arrived next door in the middle of the fight, was reassigned there, and so
missed the clean-up: it carried "13 troops in battle" for the rest of the campaign. The next
time it met the player it either fielded no soldiers or, once the strategic AI had re-split its
13 soldiers into fewer troops plus some elites, crashed the game on
`AssertGE( ubNumTroops, ubTroopsInBattle )` in `PrepareEnemyForSectorBattle()`. Stock only
notices this in `JA2BETAVERSION` builds (`ValidateAndCorrectInBattleCounters()`), which the
shipped executable is not. Nothing in the savegame format changes; a save that already carries
stale counters is repaired the next time a map is entered or left, with a line in `game_log.log`.

| Date | File | Change |
| --- | --- | --- |
| 2026-09-21 | `Strategic/Queen Command.h` | Declares `EnemyGroupHasSoldiersInBattle`, `ClearEnemyGroupInBattleCounters`, `ClearStaleInBattleCounters`. |
| 2026-09-21 | `Strategic/Queen Command.cpp` | The three helpers plus their sector-side counterparts. `ClearStaleInBattleCounters` clears every set enemy in-battle counter of any group, surface sector or underground sector (creature counters are left to the creature code, which sets them before this runs) and logs each one to `game_log.log`. `EndTacticalBattleForEnemy` uses the helpers (which now also clear the fork's `ubNeuralInBattle`) and then sweeps every group, not only those in the battle sector. `PrepareEnemyForSectorBattle` sweeps first thing: it only ever runs for a map with no enemy soldier on it, so anything still set is stale, and repairing it there is what turns the release-build assertion into a log line. `AddPossiblePendingEnemiesToBattle`: the fallback branch that books an unaccounted soldier as a troop now raises `ubNumTroops` along with `ubTroopsInBattle`. |
| 2026-09-21 | `Strategic/Strategic Movement.cpp` | `GroupArrivedAtSector` holds the arrival of an enemy group that is leaving the loaded battle sector while it has soldiers in that battle, re-posting the event 3-5 minutes later exactly like the existing hold for an arrival into a contested sector. |
| 2026-09-21 | `Strategic/Strategic AI.cpp` | `ConvertGroupTroopsToComposition` keeps the roster of a group whose soldiers are fighting on the loaded map (their classes are already fixed) and clears the in-battle counters of a group that carries stale ones before re-splitting it. |
