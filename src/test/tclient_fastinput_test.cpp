#include <game/client/automargin.h>
#include <game/client/fastinput.h>

#include <gtest/gtest.h>

#include <cmath>

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

TEST(TcFastInputAuthority, OffsetAbsolutePreservesFractionAtLargeTicks)
{
	for(const int InitialTick : {1 << 23, 1 << 24, 0x6fffffff})
	{
		int Tick = InitialTick;
		float Intra = 0.25f;
		FastInput::OffsetAbsolute(0.5f, Tick, Intra);
		EXPECT_EQ(Tick, InitialTick);
		EXPECT_FLOAT_EQ(Intra, 0.75f);
	}
}

// ---- 2026-11-19 新增：mode 15 Fast (JuKKi)（对拍 Nova 0x14020f42f / 0x1402103f0）----

TEST(TcFastInputAuthority, JukkiAmountMatchesNovaDivide1000)
{
	EXPECT_FLOAT_EQ(FastInput::JukkiAmount(0), 0.0f);
	EXPECT_FLOAT_EQ(FastInput::JukkiAmount(1000), 1.0f);
	EXPECT_FLOAT_EQ(FastInput::JukkiAmount(1500), 1.5f);
	EXPECT_FLOAT_EQ(FastInput::JukkiAmount(5000), 5.0f);
}

// Nova mode 13 语义：本机 OffsetTicks(amount/1000) 进位；他人恒 +1 整 tick 不动 intra
// （0x1402103f0 dl==0 mode==13 分支 amount>0 ? 1 : 0）。
TEST(TcFastInputAuthority, JukkiOffsetCarriesFractionAndOthersPlusOne)
{
	int Tick = 100;
	float Intra = 0.0f;
	FastInput::OffsetTicks(FastInput::JukkiAmount(1500), Tick, Intra);
	EXPECT_EQ(Tick, 101);
	EXPECT_FLOAT_EQ(Intra, 0.5f);

	// 他人路径：Tick += 1，intra 不参与。
	int OthersTick = 100;
	OthersTick += 1;
	EXPECT_EQ(OthersTick, 101);

	// 分数跨帧进位：0.5 残差 + 1.5 → 本帧整 tick +2。
	FastInput::OffsetTicks(FastInput::JukkiAmount(1500), Tick, Intra);
	EXPECT_EQ(Tick, 103);
	EXPECT_FLOAT_EQ(Intra, 0.0f);

	// 量为 0 不动。
	FastInput::OffsetTicks(FastInput::JukkiAmount(0), Tick, Intra);
	EXPECT_EQ(Tick, 103);
	EXPECT_FLOAT_EQ(Intra, 0.0f);
}

// ---- 2026-11-19 新增：mode 16 NovaPilot（对拍 Nova 0x14020f627 / 0x14020ef50 / 0x14020fa30）----

TEST(TcFastInputAuthority, NovaPilotAmountMatchesNovaDivide100)
{
	EXPECT_FLOAT_EQ(FastInput::NovaPilotAmount(0), 0.0f);
	EXPECT_FLOAT_EQ(FastInput::NovaPilotAmount(100), 1.0f);
	EXPECT_FLOAT_EQ(FastInput::NovaPilotAmount(300), 3.0f);
}

// 自适应 jitter：样本 <=4 → 0；population 方差 → 0.05·std；上限 0.35；方差 0 → 0。
TEST(TcFastInputAuthority, NovaPilotJitterNeedsFiveSamplesAndClamps)
{
	const int LowVariance[] = {99, 100, 101, 102, 103};
	// mean 101，population var (4+1+0+1+4)/5 = 2，std = sqrt(2)，jitter = 0.05·sqrt(2)。
	EXPECT_NEAR(FastInput::NovaPilotJitterTicks(LowVariance, 5), 0.05f * std::sqrt(2.0f), 1e-6f);

	const int Constant[] = {50, 50, 50, 50, 50, 50};
	EXPECT_FLOAT_EQ(FastInput::NovaPilotJitterTicks(Constant, 6), 0.0f);

	const int TwoSamples[] = {0, 100};
	EXPECT_FLOAT_EQ(FastInput::NovaPilotJitterTicks(TwoSamples, 2), 0.0f);
	EXPECT_FLOAT_EQ(FastInput::NovaPilotJitterTicks(TwoSamples, 4), 0.0f);
	EXPECT_FLOAT_EQ(FastInput::NovaPilotJitterTicks(nullptr, 10), 0.0f);

	const int Wide[] = {0, 100, 0, 100, 0};
	// mean 40，var 2400，std ≈ 48.99，jitter ≈ 2.45 → 截到上限 0.35。
	EXPECT_FLOAT_EQ(FastInput::NovaPilotJitterTicks(Wide, 5), 0.35f);
}

