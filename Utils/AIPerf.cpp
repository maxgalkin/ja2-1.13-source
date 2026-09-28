// ja2mod: tactical-AI performance log (new file). Interface in AIPerf.h.
//
// Lines written to ja2mod-aiperf.log (user profile folder, appended across
// sessions, the VFS log prefixes each line with the seconds since the log
// was opened):
//
//   decision <ms>ms id=.. team=.. cls=.. grid=.. lvl=.. alert=a>b opp=.. aps=..
//            act=<name>(<n>) data=.. mode=TB|RT sector=A9-0 men=p/e/c/m/v
//            t=<GetJA2Clock> | <counter>=<calls>/<ms> ...
//   frame    <ms>ms screen=.. overhead=<ms> ai=<ms>/<calls>(<decisions>)
//            render=<ms> refresh=<ms> mode=.. t=..
//   turn     team=.. decisions=.. ai=<ms> max=<ms>(id ..) frames=.. slow=..
//            maxframe=<ms>
//
// A "decision" is one HandleSoldierAI() call that ran at least one
// instrumented routine or took 1 ms or more; the many calls that return at
// once (not this soldier's turn, soldier busy) are not counted.

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "AIPerf.h"
#include "sgp_logger.h"
#include "Soldier Control.h"
#include "Overhead.h"
#include "Overhead Types.h"
#include "ai.h"
#include "Timer Control.h"
#include "strategicmap.h"
#include "jascreens.h"

namespace
{
	const char* const kNames[AIPerf::NUM_COUNTERS] =
	{
		"green", "yellow", "red", "black",
		"cover", "maxdist", "ungassed", "darker", "items", "flank", "climb", "advance", "retreat",
		"shot", "throw", "stab",
		"disturb", "friend",
		"goto",
		"flood", "path",
		"los", "ctgt",
		"calccover",
		"overhead", "render", "refresh",
	};

	inline bool IsFrameLevel( AIPerf::Counter eCounter )
	{
		return eCounter >= AIPerf::C_OVERHEAD;
	}

	struct Accum
	{
		INT64	iTicks[AIPerf::NUM_COUNTERS];
		UINT32	uiCalls[AIPerf::NUM_COUNTERS];

		void Reset( ) { memset( this, 0, sizeof( *this ) ); }
	};

	BOOLEAN			gfEnabled = FALSE;
	UINT32			guiMinDecisionMs = 5;
	UINT32			guiMinFrameMs = 50;
	INT64			giFreq = 0;

	int				giDepth = 0;			// DecisionScope nesting
	bool			gfLogOpen = false;
	sgp::Logger_ID	gLogId = 0;

	Accum			gDecision;				// counters of the current outermost decision

	// current frame
	Accum			gFrame;					// frame-level counters
	INT64			giFrameStart = 0;
	INT64			giFrameAiTicks = 0;
	UINT32			guiFrameAiCalls = 0;
	UINT32			guiFrameDecisions = 0;

	// current team turn
	UINT32			guiTurnDecisions = 0;
	INT64			giTurnAiTicks = 0;
	INT64			giTurnMaxTicks = 0;
	UINT8			gubTurnMaxSoldier = NOBODY;
	UINT32			guiTurnFrames = 0;
	UINT32			guiTurnSlowFrames = 0;
	INT64			giTurnMaxFrameTicks = 0;

	inline INT64 Now( )
	{
		LARGE_INTEGER li;
		QueryPerformanceCounter( &li );
		return li.QuadPart;
	}

	inline double Ms( INT64 iTicks )
	{
		return giFreq ? (double)iTicks * 1000.0 / (double)giFreq : 0.0;
	}

	inline INT64 TicksFromMs( UINT32 uiMs )
	{
		return (INT64)uiMs * giFreq / 1000;
	}

	void Write( const char* pLine )
	{
		sgp::Logger& logger = sgp::Logger::instance( );
		if ( !gfLogOpen )
		{
			gLogId = logger.createLogger( );
			logger.connectFile( gLogId, L"ja2mod-aiperf.log", true, sgp::Logger::FLUSH_ON_ENDL );
			gfLogOpen = true;

			char head[192];
			_snprintf( head, sizeof( head ), "AIPerf start min_decision_ms=%u min_frame_ms=%u qpc_hz=%I64d",
				guiMinDecisionMs, guiMinFrameMs, giFreq );
			head[sizeof( head ) - 1] = 0;
			SGP_LOG( gLogId ) << (const char*)head << sgp::endl;
		}
		SGP_LOG( gLogId ) << pLine << sgp::endl;
	}

