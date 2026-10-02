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
	static const char *StrategyName(int Strategy);
	enum { BOTSTRAT_HUNT = 0, BOTSTRAT_THROW, BOTSTRAT_RESCUE, BOTSTRAT_PATROL, BOTSTRAT_HOLD, NUM_BOTSTRATEGIES };
	// fng_trainbot: the tactical state used to be six bits packed into 96
	// buckets. Ninety-six boxes is the ceiling on everything the bot could ever
	// learn: a policy that understood "frozen enemy, 300px, on my floor, we are
	// holding" shared one number with "frozen enemy, 300px, wrong floor, we are
	// roaming". The Q-network now reads continuous features directly, so the
	// resolution is set by the feature list instead of by a bit budget.
	enum
	{
		NN_ENEMY_COUNT = 0,
		NN_NEAREST_DIST,
		NN_NEAREST_FROZEN,
		NN_FROZEN_ENEMIES,
		NN_FROZEN_MATES,
		NN_MY_HEALTH,
		NN_HOOK_GRABBED,
		NN_GROUNDED,
		NN_STRAT_HUNT,
		NN_STRAT_THROW,
		NN_STRAT_RESCUE,
		NN_STRAT_PATROL,
		NN_STRAT_HOLD,
		NN_HAS_TARGET,
		NN_TARGET_FROZEN,
		NN_TARGET_DIST,
		NN_TARGET_LOS,
		NN_TARGET_ON_MY_FLOOR,
		NN_SELF_FROZEN,
		NN_ENEMY_ABOVE,
		NN_ENEMY_BELOW,
		NN_ENEMY_DISTRACTED,
		NN_VALID_THROW,
		NN_TEAMMATES_ON_FLOOR,
		NN_FEATURE_COUNT
	};
	enum { NUM_NN_INPUTS = NN_FEATURE_COUNT, NUM_NN_HIDDEN = 12, NUM_NN_WEIGHTS = NUM_NN_INPUTS * NUM_NN_HIDDEN + NUM_NN_HIDDEN + NUM_NN_HIDDEN * NUM_BOTACTIONS + NUM_BOTACTIONS };
	enum { NUM_STRATEGY_INPUTS = 14, NUM_STRATEGY_WEIGHTS = NUM_STRATEGY_INPUTS * NUM_BOTSTRATEGIES + NUM_BOTSTRATEGIES };
	enum { NUM_CONTROL_INPUTS = 40, NUM_CONTROL_OUTPUTS = 16, NUM_CONTROL_WEIGHTS = NUM_CONTROL_INPUTS * NUM_CONTROL_OUTPUTS + NUM_CONTROL_OUTPUTS };
	// fng_trainbot: the target head. Choosing *which* enemy to attack used to be
	// a table of hand-written bonuses (900 for a frozen one while throwing, 450
	// for a live one while hunting, 2500 for being on another floor). Those
	// constants were my opinion, so the network could never disagree with it: at
	// best it could pick a strategy that routed around the opinion. Now every
	// candidate enemy is scored by a small network over observable features, and
	// the only rule that stays is the anti-pile-up filter, because four bots
	// dragging one body is a bug in the team, not a bad preference to be learned.
	enum
	{
		TGT_DIST = 0,
		TGT_FROZEN,
		TGT_LOS,
		TGT_SAME_FLOOR,
		TGT_ABOVE,
		TGT_BELOW,
		TGT_DISTRACTED,
		TGT_HIS_HEALTH,
		TGT_MY_HEALTH,
		TGT_SELF_FROZEN,
		TGT_ON_MY_HOOK,
		TGT_SPIKE_NEAR,
		TGT_MATES_ON_FLOOR,
		TGT_ENEMIES_ON_FLOOR,
		TGT_REACH,
		TGT_CONST,
		NUM_TARGET_FEATURES
	};
	enum { NUM_TARGET_INPUTS = NUM_TARGET_FEATURES, NUM_TARGET_HIDDEN = 12, NUM_TARGET_WEIGHTS = NUM_TARGET_INPUTS * NUM_TARGET_HIDDEN + NUM_TARGET_HIDDEN + NUM_TARGET_HIDDEN + 1 };
	// fng_trainbot: bump this whenever the shape of any head changes. A brain
	// file whose `ver` line does not match is ignored weight by weight, because
	// silently reading a set of 8-input weights into a 24-input network does not
	// fail loudly — it produces a network that looks trained and plays like noise.
	enum { BRAIN_VERSION = 3 };
	// fng_trainbot: what the counters mean, used by NoteStat
	enum { BOTSTAT_KILL = 0, BOTSTAT_DEATH, BOTSTAT_FREEZE, BOTSTAT_RESCUE, BOTSTAT_THROW, BOTSTAT_IDLE, NUM_BOTSTATS };

	int GetAction() const { return m_Action; }
	void EncodeNNFeatures(CGameContext *pGS, int ClientID, float *pInput) const;
	void UpdateQ(const float *pState, int Action, float Reward, const float *pNextState, bool Terminal, float Alpha);
	void EndEpisode(float Reward, float Alpha);
	float NNWeight(int Index) const;
	void SetNNWeight(int Index, float Value);
	float TargetWeight(int Index) const;
	void SetTargetWeight(int Index, float Value);
	void TargetForward(const float *pInput, float *pHidden, float *pValue) const;
	void LearnTarget(float Reward, float Rate);
	void NoteStat(int Stat, float Amount);
	void NoteReal(int Stat);
	void LogStats(CGameContext *pGS, int ClientID);
	// fng_trainbot: features of one candidate enemy, scored by the target head
	void EncodeTargetFeatures(CGameContext *pGS, int ClientID, CCharacter *pEnemy, float *pInput) const;
	float StrategyWeight(int Index) const;
	void SetStrategyWeight(int Index, float Value);
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
	int ChooseStrategy(CGameContext *pGS, int ClientID, float *pInput);
	void UpdateStrategyQ(const float *pInput, int Strategy, float Reward, float Alpha);
	void SetWeights(const float *pWeights);
	float Weight(int Action) const { return m_aWeights[Action]; }

