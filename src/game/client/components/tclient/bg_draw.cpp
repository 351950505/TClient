#include "bg_draw.h"

#include <base/dbg.h>
#include <base/io.h>
#include <base/windows.h>

#include <engine/client.h>
#include <engine/external/spt.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>

#include <game/client/animstate.h>
#include <game/client/components/tclient/bg_draw_file.h>
#include <game/client/gameclient.h>
#include <game/client/render.h>
#include <game/localization.h>

#include <algorithm>
#include <array>
#include <deque>
#include <vector>

#if defined(CONF_FAMILY_WINDOWS)
#include <windows.h>
#endif

#define MAX_ITEMS_TO_LOAD 65536

constexpr float AUTO_SAVE_INTERVAL = 60.0f;

static float cross(vec2 A, vec2 B)
{
	return A.x * B.y - A.y * B.x;
}

static bool line_intersects(vec2 A, vec2 B, vec2 C, vec2 D)
{
	const vec2 R = B - A;
	const vec2 S = D - C;
	const vec2 Ac = C - A;
	const float Denom = cross(R, S);
	const float Num1 = cross(Ac, S);
	const float Num2 = cross(Ac, R);
	if(Denom == 0.0f)
		return false;
	float T = Num1 / Denom;
	float U = Num2 / Denom;
	return (T >= 0.0f && T <= 1.0f) && (U >= 0.0f && U <= 1.0f);
}

class CBoundingBox
{
public:
	vec2 m_Min = vec2(0.0f, 0.0f);
	vec2 m_Max = vec2(0.0f, 0.0f);
	void ExtendBoundingBox(vec2 Pos, float Radius)
	{
		const vec2 MinPos = Pos - vec2(Radius, Radius);
		const vec2 MaxPos = Pos + vec2(Radius, Radius);
		if(m_Min == m_Max)
		{
			m_Min = MinPos;
			m_Max = MaxPos;
			return;
		}
		if(MinPos.x < m_Min.x)
			m_Min.x = MinPos.x;
		if(MinPos.y < m_Min.y)
			m_Min.y = MinPos.y;
		if(MaxPos.x > m_Max.x)
			m_Max.x = MaxPos.x;
		if(MaxPos.y > m_Max.y)
			m_Max.y = MaxPos.y;
	}
};

