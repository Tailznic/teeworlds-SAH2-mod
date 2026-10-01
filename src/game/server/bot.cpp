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

// fng_trainbot: the tee has its back to us, or is busy with somebody else —
// that is the moment a human actually takes the shot instead of trading
// shots head on
static bool BotTargetDistracted(CCharacter *pT, vec2 MyPos, int Tick)
{
	if(!pT || !pT->IsAlive())
		return false;

	// facing: the aim input is a direction, so the sign of the dot product
	// tells us whether he is looking our way or turned away
	vec2 Aim = pT->GetAimVec();
	float Len = length(Aim);
	vec2 ToUs = MyPos - pT->m_Pos;
	float D = length(ToUs);
	if(Len > 1.0f && D > 60.0f)
	{
		if(dot(Aim, ToUs) / (Len * D) < -0.17f) // ~100 degrees and wider
			return true;
	}

	// busy: shooting at somebody, dragging a body or hooked onto one
	if(Tick - pT->GetLastAttackTick() < 20)
		return true;
	if(pT->GetHookedPlayerID() >= 0)
		return true;
	return false;
}

// fng_trainbot: a tee stands in the firing line. In FNG a shot stops on the
// first body it meets, so shooting through a frozen teammate is the classic
// "our own bot blocked the shot and we never noticed"
static bool BotTeeInLine(CGameContext *pGS, vec2 a, vec2 b, int SelfCID, CCharacter *pTarget)
{
	vec2 L = b - a;
	float Len = length(L);
	if(Len < 1.0f)
		return false;
	vec2 u = L * (1.0f / Len);
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(i == SelfCID)
			continue;
		CPlayer *p = pGS->m_apPlayers[i];
		if(!p)
			continue;
		CCharacter *pC = p->GetCharacter();
		if(!pC || !pC->IsAlive() || pC == pTarget)
			continue;
		// a frozen tee lying on the floor is not a wall: it is half the height,
		// shots go over a body on the ground, and a map full of frozen
		// leftovers would otherwise leave the bot unable to shoot at all
		if(pC->IsFrozen())
			continue;
		// and only somebody standing between us at our own height can eat the shot
		if(fabsf(pC->m_Pos.y - a.y) > 70.0f)
			continue;
		float Along = dot(pC->m_Pos - a, u);
		if(Along < 40.0f || Along > Len - 20.0f) // beside us or past him
			continue;
		if(distance(pC->m_Pos, a + u * Along) < 30.0f)
			return true;
	}
	return false;
}

// fng_trainbot: how many of our own team stand on that floor right now — two
// bots on one level are one bot's worth of pressure
static int BotTeammatesOnFloor(CGameContext *pGS, int SelfCID, int MyTeam, int Floor)
{
	int Num = 0;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(i == SelfCID)
			continue;
		CPlayer *p = pGS->m_apPlayers[i];
		if(!p || p->GetTeam() != MyTeam || p->GetTeam() < TEAM_RED || p->GetTeam() > TEAM_BLUE)
			continue;
		CCharacter *pC = p->GetCharacter();
		if(!pC || !pC->IsAlive())
			continue;
		if(pGS->BotFloorAt(pC->m_Pos) == Floor)
			Num++;
	}
	return Num;
}

// ... a level nobody of the team holds, so the team spreads over the map
// instead of stacking on one shelf
static int BotPickUncoveredFloor(CGameContext *pGS, int SelfCID, int MyTeam)
{
	if(pGS->m_NumBotFloors <= 0)
		return -1;
	int aFree[24];
	int nFree = 0;
	for(int f = 0; f < pGS->m_NumBotFloors && nFree < 24; f++)
		if(BotTeammatesOnFloor(pGS, SelfCID, MyTeam, f) == 0)
			aFree[nFree++] = f;
	if(nFree > 0)
		return aFree[(int)(frandom() * nFree) % nFree];
	return (int)(frandom() * pGS->m_NumBotFloors) % pGS->m_NumBotFloors;
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

	// fng_trainbot: is there actually ground one step ahead? A tee stands with
	// its feet ~28px below the centre, so the floor is probed at a few depths
	// instead of one exact tile that may fall into a gap between two floors.
	for(int p = 0; p < 3; p++)
		if(pCol->GetCollisionAt(x, MyPos.y + 24.0f + (float)p * 10.0f) & CCollision::COLFLAG_SOLID)
			return false; // ground continues under the next step

	// fng_trainbot: this used to contain a blanket "central shaft" rule that
	// returned true for any position near the middle of the map below y=1850,
	// regardless of whether there was a floor underfoot. Effect: every bot that
	// walked into the middle of AliveFNG froze on the spot forever (the log
	// showed "dir 0" with a target 500px away) — they could neither approach
	// a frozen victim nor hook it. The real question is only "what is down
	// there", so that is all this function checks now.

	// Deep vertical scan (up to 840 px / 26 tiles down)
	for(int dy = 48; dy <= 840; dy += 28)
	{
		float CheckY = MyPos.y + (float)dy;
		// Check both directly below and slightly drifting in Dir
		for(int dtx = 0; dtx <= 1; dtx++)
		{
			float CheckX = x + (float)(dtx * Dir) * 24.0f;
			int f = pCol->GetCollisionAt(CheckX, CheckY);
			if(f & BOT_DANGER_MASK)
				return true;
			// If we hit solid ground along the drop, verify there isn't a spike immediately on it
			if((f & CCollision::COLFLAG_SOLID) && dtx == 0)
			{
				for(int sx = -32; sx <= 32; sx += 32)
				{
					int fFloor = pCol->GetCollisionAt(CheckX + sx, CheckY);
					int fAbove = pCol->GetCollisionAt(CheckX + sx, CheckY - 32.0f);
					if((fFloor | fAbove) & BOT_DANGER_MASK)
						return true;
				}
				// Safe landing surface found
				return false;
			}
		}
	}
	return false;
}

// fng_trainbot: the first surface a step to the side would land on, and
// whether that landing is harmless. Dragging a body off a ledge onto the teeth
// below is a real FNG throw, and the deadly-drop guard would forbid exactly
// that step — so it needs to know that *we* land somewhere safe.
static bool BotDropIsSafe(CGameContext *pGS, vec2 MyPos, int Dir)
{
	CCollision *pCol = pGS->Collision();
	float x = MyPos.x + Dir * 48.0f;
	for(int dy = 48; dy <= 840; dy += 28)
	{
		int f = pCol->GetCollisionAt(x, MyPos.y + (float)dy);
		if(f & BOT_DANGER_MASK)
			return false; // the drop ends in the teeth
		if(f & CCollision::COLFLAG_SOLID)
			return true;  // harmless ground below
	}
	return false;
}

