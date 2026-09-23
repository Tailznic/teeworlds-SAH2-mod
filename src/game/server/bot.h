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
	int m_SpikeIdx;       // cached spike goal while carrying an enemy, -1 = none
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

	// fng_trainbot: aim error that grows with unpredictable target movement
	vec2 m_AimNoise;      // current aim offset applied against the enemy
	vec2 m_LastTargetVel; // target velocity measured last tick
	float m_Predict;      // 0 = smooth movement (hits), 1 = hard zigzag (misses)
	int m_NoiseTick;      // ticks until the aim error is re-rolled
};

#endif