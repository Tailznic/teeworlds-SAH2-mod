MACRO_CONFIG_INT(SvHammerScaleX, sv_hammer_scale_x, 320, 1, 1000, CFGFLAG_SERVER, "linearly scale up hammer x power, percentage, for hammering enemies and unfrozen teammates")
MACRO_CONFIG_INT(SvHammerScaleY, sv_hammer_scale_y, 120, 1, 1000, CFGFLAG_SERVER, "linearly scale up hammer y power, percentage, for hammering enemies and unfrozen teammates")
MACRO_CONFIG_INT(SvMeltHammerScaleX, sv_melt_hammer_scale_x, 50, 1, 1000, CFGFLAG_SERVER, "linearly scale up hammer x power, percentage, for hammering frozen teammates")
MACRO_CONFIG_INT(SvMeltHammerScaleY, sv_melt_hammer_scale_y, 50, 1, 1000, CFGFLAG_SERVER, "linearly scale up hammer y power, percentage, for hammering frozen teammates")

MACRO_CONFIG_INT(SvScoreDisplay, sv_score_display, 0, 0, 1, CFGFLAG_SERVER, "0: Normal score display, 1: Calculates the deaths, etc. to the score points")

MACRO_CONFIG_INT(SvTeamScoreSpikeNormal, sv_team_score_normal, 5, 0, 100, CFGFLAG_SERVER, "Points a team receives for grabbing into normal spikes")
MACRO_CONFIG_INT(SvTeamScoreSpikeTeam, sv_team_score_team, 10, 0, 100, CFGFLAG_SERVER, "Points a team receives for grabbing into team spikes")
MACRO_CONFIG_INT(SvTeamScoreSpikeGold, sv_team_score_gold, 10, 0, 100, CFGFLAG_SERVER, "Points a team receives for grabbing into golden spikes")
MACRO_CONFIG_INT(SvTeamScoreSpikeGreen, sv_team_score_green, 10, 0, 100, CFGFLAG_SERVER, "Points a team receives for grabbing into green spikes(non 4-teams fng only)")
MACRO_CONFIG_INT(SvTeamScoreSpikePurple, sv_team_score_purple, 10, 0, 100, CFGFLAG_SERVER, "Points a team receives for grabbing into purple spikes(non 4-teams fng only)")
MACRO_CONFIG_INT(SvTeamScoreSpikeWrong, sv_team_score_wrong, -2, -100, 0, CFGFLAG_SERVER, "Points a team receives for grabbing into opponents spikes")

MACRO_CONFIG_INT(SvPlayerScoreSpikeNormal, sv_player_score_normal, 3, 0, 100, CFGFLAG_SERVER, "Points a player receives for grabbing into normal spikes")
MACRO_CONFIG_INT(SvPlayerScoreSpikeTeam, sv_player_score_team, 5, 0, 100, CFGFLAG_SERVER, "Points a player receives for grabbing into team spikes")
MACRO_CONFIG_INT(SvPlayerScoreSpikeGold, sv_player_score_gold, 7, 0, 100, CFGFLAG_SERVER, "Points a player receives for grabbing into golden spikes")
MACRO_CONFIG_INT(SvPlayerScoreSpikeGreen, sv_player_score_green, 5, 0, 100, CFGFLAG_SERVER, "Points a player receives for grabbing into green spikes(non 4-teams fng only)")
MACRO_CONFIG_INT(SvPlayerScoreSpikePurple, sv_player_score_purple, 6, 0, 100, CFGFLAG_SERVER, "Points a player receives for grabbing into purple spikes(non 4-teams fng only)")
MACRO_CONFIG_INT(SvPlayerScoreSpikeWrong, sv_player_score_wrong, -5, -100, 0, CFGFLAG_SERVER, "Points a player receives for grabbing into opponents spikes")

MACRO_CONFIG_INT(SvWrongSpikeFreeze, sv_wrong_spike_freeze, 5, 0, 60, CFGFLAG_SERVER, "The time, in seconds, a player gets frozen, if he grabbed a frozen opponent into the opponents spikes")
MACRO_CONFIG_INT(SvHitFreeze, sv_hit_freeze, 10, 0, 60, CFGFLAG_SERVER, "The time, in seconds, a player gets frozen, if he gets shot")

MACRO_CONFIG_INT(SvEmoticonDelay, sv_emoticon_delay, 3, 0, 10, CFGFLAG_SERVER, "The delay a player can use emotes")
MACRO_CONFIG_INT(SvKillDelay, sv_kill_delay, 3, -1, 10, CFGFLAG_SERVER, "The delay a player can kill it's character")
MACRO_CONFIG_INT(SvDieRespawnDelay, sv_die_respawn_delay, 3, -1, 10, CFGFLAG_SERVER, "The delay a player respawns automatically after killed")

MACRO_CONFIG_INT(SvSmoothFreezeMode, sv_smooth_freeze_mode, 1, 0, 1, CFGFLAG_SERVER, "Enable sending tune parameters to make freeze without input.")

