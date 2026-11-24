#include <engine/client/sound_wavpack_helpers.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <vector>

struct CMockWavpackContext
{
	int m_Channels;
	uint32_t m_UnpackedFrames;
	std::vector<int32_t> m_Samples;
};

struct CMockSample
{
	short *m_pData = nullptr;
	int m_NumFrames = 0;
	int m_Rate = 0;
	int m_Channels = 0;
	int m_LoopStart = -1;
	int m_PausedAt = -1;
};

struct CMockWavpackOperations
{
	int m_CloseCount = 0;
	int m_AllocationCount = 0;
	int m_FreeCount = 0;
	int m_FailAllocation = 0;

	void Close(CMockWavpackContext *)
	{
		++m_CloseCount;
	}

	uint32_t Unpack(CMockWavpackContext *pContext, int32_t *pBuffer, uint32_t NumFrames)
	{
		const uint32_t FramesToCopy = std::min(NumFrames, pContext->m_UnpackedFrames);
		const std::size_t SamplesToCopy = static_cast<std::size_t>(FramesToCopy) * pContext->m_Channels;
		std::copy_n(pContext->m_Samples.begin(), SamplesToCopy, pBuffer);
		return pContext->m_UnpackedFrames;
	}

	void *Allocate(std::size_t Size)
	{
		++m_AllocationCount;
		if(m_AllocationCount == m_FailAllocation)
			return nullptr;
		return std::calloc(1, Size);
	}

	void Free(void *pBuffer)
	{
		++m_FreeCount;
		std::free(pBuffer);
	}
};

TEST(SoundWavpackBufferSize, ChecksSampleAndByteMultiplication)
{
	std::size_t NumSamples;
	std::size_t BufferSize;
	ASSERT_TRUE(sound_detail::GetCheckedSampleBufferSize(3, 1, sizeof(int32_t), &NumSamples, &BufferSize));
	EXPECT_EQ(NumSamples, 3);
	EXPECT_EQ(BufferSize, 3 * sizeof(int32_t));
	ASSERT_TRUE(sound_detail::GetCheckedSampleBufferSize(3, 2, sizeof(short), &NumSamples, &BufferSize));
	EXPECT_EQ(NumSamples, 6);
	EXPECT_EQ(BufferSize, 6 * sizeof(short));

	const std::size_t MaxStereoFrames = sound_detail::MAX_DECODED_SAMPLE_BYTES / (2 * sizeof(int32_t));
	EXPECT_TRUE(sound_detail::GetCheckedSampleBufferSize(MaxStereoFrames, 2, sizeof(int32_t), &NumSamples, &BufferSize));
	EXPECT_EQ(BufferSize, sound_detail::MAX_DECODED_SAMPLE_BYTES);
	EXPECT_FALSE(sound_detail::GetCheckedSampleBufferSize(MaxStereoFrames + 1, 2, sizeof(int32_t), &NumSamples, &BufferSize));
	EXPECT_FALSE(sound_detail::GetCheckedSampleBufferSize(std::numeric_limits<int64_t>::max(), 2, sizeof(int32_t), &NumSamples, &BufferSize));
	EXPECT_FALSE(sound_detail::GetCheckedSampleBufferSize(1, 2, std::numeric_limits<std::size_t>::max(), &NumSamples, &BufferSize));
}

TEST(SoundWavpackReader, BoundsInputLengthReadAndPushBack)
{
	EXPECT_FALSE(sound_detail::IsValidWavpackInputLength(0));
	EXPECT_TRUE(sound_detail::IsValidWavpackInputLength(std::numeric_limits<int>::max()));
	EXPECT_FALSE(sound_detail::IsValidWavpackInputLength(static_cast<uint64_t>(std::numeric_limits<int>::max()) + 1));

	EXPECT_EQ(sound_detail::GetWavpackReadSize(4, 10, 4), 4);
	EXPECT_EQ(sound_detail::GetWavpackReadSize(100, 10, 4), 6);
	EXPECT_EQ(sound_detail::GetWavpackReadSize(0, 10, 4), 0);
	EXPECT_EQ(sound_detail::GetWavpackReadSize(-1, 10, 4), 0);
	EXPECT_EQ(sound_detail::GetWavpackReadSize(1, 0, 0), 0);
	EXPECT_EQ(sound_detail::GetWavpackReadSize(1, 10, -1), 0);
	EXPECT_EQ(sound_detail::GetWavpackReadSize(1, 10, 10), 0);
	EXPECT_EQ(sound_detail::GetWavpackReadSize(1, 10, 11), 0);

	int Position = 0;
	EXPECT_FALSE(sound_detail::TryPushBackWavpackByte(10, &Position));
	EXPECT_EQ(Position, 0);
	Position = 1;
	EXPECT_TRUE(sound_detail::TryPushBackWavpackByte(10, &Position));
	EXPECT_EQ(Position, 0);
	Position = 11;
	EXPECT_FALSE(sound_detail::TryPushBackWavpackByte(10, &Position));
	EXPECT_EQ(Position, 11);
	EXPECT_FALSE(sound_detail::TryPushBackWavpackByte(10, nullptr));
}