// Catmull-Rom：端点插值 p(0)=P1、p(1)=P2；直线样本中点保持线性。
TEST(TcFastInputAuthority, CatmullRomEndpointsAndMidpoint)
{
	EXPECT_FLOAT_EQ(FastInput::CatmullRom(0.0f, 0.0f, 1.0f, 1.0f, 0.0f), 0.0f);
	EXPECT_FLOAT_EQ(FastInput::CatmullRom(0.0f, 0.0f, 1.0f, 1.0f, 1.0f), 1.0f);
	EXPECT_FLOAT_EQ(FastInput::CatmullRom(0.0f, 0.0f, 1.0f, 1.0f, 0.5f), 0.5f);
	EXPECT_FLOAT_EQ(FastInput::CatmullRom(0.0f, 10.0f, 20.0f, 30.0f, 0.5f), 15.0f);
	EXPECT_FLOAT_EQ(FastInput::CatmullRom(10.0f, 20.0f, 40.0f, 80.0f, 0.0f), 20.0f);
	EXPECT_FLOAT_EQ(FastInput::CatmullRom(10.0f, 20.0f, 40.0f, 80.0f, 1.0f), 40.0f);
}

TEST(TcFastInputAuthority, NovaPilotJitterSwitchControlsOffset)
{
	const int Samples[] = {0, 100, 0, 100, 0};
	EXPECT_FLOAT_EQ(FastInput::NovaPilotTickOffset(100, false, Samples, 5), 1.0f);
	EXPECT_FLOAT_EQ(FastInput::NovaPilotTickOffset(100, true, Samples, 5), 1.35f);
}

TEST(TcFastInputAuthority, NovaPilotClampDistanceAndNonfinite)
{
	const vec2 Base(0.0f, 0.0f);
	EXPECT_TRUE(FastInput::NovaPilotRejectPosition(Base, vec2(1.0f, 0.0f)));
	EXPECT_FALSE(FastInput::NovaPilotRejectPosition(Base, vec2(1.01f, 0.0f)));
	EXPECT_FALSE(FastInput::NovaPilotRejectPosition(Base, vec2(300.0f, 0.0f)));
	EXPECT_TRUE(FastInput::NovaPilotRejectPosition(Base, vec2(300.01f, 0.0f)));
	EXPECT_TRUE(FastInput::NovaPilotRejectPosition(Base, vec2(std::numeric_limits<float>::infinity(), 1.0f)));
	EXPECT_TRUE(FastInput::NovaPilotRejectPosition(Base, vec2(1.0f, std::numeric_limits<float>::quiet_NaN())));
	for(int Tile : {TILE_FREEZE, TILE_DFREEZE, TILE_LFREEZE})
		EXPECT_TRUE(FastInput::NovaPilotFreezeTile(Tile));
	for(int Tile : {TILE_AIR, TILE_SOLID, TILE_UNFREEZE, TILE_DUNFREEZE, TILE_LUNFREEZE})
		EXPECT_FALSE(FastInput::NovaPilotFreezeTile(Tile));
}

TEST(TcFastInputAuthority, NovaPilotCurveFallsBackForEveryLongSegment)
{
	const vec2 Linear(15.0f, 5.0f);
	const vec2 Normal[] = {vec2(0, 0), vec2(10, 0), vec2(20, 0), vec2(30, 0)};
	EXPECT_EQ(FastInput::NovaPilotCurvedPos(Normal[0], Normal[1], Normal[2], Normal[3], 0.5f, Linear), vec2(15, 0));
	for(int Segment = 0; Segment < 3; ++Segment)
	{
		vec2 Points[4];
		for(int i = 0; i < 4; ++i)
			Points[i] = vec2(i > Segment ? 257.0f : 0.0f, 0.0f);
		EXPECT_EQ(FastInput::NovaPilotCurvedPos(Points[0], Points[1], Points[2], Points[3], 0.5f, Linear), Linear);
	}
	EXPECT_NE(FastInput::NovaPilotCurvedPos(vec2(0, 0), vec2(256, 0), vec2(256, 0), vec2(256, 0), 0.5f, Linear), Linear);
}

