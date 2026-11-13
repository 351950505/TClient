#include "island_music.h"

 #include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <base/color.h>
#include <base/dbg.h>
#include <base/math.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/client/link_probe.h>
#include <engine/engine.h>
#include <engine/graphics.h>
#include <engine/http.h>
#include <engine/shared/config.h>
#include <engine/shared/jobs.h>
#include <engine/shared/json.h>
#include <engine/textrender.h>

#include <game/client/gameclient.h>
#include <game/localization.h>

// stb_image: single-header decoder for PNG/JPEG/etc. Used for album covers
// returned by the Windows SMTC thumbnail API, which are typically JPEG and
// therefore cannot be decoded by the engine's PNG-only loader.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#include "lyric_parser.h"
#include "stb_image.h"

#if defined(CONF_FAMILY_WINDOWS)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// 系统音量控制（Core Audio endpoint volume 为主，mixer API 兜底）
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif

#if defined(CONF_FAMILY_WINDOWS) && __has_include(<winrt/Windows.Media.Control.h>)
#define TCLIENT_HAS_WINRT_MUSIC 1
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/base.h>
namespace WmControl = winrt::Windows::Media::Control;
using WmSession = WmControl::GlobalSystemMediaTransportControlsSession;
using WmSessionManager = WmControl::GlobalSystemMediaTransportControlsSessionManager;
#else
#define TCLIENT_HAS_WINRT_MUSIC 0
#endif

using TcLyric::SLyricLine;
using TcLyric::SWord;
using TcLyric::ParseSyncedLyrics;
using TcLyric::ParseYrcLyrics;

class CIslandMusic::CImpl
{
public:
	IClient *m_pClient = nullptr;
	CGameClient *m_pGameClient = nullptr;
	IHttp *m_pHttp = nullptr;
	IEngine *m_pEngine = nullptr;
	IGraphics *m_pGraphics = nullptr;
	ITextRender *m_pTextRender = nullptr;

	// lyric marquee scroll state
	std::string m_LastScrollText;
	std::string m_CurScrollText;
	int64_t m_ScrollStartTick = 0;

	// 卡拉OK/标题逐帧文本缓存(2026-11-01):yrc 逐字歌词挂上后,GreenEndX 每帧对
	// 每个字做 substr 堆分配 + TextWidth 测量(实测 DDNet 冲到 167% CPU,灵动岛
	// 掉帧)。同一行文字不变 —— 按文本+字号缓存字宽/整行宽/适配结果,换行、
	// 换曲、字号变化才重算,稳态下每帧 0 次 TextWidth。
	std::string m_KaraokeKey;          // 当前字宽缓存对应的行文本
	float m_KaraokeSize = -1.0f;       // 字号
	float m_KaraokeFullW = -1.0f;      // 整行宽度
	std::vector<float> m_KaraokeWordW; // 与 m_vWords 对齐(非法词记 -1)
	std::string m_TitleFitKey;         // 标题 FitText 缓存键(文本)
	float m_TitleFitMaxW = -1.0f;
	std::string m_TitleFitText;
	std::string m_SongFitKey;          // 无词态歌名 FitText 缓存键
	float m_SongFitMaxW = -1.0f;
	std::string m_SongFitText;
	// 常量/有界短串宽度缓存(2026-11-09):TextWidth 走的是全量字形布局
	// (CreateTextContainer + AppendTextContainer + DeleteTextContainer,每次至少
	// 3 次 make_shared),不是轻量测量。灵动岛每帧量 6 个宽度指示串 + 游戏时钟,
	// 在 968fps(1.03ms/帧)下是实打实的浪费——这些文本的字号只在缩放/字号 cvar
	// 变化时才变,内容本身是常量或 %02d:%02d 的有界集合。
	class CTextWidthCacheEntry
	{
	public:
		char m_aText[24] = "";
		float m_Size = -1.0f;
		// TextWidth 内部还会乘一次 tc_font_size/100(见 text.cpp 的 AppendTextContainer)，
		// 所以字号 cvar 也要进键，否则玩家改了全局字号后缓存宽度会失真。
		int m_FontSize = -1;
		float m_Width = -1.0f;
	};
	static constexpr int TEXT_WIDTH_CACHE_MAX = 24;
	CTextWidthCacheEntry m_aTextWidthCache[TEXT_WIDTH_CACHE_MAX];
	int m_TextWidthCacheCount = 0;
	int m_TextWidthCacheNext = 0;
	std::string m_TextWidthCacheFace;
	// vinyl record spin angle (radians)
	float m_VinylAngle = 0.0f;
	// 胶囊宽度的动画值：只有目标宽度变化（拖动宽度滑条、进入/退出空态）时才平滑过渡，
	// 平时歌词换行、时钟走字都不改变宽度。
	float m_AnimatedWidth = -1.0f;
	// 空态等化器的节奏：本地玩家按 hook（右键）的间隔驱动相位推进，
	// 按得越快跳得越急；停手后能量衰减回静止台阶。

	~CImpl()
	{
		ResetLyrics();
		StopCoverDecodeThread();
#if TCLIENT_HAS_WINRT_MUSIC
		StopWorker();
#endif
#if defined(CONF_FAMILY_WINDOWS)
		CloseIslandMap();
#endif
	}

	struct STrack
	{
		bool m_Valid = false;
		std::string m_ServiceId;
		std::string m_Title;
		std::string m_Artist;
		std::string m_Album;
		int64_t m_DurationMs = 0;
		int64_t m_PositionMs = 0;
		bool m_Playing = false;
		// 暂停是"有曲目但没在播"的第三种状态。单独记下来才能把"暂停"和
		// "会话已停止/播放器已退出"分开：SMTC 在播放器退出后仍会挂着上一条
		// 元数据，只看标题非空的话岛会永远停在那首死歌上。
		bool m_Paused = false;
	};

	// ---------------- SMTC worker (Windows) ----------------
#if TCLIENT_HAS_WINRT_MUSIC
	void StoreSnapshot(const STrack &Track)
	{
		std::lock_guard<std::mutex> Lock(m_Mutex);
		m_Latest = Track;
	}

	static STrack QuerySession(const WmSession &Session)
	{
		STrack Out;
		if(!Session)
			return Out;

		try
		{
			Out.m_ServiceId = winrt::to_string(Session.SourceAppUserModelId());
			auto PlaybackInfo = Session.GetPlaybackInfo();
			auto Status = PlaybackInfo.PlaybackStatus();
			Out.m_Playing = Status == WmControl::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
			Out.m_Paused = Status == WmControl::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused;

			auto Timeline = Session.GetTimelineProperties();
			auto ToMs = [](winrt::Windows::Foundation::TimeSpan Span) {
				return std::max<int64_t>(0, Span.count() / 10000);
			};
			Out.m_PositionMs = ToMs(Timeline.Position());
			Out.m_DurationMs = std::max<int64_t>(0, ToMs(Timeline.EndTime()) - ToMs(Timeline.StartTime()));
		}
		catch(...)
		{
			return Out;
		}

		try
		{
			auto MediaProps = Session.TryGetMediaPropertiesAsync().get();
			Out.m_Title = winrt::to_string(MediaProps.Title());
			Out.m_Artist = winrt::to_string(MediaProps.Artist());
			Out.m_Album = winrt::to_string(MediaProps.AlbumTitle());
		}
		catch(...)
		{
		}

		// A session is only worth showing when it actually carries something: it is
		// playing, or it still has a title/artist. A bare SourceAppUserModelId is not
		// enough -- Windows keeps a player's SMTC session listed for a while after
		// the app quits, and treating that as "valid" pinned the island to a dead
		// track forever instead of falling back to the clock-only pill.
		Out.m_Valid = Out.m_Playing || !Out.m_Title.empty() || !Out.m_Artist.empty();
		return Out;
	}

	// Download the album thumbnail PNG for the currently playing session into the
	// shared cover buffers, but only when the track (cover key) has changed. Runs
	// on the SMTC worker thread; the render thread picks the bytes up and decodes
	// them into a texture. Failures leave the previous cover in place.
	static std::string CoverKeyFor(const STrack &Track)
	{
		return Track.m_ServiceId + "\x1f" + Track.m_Title + "\x1f" + Track.m_Album;
	}

	void FetchCoverIfChanged(const WmSession &Session, const STrack &Track)
	{
		if(!Session || Track.m_Title.empty())
		{
			return;
		}
		const std::string Key = CoverKeyFor(Track);
		{
			std::lock_guard<std::mutex> Lock(m_Mutex);
			// A NetEase-scraped cover (key "163|...") is authoritative for this
			// track. Don't let the SMTC thumbnail path clobber m_CoverKey/m_CoverPng
			// (which would wipe the loaded art on the render thread).
			if(m_CoverKey.rfind("163|", 0) == 0)
			{
				// Try to match by title to keep it in sync; either way, skip.
				if(!Track.m_Title.empty() && m_CoverKey == "163|" + Track.m_Title)
					return;
			}
			if(Key == m_CoverKey || Key == m_CoverFailedKey)
			{
				return; // already have a cover, or already failed this exact track
			}
		}
		std::vector<uint8_t> Png;
		try
		{
			auto MediaProps = Session.TryGetMediaPropertiesAsync().get();
			auto ThumbRef = MediaProps.Thumbnail();
			if(ThumbRef)
			{
				auto Stream = ThumbRef.OpenReadAsync().get();
				const uint64_t Size64 = Stream.Size();
				if(Size64 > 0 && Size64 < (16u * 1024u * 1024u))
				{
					const uint32_t Size = (uint32_t)Size64;
					Png.resize(Size);
					winrt::Windows::Storage::Streams::DataReader Reader(Stream);
					const uint32_t Loaded = Reader.LoadAsync(Size).get();
					if(Loaded > 0)
						Reader.ReadBytes(winrt::array_view<uint8_t>(Png.data(), Png.data() + Loaded));
					Png.resize(Loaded);
					// too small to carry a PNG header -- treat as no thumbnail
					if(Loaded < 12)
						Png.clear();
				}
			}
		}
		catch(...)
		{
			Png.clear();
		}
		std::lock_guard<std::mutex> Lock(m_Mutex);
		if(!Png.empty())
		{
			// cheap fingerprint so we can tell whether the thumbnail actually
			// changes from track to track (a static SMTC logo would log the
			// same sum/len for every song, which explains "cover never changes")
			uint32_t Sum = 0;
			for(size_t i = 0; i < Png.size(); i++)
				Sum = Sum * 131u + Png[i];
			m_CoverPng = std::move(Png);
			m_CoverKey = Key;
			m_CoverDirty = true;
		}
		else
		{
			// remember the failure separately so we do not retry this track every
			// poll, but do NOT set m_CoverKey (that would falsely look like a
			// loaded cover and, worse, the 163| guard at the top keys off it).
			m_CoverFailedKey = Key;
		}
	}

	void ProcessMediaCommands(WmSessionManager &Manager)
	{
		std::vector<EMediaCommand> vCommands;
		{
			std::lock_guard<std::mutex> Lock(m_Mutex);
			vCommands.swap(m_vPendingCommands);
		}
		if(vCommands.empty())
			return;

		WmSession Session = nullptr;
		try
		{
			Session = Manager.GetCurrentSession();
		}
		catch(...)
		{
		}
		if(!Session)
			return;

		for(const EMediaCommand Command : vCommands)
		{
			try
			{
				switch(Command)
				{
				case EMediaCommand::TOGGLE_PLAY:
				{
					const auto Status = Session.GetPlaybackInfo().PlaybackStatus();
					if(Status == WmControl::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing)
						Session.TryPauseAsync().get();
					else
						Session.TryPlayAsync().get();
					break;
				}
				case EMediaCommand::NEXT:
					Session.TrySkipNextAsync().get();
					break;
				case EMediaCommand::PREV:
					Session.TrySkipPreviousAsync().get();
					break;
				}
			}
			catch(...)
			{
			}
		}
		// 立刻回写状态，按钮不用等下一次 200ms 轮询才翻转
		StoreSnapshot(QuerySession(Session));
	}

	void PollSessions(WmSessionManager &Manager, std::string &CurrentSessionId)
	{
		std::string SystemSessionId;
		try
		{
			auto Current = Manager.GetCurrentSession();
			SystemSessionId = Current ? winrt::to_string(Current.SourceAppUserModelId()) : std::string();
		}
		catch(...)
		{
		}

		STrack Best;
		WmSession BestSession = nullptr;
		int BestScore = -1;
		try
		{
			auto Sessions = Manager.GetSessions();
			const uint32_t Count = Sessions.Size();
			for(uint32_t i = 0; i < Count; ++i)
			{
				WmSession CandidateSession = Sessions.GetAt(i);
				STrack Candidate = QuerySession(CandidateSession);
				if(!Candidate.m_Valid)
					continue;
				// Media properties sometimes fail to load momentarily (or a paused
				// player reports empty title). Carry the previously known title
				// forward for the same session so lyrics/progress do not flicker --
				// but only for a couple of seconds. Without that deadline a player
				// that has already quit (its SMTC session lingers with no metadata)
				// kept the island pinned to a dead track instead of collapsing back
				// to the clock-only pill.
				if(Candidate.m_Title.empty() && Candidate.m_ServiceId == CurrentSessionId)
				{
					const int64_t Now = time_get();
					if(m_CarrySessionId != Candidate.m_ServiceId)
					{
						m_CarrySessionId = Candidate.m_ServiceId;
						m_CarryDeadline = Now + time_freq() * 2;
					}
					if(Now <= m_CarryDeadline)
					{
						const STrack Previous = TakeSnapshot();
						if(Previous.m_ServiceId == Candidate.m_ServiceId && !Previous.m_Title.empty())
						{
							Candidate.m_Title = Previous.m_Title;
							Candidate.m_Artist = Previous.m_Artist;
							Candidate.m_Album = Previous.m_Album;
							Candidate.m_Valid = true;
						}
					}
				}
				// prefer the playing session, then the system current session, then last tracked
				int Score = 0;
				if(Candidate.m_Playing)
					Score += 20;
				if(Candidate.m_ServiceId == SystemSessionId)
					Score += 5;
				if(Candidate.m_ServiceId == CurrentSessionId)
					Score += 3;
				if(!Candidate.m_Title.empty())
					Score += 2;
				// Deprioritise title-less sessions so a momentarily empty player
				// does not steal the island from a session that has real metadata.
				if(Candidate.m_Title.empty())
					Score -= 10;
				if(Score > BestScore)
				{
					BestScore = Score;
					Best = Candidate;
					BestSession = CandidateSession;
				}
			}
		}
		catch(...)
		{
			Best.m_Valid = false;
		}
		if(Best.m_Valid)
		{
			CurrentSessionId = Best.m_ServiceId;
			try
			{
				FetchCoverIfChanged(BestSession, Best);
			}
			catch(const std::exception &e)
			{
			}
			catch(...)
			{
			}
		}
		StoreSnapshot(Best);
	}

	void WorkerMain()
	{
		try
		{
			winrt::init_apartment(winrt::apartment_type::multi_threaded);
		}
		catch(...)
		{
			return;
		}

		WmSessionManager Manager = nullptr;
		std::string CurrentSessionId;

		while(true)
		{
			{
				std::unique_lock<std::mutex> Lock(m_Mutex);
				m_Cv.wait_for(Lock, std::chrono::milliseconds(200), [this]() { return m_Shutdown; });
				if(m_Shutdown)
					break;
			}
			if(!Manager)
			{
				try
				{
					Manager = WmControl::GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
				}
				catch(...)
				{
					Manager = nullptr;
				}
			}
			if(Manager)
			{
				ProcessMediaCommands(Manager);
				PollSessions(Manager, CurrentSessionId);
			}
		}

		try
		{
			winrt::uninit_apartment();
		}
		catch(...)
		{
		}
	}

	void StartWorker()
	{
		if(m_WorkerThread.joinable())
			return;
		m_WorkerThread = std::thread([this]() { WorkerMain(); });
	}

	void StopWorker()
	{
		if(!m_WorkerThread.joinable())
			return;
		{
			std::lock_guard<std::mutex> Lock(m_Mutex);
			m_Shutdown = true;
		}
		m_Cv.notify_one();
		m_WorkerThread.join();
	}

	std::thread m_WorkerThread;
	bool m_Shutdown = false;

	// Grace period for carrying a session's last known title forward when its
	// media properties momentarily come back empty. Bounded so a player that has
	// already quit cannot keep the island alive on a stale track.
	std::string m_CarrySessionId;
	int64_t m_CarryDeadline = 0;
#endif

	mutable std::mutex m_Mutex;
	std::condition_variable m_Cv;
	STrack m_Latest;

	// ---- 媒体控制命令（主线程 -> SMTC worker）----
	// WinRT 只能在初始化了 apartment 的 worker 线程上调用，所以按钮点击只入队。
	enum class EMediaCommand
	{
		TOGGLE_PLAY,
		NEXT,
		PREV,
	};
	std::vector<EMediaCommand> m_vPendingCommands;

	// Album cover art flows through two stages:
	//
	//   1. Somebody publishes raw image bytes + the "cover key" that identifies the
	//      track they belong to (service id + title + album; NetEase uses "163|title").
	//      The SMTC worker (FetchCoverIfChanged) and the NetEase HTTP callback both
	//      write m_CoverPng/m_CoverKey/m_CoverDirty under m_Mutex.
	//   2. The render thread notices m_CoverDirty and hands the bytes to the cover
	//      decode thread, which does stbi + the circular alpha mask off the render
	//      thread. The render thread only uploads the finished pixels to the GPU.
	//
	// Decoding used to happen inline in UpdateCoverTexture(): a 2048x2048 cover is a
	// 16 MiB RGBA buffer plus ~4M sqrt() calls for the round crop, all inside one
	// frame right at a track change. That is the "换歌瞬间一顿" the users reported.
	std::string m_CoverKey;             // key the publisher is writing for
	std::string m_CoverFailedKey;       // key whose cover fetch already failed
	std::vector<uint8_t> m_CoverPng;    // raw encoded bytes for m_CoverKey
	bool m_CoverDirty = false;          // set by publisher when new bytes arrive

	static constexpr size_t COVER_MAX_BYTES = 16u * 1024u * 1024u;
	static constexpr int COVER_MAX_DIMENSION = 2048;
	static constexpr size_t COVER_MAX_PIXELS = 4u * 1024u * 1024u; // 2048x2048 RGBA：解码/遮罩/上传峰值降 4 倍（换歌卡顿治理 2026-11-08）

	// ---- request slot: render thread -> decode thread ----
	std::vector<uint8_t> m_vCoverDecodeReqPng;
	std::string m_CoverDecodeReqKey;
	bool m_CoverDecodeReqValid = false;   // a request is waiting to be decoded
	std::condition_variable m_CoverDecodeCv;
	std::thread m_CoverDecodeThread;
	bool m_CoverDecodeShutdown = false;

	// ---- result slot: decode thread -> render thread ----
	std::string m_CoverDecodedKey;        // key the finished decode belongs to
	CImageInfo m_CoverDecodedImage;       // RGBA pixels, moved out by the render thread
	bool m_CoverDecodeReady = false;      // a result is waiting (success or failure)
	bool m_CoverDecodeFailed = false;
	std::string m_CoverDecodeReason;      // human-readable outcome, for m_LastTexResult

	// render-thread side texture cache (only ever touched from the render thread)
	std::string m_LoadedCoverKey;       // key currently decoded into m_CoverTexture
	std::string m_CoverPendingKey;      // key we asked the decode thread for
	IGraphics::CTextureHandle m_CoverTexture; // GPU texture for m_LoadedCoverKey
	bool m_CoverTextureValid = false;
	std::string m_LastTexResult = "(none yet)";

	// ---- anti-aliased shape textures ---------------------------------------
	// The engine draws rounded rectangles and circles as hard-edged triangle
	// fans with no alpha coverage, so every arc comes out as a visible
	// staircase. Instead each distinct silhouette is rasterised once into a
	// white RGBA texture whose alpha carries 4x4 supersampled coverage, then
	// blitted as a single tinted quad. That gives real antialiasing on every
	// curve in the island — capsule body, rim hairline, vinyl disc, grooves.
	//
	// Key: width, height, radius*100, corner mask, inner inset (for rings).
	struct SShapeKey
	{
		int m_W, m_H, m_Radius100, m_Corners, m_Inset100, m_Feather100;
		bool operator<(const SShapeKey &O) const
		{
			if(m_W != O.m_W)
				return m_W < O.m_W;
			if(m_H != O.m_H)
				return m_H < O.m_H;
			if(m_Radius100 != O.m_Radius100)
				return m_Radius100 < O.m_Radius100;
			if(m_Corners != O.m_Corners)
				return m_Corners < O.m_Corners;
			if(m_Inset100 != O.m_Inset100)
				return m_Inset100 < O.m_Inset100;
			return m_Feather100 < O.m_Feather100;
		}
	};
	std::map<SShapeKey, IGraphics::CTextureHandle> m_ShapeCache;
	// 投影专用缓存：键里带扩边量，和实体形状缓存分开，避免键空间互相污染。
	std::map<SShapeKey, IGraphics::CTextureHandle> m_ShadowCache;

	static bool ShapeCovers(float Px, float Py, float W, float H, float R, int Corners)
	{
		if(Px < 0.0f || Py < 0.0f || Px > W || Py > H)
			return false;
		if(R <= 0.0f)
			return true;
		const bool Left = Px < R;
		const bool Right = Px > W - R;
		const bool Top = Py < R;
		const bool Bottom = Py > H - R;
		// 端帽纹理的宽恰好等于半径(胶囊端帽 = 宽 H/2 的半圆,半径即 H/2,W == R),
		// 左右圆角区整块重叠:必须按掩码逐角匹配、圆心跟着匹配的角走。
		// 旧的"Left&&Top 抢先"写法在 W==R 时把右端帽(TR/BR)整块判成直角,
		// 烤出来是方块——2026-11-01 用户截图"灵动岛右边圆头没了,左边正常"即此。
		float CX, CY;
		if(Left && Top && (Corners & IGraphics::CORNER_TL))
		{
			CX = R;
			CY = R;
		}
		else if(Right && Top && (Corners & IGraphics::CORNER_TR))
		{
			CX = W - R;
			CY = R;
		}
		else if(Left && Bottom && (Corners & IGraphics::CORNER_BL))
		{
			CX = R;
			CY = H - R;
		}
		else if(Right && Bottom && (Corners & IGraphics::CORNER_BR))
		{
			CX = W - R;
			CY = H - R;
		}
		else
			return true; // 直角区/直边区:矩形范围内即覆盖
		const float DX = Px - CX;
		const float DY = Py - CY;
		return DX * DX + DY * DY <= R * R;
	}