private:
	void ForwardNN(const float *pInput, float *pHidden, float *pOutput) const;
	// fng_trainbot: pull saturated weights back inside the usable band
	void ClampNN();
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
	bool m_ThrowIsGround;  // teeth are at body level: walk him in, do not lift him
	int m_ThrowTick;       // next moment to re-plan the throw
	CCharacter *m_RegrabTarget; // victim we lost and must catch again
	int m_RegrabUntil;     // keep chasing the dropped body until this tick
	int m_LastPreyCID;     // whose body we were last dragging, -1 = none
	// fng_trainbot: the freeze is ten seconds and the rope only carries a body
	// for a fifth of that at a stretch. A drag that starts with four seconds
	// left is a drag that ends with the victim standing up, and an unfrozen tee
	// on the spikes is a self-kill worth nothing — which is what the log showed
	// ("frozen=0" on a body that had been carried for 381 ticks). So the bot
	// counts what is left of the freeze and lets go before the window shuts.
	int m_PreyFreezeLeft;  // ticks of freeze remaining on the current body
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
	float m_aStrategyWeights[NUM_STRATEGY_INPUTS][NUM_BOTSTRATEGIES];
	float m_aStrategyBias[NUM_BOTSTRATEGIES];
	float m_aLastStrategyInput[NUM_STRATEGY_INPUTS];
	float m_PendingReward;
	float m_aStateInput[NUM_NN_INPUTS];
	float m_aLastStateInput[NUM_NN_INPUTS];
	int m_LastAction;
	int m_LastStrategy;
	int m_Strategy;
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

	// fng_trainbot: target head — scores one candidate enemy per forward pass
	float m_aTargetInputHidden[NUM_TARGET_INPUTS][NUM_TARGET_HIDDEN];
	float m_aTargetHiddenBias[NUM_TARGET_HIDDEN];
	float m_aTargetHiddenValue[NUM_TARGET_HIDDEN];
	float m_aTargetValueBias;
	float m_aLastTargetInput[NUM_TARGET_INPUTS];
	float m_aLastTargetHidden[NUM_TARGET_HIDDEN];
	float m_LastTargetValue;
	int m_LastTargetCID;
	bool m_HasTargetTransition;

	// fng_trainbot: after seven hours of training the tactical network had
	// saturated every one of its 365 weights at the +-10 clamp, so its output no
	// longer depended on the state at all and every bot did the same thing. The
	// fixes are the ones the arithmetic actually calls for: rewards divided by a
	// fixed scale instead of being clamped at 100, the TD error clipped where a
	// single spike kill cannot move a weight by two units, and a decaying
	// learning rate so an old brain is still able to change its mind. These two
	// counters exist so the fix can be verified rather than assumed.
	int m_NNUpdates;      // successful TD updates, drives the rate decay
	int m_StrategyUpdates;
	float m_LastQError;   // |TD error| of the most recent update, for the log
	float m_StrategyBaseline; // running mean reward, the bandit baseline
	// fng_trainbot: reward earned by chasing the man we picked, flushed into the
	// target head when the current decision window closes
	float m_LastActionReward;
	// fng_trainbot: weapon-switch pacing. The cooldown is the minimum number of
	// ticks between two swaps; the counter is what the log prints so the fix can
	// be measured (it should be a few per minute, not a few per second).
	int m_WeaponCooldown;
	int m_WeaponSwitchCount;
	// fng_trainbot: the honest counters. Every previous fix to "they don't kill"
	// was a guess about what the bot was doing; these record what it actually
	// did, per tick, so the next one is a measurement.
	int m_LastHeldWeapon;
	int m_TicksFiring;      // ticks where the fire button was actually pressed
	int m_TicksAiming;      // ticks where a live enemy was inside the rifle line
	int m_TicksInLineOfSight;
	int m_TicksBlockedByTee; // enemy in front, but a frozen teammate in the way
	int m_TicksNoTarget;
	int m_TicksTargetFrozen; // the only enemy we can see is already frozen
	int m_IdleChargeTick;   // next debit for standing around while a game is on
	// fng_trainbot: the throw pipeline, counted. m_ThrowPreyTicks is how long a
	// frozen body was on the floor within reach of a plan; m_ThrowPlanOk is how
	// often a cluster satisfied the old "overhead only" geometry; and
	// m_ThrowSidewaysOk is how often plain same-level spikes — what a dragged
	// body actually runs into — were right there and ignored.
	int m_ThrowPreyTicks;
	int m_ThrowPlanOk;
	int m_ThrowSidewaysOk;
	int m_TicksCarried;
	int m_ThrowClustersAvailable;
	// fng_trainbot: the decisive measurement for the throw. While a body is on the
	// rope with a ground plan, does it actually get closer to the teeth? If these
	// two counts are near zero the body is not moving at all; if they are equal
	// it moves but not in the right direction; if closer wins and still no spike
	// kill lands, it is arriving somewhere else entirely.
	int m_DragCloser;
	int m_DragFarther;
	int m_DragStartGap;
	int m_DragMinGap;
	float m_LastDragGap;
	int m_HasLastDragGap;
	// fng_trainbot: throughput of the drag itself. Everything else has been
	// measured except this: how many pixels a body actually covers per tick
	// while it is on the rope. It decides the grab radius — pick too wide and
	// the body stands up halfway there, which is what the log kept showing.
	// Without this number the threshold is a guess dressed up as a constant.
	int m_DragTicks;
	int m_DragPx;
	int m_DragRuns;
	vec2 m_LastBodyPos;

	// fng_trainbot: reward-hacking check. Learning from my own reward function is
	// only safe if I can see the function's score separately from the game's. So
	// every credit event is counted twice: once in the hand-out points the bot is
	// trained on, once in what actually happened on the scoreboard. If the trained
	// total climbs while the scoreboard stays flat, the bot has learned to farm my
	// reward instead of winning, and these two lines are the only way to notice.
	float m_aStatReward[NUM_BOTSTATS];  // training signal handed out
	float m_aStatReal[NUM_BOTSTATS];    // what the game confirms
	int m_StatLastScore;
	int m_StatLastTick;
	int m_StatIdleTicks;
	bool m_HasStatBaseline;
};

#endif
