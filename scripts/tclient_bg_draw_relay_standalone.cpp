// Standalone regression for the production persistence and relay routing seams.
// Build outside the shared CMake trees; argv[1] must be a new temporary directory.
#include <game/client/components/tclient/bg_draw.h>
#include <game/client/components/tclient/relay_client.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifdef NDEBUG
#error This regression requires enabled assertions.
#endif

namespace
{
enum class EFailure
{
	NONE,
	OPEN,
	SHORT_WRITE,
	NEWLINE,
	SYNC,
	CLOSE,
	RENAME,
};

class CFile
{
	std::filesystem::path m_Path, m_Temp;
	EFailure m_Failure;
	FILE *m_pFile = nullptr;

public:
	int m_Opens = 0, m_Closes = 0;
	bool m_Synced = false;

	CFile(const std::filesystem::path &Path, EFailure Failure) :
		m_Path(Path), m_Temp(Path.string() + ".tmp"), m_Failure(Failure) {}
	bool Open()
	{
		++m_Opens;
		if(m_Failure == EFailure::OPEN)
			return false;
		m_pFile = std::fopen(m_Temp.string().c_str(), "wb");
		return m_pFile != nullptr;
	}
	unsigned Write(const char *pData, unsigned Length)
	{
		return (unsigned)std::fwrite(pData, 1, m_Failure == EFailure::SHORT_WRITE && Length ? Length - 1 : Length, m_pFile);
	}
	bool WriteNewline() { return m_Failure != EFailure::NEWLINE && std::fputc('\n', m_pFile) != EOF; }
	bool Sync()
	{
		m_Synced = true;
		if(m_Failure == EFailure::SYNC || std::fflush(m_pFile) != 0)
			return false;
#if defined(_WIN32)
		return _commit(_fileno(m_pFile)) == 0;
#else
		return fsync(fileno(m_pFile)) == 0;
#endif
	}
	bool Close()
	{
		++m_Closes;
		const bool Closed = std::fclose(m_pFile) == 0;
		m_pFile = nullptr;
		return Closed && m_Failure != EFailure::CLOSE;
	}
	bool Replace()
	{
		assert(m_Closes == 1 && m_Synced);
		if(m_Failure == EFailure::RENAME)
			return false;
		// std::filesystem::rename does not replace on Windows. This test uses
		// the platform's atomic replacement, just like production fs_rename.
#if defined(_WIN32)
		return MoveFileExW(m_Temp.c_str(), m_Path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
		return std::rename(m_Temp.c_str(), m_Path.c_str()) == 0;
#endif
	}
	void Cleanup() { std::filesystem::remove(m_Temp); }
	bool TempExists() const { return std::filesystem::exists(m_Temp); }
};

std::string Read(const std::filesystem::path &Path)
{
	std::ifstream In(Path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(In), {});
}

BgDrawPersistence::CDrawing Drawing(float X)
{
	return {{CBgDrawItemDataPoint(X, 2, 3, 1, 0, 0, 1), CBgDrawItemDataPoint(X + 1, 4, 3, 1, 0, 0, 1)}};
}

bool SaveDrawing(CFile &File, bool &Dirty, const BgDrawPersistence::CDrawing &Data)
{
	return BgDrawPersistence::Save(File, Dirty, [&](auto WriteLine) {
		for(const auto &Item : Data)
			if(!BgDrawFile::Write(WriteLine, Item))
				return false;
		return true;
	});
}

void TestSaves(const std::filesystem::path &Dir)
{
	const auto Path = Dir / "drawing.csv";
	const auto Data = Drawing(17);
	for(EFailure Failure : {EFailure::OPEN, EFailure::SHORT_WRITE, EFailure::NEWLINE, EFailure::SYNC, EFailure::CLOSE, EFailure::RENAME})
	{
		{ std::ofstream Out(Path, std::ios::binary); Out << "previous complete drawing\n"; }
		bool Dirty = true;
		CFile File(Path, Failure);
		assert(!SaveDrawing(File, Dirty, Data));
		assert(Dirty);
		assert(Read(Path) == "previous complete drawing\n");
		assert(File.m_Closes == (Failure == EFailure::OPEN ? 0 : 1));
		assert(!File.TempExists());
		CFile Retry(Path, EFailure::NONE);
		assert(SaveDrawing(Retry, Dirty, Data));
		assert(!Dirty && Retry.m_Closes == 1 && !Retry.TempExists());
		FILE *pSaved = std::fopen(Path.string().c_str(), "rb");
		CBgDrawItemData Loaded;
		assert(pSaved && BgDrawFile::Read([pSaved](char *pBuf, int Size) { return std::fgets(pBuf, Size, pSaved) != nullptr; }, Loaded));
		assert(Loaded.size() == 2 && Loaded[0].x == 17 && Loaded[1].x == 18);
		std::fclose(pSaved);
	}
#if defined(_WIN32)
	// Real sharing violation: production must not delete the old file and retry.
	{ std::ofstream Out(Path, std::ios::binary); Out << "locked previous drawing\n"; }
	HANDLE Locked = CreateFileW(Path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	assert(Locked != INVALID_HANDLE_VALUE);
	bool LockedDirty = true;
	CFile Blocked(Path, EFailure::NONE);
	assert(!SaveDrawing(Blocked, LockedDirty, Data));
	assert(LockedDirty && Blocked.m_Closes == 1 && !Blocked.TempExists());
	assert(Read(Path) == "locked previous drawing\n");
	CloseHandle(Locked);
	CFile Unlocked(Path, EFailure::NONE);
	assert(SaveDrawing(Unlocked, LockedDirty, Data) && !LockedDirty);
#endif
	// Erasing everything must replace an older nonempty file successfully.
	bool Dirty = true;
	CFile Empty(Path, EFailure::NONE);
	assert(SaveDrawing(Empty, Dirty, {}) && !Dirty && Read(Path).empty());
	CFile Clean(Path, EFailure::NONE);
	assert(!SaveDrawing(Clean, Dirty, Data) && Clean.m_Opens == 0);
}

void TestTransitions(const std::filesystem::path &Dir)
{
	BgDrawPersistence::CState State;
	const auto A = (Dir / "map-A_hash.csv").string();
	const auto B = (Dir / "map-B_hash.csv").string();
	assert(!State.Begin(A));
	State.m_Dirty = true;
	State.Suspend(Drawing(11)); // Actual graphics can now be cleared by Reset.
	assert(!State.Begin(B)); // Never put A's drawing on B.
	int Attempts = 0;
	State.Retry([&](const char *pPath, const auto &Data) {
		++Attempts;
		assert(pPath == A && Data[0][0].x == 11);
		bool Dirty = true;
		CFile File(pPath, EFailure::CLOSE);
		return SaveDrawing(File, Dirty, Data);
	});
	assert(Attempts == 1);
	State.m_Dirty = true;
	State.Suspend(Drawing(22));
	const auto Restored = State.Begin(A);
	assert(Restored && (*Restored)[0][0].x == 11 && State.m_Dirty);
	Attempts = 0;
	State.Retry([&](const char *pPath, const auto &Data) {
		++Attempts;
		assert(pPath == B && Data[0][0].x == 22);
		bool Dirty = true;
		CFile File(pPath, EFailure::NONE);
		return SaveDrawing(File, Dirty, Data);
	});
	assert(Attempts == 1 && std::filesystem::exists(B) && !std::filesystem::exists(A));
	State.Suspend({}); // Dirty empty drawings also survive a failed transition.
	const auto Empty = State.Begin(A);
	assert(Empty && Empty->empty() && State.m_Dirty);
	State.Retry([](const char *, const auto &) { assert(false); return false; });
}

void TestRelay()
{
	for(bool Enabled : {false, true})
		for(bool Online : {false, true})
			for(const std::vector<int> Peers : {std::vector<int>{}, {1}, {1, 1}, {1, 0}, {0, 0}})
			{
				int DirectSends = 0, RelaySends = 0;
				const int Count = RelayRouting::Broadcast(Enabled, Online, Peers,
					[](int Peer) { return Peer != 0; },
					[&](int Peer) { assert(Peer); ++DirectSends; },
					[&] { ++RelaySends; });
				const int Expected = Enabled ? (int)std::count(Peers.begin(), Peers.end(), 1) : 0;
				const bool AllDirect = Enabled && !Peers.empty() && Expected == (int)Peers.size();
				assert(Count == Expected && DirectSends == Expected);
				assert(RelaySends == (Online && !AllDirect ? 1 : 0));
			}
}
} // namespace

int main(int argc, char **argv)
{
	assert(argc == 2);
	const std::filesystem::path Dir(argv[1]);
	assert(std::filesystem::is_directory(Dir) && std::filesystem::is_empty(Dir));
	TestSaves(Dir);
	TestTransitions(Dir);
	TestRelay();
	std::puts("PASS: durable save failure/retry, map recovery, empty drawing, P2P/relay/offline matrix");
}
