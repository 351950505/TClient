#ifndef ENGINE_CLIENT_VIDEO_FRAME_H
#define ENGINE_CLIENT_VIDEO_FRAME_H

#include <engine/graphics.h>

extern "C" {
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

class CVideoFrameNormalizer
{
	std::vector<uint8_t> m_vReadbackBuffer;
	SwsContext *m_pSwsContext = nullptr;

public:
	// Keep standard FFmpeg-readable padding after each source plane.
	static constexpr size_t PADDING_BYTES = 64;

	CVideoFrameNormalizer() = default;
	CVideoFrameNormalizer(const CVideoFrameNormalizer &) = delete;
	CVideoFrameNormalizer &operator=(const CVideoFrameNormalizer &) = delete;

	~CVideoFrameNormalizer()
	{
		sws_freeContext(m_pSwsContext);
	}

	bool ReadAndNormalize(const TGLBackendReadPresentedImageData &ReadPresentedImage, int TargetWidth, int TargetHeight, std::vector<uint8_t> &vFrame)
	{
		constexpr size_t MAX_IMAGE_BYTES = std::numeric_limits<size_t>::max() - PADDING_BYTES;
		if(TargetWidth <= 0 || TargetHeight <= 0 || TargetWidth > std::numeric_limits<int>::max() / 4 ||
			(size_t)TargetWidth > MAX_IMAGE_BYTES / 4 / (size_t)TargetHeight)
			return false;

		const size_t TargetBufferSize = (size_t)TargetWidth * TargetHeight * 4 + PADDING_BYTES;
		const auto BlackFrame = [&]() {
			vFrame.assign(TargetBufferSize, 0);
			return false;
		};

		uint32_t SourceWidth = 0;
		uint32_t SourceHeight = 0;
		CImageInfo::EImageFormat Format = CImageInfo::FORMAT_UNDEFINED;
		if(!ReadPresentedImage || !ReadPresentedImage(SourceWidth, SourceHeight, Format, m_vReadbackBuffer) ||
			Format != CImageInfo::FORMAT_RGBA || SourceWidth == 0 || SourceHeight == 0 ||
			SourceWidth > (uint32_t)(std::numeric_limits<int>::max() / 4) || SourceHeight > (uint32_t)std::numeric_limits<int>::max() ||
			SourceHeight > MAX_IMAGE_BYTES / 4 / SourceWidth)
			return BlackFrame();

		const size_t SourceBytes = (size_t)SourceWidth * SourceHeight * 4;
		if(m_vReadbackBuffer.size() < SourceBytes)
			return BlackFrame();

		// Discard any backend flip scratch row and extend the plane for libswscale.
		m_vReadbackBuffer.resize(SourceBytes + PADDING_BYTES);
		std::fill(m_vReadbackBuffer.end() - PADDING_BYTES, m_vReadbackBuffer.end(), 0);
		if(SourceWidth == (uint32_t)TargetWidth && SourceHeight == (uint32_t)TargetHeight)
		{
			vFrame.swap(m_vReadbackBuffer);
			return true;
		}

		m_pSwsContext = sws_getCachedContext(m_pSwsContext, SourceWidth, SourceHeight, AV_PIX_FMT_RGBA,
			TargetWidth, TargetHeight, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
		if(!m_pSwsContext)
			return BlackFrame();

		vFrame.resize(TargetBufferSize);
		const uint8_t *apSource[4] = {m_vReadbackBuffer.data()};
		const int aSourceStride[4] = {(int)SourceWidth * 4};
		uint8_t *apTarget[4] = {vFrame.data()};
		const int aTargetStride[4] = {TargetWidth * 4};
		const int ScaledHeight = sws_scale(m_pSwsContext, apSource, aSourceStride, 0, SourceHeight, apTarget, aTargetStride);
		std::fill(vFrame.end() - PADDING_BYTES, vFrame.end(), 0);
		if(ScaledHeight != TargetHeight)
			return BlackFrame();
		return true;
	}
};

#endif