	IGraphics::CTextureHandle EnsureShapeTexture(int W, int H, float Rounding, int Corners, float Inset)
	{
		if(W <= 0 || H <= 0)
			return IGraphics::CTextureHandle();
		// 允许圆角半径达到短边全长：胶囊端部是宽 H/2 的半圆，其半径就是 H/2，
		// 旧的 min(W,H)*0.5 上限会把它压成 H/4，端部看起来不够圆。
		const float R = std::clamp(Rounding, 0.0f, (float)std::min(W, H));
		const SShapeKey Key{W, H, (int)std::lround(R * 100.0f), Corners, (int)std::lround(Inset * 100.0f), 0};
		auto It = m_ShapeCache.find(Key);
		if(It != m_ShapeCache.end() && It->second.IsValid())
			return It->second;

		// Every new track width adds a couple of entries. Bound the cache so a
		// long session cannot accumulate hundreds of textures; a flush just costs
		// one re-rasterisation per shape on the next frame.
		if(m_ShapeCache.size() > 64)
		{
			for(auto &Entry : m_ShapeCache)
			{
				if(Entry.second.IsValid())
					m_pGraphics->UnloadTexture(&Entry.second);
			}
			m_ShapeCache.clear();
		}

		CImageInfo Image;
		Image.m_Width = (size_t)W;
		Image.m_Height = (size_t)H;
		Image.m_Format = CImageInfo::FORMAT_RGBA;
		Image.Allocate();
		if(!Image.m_pData)
			return IGraphics::CTextureHandle();

		// Inner silhouette for ring mode: the same rounded rect shrunk by Inset.
		const bool Ring = Inset > 0.0f;
		const float IW2 = (float)W - Inset * 2.0f;
		const float IH2 = (float)H - Inset * 2.0f;
		const float IR = std::max(0.0f, R - Inset);
		// 8x8 supersampling. Only pixels whose 1 px neighbourhood straddles the
		// silhouette pay for the 64 samples; everything else is filled flat.
		// (升到 64 采样/像素：边缘灰度 64 级，高分屏上圆角肉眼更顺滑，烘焙一次性缓存。)
		const int Samples = 8;
		const float Step = 1.0f / (float)Samples;
		for(int y = 0; y < H; y++)
		{
			for(int x = 0; x < W; x++)
			{
				uint8_t *p = Image.m_pData + ((size_t)y * (size_t)W + (size_t)x) * 4;
				const float Cx = (float)x + 0.5f;
				const float Cy = (float)y + 0.5f;
				const bool OuterIn = ShapeCovers(Cx - 1.0f, Cy - 1.0f, (float)W, (float)H, R, Corners) &&
						     ShapeCovers(Cx + 1.0f, Cy - 1.0f, (float)W, (float)H, R, Corners) &&
						     ShapeCovers(Cx - 1.0f, Cy + 1.0f, (float)W, (float)H, R, Corners) &&
						     ShapeCovers(Cx + 1.0f, Cy + 1.0f, (float)W, (float)H, R, Corners);
				const bool InnerOut = !Ring ||
						      (!ShapeCovers(Cx - 1.0f - Inset, Cy - 1.0f - Inset, IW2, IH2, IR, Corners) &&
							      !ShapeCovers(Cx + 1.0f - Inset, Cy - 1.0f - Inset, IW2, IH2, IR, Corners) &&
							      !ShapeCovers(Cx - 1.0f - Inset, Cy + 1.0f - Inset, IW2, IH2, IR, Corners) &&
							      !ShapeCovers(Cx + 1.0f - Inset, Cy + 1.0f - Inset, IW2, IH2, IR, Corners));
				if(OuterIn && InnerOut)
				{
					p[0] = p[1] = p[2] = 255;
					p[3] = 255;
					continue;
				}
				const bool AnyOuter = ShapeCovers(Cx - 0.5f, Cy - 0.5f, (float)W, (float)H, R, Corners) ||
						      ShapeCovers(Cx + 0.5f, Cy - 0.5f, (float)W, (float)H, R, Corners) ||
						      ShapeCovers(Cx - 0.5f, Cy + 0.5f, (float)W, (float)H, R, Corners) ||
						      ShapeCovers(Cx + 0.5f, Cy + 0.5f, (float)W, (float)H, R, Corners);
				if(!AnyOuter)
				{
					p[0] = p[1] = p[2] = p[3] = 0;
					continue;
				}
				int Hits = 0;
				for(int sy = 0; sy < Samples; sy++)
				{
					for(int sx = 0; sx < Samples; sx++)
					{
						const float Px = (float)x + ((float)sx + 0.5f) * Step;
						const float Py = (float)y + ((float)sy + 0.5f) * Step;
						if(!ShapeCovers(Px, Py, (float)W, (float)H, R, Corners))
							continue;
						if(Ring && ShapeCovers(Px - Inset, Py - Inset, IW2, IH2, IR, Corners))
							continue;
						Hits++;
					}
				}
				p[0] = p[1] = p[2] = 255;
				p[3] = (uint8_t)std::lround((float)Hits / (float)(Samples * Samples) * 255.0f);
			}
		}

		const IGraphics::CTextureHandle Tex = m_pGraphics->LoadTextureRawMove(Image, 0, "island-shape");
		if(Tex.IsValid())
			m_ShapeCache[Key] = Tex;
		return Tex;
	}

	// Blit a cached shape. Position is snapped to whole pixels so the baked
	// coverage lands 1:1 on the framebuffer instead of being resampled.
	void DrawShape(float X, float Y, float W, float H, float Rounding, int Corners, ColorRGBA Color, float Inset = 0.0f)
	{
		const int IW = std::max(1, (int)std::lround(W));
		const int IH = std::max(1, (int)std::lround(H));
		// 2x 烘焙 + 1:1 显示：烘焙纹理是显示尺寸的两倍（几何全部 ×2），采样时双线性
		// 下采样 —— 边缘 AA 等效再翻倍，高分屏上圆角/直边更顺滑（§188 用户反馈续）。
		const IGraphics::CTextureHandle Tex = EnsureShapeTexture(IW * 2, IH * 2, Rounding * 2.0f, Corners, Inset * 2.0f);
		if(!Tex.IsValid())
			return;
		m_pGraphics->TextureSet(Tex);
		m_pGraphics->QuadsBegin();
		m_pGraphics->SetColor(Color);
		m_pGraphics->QuadsSetSubset(0.0f, 0.0f, 1.0f, 1.0f);
		const IGraphics::CQuadItem Quad(std::lround(X), std::lround(Y), (float)IW, (float)IH);
		m_pGraphics->QuadsDrawTL(&Quad, 1);
		m_pGraphics->QuadsEnd();
		m_pGraphics->TextureClear();
	}

	// Same silhouette blit with four independent corner colors, which is how the
	// capsule gets a gradient body. The baked texture is white RGB + coverage
	// alpha, so vertex color modulates it exactly like SetColor does.
	void DrawShape4(float X, float Y, float W, float H, float Rounding, int Corners,
		const ColorRGBA &TL, const ColorRGBA &TR, const ColorRGBA &BL, const ColorRGBA &BR)
	{
		const int IW = std::max(1, (int)std::lround(W));
		const int IH = std::max(1, (int)std::lround(H));
		const IGraphics::CTextureHandle Tex = EnsureShapeTexture(IW * 2, IH * 2, Rounding * 2.0f, Corners, 0.0f);
		if(!Tex.IsValid())
			return;
		m_pGraphics->TextureSet(Tex);
		m_pGraphics->QuadsBegin();
		m_pGraphics->SetColor4(TL, TR, BL, BR);
		m_pGraphics->QuadsSetSubset(0.0f, 0.0f, 1.0f, 1.0f);
		const IGraphics::CQuadItem Quad(std::lround(X), std::lround(Y), (float)IW, (float)IH);
		m_pGraphics->QuadsDrawTL(&Quad, 1);
		m_pGraphics->QuadsEnd();
		m_pGraphics->TextureClear();
	}

	// 胶囊底色渐变。归一化基准是胶囊外框（而不是正在画的那一块），这样端帽和
	// 中间矩形各自取四角颜色时都落在同一个线性函数上，拼接处严格连续。
	struct SGradient
	{
		int m_Mode = 0; // 0=纯色 1=纵向 2=对角 3=横向
		ColorRGBA m_Start = ColorRGBA(0.0f, 0.0f, 0.0f, 1.0f);
		ColorRGBA m_End = ColorRGBA(0.0f, 0.0f, 0.0f, 1.0f);
		float m_X = 0.0f, m_Y = 0.0f, m_W = 1.0f, m_H = 1.0f;

		ColorRGBA At(float Px, float Py) const
		{
			if(m_Mode == 0)
				return m_Start;
			const float U = std::clamp(m_W > 0.0f ? (Px - m_X) / m_W : 0.0f, 0.0f, 1.0f);
			const float V = std::clamp(m_H > 0.0f ? (Py - m_Y) / m_H : 0.0f, 0.0f, 1.0f);
			const float T = m_Mode == 1 ? V : m_Mode == 2 ? (U + V) * 0.5f : U;
			return ColorRGBA(
				m_Start.r + (m_End.r - m_Start.r) * T,
				m_Start.g + (m_End.g - m_Start.g) * T,
				m_Start.b + (m_End.b - m_Start.b) * T,
				m_Start.a + (m_End.a - m_Start.a) * T);
		}
	};

	// 投影纹理：把胶囊剪影光栅化成一层覆盖率遮罩，再做三遍盒式模糊（≈ 高斯），
	// 得到向外渐隐的柔和边。模糊本身提供了过渡的灰度，所以这里按 1x 烘焙——
	// 纹理尺寸直接等于显示尺寸，比实体形状的 2x 烘焙省四倍显存。
	IGraphics::CTextureHandle EnsureShadowTexture(int W, int H, float Rounding, int Corners, float Blur)
	{
		if(W <= 0 || H <= 0 || Blur <= 0.5f)
			return IGraphics::CTextureHandle();
		const int Pad = (int)std::ceil(Blur * 2.0f);
		const int TW = W + Pad * 2;
		const int TH = H + Pad * 2;
		const SShapeKey Key{TW, TH, (int)std::lround(Rounding * 100.0f), Corners, Pad, (int)std::lround(Blur * 100.0f)};
		auto It = m_ShadowCache.find(Key);
		if(It != m_ShadowCache.end() && It->second.IsValid())
			return It->second;

		if(m_ShadowCache.size() > 8)
		{
			for(auto &Entry : m_ShadowCache)
			{
				if(Entry.second.IsValid())
					m_pGraphics->UnloadTexture(&Entry.second);
			}
			m_ShadowCache.clear();
		}

		const float R = std::clamp(Rounding, 0.0f, (float)std::min(W, H));
		std::vector<uint8_t> Mask((size_t)TW * (size_t)TH, 0);
		std::vector<uint8_t> Tmp(Mask.size(), 0);
		// 光栅化：中心采样即可，锯齿会被后面的模糊完全抹掉。
		for(int y = 0; y < TH; y++)
		{
			for(int x = 0; x < TW; x++)
			{
				if(ShapeCovers((float)x - (float)Pad + 0.5f, (float)y - (float)Pad + 0.5f, (float)W, (float)H, R, Corners))
					Mask[(size_t)y * (size_t)TW + (size_t)x] = 255;
			}
		}

		const int Radius = std::max(1, (int)std::lround(Blur * 0.6f));
		auto BoxBlur = [](const std::vector<uint8_t> &Src, std::vector<uint8_t> &Dst, int W_, int H_, int Rad, bool Horizontal) {
			const int Lines = Horizontal ? H_ : W_;
			const int Len = Horizontal ? W_ : H_;
			std::vector<int> Pref((size_t)Len + 1);
			for(int Line = 0; Line < Lines; Line++)
			{
				Pref[0] = 0;
				for(int i = 0; i < Len; i++)
				{
					const size_t Idx = Horizontal ? (size_t)Line * (size_t)W_ + (size_t)i : (size_t)i * (size_t)W_ + (size_t)Line;
					Pref[(size_t)i + 1] = Pref[(size_t)i] + (int)Src[Idx];
				}
				for(int i = 0; i < Len; i++)
				{
					const int A = std::max(0, i - Rad);
					const int B = std::min(Len - 1, i + Rad);
					const int Sum = Pref[(size_t)B + 1] - Pref[(size_t)A];
					const size_t Idx = Horizontal ? (size_t)Line * (size_t)W_ + (size_t)i : (size_t)i * (size_t)W_ + (size_t)Line;
					Dst[Idx] = (uint8_t)((Sum + (B - A + 1) / 2) / (B - A + 1));
				}
			}
		};
		for(int Pass = 0; Pass < 3; Pass++)
		{
			BoxBlur(Mask, Tmp, TW, TH, Radius, true);
			BoxBlur(Tmp, Mask, TW, TH, Radius, false);
		}

		CImageInfo Image;
		Image.m_Width = (size_t)TW;
		Image.m_Height = (size_t)TH;
		Image.m_Format = CImageInfo::FORMAT_RGBA;
		Image.Allocate();
		if(!Image.m_pData)
			return IGraphics::CTextureHandle();
		for(size_t i = 0; i < Mask.size(); i++)
		{
			uint8_t *p = Image.m_pData + i * 4;
			p[0] = p[1] = p[2] = 0; // 纯黑影，颜色交给 SetColor 的 alpha 调制
			p[3] = Mask[i];
		}

		const IGraphics::CTextureHandle Tex = m_pGraphics->LoadTextureRawMove(Image, 0, "island-shadow");
		if(Tex.IsValid())
			m_ShadowCache[Key] = Tex;
		return Tex;
	}

	void DrawShadow(float X, float Y, float W, float H, float Rounding, int Corners, float Blur, ColorRGBA Color)
	{
		const int IW = std::max(1, (int)std::lround(W));
		const int IH = std::max(1, (int)std::lround(H));
		const int Pad = (int)std::ceil(Blur * 2.0f);
		const IGraphics::CTextureHandle Tex = EnsureShadowTexture(IW, IH, Rounding, Corners, Blur);
		if(!Tex.IsValid())
			return;
		m_pGraphics->TextureSet(Tex);
		m_pGraphics->QuadsBegin();
		m_pGraphics->SetColor(Color);
		m_pGraphics->QuadsSetSubset(0.0f, 0.0f, 1.0f, 1.0f);
		const IGraphics::CQuadItem Quad((float)std::lround(X) - (float)Pad, (float)std::lround(Y) - (float)Pad,
			(float)(IW + Pad * 2), (float)(IH + Pad * 2));
		m_pGraphics->QuadsDrawTL(&Quad, 1);
		m_pGraphics->QuadsEnd();
		m_pGraphics->TextureClear();
	}

	// 胶囊 = 左右两个固定尺寸的端帽 + 中间一段实心矩形。
	// 旧实现把整块胶囊烘焙成一张纹理，而纹理键里带宽度；歌词每换一行宽度就变，
	// 于是每行都要重新做一次 2x 超采样的大图烘焙（1080p 下约 1100x150），
	// 这就是切歌词时掉帧的根源。端帽尺寸只跟高度有关，宽度变化不再触发烘焙。
	void DrawPill(float X, float Y, float W, float H, int Corners, ColorRGBA Color)
	{
		const float CapW = H * 0.5f;
		if(W <= CapW * 2.0f + 0.5f)
		{
			DrawShape(X, Y, W, H, H * 0.5f, Corners, Color);
			return;
		}
		const int LeftCorners = Corners & (IGraphics::CORNER_TL | IGraphics::CORNER_BL);
		const int RightCorners = Corners & (IGraphics::CORNER_TR | IGraphics::CORNER_BR);
		// DrawShape 会把位置和尺寸取整到整像素，中间矩形必须用同一套取整后的边界，
		// 否则缩放不是整数时两者错开半像素，接缝处会露出一条细缝。
		const int LeftX = (int)std::lround(X);
		const int CapWi = (int)std::lround(CapW);
		const int RightX = (int)std::lround(X + W - CapW);
		DrawShape(X, Y, CapW, H, H * 0.5f, LeftCorners, Color);
		DrawShape(X + W - CapW, Y, CapW, H, H * 0.5f, RightCorners, Color);
		const float FillX = (float)(LeftX + CapWi);
		const float FillW = (float)(RightX - (LeftX + CapWi));
		if(FillW > 0.0f)
			m_pGraphics->DrawRect(FillX, (float)std::lround(Y), FillW, (float)std::lround(H), Color, IGraphics::CORNER_NONE, 0.0f);
	}

	// 渐变版胶囊：端帽和中间矩形各自取四角颜色，四角都来自同一个线性函数，
	// 因此三块拼起来看不出接缝（分段线性在共享边上取值一致）。
	void DrawPillGrad(float X, float Y, float W, float H, int Corners, const SGradient &Grad)
	{
		const float CapW = H * 0.5f;
		if(W <= CapW * 2.0f + 0.5f)
		{
			DrawShape4(X, Y, W, H, H * 0.5f, Corners,
				Grad.At(X, Y), Grad.At(X + W, Y), Grad.At(X, Y + H), Grad.At(X + W, Y + H));
			return;
		}
		const int LeftCorners = Corners & (IGraphics::CORNER_TL | IGraphics::CORNER_BL);
		const int RightCorners = Corners & (IGraphics::CORNER_TR | IGraphics::CORNER_BR);
		const int LeftX = (int)std::lround(X);
		const int CapWi = (int)std::lround(CapW);
		const int RightX = (int)std::lround(X + W - CapW);
		DrawShape4(X, Y, CapW, H, H * 0.5f, LeftCorners,
			Grad.At(X, Y), Grad.At(X + CapW, Y), Grad.At(X, Y + H), Grad.At(X + CapW, Y + H));
		DrawShape4(X + W - CapW, Y, CapW, H, H * 0.5f, RightCorners,
			Grad.At(X + W - CapW, Y), Grad.At(X + W, Y), Grad.At(X + W - CapW, Y + H), Grad.At(X + W, Y + H));
		const float FillX = (float)(LeftX + CapWi);
		const float FillW = (float)(RightX - (LeftX + CapWi));
		if(FillW > 0.0f)
		{
			const float FillY = (float)std::lround(Y);
			const float FillH = (float)std::lround(H);
			m_pGraphics->DrawRect4(FillX, FillY, FillW, FillH,
				Grad.At(FillX, FillY), Grad.At(FillX + FillW, FillY),
				Grad.At(FillX, FillY + FillH), Grad.At(FillX + FillW, FillY + FillH),
				IGraphics::CORNER_NONE, 0.0f);
		}
	}

	// 从配置构造胶囊渐变。Grad 的归一化基准是整条胶囊，端帽和中间矩形共用，
	// 因此三块拼接处取值一致、不会出现色阶。
	static SGradient MakeGradient(float X, float Y, float W, float H)
	{
		SGradient Grad;
		Grad.m_Mode = std::clamp(g_Config.m_TcIslandGradient, 0, 3);
		Grad.m_Start = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_TcIslandColorTop));
		Grad.m_End = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_TcIslandColorBottom));
		Grad.m_X = X;
		Grad.m_Y = Y;
		Grad.m_W = W;
		Grad.m_H = H;
		return Grad;
	}

	// 强调色：卡拉 OK 已唱部分、进度轨、等化器。alpha 由调用点决定。
	static ColorRGBA AccentColor(float Alpha)
	{
		ColorRGBA Col = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_TcIslandColorAccent));
		Col.a = Alpha;
		return Col;
	}

	// Rounded panel with a 1 px light rim. The rim is a full-size tinted blit and
	// the body is drawn 1 px smaller on top, so the outermost pixel stays visible
	// as a hairline that follows the antialiased silhouette.
	void DrawPanel(float X, float Y, float W, float H, float Rounding, int Corners, bool Rim, const SGradient &Grad)
	{
		// 投影先在胶囊和游戏画面之间铺一段渐变。纯黑不透明体直接压在高饱和的
		// 游戏画面上时，交界只有一条硬边，看起来像贴上去的贴纸；投影给了它
		// 悬浮的层次，边缘就不再"突兀"。偏移 +Y 假定光从上方来。
		const float Strength = std::clamp((float)g_Config.m_TcIslandShadow / 100.0f, 0.0f, 1.0f);
		if(Strength > 0.0f)
		{
			const float Blur = std::max(2.0f, H * 0.10f);
			DrawShadow(X, Y + Blur * 0.30f, W, H, Rounding, Corners, Blur,
				ColorRGBA(0.0f, 0.0f, 0.0f, 0.62f * Strength));
		}
		if(Rim)
		{
			DrawPill(X, Y, W, H, Corners, ColorRGBA(1.0f, 1.0f, 1.0f, 0.07f));
			DrawPillGrad(X + 1.0f, Y + 1.0f, W - 2.0f, H - 2.0f, Corners, Grad);
		}
		else
		{
			DrawPillGrad(X, Y, W, H, Corners, Grad);
		}
	}

	bool IsPlayingCached() const
	{
		std::lock_guard<std::mutex> Lock(m_Mutex);
		return m_Latest.m_Valid && m_Latest.m_Playing;
	}

	void GetTrackCached(char *pTitle, int TitleSize, char *pArtist, int ArtistSize) const
	{
		std::lock_guard<std::mutex> Lock(m_Mutex);
		if(!m_Latest.m_Valid)
			return;
		str_copy(pTitle, m_Latest.m_Title.c_str(), TitleSize);
		str_copy(pArtist, m_Latest.m_Artist.c_str(), ArtistSize);
	}

	bool HasSessionCached() const
	{
		std::lock_guard<std::mutex> Lock(m_Mutex);
		return m_Latest.m_Valid;
	}

	STrack TakeSnapshot()
	{
		std::lock_guard<std::mutex> Lock(m_Mutex);
		return m_Latest;
	}

	// ---------------- cloudmusic window-title monitor (Windows) ----------------
#if defined(CONF_FAMILY_WINDOWS)
#pragma pack(push, 8)
	struct SIslandShm
	{
		uint32_t Magic;
		uint32_t Version;
		uint32_t Playing;
		int64_t ElapsedMs;
		double LastUpdate;
		wchar_t aTitle[256];
		float Peak;
		uint32_t Meters;
		double Fraction;   // netease LS playPostion 0..1，seek 后的真实进度
		double FractionAt; // 该进度采集时刻（epoch 秒）
		uint32_t Paused;   // netease LS pauseStatus
		double DurationMs; // 从 LS 拿到的曲长（毫秒），0=未知
		double AudioMs;    // WASAPI 进程环回的已渲染音频毫秒（watcher 失效时的位置兜底，静音段会慢）
		uint32_t AudioActive; // 最近 300ms 有音频帧 = 1（最可靠的播放判定）
		double ClientDurationMs; // 【本客户端写】网易云 API resolve 的当前歌曲长
		double WatcherMs;    // 【v7】netease-watcher 的精确播放毫秒＝位置本体（-1=失效）
		double WatcherDurMs; // 【v7】netease-watcher 的曲长毫秒（-1=未知）
	};
