#include <engine/client/video_frame.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

static int s_Failures = 0;

static void Check(bool Condition, const char *pName)
{
	if(!Condition)
	{
		++s_Failures;
		std::printf("FAIL: %s\n", pName);
	}
	else
		std::printf("PASS: %s\n", pName);
}

static std::vector<uint8_t> SolidImage(uint32_t Width, uint32_t Height, std::array<uint8_t, 4> Color)
{
	std::vector<uint8_t> Pixels((size_t)Width * (Height + 1) * 4, 0xCD);
	for(size_t Index = 0; Index < (size_t)Width * Height; ++Index)
		std::copy(Color.begin(), Color.end(), Pixels.begin() + Index * 4);
	return Pixels;
}

static bool Readback(uint32_t &Width, uint32_t &Height, CImageInfo::EImageFormat &Format, std::vector<uint8_t> &Data,
	uint32_t SourceWidth, uint32_t SourceHeight, const std::vector<uint8_t> &Source, bool Success = true, CImageInfo::EImageFormat SourceFormat = CImageInfo::FORMAT_RGBA)
{
	Width = SourceWidth;
	Height = SourceHeight;
	Format = SourceFormat;
	Data = Source;
	return Success;
}

int main()
{
	CVideoFrameNormalizer Normalizer;
	std::vector<uint8_t> Frame;
	const auto Exact = SolidImage(2, 2, {10, 20, 30, 255});
	Check(Normalizer.ReadAndNormalize([&](auto &Width, auto &Height, auto &Format, auto &Data) {
		return Readback(Width, Height, Format, Data, 2, 2, Exact);
	}, 2, 2, Frame), "same-size frame readback succeeds");
	Check(Frame.size() == 80 && Frame[0] == 10 && Frame[4] == 10 && Frame[12] == 10 && Frame[79] == 0, "same-size readback ignores flip row and has zero padding");

	const auto Large = SolidImage(4, 4, {80, 120, 160, 255});
	Check(Normalizer.ReadAndNormalize([&](auto &Width, auto &Height, auto &Format, auto &Data) {
		return Readback(Width, Height, Format, Data, 4, 4, Large);
	}, 2, 2, Frame), "larger drawable is scaled into fixed recording frame");
	Check(Frame.size() == 80 && Frame[0] == 80 && Frame[1] == 120 && Frame[2] == 160 && Frame[3] == 255 && Frame[79] == 0, "scaled frame keeps fixed size, source color and zero padding");

	const auto Small = SolidImage(1, 1, {200, 40, 20, 255});
	Check(Normalizer.ReadAndNormalize([&](auto &Width, auto &Height, auto &Format, auto &Data) {
		return Readback(Width, Height, Format, Data, 1, 1, Small);
	}, 2, 2, Frame), "smaller drawable is scaled into fixed recording frame");
	Check(Frame.size() == 80 && Frame[0] == 200 && Frame[1] == 40 && Frame[2] == 20 && Frame[3] == 255 && Frame[79] == 0, "scaled-up frame keeps fixed size, source color and zero padding");

	Check(!Normalizer.ReadAndNormalize([&](auto &Width, auto &Height, auto &Format, auto &Data) {
		return Readback(Width, Height, Format, Data, 2, 2, Exact, false);
	}, 2, 2, Frame), "failed graphics readback is rejected");
	Check(Frame.size() == 80 && std::all_of(Frame.begin(), Frame.end(), [](uint8_t Value) { return Value == 0; }), "failed readback leaves a black fixed-size frame");

	const std::vector<uint8_t> Truncated(4, 0xFF);
	Check(!Normalizer.ReadAndNormalize([&](auto &Width, auto &Height, auto &Format, auto &Data) {
		return Readback(Width, Height, Format, Data, 2, 2, Truncated);
	}, 2, 2, Frame), "truncated graphics buffer is rejected");
	Check(Frame.size() == 80 && std::all_of(Frame.begin(), Frame.end(), [](uint8_t Value) { return Value == 0; }), "truncated readback cannot expose stale bytes");

	std::puts(s_Failures ? "FAILED" : "OK: video resize tests passed");
	return s_Failures ? 1 : 0;
}
