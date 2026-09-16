/**
 * @file
 * @author ja2mod
 *
 * A separate, seeded random stream for AI decisions, and a recorder for the
 * dice a decision rolled. Added by the ja2mod fork on 2026-09-15; see
 * CHANGES-ja2mod.md.
 *
 * The AI's decision code rolls dice through the same Random()/PreRandom()
 * the rest of the game uses, so two runs of the same save never decide the
 * same way twice. HandleSoldierAI and StartNPCAI wrap the handling of every
 * non-player soldier in an AIRandomScope; while one is open, the inline
 * Random() in sgp/random.h hands the call to AIRandomDraw() instead of the
 * game's generator. AIRandomDraw() then draws from a std::mt19937 seeded by
 * NEURAL_AI_SEED when that key is non-zero, and from the game's own generator
 * (through the fallback installed by NeuralHooks) when it is zero, so a zero
 * seed leaves the game's numbers exactly as they were. Either way the value
 * is appended to the roll recorder when one is running, which is how the
 * situation exporter gives the Python parity harness the dice to replay.
 *
 * The seeded stream restarts when a battle starts and when a save is loaded,
 * so a save replayed with the player doing nothing produces the same enemy
 * decisions as long as no shot is fired (combat resolution still rolls on
 * the game's stream).
 */

#ifndef TACNN_AI_RANDOM_H
#define TACNN_AI_RANDOM_H

#include <stdint.h>
#include <vector>

namespace tacnn
{
    /// Set the seed; 0 disables the separate stream (the fallback is used).
    void AIRandomSetSeed(uint32_t seed);
    uint32_t AIRandomSeed();
    /// Restart the stream from the configured seed (battle start, save load).
    void AIRandomReset();
    /// True while inside a scope; the stream itself is only used when the seed is non-zero.
    bool AIRandomActive();
    /// Uniform in [0, range) from the seeded stream; 0 when range is 0.
    uint32_t AIRandomNext(uint32_t range);
    /// How many numbers the seeded stream has produced since the last reset.
    uint32_t AIRandomDraws();

    /// The game's generator, used while the seed is 0. Installed once by NeuralHooks.
    typedef uint32_t (*AIRandomFallback)(uint32_t range);
    void AIRandomSetFallback(AIRandomFallback fallback);

    /// Start keeping every value AIRandomDraw() returns; clears the previous recording.
    void AIRandomRecordBegin();
    void AIRandomRecordStop();
    bool AIRandomRecording();
    const std::vector<uint32_t>& AIRandomRecorded();
    /// the range (die) of every recorded roll, same length and order as AIRandomRecorded
    const std::vector<uint32_t>& AIRandomRecordedRanges();

    /// RAII marker; nests. `enabled` false makes it a no-op (player-controlled soldiers).
    class AIRandomScope
    {
        public:
            explicit AIRandomScope(bool enabled);
            ~AIRandomScope();
        private:
            bool enabled_;
            AIRandomScope(const AIRandomScope&);
            AIRandomScope& operator=(const AIRandomScope&);
    };

    /// RAII marker; nests. While one is open AIRandomDraw() still draws from the same
    /// stream but records nothing. For dice that belong to a data structure rather than
    /// to the decision: the path finder rolls one per queue node for its skip-list level,
    /// dozens per search, and the Python replay, whose path finder rolls none, would read
    /// them as the decision's own.
    class AIRandomUnrecorded
    {
        public:
            AIRandomUnrecorded();
            ~AIRandomUnrecorded();
        private:
            AIRandomUnrecorded(const AIRandomUnrecorded&);
            AIRandomUnrecorded& operator=(const AIRandomUnrecorded&);
    };
}

/// Read by the inline Random() in sgp/random.h; non-zero while a scope is open.
extern unsigned int guiAIRandomDepth;
/// Called by Random() while guiAIRandomDepth is non-zero.
uint32_t AIRandomDraw(uint32_t range);

#endif