class CPathContainer
{
private:
	static constexpr int MAX_QUADS_PER_CONTAINER = 512; // Don't crash on OGL
	std::vector<int> m_vQuadContainerIndexes;
	int m_QuadCount = 0;
	IGraphics &m_Graphics;
	void Clear()
	{
		for(int QuadContainerIndex : m_vQuadContainerIndexes)
			m_Graphics.DeleteQuadContainer(QuadContainerIndex);
		m_vQuadContainerIndexes.clear();
		m_QuadCount = 0;
	}
	void Upload()
	{
		int &QuadContainerIndex = *(m_vQuadContainerIndexes.end() - 1);
		m_Graphics.QuadContainerUpload(QuadContainerIndex);
	}
	void AddQuads(IGraphics::CFreeformItem *pFreeformItem, int Count, ColorRGBA Color)
	{
		m_QuadCount += Count;
		if(m_vQuadContainerIndexes.empty())
		{
			m_vQuadContainerIndexes.push_back(m_Graphics.CreateQuadContainer(false));
		}
		else if(m_QuadCount > MAX_QUADS_PER_CONTAINER)
		{
			int &QuadContainerIndex = *(m_vQuadContainerIndexes.end() - 1);
			m_Graphics.QuadContainerUpload(QuadContainerIndex);
			m_vQuadContainerIndexes.push_back(m_Graphics.CreateQuadContainer(false));
			m_QuadCount = 0;
		}
		int &QuadContainerIndex = *(m_vQuadContainerIndexes.end() - 1);
		m_Graphics.SetColor(Color);
		m_Graphics.QuadContainerAddQuads(QuadContainerIndex, pFreeformItem, Count);
	}
	void AddCircle(vec2 Pos, float Angle1, float Angle2, float Width, ColorRGBA Color)
	{
		const float Radius = Width / 2.0f;
		const int Segments = (int)(std::fabs(Angle2 - Angle1) / (pi / 8.0f));
		const float SegmentAngle = (Angle2 - Angle1) / Segments;
		IGraphics::CFreeformItem FreeformItems[20];
		for(int i = 0; i < Segments; ++i)
		{
			const float A1 = Angle1 + (float)i * SegmentAngle;
			const float A2 = A1 + SegmentAngle;
			const vec2 P1 = Pos + direction(A1) * Radius;
			const vec2 P2 = Pos + direction(A2) * Radius;
			FreeformItems[i] = IGraphics::CFreeformItem(P1.x, P1.y, P2.x, P2.y, Pos.x, Pos.y, Pos.x, Pos.y);
		}
		AddQuads(FreeformItems, Segments, Color);
	}

public:
	CPathContainer(IGraphics &Graphics) :
		m_Graphics(Graphics) {}
	void Update(const CBgDrawItemData &Data)
	{
		Clear();
		if(Data.empty())
		{
			return;
		}
		size_t i = 0;
		auto SPTGetPt = [&](SPT_pt *pPt) {
			if(i >= Data.size())
				return false;
			pPt->color = {Data[i].r, Data[i].g, Data[i].b, Data[i].a};
			pPt->pos = {Data[i].x, Data[i].y};
			pPt->w = Data[i].w;
			i += 1;
			return true;
		};
		auto SPTAddQuad = [&](SPT_quad Quad, SPT_color Color) {
			IGraphics::CFreeformItem FreeformItem(
				Quad.a.x, Quad.a.y,
				Quad.b.x, Quad.b.y,
				Quad.d.x, Quad.d.y,
				Quad.c.x, Quad.c.y);
			AddQuads(&FreeformItem, 1, {Color.r, Color.g, Color.b, Color.a});
			return true;
		};
		auto SPTAddArc = [&](SPT_vec2 Pos, float A1, float A2, float R, SPT_color Color) {
			AddCircle({Pos.x, Pos.y}, A1, A2, R * 2.0f, {Color.r, Color.g, Color.b, Color.a});
			return true;
		};
		SPT_prims(SPTGetPt, SPTAddQuad, SPTAddArc);
		Upload();
	}
	void Render()
	{
		for(int QuadContainerIndex : m_vQuadContainerIndexes)
			m_Graphics.RenderQuadContainer(QuadContainerIndex, -1);
	}
	~CPathContainer()
	{
		Clear();
	}
};

class CBgDrawItem
{
private:
	CGameClient &m_This;

	int m_Drawing = true;

	CBoundingBox m_BoundingBox;
	CPathContainer m_PathContainer;
	CBgDrawItemData m_Data;

	float CurrentWidth() const
	{
		return (float)g_Config.m_TcBgDrawWidth * m_This.m_Camera.m_Zoom;
	}
	ColorRGBA CurrentColor() const
	{
		return color_cast<ColorRGBA>(ColorHSLA(g_Config.m_TcBgDrawColor));
	}

public:
	bool m_Killed = false;
	float m_SecondsAge = 0.0f;

	const CBgDrawItemData &Data() const { return m_Data; }
	const CBoundingBox &BoundingBox() const { return m_BoundingBox; }

