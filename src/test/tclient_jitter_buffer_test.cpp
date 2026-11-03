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
