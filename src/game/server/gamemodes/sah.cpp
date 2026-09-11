/* SAH - Stole and Hook, based on fng2 (c) KeksTW */
#include "sah.h"
#include <game/mapitems.h>
#include "../entities/character.h"
#include "../player.h"
#include "../fng2define.h"
#include <engine/shared/config.h>
#include <string.h>
#include <stdio.h>

CGameControllerSAH::CGameControllerSAH(class CGameContext *pGameServer)
: CGameControllerFNG2((class CGameContext*)pGameServer)
{
	m_pGameType = "SAH";
	m_GameFlags = GAMEFLAG_TEAMS;

	m_Warmup = m_Config.m_SvWarmup;
}

CGameControllerSAH::CGameControllerSAH(class CGameContext *pGameServer, CConfiguration& pConfig)
: CGameControllerFNG2((class CGameContext*)pGameServer, pConfig)
{
	m_pGameType = "SAH";
	m_GameFlags = GAMEFLAG_TEAMS;

	m_Warmup = m_Config.m_SvWarmup;
}

void CGameControllerSAH::OnCharacterSpawn(class CCharacter *pChr)
{
	// default health
	pChr->IncreaseHealth(10);

	// SAH weapons: a hammer to smash frozen enemies into the spikes
	// and a toy gun that does nothing (variety). There is no laser,
	// the hook does the freezing.
	pChr->GiveWeapon(WEAPON_HAMMER, -1);
	pChr->GiveWeapon(WEAPON_GUN, 10);
}

bool CGameControllerSAH::OnEntity(int Index, vec2 Pos)
{
	// no weapon pickups and no ninja in SAH: the hook is the only freezing tool
	if(Index == ENTITY_WEAPON_RIFLE || Index == ENTITY_WEAPON_SHOTGUN || Index == ENTITY_WEAPON_GRENADE || Index == ENTITY_POWERUP_NINJA)
		return false;
	return CGameControllerFNG2::OnEntity(Index, Pos);
}

bool CGameControllerSAH::IsWrongSpike(int Team, int SpikeFlags)
{
	(void)Team;
	(void)SpikeFlags;
	// SAH has no wrong spike self freeze: mistakes are punished by the scoring only
	return false;
}

int CGameControllerSAH::SpikePlayerScore(int Weapon)
{
	switch(Weapon)
	{
	case WEAPON_SPIKE_GOLD: return m_Config.m_SvPlayerScoreSpikeGold;
	case WEAPON_SPIKE_GREEN: return m_Config.m_SvPlayerScoreSpikeGreen;
	case WEAPON_SPIKE_PURPLE: return m_Config.m_SvPlayerScoreSpikePurple;
	case WEAPON_SPIKE_RED:
	case WEAPON_SPIKE_BLUE: return m_Config.m_SvPlayerScoreSpikeTeam;
	default: return m_Config.m_SvPlayerScoreSpikeNormal;
	}
}

int CGameControllerSAH::SpikeTeamScore(int Weapon)
{
	switch(Weapon)
	{
	case WEAPON_SPIKE_GOLD: return m_Config.m_SvTeamScoreSpikeGold;
	case WEAPON_SPIKE_GREEN: return m_Config.m_SvTeamScoreSpikeGreen;
	case WEAPON_SPIKE_PURPLE: return m_Config.m_SvTeamScoreSpikePurple;
	case WEAPON_SPIKE_RED:
	case WEAPON_SPIKE_BLUE: return m_Config.m_SvTeamScoreSpikeTeam;
	default: return m_Config.m_SvTeamScoreSpikeNormal;
	}
}