	bool PenUp(const CBgDrawItemDataPoint &Point)
	{
		if(!m_Drawing)
			return false;
		m_Drawing = false;
		m_PathContainer.Update(m_Data);
		return true;
	}
	bool PenUp(vec2 Pos)
	{
		return PenUp({Pos, CurrentWidth(), CurrentColor()});
	}
	bool MoveTo(const CBgDrawItemDataPoint &Point)
	{
		if(!m_Drawing)
			return false;
		if(m_Data.size() > BG_DRAW_MAX_POINTS_PER_ITEM)
			return PenUp(Point.Pos());
		if(m_Data.size() >= 2 && distance((m_Data.end() - 2)->Pos(), (m_Data.end() - 1)->Pos()) < Point.w * 0.75f)
			*(m_Data.end() - 1) = Point;
		else
			m_Data.emplace_back(Point);
		m_PathContainer.Update(m_Data);
		return true;
	}
	bool MoveTo(vec2 Pos)
	{
		return MoveTo(CBgDrawItemDataPoint(Pos, CurrentWidth(), CurrentColor()));
	}
	bool PointIntersect(const vec2 Pos, const float Radius) const
	{
		if(m_Data.empty())
			return true;
		if(m_Data.size() == 1)
			return distance(m_Data[0].Pos(), Pos) < m_Data[0].w + Radius;
		vec2 C, D = m_Data[0].Pos();
		for(auto It = std::next(m_Data.begin()); It != m_Data.end(); ++It)
		{
			const CBgDrawItemDataPoint &Point = *It;
			C = D;
			D = Point.Pos();
			vec2 Closest;
			if(!closest_point_on_line(C, D, Pos, Closest))
				continue;
			if(distance(Closest, Pos) < Radius + Point.w)
				return true;
		}
		return false;
	}
	bool LineIntersect(const vec2 A, const vec2 B) const
	{
		if(m_Data.empty())
			return true;
		if(m_Data.size() == 1)
		{
			vec2 P = m_Data[0].Pos();
			vec2 Closest;
			if(!closest_point_on_line(A, B, P, Closest))
				return false;
			return distance(Closest, P) < m_Data[0].w;
		}
		vec2 C, D = m_Data[0].Pos();
		for(auto It = std::next(m_Data.begin()); It != m_Data.end(); ++It)
		{
			const CBgDrawItemDataPoint &Point = *It;
			C = D;
			D = Point.Pos();
			if(line_intersects(A, B, C, D))
				return true;
		}
		return false;
	}
	void Render()
	{
		m_PathContainer.Render();
	}

	CBgDrawItem() = delete;
	CBgDrawItem(CGameClient &This, vec2 StartPos) :
		m_This(This), m_PathContainer(*This.Graphics())
	{
		m_BoundingBox.ExtendBoundingBox(StartPos, CurrentWidth());
		m_Data.emplace_back(StartPos, CurrentWidth(), CurrentColor());
	}
	CBgDrawItem(CGameClient &This, CBgDrawItemDataPoint StartPoint) :
		CBgDrawItem(This, StartPoint.Pos())
	{
		m_Data.clear();
		m_Data.push_back(StartPoint);
	}
	CBgDrawItem(CGameClient &This, const CBgDrawItemData &Data) :
		CBgDrawItem(This, Data[0])
	{
		if(Data.size() > 1)
			for(auto It = Data.begin() + 1; It != Data.end(); ++It)
			{
				const CBgDrawItemDataPoint &Point = *It;
				MoveTo(Point);
			}
		PenUp(Data.back());
	}
};

static void DoInput(CBgDraw &This, CBgDraw::InputMode Mode, int Value)
{
	CBgDraw::InputMode &Input = This.m_aInputData[g_Config.m_ClDummy];
	if(Value > 0)
		Input = Mode;
	else if(Input == Mode)
		Input = CBgDraw::InputMode::NONE;
}

void CBgDraw::ConBgDraw(IConsole::IResult *pResult, void *pUserData)
{
	DoInput(*(CBgDraw *)pUserData, InputMode::DRAW, pResult->GetInteger(0));
}

void CBgDraw::ConBgDrawErase(IConsole::IResult *pResult, void *pUserData)
{
	DoInput(*(CBgDraw *)pUserData, InputMode::ERASE, pResult->GetInteger(0));
}

void CBgDraw::ConBgDrawReset(IConsole::IResult *pResult, void *pUserData)
{
	CBgDraw *pThis = (CBgDraw *)pUserData;
	pThis->Reset();
	pThis->m_Persistence.m_Dirty = true;
}

void CBgDraw::ConBgDrawSave(IConsole::IResult *pResult, void *pUserData)
{
	CBgDraw *pThis = (CBgDraw *)pUserData;
	pThis->Save(pResult->GetString(0), true);
}

