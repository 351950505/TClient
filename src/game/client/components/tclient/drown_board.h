/* Copyright © 2026 TClient; Gores drown board adapted from QmClient (MIT). */
#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_DROWN_BOARD_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_DROWN_BOARD_H

#include <engine/shared/protocol.h>
#include <game/gamecore.h>
#include <game/teamscore.h>

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <string_view>

// 落水榜（移植自 QmClient 的 Gores 组队落水榜，扩展为多列统计）：
// 核心统计和你同一个 DDRace 队伍的玩家从“未冻结”进入“冻结”的次数；死亡、跳跃、
// 出钩、钩人、救援、坚持、全灭、存活/冻结时长均为客户端估算（判据见
// D:\kehuduan\NovaClient-2.2.9.1-分析\落水榜多列语义-20261119.md，对齐 NovaClient
// Compact Statboard；Nova 官方自认 Saves 为估算）。纯客户端统计，不发送任何消息；
// 换图、切换 Gores 模式或玩家换队/离开起点会重置。
class CTClientDrownBoard
{
	// 救援归因窗口（tick）：队友解冻前这么多个 tick 内钩住过其冻结位置的本队
	// 成员都记一次救援。2026-11-21 用户口径放宽为 0.5 秒（25 tick）——
	// “把 tee 从冻结状态救活就算一次”，钩一下就走的救援也能归因。
	static constexpr int SAVE_WINDOW_TICKS = 25;
	// 钩尖/钩点与目标的判定半径，与物理 ProximityRadius 一致。
	static constexpr float HOOK_HIT_RADIUS = 28.0f;

	struct SPlayer
	{
		std::string m_Name;
		std::string m_Clan;
		int m_Team = -1;
		int m_Count = 0;
		bool m_HasObservedCharacter = false;
		bool m_SeenCharacter = false;
		bool m_OnStart = false;
		bool m_Frozen = false;
		bool m_Hooking = false;
		bool m_Armed = false;
		// 多列统计（估算口径见语义规格文档）。
		int m_Deaths = 0;
		int m_Jumps = 0;
		int m_HooksLaunched = 0;
		int m_PlayerHooks = 0;
		int m_Saves = 0;
		int m_Lasts = 0;
		int m_Aleds = 0;
		int m_TicksAlive = 0;
		int m_TicksFrozen = 0;
		// 统计侧状态，与冻结计数的基线字段分开，互不干扰。
		bool m_StatHasPrev = false;
		bool m_StatPrevFrozen = false;
		int m_StatPrevHookState = HOOK_IDLE;
		int m_StatPrevJumpedTotal = -1;
		int m_HookFrozenMateTicks = 0; // >0：最近 SAVE_WINDOW_TICKS tick 内钩住过冻结队友
		int m_HookFrozenMateId = -1;
		// 冻结期间最后钩住本玩家的本队成员（不限时限）——解冻归因用：
		// "拉到平台上等自然解冻"的等待期可长达数秒，任何短窗口都会漏。
		// 2026-11-21 用户口径：把 tee 从冻结状态救活就算一次。
		int m_FrozenLastHookerId = -1;
		bool m_LastCounted = false; // 本轮“坚持”已计数
	};
	std::array<SPlayer, MAX_CLIENTS> m_aPlayers{};

public:
	// 一行统计（渲染层用）。m_Ticks* 为原始 tick 数，换算秒在渲染层做；Eff 由渲染层
	// 按 100 × Saves / max(1, Saves + Freezes) 派生。
	struct SPlayerStats
	{
		int m_Freezes = 0;
		int m_Deaths = 0;
		int m_Jumps = 0;
		int m_HooksLaunched = 0;
		int m_PlayerHooks = 0;
		int m_Saves = 0;
		int m_Lasts = 0;
		int m_Aleds = 0;
		int m_TicksAlive = 0;
		int m_TicksFrozen = 0;
	};

