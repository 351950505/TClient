#include <game/client/fastinput.h>

#include <gtest/gtest.h>

TEST(TcFastInputAuthority, PredictionTicksMatchBestClientLight)
{
	EXPECT_EQ(FastInput::PredictionTicks(0.0f), 0);
	EXPECT_EQ(FastInput::PredictionTicks(0.01f), 1);
	EXPECT_EQ(FastInput::PredictionTicks(1.0f), 1);
	EXPECT_EQ(FastInput::PredictionTicks(1.01f), 2);
}

TEST(TcFastInputAuthority, BestAmountMatchesBestClientLight)
{
	EXPECT_FLOAT_EQ(FastInput::BestAmount(0, 0, 0), 0.0f);
	EXPECT_FLOAT_EQ(FastInput::BestAmount(80, 50, 20), 0.72f);
	EXPECT_FLOAT_EQ(FastInput::BestAmount(1000, 100, 50), 7.5f);
}

TEST(TcFastInputAuthority, BestInterpolationMatchesBestClientLight)
{
	EXPECT_FLOAT_EQ(FastInput::InterpolationIntra(0.25f, 0.0f, 1), 0.25f);
	EXPECT_FLOAT_EQ(FastInput::InterpolationIntra(0.25f, 0.0f, 2), 0.15625f);
	EXPECT_FLOAT_EQ(FastInput::InterpolationIntra(0.25f, 500.0f, 3), 0.203125f);
}

TEST(TcFastInputAuthority, FSmoothingIsFrameRateIndependent)
{
	const float ExtraTime = 0.25f;
	float At60Hz = 0.0f;
	float At144Hz = 0.0f;
	for(int Frame = 0; Frame < 60; ++Frame)
		At60Hz += (1.0f - At60Hz) * FastInput::FBlendAlpha(ExtraTime, 1.0f / 60.0f);
	for(int Frame = 0; Frame < 144; ++Frame)
		At144Hz += (1.0f - At144Hz) * FastInput::FBlendAlpha(ExtraTime, 1.0f / 144.0f);
	EXPECT_NEAR(At60Hz, At144Hz, 0.00001f);
}
