#ifndef ENGINE_CLIENT_LINK_PROBE_H
#define ENGINE_CLIENT_LINK_PROBE_H

#include <base/time.h>

#include <engine/shared/protocol.h>
#include <engine/shared/uuid_manager.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>

class CLinkProbeTracker
{
	struct CPending
	{
		CUuid m_Id = {};
		int64_t m_SentTime = -1;
		bool m_Confirmed = false;
	};

	static constexpr int MAX_RTT_SAMPLES = 16;

	std::array<CPending, 64> m_aPending = {};
	int64_t m_LastFrameTime = -1;
	int64_t m_Frequency = 0;

	// 实时窗口（对外指标：dup-upstream 日志 + 设置页）：与稳定窗口共同等满 2 秒结算，
	// 仅展示周期缩短到 4 秒一窗、最少 6 个样本，避免提前超时及跨窗迟到回包误判。
	// 仍保留 0.5*Raw+0.5*old 平滑，避免单个窗口的抖动直接糊到脸上。
	int64_t m_RecentWindowStart = 0;
	uint64_t m_RecentWindowSamples = 0;
	uint64_t m_RecentWindowUnconfirmed = 0;
	uint64_t m_LastRecentWindowSamples = 0;
	float m_RecentPercent = 0.0f;
	bool m_HasRecentEstimate = false;

	// 稳定窗口（只给 Copies() 判断档位）：完全保持改造前的行为——样本等满 2 秒结算、
	// 10 秒一窗、最少 10 个样本、同样的平滑。实时窗口再灵敏也不会放大档位抖动。
	int64_t m_StableWindowStart = 0;
	uint64_t m_StableWindowSamples = 0;
	uint64_t m_StableWindowUnconfirmed = 0;
	uint64_t m_LastStableWindowSamples = 0;
	float m_StablePercent = 0.0f;
	bool m_HasStableEstimate = false;

	// 回包延迟样本：按时间顺序保留最近若干次探针的 RTT（0.5 秒一个 ≈ 最近 8 秒）
	std::array<int, MAX_RTT_SAMPLES> m_aRttMs = {};
	int m_RttCount = 0;

	uint64_t m_TotalSamples = 0;
	uint64_t m_TotalUnconfirmed = 0;
	uint64_t m_TotalIgnored = 0;

	// 档位滞回用：探针噪声大，档位不能跟着指标每几秒上下乱跳，否则"双发"就不稳定了
	int m_DupLevel = 0;
	int64_t m_DupDownSince = 0;

	void AddRttSample(int RttMs)
	{
		// 保持数组按时间升序，JitterMs() 的"相邻差"才是真正相邻两次回包
		if(m_RttCount == MAX_RTT_SAMPLES)
		{
			for(int i = 1; i < MAX_RTT_SAMPLES; i++)
				m_aRttMs[i - 1] = m_aRttMs[i];
			m_RttCount--;
		}
		m_aRttMs[m_RttCount++] = RttMs;
	}

public:
	void Reset(int64_t Now)
	{
		m_aPending = {};
		m_LastFrameTime = Now;
		m_RecentWindowStart = Now;
		m_RecentWindowSamples = 0;
		m_RecentWindowUnconfirmed = 0;
		m_LastRecentWindowSamples = 0;
		m_RecentPercent = 0.0f;
		m_HasRecentEstimate = false;
		m_StableWindowStart = Now;
		m_StableWindowSamples = 0;
		m_StableWindowUnconfirmed = 0;
		m_LastStableWindowSamples = 0;
		m_StablePercent = 0.0f;
		m_HasStableEstimate = false;
		m_aRttMs = {};
		m_RttCount = 0;
		m_TotalSamples = 0;
		m_TotalUnconfirmed = 0;
		m_TotalIgnored = 0;
		m_DupLevel = 0;
		m_DupDownSince = Now;
	}