	// 每个快照 tick 的扩展观测输入。由渲染管线在逐玩家 Observe 之后一次性整队提交；
	// m_JumpedTotal 取 CNetObj_DDNetCharacter::m_JumpedTotal，<0 表示服务器未提供。
	struct SDrownStatTick
	{
		bool m_Active = false;
		bool m_HasCharacter = false;
		bool m_Frozen = false;
		bool m_OnStart = false;
		int m_HookState = HOOK_IDLE;
		int m_JumpedTotal = -1;
		float m_X = 0.0f;
		float m_Y = 0.0f;
		float m_HookX = 0.0f;
		float m_HookY = 0.0f;
	};

	// 榜单只支持服务器声明的 Gores 模式；Team 0 的统计与显示共用同一过滤策略。
	static bool IsTrackedTeam(int Team, bool IncludeTeamZero)
	{
		return Team >= TEAM_FLOCK && Team < NUM_DDRACE_TEAMS && (IncludeTeamZero || Team != TEAM_FLOCK);
	}

	static bool IsSameTrackedTeam(int LocalTeam, int PlayerTeam, bool IncludeTeamZero)
	{
		return IsTrackedTeam(LocalTeam, IncludeTeamZero) && LocalTeam == PlayerTeam;
	}

	static bool IsBoardVisible(bool GoresGameMode, bool HasLocalClient, int LocalTeam, bool IncludeTeamZero)
	{
		return GoresGameMode && HasLocalClient && IsTrackedTeam(LocalTeam, IncludeTeamZero);
	}

	void Reset()
	{
		m_aPlayers = {};
		m_StatsLocalTeam = -1;
		m_AledEpisodeDone = false;
		m_WaitForUnfrozen = false;
		m_aEpisodeMembers = {};
		m_aEpisodeDeaths = {};
	}

	int Count(int ClientId) const
	{
		return ClientId >= 0 && ClientId < MAX_CLIENTS ? m_aPlayers[ClientId].m_Count : 0;
	}

	SPlayerStats Stats(int ClientId) const
	{
		SPlayerStats Stats;
		if(ClientId < 0 || ClientId >= MAX_CLIENTS)
			return Stats;
		const SPlayer &Player = m_aPlayers[ClientId];
		Stats.m_Freezes = Player.m_Count;
		Stats.m_Deaths = Player.m_Deaths;
		Stats.m_Jumps = Player.m_Jumps;
		Stats.m_HooksLaunched = Player.m_HooksLaunched;
		Stats.m_PlayerHooks = Player.m_PlayerHooks;
		Stats.m_Saves = Player.m_Saves;
		Stats.m_Lasts = Player.m_Lasts;
		Stats.m_Aleds = Player.m_Aleds;
		Stats.m_TicksAlive = Player.m_TicksAlive;
		Stats.m_TicksFrozen = Player.m_TicksFrozen;
		return Stats;
	}

