/* fng_trainbot: server-side practice bot — feeds CNetObj_PlayerInput for a
   slot that has no network client behind it (see CGameContext::CreateBot). */
#include <base/math.h>
#include <base/vmath.h>
#include <game/collision.h>
#include <game/generated/protocol.h>
#include "gamecontext.h"
#include "bot.h"

// every tile flag that kills on contact (spikes + generic death tiles)
static const int BOT_DANGER_MASK =
	CCollision::COLFLAG_SPIKE_NORMAL | CCollision::COLFLAG_SPIKE_RED |
	CCollision::COLFLAG_SPIKE_BLUE | CCollision::COLFLAG_SPIKE_GOLD |
	CCollision::COLFLAG_SPIKE_GREEN | CCollision::COLFLAG_SPIKE_PURPLE |
	CCollision::COLFLAG_DEATH;

// spike flags the bot may throw an enemy onto: own colour or neutral.
// enemy-colour spikes are wrong terrain (scoring rules punish / waste them)
static bool BotValidSpikeForTeam(int Flags, int Team)
{
	int S = Flags & (BOT_DANGER_MASK & ~CCollision::COLFLAG_DEATH);
	if(!S)
		return false;
	if((S & CCollision::COLFLAG_SPIKE_RED) && Team != TEAM_RED)
		return false;
	if((S & CCollision::COLFLAG_SPIKE_BLUE) && Team != TEAM_BLUE)
		return false;
	return true;
}

// distance penalty per spike colour: people farm the frequent team/normal
// tiles; gold/purple are never worth dragging the prey across the whole map
static float BotSpikePenalty(int Flags)
{
	int S = Flags & (BOT_DANGER_MASK & ~CCollision::COLFLAG_DEATH);
	if(S & (CCollision::COLFLAG_SPIKE_RED | CCollision::COLFLAG_SPIKE_BLUE))
		return 0.0f;   // team tiles: the bread and butter
	if(S & CCollision::COLFLAG_SPIKE_NORMAL)
		return 250.0f;
	if(S & CCollision::COLFLAG_SPIKE_GREEN)
		return 700.0f;
	if(S & CCollision::COLFLAG_SPIKE_PURPLE)
		return 1100.0f; // +10 player points, but only if close
	if(S & CCollision::COLFLAG_SPIKE_GOLD)
		return 1500.0f; // least priority
	return 1000.0f;
}

static bool BotIsEnemy(CPlayer *pA, CPlayer *pB)
{
	if(!pA || !pB || pA == pB)
		return false;
	int TA = pA->GetTeam();
	int TB = pB->GetTeam();
	if(TA < TEAM_RED || TA > TEAM_BLUE || TB < TEAM_RED || TB > TEAM_BLUE)
		return false;
	return TA != TB;
}

static bool BotLineOfSight(CGameContext *pGS, vec2 a, vec2 b)
{
	return pGS->Collision()->IntersectLine(a, b, 0, 0) == 0;
}

// solid tile the hook can actually latch onto (unhookable tiles bounce it)
static bool BotHookablePoint(CGameContext *pGS, vec2 p)
{
	int f = pGS->Collision()->GetCollisionAt(p.x, p.y);
	return (f & CCollision::COLFLAG_SOLID) && !(f & CCollision::COLFLAG_NOHOOK);
}

// walking one step further would drop us over an edge with kill tiles in the
// shaft below — the mid-map "stupid fall" deaths happen exactly like this
static bool BotDeadlyDrop(CGameContext *pGS, vec2 MyPos, int Dir)
{
	CCollision *pCol = pGS->Collision();
	float x = MyPos.x + Dir * 52.0f;
	if(pCol->GetCollisionAt(x, MyPos.y + 30.0f) & CCollision::COLFLAG_SOLID)
		return false; // ground continues under the next step
	for(int dy = 48; dy <= 340; dy += 28)
	{
		if(pCol->GetCollisionAt(x, MyPos.y + (float)dy) & BOT_DANGER_MASK)
			return true;
	}
	return false;
}

