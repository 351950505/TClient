/* Copyright © 2026 BestProject Team (MIT); adapted for TClient. */
#include "gradient_math.h"

#include <base/math.h>
#include <base/str.h>

#include <generated/protocol7.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

namespace
{
	int NormalizedTextLength(const char *pText, int Length)
	{
		if(pText == nullptr)
			return 0;
		const int TextLength = str_length(pText);
		return Length < 0 ? TextLength : std::clamp(Length, 0, TextLength);
	}

	std::vector<std::pair<int, int>> CollectCharacterSpans(const char *pText, int Length)
	{
		std::vector<std::pair<int, int>> vSpans;
		Length = NormalizedTextLength(pText, Length);
		if(Length == 0)
			return vSpans;

		const char *pCurrent = pText;
		const char *pEnd = pText + Length;
		while(pCurrent < pEnd)
		{
			const char *pPrevious = pCurrent;
			str_utf8_decode(&pCurrent);
			if(pCurrent <= pPrevious || pCurrent > pEnd)
				break;
			vSpans.emplace_back((int)(pPrevious - pText), (int)(pCurrent - pPrevious));
		}
		return vSpans;
	}

	std::vector<STextColorSplit> BuildColorSplits(const char *pText, int Length, const std::function<ColorRGBA(float)> &SampleColor)
	{
		const std::vector<std::pair<int, int>> vSpans = CollectCharacterSpans(pText, Length);
		std::vector<STextColorSplit> vSplits;
		vSplits.reserve(vSpans.size());
		for(size_t i = 0; i < vSpans.size(); ++i)
		{
			const float Position = vSpans.size() == 1 ? 0.5f : (float)i / (float)(vSpans.size() - 1);
			vSplits.emplace_back(vSpans[i].first, vSpans[i].second, SampleColor(Position));
		}
		return vSplits;
	}
} // namespace

ColorRGBA TClientGradient::MixColor(const ColorRGBA &A, const ColorRGBA &B, float Amount)
{
	return ColorRGBA(
		A.r + Amount * (B.r - A.r),
		A.g + Amount * (B.g - A.g),
		A.b + Amount * (B.b - A.b),
		A.a + Amount * (B.a - A.a));
}

float TClientGradient::AnimatePhase(double GlobalTime, int Speed)
{
	const double Phase = std::fmod(GlobalTime * (std::max(Speed, 0) / 100.0), 1.0);
	return (float)(Phase < 0.0 ? Phase + 1.0 : Phase);
}

int TClientGradient::PhaseBucket(float Phase)
{
	Phase = std::fmod(Phase, 1.0f);
	if(Phase < 0.0f)
		Phase += 1.0f;
	return (int)(Phase * PHASE_BUCKET_COUNT) % PHASE_BUCKET_COUNT;
}

ColorRGBA TClientGradient::SampleRainbow(float Hue)
{
	Hue = std::fmod(Hue, 1.0f);
	if(Hue < 0.0f)
		Hue += 1.0f;
	return color_cast<ColorRGBA>(ColorHSLA(Hue, 1.0f, 0.5f, 1.0f));
}

ColorRGBA TClientGradient::SampleColorStops(const ColorRGBA *pColors, int Count, float Position)
{
	if(pColors == nullptr || Count <= 0)
		return ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
	Count = std::clamp(Count, 1, 4);
	if(Count == 1)
		return pColors[0];

	float Wrapped = std::fmod(Position, 1.0f);
	if(Wrapped < 0.0f)
		Wrapped += 1.0f;
	const float Scaled = Wrapped * Count;
	const int Index = (int)Scaled % Count;
	return MixColor(pColors[Index], pColors[(Index + 1) % Count], Scaled - std::floor(Scaled));
}

std::vector<STextColorSplit> TClientGradient::BuildAnimatedColorSplits(const char *pText, const ColorRGBA &Color1, const ColorRGBA &Color2, float Phase, int Length)
{
	return BuildColorSplits(pText, Length, [&](float Position) {
		const float Wave = 0.5f + 0.5f * std::cos(2.0f * pi * (Position - Phase));
		return MixColor(Color1, Color2, Wave);
	});
}

std::vector<STextColorSplit> TClientGradient::BuildCustomColorSplits(const char *pText, const ColorRGBA *pColors, int Count, float Phase, int Length)
{
	return BuildColorSplits(pText, Length, [&](float Position) {
		return SampleColorStops(pColors, Count, Position - Phase);
	});
}

std::vector<STextColorSplit> TClientGradient::BuildRainbowColorSplits(const char *pText, float Phase, int Length)
{
	return BuildColorSplits(pText, Length, [&](float Position) {
		return SampleRainbow(Position * 0.75f + Phase);
	});
}

void TClientGradient::ApplySixupColors(CTeeRenderInfo &RenderInfo, const ColorRGBA &Body, const ColorRGBA &Feet)
{
	for(auto &Sixup : RenderInfo.m_aSixup)
	{
		for(int Part = 0; Part < protocol7::NUM_SKINPARTS; ++Part)
		{
			ColorRGBA Color = Part == protocol7::SKINPART_FEET ? Feet : Body;
			Color.a *= Sixup.m_aColors[Part].a;
			Sixup.m_aColors[Part] = Color;
			Sixup.m_aUseCustomColors[Part] = true;
		}
	}
}
