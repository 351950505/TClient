#include <engine/client/jitter_buffer.h>

#include <gtest/gtest.h>

TEST(TcJitterBuffer, NotEnoughSamplesReturnsZero)
{
	const int aStable[4] = {20, 20, 20, 20};
	EXPECT_EQ(TcJitterBuffer::TargetMs(aStable, 4, 30), 0);
	EXPECT_EQ(TcJitterBuffer::TargetMs(nullptr, 10, 30), 0);
}

TEST(TcJitterBuffer, StableNetworkNeedsNoBuffer)
{
	int aStable[TcJitterBuffer::MIN_SAMPLES + 4];
	for(int &v : aStable)
		v = 20;
	EXPECT_EQ(TcJitterBuffer::TargetMs(aStable, std::size(aStable), 30), 0);
}

TEST(TcJitterBuffer, JitteryNetworkGetsBuffer)
{
	// 20ms 均值上 ±12ms 抖动：MAD=12 → 目标 = 2*MAD 对齐偶数 = 24
	int aJittery[] = {8, 32, 8, 32, 8, 32, 8, 32, 8, 32, 8, 32};
	EXPECT_EQ(TcJitterBuffer::TargetMs(aJittery, std::size(aJittery), 30), 24);
	EXPECT_EQ(TcJitterBuffer::TargetMs(aJittery, std::size(aJittery), 100), 24);
}

TEST(TcJitterBuffer, ClampsToMax)
{
	// 30ms 均值上 ±24ms 抖动：目标 48，被 Max 钳住
	int aWild[] = {6, 54, 6, 54, 6, 54, 6, 54, 6, 54, 6, 54};
	EXPECT_EQ(TcJitterBuffer::TargetMs(aWild, std::size(aWild), 30), 30);
	EXPECT_EQ(TcJitterBuffer::TargetMs(aWild, std::size(aWild), 0), 0);
}

TEST(TcJitterBuffer, InvalidSamplesAreDiscarded)
{
	// 带入断线尖峰样本（>200ms 或 <5ms）应被丢弃，不抬均值也不触发缓冲
	int aSpiky[] = {20, 20, 20, 20, 20, 20, 20, 20, 500, 1, 500, 1};
	EXPECT_EQ(TcJitterBuffer::TargetMs(aSpiky, std::size(aSpiky), 30), 0);
}

static int FeedJitter(CAdaptiveJitterBuffer &Buffer, int TickStep, int64_t &NowMs, int Samples, int MaxMs = 100)
{
	int Tick = 0;
	Buffer.Reset();
	Buffer.OnSnapshot(Tick, TickStep, 0, 1'000'000, MaxMs);
	for(int i = 0; i < Samples; i++)
	{
		Tick += TickStep;
		const bool LongInterval = (i & 1) != 0;
		NowMs += TickStep == 1 ? (LongInterval ? 35 : 5) : (LongInterval ? 55 : 25);
		Buffer.OnSnapshot(Tick, TickStep, NowMs * 1000, 1'000'000, MaxMs);
	}
	return Buffer.BufferMs();
}

TEST(TcJitterBuffer, OneAndTwoTickCadencesAdaptOnWallClockTime)
{
	CAdaptiveJitterBuffer EveryTick;
	CAdaptiveJitterBuffer EveryOtherTick;
	int64_t EveryTickNow = 0;
	int64_t EveryOtherTickNow = 0;
	// Warm both estimators for the same two seconds with a cap inside the dead zone.
	// This separates adaptation speed from the different eight-snapshot warmup times.
	EXPECT_EQ(FeedJitter(EveryTick, 1, EveryTickNow, 100, 2), 0);
	EXPECT_EQ(FeedJitter(EveryOtherTick, 2, EveryOtherTickNow, 50, 2), 0);
	for(int i = 0; i < 20; i++)
	{
		EveryTickNow += (i & 1) ? 35 : 5;
		EveryTick.OnSnapshot(101 + i, 1, EveryTickNow * 1000, 1'000'000, 100);
	}
	for(int i = 0; i < 10; i++)
	{
		EveryOtherTickNow += (i & 1) ? 55 : 25;
		EveryOtherTick.OnSnapshot(102 + i * 2, 2, EveryOtherTickNow * 1000, 1'000'000, 100);
	}
	EXPECT_EQ(EveryTickNow, 2400);
	EXPECT_EQ(EveryOtherTickNow, 2400);
	EXPECT_EQ(EveryTick.BufferMs(), 20);
	EXPECT_EQ(EveryOtherTick.BufferMs(), 20);
}

