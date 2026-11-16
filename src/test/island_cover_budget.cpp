// Standalone RT-1 regression; run scripts/test_island_cover_budget.js, not the shared build.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct SAllocStats
{
	std::unordered_map<void *, size_t> m_Live;
	size_t m_Bytes = 0, m_Peak = 0, m_MaxRequest = 0, m_Calls = 0;
	int m_FailAt = -1;
};
static thread_local SAllocStats s_Alloc;
static void *TrackMalloc(size_t Size)
{
	++s_Alloc.m_Calls;
	s_Alloc.m_MaxRequest = std::max(s_Alloc.m_MaxRequest, Size);
	if(s_Alloc.m_FailAt >= 0 && s_Alloc.m_Calls == (size_t)s_Alloc.m_FailAt)
		return nullptr;
	void *p = std::malloc(Size);
	if(p)
	{
		s_Alloc.m_Live[p] = Size;
		s_Alloc.m_Bytes += Size;
		s_Alloc.m_Peak = std::max(s_Alloc.m_Peak, s_Alloc.m_Bytes);
	}
	return p;
}
static void TrackFree(void *p)
{
	if(!p)
		return;
	auto It = s_Alloc.m_Live.find(p);
	if(It == s_Alloc.m_Live.end())
		std::abort(); // also detects cross-thread/shared accounting mistakes
	s_Alloc.m_Bytes -= It->second;
	s_Alloc.m_Live.erase(It);
	std::free(p);
}
static void *TrackRealloc(void *p, size_t Size)
{
	if(!p)
		return TrackMalloc(Size);
	++s_Alloc.m_Calls;
	s_Alloc.m_MaxRequest = std::max(s_Alloc.m_MaxRequest, Size);
	if(s_Alloc.m_FailAt >= 0 && s_Alloc.m_Calls == (size_t)s_Alloc.m_FailAt)
		return nullptr;
	auto It = s_Alloc.m_Live.find(p);
	if(It == s_Alloc.m_Live.end())
		std::abort();
	const size_t Old = It->second;
	void *q = std::realloc(p, Size);
	if(q)
	{
		s_Alloc.m_Live.erase(It);
		s_Alloc.m_Live[q] = Size;
		s_Alloc.m_Bytes = s_Alloc.m_Bytes - Old + Size;
		s_Alloc.m_Peak = std::max(s_Alloc.m_Peak, s_Alloc.m_Bytes);
	}
	return q;
}
#define STBI_MALLOC TrackMalloc
#define STBI_REALLOC TrackRealloc
#define STBI_FREE TrackFree
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#include "cover_decode_generated.h" // actual header + cover precheck/load block extracted by runner

