/**
 * @file
 * @author ja2mod
 *
 * Added by the ja2mod fork on 2026-09-15; see CHANGES-ja2mod.md.
 */

#include "../include/AIRandom.h"

#include <random>

unsigned int guiAIRandomDepth = 0;

namespace tacnn
{
    namespace
    {
        uint32_t g_seed = 0;
        uint32_t g_draws = 0;
        std::mt19937 g_engine;
        AIRandomFallback g_fallback = 0;
        bool g_recording = false;
        std::vector<uint32_t> g_recorded;
        std::vector<uint32_t> g_recordedRanges; // the die each recorded roll came from, same index
        unsigned int g_unrecorded = 0;
    }

    void AIRandomSetSeed(uint32_t seed)
    {
        g_seed = seed;
        AIRandomReset();
    }

    uint32_t AIRandomSeed()
    {
        return g_seed;
    }

    void AIRandomReset()
    {
        g_engine.seed(g_seed);
        g_draws = 0;
    }

    bool AIRandomActive()
    {
        return guiAIRandomDepth != 0;
    }

    uint32_t AIRandomNext(uint32_t range)
    {
        if(range == 0)
            return 0;
        ++g_draws;
        // rejection sampling keeps the distribution uniform and the result
        // independent of the platform's std::uniform_int_distribution
        const uint32_t limit = 0xFFFFFFFFu - (0xFFFFFFFFu % range);
        uint32_t value;
        do
        {
            value = static_cast<uint32_t>(g_engine());
        }
        while(value >= limit);
        return value % range;
    }

    uint32_t AIRandomDraws()
    {
        return g_draws;
    }

    void AIRandomSetFallback(AIRandomFallback fallback)
    {
        g_fallback = fallback;
    }

    void AIRandomRecordBegin()
    {
        g_recorded.clear();
        g_recordedRanges.clear();
        g_recording = true;
    }

    void AIRandomRecordStop()
    {
        g_recording = false;
    }

    bool AIRandomRecording()
    {
        return g_recording;
    }

    const std::vector<uint32_t>& AIRandomRecorded()
    {
        return g_recorded;
    }

    const std::vector<uint32_t>& AIRandomRecordedRanges()
    {
        return g_recordedRanges;
    }

    AIRandomScope::AIRandomScope(bool enabled)
        : enabled_(enabled)
    {
        if(enabled_)
            ++guiAIRandomDepth;
    }

    AIRandomScope::~AIRandomScope()
    {
        if(enabled_ && guiAIRandomDepth > 0)
            --guiAIRandomDepth;
    }

    AIRandomUnrecorded::AIRandomUnrecorded()
    {
        ++g_unrecorded;
    }

    AIRandomUnrecorded::~AIRandomUnrecorded()
    {
        if(g_unrecorded > 0)
            --g_unrecorded;
    }
}

uint32_t AIRandomDraw(uint32_t range)
{
    uint32_t value;
    if(tacnn::g_seed != 0 || tacnn::g_fallback == 0)
        value = tacnn::AIRandomNext(range);
    else
        value = tacnn::g_fallback(range);
    if(tacnn::g_recording && tacnn::g_unrecorded == 0 && tacnn::g_recorded.size() < 65536)
    {
        tacnn::g_recorded.push_back(value);
        tacnn::g_recordedRanges.push_back(range);
    }
    return value;
}