void CBgDraw::ConBgDrawLoad(IConsole::IResult *pResult, void *pUserData)
{
	CBgDraw *pThis = (CBgDraw *)pUserData;
	pThis->Load(pResult->GetString(0), true);
}

static std::string BgDrawFilename(CGameClient &This, const char *pFilename)
{
	char aFilename[IO_MAX_PATH_LENGTH];
	if(pFilename && pFilename[0] != '\0')
	{
		if(str_endswith_nocase(pFilename, ".csv"))
			str_format(aFilename, sizeof(aFilename), "bgdraw/%s", pFilename);
		else
			str_format(aFilename, sizeof(aFilename), "bgdraw/%s.csv", pFilename);
	}
	else
	{
		SHA256_DIGEST Sha256 = This.Map()->Sha256();
		char aSha256[SHA256_MAXSTRSIZE];
		sha256_str(Sha256, aSha256, sizeof(aSha256));
		str_format(aFilename, sizeof(aFilename), "bgdraw/%s_%s.csv", This.Map()->BaseName(), aSha256);
	}
	return aFilename;
}

class CBgDrawSaveFile
{
	IStorage *m_pStorage;
	const char *m_pPath;
	char m_aTempPath[IO_MAX_PATH_LENGTH];
	IOHANDLE m_Handle = nullptr;

public:
	CBgDrawSaveFile(IStorage *pStorage, const char *pPath) :
		m_pStorage(pStorage), m_pPath(pPath)
	{
		IStorage::FormatTmpPath(m_aTempPath, sizeof(m_aTempPath), pPath);
	}
	bool Open()
	{
		if(!m_pStorage->CreateFolder("bgdraw", IStorage::TYPE_SAVE))
			return false;
		m_Handle = m_pStorage->OpenFile(m_aTempPath, IOFLAG_WRITE, IStorage::TYPE_SAVE);
		return m_Handle != nullptr;
	}
	unsigned Write(const char *pData, unsigned Length) { return io_write(m_Handle, pData, Length); }
	bool WriteNewline() { return io_write_newline(m_Handle); }
	bool Sync() { return io_sync(m_Handle) == 0; }
	bool Close() { return io_close(m_Handle) == 0; }
	bool Replace()
	{
#if defined(CONF_FAMILY_WINDOWS)
		// fs_rename retries access-denied errors by deleting the destination.
		// A failed drawing save must always leave the previous file intact.
		char aTempPath[IO_MAX_PATH_LENGTH], aPath[IO_MAX_PATH_LENGTH];
		m_pStorage->GetCompletePath(IStorage::TYPE_SAVE, m_aTempPath, aTempPath, sizeof(aTempPath));
		m_pStorage->GetCompletePath(IStorage::TYPE_SAVE, m_pPath, aPath, sizeof(aPath));
		return MoveFileExW(windows_utf8_to_wide(aTempPath).c_str(), windows_utf8_to_wide(aPath).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
		return m_pStorage->RenameFile(m_aTempPath, m_pPath, IStorage::TYPE_SAVE);
#endif
	}
	void Cleanup() { m_pStorage->RemoveFile(m_aTempPath, IStorage::TYPE_SAVE); }
};

bool CBgDraw::Save(const char *pFilename, bool Verbose)
{
	if(!m_Persistence.m_Dirty)
	{
		if(Verbose)
			GameClient()->Echo(TCLocalize("No changes since last save", "bgdraw"));
		return false;
	}
	// The map may already have changed when a failed drawing is retried.
	const std::string Path = pFilename && pFilename[0] ? BgDrawFilename(*GameClient(), pFilename) : m_Persistence.m_Path;
	if(Path.empty())
		return false;
	CBgDrawSaveFile File(Storage(), Path.c_str());
	int Written = 0;
	const bool Success = BgDrawPersistence::Save(File, m_Persistence.m_Dirty, [&](auto WriteLine) {
		for(const CBgDrawItem &Item : *m_pvItems)
		{
			if(!BgDrawFile::Write(WriteLine, Item.Data()))
				return false;
			++Written;
		}
		return true;
	});
	if(!Success)
		GameClient()->Echo(TCLocalize("Failed to save background drawing; unsaved changes retained", "bgdraw"));
	else if(Verbose)
	{
		char aMsg[256];
		str_format(aMsg, sizeof(aMsg), TCLocalize("Written %d items", "bgdraw"), Written);
		GameClient()->Echo(aMsg);
	}
	return Success;
}

bool CBgDraw::Load(const char *pFilename, bool Verbose)
{
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return false;
	const std::string Path = pFilename && pFilename[0] ? BgDrawFilename(*GameClient(), pFilename) : m_Persistence.m_Path;
	if(Path.empty())
		return false;
	IOHANDLE Handle = Storage()->OpenFile(Path.c_str(), IOFLAG_READ, IStorage::TYPE_SAVE);
	if(!Handle)
		return false;

	const int64_t FileSize = io_length(Handle);
	if(FileSize < 0 || FileSize > BgDrawFile::MAX_FILE_SIZE)
	{
		io_close(Handle);
		GameClient()->Echo(TCLocalize("Failed to load background drawing: file size unavailable or exceeds 16 MiB"));
		return false;
	}
	BgDrawFile::CLineReader Reader([Handle](char *pBuf, unsigned Size) {
		return io_read(Handle, pBuf, Size);
	});
	auto ReadLine = [&Reader](char *pBuf, int Length) {
		return Reader.ReadLine(pBuf, Length);
	};

	std::deque<CBgDrawItemData> Queue;
	int ItemsLoaded = 0;
	int ItemsDiscarded = 0;
	{
		CBgDrawItemData Data;
		while(BgDrawFile::Read(ReadLine, Data) && (ItemsLoaded++) < MAX_ITEMS_TO_LOAD)
		{
			if((int)Queue.size() > g_Config.m_TcBgDrawMaxItems)
			{
				ItemsDiscarded += 1;
				Queue.pop_front();
			}
			Queue.push_back(Data);
		}
	}
	io_close(Handle);
	if(Reader.LimitExceeded())
	{
		GameClient()->Echo(TCLocalize("Failed to load background drawing: read exceeds 16 MiB"));
		return false;
	}
	MakeSpaceFor(Queue.size());
	for(const CBgDrawItemData &Data : Queue)
		AddItem(*GameClient(), Data);
	if(Verbose)
	{
		char aInfo[256];
		if(ItemsDiscarded == 0)
			str_format(aInfo, sizeof(aInfo), TCLocalize("Loaded %d items", "bgdraw"), ItemsLoaded);
		else
			str_format(aInfo, sizeof(aInfo), TCLocalize("Loaded %d items (discarded %d items)", "bgdraw"), ItemsLoaded - ItemsDiscarded, ItemsDiscarded);
		GameClient()->Echo(aInfo);
	}

	return true;
}

template<typename... T>
CBgDrawItem *CBgDraw::AddItem(T &&...Aargs)
{
	MakeSpaceFor(1);
	if(g_Config.m_TcBgDrawMaxItems == 0)
		return nullptr;
	m_pvItems->emplace_back(std::forward<T>(Aargs)...);
	m_Persistence.m_Dirty = true;
	return &m_pvItems->back();
}

void CBgDraw::MakeSpaceFor(int Count)
{
	if(g_Config.m_TcBgDrawMaxItems == 0 || Count >= g_Config.m_TcBgDrawMaxItems)
	{
		m_apActiveItems.fill(std::nullopt);
		m_pvItems->clear();
		return;
	}
	while((int)m_pvItems->size() + Count > g_Config.m_TcBgDrawMaxItems)
	{
		// Prevent floating pointer
		for(std::optional<CBgDrawItem *> &ActiveItem : m_apActiveItems)
		{
			if(!ActiveItem.has_value())
				continue;
			if(ActiveItem.value() != &(*m_pvItems->begin()))
				continue;
			ActiveItem = std::nullopt;
		}
		m_pvItems->pop_front();
	}
}

void CBgDraw::OnConsoleInit()
{
	Console()->Register("+bg_draw", "", CFGFLAG_CLIENT, ConBgDraw, this, "Draw on the in game background");
	Console()->Register("+bg_draw_erase", "", CFGFLAG_CLIENT, ConBgDrawErase, this, "Erase items on the in game background");
	Console()->Register("bg_draw_reset", "", CFGFLAG_CLIENT, ConBgDrawReset, this, "Reset all drawings on the background");
	Console()->Register("bg_draw_save", "?r[filename]", CFGFLAG_CLIENT, ConBgDrawSave, this, "Save drawings to a given csv file (defaults to map name)");
	Console()->Register("bg_draw_load", "?r[filename]", CFGFLAG_CLIENT, ConBgDrawLoad, this, "Load drawings from a given csv file (defaults to map name)");
}

void CBgDraw::OnRender()
{
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;

	Graphics()->TextureClear();

	float Delta = Client()->RenderFrameTime();

	m_NextAutoSave -= Delta;
	if(m_NextAutoSave < 0)
	{
		if(g_Config.m_TcBgDrawAutoSaveLoad)
		{
			RetryPendingSaves();
			Save(nullptr, false);
		}
		m_NextAutoSave = AUTO_SAVE_INTERVAL;
	}

	for(int Dummy = 0; Dummy < NUM_DUMMIES; ++Dummy)
	{
		// Handle updating active item
		const InputMode Input = m_aInputData[Dummy];
		std::optional<CBgDrawItem *> &ActiveItem = m_apActiveItems[Dummy];
		vec2 Pos;
		if(GameClient()->m_Snap.m_SpecInfo.m_Active && Dummy == g_Config.m_ClDummy)
			Pos = GameClient()->m_Camera.m_Center;
		else
			Pos = (GameClient()->m_Controls.m_aTargetPos[Dummy] - GameClient()->m_Camera.m_Center) * GameClient()->m_Camera.m_Zoom + GameClient()->m_Camera.m_Center;
		if(Input == InputMode::DRAW)
		{
			if(ActiveItem.has_value())
				(*ActiveItem)->MoveTo(Pos);
			else if(CBgDrawItem *pItem = AddItem(*GameClient(), Pos))
				ActiveItem = pItem;
			m_Persistence.m_Dirty = true;
		}
		else if(ActiveItem.has_value())
		{
			(*ActiveItem)->PenUp(Pos);
			ActiveItem = std::nullopt;
		}
		std::optional<vec2> &LastPos = m_aLastPos[Dummy];
		if(Input == InputMode::ERASE)
		{
			if(LastPos.has_value())
			{
				for(CBgDrawItem &Item : *m_pvItems)
					if(Item.LineIntersect(*LastPos, Pos))
						Item.m_Killed = true;
			}
			else
			{
				for(CBgDrawItem &Item : *m_pvItems)
					if(Item.PointIntersect(Pos, 2.0f))
						Item.m_Killed = true;
			}
		}
		else
		{
			LastPos = std::nullopt;
		}
	}
	// Remove extra items
	MakeSpaceFor(0);
	// Update age of items, delete old items, render items
	const CScreenRect ScreenRect = Graphics()->GetScreen();
	for(CBgDrawItem &Item : *m_pvItems)
	{
		// If this item is currently active
		if(std::any_of(std::begin(m_apActiveItems), std::end(m_apActiveItems), [&](const std::optional<CBgDrawItem *> &ActiveItem) {
			   return ActiveItem.value_or(nullptr) == &Item;
		   }))
		{
			Item.m_SecondsAge = 0.0f;
		}
		else
		{
			Item.m_SecondsAge += Delta;
			if(g_Config.m_TcBgDrawFadeTime > 0 && Item.m_SecondsAge > (float)g_Config.m_TcBgDrawFadeTime)
				Item.m_Killed = true;
		}
		if(Item.BoundingBox().m_Min.x < ScreenRect.m_BottomRight.x &&
			Item.BoundingBox().m_Max.x > ScreenRect.m_TopLeft.x &&
			Item.BoundingBox().m_Min.y < ScreenRect.m_BottomRight.y &&
			Item.BoundingBox().m_Max.y > ScreenRect.m_TopLeft.y)
		{
			Item.Render();
		}
	}
	// Remove killed items
	if(m_pvItems->remove_if([&](CBgDrawItem &Item) {
		   if(!Item.m_Killed)
			   return false;
		   for(auto &ActiveItem : m_apActiveItems)
			   if(ActiveItem.value_or(nullptr) == &Item)
				   ActiveItem = std::nullopt;
		   return true;
	   }))
		m_Persistence.m_Dirty = true;
	Graphics()->SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f));
}