int CGameControllerSAH::OnCharacterDeath(class CCharacter *pVictim, class CPlayer *pKiller, int Weapon)
{
	if(!pKiller || Weapon == WEAPON_GAME)
		return 0;

	if(Weapon == WEAPON_HAMMER)
	{
		// unfreeze event (hook rescue in SAH)
		pKiller->m_Stats.m_Unfreezes++;
	}
	else if(Weapon == WEAPON_RIFLE || Weapon == WEAPON_GRENADE)
	{
		// freeze events (hook freeze in SAH): stats only, no points
		if(IsTeamplay() && pVictim->GetPlayer()->GetTeam() == pKiller->GetTeam())
			pKiller->m_Stats.m_Teamkills++;
		else
		{
			pKiller->m_Stats.m_Kills++;
			pVictim->GetPlayer()->m_Stats.m_Hits++;
		}
	}
	else if(Weapon == WEAPON_SPIKE_NORMAL || Weapon == WEAPON_SPIKE_RED || Weapon == WEAPON_SPIKE_BLUE
		|| Weapon == WEAPON_SPIKE_GOLD || Weapon == WEAPON_SPIKE_GREEN || Weapon == WEAPON_SPIKE_PURPLE)
	{
		pVictim->GetPlayer()->m_Stats.m_Deaths++;
		pVictim->GetPlayer()->m_RespawnTick = Server()->Tick()+Server()->TickSpeed()*.5f;

		// defensive: a teammate cannot score on his own frozen mate
		// (the kill credit already points to the victim itself, handled like WEAPON_WORLD in DieSpikes)
		if(IsTeamplay() && pKiller->GetTeam() == pVictim->GetPlayer()->GetTeam())
			return 0;

		int PlayerScore = SpikePlayerScore(Weapon);
		int TeamScore = SpikeTeamScore(Weapon);

		// the player who hooked (froze) the victim
		CPlayer *pFreezer = 0;
		int FreezeOwnerID = pVictim->GetFreezeOwnerID();
		if(FreezeOwnerID >= 0 && FreezeOwnerID < MAX_CLIENTS)
			pFreezer = GameServer()->m_apPlayers[FreezeOwnerID];

		// was the victim thrown into the spikes of the killer's own team?
		bool OwnSpike = IsTeamplay() && ((pKiller->GetTeam() == TEAM_RED && Weapon == WEAPON_SPIKE_RED)
			|| (pKiller->GetTeam() == TEAM_BLUE && Weapon == WEAPON_SPIKE_BLUE));

		// did the killer freeze the victim himself?
		bool SelfThrow = (pFreezer == pKiller);

		if(SelfThrow || OwnSpike)
		{
			// duel fallback: a team with a single active player cannot be stolen from,
			// so his self-throws into enemy/neutral spikes are scored positive
			int OwnTeamPlayers = 0;
			for(int i = 0; i < MAX_CLIENTS; ++i)
				if(GameServer()->m_apPlayers[i] && GameServer()->m_apPlayers[i]->GetTeam() == pKiller->GetTeam())
					++OwnTeamPlayers;

			if(g_Config.m_SvSahDuelFallback && !OwnSpike && OwnTeamPlayers == 1)
			{
				// solo team: full points instead of a penalty
				pKiller->m_Stats.m_SahScoreDelta += PlayerScore;
				pKiller->m_Stats.m_Steals++;
				m_aTeamscore[pKiller->GetTeam()] += TeamScore;
				if(pKiller->GetCharacter()) GameServer()->MakeLaserTextPoints(pKiller->GetCharacter()->m_Pos, pKiller->GetCID(), PlayerScore);
			}
			else
			{
				// mistake: -n for the killer and his whole team pays for it
				pKiller->m_Stats.m_OwnThrows++;
				pKiller->m_Stats.m_SahScoreDelta -= PlayerScore;
				m_aTeamscore[pKiller->GetTeam()] -= TeamScore;
				if(pKiller->GetCharacter()) GameServer()->MakeLaserTextPoints(pKiller->GetCharacter()->m_Pos, pKiller->GetCID(), -PlayerScore);
			}
		}
		else
		{
			// steal: the kill goes to another player than the freezer
			int KillerScore = (PlayerScore * 6 + 5) / 10; // 60% of n, rounded
			int FreezerScore = PlayerScore - KillerScore; // 40% of n, rounded

			if(pFreezer && pFreezer != pKiller)
			{
				pFreezer->m_Stats.m_StealAssists++;
				pFreezer->m_Stats.m_SahScoreDelta += FreezerScore;
				if(pFreezer->GetCharacter())
					GameServer()->MakeLaserTextPoints(pFreezer->GetCharacter()->m_Pos, pFreezer->GetCID(), FreezerScore);
			}
			else
				KillerScore = PlayerScore; // the freezer left the game, the killer takes everything

			pKiller->m_Stats.m_Steals++;
			pKiller->m_Stats.m_SahScoreDelta += KillerScore;
			m_aTeamscore[pKiller->GetTeam()] += TeamScore;
			if(pKiller->GetCharacter()) GameServer()->MakeLaserTextPoints(pKiller->GetCharacter()->m_Pos, pKiller->GetCID(), KillerScore);
		}
	}

	if(Weapon == WEAPON_SELF)
		pVictim->GetPlayer()->m_RespawnTick = Server()->Tick()+Server()->TickSpeed()*.75f;
	else if (Weapon == WEAPON_WORLD)
		pVictim->GetPlayer()->m_RespawnTick = Server()->Tick()+Server()->TickSpeed()*.75f;

	return 0;
}