#pragma pack(pop)
	static constexpr uint32_t ISLAND_SHM_MAGIC = 0x4D53494E;
	static constexpr uint32_t ISLAND_SHM_VERSION = 7;

	HANDLE m_hIslandMap = nullptr;
	void *m_pIslandView = nullptr;
	int64_t m_NextShmTry = 0;
	int64_t m_NextMonitorTry = 0;

	void CloseIslandMap()
	{
		if(m_pIslandView)
		{
			UnmapViewOfFile(m_pIslandView);
			m_pIslandView = nullptr;
		}
		if(m_hIslandMap)
		{
			CloseHandle(m_hIslandMap);
			m_hIslandMap = nullptr;
		}
	}

	void OpenIslandMap()
	{
		// v6 起需要写权限：回传 Meta 曲长（ClientDurationMs）给 monitor 做 seek 重锚换算
		HANDLE hMap = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, L"Local\\TClientIslandMedia");
		if(!hMap)
			return;
		void *pView = MapViewOfFile(hMap, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(SIslandShm));
		if(!pView)
		{
			CloseHandle(hMap);
			return;
		}
		CloseIslandMap();
		m_hIslandMap = hMap;
		m_pIslandView = pView;
	}

	void StartMonitorProcess()
	{
		wchar_t aClientPath[MAX_PATH];
		if(!GetModuleFileNameW(nullptr, aClientPath, MAX_PATH))
			return;
		wchar_t *pSlash = wcsrchr(aClientPath, L'\\');
		if(!pSlash)
			return;
		*pSlash = L'\0';
		wchar_t aMonitorPath[MAX_PATH + 48];
		_snwprintf(aMonitorPath, std::size(aMonitorPath), L"%s\\island_media_monitor.exe", aClientPath);
		aMonitorPath[std::size(aMonitorPath) - 1] = L'\0';

		// 拉起进程移出渲染线程：CreateProcessW 可能被杀软拦截阻塞数十毫秒，
		// 在渲染线程上会变成周期性单帧卡顿（2026-11-08 卡顿调查）。
		std::thread(
			[MonitorPath = std::wstring(aMonitorPath)]() {
				STARTUPINFOW Si;
				memset(&Si, 0, sizeof(Si));
				Si.cb = sizeof(Si);
				PROCESS_INFORMATION Pi;
				memset(&Pi, 0, sizeof(Pi));
				if(!CreateProcessW(MonitorPath.c_str(), nullptr, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &Si, &Pi))
					return;
				CloseHandle(Pi.hThread);
				CloseHandle(Pi.hProcess);
			})
			.detach();
	}

	// launches / refreshes the helper and keeps the mapping open
	void EnsureMonitorRunning()
	{
		const int64_t NowTick = time_get();
		if(m_NextShmTry != 0 && NowTick < m_NextShmTry)
			return;
		m_NextShmTry = NowTick + time_freq() / 2;

		if(!m_pIslandView)
		{
			OpenIslandMap();
			if(!m_pIslandView)
			{
				// 映射不存在 = 监控进程自上次开机/更新器清理后从未起来。之前只在
				// "映射存在但停更"时才补拉,而映射恰恰由监控进程自己创建——游戏重启
				// 或重启机器后两边互相等,音乐显示从此消失(2026-11-01 用户实测)。
				// 这里补上首次拉起,与下面的死进程补拉共用 5 秒节流。
				if(m_NextMonitorTry != 0 && NowTick < m_NextMonitorTry)
					return;
				m_NextMonitorTry = NowTick + time_freq() * 5;
				StartMonitorProcess();
				return;
			}
		}

		SIslandShm Snap;
		memcpy(&Snap, m_pIslandView, sizeof(Snap));
		if(Snap.Magic != ISLAND_SHM_MAGIC || Snap.Version != ISLAND_SHM_VERSION)
		{
			// stale/broken mapping, remap next round
			CloseIslandMap();
			return;
		}

		const double NowEpoch = (double)std::time(nullptr);
		if(NowEpoch - Snap.LastUpdate < 5.0)
			return; // monitor alive and writing

		// dead: respawn at most once every 5 s
		if(m_NextMonitorTry != 0 && NowTick < m_NextMonitorTry)
			return;
		m_NextMonitorTry = NowTick + time_freq() * 5;
		CloseIslandMap();
		StartMonitorProcess();
	}

	STrack MonitorTrack()
	{
		STrack Out;
		if(!m_pIslandView)
			return Out;

		const int64_t NowTick = time_get();
		if(m_NextMonitorPoll != 0 && NowTick < m_NextMonitorPoll)
			return m_LatestMonitor; // reuse cached track between polls
		m_NextMonitorPoll = NowTick + time_freq() / 4;

		SIslandShm Snap;
		memcpy(&Snap, m_pIslandView, sizeof(Snap));
		if(Snap.Magic != ISLAND_SHM_MAGIC || Snap.Version != ISLAND_SHM_VERSION || !Snap.aTitle[0])
		{
			m_LatestMonitor = Out;
			return Out;
		}

		const double NowEpoch = (double)std::time(nullptr);
		if(NowEpoch - Snap.LastUpdate > 5.0)
		{
			m_LatestMonitor = Out;
			return Out;
		}

		// title is wchar UTF-16, convert to UTF-8
		char aTitleUtf8[1024];
		const int Converted = WideCharToMultiByte(CP_UTF8, 0, Snap.aTitle, -1, aTitleUtf8, (int)sizeof(aTitleUtf8), nullptr, nullptr);
		if(Converted <= 1)
		{
			m_LatestMonitor = Out;
			return Out;
		}

		std::string Raw(aTitleUtf8);
		// "song(with x) - artist": split at the last separator
		const size_t Sep = Raw.rfind(" - ");
		if(Sep == std::string::npos)
		{
			m_LatestMonitor = Out;
			return Out;
		}

		Out.m_ServiceId = "netease-title";
		Out.m_Title = Raw.substr(0, Sep);
		Out.m_Artist = Raw.substr(Sep + 3);
		// LS pauseStatus 明确暂停时强制暂停：双保险，monitor 旧版未重启或电表误判时兜底。
		Out.m_Playing = Snap.Playing != 0 && Snap.Paused == 0;
		Out.m_Paused = Snap.Paused != 0;

		// 位置来源（v7.1）：
		//  1) watcher（WatcherMs，最权威）：netease-watcher hook 的网易云内部真实位置，
		//     含静音段、seek 立即反映、暂停冻结 —— 歌词/进度条的位置本体。
		//  2) LS 锚点（Fraction，v7.1 提前）：网易云 LocalServer 自己报的播放进度,
		//     每秒刷新,seek/静音段都对。实测 watcher 失效时音频钟会差到分钟级
		//     （2026-11-01:同一时刻 Fraction 算出 140s,AudioMs 只有 91s）,把
		//     AudioMs 放在它前面等于给歌词同步喂错位置 —— 那就是用户报的
		//     "歌词有延迟"的根源。LS 只在超 15 秒没刷新时才让位。
		//  3) 音频钟（AudioMs）：LS 也没有时的最后兜底。
		const double NowEpochPos = (double)std::time(nullptr);
		const double AnchorAge = NowEpochPos - Snap.FractionAt;
		const bool HaveLsAnchor = Snap.DurationMs > 0.0 && Snap.Fraction > 0.0 && AnchorAge >= -1.0 && AnchorAge < 15.0;
		if(Snap.WatcherMs > 0.0)
		{
			Out.m_DurationMs = Snap.WatcherDurMs > 0.0 ? (int64_t)Snap.WatcherDurMs :
				(Snap.DurationMs > 0.0 ? (int64_t)Snap.DurationMs : 0);
			int64_t Pos = (int64_t)Snap.WatcherMs;
			if(Snap.DurationMs > 0.0 && Pos > (int64_t)Snap.DurationMs + 1500)
				Pos = (int64_t)Snap.DurationMs;
			Out.m_PositionMs = std::max<int64_t>(0, Pos);
		}
		else if(HaveLsAnchor)
		{
			Out.m_DurationMs = (int64_t)Snap.DurationMs;
			int64_t Pos = (int64_t)(Snap.Fraction * Snap.DurationMs);
			if(Out.m_Playing && AnchorAge > 0.0)
				Pos += (int64_t)(AnchorAge * 1000.0);
			Out.m_PositionMs = std::max<int64_t>(0, Pos);
		}
		else if(Snap.AudioMs > 0.0 || Snap.AudioActive)
		{
			Out.m_DurationMs = Snap.DurationMs > 0.0 ? (int64_t)Snap.DurationMs : 0;
			int64_t Pos = (int64_t)Snap.AudioMs;
			// monitor 50ms 一写，写入间隙用墙钟外推（仅活跃时）
			if(Snap.AudioActive && Snap.LastUpdate > 0.0 && NowEpochPos > Snap.LastUpdate)
				Pos += (int64_t)((NowEpochPos - Snap.LastUpdate) * 1000.0);
			if(Snap.DurationMs > 0.0 && Pos > (int64_t)Snap.DurationMs + 1500)
				Pos = (int64_t)Snap.DurationMs;
			Out.m_PositionMs = std::max<int64_t>(0, Pos);
		}
		else
		{
			Out.m_DurationMs = Snap.DurationMs > 0.0 ? (int64_t)Snap.DurationMs : 0;
			Out.m_PositionMs = std::max<int64_t>(0, Snap.ElapsedMs);
		}
		Out.m_Valid = !Out.m_Title.empty() || !Out.m_Artist.empty();

		m_LatestMonitor = Out;
		return Out;
	}

	STrack m_LatestMonitor;
	int64_t m_NextMonitorPoll = 0;
#endif

	// ---------------- lyrics ----------------
	static constexpr size_t LYRICS_CACHE_MAX = 12;
	enum class ELyricsState
	{
		IDLE,
		LOADING,
		READY,
		NOTFOUND,
		OFFLINE,
	};

	struct SCacheEntry
	{
		ELyricsState m_State = ELyricsState::NOTFOUND;
		std::vector<SLyricLine> m_vLines;
	};

	static constexpr int64_t LYRICS_MAX_BYTES = 4 * 1024 * 1024;

	// Own the response and parse into job-local data, never into CImpl. A track
	// change can drop an aborted job while the pool finishes it safely. Only
	// STATE_DONE publishes m_Result; Done() also accepts jobs still aborting.
	template<typename TResult>
	class CResponseJob final : public IJob
	{
		using FParse = void (*)(const std::shared_ptr<IHttpRequest> &, TResult &);
		const std::shared_ptr<IHttpRequest> m_pRequest;
		const FParse m_pfnParse;

		void Run() override
		{
			if(State() != STATE_ABORTED)
				m_pfnParse(m_pRequest, m_Result);
		}

	public:
		TResult m_Result;

		CResponseJob(std::shared_ptr<IHttpRequest> pRequest, FParse pfnParse) :
			m_pRequest(std::move(pRequest)),
			m_pfnParse(pfnParse)
		{
			Abortable(true);
		}
	};

	static std::string BuildCacheKey(const STrack &Track)
	{
		// NOTE: duration is intentionally NOT part of the key. SMTC reports a
		// jittery (often 0 -> real) duration in the first seconds, which would
		// otherwise change the key every frame and abort/restart the request
		// forever (result: title shows but lyrics never load). Duration is only
		// used to rank the search result, not to identify the track.
		std::string Key;
		Key.reserve(128);
		Key += "v2|";
		Key += Track.m_Artist;
		Key += '|';
		Key += Track.m_Title;
		Key += '|';
		Key += Track.m_Album;
		return Key;
	}




	// media clock
	int64_t m_ClockPositionMs = 0;
	// 播放状态去抖：部分播放器的 SMTC Status 会在 Playing/Paused/Stopped 之间
	// 反复抖，一次抖动就翻转整条岛的布局（宽度、内容全换），看起来像"卡"。
	// 要求候选状态连续稳定一小段时间才真正切换。
	bool m_IdleState = false;
	bool m_IdleStateCandidate = false;
	double m_IdleCandidateSince = 0.0;

	int64_t m_ClockTick = 0;
	int64_t m_ClockDurationMs = 0;
	bool m_ClockPlaying = false;
	// Last raw position snapshot we saw, used to detect a "frozen" position
	// (some SMTC providers report a constant 0/11 ms and never advance).
	int64_t m_LastSnapshotMs = -1;
	bool m_SnapshotFrozen = false;

	int64_t CurrentPositionMs() const
	{
		int64_t Position = std::max<int64_t>(0, m_ClockPositionMs);
		if(m_ClockPlaying && m_ClockTick != 0)
			Position += ((time_get() - m_ClockTick) * 1000) / time_freq();
		if(m_ClockDurationMs > 0)
			Position = std::min(Position, m_ClockDurationMs);
		return Position;
	}

	void ResetMediaClock()
	{
		m_ClockPositionMs = 0;
		m_ClockTick = 0;
		m_ClockDurationMs = 0;
		m_ClockPlaying = false;
		m_LastSnapshotMs = -1;
		m_SnapshotFrozen = false;
	}

	void SyncMediaClock(int64_t SnapshotPositionMs, int64_t DurationMs, bool Playing)
	{
		const int64_t Snapshot = std::max<int64_t>(0, SnapshotPositionMs);
		const int64_t Now = time_get();

		// The NetEase desktop client almost never reports a real duration/position
		// over SMTC: Duration is 0 and the only "position" the monitor can offer is
		// a wall-clock counter (ElapsedMs) that starts when the *monitor process*
		// started -- it never resets on track change or seek, so it is a garbage
		// absolute value entirely unrelated to the playhead. Verified empirically by
		// dumping the shared-memory struct: frac==0, dur==0, elapsed only ever grows.
		//
		// Therefore: only trust an absolute position snapshot when the provider
		// actually gave us a real duration (DurationMs>0). In that case it is a
		// genuine SMTC/real progress and we anchor to it. When DurationMs==0 the
		// absolute position is unusable -- we ignore it entirely and let the wall
		// clock drive the position from wherever the clock was last reset to
		// (0 on track change, via ResetMediaClock()). This is the "self-timer"
		// approach: the game becomes its own playback clock, re-anchored on every
		// track switch, and never polluted by the monitor's fake cumulative elapsed.
		const int64_t kMaxReasonablePosMs = 6LL * 3600LL * 1000LL; // 6h
		const bool SnapshotBogus =
			Snapshot > kMaxReasonablePosMs ||
			(DurationMs > 0 && Snapshot > DurationMs + 10000);

		// Position-absolute snapshots are only trustworthy with a real duration.
		const bool TrustPosition = (DurationMs > 0) && !SnapshotBogus;

		// Detect a frozen position provider: some SMTC sources report a constant
		// value and never advance. When the snapshot is genuinely frozen we let the
		// wall clock drive the position; otherwise the snapshot itself is the truth.
		const bool SnapshotChanged = Snapshot != m_LastSnapshotMs;
		m_LastSnapshotMs = Snapshot;

		const bool FirstSync = m_ClockTick == 0;
		const bool StateChanged = Playing != m_ClockPlaying;

		if(TrustPosition)
		{
			// Real progress available: adopt it and restart the interpolation window.
			// 重锚条件(2026-11-01 用户反馈"歌词渐变变闪了"):原先"值一变就重锚"——
			// 监控约每秒轮一次 watcher,轮询间隔一抖,新锚点就落在插值位置后面,
			// 位置每秒往回跳一截,卡拉OK一秒一闪。改为:快照变化**且**与当前插值
			// 偏差 >300ms 才重锚 —— 小抖动由插值自然吸收,真 seek/切源立即跟上;
			// 快照值没变(源冻结)则维持旧锚点让墙钟自由跑,与原行为一致。
			const int64_t Interp = CurrentPositionMs();
			const int64_t Drift = Snapshot > Interp ? Snapshot - Interp : Interp - Snapshot;
			if(FirstSync || StateChanged || (SnapshotChanged && Drift > 300))
			{
				m_ClockPositionMs = Snapshot;
				m_ClockTick = Now;
			}
		}
		else
		{
			// No trustworthy absolute position. Keep a pure wall-clock timer:
			//  - on (re)start (m_ClockTick==0, e.g. right after ResetMediaClock on a
			//    track change) begin counting from m_ClockPositionMs (0) now;
			//  - on pause, latch the current interpolated position and stop;
			//  - on resume, restart the interpolation window from the latched value.
			if(m_ClockTick == 0)
			{
				m_ClockTick = Now;
			}
			else if(Playing != m_ClockPlaying)
			{
				if(!Playing)
				{
					// pause: freeze at current position
					m_ClockPositionMs = CurrentPositionMs();
					m_ClockTick = 0;
				}
				else
				{
					// resume: restart interpolation from the latched position
					m_ClockTick = Now;
				}
			}
		}

		// Only carry a duration when the provider gave a real one; otherwise keep 0
		// so CurrentPositionMs() does not clamp ambiguously.
		m_ClockDurationMs = std::max<int64_t>(0, DurationMs);
		m_ClockPlaying = Playing;
	}

	std::string m_ActiveKey;
	ELyricsState m_LyricsState = ELyricsState::IDLE;
	std::vector<SLyricLine> m_vLines;
	// Key + revision of m_vLines as last copied from the active SNeteaseMeta; lets
	// UpdateNetease skip the per-frame deep copy when nothing changed. Both are
	// needed: a revision alone misses a track that returns after m_vLines was
	// cleared, and a key alone misses lyrics that arrive mid-track.
	std::string m_LinesKey;
	uint64_t m_LinesRevision = 0;
	std::map<std::string, SCacheEntry> m_Cache;
	std::shared_ptr<IHttpRequest> m_pRequest;
	std::shared_ptr<CResponseJob<SCacheEntry>> m_pRequestJob;
	std::string m_RequestKey;

	void AbortRequest()
	{
		if(m_pRequest)
		{
#if defined(CONF_FAMILY_WINDOWS)
			auto It = m_NeteaseData.find(m_RequestKey);
			if(It != m_NeteaseData.end())
			{
				It->second.m_TriedLrclib = false;
				It->second.m_LyricState = ELyricsState::NOTFOUND;
			}
#endif
			m_pRequest->Abort();
			m_pRequest.reset();
		}
		if(m_pRequestJob)
		{
			m_pRequestJob->Abort();
			m_pRequestJob.reset();
		}
		m_RequestKey.clear();
	}

	void StoreLrclibResult(const std::string &Key, const SCacheEntry &Entry)
	{
		if(Key.empty())
			return;
		if(m_Cache.size() >= LYRICS_CACHE_MAX)
			m_Cache.clear();
		m_Cache[Key] = Entry;
#if defined(CONF_FAMILY_WINDOWS)
		auto It = m_NeteaseData.find(Key);
		if(It != m_NeteaseData.end())
		{
			SNeteaseMeta &Meta = It->second;
			Meta.m_vLines = Entry.m_vLines;
			++Meta.m_LinesRevision;
			if(Entry.m_State == ELyricsState::OFFLINE && m_pHttp != nullptr && Meta.m_LrclibAttempts < 3)
			{
				// 加速器/代理偶发超时：保留 LOADING，3 秒后重试；三次仍失败才置 OFFLINE。
				Meta.m_TriedLrclib = false;
				Meta.m_LyricState = ELyricsState::LOADING;
				Meta.m_LrclibRetryAt = time_get() + time_freq() * 3;
			}
			else
			{
				Meta.m_TriedLrclib = true;
				Meta.m_LyricState = Entry.m_State;
			}
		}
#endif
		if(Key == m_ActiveKey)
		{
			m_LyricsState = Entry.m_State;
			m_vLines = Entry.m_vLines;
		}
	}

	void ResetLyrics()
	{
		AbortRequest();
#if defined(CONF_FAMILY_WINDOWS)
		AbortNetease();
#endif
		m_ActiveKey.clear();
		m_LyricsState = ELyricsState::IDLE;
		m_vLines.clear();
		// Invalidate the copy cache too: with the key left behind, the same song
		// returning after this reset would match and skip the refill, leaving the
		// lyric view empty.
		m_LinesKey.clear();
		ResetMediaClock();
	}

	// ---------------- lrclib lyrics (foreign tracks / SMTC) ----------------
	void StartRequest(const STrack &Track)
	{
		if(m_pHttp == nullptr || m_pEngine == nullptr)
		{
			SCacheEntry Entry;
			Entry.m_State = ELyricsState::OFFLINE;
			StoreLrclibResult(m_ActiveKey, Entry);
			return;
		}

		const int DurationSec = (int)((std::max<int64_t>(0, Track.m_DurationMs) + 500) / 1000);

		// EscapeUrl writes in-place and returns void, so escape first
		char aEscapedTitle[512];
		char aEscapedArtist[512];
		char aEscapedAlbum[512];
		EscapeUrl(aEscapedTitle, Track.m_Title.c_str());
		EscapeUrl(aEscapedArtist, Track.m_Artist.c_str());
		EscapeUrl(aEscapedAlbum, Track.m_Album.c_str());

		char aBase[128];
		str_copy(aBase, g_Config.m_TcIslandLrclibBase[0] ? g_Config.m_TcIslandLrclibBase : "https://lrclib.net", sizeof(aBase));
		int BaseLen = str_length(aBase);
		while(BaseLen > 0 && aBase[BaseLen - 1] == '/')
			aBase[--BaseLen] = '\0';
		if(aBase[0] == '\0')
			str_copy(aBase, "https://lrclib.net", sizeof(aBase));

		// IMPORTANT: use /api/search (fuzzy) rather than /api/get (exact match).
		// SMTC titles often carry suffixes ("(Live)", "- Remastered"), missing album,
		// or slightly different artist strings, which make the exact /api/get 404.
		// Search returns an array; ProcessRequest() picks the best synced entry.
		// We keep duration in the query to help ranking when available.
		char aUrl[2048];
		if(DurationSec >= 1 && DurationSec <= 3600)
			str_format(aUrl, sizeof(aUrl), "%s/api/search?track_name=%s&artist_name=%s&duration=%d",
				aBase, aEscapedTitle, aEscapedArtist, DurationSec);
		else
			str_format(aUrl, sizeof(aUrl), "%s/api/search?track_name=%s&artist_name=%s",
				aBase, aEscapedTitle, aEscapedArtist);

		m_pRequest = HttpGet(aUrl);
		m_pRequest->MaxResponseSize(LYRICS_MAX_BYTES);
		m_pRequest->Timeout(CTimeout{15000, 45000, 500, 10});
		m_pRequest->LogProgress(HTTPLOG::FAILURE);
		m_pRequest->FailOnErrorStatus(false);
		m_pRequest->HeaderString("Lrclib-Client", "TClient (github.com/TaterClient)");
		m_RequestKey = m_ActiveKey;
		m_LyricsState = ELyricsState::LOADING;
		m_pHttp->Run(m_pRequest);
	}

	void ProcessRequest()
	{
		if(!m_pRequest || !m_pRequest->Done())
			return;
		if(!m_pRequestJob)
		{
			m_pRequestJob = std::make_shared<CResponseJob<SCacheEntry>>(m_pRequest, ParseLrclibResponse);
			m_pEngine->AddJob(m_pRequestJob);
			return;
		}
		if(m_pRequestJob->State() != IJob::STATE_DONE)
			return;

		auto pFinished = std::move(m_pRequestJob);
		m_pRequest.reset();
		const std::string FinishedKey = std::move(m_RequestKey);
		m_RequestKey.clear();
		StoreLrclibResult(FinishedKey, pFinished->m_Result);
	}

	static void ParseLrclibResponse(const std::shared_ptr<IHttpRequest> &pFinished, SCacheEntry &Entry)
	{
		Entry.m_State = ELyricsState::NOTFOUND;
		// [HTTP-ASSERT] 请求可能以 ERROR/ABORTED 结束（超时/断网），此时先取 StatusCode
		// 会触发 http.cpp 的 "Request not done" 断言弹窗；先看状态再取码，失败一律按离线处理
		const EHttpState FinishedState = pFinished->State();
		const int StatusCode = FinishedState == EHttpState::DONE ? pFinished->StatusCode() : 0;
		if(FinishedState != EHttpState::DONE || StatusCode == 0)
			Entry.m_State = ELyricsState::OFFLINE;
		else if(StatusCode >= 200 && StatusCode < 300)
		{
			json_value *pRoot = pFinished->ResultJson();
			if(pRoot != nullptr && pRoot != &json_value_none)
			{
				// /api/search returns a JSON array of candidate entries; /api/get
				// returns a single object. Pick the best entry that actually has
				// synced lyrics (fall back to plain lyrics are not used here).
				const char *pSyncedText = "";
				if(pRoot->type == json_array)
				{
					for(int i = 0; i < json_array_length(pRoot); ++i)
					{
						const json_value *pItem = json_array_get(pRoot, i);
						if(pItem == nullptr || pItem->type != json_object)
							continue;
						const json_value *pSyncedValue = json_object_get(pItem, "syncedLyrics");
						if(pSyncedValue != nullptr && pSyncedValue->type == json_string && pSyncedValue->u.string.ptr[0] != '\0')
						{
							pSyncedText = pSyncedValue->u.string.ptr;
							break;
						}
					}
				}
				else if(pRoot->type == json_object)
				{
					const json_value *pSyncedValue = json_object_get(pRoot, "syncedLyrics");
					if(pSyncedValue != nullptr && pSyncedValue->type == json_string)
						pSyncedText = pSyncedValue->u.string.ptr;
				}

				if(ParseSyncedLyrics(pSyncedText, Entry.m_vLines))
					Entry.m_State = ELyricsState::READY;
				json_value_free(pRoot);
			}
		}
	}

	void UpdateLyrics(const STrack &Track)
	{
		if(!Track.m_Valid || (Track.m_Title.empty() && Track.m_Artist.empty()))
		{
			ResetLyrics();
			return;
		}

#if defined(CONF_FAMILY_WINDOWS)
		// SMTC and the title monitor share one identity through every fallback.
		UpdateNetease(Track);
#else
		const std::string Key = BuildCacheKey(Track);
		if(Key != m_ActiveKey)
		{
			AbortRequest();
			m_ActiveKey = Key;
			m_vLines.clear();
			// Force a refill through the copy cache below even when this key was
			// seen before: m_vLines was just emptied.
			m_LinesKey.clear();
			ResetMediaClock();
			SyncMediaClock(Track.m_PositionMs, Track.m_DurationMs, Track.m_Playing);

			const auto It = m_Cache.find(Key);
			if(It != m_Cache.end())
			{
				m_LyricsState = It->second.m_State;
				m_vLines = It->second.m_vLines;
				return;
			}
			StartRequest(Track);
			return;
		}

		SyncMediaClock(Track.m_PositionMs, Track.m_DurationMs, Track.m_Playing);
		ProcessRequest();
#endif
	}

	// ---------------- netease cloudmusic lyrics via music.163.com ----------------