void CBgDraw::Reset()
{
	m_aInputData.fill(InputMode::NONE);
	m_aLastPos.fill(std::nullopt);
	m_apActiveItems.fill(std::nullopt);
	m_pvItems->clear();
}

void CBgDraw::SuspendDrawing()
{
	BgDrawPersistence::CDrawing Drawing;
	if(m_Persistence.m_Dirty)
	{
		Drawing.reserve(m_pvItems->size());
		for(const CBgDrawItem &Item : *m_pvItems)
			Drawing.push_back(Item.Data());
	}
	// Retain data, not graphics containers, and keep its original map path.
	m_Persistence.Suspend(std::move(Drawing));
	Reset();
}

void CBgDraw::BeginDrawing()
{
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return; // Normal map loading reaches ONLINE after OnMapLoad.
	const std::string Path = BgDrawFilename(*GameClient(), nullptr);
	if(m_Persistence.m_Path == Path)
		return; // Demo playback enters its state before OnMapLoad.
	if(!m_Persistence.m_Path.empty())
	{
		if(g_Config.m_TcBgDrawAutoSaveLoad)
			Save(nullptr, false);
		SuspendDrawing();
	}
	const auto Drawing = m_Persistence.Begin(Path);
	if(Drawing)
	{
		for(const CBgDrawItemData &Data : *Drawing)
			m_pvItems->emplace_back(*GameClient(), Data);
	}
	else if(g_Config.m_TcBgDrawAutoSaveLoad)
	{
		Load(nullptr, false);
		m_Persistence.m_Dirty = false;
	}
	if(g_Config.m_TcBgDrawAutoSaveLoad)
		RetryPendingSaves();
	m_NextAutoSave = AUTO_SAVE_INTERVAL;
}

