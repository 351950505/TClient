/* TClient: 自适应快照插值缓冲。估计器与每连接状态都不依赖客户端接口，便于单测。 */
#ifndef ENGINE_CLIENT_JITTER_BUFFER_H
#define ENGINE_CLIENT_JITTER_BUFFER_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>

namespace TcJitterBuffer
{
constexpr int MIN_SAMPLES = 8;
constexpr int MAX_INTERVAL_MS = 200;
constexpr int MIN_INTERVAL_MS = 5;

// Twice the mean absolute deviation, rounded up to an even number of ms.
inline int TargetMs(const int *pIntervalsMs, int Count, int MaxMs)
{
	if(!pIntervalsMs || Count < MIN_SAMPLES || MaxMs <= 0)
		return 0;
	int Valid = 0;
	long long Sum = 0;
	for(int i = 0; i < Count; i++)
	{
		if(pIntervalsMs[i] < MIN_INTERVAL_MS || pIntervalsMs[i] > MAX_INTERVAL_MS)
			continue;
		Sum += pIntervalsMs[i];
		Valid++;
	}
	if(Valid < MIN_SAMPLES)
		return 0;
	const long long Mean = Sum / Valid;
	long long Deviation = 0;
	for(int i = 0; i < Count; i++)
	{
		if(pIntervalsMs[i] < MIN_INTERVAL_MS || pIntervalsMs[i] > MAX_INTERVAL_MS)
			continue;
		Deviation += std::abs((long long)pIntervalsMs[i] - Mean);
	}
	const long long Target = ((Deviation * 2 + Valid - 1) / Valid + 1) & ~1;
	return (int)std::clamp(Target, 0LL, (long long)MaxMs);
}
} // namespace TcJitterBuffer

class CAdaptiveJitterBuffer
{
	static constexpr int SAMPLE_CAPACITY = 128;
	static constexpr int SAMPLE_MAX_AGE_MS = 2500;
	static constexpr int GAP_HOLD_MS = 200;
	static constexpr int LOCAL_STALL_MS = 75;
	static constexpr double ATTACK_MS_PER_SECOND = 50.0;
	static constexpr double RELEASE_MS_PER_SECOND = 10.0;
	static constexpr double DEAD_ZONE_MS = 2.0;

	struct CSample
	{
		int m_IntervalMs = 0;
		int64_t m_Time = 0;
	};

	std::array<CSample, SAMPLE_CAPACITY> m_aSamples{};
	int m_NumSamples = 0;
	int m_NextSample = 0;
	int m_LastTick = -1;
	int m_ExpectedStep = 0;
	int64_t m_LastArrival = -1;
	int64_t m_LastUpdate = -1;
	int64_t m_DisruptionSince = -1;
	bool m_ResyncInterval = false;
	bool m_HasTarget = false;
	int m_TargetMs = 0;
	double m_BufferMs = 0.0;

	static double Milliseconds(int64_t Delta, int64_t Frequency)
	{
		return (double)Delta * 1000.0 / (double)Frequency;
	}

	void ClearSamples()
	{
		m_NumSamples = 0;
		m_NextSample = 0;
		m_TargetMs = 0;
		m_HasTarget = false;
	}

	void OnDisruption(int64_t Now, bool DiscardSamples = true)
	{
		if(DiscardSamples)
			ClearSamples();
		m_ResyncInterval = true;
		// Repeated gaps must not keep extending the hold indefinitely.
		if(m_DisruptionSince < 0)
			m_DisruptionSince = Now;
	}

	void RefreshTarget()
	{
		int aIntervals[SAMPLE_CAPACITY];
		for(int i = 0; i < m_NumSamples; i++)
		{
			const int Index = (m_NextSample + SAMPLE_CAPACITY - m_NumSamples + i) % SAMPLE_CAPACITY;
			aIntervals[i] = m_aSamples[Index].m_IntervalMs;
		}
		m_HasTarget = m_NumSamples >= TcJitterBuffer::MIN_SAMPLES;
		m_TargetMs = TcJitterBuffer::TargetMs(aIntervals, m_NumSamples, 100);
	}

	void ExpireSamples(int64_t Now, int64_t Frequency)
	{
		bool Expired = false;
		while(m_NumSamples > 0)
		{
			const int Oldest = (m_NextSample + SAMPLE_CAPACITY - m_NumSamples) % SAMPLE_CAPACITY;
			if(Milliseconds(Now - m_aSamples[Oldest].m_Time, Frequency) <= SAMPLE_MAX_AGE_MS)
				break;
			m_NumSamples--;
			Expired = true;
		}
		if(Expired)
			RefreshTarget();
	}