TEST(TcJitterBuffer, MissingSnapshotDoesNotRaiseOrClearCurrentBuffer)
{
	CAdaptiveJitterBuffer Buffer;
	int64_t NowMs = 0;
	const int BeforeGap = FeedJitter(Buffer, 2, NowMs, 25);
	ASSERT_GT(BeforeGap, 0);

	// Server normally sends every two ticks. Delta four means one snapshot gap.
	Buffer.OnSnapshot(54, 2, (NowMs + 100) * 1000, 1'000'000, 30);
	EXPECT_EQ(Buffer.BufferMs(), BeforeGap);

	// The first post-gap interval is only a new baseline; it is not jitter evidence.
	Buffer.OnSnapshot(56, 2, (NowMs + 140) * 1000, 1'000'000, 30);
	EXPECT_EQ(Buffer.BufferMs(), BeforeGap);
}

TEST(TcJitterBuffer, ConnectionsKeepIndependentSampleHistory)
{
	CAdaptiveJitterBuffer Main;
	CAdaptiveJitterBuffer Dummy;
	int64_t MainNow = 0;
	int64_t DummyNow = 0;
	const int MainBuffer = FeedJitter(Main, 2, MainNow, 25);
	int DummyTick = 0;
	Dummy.OnSnapshot(DummyTick, 2, 0, 1'000'000, 30);
	for(int i = 0; i < 25; i++)
	{
		DummyTick += 2;
		DummyNow += 40;
		Dummy.OnSnapshot(DummyTick, 2, DummyNow * 1000, 1'000'000, 30);
	}
	EXPECT_GT(MainBuffer, 0);
	EXPECT_EQ(Dummy.BufferMs(), 0);
}

