#include <generated/protocol7.h>

#include <game/client/components/tclient/gradient_math.h>

#include <gtest/gtest.h>

TEST(TClientGradient, AnimatePhaseAndBuckets)
{
	EXPECT_FLOAT_EQ(TClientGradient::AnimatePhase(0.0, 35), 0.0f);
	EXPECT_NEAR(TClientGradient::AnimatePhase(1.0, 35), 0.35f, 0.00001f);
	EXPECT_NEAR(TClientGradient::AnimatePhase(3.0, 35), 0.05f, 0.00001f);
	EXPECT_FLOAT_EQ(TClientGradient::AnimatePhase(10.0, 0), 0.0f);

	EXPECT_EQ(TClientGradient::PhaseBucket(0.0f), 0);
	EXPECT_EQ(TClientGradient::PhaseBucket(0.5f), 32);
	EXPECT_EQ(TClientGradient::PhaseBucket(0.999f), 63);
	EXPECT_EQ(TClientGradient::PhaseBucket(1.0f), 0);
	EXPECT_EQ(TClientGradient::PhaseBucket(-0.01f), 63);
}

TEST(TClientGradient, SamplesClosedColorStops)
{
	const ColorRGBA aColors[] = {
		ColorRGBA(1.0f, 0.0f, 0.0f, 0.2f),
		ColorRGBA(0.0f, 1.0f, 0.0f, 0.6f),
		ColorRGBA(0.0f, 0.0f, 1.0f, 0.8f),
		ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f)};

	EXPECT_EQ(TClientGradient::SampleColorStops(aColors, 4, 0.0f), aColors[0]);
	EXPECT_EQ(TClientGradient::SampleColorStops(aColors, 4, 0.25f), aColors[1]);
	EXPECT_EQ(TClientGradient::SampleColorStops(aColors, 4, 0.5f), aColors[2]);
	EXPECT_EQ(TClientGradient::SampleColorStops(aColors, 4, 0.75f), aColors[3]);
	EXPECT_EQ(TClientGradient::SampleColorStops(aColors, 4, 1.0f), aColors[0]);

	const ColorRGBA Mid = TClientGradient::SampleColorStops(aColors, 2, 0.25f);
	EXPECT_NEAR(Mid.r, 0.5f, 0.00001f);
	EXPECT_NEAR(Mid.g, 0.5f, 0.00001f);
	EXPECT_NEAR(Mid.a, 0.4f, 0.00001f);
}

TEST(TClientGradient, BuildsUtf8ByteSplitsWithinLength)
{
	const char *pText = "A你🙂";
	const ColorRGBA Color1(0.2f, 0.3f, 0.4f, 0.5f);
	const ColorRGBA Color2(0.8f, 0.7f, 0.6f, 0.9f);
	const std::vector<STextColorSplit> vSplits = TClientGradient::BuildAnimatedColorSplits(pText, Color1, Color2, 0.0f);
	ASSERT_EQ(vSplits.size(), 3u);
	EXPECT_EQ(vSplits[0].m_CharIndex, 0);
	EXPECT_EQ(vSplits[0].m_Length, 1);
	EXPECT_EQ(vSplits[1].m_CharIndex, 1);
	EXPECT_EQ(vSplits[1].m_Length, 3);
	EXPECT_EQ(vSplits[2].m_CharIndex, 4);
	EXPECT_EQ(vSplits[2].m_Length, 4);

	const std::vector<STextColorSplit> vBounded = TClientGradient::BuildAnimatedColorSplits(pText, Color1, Color2, 0.0f, 4);
	ASSERT_EQ(vBounded.size(), 2u);
	EXPECT_EQ(vBounded[1].m_CharIndex, 1);
	EXPECT_EQ(vBounded[1].m_Length, 3);

	const std::vector<STextColorSplit> vTruncated = TClientGradient::BuildAnimatedColorSplits(pText, Color1, Color2, 0.0f, 2);
	ASSERT_EQ(vTruncated.size(), 1u);
	EXPECT_EQ(vTruncated[0].m_CharIndex, 0);
}

TEST(TClientGradient, RainbowWrapsHue)
{
	const ColorRGBA Start = TClientGradient::SampleRainbow(0.0f);
	const ColorRGBA Wrapped = TClientGradient::SampleRainbow(1.0f);
	EXPECT_NEAR(Start.r, Wrapped.r, 0.00001f);
	EXPECT_NEAR(Start.g, Wrapped.g, 0.00001f);
	EXPECT_NEAR(Start.b, Wrapped.b, 0.00001f);
	EXPECT_FLOAT_EQ(Start.a, 1.0f);
}

TEST(TClientGradient, AppliesSixupBodyAndFeetColors)
{
	CTeeRenderInfo RenderInfo;
	for(auto &Sixup : RenderInfo.m_aSixup)
	{
		Sixup.m_aColors[protocol7::SKINPART_MARKING].a = 0.25f;
		Sixup.m_aColors[protocol7::SKINPART_FEET].a = 0.5f;
	}
	const ColorRGBA Body(0.1f, 0.2f, 0.3f, 0.8f);
	const ColorRGBA Feet(0.9f, 0.8f, 0.7f, 0.6f);
	TClientGradient::ApplySixupColors(RenderInfo, Body, Feet);

	for(const auto &Sixup : RenderInfo.m_aSixup)
	{
		for(int Part = 0; Part < protocol7::NUM_SKINPARTS; ++Part)
			EXPECT_TRUE(Sixup.m_aUseCustomColors[Part]);
		EXPECT_NEAR(Sixup.m_aColors[protocol7::SKINPART_BODY].r, Body.r, 0.00001f);
		EXPECT_NEAR(Sixup.m_aColors[protocol7::SKINPART_BODY].a, Body.a, 0.00001f);
		EXPECT_NEAR(Sixup.m_aColors[protocol7::SKINPART_MARKING].a, Body.a * 0.25f, 0.00001f);
		EXPECT_NEAR(Sixup.m_aColors[protocol7::SKINPART_FEET].r, Feet.r, 0.00001f);
		EXPECT_NEAR(Sixup.m_aColors[protocol7::SKINPART_FEET].a, Feet.a * 0.5f, 0.00001f);
	}
}