	void OnFrame(int64_t Now, int64_t Frequency)
	{
		m_Frequency = Frequency;
		// 主线程暂停期间无法判断回复何时到达，取消在途样本并冻结窗口时钟。
		if(m_LastFrameTime >= 0 && Now - m_LastFrameTime > Frequency / 4)
		{
			for(const auto &Pending : m_aPending)
				m_TotalIgnored += Pending.m_SentTime >= 0;
			m_aPending = {};
			m_RecentWindowStart += Now - m_LastFrameTime;
			m_StableWindowStart += Now - m_LastFrameTime;
			// 暂停前后的 RTT 不可比，直接丢弃样本，避免恢复瞬间的假抖动
			m_aRttMs = {};
			m_RttCount = 0;
		}
		m_LastFrameTime = Now;
	}

	bool RecordSent(const CUuid &Id, int64_t Now)
	{
		for(auto &Pending : m_aPending)
		{
			if(Pending.m_SentTime < 0)
			{
				Pending = {Id, Now, false};
				return true;
			}
		}
		return false;
	}

	// Now 由调用方传入（time_get()），用于计算 RTT；协议本身不变。
	bool Confirm(const CUuid &Id, int64_t Now)
	{
		for(auto &Pending : m_aPending)
		{
			if(Pending.m_SentTime >= 0 && Pending.m_Id == Id)
			{
				// 同一探针的重复回包（理论上不会发生）不重复计入 RTT
				const bool FirstConfirm = !Pending.m_Confirmed;
				Pending.m_Confirmed = true;
				const int64_t Frequency = m_Frequency > 0 ? m_Frequency : time_freq();
				if(FirstConfirm && Frequency > 0 && Now >= Pending.m_SentTime)
					AddRttSample((int)((Now - Pending.m_SentTime) * 1000 / Frequency));
				return true;
			}
		}
		return false;
	}

	void Update(int64_t Now, int64_t Frequency)
	{
		m_Frequency = Frequency;
		// 两个窗口共同使用原来的 2 秒确认期限，一次结算后立即移除探针。
		// 不提前发布未确认：迟到回包无需跨窗撤回，暂停取消也不会遗留半结算样本。
		for(auto &Pending : m_aPending)
		{
			if(Pending.m_SentTime >= 0 && Now - Pending.m_SentTime >= 2 * Frequency)
			{
				m_RecentWindowSamples++;
				m_StableWindowSamples++;
				m_TotalSamples++;
				if(!Pending.m_Confirmed)
				{
					m_RecentWindowUnconfirmed++;
					m_StableWindowUnconfirmed++;
					m_TotalUnconfirmed++;
				}
				Pending = {};
			}
		}

		// 实时窗口 4 秒、最少 6 个样本：约 8 个探针/窗口，够反映最近几秒的链路状态，
		// 又不至于少到一个样本就把百分比拉爆。
		if(Now - m_RecentWindowStart >= 4 * Frequency && m_RecentWindowSamples >= 6)
		{
			const float RawPercent = m_RecentWindowUnconfirmed * 100.0f / m_RecentWindowSamples;
			m_RecentPercent = m_HasRecentEstimate ? 0.5f * RawPercent + 0.5f * m_RecentPercent : RawPercent;
			m_HasRecentEstimate = true;
			m_LastRecentWindowSamples = m_RecentWindowSamples;
			m_RecentWindowSamples = 0;
			m_RecentWindowUnconfirmed = 0;
			m_RecentWindowStart = Now;
		}

		// 稳定窗口保持 10 秒/最少 10 个样本不变：Copies() 继续用这个更稳的估计值。
		if(Now - m_StableWindowStart >= 10 * Frequency && m_StableWindowSamples >= 10)
		{
			const float RawPercent = m_StableWindowUnconfirmed * 100.0f / m_StableWindowSamples;
			m_StablePercent = m_HasStableEstimate ? 0.5f * RawPercent + 0.5f * m_StablePercent : RawPercent;
			m_HasStableEstimate = true;
			m_LastStableWindowSamples = m_StableWindowSamples;
			m_StableWindowSamples = 0;
			m_StableWindowUnconfirmed = 0;
			m_StableWindowStart = Now;
		}
	}