#if defined(CONF_FAMILY_WINDOWS)
	static constexpr size_t NETEASE_META_MAX = 12;

	struct SNeteaseMeta
	{
		bool m_Resolved = false; // search finished, song id + duration known
		bool m_TriedLyric = false;
		int64_t m_SongId = 0;
		int64_t m_DurationMs = 0;
		std::string m_Title;  // title as shown by the netease window, kept for version matching
		std::string m_Artist; // ditto
		std::string m_Album;  // 专辑名，用于区分同名的原版 / 现场版
		ELyricsState m_LyricState = ELyricsState::IDLE;
		std::vector<SLyricLine> m_vLines;
		// Bumped on every write to m_vLines above. The render thread copies the
		// vector into m_vLines each frame; without this it would deep-copy ~100
		// lines (each a string plus a per-char karaoke word vector) 968 times a
		// second for nothing.
		uint64_t m_LinesRevision = 0;
		// plain LRC from lrc.lyric, kept so a track that has karaoke yrc can
		// degrade to whole-line lyrics if yrc parsing ever fails.
		std::vector<SLyricLine> m_vFallbackLines;
		// album cover url scraped from the search response (album.picUrl). We
		// download it ourselves rather than trusting SMTC's Thumbnail().
		std::string m_PicUrl;
		bool m_CoverQueued = false; // a cover download for m_PicUrl is in flight / done
		// The /api/search/get/web response often omits album.picUrl, so when it is
		// missing we resolve it via /api/song/detail (which always carries it).
		bool m_DetailQueued = false;
		// QQ 音乐第三源（NetEase NOTFOUND 后、lrclib 兜底前尝试）
		bool m_TriedQQ = false; // QQ search/lyric completed, successfully or not
		bool m_TriedKugou = false; // 酷狗 search/candidate/lyric completed
		bool m_TriedLrclib = false;
		int m_LrclibAttempts = 0;
		int64_t m_LrclibRetryAt = 0;
		std::string m_QQMid;
		std::string m_KugouHash;
		std::string m_KugouLyricId;
		std::string m_KugouAccessKey;
	};

	class CMusicResponse
	{
	public:
		SNeteaseMeta m_Meta;
		bool m_Lyric = false;
		bool m_Detail = false;
		bool m_Cover = false;
		int m_KugouStage = -1;
		std::vector<uint8_t> m_vCoverPng;
	};

	std::shared_ptr<CResponseJob<CMusicResponse>> m_pNeteaseJob;
	std::shared_ptr<CResponseJob<CMusicResponse>> m_pQQJob;
	std::shared_ptr<CResponseJob<CMusicResponse>> m_pKugouJob;
	std::map<std::string, SNeteaseMeta> m_NeteaseData;
	std::string m_NeteaseInFlight; // cache key currently being resolved
	bool m_NeteaseInLyricPhase = false;
	bool m_NeteaseInCoverPhase = false;
	bool m_NeteaseInDetailPhase = false;
	std::shared_ptr<IHttpRequest> m_pNeteaseRequest;
	std::shared_ptr<IHttpRequest> m_pQQRequest;
	std::shared_ptr<IHttpRequest> m_pKugouRequest;
	std::string m_QQInFlight; // independent of NetEase detail/cover ownership
	bool m_QQInSearch = false;
	bool m_QQInLyric = false;
	std::string m_KugouInFlight;
	int m_KugouInStage = 0; // 0=search, 1=candidate, 2=lyric download

	void AbortNetease()
	{
		if(m_pNeteaseRequest)
		{
			auto It = m_NeteaseData.find(m_NeteaseInFlight);
			if(It != m_NeteaseData.end())
			{
				if(m_NeteaseInDetailPhase)
					It->second.m_DetailQueued = false;
				else if(m_NeteaseInCoverPhase)
					It->second.m_CoverQueued = false;
				else
				{
					It->second.m_TriedLyric = false;
					It->second.m_LyricState = ELyricsState::IDLE;
				}
			}
			m_pNeteaseRequest->Abort();
			m_pNeteaseRequest.reset();
		}
		if(m_pQQRequest)
		{
			auto It = m_NeteaseData.find(m_QQInFlight);
			if(It != m_NeteaseData.end())
			{
				It->second.m_TriedQQ = false;
				It->second.m_LyricState = ELyricsState::NOTFOUND;
			}
			m_pQQRequest->Abort();
			m_pQQRequest.reset();
		}
		if(m_pNeteaseJob)
		{
			m_pNeteaseJob->Abort();
			m_pNeteaseJob.reset();
		}
		if(m_pKugouRequest)
		{
			auto It = m_NeteaseData.find(m_KugouInFlight);
			if(It != m_NeteaseData.end())
			{
				It->second.m_TriedKugou = false;
				It->second.m_KugouHash.clear();
				It->second.m_KugouLyricId.clear();
				It->second.m_KugouAccessKey.clear();
			}
			m_pKugouRequest->Abort();
			m_pKugouRequest.reset();
		}
		if(m_pQQJob)
		{
			m_pQQJob->Abort();
			m_pQQJob.reset();
		}
		if(m_pKugouJob)
		{
			m_pKugouJob->Abort();
			m_pKugouJob.reset();
		}
		m_NeteaseInFlight.clear();
		m_QQInFlight.clear();
		m_KugouInFlight.clear();
		m_KugouInStage = 0;
		m_NeteaseInLyricPhase = false;
		m_NeteaseInCoverPhase = false;
		m_NeteaseInDetailPhase = false;
		m_QQInSearch = false;
		m_QQInLyric = false;
	}

	// 网易云封面 URL → CDN 缩略图(300×300):原图常到 3000px,渲染线程解码
	// + 纹理上传要顶一整帧甚至更多(用户:"歌曲加载的一瞬间会卡")。CDN 自带
	// 服务端缩图参数,下载/解码/上传都小一个数量级。
	static std::string NeteaseThumbUrl(const std::string &Url)
	{
		if(Url.rfind("http", 0) != 0 || Url.find('?') != std::string::npos)
			return Url;
		return Url + "?param=300y300";
	}

	static std::string NeteaseCacheKey(const STrack &Track)
	{
		std::string Key;
		Key.reserve(160);
		Key += "n163|";
		Key += Track.m_Artist;
		Key += '|';
		Key += Track.m_Title;
		Key += '|';
		Key += Track.m_Album;
		return Key;
	}

	void StartNeteaseRequest(const std::string &Key, const char *pUrl, bool Lyric = false, bool Detail = false, bool Cover = false)
	{
		m_pNeteaseRequest = HttpGet(pUrl);
		m_pNeteaseRequest->MaxResponseSize(Cover ? COVER_MAX_BYTES : LYRICS_MAX_BYTES);
		m_pNeteaseRequest->Timeout(CTimeout{10000, 30000, 500, 10});
		m_pNeteaseRequest->LogProgress(HTTPLOG::FAILURE);
		m_pNeteaseRequest->FailOnErrorStatus(false);
		m_NeteaseInFlight = Key;
		m_NeteaseInLyricPhase = Lyric;
		m_NeteaseInDetailPhase = Detail;
		m_NeteaseInCoverPhase = Cover;
		m_pHttp->Run(m_pNeteaseRequest);
	}

	void StartQQ(const std::string &Key, SNeteaseMeta &Meta)
	{
		char aUrl[2048];
		if(Meta.m_QQMid.empty())
		{
			char aEscT[512], aEscA[512];
			EscapeUrl(aEscT, Meta.m_Title.c_str());
			EscapeUrl(aEscA, Meta.m_Artist.c_str());
			str_format(aUrl, sizeof(aUrl),
				"https://c.y.qq.com/soso/fcgi-bin/client_search_cp?format=json&n=3&w=%s%%20%s", aEscT, aEscA);
		}
		else
		{
			char aEscMid[512];
			EscapeUrl(aEscMid, Meta.m_QQMid.c_str());
			str_format(aUrl, sizeof(aUrl),
				"https://c.y.qq.com/lyric/fcgi-bin/fcg_query_lyric_new.fcg?format=json&nobase64=1&g_tk=5381&loginUin=0&hostUin=0&platform=yqq&songmid=%s", aEscMid);
		}
		m_pQQRequest = HttpGet(aUrl);
		m_pQQRequest->MaxResponseSize(LYRICS_MAX_BYTES);
		m_pQQRequest->Timeout(CTimeout{10000, 30000, 500, 10});
		m_pQQRequest->LogProgress(HTTPLOG::FAILURE);
		m_pQQRequest->FailOnErrorStatus(false);
		m_pQQRequest->HeaderString("Referer", "https://y.qq.com/");
		m_QQInFlight = Key;
		m_QQInSearch = Meta.m_QQMid.empty();
		m_QQInLyric = !m_QQInSearch;
		Meta.m_LyricState = ELyricsState::LOADING;
		m_pHttp->Run(m_pQQRequest);
	}

	void UpdateNetease(const STrack &Track)
	{
		const std::string Key = NeteaseCacheKey(Track);
		const bool NewTrack = Key != m_ActiveKey;
		if(NewTrack)
		{
			AbortRequest();
			AbortNetease();
			m_ActiveKey = Key;
			m_vLines.clear();
			// Force the copy below to refill: m_vLines was just emptied, so a key
			// match alone no longer means the render-side copy is current.
			m_LinesKey.clear();
			ResetMediaClock();
			// 切歌：清掉上一首的曲长，防止新歌 resolve 完成前旧曲长参与重锚换算
			if(m_pIslandView)
				*(volatile double *)((uint8_t *)m_pIslandView + offsetof(SIslandShm, ClientDurationMs)) = 0.0;
		}

		// Evict only before inserting a new track, never while a request handler
		// holds a reference to an entry. Keep the in-flight entry if there is one.
		if(m_NeteaseData.find(Key) == m_NeteaseData.end())
		{
			while(m_NeteaseData.size() >= NETEASE_META_MAX)
			{
				auto It = m_NeteaseData.begin();
				if(It->first == m_NeteaseInFlight)
					++It;
				if(It == m_NeteaseData.end())
					break;
				m_NeteaseData.erase(It);
			}
		}
		SNeteaseMeta &Meta = m_NeteaseData[Key];
		if(NewTrack)
		{
			Meta.m_LrclibAttempts = 0;
			Meta.m_LrclibRetryAt = 0;
		}
		Meta.m_Title = Track.m_Title;
		Meta.m_Artist = Track.m_Artist;
		Meta.m_Album = Track.m_Album;
		// v6: 单字段原子回传曲长给 monitor（seek 重锚把 Fraction 换算成秒用）。
		// 必须只写这一个字段——整结构回写会与 monitor 50ms 一次的 SHM 刷新竞争、覆盖它的数据。
		if(m_pIslandView && Meta.m_DurationMs > 0)
			*(volatile double *)((uint8_t *)m_pIslandView + offsetof(SIslandShm, ClientDurationMs)) = (double)Meta.m_DurationMs;
		// 2026-10-29 起位置来源升级：monitor 的 WASAPI 进程环回提供真实"已渲染毫秒"（AudioMs），
		// 暂停自动冻结、seek 由 monitor 侧 LS 重锚修正 —— 它是真进度，不再走"纯墙钟自计时"。
		// DurationMs>0（LS 给了曲长）时 SyncMediaClock 采信绝对位置并逐轮重锚；
		// DurationMs==0 时自动退回旧的自计时分支（含暂停冻结），行为不变。
		SyncMediaClock(Track.m_PositionMs, Track.m_DurationMs, Track.m_Playing);

		// Finish all transports before deciding the next stage. Cover work never
		// owns the lyric state and continues while QQ/lrclib are in flight.
		ProcessNetease();
		ProcessQQ();
		ProcessKugou();
		ProcessRequest();

		if((m_pHttp == nullptr || m_pEngine == nullptr) && Meta.m_LyricState != ELyricsState::READY)
		{
			Meta.m_LyricState = ELyricsState::OFFLINE;
			Meta.m_TriedLyric = Meta.m_TriedQQ = Meta.m_TriedLrclib = true;
		}
		else if(m_pHttp != nullptr && m_pEngine != nullptr)
		{
			if(!m_pNeteaseRequest && !Meta.m_TriedLyric && TcDetectSourceService(Track.m_ServiceId) <= 1)
			{
				char aUrl[1400];
				if(Meta.m_Resolved)
				{
					str_format(aUrl, sizeof(aUrl), "https://music.163.com/api/song/lyric?id=%lld&lv=-1&tv=-1&yv=-1", (long long)Meta.m_SongId);
					StartNeteaseRequest(Key, aUrl, true);
				}
				else
				{
					char aEscapedQuery[768];
					std::string Query = Track.m_Title;
					if(!Track.m_Artist.empty())
						Query += " " + Track.m_Artist;
					EscapeUrl(aEscapedQuery, Query.c_str());
					str_format(aUrl, sizeof(aUrl), "https://music.163.com/api/search/get/web?s=%s&type=1&limit=20", aEscapedQuery);
					StartNeteaseRequest(Key, aUrl);
				}
				Meta.m_LyricState = ELyricsState::LOADING;
			}

			// 播放器是 QQ/酷我/酷狗时，先向它自己的平台要歌词，不拿网易云的搜索凑数。
			const int SourceService = TcDetectSourceService(Track.m_ServiceId);
			if(SourceService > 1 && !Meta.m_TriedLyric)
			{
				Meta.m_TriedLyric = true;
				Meta.m_LyricState = ELyricsState::NOTFOUND;
			}
			if(Meta.m_TriedLyric && Meta.m_vLines.empty())
			{
				// 播放器是酷狗就先用酷狗，否则 QQ 优先；两个都失败才走 lrclib。
				const bool KugouFirst = TcDetectSourceService(Track.m_ServiceId) == 4;
				bool Started = false;
				for(int Pass = 0; Pass < 2 && !Started; ++Pass)
				{
					const bool WantKugou = (Pass == 0) == KugouFirst;
					if(WantKugou)
					{
						if(!Meta.m_TriedKugou && !m_pKugouRequest)
						{
							StartKugou(Key, Meta);
							Started = true;
						}
					}
					else if(!Meta.m_TriedQQ && !m_pQQRequest)
					{
						StartQQ(Key, Meta);
						Started = true;
					}
				}
				if(!Started && Meta.m_TriedQQ && Meta.m_TriedKugou && !Meta.m_TriedLrclib && !m_pRequest && time_get() >= Meta.m_LrclibRetryAt)
				{
					Meta.m_TriedLrclib = true;
					++Meta.m_LrclibAttempts;
					Meta.m_LyricState = ELyricsState::LOADING;
					StartRequest(Track);
				}
			}

			if(!m_pNeteaseRequest && Meta.m_Resolved && Meta.m_TriedLyric)
			{
				if(Meta.m_PicUrl.empty() && !Meta.m_DetailQueued)
				{
					char aUrl[512];
					str_format(aUrl, sizeof(aUrl), "https://music.163.com/api/song/detail?ids=[%lld]", (long long)Meta.m_SongId);
					Meta.m_DetailQueued = true;
					StartNeteaseRequest(Key, aUrl, false, true);
				}
				else if(!Meta.m_PicUrl.empty() && !Meta.m_CoverQueued)
				{
					Meta.m_CoverQueued = true;
					if(Meta.m_PicUrl.rfind("http", 0) == 0)
						StartNeteaseRequest(Key, Meta.m_PicUrl.c_str(), false, false, true);
				}
			}
		}

		m_LyricsState = Meta.m_LyricState;
		if(m_LinesKey != Key || m_LinesRevision != Meta.m_LinesRevision)
		{
			m_LinesKey = Key;
			m_LinesRevision = Meta.m_LinesRevision;
			m_vLines = Meta.m_vLines;
		}
	}

	void ProcessNetease()
	{
		if(!m_pNeteaseRequest || !m_pNeteaseRequest->Done())
			return;
		if(!m_pNeteaseJob)
		{
			m_pNeteaseJob = std::make_shared<CResponseJob<CMusicResponse>>(m_pNeteaseRequest, ParseNeteaseResponse);
			auto &Response = m_pNeteaseJob->m_Result;
			Response.m_Lyric = m_NeteaseInLyricPhase;
			Response.m_Detail = m_NeteaseInDetailPhase;
			Response.m_Cover = m_NeteaseInCoverPhase;
			const auto It = m_NeteaseData.find(m_NeteaseInFlight);
			if(It != m_NeteaseData.end())
			{
				// Snapshot only the search inputs, not the cached lyric vectors.
				Response.m_Meta.m_Title = It->second.m_Title;
				Response.m_Meta.m_Artist = It->second.m_Artist;
				Response.m_Meta.m_Album = It->second.m_Album;
				Response.m_Meta.m_DurationMs = It->second.m_DurationMs;
			}
			m_pEngine->AddJob(m_pNeteaseJob);
			return;
		}
		if(m_pNeteaseJob->State() != IJob::STATE_DONE)
			return;

		auto pFinished = std::move(m_pNeteaseJob);
		m_pNeteaseRequest.reset();
		const std::string Key = std::move(m_NeteaseInFlight);
		m_NeteaseInFlight.clear();
		m_NeteaseInLyricPhase = false;
		m_NeteaseInDetailPhase = false;
		m_NeteaseInCoverPhase = false;
		const auto It = m_NeteaseData.find(Key);
		if(Key.empty() || It == m_NeteaseData.end())
			return;

		SNeteaseMeta &Meta = It->second;
		auto &Response = pFinished->m_Result;
		SNeteaseMeta &Parsed = Response.m_Meta;
		// Apply only this stage's fields. Detail/cover can finish while QQ or
		// lrclib updates lyrics, so replacing the whole Meta would lose that work.
		if(Response.m_Detail)
		{
			if(!Parsed.m_PicUrl.empty())
				Meta.m_PicUrl = std::move(Parsed.m_PicUrl);
			Meta.m_DetailQueued = true;
			return;
		}
		if(Response.m_Cover)
		{
			if(Key == m_ActiveKey && !Response.m_vCoverPng.empty())
			{
				std::lock_guard<std::mutex> Lock(m_Mutex);
				m_CoverPng = std::move(Response.m_vCoverPng);
				m_CoverKey = "163|" + Meta.m_Title;
				m_CoverDirty = true;
			}
			Meta.m_CoverQueued = true;
			return;
		}
		if(Response.m_Lyric)
		{
			Meta.m_vFallbackLines = std::move(Parsed.m_vFallbackLines);
			if(!Parsed.m_vLines.empty())
			{
				Meta.m_vLines = std::move(Parsed.m_vLines);
				++Meta.m_LinesRevision;
				Meta.m_LyricState = ELyricsState::READY;
			}
		}
		else if(Parsed.m_Resolved)
		{
			Meta.m_Resolved = true;
			Meta.m_SongId = Parsed.m_SongId;
			Meta.m_DurationMs = Parsed.m_DurationMs;
			if(!Parsed.m_PicUrl.empty())
				Meta.m_PicUrl = std::move(Parsed.m_PicUrl);
		}
		if(Response.m_Lyric || !Meta.m_Resolved)
		{
			Meta.m_TriedLyric = true;
			if(Meta.m_vLines.empty())
				Meta.m_LyricState = ELyricsState::NOTFOUND;
		}
	}

	// ---- 统一的歌词候选评分 ------------------------------------------------
	// 所有平台共用一套规则选版本：歌名本体 > 歌手 > 专辑 > 曲长 > Live/Remix 标记。
	// 旧逻辑里 QQ 取第一条、网易云对"换歌手"只扣 500 分，Live 版就串到了别的歌手。
	static std::string TcStripBrackets(const std::string &Name)
	{
		static const char *s_apOpen[] = {"(", "（", "[", "【"};
		static const char *s_apClose[] = {")", "）", "]", "】"};
		std::string Out;
		Out.reserve(Name.size());
		size_t i = 0;
		while(i < Name.size())
		{
			bool Stripped = false;
			for(int b = 0; b < 4; ++b)
			{
				const size_t OpenLen = strlen(s_apOpen[b]);
				if(Name.compare(i, OpenLen, s_apOpen[b]) == 0)
				{
					const size_t Close = Name.find(s_apClose[b], i + OpenLen);
					if(Close != std::string::npos)
					{
						i = Close + strlen(s_apClose[b]);
						Out.push_back(' ');
						Stripped = true;
					}
					break;
				}
			}
			if(!Stripped)
				Out.push_back(Name[i++]);
		}
		return Out;
	}

	// 中文歌名里的空格、连字符、大小写都不影响匹配，统一压平后再比。
	static std::string TcCompactName(const std::string &Name)
	{
		std::string Out;
		Out.reserve(Name.size());
		for(char c : Name)
		{
			if(c == ' ' || c == '\t' || c == '-' || c == '_' || c == ':' || c == '.' || c == '/')
				continue;
			Out.push_back((char)tolower((unsigned char)c));
		}
		return Out;
	}

	static bool TcContainsTag(const std::string &Name, const char *const *ppTags, size_t NumTags)
	{
		std::string Lower;
		Lower.reserve(Name.size());
		for(char c : Name)
			Lower.push_back((char)tolower((unsigned char)c));
		for(size_t i = 0; i < NumTags; ++i)
			if(Lower.find(ppTags[i]) != std::string::npos)
				return true;
		return false;
	}

	struct STcVersionTags
	{
		bool m_Live = false;
		bool m_Remix = false;
		bool m_Acoustic = false;
		bool m_Inst = false;
		bool m_Cover = false;
	};

	static void TcVersionTags(const std::string &Name, STcVersionTags &Tags)
	{
		static const char *s_apLiveTags[] = {"live", "现场", "演唱会", "concert", "unplugged"};
		static const char *s_apRemixTags[] = {"remix", "混音", "dj版", "抖音"};
		static const char *s_apInstTags[] = {"伴奏", "instrumental", "karaoke"};
		static const char *s_apAcousticTags[] = {"acoustic", "不插电"};
		static const char *s_apCoverTags[] = {"翻唱", "cover", "深情版", "女声版", "男声版", "童声版", "钢琴版", "纯音乐"};
		Tags.m_Live = TcContainsTag(Name, s_apLiveTags, sizeof(s_apLiveTags) / sizeof(s_apLiveTags[0]));
		Tags.m_Remix = TcContainsTag(Name, s_apRemixTags, sizeof(s_apRemixTags) / sizeof(s_apRemixTags[0]));
		Tags.m_Inst = TcContainsTag(Name, s_apInstTags, sizeof(s_apInstTags) / sizeof(s_apInstTags[0]));
		Tags.m_Acoustic = TcContainsTag(Name, s_apAcousticTags, sizeof(s_apAcousticTags) / sizeof(s_apAcousticTags[0]));
		Tags.m_Cover = TcContainsTag(Name, s_apCoverTags, sizeof(s_apCoverTags) / sizeof(s_apCoverTags[0]));
	}

	// 当前播放的是哪个客户端：决定优先问哪个平台的歌词，而不是每次都从网易云搜起。
	static int TcDetectSourceService(const std::string &ServiceId)
	{
		std::string Lower;
		Lower.reserve(ServiceId.size());
		for(char c : ServiceId)
			Lower.push_back((char)tolower((unsigned char)c));
		if(Lower.find("cloudmusic") != std::string::npos || Lower.find("netease") != std::string::npos)
			return 1;
		if(Lower.find("qqmusic") != std::string::npos || Lower.find("qq.com") != std::string::npos)
			return 2;
		if(Lower.find("kuwo") != std::string::npos || Lower.find("kwmusic") != std::string::npos)
			return 3;
		if(Lower.find("kugou") != std::string::npos || Lower.find("kgmusic") != std::string::npos)
			return 4;
		return 0;
	}

	static constexpr int TC_MATCH_ACCEPT_SCORE = 700;

	// 返回分数；低于 TC_MATCH_ACCEPT_SCORE 视为"这不是同一首"，继续问下一个平台。
	static int TcScoreSongMatch(const std::string &WantTitle, const std::string &WantArtist, const std::string &WantAlbum, int64_t WantDurationMs,
		const std::string &Name, const std::string &ArtistName, const std::string &AlbumName, int64_t DurationMs)
	{
		const std::string WantBase = TcCompactName(TcStripBrackets(WantTitle));
		const std::string CandBase = TcCompactName(TcStripBrackets(Name));

		int Score;
		if(!WantBase.empty() && CandBase == WantBase)
			Score = 1000;
		else if(!WantBase.empty() && (CandBase.find(WantBase) != std::string::npos || WantBase.find(CandBase) != std::string::npos))
			Score = 400;
		else
			Score = -1200;

		STcVersionTags Want, Cand;
		TcVersionTags(WantTitle, Want);
		TcVersionTags(Name, Cand);
		if(Want.m_Live != Cand.m_Live)
			Score -= 1500;
		else if(Want.m_Live)
			Score += 400;
		if(Want.m_Remix != Cand.m_Remix)
			Score -= 600;
		else if(Want.m_Remix)
			Score += 200;
		if(Want.m_Acoustic && !Cand.m_Acoustic)
			Score -= 300;
		if(Cand.m_Inst && !Want.m_Remix)
			Score -= 800;
		if(Cand.m_Cover && !Want.m_Cover)
			Score -= 700;
		else if(Cand.m_Cover && Want.m_Cover)
			Score += 150;

		if(!WantArtist.empty() && !ArtistName.empty())
		{
			const std::string A = TcCompactName(WantArtist);
			const std::string B = TcCompactName(ArtistName);
			if(A == B)
				Score += 600;
			else if(!A.empty() && !B.empty() && (A.find(B) != std::string::npos || B.find(A) != std::string::npos))
				Score += 300;
			else
				Score -= 900; // 换歌手通常是另一版/翻唱，不能压过原唱
		}

		if(!WantAlbum.empty() && !AlbumName.empty())
		{
			const std::string A = TcCompactName(WantAlbum);
			const std::string B = TcCompactName(AlbumName);
			if(A == B)
				Score += 500;
			else if(!A.empty() && !B.empty() && (A.find(B) != std::string::npos || B.find(A) != std::string::npos))
				Score += 300;
			else
				Score -= 200;
		}

		if(WantDurationMs > 0 && DurationMs > 0)
		{
			int64_t DurDiff = std::llabs((long long)(DurationMs - WantDurationMs)) / 10;
			if(DurDiff > 300)
				DurDiff = 300;
			Score -= (int)DurDiff;
		}
		return Score;
	}

	static void ParseNeteaseResponse(const std::shared_ptr<IHttpRequest> &pFinished, CMusicResponse &Response)
	{
		SNeteaseMeta &Meta = Response.m_Meta;
		const bool WasLyric = Response.m_Lyric;
		const bool WasDetail = Response.m_Detail;
		const bool WasCover = Response.m_Cover;

		// detail phase: resolve album.picUrl that the search response omitted.
		if(WasDetail)
		{
			if(pFinished->State() == EHttpState::DONE && pFinished->StatusCode() >= 200 && pFinished->StatusCode() < 300)
			{
				json_value *pRoot = pFinished->ResultJson();
				if(pRoot != nullptr && pRoot != &json_value_none && pRoot->type == json_object)
				{
					const json_value *pSongs = json_object_get(pRoot, "songs");
					const json_value *pSong = (pSongs != nullptr && pSongs->type == json_array && pSongs->u.array.length > 0) ? pSongs->u.array.values[0] : nullptr;
					const json_value *pAlbum = pSong ? json_object_get(pSong, "album") : nullptr;
					const json_value *pPic = pAlbum ? json_object_get(pAlbum, "picUrl") : nullptr;
					if(pPic != nullptr && pPic->type == json_string && pPic->u.string.length > 0)
						Meta.m_PicUrl = NeteaseThumbUrl(pPic->u.string.ptr);
				}
				if(pRoot != nullptr && pRoot != &json_value_none)
					json_value_free(pRoot);
			}
			Meta.m_DetailQueued = true;
			return;
		}

		// Copy raw cover bytes on the worker as well. The render thread only
		// moves the finished buffer after checking that the track is still active.
		if(WasCover)
		{
			if(pFinished->State() == EHttpState::DONE && pFinished->StatusCode() >= 200 && pFinished->StatusCode() < 300)
			{
				unsigned char *pResultPtr = nullptr;
				size_t ResultLen = 0;
				pFinished->Result(&pResultPtr, &ResultLen);
				if(pResultPtr != nullptr && ResultLen > 0 && ResultLen <= COVER_MAX_BYTES)
					Response.m_vCoverPng.assign(pResultPtr, pResultPtr + ResultLen);
			}
			Meta.m_CoverQueued = true;
			return;
		}

		if(pFinished->State() == EHttpState::DONE && pFinished->StatusCode() >= 200 && pFinished->StatusCode() < 300)
		{
			json_value *pRoot = pFinished->ResultJson();
			if(pRoot != nullptr && pRoot != &json_value_none && pRoot->type == json_object)
			{
				if(!WasLyric)
				{
					// search response: 用统一评分选版本（歌名本体 + 歌手 + 专辑 + 曲长 + 版本标记）。
					const json_value *pResult = json_object_get(pRoot, "result");
					const json_value *pSongs = pResult ? json_object_get(pResult, "songs") : nullptr;
					const json_value *pBestSong = nullptr;
					int BestScore = -1000000;

					if(pSongs != nullptr && pSongs->type == json_array)
					{
						for(unsigned i = 0; i < pSongs->u.array.length; ++i)
						{
							const json_value *pSong = pSongs->u.array.values[i];
							if(pSong == nullptr || pSong->type != json_object) continue;
							const json_value *pId = json_object_get(pSong, "id");
							if(pId == nullptr || pId->type != json_integer || pId->u.integer <= 0) continue;
							const json_value *pName = json_object_get(pSong, "name");
							const json_value *pDur = json_object_get(pSong, "duration");
							const char *pNameText = (pName != nullptr && pName->type == json_string) ? pName->u.string.ptr : "";

							std::string CandidateArtist;
							const json_value *pArtists = json_object_get(pSong, "artists");
							if(pArtists != nullptr && pArtists->type == json_array && pArtists->u.array.length > 0)
							{
								const json_value *pFirst = pArtists->u.array.values[0];
								const json_value *pArtistName = (pFirst != nullptr && pFirst->type == json_object) ? json_object_get(pFirst, "name") : nullptr;
								if(pArtistName != nullptr && pArtistName->type == json_string)
									CandidateArtist = pArtistName->u.string.ptr;
							}
							std::string CandidateAlbum;
							const json_value *pAlbum = json_object_get(pSong, "album");
							const json_value *pAlbumName = pAlbum ? json_object_get(pAlbum, "name") : nullptr;
							if(pAlbumName != nullptr && pAlbumName->type == json_string)
								CandidateAlbum = pAlbumName->u.string.ptr;

							const int64_t CandDurMs = (pDur != nullptr && pDur->type == json_integer) ? pDur->u.integer : 0;
							const int Score = TcScoreSongMatch(Meta.m_Title, Meta.m_Artist, Meta.m_Album, Meta.m_DurationMs,
								pNameText, CandidateArtist, CandidateAlbum, CandDurMs);
							if(Score > BestScore) { BestScore = Score; pBestSong = pSong; }
						}
					}
					if(pBestSong != nullptr && BestScore >= TC_MATCH_ACCEPT_SCORE)
					{
						const json_value *pId = json_object_get(pBestSong, "id");
						const json_value *pDur = json_object_get(pBestSong, "duration");
						Meta.m_Resolved = true; Meta.m_SongId = pId->u.integer;
						if(pDur != nullptr && pDur->type == json_integer) Meta.m_DurationMs = std::max<int64_t>(0, pDur->u.integer);
						// scrape album cover url (album.picUrl) straight from the search hit
						const json_value *pAlbum = json_object_get(pBestSong, "album");
						const json_value *pPic = pAlbum ? json_object_get(pAlbum, "picUrl") : nullptr;
						if(pPic != nullptr && pPic->type == json_string && pPic->u.string.length > 0)
							Meta.m_PicUrl = NeteaseThumbUrl(pPic->u.string.ptr);
					}
					else
					{
						// 没有足够像的候选：不要拿"别的歌手的 Live 版"凑合，交给后续平台继续找。
						Meta.m_TriedLyric = true;
						Meta.m_LyricState = ELyricsState::NOTFOUND;
					}
				}
				else
				{
					// lyric response: prefer karaoke yrc (per-character timing), fall
					// back to plain lrc (whole-line timing).
					const json_value *pYrc = json_object_get(pRoot, "yrc");
					const json_value *pYrcLyric = pYrc ? json_object_get(pYrc, "lyric") : nullptr;
					const char *pYrcText = (pYrcLyric != nullptr && pYrcLyric->type == json_string) ? pYrcLyric->u.string.ptr : "";
					const json_value *pLrc = json_object_get(pRoot, "lrc");
					const json_value *pLyric = pLrc ? json_object_get(pLrc, "lyric") : nullptr;
					const char *pLyricText = (pLyric != nullptr && pLyric->type == json_string) ? pLyric->u.string.ptr : "";

					const bool LrcOk = ParseSyncedLyrics(pLyricText, Meta.m_vFallbackLines);

						bool Ok = false;
						if(pYrcText[0] != '\0')
						{
							std::vector<SLyricLine> vYrc;
							if(ParseYrcLyrics(pYrcText, vYrc))
							{
								Meta.m_vLines = std::move(vYrc);
								++Meta.m_LinesRevision;
								Ok = true;
							}
						}
						if(!Ok && LrcOk)
						{
							Meta.m_vLines = Meta.m_vFallbackLines;
							++Meta.m_LinesRevision;
							Ok = true;
						}

					Meta.m_LyricState = Ok ? ELyricsState::READY : ELyricsState::NOTFOUND;
				}
			}
			if(pRoot != nullptr && pRoot != &json_value_none)
				json_value_free(pRoot);
		}

		// Every completed search/lyric request settles its stage, including
		// transport errors, non-2xx responses, invalid JSON and empty results.
		if(WasLyric || !Meta.m_Resolved)
		{
			Meta.m_TriedLyric = true;
			if(Meta.m_vLines.empty())
				Meta.m_LyricState = ELyricsState::NOTFOUND;
		}
	}

	// ---------------- QQ 音乐第三源（NetEase NOTFOUND 后、lrclib 前） ----------------
	// 两阶段：搜索(w=标题 歌手)拿 songmid → 歌词接口拿明文 LRC。解析复用 ParseSyncedLyrics。
	void ProcessQQ()
	{
		if(!m_pQQRequest || !m_pQQRequest->Done())
			return;
		if(!m_pQQJob)
		{
			m_pQQJob = std::make_shared<CResponseJob<CMusicResponse>>(m_pQQRequest, ParseQQResponse);
			m_pQQJob->m_Result.m_Lyric = m_QQInLyric;
			{
				const auto It = m_NeteaseData.find(m_QQInFlight);
				if(It != m_NeteaseData.end())
				{
					m_pQQJob->m_Result.m_Meta.m_Title = It->second.m_Title;
					m_pQQJob->m_Result.m_Meta.m_Artist = It->second.m_Artist;
					m_pQQJob->m_Result.m_Meta.m_Album = It->second.m_Album;
					m_pQQJob->m_Result.m_Meta.m_DurationMs = It->second.m_DurationMs;
				}
			}
			m_pEngine->AddJob(m_pQQJob);
			return;
		}
		if(m_pQQJob->State() != IJob::STATE_DONE)
			return;

		auto pFinished = std::move(m_pQQJob);
		m_pQQRequest.reset();
		const std::string Key = std::move(m_QQInFlight);
		m_QQInFlight.clear();
		m_QQInSearch = false;
		m_QQInLyric = false;
		auto It = m_NeteaseData.find(Key);
		if(Key.empty() || It == m_NeteaseData.end())
			return;
		SNeteaseMeta &Meta = It->second;
		SNeteaseMeta &Parsed = pFinished->m_Result.m_Meta;
		const bool FoundMid = !Parsed.m_QQMid.empty();
		if(FoundMid)
			Meta.m_QQMid = std::move(Parsed.m_QQMid);
		if(!Parsed.m_vLines.empty())
		{
			Meta.m_vLines = std::move(Parsed.m_vLines);
			++Meta.m_LinesRevision;
			Meta.m_vFallbackLines.clear();
			Meta.m_LyricState = ELyricsState::READY;
		}
		if(FoundMid && Key == m_ActiveKey && m_pHttp != nullptr)
		{
			StartQQ(Key, Meta);
			return;
		}
		Meta.m_TriedQQ = !FoundMid;
		if(Meta.m_vLines.empty())
			Meta.m_LyricState = ELyricsState::NOTFOUND;
	}

	static void ParseQQResponse(const std::shared_ptr<IHttpRequest> &pFinished, CMusicResponse &Response)
	{
		SNeteaseMeta &Meta = Response.m_Meta;
		const bool WasSearch = !Response.m_Lyric;
		const bool WasLyric = Response.m_Lyric;
		if(pFinished->State() == EHttpState::DONE && pFinished->StatusCode() >= 200 && pFinished->StatusCode() < 300)
		{
			json_value *pRoot = pFinished->ResultJson();
			if(pRoot != nullptr && pRoot != &json_value_none && pRoot->type == json_object)
			{
				const json_value *pCode = json_object_get(pRoot, "code");
				const bool Success = pCode->type == json_none || (pCode->type == json_integer && pCode->u.integer == 0);
				if(Success && WasSearch)
				{
					// 旧代码直接取 list[0]，Live 版会串到别人的翻唱上。现在对全部结果评分，
					// 分数不够就不采信，让后面的平台继续找。
					const json_value *pData = json_object_get(pRoot, "data");
					const json_value *pSong = json_object_get(pData, "song");
					const json_value *pList = json_object_get(pSong, "list");
					const json_value *pBest = nullptr;
					int BestScore = -1000000;
					if(pList != nullptr && pList->type == json_array)
					{
						for(unsigned i = 0; i < pList->u.array.length; ++i)
						{
							const json_value *pItem = pList->u.array.values[i];
							if(pItem == nullptr || pItem->type != json_object) continue;
							const json_value *pMid = json_object_get(pItem, "songmid");
							if(pMid == nullptr || pMid->type != json_string || pMid->u.string.length == 0) continue;
							const json_value *pName = json_object_get(pItem, "songname");
							const json_value *pAlbumName = json_object_get(pItem, "albumname");
							const json_value *pInterval = json_object_get(pItem, "interval");
							const char *pNameText = (pName != nullptr && pName->type == json_string) ? pName->u.string.ptr : "";
							const char *pAlbumText = (pAlbumName != nullptr && pAlbumName->type == json_string) ? pAlbumName->u.string.ptr : "";
							std::string CandidateArtist;
							const json_value *pSingers = json_object_get(pItem, "singer");
							if(pSingers != nullptr && pSingers->type == json_array && pSingers->u.array.length > 0)
							{
								const json_value *pFirstSinger = pSingers->u.array.values[0];
								const json_value *pSingerName = (pFirstSinger != nullptr && pFirstSinger->type == json_object) ? json_object_get(pFirstSinger, "name") : nullptr;
								if(pSingerName != nullptr && pSingerName->type == json_string)
									CandidateArtist = pSingerName->u.string.ptr;
							}
							const int64_t CandDurMs = (pInterval != nullptr && pInterval->type == json_integer) ? pInterval->u.integer * 1000 : 0;
							const int Score = TcScoreSongMatch(Meta.m_Title, Meta.m_Artist, Meta.m_Album, Meta.m_DurationMs,
								pNameText, CandidateArtist, pAlbumText, CandDurMs);
							if(Score > BestScore) { BestScore = Score; pBest = pItem; }
						}
					}
					if(pBest != nullptr && BestScore >= TC_MATCH_ACCEPT_SCORE)
					{
						const json_value *pMid = json_object_get(pBest, "songmid");
						Meta.m_QQMid = pMid->u.string.ptr;
					}
				}
				else if(Success && WasLyric)
				{
					const json_value *pLyric = json_object_get(pRoot, "lyric");
					std::vector<SLyricLine> vLines;
					if(pLyric->type == json_string && ParseSyncedLyrics(pLyric->u.string.ptr, vLines))
					{
						Meta.m_vLines = std::move(vLines);
						++Meta.m_LinesRevision;
						Meta.m_vFallbackLines.clear();
						Meta.m_LyricState = ELyricsState::READY;
					}
				}
			}
			if(pRoot != nullptr && pRoot != &json_value_none)
				json_value_free(pRoot);
		}
	}
