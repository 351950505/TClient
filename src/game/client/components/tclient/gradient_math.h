/* Copyright © 2026 BestProject Team (MIT); adapted for TClient. */
#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_GRADIENT_MATH_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_GRADIENT_MATH_H

#include <base/color.h>

#include <engine/textrender.h>

#include <game/client/render.h>

#include <vector>

namespace TClientGradient
{
	constexpr int PHASE_BUCKET_COUNT = 64;

	float AnimatePhase(double GlobalTime, int Speed);
	int PhaseBucket(float Phase);
	ColorRGBA MixColor(const ColorRGBA &A, const ColorRGBA &B, float Amount);
	ColorRGBA SampleRainbow(float Hue);
	ColorRGBA SampleColorStops(const ColorRGBA *pColors, int Count, float Position);
	std::vector<STextColorSplit> BuildAnimatedColorSplits(const char *pText, const ColorRGBA &Color1, const ColorRGBA &Color2, float Phase, int Length = -1);
	std::vector<STextColorSplit> BuildCustomColorSplits(const char *pText, const ColorRGBA *pColors, int Count, float Phase, int Length = -1);
	std::vector<STextColorSplit> BuildRainbowColorSplits(const char *pText, float Phase, int Length = -1);
	void ApplySixupColors(CTeeRenderInfo &RenderInfo, const ColorRGBA &Body, const ColorRGBA &Feet);
} // namespace TClientGradient

#endif