TEST(TcJitterBuffer, LocalFramePauseIsNotNetworkJitter)
{
	CAdaptiveJitterBuffer Buffer;
	for(int Tick = 0; Tick <= 16; ++Tick)
		Buffer.OnSnapshot(Tick, 1, Tick * 20'000, 1'000'000, 30);
	ASSERT_EQ(Buffer.BufferMs(), 0);

	// The client loop must observe its pause before it drains queued snapshots.
	Buffer.Update(440'000, 1'000'000, 30);
	Buffer.OnSnapshot(17, 1, 440'000, 1'000'000, 30);
	Buffer.Update(460'000, 1'000'000, 30);
	EXPECT_EQ(Buffer.BufferMs(), 0);
}

TEST(TcJitterBuffer, DelayedSnapshotWithResponsiveFramesRemainsJitterEvidence)
{
	CAdaptiveJitterBuffer Buffer;
	for(int Tick = 0; Tick <= 16; ++Tick)
		Buffer.OnSnapshot(Tick, 1, Tick * 20'000, 1'000'000, 30);
	for(int64_t Now = 330'000; Now <= 440'000; Now += 10'000)
		Buffer.Update(Now, 1'000'000, 30);
	Buffer.OnSnapshot(17, 1, 440'000, 1'000'000, 30);
	Buffer.Update(460'000, 1'000'000, 30);
	EXPECT_GT(Buffer.BufferMs(), 0);
}

TEST(TcJitterBuffer, RepeatedLocalStallsDoNotHoldOldDelayForever)
{
	CAdaptiveJitterBuffer Buffer;
	int64_t NowMs = 0;
	ASSERT_GT(FeedJitter(Buffer, 2, NowMs, 50, 30), 0);
	for(int i = 0; i < 60; ++i)
	{
		NowMs += 100;
		Buffer.Update(NowMs * 1000, 1'000'000, 30);
	}
	EXPECT_EQ(Buffer.BufferMs(), 0);
}

TEST(TcJitterBuffer, RepeatedTickGapsCannotRaiseOrPermanentlyFreezeDelay)
{
	CAdaptiveJitterBuffer Buffer;
	int64_t NowMs = 0;
	int Previous = FeedJitter(Buffer, 2, NowMs, 50, 30);
	ASSERT_GT(Previous, 0);
	int Tick = 100;
	for(int i = 0; i < 60; ++i)
	{
		NowMs += 80;
		Tick += 4;
		Buffer.OnSnapshot(Tick, 2, NowMs * 1000, 1'000'000, 30);
		EXPECT_LE(Buffer.BufferMs(), Previous);
		Previous = Buffer.BufferMs();
	}
	EXPECT_EQ(Buffer.BufferMs(), 0);
}

TEST(TcJitterBuffer, SilentConnectionReleasesOldDelay)
{
	CAdaptiveJitterBuffer Buffer;
	int64_t NowMs = 0;
	ASSERT_GT(FeedJitter(Buffer, 2, NowMs, 50, 30), 0);
	for(int i = 0; i < 300; ++i)
	{
		NowMs += 20;
		Buffer.Update(NowMs * 1000, 1'000'000, 30);
	}
	EXPECT_EQ(Buffer.BufferMs(), 0);
}

TEST(TcJitterBuffer, RingWrapAndResetStayConnectionLocal)
{
	CAdaptiveJitterBuffer aBuffers[2];
	int64_t NowMs = 0;
	aBuffers[0].OnSnapshot(0, 2, 0, 1'000'000, 30);
	aBuffers[1].OnSnapshot(0, 2, 0, 1'000'000, 30);
	for(int i = 1; i <= 512; ++i)
	{
		NowMs += (i & 1) ? 25 : 55;
		aBuffers[0].OnSnapshot(i * 2, 2, NowMs * 1000, 1'000'000, 30);
		aBuffers[1].OnSnapshot(i * 2, 2, i * 40'000LL, 1'000'000, 30);
	}
	ASSERT_GT(aBuffers[0].BufferMs(), 0);
	EXPECT_EQ(aBuffers[1].BufferMs(), 0);
	const int MainDelay = aBuffers[0].BufferMs();
	aBuffers[1].Reset();
	EXPECT_EQ(aBuffers[0].BufferMs(), MainDelay);
	EXPECT_EQ(aBuffers[1].BufferMs(), 0);
	aBuffers[0].Reset();
	EXPECT_EQ(aBuffers[0].BufferMs(), 0);
}

TEST(TcJitterBuffer, UnknownAndRecoveryCadencesDoNotCreateJitter)
{
	CAdaptiveJitterBuffer Buffer;
	Buffer.OnSnapshot(0, 0, 0, 1'000'000, 30);
	int64_t Now = 0;
	int Tick = 0;
	for(int i = 0; i < 15; ++i)
	{
		Tick += 10;
		Now += 200'000;
		Buffer.OnSnapshot(Tick, 0, Now, 1'000'000, 30);
		EXPECT_EQ(Buffer.BufferMs(), 0);
	}
	for(int i = 0; i < 3; ++i)
	{
		Tick += 50;
		Now += 1'000'000;
		Buffer.OnSnapshot(Tick, 0, Now, 1'000'000, 30);
		EXPECT_EQ(Buffer.BufferMs(), 0);
	}
}

TEST(TcJitterBuffer, CadenceChangeDiscardsOldJitterEvidence)
{
	CAdaptiveJitterBuffer Buffer;
	int64_t NowMs = 0;
	const int Before = FeedJitter(Buffer, 2, NowMs, 50, 30);
	ASSERT_GT(Before, 0);
	for(int i = 1; i <= 300; ++i)
	{
		NowMs += 20;
		Buffer.OnSnapshot(100 + i, 1, NowMs * 1000, 1'000'000, 30);
		EXPECT_LE(Buffer.BufferMs(), Before);
	}
	EXPECT_EQ(Buffer.BufferMs(), 0);
}

TEST(TcJitterBuffer, LoweredCapAndClockRestartAreRespected)
{
	CAdaptiveJitterBuffer Buffer;
	int64_t NowMs = 0;
	ASSERT_GT(FeedJitter(Buffer, 2, NowMs, 50, 30), 0);
	Buffer.Update(NowMs * 1000, 1'000'000, 5);
	EXPECT_LE(Buffer.BufferMs(), 5);
	Buffer.Update(NowMs * 1000, 1'000'000, 0);
	EXPECT_EQ(Buffer.BufferMs(), 0);
	Buffer.OnSnapshot(0, 2, 0, 1'000'000, 30);
	EXPECT_EQ(Buffer.BufferMs(), 0);
}

TEST(TcJitterBuffer, StableCadenceReleasesPreviousJitterDelay)
{
	CAdaptiveJitterBuffer Buffer;
	int64_t NowMs = 0;
	ASSERT_GT(FeedJitter(Buffer, 2, NowMs, 50, 30), 0);
	for(int i = 1; i <= 250; ++i)
	{
		NowMs += 40;
		Buffer.OnSnapshot(100 + i * 2, 2, NowMs * 1000, 1'000'000, 30);
	}
	EXPECT_EQ(Buffer.BufferMs(), 0);
}

TEST(TcJitterBuffer, RepeatedUpdatesAtTheSameTimeCannotMultiplyTheRate)
{
	CAdaptiveJitterBuffer Buffer;
	int64_t NowMs = 0;
	ASSERT_GT(FeedJitter(Buffer, 2, NowMs, 50, 30), 0);
	const int Before = Buffer.BufferMs();
	for(int i = 0; i < 1000; ++i)
		Buffer.Update(NowMs * 1000, 1'000'000, 30);
	EXPECT_EQ(Buffer.BufferMs(), Before);
}

TEST(TcJitterBuffer, BatchedSnapshotsDoNotCreateAFalseDelay)
{
	CAdaptiveJitterBuffer Buffer;
	for(int Tick = 0; Tick <= 16; ++Tick)
		Buffer.OnSnapshot(Tick, 1, Tick * 20'000, 1'000'000, 30);
	Buffer.Update(440'000, 1'000'000, 30);
	for(int Tick = 17; Tick <= 22; ++Tick)
		Buffer.OnSnapshot(Tick, 1, 440'000, 1'000'000, 30);
	for(int Tick = 23; Tick <= 60; ++Tick)
		Buffer.OnSnapshot(Tick, 1, 440'000 + (Tick - 22) * 20'000, 1'000'000, 30);
	EXPECT_EQ(Buffer.BufferMs(), 0);
}

static void ArriveWithResponsiveFrames(CAdaptiveJitterBuffer &Buffer, int Tick, int Step, int64_t Arrival, int64_t &FrameTime)
{
	while(FrameTime + 10'000 < Arrival)
	{
		FrameTime += 10'000;
		Buffer.Update(FrameTime, 1'000'000, 30);
	}
	Buffer.OnSnapshot(Tick, Step, Arrival, 1'000'000, 30);
	FrameTime = Arrival;
}

TEST(TcJitterBuffer, PeriodicGapsPreserveFreshJitterEvidence)
{
	CAdaptiveJitterBuffer Buffer;
	Buffer.OnSnapshot(0, 2, 0, 1'000'000, 30);
	int Tick = 0;
	int64_t FrameTime = 0;
	int LowestDelay = 30;
	for(int i = 1; i <= 450; ++i)
	{
		// After warmup, fewer than eight consecutive samples exist between gaps.
		Tick += i > 50 && i % 7 == 0 ? 4 : 2;
		const int64_t Arrival = Tick * 20'000LL + ((i & 1) ? -15'000 : 15'000);
		ArriveWithResponsiveFrames(Buffer, Tick, 2, Arrival, FrameTime);
		if(i == 50)
			ASSERT_GE(Buffer.BufferMs(), 20);
		if(i > 100)
			LowestDelay = std::min(LowestDelay, Buffer.BufferMs());
	}
	EXPECT_GE(LowestDelay, 20);
	EXPECT_GE(Buffer.BufferMs(), 20);
}

TEST(TcJitterBuffer, PeriodicGapsCanWarmUpFromFreshContinuousIntervals)
{
	CAdaptiveJitterBuffer Buffer;
	Buffer.OnSnapshot(0, 2, 0, 1'000'000, 30);
	int Tick = 0;
	int64_t FrameTime = 0;
	for(int i = 1; i <= 300; ++i)
	{
		Tick += i % 7 == 0 ? 4 : 2;
		const int64_t Arrival = Tick * 20'000LL + ((i & 1) ? -15'000 : 15'000);
		ArriveWithResponsiveFrames(Buffer, Tick, 2, Arrival, FrameTime);
	}
	EXPECT_GE(Buffer.BufferMs(), 20);
}

TEST(TcJitterBuffer, RecurrentGapsWithoutFreshSamplesCannotIncreaseDelay)
{
	CAdaptiveJitterBuffer Buffer;
	int64_t NowMs = 0;
	ASSERT_GT(FeedJitter(Buffer, 2, NowMs, 10, 30), 0);
	int64_t FrameTime = NowMs * 1000;
	int Tick = 20;
	int Previous = 0;
	for(int i = 0; i < 80; ++i)
	{
		Tick += 4;
		// Keep the frame loop responsive while the snapshot stream has gaps.
		const int64_t Arrival = (NowMs += 80) * 1000;
		ArriveWithResponsiveFrames(Buffer, Tick, 2, Arrival, FrameTime);
		if(i > 0)
			EXPECT_LE(Buffer.BufferMs(), Previous);
		Previous = Buffer.BufferMs();
	}
	EXPECT_EQ(Buffer.BufferMs(), 0);
}

TEST(TcJitterBuffer, MixedGapSamplesExpireAfterNetworkCalms)
{
	CAdaptiveJitterBuffer Buffer;
	Buffer.OnSnapshot(0, 2, 0, 1'000'000, 30);
	int Tick = 0;
	int64_t FrameTime = 0;
	for(int i = 1; i <= 300; ++i)
	{
		Tick += i % 7 == 0 ? 4 : 2;
		const int64_t Arrival = Tick * 20'000LL + ((i & 1) ? -15'000 : 15'000);
		ArriveWithResponsiveFrames(Buffer, Tick, 2, Arrival, FrameTime);
	}
	ASSERT_GE(Buffer.BufferMs(), 20);

	// Gaps continue, but must not preserve expired jitter samples.
	int64_t Arrival = FrameTime;
	for(int i = 1; i <= 250; ++i)
	{
		const int TickStep = i % 7 == 0 ? 4 : 2;
		Tick += TickStep;
		Arrival += TickStep * 20'000LL;
		ArriveWithResponsiveFrames(Buffer, Tick, 2, Arrival, FrameTime);
	}
	EXPECT_EQ(Buffer.BufferMs(), 0);
}