// hook anchor somewhere above us to pull the tee up to the higher platforms
static bool BotFindClimbAnchor(CGameContext *pGS, vec2 From, vec2 Want, vec2 *pOut)
{
	// 1. Direct angular rays targeting above/towards Want
	float AngleToWant = -pi * 0.5f; // default straight up
	vec2 Diff = Want - From;
	if(Diff.y < -30.0f)
		AngleToWant = atan2(Diff.y, Diff.x);

	// Test fan of angles around the direction of Goal, prioritizing upwards
	static const float aAngleOffsets[] = {0.0f, -0.2f, 0.2f, -0.4f, 0.4f, -0.7f, 0.7f, -1.0f, 1.0f};
	static const float aDists[] = {680.0f, 520.0f, 380.0f, 240.0f};

	for(int a = 0; a < 9; a++)
	{
		float Ang = AngleToWant + aAngleOffsets[a];
		// ensure angle is pointing mostly upwards (sin < -0.2)
		if(sin(Ang) > -0.2f)
			continue;
		vec2 DirVec = vec2(cos(Ang), sin(Ang));
		for(int d = 0; d < 4; d++)
		{
			vec2 To = From + DirVec * aDists[d];
			vec2 Hit = To;
			pGS->Collision()->IntersectLine(From, To, &Hit, 0);
			if(BotHookablePoint(pGS, Hit) && !(pGS->Collision()->GetCollisionAt(Hit.x, Hit.y) & BOT_DANGER_MASK))
			{
				if(length(Hit - From) > 70.0f && BotLineOfSight(pGS, From, Hit))
				{
					*pOut = Hit;
					return true;
				}
			}
		}
	}

	// 2. Fallback vertical scan
	static const float ady[] = {-360.0f, -520.0f, -240.0f, -660.0f};
	static const float adx[] = {0.0f, -70.0f, 70.0f, -140.0f, 140.0f, -35.0f, 35.0f};
	for(int j = 0; j < 4; j++)
	{
		for(int i = 0; i < 7; i++)
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

// fng_trainbot: an anchor for a swing. A swing is the DDNet way of crossing a
// map: hook something high and ahead, and while the rope yanks you into it,
// steer with the air control — the pull is strongest upward (gamecore damps the
// downward pull to 30%), so an anchor above and in front turns into speed
// instead of a lift. Running only ever gets you to the fight late.
static bool BotFindSwingAnchor(CGameContext *pGS, vec2 From, vec2 Want, vec2 *pOut)
{
	vec2 Diff = Want - From;
	if(Diff.x > -80.0f && Diff.x < 80.0f)
		return false; // straight up/down is a climb, not a swing
	int Dir = Diff.x > 0.0f ? 1 : -1;
	// ahead, and above — but not straight up, that is what BotFindClimbAnchor is
	static const float aAhead[] = {210.0f, 290.0f, 140.0f, 370.0f};
	static const float aUp[] = {-150.0f, -230.0f, -90.0f, -310.0f};
	for(int a = 0; a < 4; a++)
	{
		vec2 Cand = vec2(From.x + Dir * aAhead[a], From.y + aUp[a]);
		if(!BotHookablePoint(pGS, Cand))
			continue;
		if(pGS->Collision()->GetCollisionAt(Cand.x, Cand.y) & BOT_DANGER_MASK)
			continue;
		if(!BotLineOfSight(pGS, From, Cand))
			continue;
		*pOut = Cand;
		return true;
	}
	// and failing that, let a ray find the ceiling ahead
	vec2 To = From + vec2((float)Dir * 340.0f, -260.0f);
	vec2 Hit = To;
	pGS->Collision()->IntersectLine(From, To, &Hit, 0);
	if(BotHookablePoint(pGS, Hit) && !(pGS->Collision()->GetCollisionAt(Hit.x, Hit.y) & BOT_DANGER_MASK) &&
		Hit.y < From.y - 60.0f)
	{
		*pOut = Hit;
		return true;
	}
	return false;
}

void CBotAI::Reset()
{
	m_TargetCID = -1;
	m_MyFloor = -1;
	m_TargetFloor = -1;
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
	m_LastTargetVel = vec2(0.0f, 0.0f);
	m_Predict = 0.0f;
	m_StrafeDir = 1;
	m_StrafeTicks = 0;
	m_CarryTicks = 0;
	m_GrabCount = 0;
	m_ClimbTicks = 0;
	m_ClimbDir = 0;
	m_ClimbAnchorTick = 0;
	m_ClimbAnchor = vec2(0.0f, 0.0f);
	m_ClimbingUp = false;
	m_SpawnInitialized = false;
	m_BoostTicks = 0;
	m_BoostCooldown = 0;
	m_BoostDir = 1;
	m_BoostAnchor = vec2(0.0f, 0.0f);
	m_SwingTicks = 0;
	m_SwingCooldown = 0;
	m_SwingDir = 1;
	m_SwingAnchor = vec2(0.0f, 0.0f);
	m_SwingBestSpeed = 0;
	m_NavIdx = -1;
	m_NavGoal = vec2(0.0f, 0.0f);
	m_NavRetargetTick = 0;
	m_SelfCID = -1;
	m_Skill = 0.5f;
	m_AimMode = 2;
	m_AimModeTicks = 0;
	m_AimErr = 0.0f;
	m_AimErrSide = 1.0f;
	m_Lead = 0.0f;
	m_HoldFireTicks = 0;
	m_LastGrabTick = 0;
	m_FloorGoal = -1;
	m_FloorTicks = 0;
	m_HomeFloor = -1;
	m_ThrowIdx = -1;
	m_ThrowStand = vec2(0.0f, 0.0f);
	m_ThrowLearned = false;
	m_ThrowTick = 0;
	m_RegrabTarget = 0;
	m_RegrabUntil = 0;
	m_LastPreyCID = -1;
	m_HookEmit = 0;
	m_HammerIdx = -1;
	m_HammerVictim = -1;
	m_HammerTick = 0;
	m_HammerSwingTick = 0;
	m_HammerMeasureTick = 0;
	m_HammerMeasureCID = -1;
	m_HammerMeasureFrom = vec2(0.0f, 0.0f);
	m_HammerStand = vec2(0.0f, 0.0f);

	// fng_trainbot: a fresh mind — every habit is equally likely until the
	// rewards start saying otherwise
	for(int i = 0; i < NUM_BOTACTIONS; i++)
		m_aWeights[i] = 1.0f / (float)NUM_BOTACTIONS;
	for(int i = 0; i < NUM_NN_INPUTS; i++)
		for(int h = 0; h < NUM_NN_HIDDEN; h++)
			m_aNNInputHidden[i][h] = (frandom() - 0.5f) * 0.2f;
	for(int h = 0; h < NUM_NN_HIDDEN; h++)
	{
		m_aNNHiddenBias[h] = (frandom() - 0.5f) * 0.1f;
		for(int a = 0; a < NUM_BOTACTIONS; a++)
			m_aNNHiddenOutput[h][a] = (frandom() - 0.5f) * 0.4f;
	}
	for(int a = 0; a < NUM_BOTACTIONS; a++)
		m_aNNOutputBias[a] = 0.0f;
	m_PendingReward = 0.0f;
	m_LastState = 0;
	m_LastAction = BOTACT_HOLD;
	m_HasTransition = false;
	m_Action = BOTACT_HUNT;
	m_ActionTick = 0;
	m_ActionRewardTick = 0;
	m_BrainLoaded = false;
}

int CBotAI::Observation(CGameContext *pGS, int ClientID) const
{
	if(!pGS || ClientID < 0 || ClientID >= MAX_CLIENTS || !pGS->m_apPlayers[ClientID])
		return 0;
	CPlayer *pSelf = pGS->m_apPlayers[ClientID];
	CCharacter *pMe = pSelf->GetCharacter();
	if(!pMe || !pMe->IsAlive())
		return 0;

	bool Enemy = false;
	bool FrozenEnemy = false;
	bool FrozenMate = false;
	float Nearest = 1e9f;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(i == ClientID || !pGS->m_apPlayers[i])
			continue;
		CCharacter *pC = pGS->m_apPlayers[i]->GetCharacter();
		if(!pC || !pC->IsAlive())
			continue;
		if(pGS->m_apPlayers[i]->GetTeam() == pSelf->GetTeam())
		{
			if(pC->IsFrozen())
				FrozenMate = true;
		}
		else
		{
			Enemy = true;
			FrozenEnemy = FrozenEnemy || pC->IsFrozen();
			Nearest = min(Nearest, distance(pMe->m_Pos, pC->m_Pos));
		}
	}
	int DistanceBand = Nearest < 400.0f ? 0 : (Nearest < 900.0f ? 1 : 2);
	int State = DistanceBand * 2 + (Enemy ? 1 : 0);
	State = State * 2 + (FrozenMate ? 1 : 0);
	State = State * 2 + (FrozenEnemy ? 1 : 0);
	State = State * 2 + (pMe->IsHookGrabbed() && pMe->GetHookedPlayerID() >= 0 ? 1 : 0);
	State = State * 2 + (pMe->IsGrounded() ? 1 : 0);
	return clamp(State, 0, NUM_QSTATES - 1);
}

void CBotAI::EncodeNNInput(int State, float *pInput) const
{
	for(int i = 0; i < NUM_NN_INPUTS; i++)
		pInput[i] = 0.0f;
	if(State < 0 || State >= NUM_QSTATES)
		return;
	int DistanceBand = State >> 5;
	pInput[DistanceBand] = 1.0f;
	pInput[3] = (State >> 4) & 1;
	pInput[4] = (State >> 3) & 1;
	pInput[5] = (State >> 2) & 1;
	pInput[6] = (State >> 1) & 1;
	pInput[7] = State & 1;
}

void CBotAI::ForwardNN(int State, float *pHidden, float *pOutput) const
{
	float aInput[NUM_NN_INPUTS];
	EncodeNNInput(State, aInput);
	for(int h = 0; h < NUM_NN_HIDDEN; h++)
	{
		float Sum = m_aNNHiddenBias[h];
		for(int i = 0; i < NUM_NN_INPUTS; i++)
			Sum += aInput[i] * m_aNNInputHidden[i][h];
		pHidden[h] = tanhf(Sum);
	}
	for(int a = 0; a < NUM_BOTACTIONS; a++)
	{
		float Sum = m_aNNOutputBias[a];
		for(int h = 0; h < NUM_NN_HIDDEN; h++)
			Sum += pHidden[h] * m_aNNHiddenOutput[h][a];
		pOutput[a] = clamp(Sum, -100.0f, 100.0f);
	}
}

float CBotAI::NNWeight(int Index) const
{
	if(Index < 0 || Index >= NUM_NN_WEIGHTS)
		return 0.0f;
	if(Index < NUM_NN_INPUTS * NUM_NN_HIDDEN)
		return m_aNNInputHidden[Index / NUM_NN_HIDDEN][Index % NUM_NN_HIDDEN];
	Index -= NUM_NN_INPUTS * NUM_NN_HIDDEN;
	if(Index < NUM_NN_HIDDEN)
		return m_aNNHiddenBias[Index];
	Index -= NUM_NN_HIDDEN;
	if(Index < NUM_NN_HIDDEN * NUM_BOTACTIONS)
		return m_aNNHiddenOutput[Index / NUM_BOTACTIONS][Index % NUM_BOTACTIONS];
	Index -= NUM_NN_HIDDEN * NUM_BOTACTIONS;
	return m_aNNOutputBias[Index];
}

void CBotAI::SetNNWeight(int Index, float Value)
{
	if(Index < 0 || Index >= NUM_NN_WEIGHTS)
		return;
	Value = clamp(Value, -10.0f, 10.0f);
	if(Index < NUM_NN_INPUTS * NUM_NN_HIDDEN)
		m_aNNInputHidden[Index / NUM_NN_HIDDEN][Index % NUM_NN_HIDDEN] = Value;
	else
	{
		Index -= NUM_NN_INPUTS * NUM_NN_HIDDEN;
		if(Index < NUM_NN_HIDDEN)
			m_aNNHiddenBias[Index] = Value;
		else
		{
			Index -= NUM_NN_HIDDEN;
			if(Index < NUM_NN_HIDDEN * NUM_BOTACTIONS)
				m_aNNHiddenOutput[Index / NUM_BOTACTIONS][Index % NUM_BOTACTIONS] = Value;
			else
			{
				Index -= NUM_NN_HIDDEN * NUM_BOTACTIONS;
				m_aNNOutputBias[Index] = Value;
			}
		}
	}
	m_BrainLoaded = true;
}

void CBotAI::UpdateQ(int State, int Action, float Reward, int NextState, bool Terminal, float Alpha)
{
	if(State < 0 || State >= NUM_QSTATES || Action < 0 || Action >= NUM_BOTACTIONS)
		return;
	float aHidden[NUM_NN_HIDDEN], aOutput[NUM_BOTACTIONS];
	ForwardNN(State, aHidden, aOutput);
	float Next = 0.0f;
	if(!Terminal && NextState >= 0 && NextState < NUM_QSTATES)
	{
		float aNextHidden[NUM_NN_HIDDEN], aNextOutput[NUM_BOTACTIONS];
		ForwardNN(NextState, aNextHidden, aNextOutput);
		bool HaveNext = false;
		for(int a = 0; a < NUM_BOTACTIONS; a++)
		{
			if((a == BOTACT_HUNT || a == BOTACT_THROW) && !(NextState & 16))
				continue;
			if(a == BOTACT_RESCUE && !(NextState & 8))
				continue;
			if(!HaveNext || aNextOutput[a] > Next)
			{
				Next = aNextOutput[a];
				HaveNext = true;
			}
		}
	}
	const float Gamma = 0.92f;
	const float Rate = clamp(Alpha, 0.001f, 1.0f);
	const float Target = clamp(Reward + (Terminal ? 0.0f : Gamma * Next), -100.0f, 100.0f);
	const float Error = clamp(Target - aOutput[Action], -10.0f, 10.0f);
	float aInput[NUM_NN_INPUTS];
	EncodeNNInput(State, aInput);
	for(int h = 0; h < NUM_NN_HIDDEN; h++)
	{
		float HiddenError = Error * m_aNNHiddenOutput[h][Action] * (1.0f - aHidden[h] * aHidden[h]);
		m_aNNHiddenOutput[h][Action] = clamp(m_aNNHiddenOutput[h][Action] + Rate * Error * aHidden[h], -10.0f, 10.0f);
		for(int i = 0; i < NUM_NN_INPUTS; i++)
			m_aNNInputHidden[i][h] = clamp(m_aNNInputHidden[i][h] + Rate * HiddenError * aInput[i], -10.0f, 10.0f);
		m_aNNHiddenBias[h] = clamp(m_aNNHiddenBias[h] + Rate * HiddenError, -10.0f, 10.0f);
	}
	m_aNNOutputBias[Action] = clamp(m_aNNOutputBias[Action] + Rate * Error, -10.0f, 10.0f);
}

void CBotAI::EndEpisode(float Reward, float Alpha)
{
	m_PendingReward += Reward;
	if(g_Config.m_SvBotLearn && m_HasTransition)
		UpdateQ(m_LastState, m_LastAction, m_PendingReward, 0, true, Alpha);
	m_PendingReward = 0.0f;
	m_HasTransition = false;
}

void CBotAI::SetWeights(const float *pWeights)
{
	if(!pWeights)
		return;
	float Sum = 0.0f;
	for(int i = 0; i < NUM_BOTACTIONS; i++)
	{
		m_aWeights[i] = clamp(pWeights[i], 0.01f, 2.0f);
		Sum += m_aWeights[i];
	}
	if(Sum <= 0.0f)
		return;
	for(int i = 0; i < NUM_BOTACTIONS; i++)
		m_aWeights[i] /= Sum; // weights are compared against each other only
	m_BrainLoaded = true;
}

const char *CBotAI::ActionName(int Action)
{
	switch(Action)
	{
	case BOTACT_HUNT: return "hunt";
	case BOTACT_RESCUE: return "rescue";
	case BOTACT_THROW: return "throw";
	case BOTACT_HOLD: return "hold";
	default: return "roam";
	}
}

// fng_trainbot: the reward. Whatever habit the bot was following when the
// result arrived gets the credit or the blame. The pull-back towards the
// average used to be 12% per event, which was a spring so stiff that no
// amount of play could ever separate the habits: after hours the table sat at
// 0.19..0.21 everywhere, i.e. the bot had learned nothing. It is now a gentle
// 1.5% nudge that only stops one habit from eating the whole table, and the
// weights are renormalised so every bot carries the same total probability.
void CBotAI::RewardAction(CGameContext *pGS, int Action, float Amount)
{
	if(!g_Config.m_SvBotLearn || !pGS || Action < 0 || Action >= NUM_BOTACTIONS)
		return;

	float Rate = g_Config.m_SvBotLearnRate / 100.0f;
	if(m_HasTransition)
		m_PendingReward = clamp(m_PendingReward + Amount, -100.0f, 100.0f);
	float Delta = Amount * Rate * 0.06f;
	float Before = m_aWeights[Action];

	m_aWeights[Action] = clamp(m_aWeights[Action] + Delta, 0.02f, 0.95f);

	// weak mean reversion: keeps the table from collapsing into one habit
	for(int i = 0; i < NUM_BOTACTIONS; i++)
		if(i != Action)
			m_aWeights[i] = clamp(mix(m_aWeights[i], 0.2f, 0.015f), 0.02f, 0.95f);

	// renormalise: probabilities are compared against each other, and a bot
	// whose table happens to sum to 1.14 would otherwise play faster than one
	// summing to 0.91 for no reason at all
	float Sum = 0.0f;
	for(int i = 0; i < NUM_BOTACTIONS; i++)
		Sum += m_aWeights[i];
	if(Sum > 0.0f)
		for(int i = 0; i < NUM_BOTACTIONS; i++)
			m_aWeights[i] = clamp(m_aWeights[i] / Sum, 0.01f, 0.90f);

	if(g_Config.m_SvBotDebug && fabsf(m_aWeights[Action] - Before) > 0.001f)
	{
		char aBuf[224];
		str_format(aBuf, sizeof(aBuf),
			"bot learns: %s %+.1f -> %.3f (hunt %.2f rescue %.2f throw %.2f hold %.2f roam %.2f)",
			ActionName(Action), Amount, m_aWeights[Action],
			m_aWeights[BOTACT_HUNT], m_aWeights[BOTACT_RESCUE], m_aWeights[BOTACT_THROW],
			m_aWeights[BOTACT_HOLD], m_aWeights[BOTACT_ROAM]);
		pGS->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
	}
}

// Epsilon-greedy policy over neural-network action values. Rescue and combat
// tactics are masked when their corresponding targets do not exist.
int CBotAI::ChooseAction(CGameContext *pGS, int ClientID)
{
	bool aPossible[NUM_BOTACTIONS];
	CPlayer *pSelf = pGS->m_apPlayers[ClientID];
	if(!pSelf)
		return BOTACT_HOLD;

	bool FrozenTeammate = false;
	bool Enemy = false;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(i == ClientID)
			continue;
		CPlayer *p = pGS->m_apPlayers[i];
		if(!p)
			continue;
		CCharacter *pC = p->GetCharacter();
		if(!pC || !pC->IsAlive())
			continue;
		if(p->GetTeam() == pSelf->GetTeam())
		{
			if(pC->IsFrozen())
				FrozenTeammate = true;
		}
		else
			Enemy = true;
	}

	aPossible[BOTACT_HUNT] = Enemy;
	aPossible[BOTACT_RESCUE] = FrozenTeammate;
	aPossible[BOTACT_THROW] = Enemy;   // needs a victim to grab
	aPossible[BOTACT_HOLD] = true;
	aPossible[BOTACT_ROAM] = true;
	float Total = 0.0f;
	float aW[NUM_BOTACTIONS];
	for(int i = 0; i < NUM_BOTACTIONS; i++)
	{
		aW[i] = aPossible[i] ? m_aWeights[i] : 0.0f;
		Total += aW[i];
	}
	if(Total <= 0.0f)
		return BOTACT_HOLD; // nothing to do: stand where you are

	const int State = Observation(pGS, ClientID);
	const float Epsilon = g_Config.m_SvBotLearn ? g_Config.m_SvBotExplore / 100.0f : 0.0f;
	if(frandom() < Epsilon)
	{
		int aViable[NUM_BOTACTIONS];
		int nViable = 0;
		for(int i = 0; i < NUM_BOTACTIONS; i++)
			if(aW[i] > 0.0f)
				aViable[nViable++] = i;
		return aViable[nViable > 0 ? (int)(frandom() * nViable) % nViable : 0];
	}

	float aHidden[NUM_NN_HIDDEN], aQ[NUM_BOTACTIONS];
	ForwardNN(State, aHidden, aQ);
	float Best = -1e30f;
	int aBest[NUM_BOTACTIONS];
	int nBest = 0;
	for(int i = 0; i < NUM_BOTACTIONS; i++)
	{
		if(aW[i] <= 0.0f)
			continue;
		// Keep older saved tactic preferences useful alongside neural values.
		float Q = aQ[i] + m_aWeights[i];
		if(Q > Best + 0.0001f)
		{
			Best = Q;
			nBest = 0;
			aBest[nBest++] = i;
		}
		else if(fabsf(Q - Best) <= 0.0001f)
			aBest[nBest++] = i;
	}
	return nBest > 0 ? aBest[(int)(frandom() * nBest) % nBest] : BOTACT_HOLD;
}