MACRO_CONFIG_INT(SvEmotionalTees, sv_emotional_tees, 0, 0, 1, CFGFLAG_SERVER, "Enable /emote chat command.")
MACRO_CONFIG_INT(SvEmoteWheel, sv_emote_wheel, 0, 0, 1, CFGFLAG_SERVER, "Enable emote wheel like in ddrace with /emote chat command.")

//ddnet thingy
MACRO_CONFIG_INT(SvMapWindow, sv_map_window, 10, 0, 100, CFGFLAG_SERVER, "Map downloading send-ahead window")
// netlimit
MACRO_CONFIG_INT(SvNetlimit, sv_netlimit, 500, 0, 10000, CFGFLAG_SERVER, "Netlimit: Maximum amount of traffic a client is allowed to use (in kb/s)")
MACRO_CONFIG_INT(SvNetlimitAlpha, sv_netlimit_alpha, 50, 1, 100, CFGFLAG_SERVER, "Netlimit: Alpha of Exponention moving average")

MACRO_CONFIG_INT(SvTournamentType, sv_tournament_type, 1, 0, 3, CFGFLAG_SERVER, "0: custom, 1: config, 2: spectate, 3: random")

MACRO_CONFIG_INT(SvKillTakeOverTime, sv_kill_take_over_time, 250, -1, 1250, CFGFLAG_SERVER, "The time it takes hooking a frozen player to be stated as the killer. -1: disabled (no takeovers), x: the time in milliseconds it takes to takeover")

MACRO_CONFIG_INT(SvGrenadeDamageToHit, sv_grenade_damage_to_hit, 4, 0, 6, CFGFLAG_SERVER, "The damage that needs to be dealed with the grenade to freeze the opponent. 0: all shots will kill, x: damage that must be dealed to freeze the opponent")

MACRO_CONFIG_INT(SvTrivia, sv_trivia, 1, 0, 1, CFGFLAG_SERVER, "Send trivia at round end.")

MACRO_CONFIG_INT(SvPerWeaponReload, sv_per_weapon_reload, 1, 0, 1, CFGFLAG_SERVER, "Reload every weapon individually(allows switching weapons).")

//SAH (Stole and Hook)
MACRO_CONFIG_INT(SvSahDuelFallback, sv_sah_duel_fallback, 1, 0, 1, CFGFLAG_SERVER, "SAH: если в команде 1 игрок, самоубийства в чужие/нейтральные шипы дают +n. 0: самоубийства всегда штрафуются")
MACRO_CONFIG_INT(SvSahSelfKillProtect, sv_sah_self_kill_protect, 1, 0, 1, CFGFLAG_SERVER, "SAH: глобальная защита от кражи своего зафризного (1=вкл). Каждый игрок может переключить командой /sp")
MACRO_CONFIG_INT(SvSahTeamPenalty, sv_sah_team_penalty, 50, 0, 100, CFGFLAG_SERVER, "SAH: штраф для команды при оплошности в процентах (50 = половина от n)")
MACRO_CONFIG_INT(SvSahInversion, sv_sah_inversion, 1, 0, 1, CFGFLAG_SERVER, "SAH: инверсия убийств (1=свои зафризные дают -n, 0=как в FNG дают +n)")
MACRO_CONFIG_INT(SvSahShotsToUnfreeze, sv_sah_shots_to_unfreeze, 4, 1, 10, CFGFLAG_SERVER, "SAH: выстрелов пистолета для разморозки тиммейта")
MACRO_CONFIG_INT(SvSahHookableHook, sv_sah_hookable_hook, 0, 0, 1, CFGFLAG_SERVER, "SAH: unhookable тайлы отскакивают хук как лазер (1=да, 0=стандарт)")
MACRO_CONFIG_INT(SvSahHookBounces, sv_sah_hook_bounces, 3, 1, 5, CFGFLAG_SERVER, "SAH: максимальное количество отскоков хука")
MACRO_CONFIG_INT(SvSahScoreLimit, sv_sah_score_limit, 2000, 0, 2000, CFGFLAG_SERVER, "SAH: максимальный лимит очков (0 = без лимита, до 2000)")
MACRO_CONFIG_INT(SvSahDeathStars, sv_sah_death_stars, 1, 0, 1, CFGFLAG_SERVER, "SAH: красивая анимация смерти от шипов (1=звёзды вместо крови, работает на любом клиенте; 0=стандартная кровь)")
MACRO_CONFIG_INT(SvSahFreezeMarkerSpawn, sv_sah_freeze_marker_spawn, 0, 0, 1, CFGFLAG_SERVER, "SAH: визуальный маркер фризера звёздами у замороженного (1=вкл, играет звук спавна; 0=выкл)")
MACRO_CONFIG_INT(SvSahHookFireDelay, sv_sah_hook_fire_delay, 800, 0, 3000, CFGFLAG_SERVER, "SAH: кулдаун пере-хука после промаха в мс (0 = без кулдауна)")