// hook anchor somewhere above us to pull the tee up to the higher platforms
static bool BotFindClimbAnchor(CGameContext *pGS, vec2 From, vec2 Want, vec2 *pOut)
{
	static const float ady[] = {-360.0f, -500.0f, -240.0f, -640.0f};
	static const float adx[] = {0.0f, -70.0f, 70.0f, -35.0f, 35.0f};
	for(int j = 0; j < 4; j++)
	{
		for(int i = 0; i < 5; i++)
		{
			vec2 Cand = vec2(From.x + adx[i], From.y + ady[j]);
			if(length(Cand - From) > 700.0f)
				continue;
			if(!BotHookablePoint(pGS, Cand))
				continue;
			if(!BotLineOfSight(pGS, From, Cand))
				continue;
			*pOut = Cand;
			return true;
		}
	}
	return false;
}

// ground/wall point ahead and below — hooking it and running yanks the tee
// forward, the same acceleration burst people use to reach points first
static bool BotFindBoostAnchor(CGameContext *pGS, vec2 From, int Dir, vec2 *pOut)
{
	vec2 To = From + vec2((float)Dir * 380.0f, 170.0f);
	vec2 Hit = To;
	pGS->Collision()->IntersectLine(From, To, &Hit, 0);
	if(!BotHookablePoint(pGS, Hit))
		return false;
	int f = pGS->Collision()->GetCollisionAt(Hit.x, Hit.y);
	if(f & BOT_DANGER_MASK)
		return false;
	if(length(Hit - From) < 80.0f)
		return false;
	*pOut = Hit;
	return true;
}

void CBotAI::Reset()
{
	m_TargetCID = -1;
	m_SpikeIdx = -1;
	m_RetargetTick = 0;
	m_BackoffTicks = 0;
	m_BackoffDir = 1;
	m_JumpTicks = 0;
	m_JumpCooldown = 0;
	m_StuckTick = 0;
	m_LastPos = vec2(0.0f, 0.0f);
	m_FireState = 0;
	m_IdleTicks = 0;
	m_IdleDir = 1;
	m_AimNoise = vec2(0.0f, 0.0f);
	m_LastTargetVel = vec2(0.0f, 0.0f);
	m_Predict = 0.0f;
	m_NoiseTick = 0;
	m_StrafeDir = 1;
	m_StrafeTicks = 0;
	m_PumpTicks = 0;
	m_CarryTicks = 0;
	m_ClimbTicks = 0;
	m_ClimbDir = 0;
	m_ClimbAnchorTick = 0;
	m_ClimbAnchor = vec2(0.0f, 0.0f);
	m_BoostTicks = 0;
	m_BoostCooldown = 0;
	m_BoostDir = 1;
	m_BoostAnchor = vec2(0.0f, 0.0f);
	m_DodgeTicks = 0;
	m_DodgeCooldown = 0;
	m_NavIdx = -1;
	m_NavGoal = vec2(0.0f, 0.0f);
	m_NavRetargetTick = 0;
}

// fng_trainbot: pick the next patrol point — own side favoured, the mid
// band (fight zone) preferred, the high platforms and the enemy half only
// visited now and then; jitter so no two walks are the same
void CBotAI::PickNavPoint(CGameContext *pGS, vec2 MyPos, int MyTeam, int Tick)
{
	if(pGS->m_NumBotNav <= 0)
	{
		m_NavIdx = -1;
		m_NavRetargetTick = Tick + 100;
		return;
	}
	int MySide = MyTeam == TEAM_RED ? -1 : (MyTeam == TEAM_BLUE ? 1 : 0);
	float Best = 0.0f;
	int BestI = -1;
	for(int t = 0; t < 12; t++)
	{
		int i = (int)(frandom() * pGS->m_NumBotNav);
		if(i < 0 || i >= pGS->m_NumBotNav)
			continue;
		const CGameContext::CBotNavPoint &P = pGS->m_aBotNav[i];
		float d = distance(MyPos, P.m_Pos);
		if(d > 1400.0f)
			continue;
		float Score = d * 0.5f + frandom() * 700.0f;
		if(d < 200.0f)
			Score += 2000.0f;           // don't re-pick what we already reached
		if(P.m_Side == MySide && MySide != 0)
			Score -= 400.0f;            // hold your own half
		else if(P.m_Side != 0 && MySide != 0 && P.m_Side != MySide)
			Score += 300.0f;            // raids into the enemy half are rare
		if(P.m_Band == 1)
			Score -= 250.0f;            // mid band is where the fight happens
		else if(P.m_Band == 2 && frandom() < 0.3f)
			Score -= 200.0f;            // sometimes take the high ground
		if(BestI < 0 || Score < Best)
		{
			Best = Score;
			BestI = i;
		}
	}
	if(BestI < 0)
		BestI = (int)(frandom() * pGS->m_NumBotNav) % pGS->m_NumBotNav;
	m_NavIdx = BestI;
	m_NavGoal = pGS->m_aBotNav[BestI].m_Pos;
	m_NavRetargetTick = Tick + 100 + (int)(frandom() * 150.0f);
}

