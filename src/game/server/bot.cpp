/* SAH: server-side practice bot — feeds CNetObj_PlayerInput for a slot
   that has no network client behind it (see CGameContext::CreateBot). */
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
	// sharp velocity changes (zigzag, jump spam) raise m_Predict, smooth
	// running lowers it again; the aim error below grows from that score
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

	// re-roll the aim error every few ticks; its radius grows quadratically
	// with the unpredictability — only a hard zigzag really makes the bot miss
	if(m_NoiseTick <= 0)
	{
		float MaxErr = m_Predict * m_Predict * 240.0f;
		float a = frandom() * 2.0f * pi;
		m_AimNoise = vec2(cos(a), sin(a)) * (frandom() * MaxErr);
		m_NoiseTick = 3 + (int)(frandom() * 3.0f);
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
	if(!pCarried)
		m_SpikeIdx = -1; // nothing to throw right now

	// --- spike goal for the throw (own/neutral spikes only) ---
	if(pCarried && m_SpikeIdx < 0)
	{
		float Best = 0.0f;
		for(int i = 0; i < pGS->m_NumBotSpikes; i++)
		{
			if(!BotValidSpikeForTeam(pGS->m_aBotSpikes[i].m_Flags, MyTeam))
				continue;
			float d = distance(pCarried->m_Pos, pGS->m_aBotSpikes[i].m_Pos);
			if(m_SpikeIdx < 0 || d < Best)
			{
				Best = d;
				m_SpikeIdx = i;
			}
		}
	}

	bool HaveSpike = pCarried && m_SpikeIdx >= 0;
	vec2 SpikePos = HaveSpike ? pGS->m_aBotSpikes[m_SpikeIdx].m_Pos : MyPos;

	// --- release (throw) timing: let the enemy slide into the spikes ---
	bool WantRelease = false;
	if(HaveSpike)
	{
		float dMe = distance(MyPos, SpikePos);
		vec2 ToSpike = SpikePos - pCarried->m_Pos;
		float dEnemy = length(ToSpike);
		float VelToward = 0.0f;
		if(dEnemy > 1.0f)
			VelToward = dot(pCarried->GetVel(), normalize(ToSpike));
		if(dEnemy < 380.0f && VelToward > 120.0f && dMe < 450.0f)
			WantRelease = true; // already flying towards the spikes
		else if(dMe < 170.0f)
			WantRelease = true; // too close to keep dragging safely
	}
	if(WantRelease)
	{
		// step back from the spikes while the hook retracts
		float Away = MyPos.x - SpikePos.x;
		if(Away > 4.0f) m_BackoffDir = -1;
		else if(Away < -4.0f) m_BackoffDir = 1;
		else m_BackoffDir = m_IdleDir;
		m_BackoffTicks = 40;
		m_SpikeIdx = -1;
		HaveSpike = false;
	}

	bool Backing = m_BackoffTicks > 0;

	// --- movement ---
	int Dir = 0;
	if(Backing)
	{
		m_BackoffTicks--;
		Dir = m_BackoffDir;
	}
	else if(HaveSpike)
	{
		float dx = SpikePos.x - MyPos.x;
		Dir = fabsf(dx) > 8.0f ? (dx > 0.0f ? 1 : -1) : 0;
	}
	else if(pCarried)
	{
		// carrying but no spike known: stay right above the prey
		float dx = pCarried->m_Pos.x - MyPos.x;
		Dir = fabsf(dx) > 40.0f ? (dx > 0.0f ? 1 : -1) : 0;
	}
	else if(pTarget)
	{
		vec2 d = pTarget->m_Pos - MyPos;
		float dist = length(d);
		// stand ground close enough to hook/shoot, otherwise close in
		if(dist > 260.0f && fabsf(d.x) > 12.0f)
			Dir = d.x > 0.0f ? 1 : -1;
		else
			Dir = 0;
	}
	else
	{
		// nobody to fight: wander
		if(--m_IdleTicks <= 0)
		{
			m_IdleTicks = 100;
			m_IdleDir = -m_IdleDir;
		}
		Dir = m_IdleDir;
	}

	// never walk into spikes: probe the ground ahead of the chosen direction
	if(Dir != 0)
	{
		int f = pGS->Collision()->GetCollisionAt(MyPos.x + Dir * 70.0f, MyPos.y);
		int f2 = pGS->Collision()->GetCollisionAt(MyPos.x + Dir * 70.0f, MyPos.y - 20.0f);
		if((f | f2) & BOT_DANGER_MASK)
			Dir = pCarried ? 0 : -Dir; // carrying: stop short, else pace back
	}

	// --- stuck detection: hop over small obstacles ---
	if(Tick - m_StuckTick >= 16)
	{
		float Moved = distance(MyPos, m_LastPos);
		if(Dir != 0 && Moved < 10.0f && m_JumpCooldown <= 0 && !pMe->IsFrozen())
		{
			m_JumpTicks = 12;    // hold long enough for exactly one hop
			m_JumpCooldown = 45; // then a release window before the next one
		}
		m_LastPos = MyPos;
		m_StuckTick = Tick;
	}
	bool WantJump = m_JumpTicks > 0;
	if(m_JumpTicks > 0) m_JumpTicks--;
	if(m_JumpCooldown > 0) m_JumpCooldown--;

	// --- hook: keep the hold while carrying, otherwise chase with it ---
	bool JustReleased = WantRelease;
	bool WantHook = false;
	if(!Backing && !JustReleased)
	{
		if(pCarried)
			WantHook = true;
		else if(pTarget)
		{
			float d = distance(MyPos, pTarget->m_Pos);
			if(d < 750.0f && BotLineOfSight(pGS, MyPos, pTarget->m_Pos))
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

	// --- shooting: live enemies only (never shoot frozen prey), heal in SAH,
	//     nothing at all while dragging so the throw is not disturbed ---
	bool WantFire = false;
	if(!Backing && !JustReleased && !pMe->IsFrozen())
	{
		if(pHeal)
			WantFire = BotLineOfSight(pGS, MyPos, pHeal->m_Pos);
		else if(!pCarried && pTarget && !pTarget->IsFrozen())
		{
			float d = distance(MyPos, pTarget->m_Pos);
			if(d > 150.0f && d < 850.0f && BotLineOfSight(pGS, MyPos, pTarget->m_Pos))
				WantFire = true;
		}
	}
	if(WantFire)
		m_FireState = (m_FireState + 1) & INPUT_STATE_MASK;
	else if(m_FireState & 1)
		m_FireState = (m_FireState + 1) & INPUT_STATE_MASK; // land on even so fullauto stops
	Input.m_Fire = m_FireState;

	// --- aim: heal > carried prey > target, with a small lead ---
	CCharacter *pAim = pHeal ? pHeal : (pCarried ? pCarried : pTarget);
	if(pAim)
	{
		vec2 Aim = pAim->m_Pos + pAim->GetVel() * 0.15f - MyPos;
		// fng_trainbot: against the enemy the aim carries the zigzag error,
		// healing and dragging stay precise
		if(pTarget && pAim == pTarget)
			Aim += m_AimNoise;
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