void CBotAI::PickNavPoint(CGameContext *pGS, vec2 MyPos, int MyTeam, int Tick, int FloorPref)
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
	// fng_trainbot: every bot gets its own flank. Without this all four walk
	// to the same spot of the same shelf and stand shoulder to shoulder —
	// which is not only silly, it also blocks each other's shots.
	int Flank = (m_SelfCID % 2 == 0) ? -1 : 1;          // left / right approach
	int HighGround = (m_SelfCID / 2) % 3;                 // 0 mid, 1 above, 2 below
	for(int t = 0; t < 16; t++)
	{
		int i = (int)(frandom() * pGS->m_NumBotNav);
		if(i < 0 || i >= pGS->m_NumBotNav)
			continue;
		const CGameContext::CBotNavPoint &P = pGS->m_aBotNav[i];
		float d = distance(MyPos, P.m_Pos);
		if(d > 2200.0f)
			continue;
		float Score = d * 0.5f + frandom() * 700.0f;
		if(d < 200.0f)
			Score += 2000.0f;           // don't re-pick what we already reached
		if(FloorPref >= 0)
			Score += (P.m_Floor == FloorPref ? -3000.0f : 900.0f); // work one floor
		if(P.m_Side == MySide && MySide != 0)
			Score -= 400.0f;            // hold your own half
		else if(P.m_Side != 0 && MySide != 0 && P.m_Side != MySide)
			Score += 300.0f;            // raids into the enemy half are rare
		if(P.m_Band == 1)
			Score -= 250.0f;            // mid band is where the fight happens
		else if(P.m_Band == 2 && frandom() < 0.3f)
			Score -= 200.0f;            // sometimes take the high ground

		// come at it from our own side, from the height our slot is assigned
		if(P.m_Side == Flank)
			Score -= 500.0f;
		if(HighGround == 1 && P.m_Band == 2)
			Score -= 450.0f;
		else if(HighGround == 2 && P.m_Band == 0)
			Score -= 450.0f;
		else if(HighGround == 0 && P.m_Band == 1)
			Score -= 300.0f;

		// and do not stand where a teammate already stands
		for(int t2 = 0; t2 < MAX_CLIENTS; t2++)
		{
			if(t2 == m_SelfCID)
				continue;
			CPlayer *pT = pGS->m_apPlayers[t2];
			if(!pT || pT->GetTeam() != MyTeam)
				continue;
			CCharacter *pTC = pT->GetCharacter();
			if(!pTC || !pTC->IsAlive())
				continue;
			float dt = distance(pTC->m_Pos, P.m_Pos);
			if(dt < 260.0f)
				Score += (260.0f - dt) * 4.0f; // spread out instead of stacking
		}

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

// fng_trainbot: a frozen body somebody of ours is already working on. The
// pile-up in the screenshot was four bots standing on one frozen tee: he is by
// far the best-scoring target (-900 for the throw habit), so everybody picked
// him. A mate who already hooks him, drags him or stands in hammer range has
// claimed him, and the rest of the team must find another job — the way a real
// team does, where one guy takes the throw and the others keep shooting.
static bool BotBodyTaken(CGameContext *pGS, int SelfCID, int MyTeam, CCharacter *pBody, float MyDist)
{
	// somebody's rope is already on him
	if(pBody->IsHookGrabbed())
		return true;
	CPlayer *pBodyP = pBody->GetPlayer();
	int BodyCID = pBodyP ? pBodyP->GetCID() : -1;
	if(BodyCID < 0)
		return false;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(i == SelfCID)
			continue;
		CPlayer *p = pGS->m_apPlayers[i];
		if(!p || p->GetTeam() != MyTeam || p->GetTeam() < TEAM_RED || p->GetTeam() > TEAM_BLUE)
			continue;
		CCharacter *pC = p->GetCharacter();
		if(!pC || !pC->IsAlive())
			continue;
		// he is hooked to him, or standing on top of him
		if(pC->GetHookedPlayerID() == BodyCID)
			return true;
		float dt = distance(pC->m_Pos, pBody->m_Pos);
		if(dt < 140.0f && dt < MyDist)
			return true;
	}
	return false;
}

// fng_trainbot: has a mate already decided to go for this slot? The proximity
// test below only fires once somebody is actually standing on the body, so
// four bots could all set off for the same frozen tee and only then discover
// he is taken. Reading the other bots' own decisions reserves him the moment
// the first one commits, which is how a squad picks its targets.
static bool BotTargetClaimed(CGameContext *pGS, int SelfCID, int MyTeam, int CID)
{
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(i == SelfCID)
			continue;
		CPlayer *p = pGS->m_apPlayers[i];
		if(!p || p->GetTeam() != MyTeam || p->GetTeam() < TEAM_RED || p->GetTeam() > TEAM_BLUE)
			continue;
		if(pGS->m_aBotAI[i].GetTargetCID() == CID)
			return true;
	}
	return false;
}

// fng_trainbot: how many of our team are already heading for that shelf. The
// old floor-spread rule only counted bots that had physically *arrived*, so
// four of them could decide to walk to the same level at the same moment and
// then arrive shoulder to shoulder.
static int BotTeammatesHeadingFor(CGameContext *pGS, int SelfCID, int MyTeam, int Floor)
{
	if(Floor < 0)
		return 0;
	int Num = 0;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(i == SelfCID)
			continue;
		CPlayer *p = pGS->m_apPlayers[i];
		if(!p || p->GetTeam() != MyTeam || p->GetTeam() < TEAM_RED || p->GetTeam() > TEAM_BLUE)
			continue;
		CCharacter *pC = p->GetCharacter();
		if(!pC || !pC->IsAlive())
			continue;
		// already there, or planning to go there
		if(pGS->BotFloorAt(pC->m_Pos) == Floor ||
			pGS->m_aBotAI[i].GetFloorGoal() == Floor)
			Num++;
	}
	return Num;
}

// fng_trainbot: where a hammer hit would send a body. The body at B is thrown
// away from the hitter at A, so a stand point keeps the body *behind* itself
// (between stand and cluster) only when it lies past B on the same line.
static float BotKnockLine(vec2 P, vec2 A, vec2 B, bool *pBeyond)
{
	vec2 L = B - A;
	float Len = length(L);
	if(Len < 1.0f)
	{
		if(pBeyond)
			*pBeyond = false;
		return distance(P, A);
	}
	vec2 u = L * (1.0f / Len);
	float Along = dot(P - A, u);
	if(pBeyond)
		*pBeyond = Along > Len; // past the body, i.e. on the far side
	return length((P - A) - u * Along);
}

// fng_trainbot: personality. sv_bot_skill sets the table level; -1 lets every
// bot roll its own so a server with several bots feels like a lobby of people.
void CBotAI::RollProfile()
{
	if(g_Config.m_SvBotSkill < 0)
		m_Skill = clamp(0.25f + frandom() * 0.65f, 0.05f, 0.98f);
	else
		m_Skill = clamp(g_Config.m_SvBotSkill / 100.0f + (frandom() * 0.24f - 0.12f), 0.03f, 1.0f);
	RollAimForm();
}

// fng_trainbot: current form. Real players do not shoot with one constant
// accuracy: they go through series of clean shots and series of garbage, and
// the weaker they are, the longer the garbage lasts.
void CBotAI::RollAimForm()
{
	float Skill = clamp(m_Skill, 0.0f, 1.0f);
	float r = frandom();
	float DeadOn = Skill * 0.34f;
	float Good = DeadOn + Skill * 0.42f;
	float Average = Good + 0.30f;
	if(r < DeadOn)
		m_AimMode = 0;
	else if(r < Good)
		m_AimMode = 1;
	else if(r < Average)
		m_AimMode = 2;
	else
		m_AimMode = 3;

	// angular error: 3 degrees is already a clean miss on a moving tee at
	// laser range, 20 degrees flies over the whole platform
	static const float aMaxErr[4] = {0.0f, 0.055f, 0.14f, 0.36f};
	m_AimErr = aMaxErr[m_AimMode] * (0.35f + 0.65f * frandom());
	m_AimErrSide = frandom() < 0.5f ? -1.0f : 1.0f;

	// lead in ticks: the good ones read the movement and shoot ahead, the bad
	// ones shoot where the tee was a moment ago
	if(m_AimMode == 0)
		m_Lead = 3.0f + frandom() * 2.0f;
	else if(m_AimMode == 1)
		m_Lead = 1.2f + frandom() * 2.4f;
	else if(m_AimMode == 2)
		m_Lead = -1.5f + frandom() * 4.5f;
	else
		m_Lead = -6.0f + frandom() * 3.5f;

	m_AimModeTicks = 20 + (int)(frandom() * 50.0f);
	// a pause before the shot: reading the movement, or simply a bad reaction
	m_HoldFireTicks = frandom() < 0.10f * (1.0f - Skill) ? 6 + (int)(frandom() * 22.0f) : 0;
}

// fng_trainbot: rotate the ideal aim by the error of the current shot. Hard
// dodging makes every human sloppier, so the error scales with m_Predict.
vec2 CBotAI::AddAimError(vec2 Aim, float Scale)
{
	float a = m_AimErr * m_AimErrSide * Scale * (1.0f + m_Predict * 0.8f) +
		(frandom() * 2.0f - 1.0f) * 0.012f;
	float c = cosf(a), s = sinf(a);
	return vec2(Aim.x * c - Aim.y * s, Aim.x * s + Aim.y * c);
}

