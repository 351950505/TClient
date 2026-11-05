#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_BG_DRAW_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_BG_DRAW_H

#include <engine/client/enums.h>
#include <engine/console.h>

#include <game/client/component.h>
#include <game/client/components/tclient/bg_draw_file.h>

#include <array>
#include <cstring>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace BgDrawPersistence
{
using CDrawing = std::vector<CBgDrawItemData>;

// The file adapter keeps all failure points, including close and replacement,
// in the same transaction. Never truncate the destination before it succeeds.
template<typename TFile, typename TWrite>
bool Save(TFile &File, bool &Dirty, TWrite Write)
{
	if(!Dirty || !File.Open())
		return false;
	bool Success = Write([&File](const char *pLine) {
		const unsigned Length = (unsigned)std::strlen(pLine);
		return File.Write(pLine, Length) == Length && File.WriteNewline();
	});
	if(Success)
		Success = File.Sync();
	// Close even after a write or sync error, and check its result separately.
	const bool Closed = File.Close();
	Success = Success && Closed;
	if(Success)
		Success = File.Replace();
	if(Success)
		Dirty = false;
	else
		File.Cleanup();
	return Success;
}

class CState
{
	std::map<std::string, CDrawing> m_Pending;

public:
	bool m_Dirty = false;
	std::string m_Path;

	void Suspend(CDrawing Drawing)
	{
		if(m_Dirty && !m_Path.empty())
			m_Pending.insert_or_assign(m_Path, std::move(Drawing));
		m_Dirty = false;
		m_Path.clear();
	}

	std::optional<CDrawing> Begin(std::string Path)
	{
		m_Path = std::move(Path);
		const auto It = m_Pending.find(m_Path);
		if(It == m_Pending.end())
			return std::nullopt;
		CDrawing Drawing = std::move(It->second);
		m_Pending.erase(It);
		m_Dirty = true;
		return Drawing;
	}

	template<typename TSave>
	void Retry(TSave Save)
	{
		for(auto It = m_Pending.begin(); It != m_Pending.end();)
		{
			if(Save(It->first.c_str(), It->second))
				It = m_Pending.erase(It);
			else
				++It;
		}
	}
};
} // namespace BgDrawPersistence

#define BG_DRAW_MAX_POINTS_PER_ITEM 1024

class CBgDrawItem;

class CBgDraw : public CComponent
{
private:
	float m_NextAutoSave = 0.0f;
	BgDrawPersistence::CState m_Persistence;
	std::array<std::optional<CBgDrawItem *>, NUM_DUMMIES> m_apActiveItems;
	std::array<std::optional<vec2>, NUM_DUMMIES> m_aLastPos;
	std::list<CBgDrawItem> *m_pvItems;
	static void ConBgDraw(IConsole::IResult *pResult, void *pUserData);
	static void ConBgDrawErase(IConsole::IResult *pResult, void *pUserData);
	static void ConBgDrawReset(IConsole::IResult *pResult, void *pUserData);
	static void ConBgDrawSave(IConsole::IResult *pResult, void *pUserData);
	static void ConBgDrawLoad(IConsole::IResult *pResult, void *pUserData);
	void Reset();
	void SuspendDrawing();
	void BeginDrawing();
	void RetryPendingSaves();
	bool Save(const char *pFile, bool Verbose);
	bool Load(const char *pFile, bool Verbose);
	template<typename... T>
	CBgDrawItem *AddItem(T &&...Args);
	void MakeSpaceFor(int Count);

public:
	enum class InputMode
	{
		NONE = 0,
		DRAW = 1,
		ERASE = 2,
	};
	std::array<InputMode, NUM_DUMMIES> m_aInputData;

	int Sizeof() const override { return sizeof(*this); }
	void OnConsoleInit() override;
	void OnRender() override;
	void OnStateChange(int NewState, int OldState) override;
	void OnMapLoad() override;
	void OnShutdown() override;

	CBgDraw();
	~CBgDraw() override;
};

#endif
