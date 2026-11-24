#ifndef ENGINE_CLIENT_SOUND_WAVPACK_HELPERS_H
#define ENGINE_CLIENT_SOUND_WAVPACK_HELPERS_H

#include <cstddef>
#include <cstdint>
#include <limits>

namespace sound_detail
{
	constexpr std::size_t MAX_DECODED_SAMPLE_BYTES = std::size_t{256} << 20;

	inline bool IsValidWavpackInputLength(uint64_t Length)
	{
		return Length > 0 && Length <= static_cast<uint64_t>(std::numeric_limits<int>::max());
	}

	inline int GetWavpackReadSize(int RequestedSize, int BufferSize, int Position)
	{
		if(RequestedSize <= 0 || BufferSize <= 0 || Position < 0 || Position >= BufferSize)
			return 0;

		const int Remaining = BufferSize - Position;
		return RequestedSize < Remaining ? RequestedSize : Remaining;
	}

	inline bool TryPushBackWavpackByte(int BufferSize, int *pPosition)
	{
		if(!pPosition || BufferSize <= 0 || *pPosition <= 0 || *pPosition > BufferSize)
			return false;

		--*pPosition;
		return true;
	}

	enum class EWavpackDecodeResult
	{
		SUCCESS,
		INVALID_CHANNEL_COUNT,
		INVALID_SAMPLE_RATE,
		INVALID_BITS_PER_SAMPLE,
		INVALID_SAMPLE_COUNT,
		SAMPLE_BUFFER_TOO_LARGE,
		INPUT_ALLOCATION_FAILED,
		INCOMPLETE_DECODE,
		OUTPUT_ALLOCATION_FAILED,
		INVALID_CONTEXT,
	};

	inline bool GetCheckedSampleBufferSize(int64_t NumFrames, int NumChannels, std::size_t BytesPerSample, std::size_t *pNumSamples, std::size_t *pBufferSize)
	{
		if(NumFrames <= 0 || NumChannels < 1 || NumChannels > 2 || BytesPerSample == 0)
			return false;

		const uint64_t Frames = static_cast<uint64_t>(NumFrames);
		const std::size_t Channels = static_cast<std::size_t>(NumChannels);
		if(Frames > std::numeric_limits<std::size_t>::max() / Channels)
			return false;

		const std::size_t NumSamples = static_cast<std::size_t>(Frames) * Channels;
		if(NumSamples > std::numeric_limits<std::size_t>::max() / BytesPerSample)
			return false;

		const std::size_t BufferSize = NumSamples * BytesPerSample;
		if(BufferSize > MAX_DECODED_SAMPLE_BYTES)
			return false;

		*pNumSamples = NumSamples;
		*pBufferSize = BufferSize;
		return true;
	}

	template<typename Context, typename Operations>
	class CWavpackContextOwner
	{
		Context *m_pContext;
		Operations &m_Operations;

	public:
		CWavpackContextOwner(Context *pContext, Operations &Operations) :
			m_pContext(pContext), m_Operations(Operations)
		{
		}
		~CWavpackContextOwner()
		{
			if(m_pContext)
				m_Operations.Close(m_pContext);
		}

		CWavpackContextOwner(const CWavpackContextOwner &) = delete;
		CWavpackContextOwner &operator=(const CWavpackContextOwner &) = delete;
	};

	template<typename T, typename Operations>
	class CWavpackBufferOwner
	{
		T *m_pBuffer;
		Operations &m_Operations;

	public:
		CWavpackBufferOwner(T *pBuffer, Operations &Operations) :
			m_pBuffer(pBuffer), m_Operations(Operations)
		{
		}
		~CWavpackBufferOwner()
		{
			if(m_pBuffer)
				m_Operations.Free(m_pBuffer);
		}

		T *get() const { return m_pBuffer; }
		T *release()
		{
			T *pBuffer = m_pBuffer;
			m_pBuffer = nullptr;
			return pBuffer;
		}

		CWavpackBufferOwner(const CWavpackBufferOwner &) = delete;
		CWavpackBufferOwner &operator=(const CWavpackBufferOwner &) = delete;
	};

	template<typename Sample, typename Context, typename Operations>
	EWavpackDecodeResult DecodeWavpackSamples(Sample &DecodedSample, Context *pContext, unsigned int SampleRate, int BitsPerSample, int NumChannels, int64_t NumFrames, Operations &Ops)
	{
		if(!pContext)
			return EWavpackDecodeResult::INVALID_CONTEXT;
		CWavpackContextOwner<Context, Operations> ContextOwner(pContext, Ops);

		if(NumChannels < 1 || NumChannels > 2)
			return EWavpackDecodeResult::INVALID_CHANNEL_COUNT;
		if(SampleRate == 0 || SampleRate > static_cast<unsigned int>(std::numeric_limits<int>::max()))
			return EWavpackDecodeResult::INVALID_SAMPLE_RATE;
		if(BitsPerSample != 16)
			return EWavpackDecodeResult::INVALID_BITS_PER_SAMPLE;
		if(NumFrames <= 0 || NumFrames > std::numeric_limits<int>::max())
			return EWavpackDecodeResult::INVALID_SAMPLE_COUNT;

		std::size_t NumSamples;
		std::size_t InputBufferSize;
		std::size_t OutputBufferSize;
		if(!GetCheckedSampleBufferSize(NumFrames, NumChannels, sizeof(int32_t), &NumSamples, &InputBufferSize) ||
			!GetCheckedSampleBufferSize(NumFrames, NumChannels, sizeof(short), &NumSamples, &OutputBufferSize))
			return EWavpackDecodeResult::SAMPLE_BUFFER_TOO_LARGE;

		CWavpackBufferOwner<int32_t, Operations> pInputBuffer(static_cast<int32_t *>(Ops.Allocate(InputBufferSize)), Ops);
		if(!pInputBuffer.get())
			return EWavpackDecodeResult::INPUT_ALLOCATION_FAILED;

		const uint32_t ExpectedFrames = static_cast<uint32_t>(NumFrames);
		if(Ops.Unpack(pContext, pInputBuffer.get(), ExpectedFrames) != ExpectedFrames)
			return EWavpackDecodeResult::INCOMPLETE_DECODE;

		CWavpackBufferOwner<short, Operations> pOutputBuffer(static_cast<short *>(Ops.Allocate(OutputBufferSize)), Ops);
		if(!pOutputBuffer.get())
			return EWavpackDecodeResult::OUTPUT_ALLOCATION_FAILED;

		for(std::size_t i = 0; i < NumSamples; ++i)
			pOutputBuffer.get()[i] = static_cast<short>(pInputBuffer.get()[i]);

		DecodedSample.m_pData = pOutputBuffer.release();
		DecodedSample.m_NumFrames = static_cast<int>(NumFrames);
		DecodedSample.m_Rate = static_cast<int>(SampleRate);
		DecodedSample.m_Channels = NumChannels;
		DecodedSample.m_LoopStart = 0;
		DecodedSample.m_PausedAt = 0;
		return EWavpackDecodeResult::SUCCESS;
	}
} // namespace sound_detail

#endif // ENGINE_CLIENT_SOUND_WAVPACK_HELPERS_H