	// ---- 实时指标（4 秒窗口，设置页与 dup-upstream 日志使用）----
	bool HasEstimate() const { return m_HasRecentEstimate; }
	float UnconfirmedPercent() const { return m_RecentPercent; }
	uint64_t WindowSamples() const { return m_HasRecentEstimate ? m_LastRecentWindowSamples : m_RecentWindowSamples; }
	// 语义化别名：UI 明确读"短窗口"的值，将来改实时窗口也不会悄悄换掉展示口径
	float RecentUnconfirmedPercent() const { return m_RecentPercent; }
	bool HasRecentEstimate() const { return m_HasRecentEstimate; }

	// ---- 稳定指标（10 秒窗口，仅 Copies() 内部使用）----
	bool HasStableEstimate() const { return m_HasStableEstimate; }
	float StableUnconfirmedPercent() const { return m_StablePercent; }
	uint64_t StableWindowSamples() const { return m_HasStableEstimate ? m_LastStableWindowSamples : m_StableWindowSamples; }

	// ---- 延迟/抖动：只用现有 PINGEX 探针的回包时间，不新增任何协议字段 ----
	bool HasLatency() const { return m_RttCount > 0; }
	float AverageLatencyMs() const
	{
		if(m_RttCount <= 0)
			return 0.0f;
		int Sum = 0;
		for(int i = 0; i < m_RttCount; i++)
			Sum += m_aRttMs[i];
		return (float)Sum / (float)m_RttCount;
	}
	float JitterMs() const
	{
		// 抖动 = 相邻两次 RTT 差值的绝对值的平均，比标准差更直观且不受偶发尖峰平方放大
		if(m_RttCount < 2)
			return 0.0f;
		int Sum = 0;
		for(int i = 1; i < m_RttCount; i++)
			Sum += std::abs(m_aRttMs[i] - m_aRttMs[i - 1]);
		return (float)Sum / (float)(m_RttCount - 1);
	}

	uint64_t TotalSamples() const { return m_TotalSamples; }
	uint64_t TotalUnconfirmed() const { return m_TotalUnconfirmed; }
	uint64_t TotalIgnored() const { return m_TotalIgnored; }

	// 决定这一帧真实发几份。
	// Limit   = cl_dup_upstream（上限）
	// MinCopies = 双发下限：只要 Limit>=2 就至少发这么多份，探针没数据/噪音大也照样双发
	// 滞回：升档立刻生效，降档必须连续干净满 HoldSec 秒，避免来回跳（"双发不稳定"就是这么来的）
	// 注意：这里读的是 10 秒稳定窗口（m_StablePercent），不读 4 秒实时窗口，
	// 所以加快展示不会放大档位抖动；下面的滞回代码与改造前逐行一致。
	int Copies(int Limit, int MinCopies, float LowPct, float HighPct, int HoldSec, int64_t Now, int64_t Frequency)
	{
		if(Limit < 2)
			return 1;

		int Base = MinCopies;
		if(Base < 1)
			Base = 1;
		if(Base > Limit)
			Base = Limit;

		if(!m_HasStableEstimate)
		{
			// 探针还没样本：不再傻等，直接按下限（默认双发）
			m_DupLevel = Base;
			m_DupDownSince = Now;
			return Base;
		}

		int Desired;
		if(m_StablePercent >= HighPct)
			Desired = 3;
		else if(m_StablePercent >= LowPct)
			Desired = 2;
		else
			Desired = Base;
		if(Desired > Limit)
			Desired = Limit;

		if(Desired > m_DupLevel)
		{
			// 情况变坏：立刻升档，不犹豫
			m_DupLevel = Desired;
			m_DupDownSince = Now;
		}
		else if(Desired < m_DupLevel)
		{
			// 情况变好：先看指标是否真的干净，连续干净够久才降档
			const bool CleanEnough = m_StablePercent < LowPct * 0.5f;
			if(!CleanEnough)
				m_DupDownSince = Now;
			else if(Now - m_DupDownSince >= (int64_t)HoldSec * Frequency)
				m_DupLevel = Desired;
		}
		else
		{
			m_DupDownSince = Now;
		}

		int Result = m_DupLevel;
		if(Result < Base)
			Result = Base;
		if(Result > Limit)
			Result = Limit;
		if(Result < 1)
			Result = 1;
		return Result;
	}

