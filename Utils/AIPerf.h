// ja2mod: tactical-AI performance log (new file).
//
// Measures how long the tactical AI takes per decision and per frame and
// writes the slow ones to ja2mod-aiperf.log in the user profile folder, so
// that frame-rate stalls in sectors with many enemies can be attributed to a
// specific AI routine (cover search, path flood, line of sight, ...) before
// anything is optimised or parallelised.
//
// Cost when nothing is slow: one branch per instrumented call outside a
// decision, two QueryPerformanceCounter reads per instrumented call inside
// one. Everything is off when AI_PERF_LOG=0 in Ja2.ini [Ja2 Settings].
//
// Times are inclusive: a decision's "red" time contains the "cover" time,
// which contains the "flood" and "los" times.

#ifndef JA2MOD_AIPERF_H
#define JA2MOD_AIPERF_H

#include "types.h"

class SOLDIERTYPE;

namespace AIPerf
{
	// Fixed set of things measured inside a decision (plus three frame-level
	// buckets). Names printed in the log are in AIPerf.cpp, same order.
	enum Counter
	{
		C_GREEN = 0,	// DecideActionGreen
		C_YELLOW,		// DecideActionYellow
		C_RED,			// DecideActionRed
		C_BLACK,		// DecideActionBlack
		C_COVER,		// FindBestNearbyCover
		C_MAXDIST,		// FindSpotMaxDistFromOpponents
		C_UNGASSED,		// FindNearestUngassedLand
		C_DARKER,		// FindNearbyDarkerSpot
		C_ITEMS,		// SearchForItems
		C_FLANK,		// FindFlankingSpot
		C_CLIMB,		// FindClosestClimbPoint
		C_ADVANCE,		// FindAdvanceSpot
		C_RETREAT,		// FindRetreatSpot
		C_SHOT,			// CalcBestShot
		C_THROW,		// CalcBestThrow
		C_STAB,			// CalcBestStab
		C_DISTURB,		// ClosestReachableDisturbance
		C_FRIEND,		// ClosestReachableFriendInTrouble
		C_GOTO,			// InternalGoAsFarAsPossibleTowards
		C_FLOOD,		// FindBestPath with COPYREACHABLE / COPYREACHABLE_AND_APS
		C_PATH,			// FindBestPath, plain path query
		C_LOS,			// LineOfSightTest
		C_CTGT,			// ChanceToGetThrough
		C_CALCCOVER,	// CalcCoverValue
		C_OVERHEAD,		// frame level: ExecuteOverhead
		C_RENDER,		// frame level: RenderWorld
		C_REFRESH,		// frame level: RefreshScreen
		NUM_COUNTERS
	};

	// Called once from GetRuntimeSettings() with the Ja2.ini values. The log
	// file is opened lazily on the first line written, after the VFS is up.
	void Configure( BOOLEAN fEnabled, UINT32 uiMinDecisionMs, UINT32 uiMinFrameMs );

	// GameLoop(): brackets one iteration; a frame slower than
	// AI_PERF_LOG_FRAME_MS gets a "frame" line.
	void BeginFrame( );
	void EndFrame( );

	// EndTurn(): writes a "turn" summary for the team whose turn ends, if
	// any decision did work during it.
	void TurnEnded( UINT8 ubTeam );

	// One HandleSoldierAI() call. Nested calls (the AI re-entering itself
	// through NPC scripts) are attributed to the outermost one.
	class DecisionScope
	{
	public:
		explicit DecisionScope( SOLDIERTYPE* pSoldier );
		~DecisionScope( );
	private:
		DecisionScope( DecisionScope const& );
		DecisionScope& operator=( DecisionScope const& );
		SOLDIERTYPE*	_pSoldier;
		INT64			_iStart;		// 0 when not armed
		INT8			_bAlertBefore;
	};

	// One timed call of a counter. Decision-level counters are armed only
	// inside a DecisionScope; frame-level ones whenever the log is enabled.
	class Scope
	{
	public:
		explicit Scope( Counter eCounter );
		~Scope( );
	private:
		Scope( Scope const& );
		Scope& operator=( Scope const& );
		Counter	_eCounter;
		INT64	_iStart;		// 0 when not armed
	};
}

#endif // JA2MOD_AIPERF_H