#endif

	// ---------------- 酷狗歌词源（播放器是酷狗时优先） ----------------
	// 三阶段：搜索拿 hash → krcs 拿歌词 id/accesskey → 下载 base64 LRC。
	void StartKugou(const std::string &Key, SNeteaseMeta &Meta)
	{
		char aUrl[2048];
		int Stage = 2;
		if(Meta.m_KugouHash.empty())
			Stage = 0;
		else if(Meta.m_KugouLyricId.empty())
			Stage = 1;

		if(Stage == 0)
		{
			char aEscT[512], aEscA[512];
			EscapeUrl(aEscT, Meta.m_Title.c_str());
			EscapeUrl(aEscA, Meta.m_Artist.c_str());
			str_format(aUrl, sizeof(aUrl), "http://mobilecdn.kugou.com/api/v3/search/song?format=json&keyword=%s%%20%s&page=1&pagesize=15", aEscT, aEscA);
		}
		else if(Stage == 1)
		{
			char aEscT[512];
			EscapeUrl(aEscT, Meta.m_Title.c_str());
			const int DurationSec = Meta.m_DurationMs > 0 ? (int)(Meta.m_DurationMs / 1000) : 0;
			str_format(aUrl, sizeof(aUrl), "http://krcs.kugou.com/search?ver=1&man=yes&client=mobi&keyword=%s&duration=%d&hash=%s", aEscT, DurationSec, Meta.m_KugouHash.c_str());
		}
		else
		{
			str_format(aUrl, sizeof(aUrl), "http://lyrics.kugou.com/download?ver=1&client=pc&id=%s&accesskey=%s&fmt=lrc&charset=utf8", Meta.m_KugouLyricId.c_str(), Meta.m_KugouAccessKey.c_str());
		}

		m_pKugouRequest = HttpGet(aUrl);
		m_pKugouRequest->MaxResponseSize(LYRICS_MAX_BYTES);
		m_pKugouRequest->Timeout(CTimeout{10000, 30000, 500, 10});
		m_pKugouRequest->LogProgress(HTTPLOG::FAILURE);
		m_pKugouRequest->FailOnErrorStatus(false);
		m_pKugouRequest->HeaderString("Referer", "https://www.kugou.com/");
		m_KugouInFlight = Key;
		m_KugouInStage = Stage;
		Meta.m_LyricState = ELyricsState::LOADING;
		m_pHttp->Run(m_pKugouRequest);
	}

	static void ParseKugouResponse(const std::shared_ptr<IHttpRequest> &pFinished, CMusicResponse &Response)
	{
		SNeteaseMeta &Meta = Response.m_Meta;
		const int Stage = Response.m_KugouStage;
		if(pFinished->State() != EHttpState::DONE || pFinished->StatusCode() < 200 || pFinished->StatusCode() >= 300)
			return;
		json_value *pRoot = pFinished->ResultJson();
		if(pRoot == nullptr || pRoot == &json_value_none || pRoot->type != json_object)
		{
			if(pRoot != nullptr && pRoot != &json_value_none)
				json_value_free(pRoot);
			return;
		}

		if(Stage == 0)
		{
			const json_value *pData = json_object_get(pRoot, "data");
			const json_value *pInfo = pData ? json_object_get(pData, "info") : nullptr;
			const json_value *pBest = nullptr;
			int BestScore = -1000000;
			if(pInfo != nullptr && pInfo->type == json_array)
			{
				for(unsigned i = 0; i < pInfo->u.array.length; ++i)
				{
					const json_value *pItem = pInfo->u.array.values[i];
					if(pItem == nullptr || pItem->type != json_object) continue;
					const json_value *pHash = json_object_get(pItem, "hash");
					if(pHash == nullptr || pHash->type != json_string || pHash->u.string.length == 0) continue;
					const json_value *pName = json_object_get(pItem, "songname");
					const json_value *pSinger = json_object_get(pItem, "singername");
					const json_value *pAlbum = json_object_get(pItem, "album_name");
					const json_value *pDuration = json_object_get(pItem, "duration");
					const char *pNameText = (pName != nullptr && pName->type == json_string) ? pName->u.string.ptr : "";
					const char *pSingerText = (pSinger != nullptr && pSinger->type == json_string) ? pSinger->u.string.ptr : "";
					const char *pAlbumText = (pAlbum != nullptr && pAlbum->type == json_string) ? pAlbum->u.string.ptr : "";
					const int64_t CandDurMs = (pDuration != nullptr && pDuration->type == json_integer) ? pDuration->u.integer * 1000 : 0;
					const int Score = TcScoreSongMatch(Meta.m_Title, Meta.m_Artist, Meta.m_Album, Meta.m_DurationMs,
						pNameText, pSingerText, pAlbumText, CandDurMs);
					if(Score > BestScore) { BestScore = Score; pBest = pItem; }
				}
			}
			if(pBest != nullptr && BestScore >= TC_MATCH_ACCEPT_SCORE)
			{
				const json_value *pHash = json_object_get(pBest, "hash");
				Meta.m_KugouHash = pHash->u.string.ptr;
			}
		}
		else if(Stage == 1)
		{
			const json_value *pCands = json_object_get(pRoot, "candidates");
			if(pCands != nullptr && pCands->type == json_array && pCands->u.array.length > 0)
			{
				const json_value *pCand = pCands->u.array.values[0];
				const json_value *pId = pCand ? json_object_get(pCand, "id") : nullptr;
				const json_value *pKey = pCand ? json_object_get(pCand, "accesskey") : nullptr;
				if(pId != nullptr && pId->type == json_integer)
				{
					char aId[32];
					str_format(aId, sizeof(aId), "%lld", (long long)pId->u.integer);
					Meta.m_KugouLyricId = aId;
				}
				else if(pId != nullptr && pId->type == json_string)
					Meta.m_KugouLyricId = pId->u.string.ptr;
				if(pKey != nullptr && pKey->type == json_string)
					Meta.m_KugouAccessKey = pKey->u.string.ptr;
			}
		}
		else if(Stage == 2)
		{
			const json_value *pContent = json_object_get(pRoot, "content");
			if(pContent != nullptr && pContent->type == json_string && pContent->u.string.length > 0)
			{
				const size_t SrcLen = pContent->u.string.length;
				std::vector<char> vDecoded(SrcLen / 4 * 3 + 8);
				const int DecodedLen = str_base64_decode(vDecoded.data(), (int)vDecoded.size() - 1, pContent->u.string.ptr);
				if(DecodedLen > 0)
				{
					vDecoded[DecodedLen] = '\0';
					std::vector<SLyricLine> vLines;
					if(ParseSyncedLyrics(vDecoded.data(), vLines))
					{
						Meta.m_vLines = std::move(vLines);
						++Meta.m_LinesRevision;
						Meta.m_vFallbackLines.clear();
						Meta.m_LyricState = ELyricsState::READY;
					}
				}
			}
		}
		json_value_free(pRoot);
	}

	void ProcessKugou()
	{
		if(!m_pKugouRequest || !m_pKugouRequest->Done())
			return;
		if(!m_pKugouJob)
		{
			m_pKugouJob = std::make_shared<CResponseJob<CMusicResponse>>(m_pKugouRequest, ParseKugouResponse);
			m_pKugouJob->m_Result.m_KugouStage = m_KugouInStage;
			{
				const auto It = m_NeteaseData.find(m_KugouInFlight);
				if(It != m_NeteaseData.end())
				{
					m_pKugouJob->m_Result.m_Meta.m_Title = It->second.m_Title;
					m_pKugouJob->m_Result.m_Meta.m_Artist = It->second.m_Artist;
					m_pKugouJob->m_Result.m_Meta.m_Album = It->second.m_Album;
					m_pKugouJob->m_Result.m_Meta.m_DurationMs = It->second.m_DurationMs;
				}
			}
			m_pEngine->AddJob(m_pKugouJob);
			return;
		}
		if(m_pKugouJob->State() != IJob::STATE_DONE)
			return;

		auto pFinished = std::move(m_pKugouJob);
		m_pKugouRequest.reset();
		const std::string Key = std::move(m_KugouInFlight);
		m_KugouInFlight.clear();
		const int Stage = m_KugouInStage;
		m_KugouInStage = 0;
		auto It = m_NeteaseData.find(Key);
		if(Key.empty() || It == m_NeteaseData.end())
			return;
		SNeteaseMeta &Meta = It->second;
		SNeteaseMeta &Parsed = pFinished->m_Result.m_Meta;
		if(!Parsed.m_KugouHash.empty())
			Meta.m_KugouHash = std::move(Parsed.m_KugouHash);
		if(!Parsed.m_KugouLyricId.empty())
		{
			Meta.m_KugouLyricId = std::move(Parsed.m_KugouLyricId);
			Meta.m_KugouAccessKey = std::move(Parsed.m_KugouAccessKey);
		}
		if(!Parsed.m_vLines.empty())
		{
			Meta.m_vLines = std::move(Parsed.m_vLines);
			++Meta.m_LinesRevision;
			Meta.m_TriedKugou = true;
			Meta.m_LyricState = ELyricsState::READY;
			return;
		}

		// 成功拿到下一阶段的参数就继续，否则这一源到此为止。
		if(Stage == 0 && !Meta.m_KugouHash.empty() && Key == m_ActiveKey && m_pHttp != nullptr)
		{
			StartKugou(Key, Meta);
			return;
		}
		if(Stage == 1 && !Meta.m_KugouLyricId.empty() && Key == m_ActiveKey && m_pHttp != nullptr)
		{
			StartKugou(Key, Meta);
			return;
		}
		Meta.m_TriedKugou = true;
		if(Meta.m_vLines.empty())
			Meta.m_LyricState = ELyricsState::NOTFOUND;
	}

	int FindLineIndex(int64_t PositionMs) const
	{
		if(m_vLines.empty())
			return -1;
		// Before the first lyric line (intro / pre-roll, or right after a track
		// change when the self-timer restarts at 0) still show the first line
		// rather than collapsing back to the song title -- otherwise the display
		// flickers between "lyric" and "title" every time the position dips below
		// the first timestamp.
		if(PositionMs < m_vLines.front().m_StartMs)
			return 0;

		int Lo = 0;
		int Hi = (int)m_vLines.size() - 1;
		int Best = 0;
		while(Lo <= Hi)
		{
			const int Mid = Lo + (Hi - Lo) / 2;
			if(m_vLines[Mid].m_StartMs <= PositionMs)
			{
				Best = Mid;
				Lo = Mid + 1;
			}
			else
			{
				Hi = Mid - 1;
			}
		}
		return Best;
	}

	// ---------------- track collection ----------------
	// 失联抑制：标题抖动的播放器（酷狗等 SMTC 实现差的）会让 Track 瞬时变空 →
	// BuildCacheKey 变 → 歌词被清（用户反馈的"歌词一会有一会没"）。3 秒内沿用上一次
	// 有效 track，等播放器恢复；真关掉播放器则 3 秒后正常消失。
	STrack m_LastGoodTrack;
	double m_LastGoodTrackAt = 0.0;

	STrack CollectTrackStable()
	{
		STrack T = CollectTrack();
		const bool HasTitle = !T.m_Title.empty() || !T.m_Artist.empty();
		if(T.m_Valid && HasTitle)
		{
			m_LastGoodTrack = T;
			m_LastGoodTrackAt = (double)std::time(nullptr);
			return T;
		}
		if(!m_LastGoodTrack.m_Title.empty() && (double)std::time(nullptr) - m_LastGoodTrackAt < 3.0)
			return m_LastGoodTrack;
		return T;
	}

	STrack CollectTrack()
	{
#if defined(CONF_FAMILY_WINDOWS)
		if(g_Config.m_TcIslandNetease)
		{
			EnsureMonitorRunning();
			const STrack Monitor = MonitorTrack();
			const STrack SmTc = TakeSnapshot();
			// prefer SMTC (precise position/duration); fall back to the monitor,
			// or leave SMTC when it merely exists (paused) while monitor is active
			// Some players (notably the NetEase desktop client) report a bogus
			// SMTC Timeline.Position() -- an absurd absolute value with duration 0.
			// Treat such a snapshot as untrustworthy and fall back to the monitor,
			// which reads the player's real progress.
			const int64_t kMaxReasonablePosMs = 6LL * 3600LL * 1000LL; // 6h
			const bool SmTcPosBogus =
				SmTc.m_Valid &&
				(SmTc.m_PositionMs > kMaxReasonablePosMs ||
				 (SmTc.m_DurationMs > 0 && SmTc.m_PositionMs > SmTc.m_DurationMs + 10000));
			// The NetEase desktop client also reports Position()==0 with Duration==0
			// while playing -- a frozen-at-zero snapshot that is useless for lyrics.
			// When the monitor has a real, advancing position, trust it instead.
			const bool SmTcPosUnusable =
				SmTc.m_Valid && SmTc.m_DurationMs == 0 &&
				(SmTc.m_PositionMs == 0 || SmTc.m_PositionMs < m_LastSnapshotMs);
			STrack Track;
			if((SmTcPosBogus || SmTcPosUnusable) && Monitor.m_Valid && Monitor.m_Playing)
				Track = Monitor;
			else if(SmTcPosBogus && Monitor.m_Valid)
				Track = Monitor;
			else if(Monitor.m_Valid && (!SmTc.m_Valid || (!SmTc.m_Playing && Monitor.m_Playing)))
				Track = Monitor;
			else
				Track = SmTc;
			// 播放/暂停状态的权威归属：
			//  - 网易云标题路径（service=netease-title）：monitor 权威 —— 它用音频帧/LS 活性判暂停，
			//    是实测唯一可靠的信号（网易云暂停时 SMTC 仍报 Playing，见 §175）。
			//  - 其它播放器（QQ 音乐等，Track 来自 SMTC）：它们的 SMTC 状态是正常的，**不能**被
			//    monitor 盖掉 —— monitor 只认网易云，QQ 音乐在播时它报"没播"，会误杀播放动画
			//    （用户实测：QQ 音乐五线谱不跳）。仅当 monitor 检测到网易云且有数据时才覆盖。
			if(Monitor.m_Valid && Track.m_ServiceId == "netease-title")
				Track.m_Playing = Monitor.m_Playing;
			return Track;
		}
#endif
		return TakeSnapshot();
	}

	// ---------------- rendering ----------------
	static void FitText(ITextRender *pTextRender, float Size, char (&aBuf)[512], float MaxWidth)
	{
		if(MaxWidth <= 0)
			return;
		while(aBuf[0] != '\0' && pTextRender->TextWidth(Size, aBuf) > MaxWidth)
			aBuf[strlen(aBuf) - 1] = '\0';
	}

	// 缓存 TextWidth 的结果(2026-11-09)：TextWidth 内部走 TextEx →
	// CreateTextContainer + AppendTextContainer + DeleteTextContainer 的全量字形布局，
	// 每次至少 3 次 make_shared，代价远高于字面量除法。灵动岛每帧量的这些串要么是
	// 常量("00:00"、"↑~999.9k"…)、要么来自有界集合(计时器 %02d:%02d)，而字号只在
	// Scale/字号 cvar 变化时才变；字体变化时先清表，再按 (文本, 字号) 匹配。
	// 表满后按轮转位覆盖，计时器短串也能复用。
	float MeasureCached(ITextRender *pTextRender, float Size, const char *pText)
	{
		if(!pText || pText[0] == '\0')
			return 0.0f;
		const char *pFace = g_Config.m_TcIslandFont[0] ? g_Config.m_TcIslandFont : g_Config.m_TcCustomFont;
		if(!pFace[0])
		{
			const auto *pFaces = pTextRender->GetCustomFaces();
			if(pFaces && !pFaces->empty())
				pFace = pFaces->front().c_str();
		}
		// 切换独立、全局或默认字体后，旧字宽不再适用于当前字形布局。
		if(m_TextWidthCacheFace != pFace)
		{
			m_TextWidthCacheFace = pFace;
			m_TextWidthCacheCount = 0;
			m_TextWidthCacheNext = 0;
		}
		const int FontSize = g_Config.m_TcFontSize;
		for(int i = 0; i < m_TextWidthCacheCount; i++)
		{
			if(m_aTextWidthCache[i].m_Size == Size && m_aTextWidthCache[i].m_FontSize == FontSize && str_comp(m_aTextWidthCache[i].m_aText, pText) == 0)
				return m_aTextWidthCache[i].m_Width;
		}
		const float Width = pTextRender->TextWidth(Size, pText);
		if(str_length(pText) < (int)sizeof(m_aTextWidthCache[0].m_aText))
		{
			int Slot;
			if(m_TextWidthCacheCount < TEXT_WIDTH_CACHE_MAX)
				Slot = m_TextWidthCacheCount++;
			else
				Slot = m_TextWidthCacheNext++ % TEXT_WIDTH_CACHE_MAX;
			CTextWidthCacheEntry &Entry = m_aTextWidthCache[Slot];
			str_copy(Entry.m_aText, pText);
			Entry.m_Size = Size;
			Entry.m_FontSize = FontSize;
			Entry.m_Width = Width;
		}
		return Width;
	}

	// TClient: game round timer, drawn inside the island (left of the disc icon)
	// so it no longer sits at the top-centre and gets covered by the island.
	// Mirrors CHud::RenderGameTimer()'s time computation. Returns empty string
	// when there is no game info / timer to show.
	void GetGameTimeString(char *pBuf, int BufSize)
	{
		pBuf[0] = '\0';
		auto *pGameClient = m_pGameClient;
		if(!pGameClient || !pGameClient->m_Snap.m_pGameInfoObj)
			return;

		const CNetObj_GameInfo *pInfo = pGameClient->m_Snap.m_pGameInfoObj;
		int Time = 0;
		if(pInfo->m_GameStateFlags & GAMESTATEFLAG_SUDDENDEATH)
			return; // sudden death shows its own banner, no timer
		if(pInfo->m_TimeLimit && (pInfo->m_WarmupTimer <= 0))
		{
			Time = pInfo->m_TimeLimit * 60 - ((pGameClient->Client()->GameTick(g_Config.m_ClDummy) - pInfo->m_RoundStartTick) / pGameClient->Client()->GameTickSpeed());
			if(pInfo->m_GameStateFlags & GAMESTATEFLAG_GAMEOVER)
				Time = 0;
		}
		else if(pInfo->m_GameStateFlags & GAMESTATEFLAG_RACETIME)
		{
			Time = (pGameClient->Client()->GameTick(g_Config.m_ClDummy) + pInfo->m_WarmupTimer) / pGameClient->Client()->GameTickSpeed();
		}
		else
		{
			Time = (pGameClient->Client()->GameTick(g_Config.m_ClDummy) - pInfo->m_RoundStartTick) / pGameClient->Client()->GameTickSpeed();
		}
		// 默认格式：不足一小时显示 分:秒，超过一小时自动变成 时:分:秒（str_time 原行为）。
		// 胶囊宽度另按 00:00:00 预留，短格式时左侧留出小时位空白。
		str_time((int64_t)Time * 100, ETimeFormat::DAYS, pBuf, BufSize);
	}

	// ---- cover decode thread ------------------------------------------------
	// stbi decode + the circular alpha mask used to run inline in
	// UpdateCoverTexture(), i.e. on the render thread, exactly when a track
	// changed: a 2048x2048 cover is a 16 MiB RGBA buffer plus ~4M sqrt() calls.
	// It runs here now; the render thread only uploads finished pixels.
	void CoverDecodeMain()
	{
		auto ValidDimensions = [](int W, int H) {
			if(W <= 0 || H <= 0 || W > COVER_MAX_DIMENSION || H > COVER_MAX_DIMENSION)
				return false;
			const size_t Width = (size_t)W, Height = (size_t)H;
			return Width <= COVER_MAX_PIXELS / Height && Width <= std::numeric_limits<size_t>::max() / Height / 4;
		};

		while(true)
		{
			std::vector<uint8_t> Png;
			std::string Key;
			{
				std::unique_lock<std::mutex> Lock(m_Mutex);
				m_CoverDecodeCv.wait(Lock, [this]() { return m_CoverDecodeShutdown || m_CoverDecodeReqValid; });
				if(m_CoverDecodeShutdown)
					return;
				Png = std::move(m_vCoverDecodeReqPng);
				m_vCoverDecodeReqPng.clear();
				Key = std::move(m_CoverDecodeReqKey);
				m_CoverDecodeReqKey.clear();
				m_CoverDecodeReqValid = false;
			}

			CImageInfo Image;
			bool Failed = false;
			std::string Reason;
			int IW = 0, IH = 0, IChannels = 0;

			if(Png.empty() || Png.size() > COVER_MAX_BYTES || Png.size() > (size_t)std::numeric_limits<int>::max())
			{
				Failed = true;
				Reason = "invalid cover bytes=" + std::to_string(Png.size());
			}

			unsigned char *pPixels = nullptr;
			if(!Failed)
			{
				const int InputSize = (int)Png.size();
				if(!stbi_info_from_memory(Png.data(), InputSize, &IW, &IH, &IChannels) || !ValidDimensions(IW, IH))
				{
					Failed = true;
					Reason = "invalid cover dimensions or format";
				}
				else
				{
					pPixels = stbi_load_from_memory(Png.data(), InputSize, &IW, &IH, &IChannels, 4);
					if(!pPixels || !ValidDimensions(IW, IH))
					{
						Failed = true;
						Reason = "stbi-FAIL bytes=" + std::to_string(Png.size()) + " err=" + std::string(stbi_failure_reason() ? stbi_failure_reason() : "?");
					}
					else
					{
						Image.m_Width = (size_t)IW;
						Image.m_Height = (size_t)IH;
						Image.m_Format = CImageInfo::FORMAT_RGBA;
						Image.Allocate();
						if(!Image.m_pData)
						{
							Failed = true;
							Reason = "cover allocation failed";
						}
						else
						{
							memcpy(Image.m_pData, pPixels, (size_t)IW * (size_t)IH * 4);
							// 网易云黑胶样式：封面裁成圆形（边缘 2px 抗锯齿），黑胶边从圆外露出
							const float CXp = (IW - 1) * 0.5f;
							const float CYp = (IH - 1) * 0.5f;
							const float Radius = std::min(IW, IH) * 0.5f - 1.0f;
							unsigned char *pPx = (unsigned char *)Image.m_pData;
							for(int y = 0; y < IH; ++y)
							{
								for(int x = 0; x < IW; ++x)
								{
									const float Dx = x - CXp;
									const float Dy = y - CYp;
									const float Edge = Radius - std::sqrt(Dx * Dx + Dy * Dy);
									unsigned char &Alpha = pPx[((size_t)y * (size_t)IW + (size_t)x) * 4 + 3];
									if(Edge <= 0.0f)
										Alpha = 0;
									else if(Edge < 2.0f)
										Alpha = (unsigned char)(Alpha * (Edge / 2.0f));
								}
							}
							Reason = "decoded " + std::to_string(IW) + "x" + std::to_string(IH);
						}
					}
				}
			}
			if(pPixels)
				stbi_image_free(pPixels);

			{
				std::lock_guard<std::mutex> Lock(m_Mutex);
				// operator= 只搬字段、不释放旧数据，先手动释放
				if(m_CoverDecodedImage.m_pData)
					m_CoverDecodedImage.Free();
				m_CoverDecodedImage = std::move(Image);
				m_CoverDecodedKey = Key;
				m_CoverDecodeFailed = Failed;
				m_CoverDecodeReason = std::move(Reason);
				m_CoverDecodeReady = true;
			}
		}
	}

	void StartCoverDecodeThread()
	{
		if(m_CoverDecodeThread.joinable())
			return;
		m_CoverDecodeThread = std::thread([this]() { CoverDecodeMain(); });
	}

	void StopCoverDecodeThread()
	{
		if(!m_CoverDecodeThread.joinable())
			return;
		{
			std::lock_guard<std::mutex> Lock(m_Mutex);
			m_CoverDecodeShutdown = true;
		}
		m_CoverDecodeCv.notify_all();
		m_CoverDecodeThread.join();
	}

	// Ensure the GPU texture matches the published cover. The render thread only
	// publishes raw bytes to the decode thread and uploads finished pixels; the
	// PNG decode and circular mask happen off-thread (see CoverDecodeMain).
	// Returns true when m_CoverTexture holds a valid texture ready to draw.
	bool UpdateCoverTexture()
	{
		std::string Key;
		bool GotResult = false;
		bool Failed = false;
		std::string ResultKey;
		std::string Reason;
		bool RequestPending = false;
		CImageInfo Finished;
		{
			std::lock_guard<std::mutex> Lock(m_Mutex);
			Key = m_CoverKey;
			if(m_CoverDirty)
			{
				m_CoverDirty = false;
				// Hand the fresh bytes to the decode thread. If it is still busy
				// with an older request, replace it: only the newest cover matters.
				m_vCoverDecodeReqPng = std::move(m_CoverPng);
				m_CoverPng.clear();
				m_CoverDecodeReqKey = Key;
				m_CoverDecodeReqValid = true;
				m_CoverPendingKey = Key;
				m_CoverDecodeCv.notify_one();
			}
			RequestPending = m_CoverPendingKey == Key;
			if(m_CoverDecodeReady)
			{
				m_CoverDecodeReady = false;
				GotResult = true;
				Failed = m_CoverDecodeFailed;
				ResultKey = m_CoverDecodedKey;
				Reason = m_CoverDecodeReason;
				if(m_CoverDecodedImage.m_pData)
					Finished = std::move(m_CoverDecodedImage);
				if(ResultKey == Key)
					m_CoverPendingKey.clear();
			}
		}

		// Finished decode for the track we are showing: upload and swap the texture.
		if(GotResult && !Failed && Finished.m_pData && ResultKey == Key)
		{
			if(m_CoverTextureValid)
				m_pGraphics->UnloadTexture(&m_CoverTexture);
			m_CoverTextureValid = false;
			m_LoadedCoverKey = Key;
			m_CoverTexture = m_pGraphics->LoadTextureRawMove(Finished, 0, "island-cover");
			m_CoverTextureValid = m_CoverTexture.IsValid();
			m_LastTexResult = Reason + " valid=" + std::to_string((int)m_CoverTextureValid);
			return m_CoverTextureValid;
		}

		// Drop stale art when the track changed and nothing new will arrive: a
		// failed decode for this key, or a key with no cover queued at all. While
		// a decode is still in flight the previous texture stays up, so a track
		// change shows the old art for a few frames instead of a hole.
		if(Key != m_LoadedCoverKey && (!RequestPending || (GotResult && ResultKey == Key)))
		{
			if(GotResult && Failed)
				m_LastTexResult = Reason;
			if(m_CoverTextureValid)
				m_pGraphics->UnloadTexture(&m_CoverTexture);
			m_CoverTextureValid = false;
			m_LoadedCoverKey = Key;
		}
		return m_CoverTextureValid;
	}

	void Render()
	{
		if(m_pClient->State() != IClient::STATE_ONLINE && m_pClient->State() != IClient::STATE_DEMOPLAYBACK)
			return;
		if(!g_Config.m_TcIslandEnable)
			return;

		// 灵动岛独立字体：tc_island_font 非空时临时切换默认 face，画完立刻恢复全局
		// 自定义字体。SetCustomFace 改的是 glyph map 的全局默认 face，只在本次 Render
		// 期间生效，对已创建的文本容器无影响，因此不影响其它组件的渲染。
		const bool UseIslandFont = g_Config.m_TcIslandFont[0] != '\0';
		if(UseIslandFont)
			m_pTextRender->SetCustomFace(g_Config.m_TcIslandFont);
		auto RestoreFace = [&]() {
			if(!UseIslandFont)
				return;
			if(g_Config.m_TcCustomFont[0] != 0)
			{
				m_pTextRender->SetCustomFace(g_Config.m_TcCustomFont);
				return;
			}
			const auto *pFaces = m_pTextRender->GetCustomFaces();
			if(pFaces && !pFaces->empty()) // 空名单时 front() 是 UB
				m_pTextRender->SetCustomFace(pFaces->front().c_str());
		};

		const STrack Track = CollectTrackStable();
		UpdateLyrics(Track);

		// ---- resolution ----------------------------------------------------
		// The island used to live in a 300-unit-tall virtual space, which meant
		// every measurement was multiplied by ScreenHeight/300 (~3.6x at 1080p).
		// Text sizes then landed on fractional pixels and got rounded to the
		// nearest atlas size, so glyphs came out soft and the rim looked fuzzy.
		// Rendering in real screen pixels (1 unit == 1 px) keeps glyph sizes
		// integral and makes every hairline exactly as wide as it says.
		const int RealW = std::max(1, m_pGraphics->ScreenWidth());
		const int RealH = std::max(1, m_pGraphics->ScreenHeight());
		m_pGraphics->MapScreen(CScreenRect{0.0f, 0.0f, (float)RealW, (float)RealH});

		// keep the island physically the same size across resolutions: the
		// reference is a 72 px tall capsule on a 1080 px tall screen. Every inner
		// measurement below is a 72-px-basis value, so at 1080p with the size
		// setting at 100% they all land on whole pixels.
		const float UiScale = std::clamp((float)g_Config.m_TcIslandSize / 100.0f, 0.4f, 2.0f);
		const float ResScale = std::clamp((float)RealH / 1080.0f, 0.7f, 1.8f);
		const float Height = 72.0f * UiScale * ResScale;
		const float Scale = Height / 72.0f;
		const float ScreenW = (float)RealW;

		// ---- style: notch vs default Dynamic Island ------------------------
		// Notch style glues the pill to the very top edge with only the lower two
		// corners rounded. Default style floats the pill a little below the top
		// edge with all four corners rounded (a true floating capsule).
		const bool NotchStyle = g_Config.m_TcIslandNotch != 0;
		// 刘海模式整体上移 1px：把烘焙纹理顶边的 AA 半透明像素推出屏幕，让顶边真正"贴合"
		const float OffsetX = (float)g_Config.m_TcIslandOffsetX * Scale;
		const float OffsetY = (float)g_Config.m_TcIslandOffsetY * Scale;
		const float PillY = (NotchStyle ? -1.0f : Height * 0.55f) + OffsetY;
		const int PillCorners = NotchStyle ? (IGraphics::CORNER_BL | IGraphics::CORNER_BR) : IGraphics::CORNER_ALL;
		// 两种模式都是完整胶囊（端部半径 = 高度一半，iOS 灵动岛观感）。
		// 旧的 14px 圆角矩形端部弧太短，看起来"方"——用户反馈的"圆角不够光滑"即此。
		// 已核验：现有内容边距（时钟 19px / 封面 15px）在半圆端内仍然安全，布局无需收缩。
		const float PillRounding = Height * 0.5f;

		// ---- Dynamic Island capsule ------------------------------------------
		// A rounded pill at the top edge of the screen, horizontally centred; in
		// notch style only the two bottom corners are rounded and the top edge sits
		// flush against the screen top. The body colour comes from the user's two
		// gradient stops (tc_island_color_top / _bottom) and the accent colour drives
		// the karaoke fill, the progress rail and the equaliser. Content is either
		// the song title or a single karaoke lyric line, whichever is enabled.

		// 空态（没放歌）时音乐模块整体隐藏：胶囊收缩成只包住时钟单元，游戏计时
		// 和本地时间照旧，封面 / 歌名 / 歌词 / 等化器 / 进度轨一概不画；两个时钟
		// 都没开就整条胶囊都不画。用户 2026-11-02 要求，覆盖 1.7.8.18 的常显方案。
		// 三态：空态（没有任何曲目，或会话已明确停止/退出）、播放、暂停。
		// 判据用 Status 而不是"标题是否非空"：Windows 会把已退出播放器的 SMTC
		// 会话继续挂一段时间，只按标题判断的话岛永远收不起来（用户报的
		// "没放歌也没消失"就是这个）。
		const bool HasTrack = !Track.m_Title.empty() || !Track.m_Artist.empty();
		const bool Stopped = HasTrack && !Track.m_Playing && !Track.m_Paused;
		const bool RawIdle = !HasTrack || Stopped;

		// 去抖后再决定，避免状态抖动把整条岛翻来覆去地重建。
		{
			const double NowIdle = (double)time_get() / (double)time_freq();
			if(RawIdle != m_IdleStateCandidate)
			{
				m_IdleStateCandidate = RawIdle;
				m_IdleCandidateSince = NowIdle;
			}
			else if(m_IdleStateCandidate != m_IdleState && NowIdle - m_IdleCandidateSince > 0.25)
			{
				m_IdleState = m_IdleStateCandidate;
			}
		}
		const bool Idle = m_IdleState;

		// 状态切换时打一行，用户再报"没消失/不跳"时可以直接看日志定位是
		// 哪个 provider 报了什么样的状态。
		{
			static int s_LastStateSig = -1;
			const int StateSig = (RawIdle ? 4 : 0) + (Track.m_Playing ? 2 : 0) + (Track.m_Paused ? 1 : 0);
			if(StateSig != s_LastStateSig)
			{
				s_LastStateSig = StateSig;
				char aLogTitle[96];
				str_copy(aLogTitle, Track.m_Title.c_str(), sizeof(aLogTitle));
				dbg_msg("island_music", "state=%s playing=%d paused=%d title='%s'",
					RawIdle ? "idle" : "active", Track.m_Playing ? 1 : 0, Track.m_Paused ? 1 : 0, aLogTitle);
			}
		}

		// content strings (artist intentionally not shown)
		char aSong[512];
		if(!Track.m_Title.empty())
			str_copy(aSong, Track.m_Title.c_str());
		else if(!Track.m_Artist.empty())
			str_copy(aSong, Track.m_Artist.c_str());
		else
			str_copy(aSong, TCLocalize("No music playing"));

		// sizes: real pixel values at the 72 px reference height, so on a 1080p
		// screen the text renderer gets exactly integer font sizes and the glyphs
		// stay crisp. Anything derived from Height*0.x lands on half pixels.
		const float TitleSize = 12.0f * Scale;
		const float LyricSize = 18.0f * Scale;
		const float TimerSize = 16.0f * Scale;

		const int64_t LyricPosMs = CurrentPositionMs() - (int64_t)g_Config.m_TcIslandLyricOffset;
		const int LyricIndex = g_Config.m_TcIslandShowLyrics ? FindLineIndex(LyricPosMs) : -1;
		char aLyricBuf[512];
		const bool UseLyric = LyricIndex >= 0;
		if(UseLyric)
			str_copy(aLyricBuf, m_vLines[LyricIndex].m_Text.c_str());

		// cover art: try to load the current track's album thumbnail
		const bool HasCover = UpdateCoverTexture();

		// ---- left zone: game round timer -----------------------------------
		char aGameTime[32];
		GetGameTimeString(aGameTime, sizeof(aGameTime));
		const bool ShowGameTime = g_Config.m_TcIslandShowProgress && aGameTime[0] != '\0';
		float GameW = 0.0f;
		float GameTextW = 0.0f;
		if(ShowGameTime)
		{
			// 时钟本身用默认格式（分:秒，超过一小时才出现小时位），
			// 但胶囊始终按 00:00:00 预留宽度；短格式右对齐，左侧留出小时位空白。
			GameW = MeasureCached(m_pTextRender, TimerSize, "00:00:00");
			GameTextW = MeasureCached(m_pTextRender, TimerSize, "00:00");
		}
		const float GameTextOffset = (ShowGameTime && aGameTime[5] == '\0') ? (GameW - GameTextW) * 0.5f : 0.0f;

		// ---- link status cell (left of the game clock) -----------------------
		// Keep the probe's round-trip unconfirmed rate and the downlink snapshot
		// gap separate. The LED reflects the worst available observation only.
		const bool ShowLink = g_Config.m_TcIslandLinkStatus != 0 && m_pClient->State() == IClient::STATE_ONLINE;
		const CLinkProbeTracker *pProbe = ShowLink ? m_pClient->LinkProbe() : nullptr;
		const bool HasProbeEstimate = pProbe != nullptr && pProbe->HasRecentEstimate();
		const float ProbeUnconfirmedPercent = HasProbeEstimate ? pProbe->RecentUnconfirmedPercent() : -1.0f;
		const float SnapshotGapPercent = ShowLink ? m_pClient->DownlinkLossPercent() : -1.0f;
		const bool HasSnapshotGap = SnapshotGapPercent >= 0.0f;
		const float LinkSeverityPercent = std::max(ProbeUnconfirmedPercent, SnapshotGapPercent);
		const bool HasProbeWarning = HasProbeEstimate && ProbeUnconfirmedPercent > 0.0f &&
			ProbeUnconfirmedPercent >= (float)g_Config.m_ClDupUpstreamLowPct;
		const bool HasSnapshotWarning = HasSnapshotGap && SnapshotGapPercent > 0.0f &&
			SnapshotGapPercent >= (float)g_Config.m_ClDupUpstreamLowPct;
		const bool HasLinkWarning = HasProbeWarning || HasSnapshotWarning;
		const bool LinkKnown = (HasProbeEstimate && HasSnapshotGap) || HasLinkWarning;
		const int LinkLevel = !HasLinkWarning ? 0 : (LinkSeverityPercent >= (float)g_Config.m_ClDupUpstreamHighPct ? 2 : 1);
		const bool HasLatency = pProbe != nullptr && pProbe->HasLatency();
		const float LatencyMs = HasLatency ? pProbe->AverageLatencyMs() : -1.0f;
		char aProbeText[48], aSnapshotText[48], aLatencyText[16];
		if(HasProbeWarning)
			str_format(aProbeText, sizeof(aProbeText), TCLocalize("Probe %.1f%%"), ProbeUnconfirmedPercent);
		if(HasSnapshotWarning)
			str_format(aSnapshotText, sizeof(aSnapshotText), TCLocalize("Snapshot gap %.1f%%"), SnapshotGapPercent);
		if(HasLatency)
			str_format(aLatencyText, sizeof(aLatencyText), "%d ms", (int)(LatencyMs + 0.5f));
		else
			str_copy(aLatencyText, "--");
		char aProbeMaxText[48], aSnapshotMaxText[48];
		str_format(aProbeMaxText, sizeof(aProbeMaxText), TCLocalize("Probe %.1f%%"), 100.0f);
		str_format(aSnapshotMaxText, sizeof(aSnapshotMaxText), TCLocalize("Snapshot gap %.1f%%"), 100.0f);
		// Healthy links keep the original one-line latency readout. Only warning
		// observations get labelled percentage rows, fitted to the fixed capsule.
		const float LinkFontScale = std::clamp(g_Config.m_TcFontSize / 100.0f, 0.5f, 2.0f);
		const float LinkGlyphSize = std::min(12.0f * Scale * LinkFontScale, 16.0f * Scale);
		const float LinkSize = LinkGlyphSize / LinkFontScale;
		const float LinkLatencySize = 13.0f * Scale;
		const float LinkRowStep = LinkGlyphSize + 4.0f * Scale;
		const int LinkWarningRows = (int)HasProbeWarning + (int)HasSnapshotWarning;
		const float LinkRowsH = LinkGlyphSize + std::max(0, LinkWarningRows - 1) * LinkRowStep;
		const float LinkDot = 6.0f * Scale;
		const float LinkGap = 7.0f * Scale;
		const float LinkPad = 17.0f * Scale;
		const float LinkTextW = ShowLink ? std::max(std::max(MeasureCached(m_pTextRender, LinkSize, aProbeMaxText), MeasureCached(m_pTextRender, LinkSize, aSnapshotMaxText)),
			MeasureCached(m_pTextRender, LinkLatencySize, "9999 ms")) : 0.0f;
		const float LinkCellW = ShowLink ? (LinkPad + LinkDot + LinkGap + LinkTextW + LinkPad) : 0.0f;
		auto LinkColor = [&](float Alpha) -> ColorRGBA {
			if(!LinkKnown)
				return ColorRGBA(1.0f, 1.0f, 1.0f, 0.30f * Alpha);
			if(LinkLevel >= 2)
				return ColorRGBA(0.98f, 0.42f, 0.38f, Alpha);
			if(LinkLevel >= 1)
				return ColorRGBA(0.99f, 0.75f, 0.30f, Alpha);
			return ColorRGBA(0.42f, 0.88f, 0.55f, Alpha);
		};
		// Share the same fixed-width cell between idle and playing layouts.
		auto DrawLinkCell = [&](float CellX, float Alpha) {
			const float CY = PillY + Height * 0.5f;
			const float R = std::round(LinkDot * 0.5f);
			if(!LinkKnown)
				DrawShape(CellX + LinkPad, CY - R, R * 2.0f, R * 2.0f, R, IGraphics::CORNER_ALL,
					LinkColor(0.55f * Alpha), std::max(1.0f, 1.2f * Scale));
			else
			{
				const float HaloR = R + 2.0f * Scale;
				DrawShape(CellX + LinkPad + R - HaloR, CY - HaloR, HaloR * 2.0f, HaloR * 2.0f, HaloR,
					IGraphics::CORNER_ALL, LinkColor(0.16f * Alpha));
				DrawShape(CellX + LinkPad, CY - R, R * 2.0f, R * 2.0f, R, IGraphics::CORNER_ALL,
					LinkColor(0.95f * Alpha));
			}
			const float TextX = CellX + LinkPad + LinkDot + LinkGap;
			if(HasLinkWarning)
			{
				float RowY = PillY + (Height - LinkRowsH) * 0.5f;
				m_pTextRender->TextColor(1.0f, 1.0f, 1.0f, 0.78f * Alpha);
				if(HasProbeWarning)
				{
					m_pTextRender->Text(TextX, RowY, LinkSize, aProbeText);
					RowY += LinkRowStep;
				}
				if(HasSnapshotWarning)
					m_pTextRender->Text(TextX, RowY, LinkSize, aSnapshotText);
			}
			else
			{
				const float LatencyGlyphSize = LinkLatencySize * LinkFontScale;
				m_pTextRender->TextColor(1.0f, 1.0f, 1.0f, (HasLatency ? 0.72f : 0.42f) * Alpha);
				m_pTextRender->Text(TextX, PillY + (Height - LatencyGlyphSize) * 0.5f, LinkLatencySize, aLatencyText);
			}
			m_pTextRender->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
		};

		// ---- right zone: equaliser only -------------------------------------
		// The elapsed time used to live here, but the progress hairline along the
		// bottom edge already says how far into the track we are, so the number
		// was pure duplication. The column now carries just the bars.
		const int NumBars = 4;
		// 透明模式下没有黑胶囊撑底，柱子直接压在游戏画面上：3px 宽 + 0.35
		// alpha 在亮场景里几乎看不见（玩家反馈的"右边缺了"）。统一加宽，并靠
		// 下面的最小 alpha 保证任何背景下都能看清轮廓。
		const float EqBarW = 4.0f * Scale;
		const float EqBarGap = 3.0f * Scale;
		const float EqW = NumBars * EqBarW + (NumBars - 1) * EqBarGap;
		// 空态右端装饰的边长（正方形框，内接一个圆）。与播放态柱区同高。
		const float EqSize = 18.0f * Scale;

		// ---- layout: gauge cells split by hairline dividers -----------------
		// Each zone is a cell with its own horizontal padding; a 1 px rule sits on
		// the left edge of every cell but the first. This reads as a machined
		// panel rather than one long smear of text.
		const float CoverSize = 44.0f * Scale;
		const float ClockPad = 19.0f * Scale;          // padding inside the clock cell
		const float DiscPad = 15.0f * Scale;           // padding either side of the disc
		// 端帽是半径 = Height/2 的半圆，右边缘要留出整个端帽宽度再放柱子，
		// 否则柱子的右半截会被圆角裁掉（玩家反馈的"右边没了"）。
		const float EqPadR = Height * 0.5f + 8.0f * Scale;
		const float EqPadL = 18.0f * Scale;            // divider -> equaliser

		// fixed left/right chrome: the clock cell, the disc cell, the text gutters
		// and the equaliser cell on the right
		const float ClockCellW = ClockPad + GameW + ClockPad;
		const float DiscCellW = DiscPad + CoverSize + DiscPad;
		const float EqCellW = EqPadL + EqW + EqPadR;
		// TClient: 本地时间单元（歌词右侧、等化器左侧）。打开才占宽度。
		const bool ShowLocalClock = g_Config.m_TcIslandLocalClock != 0;
		char aLocalClock[8] = "";
		if(ShowLocalClock)
		{
			const std::time_t NowSec = std::time(nullptr);
			struct tm LocalTm;
#if defined(CONF_FAMILY_WINDOWS)
			localtime_s(&LocalTm, &NowSec);
#else
			localtime_r(&NowSec, &LocalTm);
#endif
			str_format(aLocalClock, sizeof(aLocalClock), "%02d:%02d", LocalTm.tm_hour, LocalTm.tm_min);
		}
		const float LocalClockTextW = ShowLocalClock ? MeasureCached(m_pTextRender, TimerSize, "00:00") : 0.0f;
		const float LocalClockCellW = ShowLocalClock ? (ClockPad + LocalClockTextW + ClockPad) : 0.0f;
		const float TextGapL = 14.0f * Scale;          // disc cell -> lyric text
		const float TextGapR = 20.0f * Scale;          // lyric text -> divider（与绘制侧保持一致）
		const float LeftChrome = LinkCellW + (ShowGameTime ? ClockCellW : 0.0f) + DiscCellW + TextGapL;
		const float RightChrome = TextGapR + EqCellW + (ShowLocalClock ? ClockPad + LocalClockTextW + ClockPad : 0.0f);
		const float ChromeW = LeftChrome + RightChrome;
		// the widest the capsule may get on this screen: keep it on-screen with a
		// comfortable margin, but never below the compact minimum.
		const float MaxPillW = std::min(ScreenW * 0.96f, (float)g_Config.m_TcIslandWidth * Scale);
		const float MinPillW = 180.0f * Scale;

		// 宽度固定：胶囊不再随歌词长短伸缩，换行时连宽度都不变，因此端帽纹理永远是
		// 同一张、整帧不需要任何新的烘焙。长歌词由下面的 FitText/marquee 在框内处理。
		// 打开本地时间等额外单元后，chrome 可能超过 MaxPillW 的可用量：
		// 宽度至少要容得下 chrome + 最小歌词区，否则右侧单元会被挤出胶囊。
		const float NeededW = ChromeW + 120.0f * Scale;
		// 空态且没有任何时钟要显示：干脆连胶囊都不画，不留一个空壳在屏幕顶上。
		if(Idle && !ShowGameTime && !ShowLocalClock && !ShowLink)
		{
			RestoreFace();
			return;
		}
		// 空态胶囊只包住时钟单元和右端的装饰等化器（两个时钟都开时并排，中间
		// 一条分隔线），宽度因此可以缩到远小于播放态的固定宽；播放态宽度仍按内容算。
		const float IdleW = LinkCellW + (ShowGameTime ? ClockCellW : 0.0f) + (ShowLocalClock ? LocalClockCellW : 0.0f) +
			EqPadL + EqSize + EqPadR;
		const float TargetWidth = Idle
			? std::floor(std::clamp(IdleW, MinPillW, ScreenW * 0.96f) / 8.0f) * 8.0f
			: std::floor(std::clamp(std::max(MaxPillW, NeededW), MinPillW, ScreenW * 0.96f) / 8.0f) * 8.0f;
		// 宽度只在目标值变化时平滑过渡（拖宽度滑条、进入/退出空态），平时恒定不动。
		// 按 8px 量化，一次过渡最多烘焙约十几张端帽纹理，不会冲爆形状缓存。
		if(m_AnimatedWidth < 0.0f)
			m_AnimatedWidth = TargetWidth;
		else
		{
			const float Dt = std::clamp(m_pClient->RenderFrameTime(), 0.0f, 0.1f);
			m_AnimatedWidth += (TargetWidth - m_AnimatedWidth) * (1.0f - std::exp(-Dt / 0.18f));
			if(std::fabs(TargetWidth - m_AnimatedWidth) < 0.5f)
				m_AnimatedWidth = TargetWidth;
		}
		const float Width = std::floor(m_AnimatedWidth / 8.0f) * 8.0f;

		// 夹住水平位置：偏移量很大时（滑条 ±600 还要乘缩放）胶囊会被推出屏幕，
		// 右端的圆头和等化器被裁掉，看起来就是"右边没了"。留 8px 边距。
		const float X = std::clamp((ScreenW - Width) * 0.5f + OffsetX, 8.0f, std::max(8.0f, ScreenW - Width - 8.0f));

		// ---- draw the capsule body ------------------------------------------
		// Gauge style: no halo, no glow. One inset hairline marks the rim, and the
		// body is a flat near-black panel. A 2 px progress rail runs along the
		// bottom edge, so the only colour in the pill is the karaoke green.
		//
		// The silhouette is baked into an RGBA texture with supersampled coverage
		// instead of using DrawRect's triangle fan, which has no antialiasing and
		// left the arcs visibly stair-stepped.
		// 透明模式完全不画任何底色、阴影或装饰，只保留可调的文字黑描边，
		// 内容直接叠在游戏画面上；不透明模式维持原胶囊面板不变。
		const bool TransparentMode = g_Config.m_TcIslandTransparent != 0;
		if(TransparentMode)
		{
			m_pTextRender->TextOutlineColor(0.0f, 0.0f, 0.0f, std::clamp((float)g_Config.m_TcIslandTransparentTextShadow / 100.0f, 0.0f, 1.0f));
		}
		else
		{
			DrawPanel(X, PillY, Width, Height, PillRounding, PillCorners, !NotchStyle,
				MakeGradient(X, PillY, Width, Height));
		}

		// 暂停是第三种状态：有曲目但没在播（m_ClockPlaying=false）。封面和歌名
		// 保留，让人看得出停在哪首；文字、进度轨、等化器一起压暗，一眼区分"停住"
		// 和"正在播"。
		const float Dim = m_ClockPlaying ? 1.0f : 0.55f;

		// ---- idle: clock cells + decorative equaliser ------------------------
		// 没放歌时音乐模块整块跳过，胶囊里只留时钟和右端那排静止的音柱——用户
		// 要求"右边那个跳动的保留，点缀一下"，但没歌时不跟随播放时钟，做成递增
		// 台阶的低亮造型，读作待机而不是"正在播放"。
		if(Idle)
		{
			const float SepTop = PillY + Height * 0.19f;
			const float SepH = Height * 0.62f;
			// 音柱保持静止的递增台阶。1.7.8.37 曾让它跟着本地玩家的 hook 频率跳，
			// 用户实测后又卡又吵，1.7.8.40 移除；这里只做右端点缀，不表达任何状态。
			auto DrawSep = [&](float SepX) {
				m_pGraphics->DrawRect(SepX, SepTop, 1.0f, SepH,
					ColorRGBA(1.0f, 1.0f, 1.0f, 0.08f), IGraphics::CORNER_NONE, 0.0f);
			};
			float CursorX = X + (Width - IdleW) * 0.5f;
			const float TextY = PillY + (Height - TimerSize) * 0.5f;
			if(ShowLink)
			{
				DrawLinkCell(CursorX, 1.0f);
				CursorX += LinkCellW;
				DrawSep(CursorX);
			}
			if(ShowGameTime)
			{
				m_pTextRender->TextColor(1.0f, 1.0f, 1.0f, 0.80f);
				m_pTextRender->Text(CursorX + ClockPad + GameTextOffset, TextY, TimerSize, aGameTime);
				m_pTextRender->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
				CursorX += ClockCellW;
				DrawSep(CursorX);
			}
			if(ShowLocalClock)
			{
				m_pTextRender->TextColor(1.0f, 1.0f, 1.0f, 0.92f);
				m_pTextRender->Text(CursorX + ClockPad, TextY, TimerSize, aLocalClock);
				m_pTextRender->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
				CursorX += LocalClockCellW;
				DrawSep(CursorX);
			}
			// 待机唱片：一枚 1 px 细环 + 中心轴孔。原来那四根递增音柱在 18 px 的
			// 小框里是四个独立形状，读起来像音量条/信号格；一个环加一个孔只有两层
			// 视觉信息，安静、有焦点，仍然是"这里平时放音乐"的隐喻。播放态的黑胶
			// 是实心盘 + 封面，这里是线稿，两者不会混。
			// 只有一个环：中心那个"轴孔"点在 18 px 里读不出唱片含义，反倒像个
			// 不明所以的高亮点，去掉。
			// 半径与圆心都吸附到整数像素：DrawShape 内部按 lround 贴图，半像素的
			// 圆心会让圆环边缘每帧落在不同的像素栅格上，看着像在抖。
			const float DiscR = std::round(EqSize * 0.5f);
			const float DiscCX = std::round(CursorX + EqPadL + DiscR);
			const float DiscCY = std::round(PillY + Height * 0.5f);
			// 透明模式下没有胶囊底撑腰，环线必须够粗、alpha 够高，否则直接压在
			// 亮场景上会消失（四柱当年就踩过这个"右边缺了"的坑）。
			DrawShape(DiscCX - DiscR, DiscCY - DiscR, DiscR * 2.0f, DiscR * 2.0f, DiscR,
				IGraphics::CORNER_ALL, AccentColor(0.70f), std::max(1.0f, 1.5f * Scale));
			RestoreFace();
			return;
		}

		// ---- bottom progress rail -------------------------------------------
		// A hairline of the track's elapsed fraction, clipped to the rounded body
		// so it follows the capsule silhouette instead of cutting across the
		// corners.
		//
		// The NetEase desktop client routinely reports Duration==0 over SMTC, so
		// m_ClockDurationMs alone leaves the rail empty for most tracks. Fall back
		// to the end of the last lyric line, which is a decent stand-in for the
		// track length whenever lyrics are available.
		// tc_island_show_song_progress 只管这条歌曲进度轨（对局时间走
		// tc_island_show_progress，两者互不牵连）。
		if(g_Config.m_TcIslandShowSongProgress)
		{
			int64_t TotalMs = m_ClockDurationMs;
			if(TotalMs <= 0)
			{
				for(const SLyricLine &L : m_vLines)
				{
					int64_t End = L.m_StartMs;
					for(const SWord &W : L.m_vWords)
						End = std::max(End, W.m_EndMs);
					TotalMs = std::max(TotalMs, End);
				}
			}

			const int64_t PosMs = CurrentPositionMs();
			// 旧版把 2px 硬线贴在胶囊底边,看起来像贴上去的划痕(用户:"很突兀,
			// 没有融合到灵动岛里面")。改成离底边 6px 的 3px 圆角小轨:空态也画
			// 轨道底,播放时高亮段从轨道左端长出来,读作岛自己的进度而不是贴条。
			const float RailH = 3.0f * Scale;
			const float RailY = PillY + Height - RailH - 6.0f * Scale;
			const float RailInset = NotchStyle ? Height * 0.30f : PillRounding;
			const float RailX = X + RailInset;
			const float RailW = std::max(0.0f, Width - RailInset * 2.0f);
			float Frac = 0.0f;
			if(TotalMs > 0)
				Frac = std::clamp((float)PosMs / (float)TotalMs, 0.0f, 1.0f);
			if(RailW > RailH)
			{
				const float Rounding = RailH * 0.5f;
				m_pGraphics->DrawRect(RailX, RailY, RailW, RailH,
					ColorRGBA(1.0f, 1.0f, 1.0f, 0.10f), IGraphics::CORNER_ALL, Rounding);
				if(Frac > 0.0f)
					m_pGraphics->DrawRect(RailX, RailY, std::max(RailH, RailW * Frac), RailH,
						AccentColor(0.9f * Dim), IGraphics::CORNER_ALL, Rounding);
			}
		}

		// ---- foreground content (drawn AFTER the body so it is not covered) ----

		// ---- cell dividers ---------------------------------------------------
		// Hairlines only span the middle 62% of the height so they read as seams
		// between cells rather than rules slicing the capsule in half.
		const float SepTop = PillY + Height * 0.19f;
		const float SepH = Height * 0.62f;
		auto DrawDivider = [&](float SepX) {
			m_pGraphics->DrawRect(SepX, SepTop, 1.0f, SepH,
				ColorRGBA(1.0f, 1.0f, 1.0f, 0.08f), IGraphics::CORNER_NONE, 0.0f);
		};

		float CellX = X;
		if(ShowLink)
		{
			DrawLinkCell(CellX, Dim);
			CellX += LinkCellW;
			DrawDivider(CellX);
		}
		if(ShowGameTime)
		{
			// game round time, dim white so it recedes next to the artwork
			m_pTextRender->TextColor(1.0f, 1.0f, 1.0f, 0.80f);
			m_pTextRender->Text(CellX + ClockPad + GameTextOffset, PillY + (Height - TimerSize) * 0.5f, TimerSize, aGameTime);
			m_pTextRender->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
			CellX += ClockCellW;
			DrawDivider(CellX);
		}

		// album cover shown as a spinning vinyl record: a black disc with the
		// artwork as the centre label, slowly rotating while music plays.
		const float DiscCellX = CellX;
		const float CoverX = DiscCellX + DiscPad;
		const float CoverY = PillY + (Height - CoverSize) * 0.5f;
		const float DiscCX = CoverX + CoverSize * 0.5f;
		const float DiscCY = CoverY + CoverSize * 0.5f;
		const float DiscR = CoverSize * 0.5f;
		// rotation angle: advance with wall time while playing, freeze when paused
		{
			const int64_t NowR = time_get();
			static int64_t s_LastRotTick = 0;
			if(s_LastRotTick == 0)
				s_LastRotTick = NowR;
			const double DtSec = (double)(NowR - s_LastRotTick) / (double)time_freq();
			s_LastRotTick = NowR;
			if(m_ClockPlaying)
				m_VinylAngle += (float)(DtSec * (2.0f * pi / 9.0f)); // one turn every 9s (网易云式慢转)
			if(m_VinylAngle > 2.0f * pi)
				m_VinylAngle -= 2.0f * pi;
		}
		// black vinyl disc base (also the fallback placeholder disc)
		DrawShape(CoverX, CoverY, CoverSize, CoverSize, DiscR, IGraphics::CORNER_ALL,
			ColorRGBA(0.06f, 0.06f, 0.07f, 1.0f));
		// subtle groove rings for the "record" look: 1 px rings, not filled
		// circles, so they read as grooves instead of washing out the whole disc.
		// 封面占盘面 ~78% 后，刻纹只在封面外的黑胶窄带里可见。
		for(int g = 1; g <= 3; g++)
		{
			const float RingR = DiscR * (0.855f + 0.045f * g);
			const float RingD = RingR * 2.0f;
			DrawShape(DiscCX - RingR, DiscCY - RingR, RingD, RingD, RingR, IGraphics::CORNER_ALL,
				ColorRGBA(1.0f, 1.0f, 1.0f, 0.05f), 1.0f);
		}
		if(HasCover)
		{
			// centre label: the artwork, circular (texture pre-masked), spinning with the record
			const float LabelD = CoverSize * 0.78f;
			const float LabelX = DiscCX - LabelD * 0.5f;
			const float LabelY = DiscCY - LabelD * 0.5f;
			m_pGraphics->TextureSet(m_CoverTexture);
			m_pGraphics->QuadsSetSubset(0.0f, 0.0f, 1.0f, 1.0f);
			const IGraphics::CQuadItem CoverQuad(LabelX, LabelY, LabelD, LabelD);
			m_pGraphics->QuadsBegin();
			m_pGraphics->QuadsSetRotation(m_VinylAngle);
			m_pGraphics->QuadsDrawTL(&CoverQuad, 1);
			m_pGraphics->QuadsEnd();
			m_pGraphics->TextureClear();
			m_pGraphics->QuadsSetRotation(0.0f);
			// vinyl spindle hole at the centre (covers the artwork's centre point)
			const float HoleD = CoverSize * 0.055f;
			DrawShape(DiscCX - HoleD * 0.5f, DiscCY - HoleD * 0.5f, HoleD, HoleD, HoleD * 0.5f, IGraphics::CORNER_ALL,
				ColorRGBA(0.06f, 0.06f, 0.07f, 1.0f));
		}
		else
		{
			// no artwork yet: soft placeholder ring in the middle of the disc
			const float InnerSize = CoverSize * 0.34f;
			DrawShape(DiscCX - InnerSize * 0.5f, DiscCY - InnerSize * 0.5f, InnerSize, InnerSize,
				InnerSize * 0.5f, IGraphics::CORNER_ALL, ColorRGBA(1.0f, 1.0f, 1.0f, 0.14f));
		}
		// centre spindle hole
		{
			const float HoleR = CoverSize * 0.045f;
			DrawShape(DiscCX - HoleR, DiscCY - HoleR, HoleR * 2.0f, HoleR * 2.0f,
				HoleR, IGraphics::CORNER_ALL, ColorRGBA(0.0f, 0.0f, 0.0f, 0.9f));
		}

		// ---- lyric / title column -------------------------------------------
		const float TextX = DiscCellX + DiscCellW + TextGapL;
		const float EqCellX = X + Width - EqCellW;
		const float LocalClockCellX = EqCellX - LocalClockCellW;
		// 歌词右侧留白用外层同一个 TextGapR（20px）：等化器左边还有 EqPadL，
		// 文字贴着分隔线时视觉上会把整块右侧"吃掉"。
		const float TextRight = (ShowLocalClock ? LocalClockCellX : EqCellX) - TextGapR;
		// 先画时间单元，再画等化器 —— 等化器在最右端，必须在最后覆盖绘制，
		// 否则会被后面绘制的内容盖住（之前"右边不见了"就是这个顺序问题）。
		if(ShowLocalClock)
		{
			DrawDivider(LocalClockCellX);
			m_pTextRender->TextColor(1.0f, 1.0f, 1.0f, 0.92f);
			m_pTextRender->Text(LocalClockCellX + ClockPad, PillY + (Height - TimerSize) * 0.5f, TimerSize, aLocalClock);
			m_pTextRender->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
		}
		DrawDivider(EqCellX);

		// ---- right cell: equaliser ------------------------------------------
		// Four bars, animated from the playback clock. No label, no numbers: the
		// bottom rail already carries the position, so this column is purely a
		// "is it alive" indicator.
		{
			const float EqH = 18.0f * Scale;
			const float EqY = PillY + (Height - EqH) * 0.5f;
			const double NowSec = (double)time_get() / (double)time_freq();
			for(int i = 0; i < NumBars; i++)
			{
				float Frac = 0.35f;
				if(m_ClockPlaying)
				{
					const double Phase = NowSec * 3.4 + i * 0.62;
					Frac = 0.28f + 0.72f * (float)(0.5 + 0.5 * std::sin(Phase));
				}
				const float BarH = std::max(EqH * 0.22f, EqH * Frac);
				// heights are whole pixels, so the shape cache holds at most one
				// entry per distinct bar height instead of growing every frame.
				// 2px 量化：高度每差 1px 就是一张新烘焙纹理，逐帧变化会把
				// 形状缓存冲爆（缓存满 64 条会整体清空重烤），也让每一帧都在烘焙。
				const float SnappedH = std::max(2.0f, std::round(BarH * 0.5f) * 2.0f);
				// 暂停态 0.35 在透明模式（无黑底）下几乎不可见，抬到 0.62；
				// 播放态保持 0.85，两者的差异仍能看出"在动/没在动"。
				const float BarAlpha = m_ClockPlaying ? 0.85f : 0.34f;
				DrawShape(EqCellX + EqPadL + i * (EqBarW + EqBarGap), EqY + (EqH - SnappedH), EqBarW, SnappedH,
					EqBarW * 0.5f, IGraphics::CORNER_ALL, AccentColor(BarAlpha));
			}
		}

		if(UseLyric)
		{
			// two rows: song title above, the karaoke line below
			const float MaxTextW = TextRight - TextX;
			str_copy(aLyricBuf, m_vLines[LyricIndex].m_Text.c_str());
			const SLyricLine &Line = m_vLines[LyricIndex];
			const bool HasWords = !Line.m_vWords.empty();

			// 逐帧文本缓存(2026-11-01):yrc 逐字歌词挂上后 GreenEndX 每帧对每个字
			// 做 substr 堆分配 + TextWidth 测量,是灵动岛掉帧主因(实测 DDNet 167%
			// CPU)。同一行文字不变 —— 按文本+字号缓存整行宽与逐字宽,换行/换曲/
			// 字号变化才重算,稳态每帧 0 次 TextWidth。
			if(m_KaraokeKey != Line.m_Text || m_KaraokeSize != LyricSize || (int)m_KaraokeWordW.size() != (int)Line.m_vWords.size() || m_KaraokeFullW < 0.0f)
			{
				m_KaraokeKey = Line.m_Text;
				m_KaraokeSize = LyricSize;
				m_KaraokeFullW = m_pTextRender->TextWidth(LyricSize, aLyricBuf);
				m_KaraokeWordW.assign(Line.m_vWords.size(), -1.0f);
				for(size_t wi = 0; wi < Line.m_vWords.size(); ++wi)
				{
					const SWord &W = Line.m_vWords[wi];
					if(W.m_CharBegin < 0 || W.m_CharBegin + W.m_CharLen > (int)Line.m_Text.size())
						continue;
					m_KaraokeWordW[wi] = m_pTextRender->TextWidth(LyricSize, Line.m_Text.substr(W.m_CharBegin, W.m_CharLen).c_str());
				}
			}
			const float FullW = m_KaraokeFullW;
			const float TitleGap = 6.0f * Scale;
			// centre the two-row block vertically instead of pinning it to a
			// fraction of the height, which left a big gap under the lyric.
			const float BlockH = TitleSize + TitleGap + LyricSize;
			const float TitleY = PillY + (Height - BlockH) * 0.5f;
			const float LyricY = TitleY + TitleSize + TitleGap;

			// small, dim title keeps the lyric as the loud element
			{
				if(m_TitleFitKey != aSong || m_TitleFitMaxW != MaxTextW || m_TitleFitText.empty())
				{
					char aTitleFit[512];
					str_copy(aTitleFit, aSong);
					FitText(m_pTextRender, TitleSize, aTitleFit, MaxTextW);
					m_TitleFitKey = aSong;
					m_TitleFitMaxW = MaxTextW;
					m_TitleFitText = aTitleFit;
				}
				m_pTextRender->TextColor(1.0f, 1.0f, 1.0f, 0.55f * Dim);
				m_pTextRender->Text(TextX, TitleY, TitleSize, m_TitleFitText.c_str());
			}

			const ColorRGBA ColorUnsung(1.0f, 1.0f, 1.0f, 0.95f * Dim);  // not yet sung -> white
			const ColorRGBA ColorSung = AccentColor(Dim);                 // sung / singing -> accent

			// Paint the current line at baseline X (already includes any marquee
			// offset). If the line carries per-character timings, do a smooth
			// left-to-right karaoke fill; otherwise fall back to whole-line colour.
			// Compute the exact x position where green fill currently ends so the
			// wipe is continuous (sub-character progress), not a hard per-char jump.
			auto GreenEndX = [&](float DrawX) -> float {
				// Karaoke fill lead: tc_island_karaoke_lead(默认 0,可负 = 落后)。
				const int64_t KaraokePosMs = LyricPosMs + g_Config.m_TcIslandKaraokeLead;
				if(!HasWords)
					return DrawX + (Line.m_StartMs > KaraokePosMs ? 0.0f : m_KaraokeFullW);
				float CurX = DrawX;
				for(size_t wi = 0; wi < Line.m_vWords.size(); ++wi)
				{
					const SWord &W = Line.m_vWords[wi];
					if(W.m_CharBegin < 0 || W.m_CharBegin + W.m_CharLen > (int)Line.m_Text.size())
						continue;
					const float CharW = m_KaraokeWordW[wi];
					if(KaraokePosMs < W.m_StartMs)
						return CurX; // not reached this char yet -> fill ends here
					if(KaraokePosMs >= W.m_EndMs)
					{
						CurX += CharW; // fully sung
						continue;
					}
					// inside this char: partial fill by time fraction
					const float Span = (float)(W.m_EndMs - W.m_StartMs);
					const float Frac = Span > 0.0f ? (float)(KaraokePosMs - W.m_StartMs) / Span : 1.0f;
					return CurX + CharW * std::clamp(Frac, 0.0f, 1.0f);
				}
				return CurX; // whole line sung
			};

			// ClipEnable takes device pixels. We now render in real pixels, so the
			// mapping is 1:1 and no rescale factor is needed (the old /300.0f here
			// was the leftover from the virtual-space coordinate system).
			const float SXc = 1.0f;
			const float SYc = 1.0f;
			const int ClipY = (int)(PillY * SYc);
			const int ClipH = (int)((Height + 2.0f) * SYc);

			// DrawLine draws the full line then overlays the sung portion in green.
			// WinLeft/WinRight bound the visible window (marquee); pass the line's
			// own span for the static case so nothing is clipped.
			auto DrawLine = [&](float DrawX, float WinLeft, float WinRight) {
				auto FillRect = [&](float L, float R, const ColorRGBA &Col) {
					if(R <= L + 0.5f)
						return false;
					m_pTextRender->TextColor(Col);
					m_pGraphics->ClipEnable((int)(L * SXc), ClipY, (int)((R - L) * SXc), ClipH);
					m_pTextRender->Text(DrawX, LyricY, LyricSize, aLyricBuf);
					m_pGraphics->ClipDisable();
					return true;
				};
				if(!HasWords)
				{
					const bool Future = Line.m_StartMs > LyricPosMs;
					// clip the whole line to the visible window (harmless when wide)
					m_pTextRender->TextColor(Future ? ColorUnsung : ColorSung);
					m_pGraphics->ClipEnable((int)(WinLeft * SXc), ClipY, (int)((WinRight - WinLeft) * SXc), ClipH);
					m_pTextRender->Text(DrawX, LyricY, LyricSize, aLyricBuf);
					m_pGraphics->ClipDisable();
					return;
				}
				// 1) white full line clipped to visible window
				FillRect(WinLeft, WinRight, ColorUnsung);
				// 2) green sung portion, clipped to [window ∩ [DrawX, GreenEnd])
				const float GreenEnd = GreenEndX(DrawX);
				const float GL = std::max(WinLeft, DrawX);
				const float GR = std::min(WinRight, GreenEnd);
				FillRect(GL, GR, ColorSung);
			};

			const int64_t NowTick = time_get();
			// track the displayed line so the marquee restarts on line change
			m_CurScrollText = aLyricBuf;
			if(FullW <= MaxTextW || MaxTextW <= 0.0f)
			{
				// fits: static, left aligned, no clipping window needed
				DrawLine(TextX, TextX, TextX + FullW + 2.0f);
			}
				else
				{
					// too long: marquee. restart the scroll whenever the line changes.
					if(m_LastScrollText != m_CurScrollText)
					{
						m_LastScrollText = m_CurScrollText;
						m_ScrollStartTick = NowTick;
					}
					const float Overflow = FullW - MaxTextW;
					// 滚动速度按"这句歌词的实际时长"自适应：几何固定速度会让长句在行内
					// 反复回卷（结尾只停 1 秒就跳回开头，2026-10-29 用户实测）。
					// 行时长 = 下一行开始 - 本行开始（最后一行用曲长兜底）；
					// 要求一个滚动周期不超过行时长，即速度 ≥ Overflow/(行时长-停留)，
					// 下限 28px/s 保底（行特别短时接受少量回卷），上限 140px/s 防闪滚。
					float LineDurSec = 6.0f; // 无数据时的保守缺省
					if(!m_vLines.empty())
					{
						const int64_t StartMs = m_vLines[LyricIndex].m_StartMs;
						int64_t EndMs;
						if(LyricIndex + 1 < (int)m_vLines.size())
							EndMs = m_vLines[LyricIndex + 1].m_StartMs;
						else if(m_ClockDurationMs > 0)
							EndMs = m_ClockDurationMs;
						else
							EndMs = StartMs + 8000;
						LineDurSec = std::max(2.0f, (float)((EndMs - StartMs) / 1000.0));
					}
					const float HoldTotal = 1.2f + 1.0f; // LeadDelay + EndHold
					const float AvailSec = std::max(1.5f, LineDurSec - HoldTotal);
					const float PxPerSec = std::clamp(Overflow / AvailSec, std::max(28.0f, 40.0f * Scale), 140.0f);
					const float LeadDelaySec = 1.2f;  // hold at the start before scrolling
					const float ElapsedSec = (float)((NowTick - m_ScrollStartTick) / (double)time_freq());
					// 不回卷：滚到行尾就停住（歌词不是跑马灯——滚回开头再看一遍没有意义，
					// 用户反馈的"回跳"即旧版 fmod 循环）。行结束时直接切下一行。
					float Offset;
					Offset = std::clamp((ElapsedSec - LeadDelaySec) * PxPerSec, 0.0f, Overflow);

				// the visible window is [TextX, TextX + MaxTextW]; DrawLine clips
				// the white line and the green fill to it, so no outer clip needed.
				DrawLine(TextX - Offset, TextX, TextX + MaxTextW);
			}
		}
		else
		{
			// no lyric: song title alone, vertically centred on the right
			const float MaxTextW = TextRight - TextX;
			if(m_SongFitKey != aSong || m_SongFitMaxW != MaxTextW || m_SongFitText.empty())
			{
				char aFitSong[512];
				str_copy(aFitSong, aSong);
				FitText(m_pTextRender, LyricSize, aFitSong, MaxTextW);
				m_SongFitKey = aSong;
				m_SongFitMaxW = MaxTextW;
				m_SongFitText = aFitSong;
			}
			m_pTextRender->TextColor(1.0f, 1.0f, 1.0f, 0.96f * Dim);
			m_pTextRender->Text(TextX, PillY + (Height - LyricSize) * 0.5f, LyricSize, m_SongFitText.c_str());
		}
		m_pTextRender->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
		if(TransparentMode)
			m_pTextRender->TextOutlineColor(m_pTextRender->DefaultTextOutlineColor());
		RestoreFace();
	}
};

