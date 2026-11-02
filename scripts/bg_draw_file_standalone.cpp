// Standalone: compile this file together with bg_draw_file.cpp.
// Assertions execute the checks and must not be disabled in release builds.
#ifdef NDEBUG
#error "bg_draw_file_standalone requires assertions enabled"
#endif
#include "../src/game/client/components/tclient/bg_draw_file.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>

int main()
{
	std::vector<std::string> Lines = {"# comment", "1,2", "3,4,6,0.1,0.2,0.3,0.4", ",", "5,6"};
	size_t Index = 0;
	auto ReadLine = [&](char *pBuf, int Length) {
		if(Index == Lines.size())
			return false;
		std::strncpy(pBuf, Lines[Index++].c_str(), Length - 1);
		pBuf[Length - 1] = '\0';
		return true;
	};
	CBgDrawItemData Data;
	assert(BgDrawFile::Read(ReadLine, Data));
	assert(Data.size() == 2 && Data[0].w == 5.0f && Data[1].x == 3.0f && Data[1].a == 0.4f);
	assert(BgDrawFile::Read(ReadLine, Data));
	assert(Data.size() == 1 && Data[0].x == 5.0f);
	assert(!BgDrawFile::Read(ReadLine, Data));

	Lines.assign(4100, "1,2");
	Lines.emplace_back(",");
	Lines.emplace_back("7,8");
	Lines.emplace_back(",");
	Index = 0;
	assert(BgDrawFile::Read(ReadLine, Data) && Data.size() == 4096);
	assert(BgDrawFile::Read(ReadLine, Data) && Data.size() == 1 && Data[0].x == 7.0f);
	std::vector<std::string> Written;
	assert(BgDrawFile::Write([&](const char *pLine) { Written.emplace_back(pLine); return true; }, Data));
	assert(Written.size() == 2 && Written.back() == ",");

	std::string Input = "1,2\r\n,\n" + std::string(5000, 'x') + "\n3,4\n,\n5,6";
	size_t Pos = 0, Calls = 0;
	BgDrawFile::CLineReader Reader([&](char *pBuf, unsigned Size) {
		++Calls;
		const size_t Count = std::min<size_t>(Size, Input.size() - Pos);
		std::memcpy(pBuf, Input.data() + Pos, Count);
		Pos += Count;
		return static_cast<unsigned>(Count);
	});
	auto BufferedRead = [&](char *pBuf, int Length) { return Reader.ReadLine(pBuf, Length); };
	assert(BgDrawFile::Read(BufferedRead, Data) && Data.size() == 1 && Data[0].x == 1);
	assert(BgDrawFile::Read(BufferedRead, Data) && Data.size() == 1 && Data[0].x == 3);
	assert(BgDrawFile::Read(BufferedRead, Data) && Data.size() == 1 && Data[0].x == 5);
	assert(!BgDrawFile::Read(BufferedRead, Data) && !Reader.LimitExceeded());
	assert(Calls < 10);

	for(unsigned Extra = 0; Extra <= 1; ++Extra)
	{
		Input.assign(BgDrawFile::MAX_FILE_SIZE + Extra, 'x');
		Pos = 0;
		BgDrawFile::CLineReader Limited([&](char *pBuf, unsigned Size) {
			const size_t Count = std::min<size_t>(Size, Input.size() - Pos);
			std::memcpy(pBuf, Input.data() + Pos, Count);
			Pos += Count;
			return static_cast<unsigned>(Count);
		});
		char Buffer[256];
		assert(Limited.ReadLine(Buffer, sizeof(Buffer)) == (Extra == 0));
		assert(Limited.LimitExceeded() == (Extra != 0));
		assert(Pos <= BgDrawFile::MAX_FILE_SIZE + 1);
	}
	return 0;
}