	void Observe(int ClientId, bool Active, std::string_view Name, std::string_view Clan, int Team, bool HasCharacter, bool Frozen, bool Hooking, bool OnStart, bool IncludeTeamZero)
	{
		if(ClientId < 0 || ClientId >= MAX_CLIENTS)
			return;
		SPlayer &Player = m_aPlayers[ClientId];
		if(!Active || !IsTrackedTeam(Team, IncludeTeamZero))
		{
			Player = {};
			return;
		}
		// 身份或队伍改变后不复用旧槽位，避免离服、换队与 ClientId 重用串榜。
		if(Player.m_Team != Team || Player.m_Name != Name || Player.m_Clan != Clan)
		{
			Player = {};
			Player.m_Name = Name;
			Player.m_Clan = Clan;
			Player.m_Team = Team;
		}
		if(!HasCharacter)
		{
			// 角色暂时不在 snapshot 中时保留次数，不推断冻结或出钩事件。
			Player.m_SeenCharacter = false;
			Player.m_OnStart = false;
			Player.m_Frozen = false;
			Player.m_Hooking = false;
			Player.m_Armed = false;
			return;
		}
		if(Player.m_SeenCharacter && Player.m_OnStart && !OnStart && !(m_AledEpisodeDone && Team == m_StatsLocalTeam))
		{
			// 离开起点时只清零该玩家，新一轮落水从当前冻结和出钩状态重新建立基线。
			ResetCounters(Player);
			Player.m_HasObservedCharacter = false;
			Player.m_SeenCharacter = false;
		}
		Player.m_OnStart = OnStart;
		if(!Player.m_SeenCharacter)
		{
			Player.m_SeenCharacter = true;
			Player.m_Frozen = Frozen;
			Player.m_Hooking = Hooking;
			// 首次看到已冻结角色只建立基线；正常角色的首次落水可计数。
			Player.m_Armed = !Frozen && !Player.m_HasObservedCharacter;
			Player.m_HasObservedCharacter = true;
			return;
		}
		if(Frozen)
		{
			if(!Player.m_Frozen && Player.m_Armed && Player.m_Count < std::numeric_limits<int>::max())
				++Player.m_Count;
			Player.m_Armed = false;
		}
		else if(Hooking && !Player.m_Hooking)
		{
			// 救起后实际重新出钩才允许下一次落水，冻结中出钩不会重新解锁。
			Player.m_Armed = true;
		}
		Player.m_Frozen = Frozen;
		Player.m_Hooking = Hooking;
	}

	// 玩家死亡（Sv_Kill 的 victim）时由外部喂入；身份与队伍校验仍由 Observe 的
	// 槽位规则兜底（离服/换队/换名后下一次 Observe 会清槽）。
	void FeedDeath(int ClientId)
	{
		if(ClientId < 0 || ClientId >= MAX_CLIENTS)
			return;
		if(m_aPlayers[ClientId].m_Deaths < std::numeric_limits<int>::max())
			++m_aPlayers[ClientId].m_Deaths;
		m_aPlayers[ClientId].m_PendingRescuers = 0; // 死亡：救援确认作废
		m_aPlayers[ClientId].m_FrozenLastHookerId = -1;
		if(m_AledEpisodeDone && m_aEpisodeMembers[ClientId])
			m_aEpisodeDeaths[ClientId] = true;
	}

	void FeedTeamDeath(int Team)
	{
		if(!IsTrackedTeam(Team, true))
			return;
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
		{
			if(m_aPlayers[ClientId].m_Team == Team)
				FeedDeath(ClientId);
		}
	}

