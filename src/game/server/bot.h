/* SAH: server-side practice bot (no network client behind its slot) */
#ifndef GAME_SERVER_BOT_H
#define GAME_SERVER_BOT_H

#include <base/vmath.h>

class CGameContext;

class CBotAI
{
public:
	void Reset();
	void Tick(CGameContext *pGameServer, int ClientID);

private:
	int m_TargetCID;      // current enemy to chase
	int m_RetargetTick;   // next tick we may re-pick the target
	int m_BackoffTicks;   // stepping away after a throw release
	int m_BackoffDir;     // -1/1 direction while backing off
	int m_JumpTicks;      // jump held this many ticks left (stuck hop)
	int m_JumpCooldown;   // ticks until another hop is allowed
	int m_StuckTick;      // last position check
	vec2 m_LastPos;       // position at the last stuck check
	int m_FireState;      // persistent input fire counter (must never step backwards)
	int m_IdleTicks;      // wander timer when there is nobody to fight
	int m_IdleDir;        // wander direction

	// fng_trainbot: how unreadable the enemy's movement is this moment
	vec2 m_LastTargetVel; // target velocity measured last tick
	float m_Predict;      // 0 = smooth movement (hits), 1 = hard zigzag (misses)

	// positioning: patrol inside the engagement band instead of standing still
	int m_StrafeDir;
	int m_StrafeTicks;

	int m_CarryTicks;     // ticks the current prey has been on the hook

	// vertical navigation: hook-climb to higher platforms
	int m_ClimbTicks;
	int m_ClimbDir;
	int m_ClimbAnchorTick;
	vec2 m_ClimbAnchor;
	bool m_ClimbingUp;
	bool m_SpawnInitialized;

	// hook-boost bursts (people hook the ground ahead to accelerate)
	int m_BoostTicks;
	int m_BoostCooldown;
	int m_BoostDir;
	vec2 m_BoostAnchor;

	// fng_trainbot: patrol along the map's precomputed standable shelves
	int m_NavIdx;            // current patrol point, -1 = pick one
	vec2 m_NavGoal;
	int m_NavRetargetTick;
	void PickNavPoint(CGameContext *pGS, vec2 MyPos, int MyTeam, int Tick, int FloorPref = -1);

	// fng_trainbot: human accuracy. Every bot gets its own skill, and its form
	// swings between "dead on" and "wild miss" while it plays.
	float m_Skill;         // 0..1 base skill of this bot
	int m_AimMode;         // 0 = dead on, 1 = good, 2 = average, 3 = wild miss
	int m_AimModeTicks;    // ticks left of the current form
	float m_AimErr;        // angular error of the current shot, radians
	float m_AimErrSide;    // -1/1: which side of the target we miss
	float m_Lead;          // target lead in ticks, negative = the bot reacts late
	int m_HoldFireTicks;   // a thinking player sometimes simply does not shoot
	int m_LastGrabTick;    // when the prey was last put on the hook (re-reel)
	void RollProfile();
	void RollAimForm();
	vec2 AddAimError(vec2 Aim, float Scale = 1.0f);

	// fng_trainbot: floor hunting — the bot works a floor, not a tile
	int m_FloorGoal;       // floor currently being hunted/patrolled, -1 = none
	int m_FloorTicks;      // how long that plan is kept
	int m_HomeFloor;       // favourite floor the bot keeps coming back to
	int m_MyFloor;         // last floor we stood on (jumps do not change it)
	int m_TargetFloor;     // last floor of the current enemy, -1 = nobody

	// fng_trainbot: spike throws. A hooked victim drifts towards the hooker, so
	// the throw is geometry: stand where a spike cluster sits on the line
	// prey -> bot and reel in.
	int m_ThrowIdx;        // chosen spike cluster, -1 = no plan
	vec2 m_ThrowStand;     // spot to stand on while dragging
	int m_ThrowTick;       // next moment to re-plan the throw
	void PlanThrow(CGameContext *pGS, vec2 MyPos, vec2 PreyPos, int MyTeam, int Tick);
};

#endif