void CIslandMusic::TogglePlayPause()
{
	EnsureImpl();
#if TCLIENT_HAS_WINRT_MUSIC
	std::lock_guard<std::mutex> Lock(m_pImpl->m_Mutex);
	m_pImpl->m_vPendingCommands.push_back(CImpl::EMediaCommand::TOGGLE_PLAY);
#endif
}

void CIslandMusic::NextTrack()
{
	EnsureImpl();
#if TCLIENT_HAS_WINRT_MUSIC
	std::lock_guard<std::mutex> Lock(m_pImpl->m_Mutex);
	m_pImpl->m_vPendingCommands.push_back(CImpl::EMediaCommand::NEXT);
#endif
}

void CIslandMusic::PrevTrack()
{
	EnsureImpl();
#if TCLIENT_HAS_WINRT_MUSIC
	std::lock_guard<std::mutex> Lock(m_pImpl->m_Mutex);
	m_pImpl->m_vPendingCommands.push_back(CImpl::EMediaCommand::PREV);
#endif
}

bool CIslandMusic::IsPlaying() const
{
	return m_pImpl && m_pImpl->IsPlayingCached();
}

void CIslandMusic::GetTrack(char *pTitle, int TitleSize, char *pArtist, int ArtistSize) const
{
	if(pTitle && TitleSize > 0)
		pTitle[0] = '\0';
	if(pArtist && ArtistSize > 0)
		pArtist[0] = '\0';
	if(!m_pImpl)
		return;
	m_pImpl->GetTrackCached(pTitle, TitleSize, pArtist, ArtistSize);
}