// fng_trainbot: the throw. A tee on the hook is dragged towards whoever holds
// the hook, so the trick is standing where a spike cluster sits on the line
// prey -> bot. That is the whole geometry of a FNG throw.
// fng_trainbot: the throw, rebuilt from 69 throws a person made on this map.
//
// The five that scored have nothing in common with the plan this bot used to
// make. Every one of them pulled the body *upward*, 215..368px, towards a
// cluster that stood 159..406px ABOVE where the body lay and less than 130px to
// the side. The thrower did not walk away at all — his x coordinate did not
// change by a single pixel in any of the five; he hooked the body at his feet,
// went up the wall, and the rope walked the body up onto the ledge.
//
// The old plan stood far away with the cluster on the line prey->bot and
// dragged sideways. On this map that is simply the wrong throw, which is why
// the bot hooked bodies for minutes at a time and never scored: the drag went
// along the shelf, parallel to the teeth, and the body ended up 170..545px
// short. The misses in the recorded data average 93px above the body against
// 247px for the hits, and the lift they achieved was 64px against 283px.
void CBotAI::PlanThrow(CGameContext *pGS, vec2 MyPos, vec2 PreyPos, int MyTeam, int Tick)
{
	m_ThrowIdx = -1;
	// re-planning costs, and a plan that is kept for a moment walks a straight
	// line — humans do not re-decide every tick either
	m_ThrowTick = Tick + 10 + (int)(frandom() * 8.0f);
	if(!g_Config.m_SvBotThrow || pGS->m_NumBotThrowTargets <= 0)
		return;

	// the teeth have to be overhead: above the body, close to straight up, and
	// within the height a 1.25s drag can actually lift
	int BestT = -1;
	float Best = 0.0f;
	for(int t = 0; t < pGS->m_NumBotThrowTargets; t++)
	{
		const CGameContext::CBotThrowTarget &T = pGS->m_aBotThrowTargets[t];
		if(!BotValidSpikeForTeam(T.m_Flags, MyTeam))
			continue;
		float dx = T.m_Pos.x - PreyPos.x;
		float dy = T.m_Pos.y - PreyPos.y; // y grows downwards, so this is negative
		if(dy > -110.0f)                  // not high enough to drop onto
			continue;
		if(dy < -470.0f)                  // further than the rope can lift him
			continue;
		if(fabsf(dx) > 200.0f)            // the recorded throws were almost vertical
			continue;
		// straight up and near wins; the rest only if nothing better exists
		float Score = -dy + fabsf(dx) * 0.7f + distance(MyPos, T.m_Pos) * 0.25f +
			BotSpikePenalty(T.m_Flags) * 0.5f + frandom() * 60.0f;
		// fng_trainbot: the book overrides opinion. If a player has already made
		// this throw from somewhere near here and it landed, that is worth more
		// than any rule of ours; and a cluster that has been tried enough times
		// and never once worked is a cluster to leave alone.
		float Rate = pGS->BotClusterHitRate(T.m_Pos);
		if(Rate >= 0.0f && Rate < 0.2f)
			Score += 900.0f;
		else if(Rate > 0.0f)
			Score -= 400.0f;
		if(BestT < 0 || Score < Best)
		{
			Best = Score;
			BestT = t;
		}
	}
	if(BestT < 0)
		return;
	m_ThrowIdx = BestT;

	// fng_trainbot: stand at the body, not at the far end of a drag line. Every
	// recorded throw started from next to the victim, and the rope only bites at
	// about 30px, so a stand point further away cannot even hook him. The feet
	// line up under the teeth and then climb: that is the whole throw.
	{
		const CGameContext::CBotThrowTarget &BT = pGS->m_aBotThrowTargets[BestT];
		// ... but if somebody has already made this very throw, copy where they
		// stood. That is the whole point of the book: a person's spot replaces
		// our guess, and a miss we both tried from is not repeated.
		vec2 Learned;
		bool HaveLearned = pGS->BotRecipeStand(BT.m_Pos, PreyPos, &Learned);
		if(HaveLearned)
			m_ThrowStand = Learned;
		else
		{
			// fng_trainbot: the hook pulls the body towards whoever holds the rope,
			// with a force that grows with the distance (gamecore), and the
			// downward half of that pull is damped to 30% (HookVel.y *= 0.3).
			// So a body can only be *lifted* from above: standing next to it on
			// the same floor drags it sideways along the ground, which is exactly
			// the throw that never worked. The recorded throws all ended with
			// the thrower up at the teeth and the body 215..368px higher, x never
			// changing. So the stance belongs over the body, near the cluster —
			// still well inside the 380px rope.
			m_ThrowStand = vec2(
				PreyPos.x + clamp(BT.m_Pos.x - PreyPos.x, -110.0f, 110.0f),
				min(PreyPos.y, BT.m_Pos.y + 40.0f));
		}
		// never plan a stance inside the teeth
		if(pGS->Collision()->GetCollisionAt(m_ThrowStand.x, m_ThrowStand.y) & BOT_DANGER_MASK)
			m_ThrowStand = vec2(PreyPos.x, PreyPos.y + 22.0f);
		// and never out of the rope's reach
		if(distance(PreyPos, m_ThrowStand) > 330.0f)
			m_ThrowStand = vec2(PreyPos.x, PreyPos.y + 22.0f);
		m_ThrowLearned = HaveLearned;
	}

	if(g_Config.m_SvBotDebug)
	{
		const CGameContext::CBotThrowTarget &T = pGS->m_aBotThrowTargets[BestT];
		char aBuf[192];
		str_format(aBuf, sizeof(aBuf),
			"bot throw: cluster %d at %.0f,%.0f r%.0f is %.0fpx ABOVE the body -> stand at %.0f,%.0f%s",
			BestT, T.m_Pos.x, T.m_Pos.y, T.m_Radius, PreyPos.y - T.m_Pos.y,
			m_ThrowStand.x, m_ThrowStand.y, m_ThrowLearned ? " [learned]" : "");
		pGS->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
	}
}

// fng_trainbot: the hammer throw. Where the rope drag pulls the body *through*
// the spikes towards the bot, a hammer hit sends the body *away* from the bot,
// so this plan wants the opposite geometry: a spot on the far side of the body
// from the cluster, so that one swing launches him straight into the teeth.
// The hammer reaches only ~35px and launches a body a few hundred pixels, so
// the cluster has to be close and the line has to be reasonably clean.
void CBotAI::PlanHammer(CGameContext *pGS, vec2 MyPos, vec2 PreyPos, int MyTeam, int Tick, int VictimCID)
{
	m_HammerIdx = -1;
	m_HammerTick = Tick + 12 + (int)(frandom() * 10.0f);
	if(VictimCID >= 0)
		m_HammerVictim = VictimCID; // remember which body this is for
	if(!g_Config.m_SvBotThrow || pGS->m_NumBotThrowTargets <= 0 || pGS->m_NumBotNav <= 0)
		return;

	float Best = 0.0f;
	int BestT = -1;
	vec2 BestStand = vec2(0.0f, 0.0f);
	for(int t = 0; t < pGS->m_NumBotThrowTargets; t++)
	{
		const CGameContext::CBotThrowTarget &T = pGS->m_aBotThrowTargets[t];
		if(!BotValidSpikeForTeam(T.m_Flags, MyTeam))
			continue;
		float dPrey = distance(PreyPos, T.m_Pos);
		// a swing throws a body a few hundred pixels through the air; the
		// measurement showed a sideways hit moves it rather less, so do not
		// plan on clusters across the whole shelf
		if(dPrey > 400.0f)
			continue;
		// the push is horizontal with a slight lift, so a cluster straight
		// above or below the body cannot be aimed at, and one far under the
		// shelf is reached only by luck
		float dxPrey = T.m_Pos.x - PreyPos.x;
		float dyPrey = T.m_Pos.y - PreyPos.y;
		if(fabsf(dxPrey) < 24.0f)
			continue;
		if(dyPrey > 300.0f || dyPrey < -240.0f)
			continue;

		// a stand point on the far side, close to the line cluster -> body
		for(int n = 0; n < pGS->m_NumBotNav; n += 2)
		{
			vec2 S = pGS->m_aBotNav[n].m_Pos;
			float dStand = distance(MyPos, S);
			if(dStand > 620.0f)
				continue;
			float dBody = distance(S, PreyPos);
			// close enough that walking in from there still lands the swing,
			// far enough that we do not stand on top of the body
			if(dBody < 18.0f || dBody > 170.0f)
				continue;
			// the stand has to be on the *opposite* side of the body, so the
			// swing sends him towards the cluster and not away from it
			if((S.x - PreyPos.x) * dxPrey > 0.0f)
				continue;
			if(fabsf(S.x - PreyPos.x) < 14.0f)
				continue;
			// and roughly on the cluster -> body line, so the fling does not
			// shoot him off the side of the cluster
			float Miss = BotKnockLine(S, T.m_Pos, PreyPos, 0);
			if(Miss > 130.0f)
				continue;
			// and standing there must not be standing in the spikes already
			if(pGS->Collision()->GetCollisionAt(S.x, S.y) & BOT_DANGER_MASK)
				continue;
			if(pGS->Collision()->GetCollisionAt(S.x, S.y - 24.0f) & BOT_DANGER_MASK)
				continue;

			float Score = Miss * 0.8f + dStand * 0.6f + dPrey * 0.6f +
				fabsf(S.y - PreyPos.y) * 0.5f +
				BotSpikePenalty(T.m_Flags) * 0.5f + frandom() * 60.0f;
			if(t == m_HammerIdx)
				Score -= 140.0f; // stick with a plan that is already working
			if(BestT < 0 || Score < Best)
			{
				Best = Score;
				BestT = t;
				BestStand = S;
			}
		}
	}
	if(BestT < 0)
		return;
	bool Changed = BestT != m_HammerIdx;
	m_HammerIdx = BestT;
	m_HammerStand = BestStand;
	if(g_Config.m_SvBotDebug && Changed)
	{
		const CGameContext::CBotThrowTarget &T = pGS->m_aBotThrowTargets[BestT];
		char aBuf[192];
		str_format(aBuf, sizeof(aBuf),
			"bot hammer: cluster %d at %.0f,%.0f -> stand at %.0f,%.0f",
			BestT, T.m_Pos.x, T.m_Pos.y, BestStand.x, BestStand.y);
		pGS->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
	}
}

void CBotAI::Tick(CGameContext *pGS, int ClientID)