	int DupLevel() const { return m_DupLevel; }
};

// [SNAPSHOT-GAP] Downstream snapshot tick continuity tracker.
//
// The server does not send every game tick: by default it sends a global snapshot
// every 2 ticks, sv_high_bandwidth switches it to every tick, and a client in
// SNAPRATE_RECOVER only gets one snapshot per second. A missing tick is therefore
// measured against a locally estimated normal step, never against 1.
class CSnapshotGapTracker
{
	static constexpr int DEFAULT_STEP = 2;
	// The server cadence is 1 or 2 ticks. Bigger deltas are gaps or a rate change
	// and must not drag the baseline upwards, otherwise loss would hide itself.
	static constexpr int STEP_WINDOW = 64;
	static constexpr int WARMUP_SAMPLES = 6;
	static constexpr int RECENT_LOSS_SAMPLES = 512;
	static constexpr int RECENT_LOSS_SECONDS = 10;

	struct CLossSample
	{
		int64_t m_Time = 0;
		int m_Ticks = 0;
	};

	std::array<uint8_t, STEP_WINDOW> m_aStepSamples = {};
	int m_NumStepSamples = 0;
	int m_NextStepSample = 0;
	int m_ExpectedStep = DEFAULT_STEP;

	int m_LastTick = -1;
	int64_t m_LastTime = -1;

	// One event is a group of gaps that arrive close together. Event length is
	// measured in missing snapshots: with ExpectedStep=2 one lost packet is
	// already 2 ticks, so counting raw ticks would classify every random loss as
	// a burst. One missing snapshot is random loss, two or more is a burst.
	bool m_EventActive = false;
	int m_EventLostTicks = 0;
	int m_EventLostSnapshots = 0;
	int64_t m_EventLastGapTime = -1;

	int m_BurstLossEvents = 0;
	int m_RandomLossEvents = 0;
	int m_BurstLostTicks = 0;
	int m_TotalLostTicks = 0;
	int m_SkippedIntervals = 0;
	// Set after a pause/recover skip: the very next snapshot only re-syncs, so the
	// transition back from the 1 snapshot/second cadence to the normal one is not
	// mistaken for a gap either.
	bool m_ResyncNext = false;

	std::array<CLossSample, RECENT_LOSS_SAMPLES> m_aRecentLoss = {};
	int m_NumRecentLoss = 0;
	int m_NextRecentLoss = 0;
	// Start of the current uninterrupted observation segment, not the connection:
	// a pause/recover leaves its duration unknown until fresh snapshots re-sync.
	int64_t m_FirstTime = -1;

	static int64_t ObservationTimeout() { return 9 * time_freq() / 10; }

	// Relearn cadence and the recent percentage without discarding lifetime loss
	// events. Unknown gaps must neither manufacture loss nor dilute the denominator.
	void ResetRecentObservation()
	{
		m_aStepSamples = {};
		m_NumStepSamples = 0;
		m_NextStepSample = 0;
		m_ExpectedStep = DEFAULT_STEP;
		m_aRecentLoss = {};
		m_NumRecentLoss = 0;
		m_NextRecentLoss = 0;
		m_FirstTime = -1;
	}

	int CountStep(int Step) const
	{
		int Count = 0;
		for(int i = 0; i < m_NumStepSamples; i++)
			Count += m_aStepSamples[i] == Step;
		return Count;
	}

	void UpdateExpectedStep()
	{
		// The normal cadence is 1 tick (sv_high_bandwidth) or 2 ticks. Loss only
		// ever produces larger deltas, so only 1 and 2 can become the baseline.
		// Downward switches need a few samples, upward switches need a sustained
		// majority; otherwise heavy loss at step 1 could fake a step-2 server.
		const int Count1 = CountStep(1);
		const int Count2 = CountStep(2);
		if(m_ExpectedStep == 2)
		{
			if(Count1 >= WARMUP_SAMPLES && Count1 > Count2)
				m_ExpectedStep = 1;
		}
		else
		{
			if(Count2 >= STEP_WINDOW / 2 && Count2 > Count1)
				m_ExpectedStep = 2;
		}
	}

