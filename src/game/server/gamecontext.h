/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_SERVER_GAMECONTEXT_H
#define GAME_SERVER_GAMECONTEXT_H

#include <engine/server.h>
#include <engine/console.h>
#include <engine/shared/memheap.h>

#include <game/layers.h>
#include <game/voting.h>

#include "eventhandler.h"
#include "gamecontroller.h"
#include "gameworld.h"
#include "player.h"
#include "bot.h"

#include <string>

#ifndef QUADRO_MASK
#define QUADRO_MASK
struct QuadroMask {
	long long m_Mask[4];
	QuadroMask(long long mask) {
		memset(m_Mask, mask, sizeof(m_Mask));
	}
	QuadroMask() {}
	QuadroMask(long long Mask, int id) {
		memset(m_Mask, 0, sizeof(m_Mask));
		m_Mask[id] = Mask;
	}

	void operator|=(const QuadroMask& mask) {
		m_Mask[0] |= mask[0];
		m_Mask[1] |= mask[1];
		m_Mask[2] |= mask[2];
		m_Mask[3] |= mask[3];
	}

	long long& operator[](int id) {
		return m_Mask[id];
	}

	long long operator[](int id) const {
		return m_Mask[id];
	}

	QuadroMask operator=(long long mask) {
		memset(m_Mask, mask, sizeof(m_Mask));
		return *this;
	}

	long long operator & (const QuadroMask& mask){
		return (m_Mask[0] & mask[0]) | (m_Mask[1] & mask[1]) | (m_Mask[2] & mask[2]) | (m_Mask[3] & mask[3]);
	}	

	QuadroMask& operator^(long long mask) {
		m_Mask[0] ^= mask;
		m_Mask[1] ^= mask;
		m_Mask[2] ^= mask;
		m_Mask[3] ^= mask;
		return *this;
	}
	
	bool operator==(QuadroMask& q){
		return m_Mask[0] == q.m_Mask[0] && m_Mask[1] == q.m_Mask[1] && m_Mask[2] == q.m_Mask[2] && m_Mask[3] == q.m_Mask[3];
	}

	int Count() {
		int Counter = 0;
		for (int i = 0; i < 4; ++i) {
			for (int n = 0; n < 64; ++n) {
				if ((m_Mask[i] & (1ll << n)) != 0)
					++Counter;
			}
		}

		return Counter;
	}

	int PositionOfNonZeroBit(int Offset) {
		for (int i = (Offset / 64); i < 4; ++i) {
			for (int n = (Offset % 64); n < 64; ++n) {
				if ((m_Mask[i] & (1ll << n)) != 0) {
					return i * 64 + n;
				}
			}
		}
		return -1;
	}
	
	void SetBitOfPosition(int Pos){
		m_Mask[Pos / 64] |= 1 << (Pos % 64);
	}
};
#endif

//str_comp_nocase_whitespace
//IMPORTANT: the pArgs can not be accessed by null zero termination. they are not splited by 0, but by space... in case a function needs the whole argument at once. use functions above
typedef void (*ServerCommandExecuteFunc)(class CGameContext* pContext, int pClientID, const char** pArgs, int ArgNum);

struct sServerCommand{
	const char* m_Cmd;
	const char* m_Desc;
	const char* m_ArgFormat;
	sServerCommand* m_NextCommand;
	ServerCommandExecuteFunc m_Func;
	
	sServerCommand(const char* pCmd, const char* pDesc, const char* pArgFormat, ServerCommandExecuteFunc pFunc) : m_Cmd(pCmd), m_Desc(pDesc), m_ArgFormat(pArgFormat), m_NextCommand(0), m_Func(pFunc) {}
	
	void ExecuteCommand(class CGameContext* pContext, int pClientID, const char* pArgs){	
		const char* m_Args[128/2];
		int m_ArgCount = 0;
		
		const char* c = pArgs;
		const char* s = pArgs;
		while(c && *c){
			if(is_whitespace(*c)){
				m_Args[m_ArgCount++] = s;
				s = c + 1;
			}
			++c;
		}
		if (s) {
			m_Args[m_ArgCount++] = s;
		}
		
		m_Func(pContext, pClientID, m_Args, m_ArgCount);
	}
};