TEST(SoundWavpackDecode, RejectsInvalidMetadataAndClosesContext)
{
	for(const int Channels : {0, 3})
	{
		CMockSample Sample;
		CMockWavpackContext Context{Channels, 1, {1}};
		CMockWavpackOperations Operations;
		EXPECT_EQ(sound_detail::DecodeWavpackSamples(Sample, &Context, 48000, 16, Channels, 1, Operations), sound_detail::EWavpackDecodeResult::INVALID_CHANNEL_COUNT);
		EXPECT_EQ(Operations.m_CloseCount, 1);
		EXPECT_EQ(Operations.m_AllocationCount, 0);
	}

	CMockSample Sample;
	CMockWavpackContext Context{1, 1, {1}};
	CMockWavpackOperations Operations;
	EXPECT_EQ(sound_detail::DecodeWavpackSamples(Sample, &Context, 0, 16, 1, 1, Operations), sound_detail::EWavpackDecodeResult::INVALID_SAMPLE_RATE);
	EXPECT_EQ(Operations.m_CloseCount, 1);
	EXPECT_EQ(Operations.m_AllocationCount, 0);

	CMockSample OversizedSample;
	CMockWavpackContext OversizedContext{1, 1, {1}};
	CMockWavpackOperations OversizedOperations;
	const int64_t TooManyFrames = sound_detail::MAX_DECODED_SAMPLE_BYTES / sizeof(int32_t) + 1;
	EXPECT_EQ(sound_detail::DecodeWavpackSamples(OversizedSample, &OversizedContext, 48000, 16, 1, TooManyFrames, OversizedOperations), sound_detail::EWavpackDecodeResult::SAMPLE_BUFFER_TOO_LARGE);
	EXPECT_EQ(OversizedOperations.m_CloseCount, 1);
	EXPECT_EQ(OversizedOperations.m_AllocationCount, 0);
}

TEST(SoundWavpackDecode, ClosesAndFreesBuffersOnAllocationFailures)
{
	for(const int FailAllocation : {1, 2})
	{
		CMockSample Sample;
		CMockWavpackContext Context{1, 1, {7}};
		CMockWavpackOperations Operations;
		Operations.m_FailAllocation = FailAllocation;
		const sound_detail::EWavpackDecodeResult Expected = FailAllocation == 1 ? sound_detail::EWavpackDecodeResult::INPUT_ALLOCATION_FAILED : sound_detail::EWavpackDecodeResult::OUTPUT_ALLOCATION_FAILED;
		EXPECT_EQ(sound_detail::DecodeWavpackSamples(Sample, &Context, 48000, 16, 1, 1, Operations), Expected);
		EXPECT_EQ(Operations.m_CloseCount, 1);
		EXPECT_EQ(Operations.m_AllocationCount, FailAllocation);
		EXPECT_EQ(Operations.m_FreeCount, FailAllocation == 1 ? 0 : 1);
		EXPECT_EQ(Sample.m_pData, nullptr);
	}
}

TEST(SoundWavpackDecode, RejectsPartialUnpackAndReleasesInput)
{
	CMockSample Sample;
	CMockWavpackContext Context{2, 2, {1, 2, 3, 4, 5, 6}};
	CMockWavpackOperations Operations;
	EXPECT_EQ(sound_detail::DecodeWavpackSamples(Sample, &Context, 48000, 16, 2, 3, Operations), sound_detail::EWavpackDecodeResult::INCOMPLETE_DECODE);
	EXPECT_EQ(Operations.m_CloseCount, 1);
	EXPECT_EQ(Operations.m_AllocationCount, 1);
	EXPECT_EQ(Operations.m_FreeCount, 1);
	EXPECT_EQ(Sample.m_pData, nullptr);
}

TEST(SoundWavpackDecode, PublishesOnlyCompletelyDecodedSamples)
{
	CMockSample Sample;
	CMockWavpackContext Context{2, 2, {123, -123, 32767, -32768}};
	CMockWavpackOperations Operations;
	EXPECT_EQ(sound_detail::DecodeWavpackSamples(Sample, &Context, 44100, 16, 2, 2, Operations), sound_detail::EWavpackDecodeResult::SUCCESS);
	ASSERT_NE(Sample.m_pData, nullptr);
	EXPECT_EQ(Sample.m_NumFrames, 2);
	EXPECT_EQ(Sample.m_Rate, 44100);
	EXPECT_EQ(Sample.m_Channels, 2);
	EXPECT_EQ(Sample.m_LoopStart, 0);
	EXPECT_EQ(Sample.m_PausedAt, 0);
	EXPECT_EQ(Sample.m_pData[0], 123);
	EXPECT_EQ(Sample.m_pData[1], -123);
	EXPECT_EQ(Sample.m_pData[2], 32767);
	EXPECT_EQ(Sample.m_pData[3], -32768);
	EXPECT_EQ(Operations.m_CloseCount, 1);
	EXPECT_EQ(Operations.m_FreeCount, 1);
	Operations.Free(Sample.m_pData);
	EXPECT_EQ(Operations.m_FreeCount, 2);
}