	void AddStepSample(int Delta)
	{
		// Record every positive delta (clamped, larger values never win the vote).
		if(Delta < 1)
			return;
		m_aStepSamples[m_NextStepSample] = (uint8_t)std::min(Delta, 255);
		m_NextStepSample = (m_NextStepSample + 1) % STEP_WINDOW;
		if(m_NumStepSamples < STEP_WINDOW)
			m_NumStepSamples++;
		UpdateExpectedStep();
	}

	// Do not report loss before the normal cadence has actually been observed:
	// right after connecting the server sends SNAPRATE_INIT snapshots every 10
	// ticks, which must not be mistaken for a burst.
	bool DetectionReady() const { return CountStep(m_ExpectedStep) >= WARMUP_SAMPLES; }

	void RecordLoss(int64_t Now, int Ticks)
	{
		m_aRecentLoss[m_NextRecentLoss] = {Now, Ticks};
		m_NextRecentLoss = (m_NextRecentLoss + 1) % RECENT_LOSS_SAMPLES;
		if(m_NumRecentLoss < RECENT_LOSS_SAMPLES)
			m_NumRecentLoss++;
	}

	void FinalizeEvent()
	{
		if(!m_EventActive)
			return;
		if(m_EventLostSnapshots >= 2)
		{
			m_BurstLossEvents++;
			m_BurstLostTicks += m_EventLostTicks;
		}
		else
		{
			m_RandomLossEvents++;
		}
		m_EventActive = false;
		m_EventLostTicks = 0;
		m_EventLostSnapshots = 0;
		m_EventLastGapTime = -1;
	}

public:
	// Close finished events even without another snapshot, using twice the merge
	// window. Separately invalidate recent sampling when observations go stale.
	void Update(int64_t Now)
	{
		if(m_EventActive)
		{
			const int64_t NormalInterval = (int64_t)m_ExpectedStep * time_freq() / SERVER_TICK_SPEED;
			if(Now - m_EventLastGapTime > NormalInterval * 3)
				FinalizeEvent();
		}
		if(m_LastTime >= 0 && Now - m_LastTime >= ObservationTimeout() && !m_ResyncNext)
		{
			ResetRecentObservation();
			m_ResyncNext = true;
		}
	}

	void Reset()
	{
		ResetRecentObservation();
		m_LastTick = -1;
		m_LastTime = -1;
		m_EventActive = false;
		m_EventLostTicks = 0;
		m_EventLostSnapshots = 0;
		m_EventLastGapTime = -1;
		m_BurstLossEvents = 0;
		m_RandomLossEvents = 0;
		m_BurstLostTicks = 0;
		m_TotalLostTicks = 0;
		m_SkippedIntervals = 0;
		m_ResyncNext = false;
	}