void CBgDraw::RetryPendingSaves()
{
	m_Persistence.Retry([&](const char *pPath, const BgDrawPersistence::CDrawing &Drawing) {
		CBgDrawSaveFile File(Storage(), pPath);
		bool Dirty = true;
		return BgDrawPersistence::Save(File, Dirty, [&](auto WriteLine) {
			for(const CBgDrawItemData &Data : Drawing)
				if(!BgDrawFile::Write(WriteLine, Data))
					return false;
			return true;
		});
	});
}

void CBgDraw::OnMapLoad()
{
	// Capture identity while the map is valid. Pending data never borrows the
	// identity of a later map, even if saving keeps failing across transitions.
	BeginDrawing();
}

void CBgDraw::OnStateChange(int NewState, int OldState)
{
	if(OldState == IClient::STATE_ONLINE || OldState == IClient::STATE_DEMOPLAYBACK)
	{
		if(g_Config.m_TcBgDrawAutoSaveLoad)
		{
			RetryPendingSaves();
			Save(nullptr, false);
		}
		SuspendDrawing();
	}
	if(NewState == IClient::STATE_ONLINE || NewState == IClient::STATE_DEMOPLAYBACK)
		BeginDrawing();
}

void CBgDraw::OnShutdown()
{
	if(g_Config.m_TcBgDrawAutoSaveLoad)
	{
		RetryPendingSaves();
		Save(nullptr, false);
	}
	SuspendDrawing();
}

CBgDraw::CBgDraw() :
	m_pvItems(new std::list<CBgDrawItem>())
{
	Reset();
}

CBgDraw::~CBgDraw()
{
	delete m_pvItems;
}