/*
	Tick
		Game Context (CGameContext::tick)
			Game World (GAMEWORLD::tick)
				Reset world if requested (GAMEWORLD::reset)
				All entities in the world (ENTITY::tick)
				All entities in the world (ENTITY::tick_defered)
				Remove entities marked for deletion (GAMEWORLD::remove_entities)
			Game Controller (GAMECONTROLLER::tick)
			All players (CPlayer::tick)


	Snap
		Game Context (CGameContext::snap)
			Game World (GAMEWORLD::snap)
				All entities in the world (ENTITY::snap)
			Game Controller (GAMECONTROLLER::snap)
			Events handler (EVENT_HANDLER::snap)
			All players (CPlayer::snap)

*/
class CGameContext : public IGameServer
{
	IServer *m_pServer;
	class IConsole *m_pConsole;
	CLayers m_Layers;
	CCollision m_Collision;
	CNetObjHandler m_NetObjHandler;
	CTuningParams m_Tuning;

	static void ConTuneParam(IConsole::IResult *pResult, void *pUserData);
	static void ConTuneReset(IConsole::IResult *pResult, void *pUserData);
	static void ConTuneDump(IConsole::IResult *pResult, void *pUserData);
	static void ConPause(IConsole::IResult *pResult, void *pUserData);
	static void ConChangeMap(IConsole::IResult *pResult, void *pUserData);
	static void ConRestart(IConsole::IResult *pResult, void *pUserData);
	static void ConBroadcast(IConsole::IResult *pResult, void *pUserData);
	static void ConSay(IConsole::IResult *pResult, void *pUserData);
	static void ConSetTeam(IConsole::IResult *pResult, void *pUserData);
	static void ConSetTeamAll(IConsole::IResult *pResult, void *pUserData);
	static void ConSwapTeams(IConsole::IResult *pResult, void *pUserData);
	static void ConShuffleTeams(IConsole::IResult *pResult, void *pUserData);
	static void ConLockTeams(IConsole::IResult *pResult, void *pUserData);
	static void ConAddVote(IConsole::IResult *pResult, void *pUserData);
	static void ConRemoveVote(IConsole::IResult *pResult, void *pUserData);
	static void ConForceVote(IConsole::IResult *pResult, void *pUserData);
	static void ConClearVotes(IConsole::IResult *pResult, void *pUserData);
	static void ConVote(IConsole::IResult *pResult, void *pUserData);
	static void ConchainSpecialMotdupdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);

	CGameContext(int Resetting);
	CGameContext(int Resetting, CConfiguration* pConfig);
	void Construct(int Resetting);

	bool m_Resetting;
	
	sServerCommand* FindCommand(const char* pCmd);
	void AddServerCommandSorted(sServerCommand* pCmd);