	// 多列统计：整队批量观测。pTicks 必须按 ClientId 全量给出（与同一 tick 的
	// Observe 调用同源）。ResetOnTeamWipe：全队冻结后，等本轮全员死亡才清空统计。
	void ObserveStats(const SDrownStatTick *pTicks, int LocalTeam, bool IncludeTeamZero, bool ResetOnTeamWipe, int ElapsedTicks = 1)
	{
		if(pTicks == nullptr || ElapsedTicks <= 0)
			return;
		if(LocalTeam != m_StatsLocalTeam)
		{
			// 本地换队：已计入的数字保留（沿用显示过滤），轮次状态全部重来。
			m_StatsLocalTeam = LocalTeam;
			m_AledEpisodeDone = false;
			m_WaitForUnfrozen = false;
			m_aEpisodeMembers = {};
			m_aEpisodeDeaths = {};
			for(SPlayer &Player : m_aPlayers)
				Player.m_LastCounted = false;
		}
		if(m_AledEpisodeDone)
		{
			bool AllDead = true;
			for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
			{
				// 永久离线的成员等不到死亡消息：不阻塞本轮重置，计数继续。
				if(m_aEpisodeMembers[ClientId] && !m_aEpisodeDeaths[ClientId] && pTicks[ClientId].m_Active)
					AllDead = false;
			}
			if(AllDead)
			{
				for(SPlayer &Player : m_aPlayers)
				{
					if(Player.m_Team != LocalTeam)
						continue;
					if(ResetOnTeamWipe)
						ResetCounters(Player);
					Player.m_LastCounted = false;
				}
				m_AledEpisodeDone = false;
				m_WaitForUnfrozen = true;
				m_aEpisodeDeaths = {};
			}
		}
		if(m_WaitForUnfrozen)
		{
			// 等待期照常推进衰减，解锁首 tick 才不会拿旧钩冻/旧冻结基线虚记。
			for(SPlayer &Player : m_aPlayers)
			{
				Player.m_HookFrozenMateTicks = std::max(0, Player.m_HookFrozenMateTicks - ElapsedTicks);
				Player.m_StatHasPrev = false;
				Player.m_StatPrevJumpedTotal = -1;
				Player.m_PendingRescuers = 0; // 旧轮次的救援待确认一并作废
			}
			// K 消息可能先于重生快照；仍在服的成员全部重新出现并解冻前，
			// 不把旧冻结状态计作新一轮。永久离线的成员不再阻塞计数；
			// 转观战/转队的成员同样不阻塞——Observe 换队或清槽后 m_Team 已
			// 不是本轮队伍，永远等不到"重新出现并解冻"（2026-11-20 审查 P2-1）。
			for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
			{
				const SDrownStatTick &Now = pTicks[ClientId];
				if(!m_aEpisodeMembers[ClientId] || !Now.m_Active)
					continue;
				if(m_aPlayers[ClientId].m_Team != m_StatsLocalTeam)
					continue;
				if(!Now.m_HasCharacter || Now.m_Frozen)
					return;
			}
			m_WaitForUnfrozen = false;
			m_aEpisodeMembers = {};
			return;
		}

		const float HookHitRadius2 = HOOK_HIT_RADIUS * HOOK_HIT_RADIUS;

	// 1) 救援归因（解冻 tick）：归因给冻结期间最后钩住它的本队成员（不设
	// 时限——"拉到平台上等自然解冻"的等待期可长达数秒）；真正计数在被救者
	// 「进入活的状态」时（见第 2 段）。2026-11-21 用户口径：把 tee 从冻结
	// 状态救活就算一次。覆盖：拉出冻结砖、钩住吊到平台上等自然解冻、空中
	// 反复拉活、钩拽加速融化。已知漏计：深冻被锤敲/激光解除且无钩（他人
	// 开火无网络信号）。
	for(int VictimId = 0; VictimId < MAX_CLIENTS; ++VictimId)
	{
		SPlayer &Victim = m_aPlayers[VictimId];
		const SDrownStatTick &VictimTick = pTicks[VictimId];
		if(!Victim.m_StatHasPrev || !Victim.m_StatPrevFrozen)
			continue;
		if(!VictimTick.m_Active || !VictimTick.m_HasCharacter || VictimTick.m_Frozen)
			continue;
		const int HookerId = Victim.m_FrozenLastHookerId;
		Victim.m_FrozenLastHookerId = -1;
		if(HookerId < 0 || HookerId >= MAX_CLIENTS)
			continue;
		const SPlayer &Rescuer = m_aPlayers[HookerId];
		if(Rescuer.m_Team != Victim.m_Team || !IsTrackedTeam(Rescuer.m_Team, IncludeTeamZero))
			continue;
		Victim.m_PendingRescuers = 1ULL << HookerId;
		Victim.m_PendingSaveX = VictimTick.m_X;
		Victim.m_PendingSaveY = VictimTick.m_Y;
	}

		// 2) 逐玩家计数与状态推进。
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
		{
			SPlayer &Player = m_aPlayers[ClientId];
			const SDrownStatTick &Now = pTicks[ClientId];
			const bool Tracked = Player.m_SeenCharacter && IsTrackedTeam(Player.m_Team, IncludeTeamZero);
			if(!Tracked || !Now.m_Active || !Now.m_HasCharacter)
			{
				Player.m_HookFrozenMateTicks = std::max(0, Player.m_HookFrozenMateTicks - ElapsedTicks);
				Player.m_StatHasPrev = false;
				Player.m_StatPrevJumpedTotal = -1;
				Player.m_PendingRescuers = 0; // 角色消失/离线：救援确认作废
				continue;
			}

			if(Now.m_HookState == HOOK_FLYING && Player.m_StatHasPrev && Player.m_StatPrevHookState != HOOK_FLYING &&
				Player.m_HooksLaunched < std::numeric_limits<int>::max())
				++Player.m_HooksLaunched;
			if(Now.m_HookState == HOOK_GRABBED && Player.m_StatHasPrev && Player.m_StatPrevHookState != HOOK_GRABBED)
			{
				for(int MateId = 0; MateId < MAX_CLIENTS; ++MateId)
				{
					if(MateId == ClientId)
						continue;
					const SPlayer &Mate = m_aPlayers[MateId];
					if(Mate.m_Team != Player.m_Team || !IsTrackedTeam(Mate.m_Team, IncludeTeamZero))
						continue;
					const SDrownStatTick &MateTick = pTicks[MateId];
					if(!MateTick.m_Active || !MateTick.m_HasCharacter)
						continue;
					const float DX = MateTick.m_X - Now.m_HookX;
					const float DY = MateTick.m_Y - Now.m_HookY;
					if(DX * DX + DY * DY <= HookHitRadius2)
					{
						if(Player.m_PlayerHooks < std::numeric_limits<int>::max())
							++Player.m_PlayerHooks;
						break;
					}
				}
			}
			if(Now.m_JumpedTotal >= 0)
			{
				if(Player.m_StatPrevJumpedTotal >= 0 && Now.m_JumpedTotal > Player.m_StatPrevJumpedTotal)
					Player.m_Jumps += std::min(Now.m_JumpedTotal - Player.m_StatPrevJumpedTotal, 255);
				Player.m_StatPrevJumpedTotal = Now.m_JumpedTotal;
			}
			if(Now.m_Frozen)
				Player.m_TicksFrozen += std::min(ElapsedTicks, std::numeric_limits<int>::max() - Player.m_TicksFrozen);
			else
				Player.m_TicksAlive += std::min(ElapsedTicks, std::numeric_limits<int>::max() - Player.m_TicksAlive);

			// 救援确认：待确认的被救者进入活的状态（自己出钩、被钩或累计移动
			// ≥半砖——锤/激光击打会让它立刻恢复行动自然落入触发），钩手各 +1；
			// 再次冻结或死亡（由 FeedDeath 清）则作废。
			if(Player.m_PendingRescuers != 0)
			{
				const float PendingDX = Now.m_X - Player.m_PendingSaveX;
				const float PendingDY = Now.m_Y - Player.m_PendingSaveY;
				const bool EnteredAlive = Now.m_HookState == HOOK_FLYING || Now.m_HookState == HOOK_GRABBED ||
					PendingDX * PendingDX + PendingDY * PendingDY >= 16.0f * 16.0f;
				if(Now.m_Frozen)
					Player.m_PendingRescuers = 0;
				else if(EnteredAlive)
				{
					for(int RescuerId = 0; RescuerId < MAX_CLIENTS; ++RescuerId)
					{
						if(!(Player.m_PendingRescuers & (1ULL << RescuerId)))
							continue;
						SPlayer &Rescuer = m_aPlayers[RescuerId];
						if(Rescuer.m_Saves < std::numeric_limits<int>::max())
							++Rescuer.m_Saves;
					}
					Player.m_PendingRescuers = 0;
				}
			}

			Player.m_StatPrevFrozen = Now.m_Frozen;
			Player.m_StatPrevHookState = Now.m_HookState;
			Player.m_StatHasPrev = true;
			Player.m_HookFrozenMateTicks = std::max(0, Player.m_HookFrozenMateTicks - ElapsedTicks);
			if(Now.m_HookState == HOOK_FLYING || Now.m_HookState == HOOK_GRABBED)
			{
				for(int MateId = 0; MateId < MAX_CLIENTS; ++MateId)
				{
					if(MateId == ClientId)
						continue;
					const SPlayer &Mate = m_aPlayers[MateId];
					if(Mate.m_Team != Player.m_Team || !IsTrackedTeam(Mate.m_Team, IncludeTeamZero))
						continue;
					const SDrownStatTick &MateTick = pTicks[MateId];
					if(!MateTick.m_Active || !MateTick.m_HasCharacter || !MateTick.m_Frozen)
						continue;
					const float DX = MateTick.m_X - Now.m_HookX;
					const float DY = MateTick.m_Y - Now.m_HookY;
					if(DX * DX + DY * DY <= HookHitRadius2)
					{
						Player.m_HookFrozenMateTicks = SAVE_WINDOW_TICKS;
						Player.m_HookFrozenMateId = MateId;
						break;
					}
				}
			}
		}

		// 3) 坚持 / 全灭（整队轮次，只统计已离开起点的玩家）。
		int Eligible = 0;
		int UnfrozenEligible = 0;
		int LastUnfrozenId = -1;
		if(IsTrackedTeam(LocalTeam, IncludeTeamZero))
		{
			for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
			{
				const SPlayer &Player = m_aPlayers[ClientId];
				const SDrownStatTick &Now = pTicks[ClientId];
				if(Player.m_Team != LocalTeam)
					continue;
				if(!Now.m_Active || !Now.m_HasCharacter || Now.m_OnStart)
					continue;
				++Eligible;
				if(!Now.m_Frozen)
				{
					++UnfrozenEligible;
					LastUnfrozenId = ClientId;
				}
			}
		}
		if(Eligible >= 2 && UnfrozenEligible == 1 && LastUnfrozenId >= 0)
		{
			SPlayer &Player = m_aPlayers[LastUnfrozenId];
			if(!Player.m_LastCounted && Player.m_Lasts < std::numeric_limits<int>::max())
			{
				++Player.m_Lasts;
				Player.m_LastCounted = true;
			}
		}
		else if(UnfrozenEligible >= 2)
		{
			for(SPlayer &Player : m_aPlayers)
				Player.m_LastCounted = false;
		}
		if(Eligible >= 2 && UnfrozenEligible == 0)
		{
			if(!m_AledEpisodeDone)
			{
				m_AledEpisodeDone = true;
				for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
				{
					const SDrownStatTick &Now = pTicks[ClientId];
					if(m_aPlayers[ClientId].m_Team != LocalTeam)
						continue;
					if(!Now.m_Active || !Now.m_HasCharacter || Now.m_OnStart)
						continue;
					m_aEpisodeMembers[ClientId] = true;
					if(m_aPlayers[ClientId].m_Aleds < std::numeric_limits<int>::max())
						++m_aPlayers[ClientId].m_Aleds;
				}
			}
		}
	}

private:
	// 新一轮开局：清空全部计数，但保留身份槽位与冻结基线（由 Observe 重建）。
	static void ResetCounters(SPlayer &Player)
	{
		Player.m_Count = 0;
		Player.m_Deaths = 0;
		Player.m_Jumps = 0;
		Player.m_HooksLaunched = 0;
		Player.m_PlayerHooks = 0;
		Player.m_Saves = 0;
		Player.m_Lasts = 0;
		Player.m_Aleds = 0;
		Player.m_TicksAlive = 0;
		Player.m_TicksFrozen = 0;
		Player.m_StatHasPrev = false;
		Player.m_HookFrozenMateTicks = 0;
		Player.m_HookFrozenMateId = -1;
		Player.m_PendingRescuers = 0;
		Player.m_StatPrevJumpedTotal = -1;
		Player.m_LastCounted = false;
	}

	int m_StatsLocalTeam = -1;
	bool m_AledEpisodeDone = false;
	bool m_WaitForUnfrozen = false;
	std::array<bool, MAX_CLIENTS> m_aEpisodeMembers{};
	std::array<bool, MAX_CLIENTS> m_aEpisodeDeaths{};
};

#endif