{
	CPlayer *pSelf = pGS->m_apPlayers[ClientID];
	if(!pSelf)
		return;

	// our slot decides which flank and which height we prefer, so the team
	// surrounds instead of queueing up behind one another
	m_SelfCID = ClientID;

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
		// A death without a player-kill callback still ends this learning episode.
		if(m_HasTransition)
			EndEpisode(-1.2f, g_Config.m_SvBotLearnRate / 100.0f);
		// re-arm the spawn init for the character that will replace this one
		m_SpawnInitialized = false;
		m_FireState = (m_FireState + 1) & INPUT_STATE_MASK;
		Input.m_Fire = m_FireState;
		pGS->OnClientDirectInput(ClientID, &Input);
		pGS->OnClientPredictedInput(ClientID, &Input);
		// fng_trainbot: a bot that never comes alive shows nothing else to look
		// at — this is the first line to check when it seems to do nothing
		if(g_Config.m_SvBotDebug && pGS->Server()->Tick() % 200 == 0)
		{
			char aBuf[192];
			str_format(aBuf, sizeof(aBuf),
				"bot %d: no character, t=%d die=%d respawn=%d team=%d",
				ClientID, pGS->Server()->Tick(), pSelf->m_DieTick,
				pSelf->m_RespawnTick, pSelf->GetTeam());
			pGS->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
		}
		return;
	}

	const int Tick = pGS->Server()->Tick();
	const vec2 MyPos = pMe->m_Pos;
	const int MyTeam = pSelf->GetTeam();

	// fng_trainbot: fresh character (first tick after spawn/respawn) — arm
	// the timers from this tick, drop stale climb/boost state and pick the
	// first patrol point so the bot starts moving right away
	if(!m_SpawnInitialized)
	{
		m_SpawnInitialized = true;
		m_StuckTick = Tick;
		m_LastPos = MyPos;
		m_ClimbTicks = 0;
		m_ClimbingUp = false;
		m_BoostTicks = 0;
		m_BackoffTicks = 0;
		m_SwingTicks = 0;
		m_SwingCooldown = 0;
		m_JumpTicks = 0;
		m_JumpCooldown = 0;
		m_ThrowIdx = -1;
		m_HammerIdx = -1;
		m_HammerVictim = -1;
		m_HammerTick = 0;
		m_HammerSwingTick = 0;
		m_HammerMeasureTick = 0;
		m_HammerMeasureCID = -1;
		m_HammerMeasureFrom = vec2(0.0f, 0.0f);
		m_HammerStand = vec2(0.0f, 0.0f);
		m_RegrabTarget = 0;
		m_RegrabUntil = 0;
		m_LastPreyCID = -1;
		m_HookEmit = 0;
		m_LastGrabTick = Tick;
		RollProfile();
		// a player has a favourite spot: remember the floor we spawn on and
		// come back to it when there is nothing better to do
		m_HomeFloor = pGS->BotFloorAt(MyPos);
		m_FloorGoal = m_HomeFloor;
		m_MyFloor = m_HomeFloor;
		m_FloorTicks = 0;
		m_NavIdx = -1;
		PickNavPoint(pGS, MyPos, MyTeam, Tick, m_FloorGoal);
		if(g_Config.m_SvBotDebug)
		{
			char aBuf[192];
			str_format(aBuf, sizeof(aBuf), "bot %d: skill %.2f, form %d, home floor %d",
				ClientID, m_Skill, m_AimMode, m_HomeFloor);
			pGS->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
		}
	}

	// --- pick a target: the habit decides who looks worth attacking ---
	// fng_trainbot: who is already working on this body. Without this every bot
	// scored the same frozen tee the best (he is worth 900 points!) and all four
	// walked onto him at once — the pile-up in the screenshot. A body that a
	// mate already hooks, drags or hammers is *his*, and the rest of us look
	// for somebody else to freeze instead.
	// fng_trainbot: also spread by height. All four bots chasing one frozen tee
	// ended up on his shelf, because the floor-spread rule only looked at floors
	// a teammate had already *reached*. Counting everybody who is heading the
	// same way keeps two of them off the same platform.
	if(Tick >= m_RetargetTick)
	{
		m_RetargetTick = Tick + 10;
		m_TargetCID = -1;
		float Best = 0.0f;
		int BestT = -1;
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
			float d = distance(MyPos, pC->m_Pos);
			float Score = d - (pC->IsFrozen() ? 600.0f : 0.0f);

			// fng_trainbot: this is where the learned habit becomes visible.
			// Before, the habit only chose a floor to walk to and the fighting
			// itself was always identical — which is why two bots with very
			// different weights played exactly the same game.
			if(m_Action == BOTACT_THROW)
				Score -= pC->IsFrozen() ? 900.0f : 250.0f; // prey first, live ones only if nothing frozen
			else if(m_Action == BOTACT_HUNT)
				Score -= pC->IsFrozen() ? 100.0f : 450.0f; // the point is to freeze him ourselves
			else if(m_Action == BOTACT_HOLD)
			{
				// holding a spot: do not walk the whole map for a kill
				int F = pGS->BotFloorAt(pC->m_Pos);
				if(F >= 0 && F != m_MyFloor)
					Score += 2500.0f;
			}

			// fng_trainbot: somebody of ours is already on this body — hooked to
			// him, dragging him or standing right next to him swinging. One body,
			// one thrower. The penalty has to beat the frozen bonus, otherwise
			// the pile-up is still the best-scoring option for everybody.
			// A live enemy is never "taken": freezing him ourselves is the job.
			if(pC->IsFrozen() && (BotTargetClaimed(pGS, ClientID, MyTeam, i) ||
				BotBodyTaken(pGS, ClientID, MyTeam, pC, d)))
				Score += 2200.0f;
			// fng_trainbot: a live enemy is not "taken" (freezing him is the
			// job), but a mate already walking at him is — one shooter per
			// target, or the team forms a queue in front of him
			else if(!pC->IsFrozen() && BotTargetClaimed(pGS, ClientID, MyTeam, i))
				Score += 1200.0f;
			// fng_trainbot: and a mate already walking towards this man's shelf
			// is reason enough to look for a fight on a different level
			if(pGS->m_NumBotFloors > 0)
			{
				int F = pGS->BotFloorAt(pC->m_Pos);
				if(F >= 0 && BotTeammatesHeadingFor(pGS, ClientID, MyTeam, F) > 0)
					Score += pC->IsFrozen() ? 400.0f : 700.0f;
			}

			if(BestT < 0 || Score < Best)
			{
				Best = Score;
				BestT = i;
			}
		}
		m_TargetCID = BestT;
	}

	// --- Choose a tactical action from the learned neural Q-network every few
	// seconds. Actions without a valid target are masked; reward updates train
	// the selected action for the observed state.
	if(Tick >= m_ActionTick)
	{
		const int State = Observation(pGS, ClientID);
		const float Alpha = g_Config.m_SvBotLearnRate / 100.0f;
		if(g_Config.m_SvBotLearn && m_HasTransition)
			UpdateQ(m_LastState, m_LastAction, m_PendingReward, State, false, Alpha);
		m_PendingReward = 0.0f;
		m_Action = ChooseAction(pGS, ClientID);
		m_LastState = State;
		m_LastAction = m_Action;
		m_HasTransition = true;
		m_ActionTick = Tick + pGS->Server()->TickSpeed() * (2 + (int)(frandom() * 3.0f));
		m_ActionRewardTick = Tick;
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

	// fng_trainbot: has a mate already picked this man as his own? Claims are
	// checked for the target as well as for a frozen body: with four bots a side
	// the pile did not disappear, it moved — four team-mates walking at one live
	// enemy until they stood in one heap. Same rule, same reason.
	int TargetCID = pTarget && pTarget->GetPlayer() ? pTarget->GetPlayer()->GetCID() : -1;
	bool TargetMine = pTarget && (TargetCID < 0 || !BotTargetClaimed(pGS, ClientID, MyTeam, TargetCID));

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

	// fng_trainbot: the shot belongs to the bot's current form, re-rolled every
	// second or two; the error also grows when the target moves unpredictably
	if(m_AimModeTicks <= 0)
		RollAimForm();
	else
		m_AimModeTicks--;
	if(m_HoldFireTicks > 0)
		m_HoldFireTicks--;

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
	// fng_trainbot: the game drops a grabbed tee after ~1.25s, so reeling in is
	// a burst of grabs. Remember the tick every fresh grab started.
	if(pCarried)
	{
		if(m_CarryTicks == 0)
		{
			m_GrabCount++;
			m_LastPreyCID = pCarried->GetPlayer()->GetCID();
			m_RegrabUntil = Tick + 220;
			if(g_Config.m_SvBotDebug)
			{
				char aBuf[192];
				str_format(aBuf, sizeof(aBuf), "bot %d: grabbed prey at %.0f,%.0f (grab #%d)",
					pSelf->GetCID(), pCarried->m_Pos.x, pCarried->m_Pos.y, m_GrabCount);
				pGS->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
			}
		}
		if(m_CarryTicks < 100000)
			m_CarryTicks++;
		if(m_CarryTicks == 1)
			m_LastGrabTick = Tick;
		// fng_trainbot: the rope lets go after ~1.25s, but the body stays
		// frozen for the whole timer. Keep an eye on it well past the drop so
		// the bot walks back and hooks him again instead of forgetting.
		m_RegrabUntil = Tick + 220;
	}
	else
	{
		m_CarryTicks = 0;
		// fng_trainbot: the body we were dragging a moment ago is still frozen
		// and still worth a spike. A player simply hooks him again and keeps
		// walking; forgetting the victim here is what made a drag end after a
		// single grab and never reach the teeth. The window does NOT depend on
		// the throw plan being found on this very tick — the plan flickers as
		// the geometry changes, and tying the two together killed the window.
		CPlayer *pOld = (m_LastPreyCID >= 0 && m_LastPreyCID < MAX_CLIENTS) ? pGS->m_apPlayers[m_LastPreyCID] : 0;
		CCharacter *pOldC = pOld ? pOld->GetCharacter() : 0;
		if(Tick < m_RegrabUntil && pOldC && pOldC->IsAlive() && pOldC->IsFrozen() && BotIsEnemy(pSelf, pOld))
		{
			m_RegrabTarget = pOldC;
			if(Tick >= m_ThrowTick)
				PlanThrow(pGS, MyPos, pOldC->m_Pos, MyTeam, Tick);
		}
		else
		{
			m_RegrabTarget = 0;
			m_LastPreyCID = -1;
			m_ThrowIdx = -1; // nothing to throw right now
			m_ThrowTick = 0;
		}
	}

	// fng_trainbot: the body we are trying to turn into a spike kill — either
	// the one already on our hook, or a frozen enemy lying nearby.
	CCharacter *pPrey = pCarried;
	if(!pPrey && pTarget && pTarget->IsAlive() && pTarget->IsFrozen())
		pPrey = pTarget;

	// fng_trainbot: is this body ours, or is a mate already elbow-deep in him?
	// One thrower per body. Everybody else keeps shooting at live enemies,
	// which is the whole difference between a team and a queue.
	//
	// fng_trainbot: the test that matters is the *claim*, not the proximity.
	// Standing near a body was only true once somebody had already walked up,
	// so four bots all walked up together and only then noticed the queue — the
	// exact pile from the screenshot. Whoever picked this body as his target
	// first owns him, and the rest keep their distance from the start.
	float PreyDist = pPrey ? distance(MyPos, pPrey->m_Pos) : 0.0f;
	CPlayer *pPreyP = pPrey ? pPrey->GetPlayer() : 0;
	int PreyCID = pPreyP ? pPreyP->GetCID() : -1;
	bool PreyMine = pPrey && !pCarried &&
		(PreyCID < 0 || !BotTargetClaimed(pGS, ClientID, MyTeam, PreyCID)) &&
		!BotBodyTaken(pGS, ClientID, MyTeam, pPrey, PreyDist);

	// fng_trainbot: while the body is not on the hook, a hammer swing is the
	// cheaper play — one hit launches him several hundred pixels towards the
	// teeth, where the rope would have dropped him after 1.25s long before
	// arrival. The plan is re-made every moment or so, and dropped as soon as
	// there is no frozen enemy left to throw.
	if(pPrey && !pCarried && g_Config.m_SvBotThrow && pPrey->GetPlayer())
	{
		if(m_HammerIdx < 0 || Tick >= m_HammerTick || m_HammerVictim != pPrey->GetPlayer()->GetCID())
			PlanHammer(pGS, MyPos, pPrey->m_Pos, MyTeam, Tick, pPrey->GetPlayer()->GetCID());
	}
	else
	{
		m_HammerIdx = -1;
		m_HammerVictim = -1;
		m_HammerTick = Tick + 10;
	}
	bool HammerTactic = m_HammerIdx >= 0 && !pCarried && pPrey != 0 && !pMe->IsFrozen() && PreyMine;
	// the swing lands when the body is right in front of us (the hammer hit box
	// is only about 35px wide) and we are on the far side of him, so the knock
	// points at the cluster rather than away from it. Asking for the exact
	// stand spot was wrong: to get in range the bot has to *leave* that spot.
	bool HammerSwing = false;
	if(HammerTactic)
	{
		float dBody = distance(MyPos, pPrey->m_Pos);
		const CGameContext::CBotThrowTarget &HT = pGS->m_aBotThrowTargets[m_HammerIdx];
		bool FarSide = dot(MyPos - pPrey->m_Pos, HT.m_Pos - pPrey->m_Pos) < 0.0f;
		// and at roughly the body's own height: from a different shelf the push
		// goes almost straight up (measured: 25px of travel) and never reaches
		// the teeth, so the swing is only worth taking sideways-on
		bool SameLevel = fabsf(MyPos.y - pPrey->m_Pos.y) < 46.0f;
		if(dBody > 7.0f && dBody < 38.0f && FarSide && SameLevel &&
			BotLineOfSight(pGS, MyPos, pPrey->m_Pos))
			HammerSwing = true;
	}

	// --- fng_trainbot: the throw. A victim on the hook drifts towards the
	//     hooker, so we do not aim at the spikes at all — we stand where a
	//     cluster sits on the line prey -> bot and the drag delivers him. ---
	if(pCarried && Tick >= m_ThrowTick)
		PlanThrow(pGS, MyPos, pCarried->m_Pos, MyTeam, Tick);

	bool HaveSpike = pCarried && m_ThrowIdx >= 0;
	vec2 SpikePos = HaveSpike ? pGS->m_aBotThrowTargets[m_ThrowIdx].m_Pos : MyPos;
	float SpikeR = HaveSpike ? pGS->m_aBotThrowTargets[m_ThrowIdx].m_Radius : 0.0f;

	// --- fng_trainbot: floors, not tiles. A player who cannot reach a tee
	//     does not hover in the air — he walks to the floor the prey is on.
	//     Mid-jump nobody knows what floor he is in, so the last one stands. ---
	int MyFloor = pGS->BotFloorAt(MyPos);
	if(MyFloor >= 0)
		m_MyFloor = MyFloor;
	else
		MyFloor = m_MyFloor;
	int TargetFloor = -1;
	if(pCarried)
		TargetFloor = pGS->BotFloorAt(pCarried->m_Pos);
	else if(pTarget)
		TargetFloor = pGS->BotFloorAt(pTarget->m_Pos);
	if(TargetFloor >= 0)
		m_TargetFloor = TargetFloor;
	else if(m_TargetCID < 0)
		m_TargetFloor = -1; // out of enemies, out of memory
	else
		TargetFloor = m_TargetFloor; // he is airborne: we still know his shelf
	if(m_FloorTicks > 0)
		m_FloorTicks--;
	else
	{
		// plans are re-made every half second or so: a human re-reads the map
		// constantly rather than committing to one shelf forever
		m_FloorTicks = 30 + (int)(frandom() * 60.0f);
		float Roam = clamp(g_Config.m_SvBotRoam / 100.0f, 0.0f, 1.0f);

		// fng_trainbot: the learned habit decides where "there" is. Hunting
		// means the enemy's shelf, holding means our own, rescuing means the
		// frozen teammate's, roaming means a level nobody of the team holds.
		if(m_Action == BOTACT_RESCUE)
		{
			// the first frozen teammate standing on a known floor wins: freeing
			// anybody at all is worth more than freeing the nearest one
			int RescueFloor = -1;
			for(int i = 0; i < MAX_CLIENTS && RescueFloor < 0; i++)
			{
				if(i == ClientID)
					continue;
				CPlayer *p = pGS->m_apPlayers[i];
				if(!p || p->GetTeam() != MyTeam)
					continue;
				CCharacter *pC = p->GetCharacter();
				if(!pC || !pC->IsAlive() || !pC->IsFrozen())
					continue;
				RescueFloor = pGS->BotFloorAt(pC->m_Pos);
			}
			if(RescueFloor >= 0)
				m_FloorGoal = RescueFloor;
		}
		else if(m_Action == BOTACT_HOLD && m_HomeFloor >= 0)
			m_FloorGoal = m_HomeFloor; // stand our ground instead of roaming
		else if(TargetFloor >= 0)
		{
			// fng_trainbot: everybody piling onto the enemy's shelf was the
			// whole problem — four bots on one floor shoot each other in the
			// back. If a teammate is already on that shelf we take the one
			// above or below instead, where we still see the fight.
			if(BotTeammatesOnFloor(pGS, ClientID, MyTeam, TargetFloor) > 0)
			{
				int aNear[6];
				int nNear = 0;
				for(int f = 0; f < pGS->m_NumBotFloors && nNear < 6; f++)
				{
					if(f == TargetFloor)
						continue;
					if(abs(f - TargetFloor) > 3)
						continue;
					if(BotTeammatesOnFloor(pGS, ClientID, MyTeam, f) > 0)
						continue;
					aNear[nNear++] = f;
				}
				m_FloorGoal = nNear > 0 ? aNear[(int)(frandom() * nNear) % nNear] : TargetFloor;
			}
			else
				m_FloorGoal = TargetFloor; // nobody there yet: we take it
		}
		else if(pGS->m_NumBotFloors > 0 && MyFloor == m_FloorGoal &&
			frandom() < 0.15f + Roam * 0.6f)
			// nothing to do here anymore — take a walk, like someone hunting
			// for a game. A level nobody of the team holds comes first, so
			// the bots spread over the map instead of stacking on one shelf
			m_FloorGoal = BotPickUncoveredFloor(pGS, ClientID, MyTeam);
		else if(m_HomeFloor >= 0 && frandom() < 0.65f - Roam * 0.4f)
			m_FloorGoal = m_HomeFloor; // otherwise the favourite floor wins
		else if(pGS->m_NumBotFloors > 0)
			m_FloorGoal = BotPickUncoveredFloor(pGS, ClientID, MyTeam);
	}
	int FloorPref = TargetFloor >= 0 ? TargetFloor : m_FloorGoal;

	// fng_trainbot: the console trail you watch while tuning the bot — every
	// five seconds what level it thinks it is on and what it is doing about it
	if(g_Config.m_SvBotDebug && Tick % 100 == ClientID % 100)
	{
		char aBuf[320];
		int Hooked = pMe->GetHookedPlayerID();
		float DistTo = pTarget ? distance(MyPos, pTarget->m_Pos) : -1.0f;
		str_format(aBuf, sizeof(aBuf),
			"bot %d: here %d, working %d (home %d, enemy %d), target %d%s, carry %d, trap %d, hook %d%s, dist %.0f, act %s",
			ClientID, MyFloor, m_FloorGoal, m_HomeFloor, TargetFloor, m_TargetCID,
			m_ClimbTicks > 0 ? " climbing" : "", m_CarryTicks, m_ThrowIdx, Hooked,
			pTarget && pTarget->IsFrozen() ? " FROZEN" : "", DistTo,
			CBotAI::ActionName(m_Action));
		pGS->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
	}

	// fng_trainbot: one navigation goal — the throw spot when dragging, the
	// enemy when there is one, otherwise the patrol point of the chosen floor
	vec2 Goal = HammerTactic ? m_HammerStand : (pTarget ? pTarget->m_Pos : m_NavGoal);
	if(pCarried && HaveSpike)
		Goal = SpikePos; // the teeth, not a stand spot: the way up is the target
	bool HaveGoal = pTarget != 0 || (pGS->m_NumBotNav > 0 && m_NavIdx >= 0) || (pCarried && HaveSpike);

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
			m_ClimbingUp = false;
			Climbing = false;
		}
	}
	// fng_trainbot: climbing is allowed while dragging too, and that is the
	// whole point of the new throw. A recorded throw lifted the body 283px by
	// the hooker going *up* — the rope carries what is under him, and a body
	// that is dragged sideways simply slides off. Climbing used to be switched
	// off whenever a body was on the rope, which is precisely what made the
	// upward lift impossible.
	if(m_BackoffTicks <= 0 && HaveGoal && !Boosting && (!pCarried || (pCarried && HaveSpike)))
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
			m_ClimbingUp = A.y < MyPos.y - 20.0f;
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
	if(m_SwingCooldown > 0)
		m_SwingCooldown--;

	// fng_trainbot: swinging. Only on the way to something, only when the way is
	// long, and never while a body is on the rope — a swing takes the hook away
	// from the throw, which is the one thing that must not be interrupted.
	bool Swinging = m_SwingTicks > 0;
	if(Swinging)
	{
		m_SwingTicks--;
		// the rope only helps while we are actually gaining speed along it; once
		// the swing has carried us past its apex it is just dead weight
		int Speed = (int)(pMe->GetVel().x * (float)m_SwingDir);
		bool Past = Speed <= m_SwingBestSpeed - 6;
		if(distance(MyPos, Goal) < 260.0f || Past ||
			(pMe->IsGrounded() && m_SwingTicks < 20) || !pMe->IsHookGrabbed())
		{
			m_SwingTicks = 0;
			m_SwingCooldown = 24;
			Swinging = false;
		}
		else if(Speed > m_SwingBestSpeed)
			m_SwingBestSpeed = Speed;
	}
	if(!Swinging && !pCarried && !Climbing && !Boosting && !HammerTactic &&
		m_BackoffTicks <= 0 && m_SwingCooldown <= 0 && HaveGoal)
	{
		float d = distance(MyPos, Goal);
		float ddx = Goal.x - MyPos.x;
		// a swing carries a few hundred pixels, so swinging at a goal on the
		// far side of the map is theatre: it burns the hook and the cooldown
		// and the bot ends up exactly where it started. Only swing at something
		// a couple of swings can actually add up to.
		if(d > 480.0f && d < 1800.0f && fabsf(ddx) > 200.0f)
		{
			vec2 A;
			if(BotFindSwingAnchor(pGS, MyPos, Goal, &A))
			{
				m_SwingAnchor = A;
				m_SwingTicks = 30;
				m_SwingCooldown = 40;
				m_SwingDir = ddx > 0.0f ? 1 : -1;
				m_SwingBestSpeed = 0;
				Swinging = true;
				if(g_Config.m_SvBotDebug)
				{
					char aBuf[160];
					str_format(aBuf, sizeof(aBuf), "bot %d: swing at %.0f,%.0f towards %.0f,%.0f",
						ClientID, A.x, A.y, Goal.x, Goal.y);
					pGS->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
				}
			}
		}
	}

	if(!pCarried && !Climbing && !Boosting && !Swinging && m_BackoffTicks <= 0 &&
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

	// fng_trainbot: standing in the way is worse than standing still. The shot
	// itself is gated on !BotTeeInLine below, and the engagement band below that
	// never stands still, so the bot ends up off the line without a special
	// rule — a bool computed here and never read is not that rule.

	// --- movement: take position, never just stand there ---
	int Dir = 0;
	if(m_BackoffTicks > 0)
	{
		m_BackoffTicks--;
		Dir = m_BackoffDir;
	}
	else if(pCarried)
	{
		// fng_trainbot: the recorded throws all went straight up. The feet line
		// up under the teeth and then climb, and the rope walks the body up on
		// top of the hooker. So: get under the cluster, then go up — never walk
		// away along the shelf, which is what used to slide the body past the
		// teeth and end 170..545px short every time.
		if(HaveSpike)
		{
			float dxs = SpikePos.x - MyPos.x;
			float dxsBody = pCarried->m_Pos.x - MyPos.x;
			// stay under the body while it is still below us: the rope only
			// lifts what is roughly underneath, sideways and the body slides off
			if(pCarried->m_Pos.y > MyPos.y - 60.0f)
			{
				float Want = fabsf(dxsBody) > 12.0f ? dxsBody : dxs;
				Dir = fabsf(Want) > 12.0f ? (Want > 0.0f ? 1 : -1) : 0;
			}
			else
			{
				// we are level with the body or above it: the rest of the lift is
				// the hook-climb above, which the vertical state handles
				Dir = fabsf(dxs) > 10.0f ? (dxs > 0.0f ? 1 : -1) : 0;
			}
		}
		else
		{
			// no throw in sight: stay above the prey and keep the pull
			float dx = pCarried->m_Pos.x - MyPos.x;
			Dir = fabsf(dx) > 24.0f ? (dx > 0.0f ? 1 : -1) : 0;
		}
	}
	else if(Boosting)
		Dir = m_BoostDir;
	else if(Swinging)
	{
		// fng_trainbot: steer through the swing. The rope decides the arc, the
		// feet only decide how much of it we keep: aim at the goal and let the
		// air control bend the flight, which is exactly what a person does and
		// what makes a swinging bot much harder to shoot than a walking one.
		float dsw = Goal.x - MyPos.x;
		Dir = fabsf(dsw) > 24.0f ? (dsw > 0.0f ? 1 : -1) : m_SwingDir;
	}
	else if(HammerTactic)
	{
		// fng_trainbot: hammer throw — first get to the far side of the body so
		// the swing points at the teeth, then step in until the hit box covers
		// him. The moment we are on the right side the stand is forgotten (it
		// used to be re-checked every tick, which sent the bot walking back and
		// forth instead of ever swinging).
		float dBody = distance(MyPos, pPrey->m_Pos);
		const CGameContext::CBotThrowTarget &HT = pGS->m_aBotThrowTargets[m_HammerIdx];
		bool FarSide = dot(MyPos - pPrey->m_Pos, HT.m_Pos - pPrey->m_Pos) < 0.0f;
		float dvx = pPrey->m_Pos.x - MyPos.x;
		if(!FarSide && distance(MyPos, m_HammerStand) > 30.0f)
		{
			float dxs = m_HammerStand.x - MyPos.x;
			Dir = fabsf(dxs) > 10.0f ? (dxs > 0.0f ? 1 : -1) : 0;
		}
		else if(dBody > 24.0f)
			Dir = fabsf(dvx) > 5.0f ? (dvx > 0.0f ? 1 : -1) : 0;
		else
			Dir = 0;
	}
	else if(m_RegrabTarget && m_RegrabTarget->IsAlive() && m_RegrabTarget->IsFrozen())
	{
		// fng_trainbot: we lost the body but he is still frozen — walk back to
		// him and take the rope again instead of starting over somewhere else
		float dxr = m_RegrabTarget->m_Pos.x - MyPos.x;
		float Dr = distance(MyPos, m_RegrabTarget->m_Pos);
		Dir = Dr > 26.0f ? (dxr > 0.0f ? 1 : -1) : 0;
	}
	else if(pTarget)
	{
		float dx = pTarget->m_Pos.x - MyPos.x;
		float dy = pTarget->m_Pos.y - MyPos.y;
		float hd = fabsf(dx);
		// fng_trainbot: a tee on another floor that we cannot shoot is not
		// jumped at — that is what a bot does. A player walks to his level.
		// But we walk to OUR level, not always to the enemy's: if a teammate
		// already holds his shelf we take the one above or below, otherwise the
		// whole team ends up shoulder to shoulder on one floor.
		int ChaseFloor = HammerTactic ? pGS->BotFloorAt(m_HammerStand) : TargetFloor;
		float DTarget = distance(MyPos, pTarget->m_Pos);
		// fng_trainbot: spreading the team over neighbouring shelves only makes
		// sense once the fight is joined. Applied while still walking to the
		// enemy it did the opposite: everybody left for a different level and
		// nobody ever arrived (the log showed distances of 765..3500px and a
		// hook that never touched anybody).
		if(!HammerTactic && DTarget < 700.0f && m_FloorGoal >= 0 && m_FloorGoal != TargetFloor &&
			BotTeammatesOnFloor(pGS, ClientID, MyTeam, TargetFloor) > 0)
			ChaseFloor = m_FloorGoal;
		bool OtherFloor = ChaseFloor >= 0 && MyFloor >= 0 && ChaseFloor != MyFloor;
		bool InReach = hd < 400.0f && dy > -170.0f && dy < 220.0f;
		if(HammerTactic)
		{
			// Follow the calculated far-side stance, then close to hammer range.
			// Chasing the corpse directly made the bot stand on top of it without
			// ever lining up the hit towards the spike cluster.
			int StandFloor = pGS->BotFloorAt(m_HammerStand);
			bool StandOtherFloor = StandFloor >= 0 && MyFloor >= 0 && StandFloor != MyFloor;
			if(StandOtherFloor && !Climbing)
			{
				if(m_NavIdx < 0 || pGS->m_NumBotNav <= 0 || Tick >= m_NavRetargetTick ||
					distance(MyPos, m_NavGoal) < 72.0f)
					PickNavPoint(pGS, MyPos, MyTeam, Tick, StandFloor);
				float dxn = m_NavGoal.x - MyPos.x;
				Dir = fabsf(dxn) > 16.0f ? (dxn > 0.0f ? 1 : -1) : 0;
			}
			else if(Climbing)
				Dir = m_ClimbDir;
			else if(distance(MyPos, m_HammerStand) > 36.0f)
			{
				float dxs = m_HammerStand.x - MyPos.x;
				Dir = fabsf(dxs) > 16.0f ? (dxs > 0.0f ? 1 : -1) : 0;
			}
			else
			{
				float dBody = distance(MyPos, pTarget->m_Pos);
				Dir = dBody > 28.0f ? (dx > 0.0f ? 1 : -1) : 0;
			}
		}
		else if(OtherFloor && !InReach && !Climbing)
		{
			if(m_NavIdx < 0 || pGS->m_NumBotNav <= 0 || Tick >= m_NavRetargetTick ||
				distance(MyPos, m_NavGoal) < 72.0f)
				PickNavPoint(pGS, MyPos, MyTeam, Tick, ChaseFloor);
			float dxn = m_NavGoal.x - MyPos.x;
			Dir = fabsf(dxn) > 16.0f ? (dxn > 0.0f ? 1 : -1) : 0;
		}
		else if(Climbing)
			Dir = m_ClimbDir; // drift towards the prey while being pulled up
		else if(dy < -140.0f && hd < 700.0f)
			Dir = hd > 40.0f ? (dx > 0.0f ? 1 : -1) : 0; // get under him
		else if(hd > 430.0f || dy > 260.0f)
		{
			// fng_trainbot: close the gap — but a mate already owns this man, so
			// stop at shooting range instead of walking into the same tile. Four
			// bots converging on one enemy is the same pile as four bots on one
			// frozen body, just with somebody still moving in the middle of it.
			float StopLive = TargetMine ? 430.0f : 1050.0f;
			Dir = hd > StopLive ? (dx > 0.0f ? 1 : -1) : 0;
		}
		else if(hd < 230.0f && !pTarget->IsFrozen())
			Dir = dx != 0.0f ? (dx > 0.0f ? -1 : 1) : m_StrafeDir; // too close: make space
		else if(pTarget->IsFrozen())
		{
			// fng_trainbot: a frozen body is cargo, but it is also a solid object
			// you can climb on — and that is the pose from the screenshot: the bot
			// balanced on top of the corpse and the rope caught him from
			// underneath, which cannot be thrown anywhere. Two rules: never walk
			// into the body, and step off it if we ended up on it.
			float D = distance(MyPos, pTarget->m_Pos);
			bool OnTop = pMe->IsGrounded() && pTarget->m_Pos.y > MyPos.y + 26.0f &&
				fabsf(dx) < 44.0f;
			// the rope only bites a body it literally touches (~30px), so stopping
			// far away means never catching anything
			//
			// ... unless a mate has already claimed him. Then walking up is what
			// produced the pile in the first place: four tees shoulder to
			// shoulder on one corpse. Stand off and cover the throw instead.
			float Stop = PreyMine ? 30.0f : 190.0f;
			if(OnTop)
				Dir = m_SwingDir ? m_SwingDir : (dx > 0.0f ? -1 : 1);
			else
				Dir = D > Stop ? (dx > 0.0f ? 1 : -1) : (D < Stop - 40.0f ? (dx > 0.0f ? -1 : 1) : 0);
		}
		else
		{
			// engagement band: patrol left-right instead of standing still.
			// The side must NOT re-roll every tick — that is not "changing
			// sides", that is a tee vibrating on the spot. The side is chosen
			// once and kept until the timer runs out, and even then only with
			// a coin flip, so the walk reads as a person picking a spot.
			if(--m_StrafeTicks <= 0)
			{
				m_StrafeTicks = 40 + (int)(frandom() * 40.0f);
				m_StrafeDir = frandom() < 0.5f ? -1 : 1;
			}
			Dir = m_StrafeDir;
		}
	}
	else
	{
		// fng_trainbot: patrol the map's shelves instead of pacing one
		// spot — the floor we decided to work on first, own side favoured,
		// re-pick on arrival or when the walk takes too long
		if(m_NavIdx < 0 || pGS->m_NumBotNav <= 0 ||
			(Tick >= m_NavRetargetTick && !Climbing) ||
			distance(MyPos, m_NavGoal) < 72.0f)
			PickNavPoint(pGS, MyPos, MyTeam, Tick, FloorPref);
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

	// --- safety & throw navigation ---
	bool EdgeBlocked = false;
	if(Dir != 0)
	{
		int f = pGS->Collision()->GetCollisionAt(MyPos.x + Dir * 48.0f, MyPos.y);
		int f2 = pGS->Collision()->GetCollisionAt(MyPos.x + Dir * 48.0f, MyPos.y - 20.0f);
		int f3 = pGS->Collision()->GetCollisionAt(MyPos.x + Dir * 48.0f, MyPos.y + 16.0f);
		if((f | f2 | f3) & BOT_DANGER_MASK)
		{
			if(Boosting)
			{
				m_BoostTicks = 0;
				Boosting = false;
				Dir = 0;
			}
			else if(pCarried)
			{
				// we are right at the spikes edge with prey: stop walking into spikes
				Dir = 0;
				EdgeBlocked = true;
			}
			else if(Climbing)
				Dir = 0;
			else
				Dir = -Dir;
		}
		else if(BotDeadlyDrop(pGS, MyPos, Dir) && !(Climbing && m_ClimbingUp))
		{
			// fng_trainbot: the shaft guard holds unless we are hooked
			// upwards — crossing the pit on the hook is the climb itself.
			// It also stands down while a body is on the rope and the step
			// leads down towards the throw stand: dropping off the ledge is
			// exactly how the drag walks the prey onto the teeth of the shelf
			// below, and the guard must not forbid the throw itself.
			bool ThrowDescent = pCarried && HaveSpike && MyPos.y < m_ThrowStand.y - 40.0f &&
				fabsf(MyPos.x - m_ThrowStand.x) < 240.0f && BotDropIsSafe(pGS, MyPos, Dir);
			if(!ThrowDescent)
			{
				Dir = 0;
				if(pCarried)
					EdgeBlocked = true;
			}
		}
	}

	// fng_trainbot: walking into a wall or the spikes' own edge means the
	// angle we picked does not work — choose a different one right away
	// instead of leaning on the collision until the plan times out
	if(EdgeBlocked)
		m_ThrowTick = 0;

	// --- fng_trainbot: the reel. A hooked tee is dragged towards whoever holds
	//     the hook, and the game drops him after ~1.25s and re-latches the next
	//     time the button is pressed — which is exactly what a player holding
	//     the key does while walking a body somewhere unpleasant. So the hook
	//     stays down and the only decision left is when to give up on him. ---
	bool WantRelease = false;
	if(pCarried)
	{
		float dPreySpike = HaveSpike ? distance(pCarried->m_Pos, SpikePos) : 1e9f;
		if(!HaveSpike && m_CarryTicks > 120)
			WantRelease = true; // nowhere to put him: drop him and fight clean
		else if(HaveSpike && m_CarryTicks > 300 && dPreySpike > 220.0f + SpikeR)
			WantRelease = true; // the throw went nowhere, stop carrying cargo
	}

	// little hops while dragging — moving a body is heavy work
	bool FlingJump = pCarried && pMe->IsGrounded() && (m_CarryTicks % 23) == 3;

	if(WantRelease)
	{
		// step off the spikes' edge while re-aiming, never into them
		float Away = MyPos.x - SpikePos.x;
		if(Away > 4.0f) m_BackoffDir = -1;
		else if(Away < -4.0f) m_BackoffDir = 1;
		else m_BackoffDir = m_IdleDir;
		m_BackoffTicks = 12;
		m_CarryTicks = 0;
		m_ThrowIdx = -1;
		m_ThrowTick = 0;
		HaveSpike = false;
	}

	// --- stuck detection: hop over small obstacles ---
	if(Tick - m_StuckTick >= 16)
	{
		float Moved = distance(MyPos, m_LastPos);
		if(Dir != 0 && Moved < 10.0f && m_JumpCooldown <= 0 && !pMe->IsFrozen())
		{
			m_JumpTicks = 12;
			m_JumpCooldown = 35;
		}
		m_LastPos = MyPos;
		m_StuckTick = Tick;
	}
	// fng_trainbot: swing off the mark like a person — a running jump into the
	// rope turns ground speed into the arc, a standing start throws it away
	bool SwingJump = Swinging && pMe->IsGrounded() && m_SwingTicks > 12;
	bool WantJump = m_JumpTicks > 0 || FlingJump || SwingJump;
	if(m_JumpTicks > 0) m_JumpTicks--;
	if(m_JumpCooldown > 0) m_JumpCooldown--;

	// --- hook priority: carry prey > climb/boost anchor > attack enemy ---
	bool JustReleased = WantRelease;
	bool WantHook = false;
	// fng_trainbot: the hook only reaches about HookLength (380px). Firing it
	// from 650px looked like the bot was trying and did nothing: the hook flew
	// into the wall behind the target and retracted, so a frozen victim was
	// never actually grabbed and the throw never started.
	const float HookReach = 360.0f;
	if(m_BackoffTicks <= 0 && !JustReleased)
	{
		if(pCarried)
			WantHook = true;
		else if(Climbing || Boosting)
			WantHook = true;
		// fng_trainbot: a swing is nothing without the rope in the anchor — this
		// is the whole difference between crossing the map and jogging across it
		else if(Swinging)
			WantHook = true;
		// fng_trainbot: during a hammer throw the rope would pull the body
		// away from the teeth (it drifts towards the hooker), so the hammer
		// tactic never fires the hook
		else if(!HammerTactic && pTarget)
		{
			// Grab frozen enemy or close live enemy with hook — only when he is
			// really inside the rope's reach
			float d = distance(MyPos, pTarget->m_Pos);
			// fng_trainbot: the rope does not care what it catches and the game
			// uses ONE state for a tee and for a wall — only m_HookedPlayer tells
			// them apart, and it stays -1 on a block. Aiming from below catches
			// the underside of a corpse and lifts nothing at all, so the grab is
			// only taken from the same height or from above.
			bool SameLevel = pTarget->m_Pos.y < MyPos.y + 60.0f;
			bool Reach = d < HookReach && SameLevel && BotLineOfSight(pGS, MyPos, pTarget->m_Pos);
			if(!pTarget->IsFrozen())
				WantHook = Reach;
			// fng_trainbot: a mate is already dragging this one — two ropes on the
			// same body mean the two bots pull against each other and neither
			// throw lands, so only the bot who claimed him hooks.
			else if(PreyMine && Reach)
			{
				// fng_trainbot: do not grab a body we cannot turn into a spike
				// kill. The drag lasts ~1.25s and the game drops the victim, so
				// catching somebody with no reachable spikes is pure waste: it
				// spends the freeze window, blocks our own lane of fire and
				// teaches nothing. Only take the bait when spikes are in range.
				bool WorthIt = !g_Config.m_SvBotThrow || pGS->m_NumBotThrowTargets <= 0;
				if(!WorthIt)
				{
					float Nearest = 1e9f;
					for(int t = 0; t < pGS->m_NumBotThrowTargets; t++)
					{
						const CGameContext::CBotThrowTarget &T = pGS->m_aBotThrowTargets[t];
						if(!BotValidSpikeForTeam(T.m_Flags, MyTeam))
							continue;
						float ds = distance(pTarget->m_Pos, T.m_Pos);
						if(ds < Nearest)
							Nearest = ds;
					}
					WorthIt = Nearest < 850.0f;
				}
				if(WorthIt)
					WantHook = true;
			}
		}
		else if(!HammerTactic && m_RegrabTarget && m_RegrabTarget->IsAlive() && m_RegrabTarget->IsFrozen())
		{
			// fng_trainbot: we lost the body mid-drag but he is still frozen —
			// go and take him again instead of starting over with somebody else
			float d = distance(MyPos, m_RegrabTarget->m_Pos);
			if(d < HookReach && BotLineOfSight(pGS, MyPos, m_RegrabTarget->m_Pos))
				WantHook = true;
		}
	}

	// --- rescues: a frozen teammate is freed with the hammer in plain FNG and
	// with the pistol in SAH. This used to sit behind UsesSahScoring(), so on a
	// normal fng2 server the bots never even looked for their own frozen
	// mates — they walked past them and kept fighting.
	CCharacter *pHeal = 0;
	CCharacter *pHealThreat = 0;
	if(MyTeam >= TEAM_RED && MyTeam <= TEAM_BLUE)
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

		// fng_trainbot: unpicking a teammate with an enemy standing over him
		// is how the whole team ends up frozen — the freezer has to go first.
		// A human reads that in half a second; the bot needs it spelled out.
		if(pHeal)
		{
			float BestT = 0.0f;
			for(int i = 0; i < MAX_CLIENTS; i++)
			{
				if(i == ClientID)
					continue;
				CPlayer *p = pGS->m_apPlayers[i];
				if(!p || !BotIsEnemy(pSelf, p))
					continue;
				CCharacter *pC = p->GetCharacter();
				if(!pC || !pC->IsAlive() || pC->IsFrozen())
					continue;
				float d = distance(pC->m_Pos, pHeal->m_Pos);
				if(d > 700.0f)
					continue;
				if(!pHealThreat || d < BestT)
				{
					BestT = d;
					pHealThreat = pC;
				}
			}
			// close and aiming at us? even then the enemy wins the race
			if(pHealThreat && BotTargetDistracted(pHealThreat, MyPos, Tick) == false &&
				BotLineOfSight(pGS, MyPos, pHealThreat->m_Pos))
				pHeal = 0; // fight first, rescue later
			else if(pHealThreat && distance(pHealThreat->m_Pos, pHeal->m_Pos) < 260.0f)
				pHeal = 0; // he is right on top of the rescue — deal with him
		}
	}

	// --- shooting ---
	bool Busy = m_BackoffTicks > 0 || JustReleased || Climbing || Boosting || pCarried || Swinging;
	bool WantFire = false;
	if(!Busy && !pMe->IsFrozen())
	{
		if(pHeal)
			WantFire = BotLineOfSight(pGS, MyPos, pHeal->m_Pos);
		else if(pTarget && !pTarget->IsFrozen())
		{
			float d = distance(MyPos, pTarget->m_Pos);
			// fng_trainbot: never shoot through a body. In FNG the bullet stops
			// on the first tee it meets, so a frozen teammate in the line eats
			// every shot and the bot wonders why nobody dies
			if(d > 20.0f && d < 780.0f && BotLineOfSight(pGS, MyPos, pTarget->m_Pos) &&
				!BotTeeInLine(pGS, MyPos, pTarget->m_Pos, ClientID, pTarget))
				WantFire = true;
		}
	}
	// fng_trainbot: the shot that leaves the instant a tee appears in front of
	// the barrel is the loudest tell of a bot — people hesitate, read the
	// movement, sometimes misclick. Only the laser suffers; a hooked prey must
	// not be dropped because the bot decided to think.
	if(WantFire && m_HoldFireTicks > 0)
		WantFire = false;
	// ... but hesitation is exactly what a person drops when the enemy turns
	// its back or gets busy with somebody else — that is the free hit
	if(!WantFire && pTarget && !pTarget->IsFrozen() && !Busy && !pMe->IsFrozen() &&
		BotTargetDistracted(pTarget, MyPos, Tick) &&
		distance(MyPos, pTarget->m_Pos) > 20.0f && distance(MyPos, pTarget->m_Pos) < 780.0f &&
		BotLineOfSight(pGS, MyPos, pTarget->m_Pos) &&
		!BotTeeInLine(pGS, MyPos, pTarget->m_Pos, ClientID, pTarget))
		WantFire = true;
	// fng_trainbot: the hammer throw overrides the hesitation — that swing is
	// the whole point of the current plan, and a missed one wastes the freeze
	if(HammerSwing)
	{
		WantFire = true;
		// fng_trainbot: one line per swing is what tells you whether the hammer
		// throw ever actually leaves the hands
		if(m_HammerMeasureTick == 0 && pPrey->GetPlayer())
		{
			m_HammerMeasureTick = Tick + 50;
			m_HammerMeasureCID = pPrey->GetPlayer()->GetCID();
			m_HammerMeasureFrom = pPrey->m_Pos;
		}
		if(g_Config.m_SvBotDebug && Tick - m_HammerSwingTick > 20)
		{
			m_HammerSwingTick = Tick;
			char aBuf[160];
			str_format(aBuf, sizeof(aBuf), "bot %d: HAMMER SWING at prey %.0f,%.0f -> cluster %d",
				ClientID, pPrey->m_Pos.x, pPrey->m_Pos.y, m_HammerIdx);
			pGS->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
		}
	}
	if(WantFire)
		m_FireState = (m_FireState + 1) & INPUT_STATE_MASK;
	else if(m_FireState & 1)
		m_FireState = (m_FireState + 1) & INPUT_STATE_MASK;
	Input.m_Fire = m_FireState;

	// --- aim: boost/climb anchors > the prey on our hook > heal > target ---
	vec2 Aim;
	bool HaveAim = false;
	if(Boosting)
	{
		Aim = m_BoostAnchor - MyPos;
		HaveAim = true;
	}
	else if(Climbing)
	{
		Aim = m_ClimbAnchor - MyPos;
		HaveAim = true;
	}
	else if(pCarried)
	{
		// fng_trainbot: the body is on our hook — look at him straight, no
		// error at all. The hook has to touch him within ~30px, so even a small
		// wobble means the body is dropped and the whole throw dies here.
		Aim = pCarried->m_Pos - MyPos;
		HaveAim = true;
	}
	else if(HammerSwing)
	{
		// fng_trainbot: the swing has to land dead on the body — the hammer
		// hit box is tiny, so no aim error at all here
		Aim = pPrey->m_Pos - MyPos;
		HaveAim = true;
	}
	else
	{
		CCharacter *pAim = pHeal ? pHeal : pTarget;
		if(pAim)
		{
			// fng_trainbot: a human does not shoot where the tee is, he shoots
			// where he thinks it will be. Good players lead the target, weak
			// ones shoot late, and everyone guesses a dodge now and then.
			vec2 A = pAim->m_Pos - MyPos;
			// ...but the moment we are going for the hook, the wobble has to go.
			// Aiming error of up to 20 degrees is a "human" laser shot and a
			// guaranteed miss for the rope, which is why no frozen victim was
			// ever actually caught.
			bool Hooking = pAim == pTarget && pTarget->IsFrozen() && !Boosting && !Climbing;
			if(pAim == pTarget && !pTarget->IsFrozen() && !Hooking)
			{
				float Lead = m_Lead;
				if(m_AimMode >= 2 && m_Predict > 0.3f)
					Lead += m_Predict * 4.0f * (frandom() < 0.5f ? -1.0f : 1.0f);
				A += pAim->GetVel() * (Lead - (1.0f - clamp(m_Skill, 0.0f, 1.0f)) * 6.0f);
			}
			Aim = Hooking ? A : AddAimError(A);
			HaveAim = true;
		}
		else if(HaveGoal)
		{
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
	// fng_trainbot: the engine only launches the rope on a *press* while the
	// hook is idle; holding the button leaves it retracted (state -1) and the
	// hook never fires again. That is why the log showed 'grab #1' and never a
	// '#2': one hold = one grab for the whole freeze. So the bot taps — release
	// one tick, press the next — and only holds the button while a rope is
	// actually out (flying or grabbed).
	const int HookState = pMe->GetHookState();
	const int HookedID = pMe->GetHookedPlayerID();
	const bool HookOnTee = HookState == HOOK_GRABBED && HookedID >= 0;
	if(WantHook)
	{
		// fng_trainbot: HOOK_GRABBED means "attached to *something*" — the engine
		// uses the same state for a body and for a wall, and only m_HookedPlayer
		// tells them apart. Holding the button on a wall grab kept the rope out
		// for good, and the bot could never fire again, which is why it stood
		// next to a frozen body and never picked him up: one unlucky hook into
		// the floor and the throw tactic was dead for that spawn. The button is
		// held only while the rope is in the air or on a player, and tapped
		// otherwise.
		if(HookState == HOOK_FLYING || HookOnTee)
			m_HookEmit = 1;
		else
			m_HookEmit = m_HookEmit ? 0 : 1; // tap: 1,0,1,0…
	}
	else
		m_HookEmit = 0;
	Input.m_Hook = m_HookEmit;

	// --- fng_trainbot: what to hold in hands. The hook is a button, not a
	// weapon, so this is about the gun: in SAH there is only the pistol (it
	// pushes enemies and thaws teammates), in plain fng2 the rifle is the
	// freezing tool, the hammer is what frees a teammate and the grenade is
	// the answer to somebody far above.
	int Want = WEAPON_GUN;
	// the hammer is the whole point of a hammer throw, so it is picked as soon
	// as the plan exists — not only when the body is already in reach, which
	// was the bug that made the tactic look broken. But not before either: in
	// FNG the rifle is the only thing that freezes anybody, and a bot that
	// picked up the hammer for the whole walk to the body simply stopped
	// contributing. So the hammer comes out for the approach, and no earlier.
	if(HammerTactic && PreyMine && PreyDist < 420.0f && pMe->HasWeapon(WEAPON_HAMMER))
		Want = WEAPON_HAMMER;
	// rescue next: in FNG a teammate is freed by hammering him, and that is a
	// guaranteed point for the team, so it outranks carrying the rifle
	else if(pHeal && pMe->HasWeapon(WEAPON_HAMMER))
		Want = WEAPON_HAMMER;
	else if(pMe->HasWeapon(WEAPON_RIFLE))
	{
		bool Far = pTarget && distance(MyPos, pTarget->m_Pos) > 700.0f &&
			pTarget->m_Pos.y < MyPos.y - 300.0f;
		if(Far && pMe->HasWeapon(WEAPON_GRENADE))
			Want = WEAPON_GRENADE; // lob it up at the ledge above
		else
			Want = WEAPON_RIFLE;
	}
	if(!pMe->HasWeapon(Want))
		Want = pMe->GetActiveWeapon();
	// fng_trainbot: the wire format is 1-based and 0 means "no request"
	// (CCharacter::HandleWeaponSwitch does `WantedWeapon = m_WantedWeapon-1`
	// behind an `if(m_LatestInput.m_WantedWeapon)` test). WEAPON_HAMMER is 0,
	// so sending the raw enum asked for "nothing" and the bot could never
	// take the hammer in his hands — it stood there swinging a rifle. Add one.
	if(Want != pMe->GetActiveWeapon())
		Input.m_WantedWeapon = Want + 1;

	// fng_trainbot: do not shove the mate we came to free, and do not stand
	// inside him either — walking through the teammate is the single most
	// annoying thing a bot can do in a team game
	if(pHeal)
	{
		float dxh = pHeal->m_Pos.x - MyPos.x;
		float ad = fabsf(dxh);
		// hammer reach is short: stop just short of him and swing
		if(ad > 46.0f)
			Dir = dxh > 0.0f ? 1 : -1;
		else
			Dir = 0;
	}

	// fng_trainbot: the trace you actually need when the bots stand still and
	// you cannot see why — where we are, where the goal is, and whether we
	// decided to move at all this tick
	// fng_trainbot: report how far one swing actually threw the body, so the
	// hammer plan can aim at clusters the fling can really reach (tuning aid)
	if(g_Config.m_SvBotDebug && m_HammerMeasureTick > 0 && Tick >= m_HammerMeasureTick)
	{
		m_HammerMeasureTick = 0;
		CPlayer *pMV = (m_HammerMeasureCID >= 0 && m_HammerMeasureCID < MAX_CLIENTS) ?
			pGS->m_apPlayers[m_HammerMeasureCID] : 0;
		CCharacter *pMVC = pMV ? pMV->GetCharacter() : 0;
		char aBuf[192];
		if(pMVC && pMVC->IsAlive())
			str_format(aBuf, sizeof(aBuf), "bot %d: hammer threw the body %.0fpx (%.0f,%.0f -> %.0f,%.0f)",
				ClientID, distance(m_HammerMeasureFrom, pMVC->m_Pos),
				m_HammerMeasureFrom.x, m_HammerMeasureFrom.y, pMVC->m_Pos.x, pMVC->m_Pos.y);
		else
			str_format(aBuf, sizeof(aBuf), "bot %d: the hammered body is gone", ClientID);
		pGS->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
	}

	if(g_Config.m_SvBotDebug && Tick % 25 == ClientID % 25)
	{
		char aBuf[320];
		str_format(aBuf, sizeof(aBuf),
			"bot %d t%d @%.0f,%.0f: goal %.0f,%.0f (%.0fpx) dir %d, hook %d%s%s, jump %d, grounded %d, act %s, target %d%s, hammer %d%s, w%d mine %d",
			ClientID, Tick, MyPos.x, MyPos.y, Goal.x, Goal.y, distance(MyPos, Goal), Dir,
			HookState,
			HookOnTee ? "=TEE" : (HookState == HOOK_FLYING ? "=flying" : (HookState == HOOK_GRABBED ? "=BLOCK!" : "=idle")),
			WantHook ? " WANT" : "",
			WantJump ? 1 : 0, pMe->IsGrounded() ? 1 : 0,
			CBotAI::ActionName(m_Action), m_TargetCID,
			pTarget && pTarget->IsFrozen() ? " FROZEN" : "", m_HammerIdx,
			HammerSwing ? " SWING" : "", pMe->GetActiveWeapon(), PreyMine ? 1 : 0);
		pGS->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
	}

	pGS->OnClientDirectInput(ClientID, &Input);
	pGS->OnClientPredictedInput(ClientID, &Input);
}