	// Call once for every snapshot that was fully verified and stored.
	void OnSnapshot(int Tick, int64_t Now)
	{
		if(m_LastTick < 0 || m_LastTime < 0)
		{
			m_LastTick = Tick;
			m_LastTime = Now;
			if(m_FirstTime < 0)
				m_FirstTime = Now;
			return;
		}

		if(Tick <= m_LastTick)
		{
			// Duplicates are filtered by the caller. A restarted tick stream must
			// relearn its cadence and recent window, including SNAPRATE_INIT.
			Reset();
			m_SkippedIntervals++;
			m_LastTick = Tick;
			m_LastTime = Now;
			m_FirstTime = Now;
			return;
		}
		const int Delta = Tick - m_LastTick;

		// A long gap can be a local pause, SNAPRATE_RECOVER or real loss. Do not
		// invent missing snapshots, but invalidate sampling instead of reporting 0%.
		const bool TimePause = Now - m_LastTime >= ObservationTimeout();
		const bool TickPause = Delta >= SERVER_TICK_SPEED;
		if(TimePause || TickPause)
		{
			FinalizeEvent();
			m_SkippedIntervals++;
			ResetRecentObservation();
			m_ResyncNext = true;
			m_LastTick = Tick;
			m_LastTime = Now;
			return;
		}

		// The first normal snapshot after a recover/pause only re-syncs: no loss
		// is inferred across the slow-cadence transition. Start a fresh denominator.
		if(m_ResyncNext)
		{
			m_ResyncNext = false;
			m_FirstTime = Now;
			AddStepSample(Delta);
			m_LastTick = Tick;
			m_LastTime = Now;
			return;
		}

		AddStepSample(Delta);
		const int64_t NormalInterval = (int64_t)m_ExpectedStep * time_freq() / SERVER_TICK_SPEED;

		if(!DetectionReady() || Delta <= m_ExpectedStep)
		{
			// Normal cadence. The event itself is closed by Update() after the merge
			// window, not here: a normal snapshot in between must not split an event.
			m_LastTick = Tick;
			m_LastTime = Now;
			return;
		}

		const int GapTicks = Delta - m_ExpectedStep;
		const int GapSnapshots = (Delta + m_ExpectedStep - 1) / m_ExpectedStep - 1;
		m_TotalLostTicks += GapTicks;
		RecordLoss(Now, GapTicks);

		// Gaps that arrive within 1.5 normal snapshot intervals belong to the same
		// event; anything later starts a new one.
		const int64_t MergeWindow = NormalInterval * 3 / 2;
		if(m_EventActive && Now - m_EventLastGapTime <= MergeWindow)
		{
			m_EventLostTicks += GapTicks;
			m_EventLostSnapshots += GapSnapshots;
			m_EventLastGapTime = Now;
		}
		else
		{
			FinalizeEvent();
			m_EventActive = true;
			m_EventLostTicks = GapTicks;
			m_EventLostSnapshots = GapSnapshots;
			m_EventLastGapTime = Now;
		}

		m_LastTick = Tick;
		m_LastTime = Now;
	}

	// Percentage of all lost ticks that belonged to burst events (events with two
	// or more missing snapshots). Random losses make up the rest of TotalLossTicks().
	int BurstPercent() const
	{
		if(m_TotalLostTicks <= 0)
			return 0;
		return (int)((int64_t)m_BurstLostTicks * 100 / m_TotalLostTicks);
	}

	int RecentLossTicks(int64_t Now) const
	{
		int Ticks = 0;
		for(int i = 0; i < m_NumRecentLoss; i++)
		{
			if(Now - m_aRecentLoss[i].m_Time <= (int64_t)RECENT_LOSS_SECONDS * time_freq())
				Ticks += m_aRecentLoss[i].m_Ticks;
		}
		return Ticks;
	}

	// Percentage of the expected downlink snapshots that went missing over the
	// recent window. Returns -1 while observations are stale, re-sync is pending,
	// or fresh observation time/cadence samples are insufficient. This also guards
	// reads between Update() calls from showing unknown gaps as a fake 0%.
	float RecentLossPercent(int64_t Now) const
	{
		if(m_FirstTime < 0 || m_LastTime < 0 || m_ResyncNext ||
			Now - m_LastTime >= ObservationTimeout() || !DetectionReady() || m_ExpectedStep <= 0)
			return -1.0f;
		int64_t WindowStart = Now - (int64_t)RECENT_LOSS_SECONDS * time_freq();
		if(m_FirstTime > WindowStart)
			WindowStart = m_FirstTime;
		const int64_t Observable = Now - WindowStart;
		// Two seconds is the shortest span where one missing snapshot does not
		// swing the number by tens of percent.
		if(Observable < 2 * time_freq())
			return -1.0f;
		const double ExpectedTicks = (double)Observable * (double)SERVER_TICK_SPEED / (double)time_freq();
		if(ExpectedTicks <= 0.0)
			return -1.0f;
		const double Percent = (double)RecentLossTicks(Now) * 100.0 / ExpectedTicks;
		return (float)std::clamp(Percent, 0.0, 100.0);
	}

	int BurstLossEvents() const { return m_BurstLossEvents; }
	int RandomLossEvents() const { return m_RandomLossEvents; }
	int TotalLossTicks() const { return m_TotalLostTicks; }
	int SkippedIntervals() const { return m_SkippedIntervals; }
	int ExpectedStep() const { return m_ExpectedStep; }
	int JitterSampleStep() const { return DetectionReady() ? m_ExpectedStep : 0; }
};

#endif