	void AddSample(int IntervalMs, int64_t Now, int64_t Frequency)
	{
		ExpireSamples(Now, Frequency);
		m_aSamples[m_NextSample] = {IntervalMs, Now};
		m_NextSample = (m_NextSample + 1) % SAMPLE_CAPACITY;
		m_NumSamples = std::min(m_NumSamples + 1, SAMPLE_CAPACITY);
		RefreshTarget();
		if(m_HasTarget)
			m_DisruptionSince = -1;
	}

public:
	void Reset()
	{
		// Sample storage need not be cleared: only entries counted after reset are read.
		ClearSamples();
		m_LastTick = -1;
		m_ExpectedStep = 0;
		m_LastArrival = -1;
		m_LastUpdate = -1;
		m_DisruptionSince = -1;
		m_ResyncInterval = false;
		m_BufferMs = 0.0;
	}
	int BufferMs() const { return (int)(m_BufferMs + 0.5); }

	// ExpectedStep is the learned normal cadence (1 or 2 ticks), not an assertion
	// that a larger tick gap was network packet loss. Zero means cadence is unknown.
	void OnSnapshot(int Tick, int ExpectedStep, int64_t Now, int64_t Frequency, int MaxMs)
	{
		if(Tick < 0 || Now < 0 || Frequency <= 0)
		{
			Reset();
			return;
		}
		if((m_LastTick >= 0 && Tick < m_LastTick) || (m_LastArrival >= 0 && Now < m_LastArrival))
			Reset();
		if(m_LastArrival < 0)
		{
			m_LastTick = Tick;
			m_ExpectedStep = ExpectedStep;
			m_LastArrival = Now;
			Update(Now, Frequency, MaxMs);
			return;
		}
		if(Tick <= m_LastTick)
		{
			OnDisruption(Now);
			m_LastTick = Tick;
			m_ExpectedStep = ExpectedStep;
			m_LastArrival = Now;
			Update(Now, Frequency, MaxMs);
			return;
		}

		const double IntervalMs = Milliseconds(Now - m_LastArrival, Frequency);
		const bool NormalCadence = (ExpectedStep == 1 || ExpectedStep == 2) && ExpectedStep == m_ExpectedStep;
		const int64_t TickDelta = (int64_t)Tick - m_LastTick;
		if(!NormalCadence || TickDelta < ExpectedStep ||
			IntervalMs < TcJitterBuffer::MIN_INTERVAL_MS || IntervalMs > TcJitterBuffer::MAX_INTERVAL_MS)
			OnDisruption(Now);
		else if(TickDelta > ExpectedStep)
			// A short gap invalidates this interval, not recent continuous samples.
			OnDisruption(Now, false);
		else if(m_ResyncInterval)
			m_ResyncInterval = false;
		else
			AddSample((int)(IntervalMs + 0.5), Now, Frequency);

		m_LastTick = Tick;
		m_ExpectedStep = ExpectedStep;
		m_LastArrival = Now;
		Update(Now, Frequency, MaxMs);
	}

	// Advance from the client loop as well as snapshot arrival, so stale data can
	// decay without waiting for a packet. Rates are in wall-clock time, not packets.
	void Update(int64_t Now, int64_t Frequency, int MaxMs)
	{
		if(Now < 0 || Frequency <= 0 || (m_LastUpdate >= 0 && Now < m_LastUpdate))
		{
			Reset();
			return;
		}
		MaxMs = std::clamp(MaxMs, 0, 100);
		m_BufferMs = std::clamp(m_BufferMs, 0.0, (double)MaxMs);
		ExpireSamples(Now, Frequency);
		const double ElapsedSeconds = m_LastUpdate >= 0 ? (double)(Now - m_LastUpdate) / (double)Frequency : 0.0;
		m_LastUpdate = Now;
		if(ElapsedSeconds * 1000.0 > LOCAL_STALL_MS)
		{
			// Update runs before queued snapshots are drained. Discard the first
			// arrival interval after a local stall rather than blaming the network.
			OnDisruption(Now);
		}
		if(m_LastArrival >= 0 && Milliseconds(Now - m_LastArrival, Frequency) > TcJitterBuffer::MAX_INTERVAL_MS)
			OnDisruption(m_LastArrival);
		if(m_DisruptionSince >= 0 && Milliseconds(Now - m_DisruptionSince, Frequency) <= GAP_HOLD_MS)
			return;

		const int Target = m_HasTarget ? std::min(m_TargetMs, MaxMs) : 0;
		double Desired = (double)Target;
		if(Target > 0)
		{
			if(Desired > m_BufferMs)
				Desired = std::max(m_BufferMs, Desired - DEAD_ZONE_MS);
			else
				Desired = std::min(m_BufferMs, Desired + DEAD_ZONE_MS);
		}
		// Retained samples may sustain the delay across a short gap, but only a
		// fresh valid interval may enable further growth.
		if(m_DisruptionSince >= 0)
			Desired = std::min(Desired, m_BufferMs);
		if(Desired > m_BufferMs)
			m_BufferMs = std::min(Desired, m_BufferMs + ATTACK_MS_PER_SECOND * ElapsedSeconds);
		else
			m_BufferMs = std::max(Desired, m_BufferMs - RELEASE_MS_PER_SECOND * ElapsedSeconds);
	}
};

#endif