static void ResetAlloc(int FailAt = -1)
{
	if(s_Alloc.m_Bytes || !s_Alloc.m_Live.empty())
	{
		std::fprintf(stderr, "leak before reset: live=%zu allocations=%zu fail_at=%d\n", s_Alloc.m_Bytes, s_Alloc.m_Live.size(), s_Alloc.m_FailAt);
		std::abort();
	}
	s_Alloc = SAllocStats{};
	s_Alloc.m_FailAt = FailAt;
}
static std::vector<uint8_t> ReadBytes(const std::string &Path)
{
	std::ifstream File(Path, std::ios::binary);
	if(!File)
		std::abort();
	return std::vector<uint8_t>(std::istreambuf_iterator<char>(File), {});
}
static bool NoLeaks()
{
	return s_Alloc.m_Bytes == 0 && s_Alloc.m_Live.empty();
}
struct SFixture
{
	std::string m_Name;
	bool m_Accept;
	int m_Width, m_Height;
};
int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	if(argc != 3)
		return 2;
	const std::string Root = argv[1];
	const bool Red = std::string(argv[2]) == "red";
	std::ifstream Manifest(Root + "/manifest.txt");
	std::vector<SFixture> Fixtures;
	SFixture Fixture;
	int Accept;
	while(Manifest >> Fixture.m_Name >> Accept >> Fixture.m_Width >> Fixture.m_Height)
	{
		Fixture.m_Accept = Accept != 0;
		Fixtures.push_back(Fixture);
	}
	if(Fixtures.empty())
		return 2;
	int Failures = 0, Tested = 0;
	for(const auto &F : Fixtures)
	{
		const auto Bytes = ReadBytes(Root + "/" + F.m_Name);
		ResetAlloc();
		int W = 0, H = 0, N = 0;
		bool Failed = false;
		unsigned char *p = DecodeCover(Bytes, W, H, N, Failed);
		const size_t Calls = s_Alloc.m_Calls, Peak = s_Alloc.m_Peak, Max = s_Alloc.m_MaxRequest;
		bool Ok = F.m_Accept ? p && !Failed && W == F.m_Width && H == F.m_Height : !p && Failed;
		if(F.m_Accept && p)
		{
			const auto Expected = ReadBytes(Root + "/" + F.m_Name + ".rgba");
			Ok = Ok && Expected.size() == (size_t)W * H * 4 && std::memcmp(p, Expected.data(), Expected.size()) == 0;
		}
		if(!F.m_Accept && F.m_Width > 0)
			Ok = Ok && Max <= 65536 && Peak <= 131072;
		stbi_image_free(p);
		Ok = Ok && NoLeaks();
		if(!Ok)
			++Failures;
		++Tested;
		if(!F.m_Accept || !Ok || F.m_Width == 2048)
			std::printf("fixture=%s result=%s accepted=%d peak=%zu max_request=%zu live=%zu calls=%zu\n", F.m_Name.c_str(), Ok ? "PASS" : "FAIL", p != nullptr, Peak, Max, s_Alloc.m_Bytes, Calls);

		// Force every allocator call in ordinary small PNG/JPEG and the rejected bomb to fail once.
		if(!Red && F.m_Width > 0 && F.m_Width < 64 && F.m_Height < 64)
		{
			for(size_t I = 1; I <= Calls; ++I)
			{
				ResetAlloc((int)I);
				Failed = false;
				p = DecodeCover(Bytes, W, H, N, Failed);
				stbi_image_free(p);
				if(!NoLeaks())
					++Failures;
			}
		}
	}
	if(Red)
	{
		std::printf("RED expected_regression_failures=%d fixtures=%d\n", Failures, Tested);
		return Failures ? 0 : 1;
	}

	const auto Bomb = ReadBytes(Root + "/confirmed-8222.png");
	const auto Good = ReadBytes(Root + "/rgba8-adam7.png");
	const auto Expected = ReadBytes(Root + "/rgba8-adam7.png.rgba");
	const auto Jpeg = ReadBytes(Root + "/gray.jpg");
	std::atomic<int> Ready{0}, ThreadFailures{0};
	std::atomic<bool> Go{false};
	std::vector<std::thread> Threads;
	for(int T = 0; T < 8; ++T)
	{
		Threads.emplace_back([&, T]() {
			ResetAlloc();
			int W = 0, H = 0, N = 0;
			bool Failed = false;
			unsigned char *pHeld = DecodeCover(Good, W, H, N, Failed);
			if(!pHeld || Failed || s_Alloc.m_Bytes != Expected.size())
				++ThreadFailures;
			++Ready;
			while(!Go.load())
				std::this_thread::yield();
			for(int I = 0; I < 40; ++I)
			{
				Failed = false;
				unsigned char *pBad = DecodeCover(Bomb, W, H, N, Failed);
				if(pBad || !Failed)
					++ThreadFailures;
				stbi_image_free(pBad);
				Failed = false;
				unsigned char *pGood = DecodeCover((T & 1) ? Jpeg : Good, W, H, N, Failed);
				if(!pGood || Failed)
					++ThreadFailures;
				stbi_image_free(pGood);
				if(s_Alloc.m_Bytes != Expected.size())
					++ThreadFailures;
			}
			// The ordinary API must remain unchanged even on a thread that just rejected the bomb.
			unsigned char *pOrdinary = stbi_load_from_memory(Bomb.data(), (int)Bomb.size(), &W, &H, &N, 4);
			if(!pOrdinary || W != 1 || H != 1)
				++ThreadFailures;
			stbi_image_free(pOrdinary);
			if(pHeld && std::memcmp(pHeld, Expected.data(), Expected.size()) != 0)
				++ThreadFailures;
			stbi_image_free(pHeld);
			if(!NoLeaks())
				++ThreadFailures;
		});
	}
	while(Ready.load() != 8)
		std::this_thread::yield();
	Go = true;
	for(auto &Thread : Threads)
		Thread.join();
	Failures += ThreadFailures.load();
	std::printf("GREEN fixtures=%d allocation_failure_sweep=done threads=8 iterations_per_thread=40 ordinary_api=unchanged failures=%d live=%zu\n", Tested, Failures, s_Alloc.m_Bytes);
	return Failures ? 1 : 0;
}