bool CIslandMusic::HasSession() const
{
	return m_pImpl && m_pImpl->HasSessionCached();
}

void CIslandMusic::EnsureImpl()
{
	if(!m_pImpl)
	{
		m_pImpl = new CImpl();
		m_pImpl->m_pClient = Client();
		m_pImpl->m_pGameClient = GameClient();
		m_pImpl->m_pHttp = Http();
		m_pImpl->m_pEngine = Engine();
		m_pImpl->m_pGraphics = Graphics();
		m_pImpl->m_pTextRender = TextRender();
	}
}

void CIslandMusic::OnInit()
{
	EnsureImpl();
#if TCLIENT_HAS_WINRT_MUSIC
	if(g_Config.m_TcIslandEnable)
		m_pImpl->StartWorker();
	m_pImpl->StartCoverDecodeThread();
#endif
}

void CIslandMusic::OnReset()
{
	if(m_pImpl)
		m_pImpl->ResetLyrics();
}

void CIslandMusic::OnRender()
{
	EnsureImpl();
#if TCLIENT_HAS_WINRT_MUSIC
	if(g_Config.m_TcIslandEnable)
		m_pImpl->StartWorker();
	m_pImpl->StartCoverDecodeThread();
	m_pImpl->Render();
#endif
}

void CIslandMusic::OnShutdown()
{
	if(m_pImpl)
		m_pImpl->ResetLyrics();
#if TCLIENT_HAS_WINRT_MUSIC
	if(m_pImpl)
		m_pImpl->StopCoverDecodeThread();
	if(m_pImpl)
		m_pImpl->StopWorker();
#endif
#if defined(CONF_FAMILY_WINDOWS)
	if(m_pImpl)
		m_pImpl->CloseIslandMap();
#endif
}