TEST(TcFastInputAuthority, NovaPilotCurveRejectsEntireNonfiniteVector)
{
	const vec2 Linear(15.0f, 5.0f);
	for(int Slot = 0; Slot < 4; ++Slot)
		for(int Axis = 0; Axis < 2; ++Axis)
		{
			vec2 Points[] = {vec2(0, 0), vec2(10, 0), vec2(20, 0), vec2(30, 0)};
			Points[Slot][Axis] = std::numeric_limits<float>::quiet_NaN();
			EXPECT_EQ(FastInput::NovaPilotCurvedPos(Points[0], Points[1], Points[2], Points[3], 0.5f, Linear), Linear);
		}
	EXPECT_EQ(FastInput::NovaPilotCurvedPos(vec2(0, 0), vec2(10, 0), vec2(20, 0), vec2(30, 0), std::numeric_limits<float>::infinity(), Linear), Linear);
}

// ---- 2026-11-19 新增：自动余量（automargin.h，对拍 Nova 0x14020ead0/0x14020eb92 + BC-Light automargin.cpp）----

TEST(TcAutoMargin, SavesAndRestoresUserMargin)
{
	AutoMargin::CState State;
	AutoMargin::CUpdateInput In;
	In.Enabled = true;
	In.Online = true;
	In.DeltaTime = 100.0f; // 远超 0.5s 检查间隔 → 每次调用都触发评估
	In.CurrentMargin = 30;
	EXPECT_TRUE(AutoMargin::Update(State, In).SaveMargin);
	EXPECT_TRUE(State.WasEnabled);

	// 接线层随后写入 State.SavedMargin；关闭后恢复用户原值。
	State.SavedMargin = 30;
	In.Enabled = false;
	const AutoMargin::CStep Off = AutoMargin::Update(State, In);
	EXPECT_TRUE(Off.RestoreMargin);
	EXPECT_EQ(Off.RestoreValue, 30);
	EXPECT_EQ(State.SavedMargin, -1);
	EXPECT_FALSE(State.WasEnabled);
	EXPECT_FALSE(Off.AdjustMargin);
}

TEST(TcAutoMargin, KeepsLatestPingBetweenEvaluations)
{
	AutoMargin::CState State;
	AutoMargin::CUpdateInput In;
	In.Enabled = true;
	In.Online = true;
	In.DeltaTime = 0.5f;
	In.CurrentMargin = 9;
	State.Window.AddSample(100.0f);
	const AutoMargin::CStep First = AutoMargin::Update(State, In);
	ASSERT_TRUE(First.AdjustMargin);
	In.CurrentMargin = First.MarginTarget;
	for(int i = 0; i < 10; ++i)
	{
		const AutoMargin::CStep Step = AutoMargin::Update(State, In);
		EXPECT_FLOAT_EQ(State.Window.Latest, 100.0f);
		EXPECT_FLOAT_EQ(State.SmoothedPing, 100.0f);
		EXPECT_FALSE(Step.AdjustMargin);
		EXPECT_TRUE(State.Latched);
	}
}

TEST(TcAutoMargin, EvaluatesAtHalfSecondIntervals)
{
	AutoMargin::CState State;
	AutoMargin::CUpdateInput In;
	In.Enabled = true;
	In.Online = true;
	In.CurrentMargin = 100;
	State.Window.AddSample(10.0f);
	EXPECT_EQ(AutoMargin::Update(State, In).MarginTarget, 95);
	In.CurrentMargin = 95;
	In.DeltaTime = 0.125f;
	for(int i = 0; i < 3; ++i)
		EXPECT_FALSE(AutoMargin::Update(State, In).AdjustMargin);
	EXPECT_EQ(AutoMargin::Update(State, In).MarginTarget, 90);
}

TEST(TcAutoMargin, RestoreIsIdempotentAcrossResetAndReconnect)
{
	AutoMargin::CState State;
	AutoMargin::CUpdateInput In;
	In.Enabled = true;
	In.Online = true;
	In.CurrentMargin = 37;
	State.Window.AddSample(120.0f);
	AutoMargin::Update(State, In);
	EXPECT_EQ(State.SavedMargin, 37);
	int Margin = 30;
	State.Restore(Margin);
	EXPECT_EQ(Margin, 37);
	EXPECT_FALSE(State.WasEnabled);
	EXPECT_EQ(State.SavedMargin, -1);
	EXPECT_FLOAT_EQ(State.Window.Latest, -1.0f);
	Margin = 42;
	State.Restore(Margin);
	EXPECT_EQ(Margin, 42);
	In.CurrentMargin = Margin;
	AutoMargin::Update(State, In);
	EXPECT_EQ(State.SavedMargin, 42);
	In.Online = false;
	const auto Disconnected = AutoMargin::Update(State, In);
	EXPECT_TRUE(Disconnected.RestoreMargin);
	EXPECT_EQ(Disconnected.RestoreValue, 42);
	EXPECT_FALSE(Disconnected.AdjustMargin);
	EXPECT_FALSE(AutoMargin::Update(State, In).RestoreMargin);
}