	// snprintf-style append that never overruns and never goes backwards.
	int Append( char* pBuf, int iLen, int iPos, const char* pFmt, ... )
	{
		if ( iPos >= iLen - 1 )
			return iLen - 1;
		va_list args;
		va_start( args, pFmt );
		int n = _vsnprintf( pBuf + iPos, iLen - iPos, pFmt, args );
		va_end( args );
		if ( n < 0 || n >= iLen - iPos )
		{
			pBuf[iLen - 1] = 0;
			return iLen - 1;
		}
		return iPos + n;
	}

	int AppendCounters( char* pBuf, int iLen, int iPos, Accum const& a, bool fFrameLevel )
	{
		for ( int c = 0; c < AIPerf::NUM_COUNTERS; ++c )
		{
			if ( IsFrameLevel( (AIPerf::Counter)c ) != fFrameLevel || a.uiCalls[c] == 0 )
				continue;
			iPos = Append( pBuf, iLen, iPos, " %s=%u/%.1f", kNames[c], a.uiCalls[c], Ms( a.iTicks[c] ) );
		}
		return iPos;
	}

	const char* ModeName( )
	{
		return ( gTacticalStatus.uiFlags & INCOMBAT ) ? ( ( gTacticalStatus.uiFlags & TURNBASED ) ? "TB" : "RT" ) : "none";
	}

	bool AnyDecisionCounter( Accum const& a )
	{
		for ( int c = 0; c < AIPerf::C_OVERHEAD; ++c )
		{
			if ( a.uiCalls[c] )
				return true;
		}
		return false;
	}

	void LogDecision( SOLDIERTYPE* pSoldier, INT64 iTicks, INT8 bAlertBefore )
	{
		char buf[1024];
		int pos = 0;
		const int len = sizeof( buf );

		INT8 bAction = pSoldier->aiData.bAction;
		const char* pAction = ( bAction >= 0 && bAction <= AI_ACTION_LAST ) ? gzActionStr[bAction] : "?";
		char cRow = ( gWorldSectorY >= 1 && gWorldSectorY <= 16 ) ? (char)( 'A' + gWorldSectorY - 1 ) : '?';

		pos = Append( buf, len, pos, "decision %.1fms id=%u team=%d cls=%u grid=%d lvl=%d alert=%d>%d opp=%d aps=%d act=%s(%d) data=%d mode=%s sector=%c%d-%d men=%d/%d/%d/%d/%d t=%u |",
			Ms( iTicks ), (unsigned)pSoldier->ubID, (int)pSoldier->bTeam, (unsigned)pSoldier->ubSoldierClass,
			(int)pSoldier->sGridNo, (int)pSoldier->pathing.bLevel, (int)bAlertBefore, (int)pSoldier->aiData.bAlertStatus,
			(int)pSoldier->aiData.bOppCnt, (int)pSoldier->bActionPoints, pAction, (int)bAction, (int)pSoldier->aiData.usActionData,
			ModeName( ), cRow, (int)gWorldSectorX, (int)gbWorldSectorZ,
			(int)gTacticalStatus.Team[OUR_TEAM].bMenInSector, (int)gTacticalStatus.Team[ENEMY_TEAM].bMenInSector,
			(int)gTacticalStatus.Team[CREATURE_TEAM].bMenInSector, (int)gTacticalStatus.Team[MILITIA_TEAM].bMenInSector,
			(int)gTacticalStatus.Team[CIV_TEAM].bMenInSector, (unsigned)GetJA2Clock( ) );
		pos = AppendCounters( buf, len, pos, gDecision, false );
		Write( buf );
	}

	void LogFrame( INT64 iTicks )
	{
		char buf[512];
		int pos = 0;
		const int len = sizeof( buf );

		pos = Append( buf, len, pos, "frame %.1fms screen=%u overhead=%.1f ai=%.1f/%u(%u) render=%.1f refresh=%.1f mode=%s t=%u",
			Ms( iTicks ), (unsigned)guiCurrentScreen, Ms( gFrame.iTicks[AIPerf::C_OVERHEAD] ),
			Ms( giFrameAiTicks ), guiFrameAiCalls, guiFrameDecisions,
			Ms( gFrame.iTicks[AIPerf::C_RENDER] ), Ms( gFrame.iTicks[AIPerf::C_REFRESH] ),
			ModeName( ), (unsigned)GetJA2Clock( ) );
		Write( buf );
	}

