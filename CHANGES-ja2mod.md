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
