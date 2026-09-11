/* SAH - Stole and Hook, based on fng2 (c) KeksTW */
#ifndef GAME_SERVER_GAMEMODES_SAH_H
#define GAME_SERVER_GAMEMODES_SAH_H

#include <game/server/gamecontroller.h>
#include "fng2.h"
#include <base/vmath.h>

class CGameControllerSAH : public CGameControllerFNG2
{
public:
	CGameControllerSAH(class CGameContext* pGameServer);
	CGameControllerSAH(class CGameContext* pGameServer, CConfiguration& pConfig);

	virtual void OnCharacterSpawn(class CCharacter *pChr);
	virtual int OnCharacterDeath(class CCharacter *pVictim, class CPlayer *pKiller, int Weapon);
	virtual bool OnEntity(int Index, vec2 Pos);
	virtual bool IsWrongSpike(int Team, int SpikeFlags);

	virtual bool UsesSahScoring() const { return true; }

protected:
	//standard fng spike score values (player/team) for a spike weapon type
	int SpikePlayerScore(int Weapon);
	int SpikeTeamScore(int Weapon);
};
#endif