	void ResetTurn( )
	{
		guiTurnDecisions = 0;
		giTurnAiTicks = 0;
		giTurnMaxTicks = 0;
		gubTurnMaxSoldier = NOBODY;
		guiTurnFrames = 0;
		guiTurnSlowFrames = 0;
		giTurnMaxFrameTicks = 0;
	}
}

void AIPerf::Configure( BOOLEAN fEnabled, UINT32 uiMinDecisionMs, UINT32 uiMinFrameMs )
{
	LARGE_INTEGER li;
	if ( !QueryPerformanceFrequency( &li ) || li.QuadPart <= 0 )
	{
		gfEnabled = FALSE;
		return;
	}
	giFreq = li.QuadPart;
	gfEnabled = fEnabled ? TRUE : FALSE;
	guiMinDecisionMs = uiMinDecisionMs;
	guiMinFrameMs = uiMinFrameMs;
	gDecision.Reset( );
	gFrame.Reset( );
	ResetTurn( );
}

void AIPerf::BeginFrame( )
{
	if ( !gfEnabled )
		return;
	giFrameStart = Now( );
	gFrame.Reset( );
	giFrameAiTicks = 0;
	guiFrameAiCalls = 0;
	guiFrameDecisions = 0;
}

void AIPerf::EndFrame( )
{
	if ( !gfEnabled || giFrameStart == 0 )
		return;
	INT64 iTicks = Now( ) - giFrameStart;
	giFrameStart = 0;

	++guiTurnFrames;
	if ( iTicks > giTurnMaxFrameTicks )
		giTurnMaxFrameTicks = iTicks;
	if ( iTicks >= TicksFromMs( guiMinFrameMs ) )
	{
		++guiTurnSlowFrames;
		LogFrame( iTicks );
	}
}

void AIPerf::TurnEnded( UINT8 ubTeam )
{
	if ( !gfEnabled )
		return;
	if ( guiTurnDecisions > 0 )
	{
		char buf[256];
		Append( buf, sizeof( buf ), 0, "turn team=%u decisions=%u ai=%.1fms max=%.1fms(id %u) frames=%u slow=%u maxframe=%.1fms",
			(unsigned)ubTeam, guiTurnDecisions, Ms( giTurnAiTicks ), Ms( giTurnMaxTicks ), (unsigned)gubTurnMaxSoldier,
			guiTurnFrames, guiTurnSlowFrames, Ms( giTurnMaxFrameTicks ) );
		Write( buf );
	}
	ResetTurn( );
}

AIPerf::DecisionScope::DecisionScope( SOLDIERTYPE* pSoldier )
	: _pSoldier( pSoldier ), _iStart( 0 ), _bAlertBefore( 0 )
{
	if ( !gfEnabled || pSoldier == NULL )
		return;
	if ( giDepth++ > 0 )
		return;
	gDecision.Reset( );
	_bAlertBefore = pSoldier->aiData.bAlertStatus;
	_iStart = Now( );
}

AIPerf::DecisionScope::~DecisionScope( )
{
	if ( !gfEnabled || _pSoldier == NULL )
		return;
	--giDepth;
	if ( _iStart == 0 )
		return;
	INT64 iTicks = Now( ) - _iStart;

	giFrameAiTicks += iTicks;
	++guiFrameAiCalls;

	bool fDidWork = AnyDecisionCounter( gDecision ) || iTicks >= TicksFromMs( 1 );
	if ( !fDidWork )
		return;

	++guiFrameDecisions;
	++guiTurnDecisions;
	giTurnAiTicks += iTicks;
	if ( iTicks > giTurnMaxTicks )
	{
		giTurnMaxTicks = iTicks;
		gubTurnMaxSoldier = _pSoldier->ubID;
	}
	if ( iTicks >= TicksFromMs( guiMinDecisionMs ) )
		LogDecision( _pSoldier, iTicks, _bAlertBefore );
}

AIPerf::Scope::Scope( Counter eCounter )
	: _eCounter( eCounter ), _iStart( 0 )
{
	if ( !gfEnabled )
		return;
	if ( giDepth == 0 && !IsFrameLevel( eCounter ) )
		return;
	_iStart = Now( );
}

AIPerf::Scope::~Scope( )
{
	if ( _iStart == 0 )
		return;
	INT64 iTicks = Now( ) - _iStart;
	Accum& a = IsFrameLevel( _eCounter ) ? gFrame : gDecision;
	a.iTicks[_eCounter] += iTicks;
	++a.uiCalls[_eCounter];
}
