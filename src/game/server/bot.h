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

	// fng_trainbot: what this bot decided to do lately. Everything the bot
	// does belongs to one of these, and rewards are attached to it — that is
	// the whole idea: the bot learns which habit actually wins games.
	enum
	{
		BOTACT_HUNT = 0, // push the enemy, start the fight
		BOTACT_RESCUE,   // go free a frozen teammate
		BOTACT_THROW,    // drag the prey onto the spikes
		BOTACT_HOLD,     // hold a level and cover the team
		BOTACT_ROAM,     // walk the map looking for a game
		NUM_BOTACTIONS
	};
	static const char *ActionName(int Action);
	enum { NUM_QSTATES = 96, NUM_NN_INPUTS = 8, NUM_NN_HIDDEN = 12, NUM_NN_WEIGHTS = NUM_NN_INPUTS * NUM_NN_HIDDEN + NUM_NN_HIDDEN + NUM_NN_HIDDEN * NUM_BOTACTIONS + NUM_BOTACTIONS };
	enum { NUM_CONTROL_INPUTS = 40, NUM_CONTROL_OUTPUTS = 16, NUM_CONTROL_WEIGHTS = NUM_CONTROL_INPUTS * NUM_CONTROL_OUTPUTS + NUM_CONTROL_OUTPUTS };

	int GetAction() const { return m_Action; }
	int Observation(CGameContext *pGS, int ClientID) const;
	void UpdateQ(int State, int Action, float Reward, int NextState, bool Terminal, float Alpha);
	void EndEpisode(float Reward, float Alpha);
	float NNWeight(int Index) const;
	void SetNNWeight(int Index, float Value);
	float ControlWeight(int Index) const;
	void SetControlWeight(int Index, float Value);
	void ControlForward(const float *pInput, float *pOutput) const;
	void LearnControl(float Reward, float Rate);

	// fng_trainbot: current tactical target. Other bots use it to avoid
	// crowding the same frozen tee.
	int GetTargetCID() const { return m_TargetCID; }
	int GetFloorGoal() const { return m_FloorGoal; }
	// reward for what we were doing (Amount > 0 good, < 0 bad); update the
	// legacy tactic preference and the neural network's pending reward
	void RewardAction(CGameContext *pGS, int Action, float Amount);
	// pick a neural-network tactic that is currently possible
	int ChooseAction(CGameContext *pGS, int ClientID);
	void SetWeights(const float *pWeights);
	float Weight(int Action) const { return m_aWeights[Action]; }

private:
	void EncodeNNInput(int State, float *pInput) const;
	void ForwardNN(int State, float *pHidden, float *pOutput) const;
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
	int m_GrabCount;      // how many times we actually caught somebody (debug)
	// fng_trainbot: the engine only launches the rope when the hook button is
	// *pressed* while the hook is idle — holding it down leaves the rope in the
	// retracted state forever, which is why the very first grab used to be the
	// last one. This remembers what went into m_Hook last tick so the bot taps.
	int m_HookEmit;

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

	// fng_trainbot: swinging. The way a person crosses a map is not running, it
	// is hooking a spot ahead and above and steering through the pull until the
	// rope runs out, then doing it again. It is several times faster than the
	// ground, and it is what makes a bot arrive at a fight while it is still
	// being set up instead of ten seconds after it.
	int m_SwingTicks;      // ticks left of the current swing
	int m_SwingCooldown;   // ticks before the next one may start
	int m_SwingDir;        // which way the swing carries us, -1 / +1
	vec2 m_SwingAnchor;
	int m_SwingBestSpeed;  // speed along the swing, to know when it stops helping

	// fng_trainbot: patrol along the map's precomputed standable shelves
	int m_SelfCID;           // our own slot, used to give every bot its own flank
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
	bool m_ThrowLearned;   // this stand came from the throw book, not from a rule
	int m_ThrowTick;       // next moment to re-plan the throw
	CCharacter *m_RegrabTarget; // victim we lost and must catch again
	int m_RegrabUntil;     // keep chasing the dropped body until this tick
	int m_LastPreyCID;     // whose body we were last dragging, -1 = none
	void PlanThrow(CGameContext *pGS, vec2 MyPos, vec2 PreyPos, int MyTeam, int Tick);

	// fng_trainbot: hammering the frozen prey into the spikes. The hook drops a
	// body after ~1.25s, so dragging it all the way across a shelf often runs
	// out of time; the hammer instead throws the body a long way in a single
	// hit. The bot stands on the far side of the body and smashes it towards
	// the cluster — a second, much cheaper way to score in plain FNG.
	int m_HammerIdx;       // spike cluster to knock the body into, -1 = none
	int m_HammerVictim;    // cid of the body this plan is for, -1 = none
	int m_HammerTick;      // next moment to re-plan the hammer throw
	int m_HammerSwingTick; // last tick we logged an actual swing (debug)
	// fng_trainbot: measuring how far a real swing throws a body — the hammer
	// fling is what decides which clusters are worth aiming at
	int m_HammerMeasureTick;
	int m_HammerMeasureCID;
	vec2 m_HammerMeasureFrom;
	vec2 m_HammerStand;    // spot on the far side of the body
	void PlanHammer(CGameContext *pGS, vec2 MyPos, vec2 PreyPos, int MyTeam, int Tick, int VictimCID = -1);

	// fng_trainbot: the learned part. Every habit has a weight, rewards push
	// the weight of the habit that earned them up and the rest down, and the
	// table is saved so the bot plays better tomorrow than today.
	float m_aWeights[NUM_BOTACTIONS];
	float m_aNNInputHidden[NUM_NN_INPUTS][NUM_NN_HIDDEN];
	float m_aNNHiddenBias[NUM_NN_HIDDEN];
	float m_aNNHiddenOutput[NUM_NN_HIDDEN][NUM_BOTACTIONS];
	float m_aNNOutputBias[NUM_BOTACTIONS];
	float m_PendingReward;
	int m_LastState;
	int m_LastAction;
	bool m_HasTransition;
	int m_Action;          // tactical action selected by the learned policy
	int m_ActionTick;      // when that action expires
	int m_ActionRewardTick;
	bool m_BrainLoaded;
	float m_aControlWeights[NUM_CONTROL_WEIGHTS];
	float m_aLastControlInput[NUM_CONTROL_INPUTS];
	float m_aLastControlOutput[NUM_CONTROL_OUTPUTS];
	int m_aLastControlChoice[5]; // move, jump, fire, hook, weapon
	bool m_HasControlTransition;
};

#endif