public:
	sServerCommand* m_FirstServerCommand;
	void AddServerCommand(const char* pCmd, const char* pDesc, const char* pArgFormat, ServerCommandExecuteFunc pFunc);
	void ExecuteServerCommand(int pClientID, const char* pLine);
	
	static void CmdStats(CGameContext* pContext, int pClientID, const char** pArgs, int ArgNum);
	static void CmdWhisper(CGameContext* pContext, int pClientID, const char** pArgs, int ArgNum);
	static void CmdConversation(CGameContext* pContext, int pClientID, const char** pArgs, int ArgNum);
	static void CmdHelp(CGameContext* pContext, int pClientID, const char** pArgs, int ArgNum);
	static void CmdStatus(CGameContext* pContext, int pClientID, const char** pArgs, int ArgNum);
	static void CmdEmote(CGameContext* pContext, int pClientID, const char** pArgs, int ArgNum);
	static void CmdSelfKillProtect(CGameContext* pContext, int pClientID, const char** pArgs, int ArgNum);
	static void CmdPause(CGameContext* pContext, int pClientID, const char** pArgs, int ArgNum);
	static void CmdSpec(CGameContext* pContext, int pClientID, const char** pArgs, int ArgNum);
	
	IServer *Server() const { return m_pServer; }
	class IConsole *Console() { return m_pConsole; }
	CCollision *Collision() { return &m_Collision; }
	CTuningParams *Tuning() { return &m_Tuning; }

	CGameContext();
	~CGameContext();

	void Clear();

	CEventHandler m_Events;
	CPlayer *m_apPlayers[MAX_CLIENTS];

	// SAH: server-side practice bot (slot without a network client)
	bool m_aIsBot[MAX_CLIENTS];
	class CBotAI m_aBotAI[MAX_CLIENTS];
	int CreateBot();
	void RemoveBot(int ClientID, bool Announce);
	void CreateConfiguredBots();
	void CleanupBotsOnInit();
	void TickBots();

	// fng_trainbot: bot gloats in chat right after it froze someone (sv_bot_taunt);
	// per-bot cooldown so a busy bot never floods the chat
	int m_aBotTauntTick[MAX_CLIENTS];
	void BotTauntOnFreeze(int FreezerCID, int VictimCID);

	// fng_trainbot: rewards and the brain behind them. Every bot keeps a weight
	// per habit, the result of a fight is credited to the habit that was
	// running, and the table is written to disk so the bots keep learning
	// between restarts.
	void LoadBotBrains();
	void SaveBotBrains();
	void BotRewardKill(int KillerCID, int VictimCID, int Weapon);
	void BotRewardRescue(int RescuerCID, int VictimCID);
	void BotRewardFreeze(int FreezerCID, int VictimCID);
	int m_BotBrainSaveTick;

	// SAH: precomputed spike tiles for bot throw navigation
	enum { MAX_BOT_SPIKES = 2048 };
	struct CBotSpike
	{
		vec2 m_Pos;
		int m_Flags;
	};
	CBotSpike m_aBotSpikes[MAX_BOT_SPIKES];
	int m_NumBotSpikes;
	void CollectBotSpikes();

	// fng_trainbot: standable shelves of the map, for patrol navigation
	enum { MAX_BOT_NAV = 768 };
	struct CBotNavPoint
	{
		vec2 m_Pos;
		signed char m_Band;   // 0 = low, 1 = mid (fight zone), 2 = high
		signed char m_Side;   // -1 = left (red), 0 = centre, 1 = right (blue)
		signed char m_Floor;  // index into m_aBotFloors, -1 = unclustered
	};
	CBotNavPoint m_aBotNav[MAX_BOT_NAV];
	int m_NumBotNav;
	void CollectBotNav();

	// fng_trainbot: the map's "этажи" — nav points clustered by height. The bot
	// hunts and patrols whole floors instead of stumbling over random tiles.
	enum { MAX_BOT_FLOORS = 24 };
	struct CBotFloor
	{
		float m_TopY, m_BottomY; // world Y span of the walkable surface
		float m_MinX, m_MaxX;    // horizontal extent of the floor
		int m_NumPoints;
	};
	CBotFloor m_aBotFloors[MAX_BOT_FLOORS];
	int m_NumBotFloors;
	void CollectBotFloors();
	int BotFloorAt(vec2 Pos) const;
	int BotNavPointOnFloor(int Floor, vec2 Near, int Spread, vec2 *pOut) const;

	// fng_trainbot: spike clusters, one per throw target. A victim on the hook
	// drifts towards the hooker, so a "throw" is pure geometry: stand where the
	// cluster lands on the line prey -> bot and reel in.
	enum { MAX_BOT_THROW_TARGETS = 128 };
	struct CBotThrowTarget
	{
		vec2 m_Pos;      // cluster centre
		int m_Flags;     // spike colour flags of the cluster
		int m_Count;     // spike tiles in the cluster
		float m_Radius;  // half diagonal: how close the drag line must pass
		int m_Floor;     // floor the cluster belongs to, -1 = none
	};
	CBotThrowTarget m_aBotThrowTargets[MAX_BOT_THROW_TARGETS];
	int m_NumBotThrowTargets;
	void CollectBotThrowTargets();

	// fng_trainbot: throw telemetry — see sv_bot_throwlog. One entry per victim,
	// so the whole server (bots and humans alike) is watched without touching
	// either of them. A throw is "somebody's rope is on a frozen enemy".
	struct CThrowTrace
	{
		int m_Thrower;    // cid holding the rope, -1 = idle
		int m_StartTick;
		vec2 m_PreyStart;  // where the victim lay when the rope went taut
		vec2 m_BotStart;   // where the thrower stood at that moment
		vec2 m_BotEnd;     // where he finished the drag
		vec2 m_PreyEnd;    // where the victim came to rest / died
		int m_Cluster;     // nearest cluster at the start, -1 = none
		float m_Reach;     // how close the victim got to that cluster
		bool m_Active;
	};
	CThrowTrace m_aThrowTrace[MAX_CLIENTS];
	void UpdateThrowTraces();
	int NearestThrowTarget(vec2 Pos, float *pDist) const;

	// fng_trainbot: the throw book. Every completed throw on the server — a
	// person's or a bot's — is filed here with its result, and the bots plan
	// their own throws out of this table instead of out of a rule written by
	// hand. The file outlives the process, so a server that gets played keeps
	// getting better at throwing, and nobody has to teach it anything again.
	//
	// The cluster is stored as a *position*, not as an index, so a change of
	// map cannot turn the table into nonsense: at use time the position is
	// resolved back to whatever cluster happens to be there now.
	enum { MAX_BOT_THROW_RECIPES = 512 };
	struct CThrowRecipe
	{
		vec2 m_Cluster;  // where the teeth are
		vec2 m_Prey;     // where the body was lying
		vec2 m_Stand;    // where the thrower stood
		int m_Hits;      // how often this ended in the teeth
		int m_Tries;
	};
	CThrowRecipe m_aThrowRecipes[MAX_BOT_THROW_RECIPES];
	int m_NumBotThrowRecipes;
	int m_BotThrowBookSaveTick;
	void LoadThrowBook();
	void SaveThrowBook();
	void NoteThrowRecipe(vec2 Cluster, vec2 Prey, vec2 Stand, bool Hit);
	// the stand a player used for a throw like this one. Returns false when
	// nothing similar is on file, and the caller falls back to its own rule.
	bool BotRecipeStand(vec2 Cluster, vec2 Prey, vec2 *pOut) const;
	// how often throws at this cluster actually land, 0.0..1.0
	float BotClusterHitRate(vec2 Cluster) const;

	// fng_trainbot: what every player was actually doing, sampled 12 times a
	// second. This is the raw material for teaching the bots. The recorder does
	// not care who is holding the controls, so a person and a bot produce the
	// same record: when a person walks somebody into the nastiest corner of the
	// spikes, the four seconds of input that got him there are on disk, and the
	// bot's planner can copy that shape instead of guessing from the tile grid.
	// Every outcome (spike kill, rescue, freeze, death) flushes the window with
	// a reward attached, so the good play and the bad play can be told apart.
	enum { MAX_ACTION_FRAMES = 48 }; // 48 * 4 ticks = ~3.8s of history
	struct CActionFrame
	{
		short m_PosX, m_PosY;
		short m_VelX, m_VelY;
		signed char m_Dir;     // -1 / 0 / +1
		signed char m_Jump;    // 0 / 1
		signed char m_Hook;    // 0 / 1 — the rope button
		signed char m_Fire;    // 0 / 1
		signed char m_Weapon;  // 0 hammer, 1 gun, 3 grenade, 4 rifle
		signed char m_Flags;   // 1 grounded, 2 frozen, 4 dragging a body
	};
	CActionFrame m_aAction[MAX_CLIENTS][MAX_ACTION_FRAMES];
	int m_aActionHead[MAX_CLIENTS];  // next slot to write
	int m_aActionCount[MAX_CLIENTS]; // how many are valid
	void RecordActionFrames();
	// pType/pExtra/describe the moment (e.g. "SPIKE", cluster=12). The last
	// few seconds of that player's input are written out with the reward, so
	// one block per event is a ready-made training example.
	void ExportActionTrace(int CID, const char *pType, int Extra, float Reward);
	// somebody was just frozen: file the shooter's input under FREEZE. Off by
	// default because freezing is the most frequent event in FNG by far
	void BotNoteFreeze(int FreezerCID, int VictimCID);
	// the position somebody died at, remembered for the trace export
	vec2 m_aLastDeathPos[MAX_CLIENTS];
	int m_aLastDeathCluster[MAX_CLIENTS];


	// SAH: spike-death melting ring animation state (per victim, position is static)
	struct CDeathAnim
	{
		bool m_Active;
		vec2 m_Pos;
		int m_StartTick;
	};
	CDeathAnim m_aDeathAnims[MAX_CLIENTS];

	IGameController *m_pController;
	CGameWorld m_World;

	// helper functions
	class CCharacter *GetPlayerChar(int ClientID);

	int m_LockTeams;

	// voting
	void StartVote(const char *pDesc, const char *pCommand, const char *pReason);
	void EndVote();
	void SendVoteSet(int ClientID);
	void SendVoteStatus(int ClientID, int Total, int Yes, int No);
	void AbortVoteKickOnDisconnect(int ClientID);

	int m_VoteCreator;
	int64 m_VoteCloseTime;
	bool m_VoteUpdate;
	int m_VotePos;
	char m_aVoteDescription[VOTE_DESC_LENGTH];
	char m_aVoteCommand[VOTE_CMD_LENGTH];
	char m_aVoteReason[VOTE_REASON_LENGTH];
	int m_NumVoteOptions;
	int m_VoteEnforce;
	enum
	{
		VOTE_ENFORCE_UNKNOWN=0,
		VOTE_ENFORCE_NO,
		VOTE_ENFORCE_YES,
	};
	CHeap *m_pVoteOptionHeap;
	CVoteOptionServer *m_pVoteOptionFirst;
	CVoteOptionServer *m_pVoteOptionLast;

	// helper functions
	void MakeLaserTextPoints(vec2 pPos, int pOwner, int pPoints);
	void MakeLaserTextFreeze(vec2 pPos, int pOwner, int pSeconds);
	
	void CreateDamageInd(vec2 Pos, float AngleMod, int Amount, int Team, int FromPlayerID = -1);
	void CreateDamageIndForClient(vec2 Pos, float Angle, int Amount, int ClientID);
	void CreateDamageIndMasked(vec2 Pos, float Angle, int Amount, QuadroMask Mask);
	void CreateSahDeathAnim(vec2 Pos, int ClientID);
	void TickDeathAnims();
	void CreateSoundTeam(vec2 Pos, int Sound, int TeamID, int FromPlayerID = -1);

	void CreateExplosion(vec2 Pos, int Owner, int Weapon, bool NoDamage, QuadroMask Mask=QuadroMask(-1ll));
	void CreateHammerHit(vec2 Pos);
	void CreatePlayerSpawn(vec2 Pos);
	void CreatePlayerSpawnForClient(vec2 Pos, int ClientID);
	void CreateSahFreezeMarker(vec2 Pos, int ClientID);
	void CreateDeath(vec2 Pos, int Who);
	void CreateSound(vec2 Pos, int Sound, QuadroMask Mask=QuadroMask(-1ll));
	void CreateSoundGlobal(int Sound, int Target=-1);


	enum
	{
		CHAT_ALL=-2,
		CHAT_SPEC=-1,
		CHAT_RED=0,
		CHAT_BLUE=1,
		//for ddnet client only
		CHAT_WHISPER_SEND=2,
		CHAT_WHISPER_RECV=3,
	};

	// network
	void SendChatTarget(int To, const char *pText);
	void SendChat(int ClientID, int Team, const char *pText, int To = -1);
	void SendEmoticon(int ClientID, int Emoticon);
	void SendWeaponPickup(int ClientID, int Weapon);
	void SendBroadcast(const char *pText, int ClientID);


	//
	void CheckPureTuning();
	void SendTuningParams(int ClientID);
	void SendFakeTuningParams(int ClientID);

	//
	void SwapTeams();

	// engine events
	virtual void OnInit();
	virtual void OnInit(class IKernel *pKernel, class IMap* pMap, struct CConfiguration* pConfigFile = 0);
	virtual void OnConsoleInit();
	virtual void OnShutdown();

	virtual int PreferedTeamPlayer(int ClientID);

	virtual void OnTick();
	virtual void OnPreSnap();
	virtual void OnSnap(int ClientID);
	virtual void OnPostSnap();

	virtual void OnMessage(int MsgID, CUnpacker *pUnpacker, int ClientID);

	virtual void OnClientConnected(int ClientID, int PreferedTeam = -2);
	virtual void OnClientEnter(int ClientID);
	virtual bool OnClientDrop(int ClientID, const char *pReason, bool Force);
	virtual void OnClientDirectInput(int ClientID, void *pInput);
	virtual void OnClientPredictedInput(int ClientID, void *pInput);

	virtual bool IsClientReady(int ClientID);
	virtual bool IsClientPlayer(int ClientID);

	virtual const char *GameType();
	virtual const char *Version();
	virtual const char *NetVersion();

	// fng_trainbot: bots on a team — the engine uses this to keep ticking the
	// world when the server runs bots without a single network client
	virtual int NumBots() const;

	void SendRoundStats();
	void SendRandomTrivia();

	template<class T>
	int SendPackMsg(T *pMsg, int Flags)
	{
		for (int i = 0; i < MAX_CLIENTS; ++i) {
			CPlayer* p = m_apPlayers[i];
			if (!p || p->m_IsBot) continue; // bot slots have no engine connection
			Server()->SendPackMsg(pMsg, Flags, i);
		}
		return 0;
	}

	int SendPackMsg(CNetMsg_Sv_KillMsg *pMsg, int Flags);

	int SendPackMsg(CNetMsg_Sv_Emoticon *pMsg, int Flags);

	int SendPackMsg(CNetMsg_Sv_Chat *pMsg, int Flags);

	int SendPackMsg(CNetMsg_Sv_Chat *pMsg, int Flags, int ClientID);
};

inline QuadroMask CmaskAll() { return QuadroMask(-1); }
inline QuadroMask CmaskOne(int ClientID) { return QuadroMask(1ll<<(ClientID%(sizeof(long long)*8)), (ClientID/(sizeof(long long)*8))); }
inline QuadroMask CmaskAllExceptOne(int ClientID) { return CmaskOne(ClientID)^0xffffffffffffffffll; }
inline bool CmaskIsSet(QuadroMask Mask, int ClientID) { return (Mask&CmaskOne(ClientID)) != 0; }
#endif