void CBotAI::Tick(CGameContext *pGS, int ClientID)
{
	CPlayer *pSelf = pGS->m_apPlayers[ClientID];
	if(!pSelf)
		return;

	CNetObj_PlayerInput Input;
	mem_zero(&Input, sizeof(Input));
	Input.m_PlayerFlags = PLAYERFLAG_PLAYING;
	Input.m_TargetX = 0;
	Input.m_TargetY = -1;

	// dead / waiting for respawn: keep the fire counter moving so
	// CPlayer::OnDirectInput keeps requesting a respawn (m_Spawning)
	CCharacter *pMe = pSelf->GetCharacter();
	if(!pMe || !pMe->IsAlive())
	{
		m_FireState = (m_FireState + 1) & INPUT_STATE_MASK;
		Input.m_Fire = m_FireState;
		pGS->OnClientDirectInput(ClientID, &Input);
		pGS->OnClientPredictedInput(ClientID, &Input);
		return;
	}

	const int Tick = pGS->Server()->Tick();
	const vec2 MyPos = pMe->m_Pos;
	const int MyTeam = pSelf->GetTeam();

	// --- pick a target: nearest enemy, frozen prey counts as much closer ---
	if(Tick >= m_RetargetTick)
	{
		m_RetargetTick = Tick + 10;
		m_TargetCID = -1;
		float Best = 0.0f;
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(i == ClientID)
				continue;
			CPlayer *p = pGS->m_apPlayers[i];
			if(!BotIsEnemy(pSelf, p))
				continue;
			CCharacter *pC = p->GetCharacter();
			if(!pC || !pC->IsAlive())
				continue;
			float Score = distance(MyPos, pC->m_Pos) - (pC->IsFrozen() ? 600.0f : 0.0f);
			if(m_TargetCID < 0 || Score < Best)
			{
				Best = Score;
				m_TargetCID = i;
			}
		}
	}

	CCharacter *pTarget = 0;
	if(m_TargetCID >= 0)
	{
		CPlayer *p = pGS->m_apPlayers[m_TargetCID];
		if(BotIsEnemy(pSelf, p))
		{
			pTarget = p->GetCharacter();
			if(pTarget && !pTarget->IsAlive())
				pTarget = 0;
		}
		else
			m_TargetCID = -1;
	}

	// --- fng_trainbot: score how unpredictably the target moves ---
	if(pTarget)
	{
		vec2 Vel = pTarget->GetVel();
		float Dv = length(Vel - m_LastTargetVel);
		m_LastTargetVel = Vel;
		float Inst = clamp(Dv / 60.0f, 0.0f, 1.0f);
		m_Predict = clamp(m_Predict * 0.97f + Inst * 0.15f, 0.0f, 1.0f);
	}
	else
	{
		m_LastTargetVel = vec2(0.0f, 0.0f);
		m_Predict *= 0.9f;
	}

	// fng_trainbot: deviations from the ideal shot are re-rolled every 1-2
	// ticks with a base error even against a still target — the bot does NOT
	// model laser bounces off walls (people are the worst at those anyway),
	// it just misses more often; zigzag movement widens the error further
	if(m_NoiseTick <= 0)
	{
		float MaxErr = 65.0f + m_Predict * m_Predict * 270.0f;
		float a = frandom() * 2.0f * pi;
		m_AimNoise = vec2(cos(a), sin(a)) * (0.35f + 0.65f * frandom()) * MaxErr;
		m_NoiseTick = 1 + (frandom() < 0.35f ? 1 : 0);
	}
	m_NoiseTick--;

	// --- am I dragging a frozen enemy on my hook? ---
	CCharacter *pCarried = 0;
	if(pMe->IsHookGrabbed())
	{
		int Hooked = pMe->GetHookedPlayerID();
		if(Hooked >= 0 && Hooked < MAX_CLIENTS)
		{
			CPlayer *pH = pGS->m_apPlayers[Hooked];
			if(BotIsEnemy(pSelf, pH))
			{
				CCharacter *pHC = pH->GetCharacter();
				if(pHC && pHC->IsAlive() && pHC->IsFrozen())
					pCarried = pHC;
			}
		}
	}
	if(pCarried)
	{
		if(m_CarryTicks < 100000)
			m_CarryTicks++;
	}
	else
	{
		m_CarryTicks = 0;
		m_PumpTicks = 0;
		m_SpikeIdx = -1; // nothing to throw right now
	}

	// --- spike goal: closest effective kill tiles, weighted by colour ---
	//     priority: team > normal > green > purple > gold — never drag the
	//     prey across the whole map just for the fancy tiles
	if(pCarried && m_SpikeIdx < 0)
	{
		float Best = 0.0f;
		for(int i = 0; i < pGS->m_NumBotSpikes; i++)
		{
			if(!BotValidSpikeForTeam(pGS->m_aBotSpikes[i].m_Flags, MyTeam))
				continue;
			float d = distance(pCarried->m_Pos, pGS->m_aBotSpikes[i].m_Pos);
			if(d > 2000.0f)
				continue;
			float Score = d + BotSpikePenalty(pGS->m_aBotSpikes[i].m_Flags);
			if(m_SpikeIdx < 0 || Score < Best)
			{
				Best = Score;
				m_SpikeIdx = i;
			}
		}
	}

	bool HaveSpike = pCarried && m_SpikeIdx >= 0;
	vec2 SpikePos = HaveSpike ? pGS->m_aBotSpikes[m_SpikeIdx].m_Pos : MyPos;

	// fng_trainbot: one navigation goal — the enemy when there is one,
	// otherwise the current patrol point on the map's shelves
	vec2 Goal = pTarget ? pTarget->m_Pos : m_NavGoal;
	bool HaveGoal = pTarget != 0 || (pGS->m_NumBotNav > 0 && m_NavIdx >= 0);

	// --- vertical states: hook-climb up, hook-boost forward ---
	bool Climbing = m_ClimbTicks > 0;
	bool Boosting = m_BoostTicks > 0;
	bool WantClimb = false;

	if(Climbing)
	{
		m_ClimbTicks--;
		if(!HaveGoal || m_ClimbTicks <= 0 ||
			Goal.y - MyPos.y > -60.0f ||
			!BotHookablePoint(pGS, m_ClimbAnchor))
		{
			m_ClimbTicks = 0;
			Climbing = false;
		}
	}
	if(!pCarried && m_BackoffTicks <= 0 && HaveGoal && !Boosting)
	{
		float dy = Goal.y - MyPos.y;
		float hd = fabsf(Goal.x - MyPos.x);
		if(dy < -140.0f && hd < 700.0f)
			WantClimb = true;
	}
	if(WantClimb && !Climbing && Tick >= m_ClimbAnchorTick)
	{
		m_ClimbAnchorTick = Tick + 8;
		vec2 A;
		if(BotFindClimbAnchor(pGS, MyPos, Goal, &A))
		{
			m_ClimbAnchor = A;
			m_ClimbTicks = 70;
			float dxa = Goal.x - MyPos.x;
			m_ClimbDir = dxa > 40.0f ? 1 : (dxa < -40.0f ? -1 : 0);
			Climbing = true;
		}
	}

	if(Boosting)
	{
		m_BoostTicks--;
		if(m_BoostTicks <= 0 || !BotHookablePoint(pGS, m_BoostAnchor))
		{
			m_BoostTicks = 0;
			Boosting = false;
		}
	}
	if(m_BoostCooldown > 0)
		m_BoostCooldown--;
	if(m_DodgeTicks > 0)
		m_DodgeTicks--;
	if(m_DodgeCooldown > 0)
		m_DodgeCooldown--;
	if(!pCarried && !Climbing && !Boosting && m_BackoffTicks <= 0 &&
		m_BoostCooldown <= 0 && HaveGoal)
	{
		float d = distance(MyPos, Goal);
		float ddx = Goal.x - MyPos.x;
		int sdir = ddx > 12.0f ? 1 : (ddx < -12.0f ? -1 : 0);
		if(d > 650.0f && sdir != 0 && pMe->IsGrounded())
		{
			vec2 A;
			if(BotFindBoostAnchor(pGS, MyPos, sdir, &A))
			{
				m_BoostAnchor = A;
				m_BoostTicks = 14;
				m_BoostCooldown = 110;
				m_BoostDir = sdir;
				Boosting = true;
			}
		}
	}

	// fng_trainbot: the human hook-down redirect — 37% of all hook launches
	// in the demo went straight down; a quick mid-air pump to kill momentum
	// or dodge, no jump with it (the demo never combined the two)
	if(!pCarried && !Climbing && !Boosting && m_BackoffTicks <= 0 &&
		m_DodgeTicks <= 0 && m_DodgeCooldown <= 0 && pTarget &&
		!pMe->IsFrozen() && !pMe->IsGrounded() &&
		distance(MyPos, pTarget->m_Pos) < 600.0f && frandom() < 0.02f)
	{
		m_DodgeTicks = 8;
		m_DodgeCooldown = 80 + (int)(frandom() * 100.0f);
	}

	// --- movement: take position, never just stand there ---
	int Dir = 0;
	if(m_BackoffTicks > 0)
	{
		m_BackoffTicks--;
		Dir = m_BackoffDir;
	}
	else if(pCarried && m_PumpTicks > 0)
	{
		// pump phase of the throw: keep the hook, drag the prey back off
		// the spikes, then charge again — builds momentum for the release
		m_PumpTicks--;
		float Away = SpikePos.x - MyPos.x;
		Dir = Away > 4.0f ? -1 : (Away < -4.0f ? 1 : m_IdleDir);
	}
	else if(pCarried)
	{
		// close in on the goal (or stay over the prey if no spike found)
		float dx = (HaveSpike ? SpikePos.x : pCarried->m_Pos.x) - MyPos.x;
		Dir = fabsf(dx) > 8.0f ? (dx > 0.0f ? 1 : -1) : 0;
	}
	else if(Boosting)
		Dir = m_BoostDir;
	else if(pTarget)
	{
		float dx = pTarget->m_Pos.x - MyPos.x;
		float dy = pTarget->m_Pos.y - MyPos.y;
		float hd = fabsf(dx);
		if(Climbing)
			Dir = m_ClimbDir; // drift towards the prey while being pulled up
		else if(dy < -140.0f && hd < 700.0f)
			Dir = hd > 40.0f ? (dx > 0.0f ? 1 : -1) : 0; // get under him
		else if(hd > 430.0f || dy > 260.0f)
			Dir = hd > 12.0f ? (dx > 0.0f ? 1 : -1) : 0; // close the gap / drop down
		else if(hd < 230.0f)
			Dir = dx != 0.0f ? (dx > 0.0f ? -1 : 1) : m_StrafeDir; // too close: make space
		else
		{
			// engagement band: patrol left-right instead of standing still
			if(--m_StrafeTicks <= 0)
			{
				m_StrafeTicks = 25 + (int)(frandom() * 25.0f);
				m_StrafeDir = frandom() < 0.5f ? -1 : 1;
			}
			Dir = m_StrafeDir;
		}
	}
	else
	{
		// fng_trainbot: patrol the map's shelves instead of pacing one
		// spot — own side first, mid band preferred, re-pick on arrival
		// or when the walk takes too long
		if(m_NavIdx < 0 || pGS->m_NumBotNav <= 0 ||
			(Tick >= m_NavRetargetTick && !Climbing) ||
			distance(MyPos, m_NavGoal) < 72.0f)
			PickNavPoint(pGS, MyPos, MyTeam, Tick);
		if(m_NavIdx >= 0 && pGS->m_NumBotNav > 0)
		{
			float dx = m_NavGoal.x - MyPos.x;
			Dir = fabsf(dx) > 16.0f ? (dx > 0.0f ? 1 : -1) : 0;
		}
		else
		{
			// no nav data on this map: legacy wander
			if(--m_IdleTicks <= 0)
			{
				m_IdleTicks = 100;
				m_IdleDir = -m_IdleDir;
			}
			Dir = m_IdleDir;
		}
	}

	// --- safety: horizontal spikes ahead + deadly shafts below the edge ---
	bool EdgeBlocked = false;
	if(Dir != 0)
	{
		int f = pGS->Collision()->GetCollisionAt(MyPos.x + Dir * 70.0f, MyPos.y);
		int f2 = pGS->Collision()->GetCollisionAt(MyPos.x + Dir * 70.0f, MyPos.y - 20.0f);
		int f3 = pGS->Collision()->GetCollisionAt(MyPos.x + Dir * 70.0f, MyPos.y + 16.0f);
		if((f | f2 | f3) & BOT_DANGER_MASK)
		{
			if(Boosting)
			{
				m_BoostTicks = 0;
				Boosting = false;
				Dir = 0;
			}
			else if(pCarried || Climbing)
				Dir = 0;
			else
				Dir = -Dir; // pace back instead of walking into the spikes
			EdgeBlocked = pCarried != 0;
		}
		else if(BotDeadlyDrop(pGS, MyPos, Dir))
		{
			// the mid-map fall: something lethal lies at the bottom of this
			// edge — stop before stepping into the shaft
			Dir = 0;
			EdgeBlocked = pCarried != 0;
		}
	}

	// --- release (throw): let the prey's momentum carry it into the tiles ---
	bool WantRelease = false;
	if(pCarried && HaveSpike)
	{
		float dMe = distance(MyPos, SpikePos);
		vec2 ToSpike = SpikePos - pCarried->m_Pos;
		float dEnemy = length(ToSpike);
		float VelToward = dEnemy > 1.0f ? dot(pCarried->GetVel(), normalize(ToSpike)) : 0.0f;
		float Slide = VelToward * 6.5f + 50.0f; // ground-friction slide estimate
		if(dEnemy < 750.0f && Slide > dEnemy - 30.0f)
			WantRelease = true; // momentum covers the remaining gap
		else if(EdgeBlocked && dEnemy < 320.0f && VelToward > 30.0f)
			WantRelease = true; // at the lip: push it over
		else if(EdgeBlocked && SpikePos.y > MyPos.y + 60.0f &&
			fabsf(SpikePos.x - MyPos.x) < 160.0f && dEnemy < 500.0f)
			WantRelease = true; // the goal is down the shaft: drop it in
		else if(m_CarryTicks > 260)
			WantRelease = true; // frozen on the hook too long: reset the cycle
		(void)dMe;
	}
	if(WantRelease)
	{
		float Away = MyPos.x - SpikePos.x;
		if(Away > 4.0f) m_BackoffDir = -1;
		else if(Away < -4.0f) m_BackoffDir = 1;
		else m_BackoffDir = m_IdleDir;
		m_BackoffTicks = 40;
		m_PumpTicks = 0;
		m_CarryTicks = 0;
		m_SpikeIdx = -1;
		HaveSpike = false;
	}
	else if(pCarried && HaveSpike && EdgeBlocked && m_PumpTicks <= 0 &&
		m_BackoffTicks <= 0 && m_CarryTicks > 40)
	{
		// momentum isn't building: oscillate — drag the prey back a little,
		// then charge the spikes again (people do exactly this)
		m_PumpTicks = 24;
	}

	// --- stuck detection: hop over small obstacles ---
	if(Tick - m_StuckTick >= 16)
	{
		float Moved = distance(MyPos, m_LastPos);
		if(Dir != 0 && Moved < 10.0f && m_JumpCooldown <= 0 && !pMe->IsFrozen())
		{
			m_JumpTicks = 12;
			m_JumpCooldown = 45;
		}
		m_LastPos = MyPos;
		m_StuckTick = Tick;
	}
	bool WantJump = m_JumpTicks > 0;
	if(m_JumpTicks > 0) m_JumpTicks--;
	if(m_JumpCooldown > 0) m_JumpCooldown--;

	// fng_trainbot: humans hop constantly even when nothing blocks them
	// (demo: ~28 ground jumps/min) — keeps the walk from looking robotic
	if(pMe->IsGrounded() && m_JumpCooldown <= 0 && !pMe->IsFrozen() &&
		!pCarried && !Climbing && !Boosting && m_DodgeTicks <= 0 &&
		m_BackoffTicks <= 0 && frandom() < 0.008f)
	{
		m_JumpTicks = 10;
		m_JumpCooldown = 60;
		WantJump = true;
	}

	// --- hook priority: throw cycle > climb/boost anchor > attack ---
	bool JustReleased = WantRelease;
	bool WantHook = false;
	if(m_BackoffTicks <= 0 && !JustReleased)
	{
		if(pCarried || m_PumpTicks > 0)
			WantHook = true;
		else if(Climbing || Boosting)
			WantHook = true;
		else if(m_DodgeTicks > 0)
			WantHook = true; // the straight-down redirect
		else if(pTarget)
		{
			// fng_trainbot: demo grab range was avg 157 / max 317 px —
			// people never hook from 750, they walk in first
			float d = distance(MyPos, pTarget->m_Pos);
			if(d < 340.0f && BotLineOfSight(pGS, MyPos, pTarget->m_Pos))
				WantHook = true;
		}
	}

	// --- SAH: frozen teammates are saved with the pistol ---
	CCharacter *pHeal = 0;
	if(pGS->m_pController->UsesSahScoring() && MyTeam >= TEAM_RED && MyTeam <= TEAM_BLUE)
	{
		float BestH = 0.0f;
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(i == ClientID)
				continue;
			CPlayer *p = pGS->m_apPlayers[i];
			if(!p || p->GetTeam() != MyTeam)
				continue;
			CCharacter *pC = p->GetCharacter();
			if(!pC || !pC->IsAlive() || !pC->IsFrozen())
				continue;
			float d = distance(MyPos, pC->m_Pos);
			if(d > 650.0f)
				continue;
			if(!pHeal || d < BestH)
			{
				BestH = d;
				pHeal = pC;
			}
		}
	}

	// --- shooting: live enemies only, never while climbing/boosting,
	//     dragging or dodging (the throw stays undisturbed) ---
	bool Busy = m_BackoffTicks > 0 || JustReleased || Climbing || Boosting ||
		m_PumpTicks > 0 || pCarried || m_DodgeTicks > 0;
	bool WantFire = false;
	if(!Busy && !pMe->IsFrozen())
	{
		if(pHeal)
			WantFire = BotLineOfSight(pGS, MyPos, pHeal->m_Pos);
		else if(pTarget && !pTarget->IsFrozen())
		{
			// fng_trainbot: demo shots flew at avg 3.3 / p50 1.9 tiles —
			// close-range duelling, not sniping across the map
			float d = distance(MyPos, pTarget->m_Pos);
			if(d > 60.0f && d < 420.0f && BotLineOfSight(pGS, MyPos, pTarget->m_Pos))
				WantFire = true;
		}
	}
	if(WantFire)
		m_FireState = (m_FireState + 1) & INPUT_STATE_MASK;
	else if(m_FireState & 1)
		m_FireState = (m_FireState + 1) & INPUT_STATE_MASK; // land on even so fullauto stops
	Input.m_Fire = m_FireState;

	// --- aim: dodge down > boost/climb anchors > heal > prey > target+noise
	//     > the patrol point we walk to ---
	vec2 Aim;
	bool HaveAim = false;
	if(m_DodgeTicks > 0)
	{
		// straight down with a slight jitter — the human hook-down pump
		Aim = vec2((frandom() - 0.5f) * 60.0f, 300.0f);
		HaveAim = true;
	}
	else if(Boosting)
	{
		Aim = m_BoostAnchor - MyPos;
		HaveAim = true;
	}
	else if(Climbing)
	{
		Aim = m_ClimbAnchor - MyPos;
		HaveAim = true;
	}
	else
	{
		CCharacter *pAim = pHeal ? pHeal : (pCarried ? pCarried : pTarget);
		if(pAim)
		{
			Aim = pAim->m_Pos + pAim->GetVel() * 0.15f - MyPos;
			// fng_trainbot: against the enemy the aim carries the deviation,
			// healing and dragging stay precise
			if(pTarget && pAim == pTarget)
				Aim += m_AimNoise;
			HaveAim = true;
		}
		else if(HaveGoal)
		{
			// fng_trainbot: while patrolling look where we walk
			Aim = Goal - MyPos;
			HaveAim = true;
		}
	}
	if(HaveAim)
	{
		if(length(Aim) < 1.0f)
			Aim = vec2(0.0f, -1.0f);
		Input.m_TargetX = (int)Aim.x;
		Input.m_TargetY = (int)Aim.y;
	}

	Input.m_Direction = Dir;
	Input.m_Jump = WantJump ? 1 : 0;
	Input.m_Hook = WantHook ? 1 : 0;

	pGS->OnClientDirectInput(ClientID, &Input);
	pGS->OnClientPredictedInput(ClientID, &Input);
}