TEST(TcAutoMargin, HighPingLatchesAndRaisesMargin)
{
	AutoMargin::CState State;
	AutoMargin::CUpdateInput In;
	In.Enabled = true;
	In.Online = true;
	In.DeltaTime = 100.0f;
	In.CurrentMargin = 9;
	for(int i = 0; i < 40; ++i)
		State.Window.AddSample(80.0f);
	const AutoMargin::CStep Step = AutoMargin::Update(State, In);
	// estimate = max(初始化 80, 80) ≥ 50 → 闩锁；target = round(max(9, 80·0.25)) = 20。
	EXPECT_TRUE(State.Latched);
	EXPECT_TRUE(Step.AdjustMargin);
	EXPECT_EQ(Step.MarginTarget, 20);
	EXPECT_EQ(State.Window.Count, 0); // 样本窗已消费清零
}

TEST(TcAutoMargin, JitterMarginUsesBandEma094AndBandRaw035)
{
	AutoMargin::CState State;
	AutoMargin::CUpdateInput In;
	In.Enabled = true;
	In.Online = true;
	In.DeltaTime = 100.0f;
	In.CurrentMargin = 9;
	State.Latched = true;
	State.SmoothedPing = 80.0f;
	State.SmoothedJitter = 10.0f;
	State.Window.AddSample(100.0f);
	State.Window.AddSample(60.0f);
	// band_raw = 40 → band EMA 升至 10 + 30·0.35 = 20.5；
	// estimate = max(80, 60, 80 + 20.5·0.25) ≈ 85.125；
	// jitter 分量 = max(0.94·20.5, 0.35·40) ≈ 19.27；target = round(21.28 + 19.27) = 41。
	const AutoMargin::CStep Step = AutoMargin::Update(State, In);
	EXPECT_TRUE(Step.AdjustMargin);
	EXPECT_EQ(Step.MarginTarget, 41);
}

TEST(TcAutoMargin, StepsDownFivePerEvalAndRespectsDeadzone)
{
	AutoMargin::CState State;
	AutoMargin::CUpdateInput In;
	In.Enabled = true;
	In.Online = true;
	In.DeltaTime = 100.0f;
	In.CurrentMargin = 100;
	State.Latched = true;
	State.SmoothedPing = 80.0f;
	State.Window.AddSample(80.0f);
	// 目标 20，当前 100 → 下调 min(80, 5) = 5。
	const AutoMargin::CStep Down = AutoMargin::Update(State, In);
	EXPECT_TRUE(Down.AdjustMargin);
	EXPECT_EQ(Down.MarginTarget, 95);

	// 死区：目标与当前差 ≤1 不写。estimate 42 → target round(10.5)=11，当前 12。
	AutoMargin::CState Dead;
	AutoMargin::CUpdateInput DeadIn = In;
	DeadIn.CurrentMargin = 12;
	Dead.Latched = true;
	Dead.SmoothedPing = 42.0f;
	Dead.Window.AddSample(42.0f);
	const AutoMargin::CStep DeadStep = AutoMargin::Update(Dead, DeadIn);
	EXPECT_FALSE(DeadStep.AdjustMargin);
	EXPECT_TRUE(Dead.Latched); // estimate 42 不低于退出线 42
}

TEST(TcAutoMargin, MarginClampAtThreeHundredAndStepDownFromAbove)
{
	AutoMargin::CState State;
	AutoMargin::CUpdateInput In;
	In.Enabled = true;
	In.Online = true;
	In.DeltaTime = 100.0f;
	In.CurrentMargin = 100;
	State.Latched = true;
	State.SmoothedPing = 1500.0f;
	State.Window.AddSample(1500.0f);
	// estimate 1500 → 理论 target 375 → clamp 300。
	const AutoMargin::CStep High = AutoMargin::Update(State, In);
	EXPECT_TRUE(High.AdjustMargin);
	EXPECT_EQ(High.MarginTarget, 300);
}

TEST(TcAutoMargin, UnlatchedTargetIsBaseMarginWithStepDown)
{
	AutoMargin::CState State;
	AutoMargin::CUpdateInput In;
	In.Enabled = true;
	In.Online = true;
	In.DeltaTime = 100.0f;
	In.CurrentMargin = 20;
	State.Latched = false;
	State.SmoothedPing = 10.0f;
	State.Window.AddSample(10.0f);
	// 未闩锁 → 目标 9；20 → 下调 5 → 15（每步 ≤5）。
	const AutoMargin::CStep Step = AutoMargin::Update(State, In);
	EXPECT_TRUE(Step.AdjustMargin);
	EXPECT_EQ(Step.MarginTarget, 15);
}
