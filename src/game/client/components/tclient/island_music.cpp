#include "island_music.h"

 #include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <cstring>
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

#include <engine/graphics.h>
#include <engine/http.h>
#include <engine/shared/config.h>
#include <engine/shared/json.h>
#include <engine/textrender.h>

#include <game/client/gameclient.h>

// stb_image: single-header decoder for PNG/JPEG/etc. Used for album covers
// returned by the Windows SMTC thumbnail API, which are typically JPEG and
// therefore cannot be decoded by the engine's PNG-only loader.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#include "stb_image.h"

#if defined(CONF_FAMILY_WINDOWS)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
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

class CIslandMusic::CImpl
{
public:
	IClient *m_pClient = nullptr;
	CGameClient *m_pGameClient = nullptr;
	IHttp *m_pHttp = nullptr;
	IGraphics *m_pGraphics = nullptr;
	ITextRender *m_pTextRender = nullptr;

	// lyric marquee scroll state
	std::string m_LastScrollText;
	std::string m_CurScrollText;
	int64_t m_ScrollStartTick = 0;
	// vinyl record spin angle (radians)
	float m_VinylAngle = 0.0f;

	~CImpl()
	{
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
				PollSessions(Manager, CurrentSessionId);
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

	std::mutex m_Mutex;
	std::condition_variable m_Cv;
	STrack m_Latest;

	// Album cover art. The SMTC worker downloads the current track's thumbnail
	// PNG bytes into m_CoverPng (guarded by m_Mutex) whenever the "cover key"
	// (service id + title + album) changes. The render thread watches m_CoverKey,
	// decodes the PNG into a texture once, and reuses it until the key changes.
	std::string m_CoverKey;             // key the worker publishes for
	std::string m_CoverFailedKey;       // key whose cover fetch already failed
	std::vector<uint8_t> m_CoverPng;    // raw PNG bytes for m_CoverKey
	bool m_CoverDirty = false;          // set by worker when new bytes arrive

	// render-thread side texture cache
	std::string m_LoadedCoverKey;       // key currently decoded into m_CoverTexture
	std::vector<uint8_t> m_LoadedPng;   // PNG bytes owned by the render thread
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
		int m_W, m_H, m_Radius100, m_Corners, m_Inset100;
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
			return m_Inset100 < O.m_Inset100;
		}
	};
	std::map<SShapeKey, IGraphics::CTextureHandle> m_ShapeCache;

	static float ShapeRadiusAt(float Px, float Py, float W, float H, float R, int Corners)
	{
		if(R <= 0.0f)
			return 0.0f;
		const bool Left = Px < R;
		const bool Right = Px > W - R;
		const bool Top = Py < R;
		const bool Bottom = Py > H - R;
		if(Left && Top)
			return (Corners & IGraphics::CORNER_TL) ? R : 0.0f;
		if(Right && Top)
			return (Corners & IGraphics::CORNER_TR) ? R : 0.0f;
		if(Left && Bottom)
			return (Corners & IGraphics::CORNER_BL) ? R : 0.0f;
		if(Right && Bottom)
			return (Corners & IGraphics::CORNER_BR) ? R : 0.0f;
		return 0.0f;
	}

	static bool ShapeCovers(float Px, float Py, float W, float H, float R, int Corners)
	{
		if(Px < 0.0f || Py < 0.0f || Px > W || Py > H)
			return false;
		const float RR = ShapeRadiusAt(Px, Py, W, H, R, Corners);
		if(RR <= 0.0f)
			return true;
		const float CX = Px < R ? RR : W - RR;
		const float CY = Py < R ? RR : H - RR;
		const float DX = Px - CX;
		const float DY = Py - CY;
		return DX * DX + DY * DY <= RR * RR;
	}

	IGraphics::CTextureHandle EnsureShapeTexture(int W, int H, float Rounding, int Corners, float Inset)
	{
		if(W <= 0 || H <= 0)
			return IGraphics::CTextureHandle();
		// 允许圆角半径达到短边全长：胶囊端部是宽 H/2 的半圆，其半径就是 H/2，
		// 旧的 min(W,H)*0.5 上限会把它压成 H/4，端部看起来不够圆。
		const float R = std::clamp(Rounding, 0.0f, (float)std::min(W, H));
		const SShapeKey Key{W, H, (int)std::lround(R * 100.0f), Corners, (int)std::lround(Inset * 100.0f)};
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

	// Rounded panel with a 1 px light rim. The rim is a full-size tinted blit and
	// the body is drawn 1 px smaller on top, so the outermost pixel stays visible
	// as a hairline that follows the antialiased silhouette.
	void DrawPanel(float X, float Y, float W, float H, float Rounding, int Corners, bool Rim)
	{
		const ColorRGBA Body(0.027f, 0.031f, 0.039f, 1.0f);
		if(Rim)
		{
			DrawPill(X, Y, W, H, Corners, ColorRGBA(1.0f, 1.0f, 1.0f, 0.07f));
			DrawPill(X + 1.0f, Y + 1.0f, W - 2.0f, H - 2.0f, Corners, Body);
		}
		else
		{
			DrawPill(X, Y, W, H, Corners, Body);
		}
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

		STARTUPINFOW Si;
		memset(&Si, 0, sizeof(Si));
		Si.cb = sizeof(Si);
		PROCESS_INFORMATION Pi;
		memset(&Pi, 0, sizeof(Pi));
		if(!CreateProcessW(aMonitorPath, nullptr, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &Si, &Pi))
			return;
		CloseHandle(Pi.hThread);
		CloseHandle(Pi.hProcess);
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
				return;
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

		// 位置来源（v7）：
		//  1) watcher（WatcherMs，最权威）：netease-watcher hook 的网易云内部真实位置，
		//     含静音段、seek 立即反映、暂停冻结 —— 歌词/进度条的位置本体。
		//  2) 音频钟（AudioMs）：watcher 失效（hook 挂/未启动）时的兜底 —— 静音段会慢，
		//     但至少与声音同步。
		//  3) LS 锚点 / 标题计时（老版网易云兜底）。
		const double NowEpochPos = (double)std::time(nullptr);
		if(Snap.WatcherMs > 0.0)
		{
			Out.m_DurationMs = Snap.WatcherDurMs > 0.0 ? (int64_t)Snap.WatcherDurMs :
				(Snap.DurationMs > 0.0 ? (int64_t)Snap.DurationMs : 0);
			int64_t Pos = (int64_t)Snap.WatcherMs;
			if(Snap.DurationMs > 0.0 && Pos > (int64_t)Snap.DurationMs + 1500)
				Pos = (int64_t)Snap.DurationMs;
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
			const double AnchorAge = NowEpochPos - Snap.FractionAt;
			const bool HaveLsAnchor = Snap.DurationMs > 0.0 && Snap.Fraction > 0.0 && AnchorAge >= -1.0 && AnchorAge < 15.0;
			if(HaveLsAnchor)
			{
				Out.m_DurationMs = (int64_t)Snap.DurationMs;
				int64_t Pos = (int64_t)(Snap.Fraction * Snap.DurationMs);
				if(Out.m_Playing && AnchorAge > 0.0)
					Pos += (int64_t)(AnchorAge * 1000.0);
				Out.m_PositionMs = std::max<int64_t>(0, Pos);
			}
			else
			{
				Out.m_DurationMs = Snap.DurationMs > 0.0 ? (int64_t)Snap.DurationMs : 0;
				Out.m_PositionMs = std::max<int64_t>(0, Snap.ElapsedMs);
			}
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

	// per-character karaoke timing, relative to the line start.
	struct SWord
	{
		int64_t m_StartMs = 0; // absolute start time of this char
		int64_t m_EndMs = 0;   // absolute end time (start of next char / line end)
		int m_CharBegin = 0;   // offset of the char in the line's UTF-8 text
		int m_CharLen = 0;     // UTF-8 byte length of the char
	};

	struct SLyricLine
	{
		int64_t m_StartMs = 0;
		std::string m_Text;
		// empty -> whole line turns green at m_StartMs (classic LRC).
		// non-empty -> netease karaoke: each char lights up at its own time.
		std::vector<SWord> m_vWords;
	};

	struct SCacheEntry
	{
		ELyricsState m_State = ELyricsState::NOTFOUND;
		std::vector<SLyricLine> m_vLines;
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
		Key += "v1|";
		Key += Track.m_Artist;
		Key += '|';
		Key += Track.m_Title;
		return Key;
	}

	static bool ParseLrcTimestamp(const char *pText, int64_t &OutMs, const char **ppEnd)
	{
		if(pText == nullptr || pText[0] != '[')
			return false;

		int Minutes = 0;
		int Seconds = 0;
		int Fraction = 0;
		int FractionDigits = 0;
		const char *p = pText + 1;
		if(!isdigit((unsigned char)*p))
			return false;

		while(isdigit((unsigned char)*p))
		{
			Minutes = Minutes * 10 + (*p - '0');
			++p;
		}
		if(*p != ':')
			return false;
		++p;
		if(!isdigit((unsigned char)*p))
			return false;
		while(isdigit((unsigned char)*p))
		{
			Seconds = Seconds * 10 + (*p - '0');
			++p;
		}
		if(*p == '.' || *p == ',')
		{
			++p;
			while(isdigit((unsigned char)*p) && FractionDigits < 3)
			{
				Fraction = Fraction * 10 + (*p - '0');
				++FractionDigits;
				++p;
			}
			while(isdigit((unsigned char)*p))
				++p;
		}
		if(*p != ']')
			return false;
		++p;

		while(FractionDigits > 0 && FractionDigits < 3)
		{
			Fraction *= 10;
			++FractionDigits;
		}

		OutMs = (int64_t)Minutes * 60000 + (int64_t)Seconds * 1000 + Fraction;
		if(ppEnd)
			*ppEnd = p;
		return true;
	}

	static bool ParseSyncedLyrics(const char *pSyncedLyrics, std::vector<SLyricLine> &vOut)
	{
		vOut.clear();
		if(pSyncedLyrics == nullptr || pSyncedLyrics[0] == '\0')
			return false;

		const char *p = pSyncedLyrics;
		std::vector<int64_t> vTimestamps;
		while(*p)
		{
			while(*p == '\r' || *p == '\n')
				++p;
			if(*p == '\0')
				break;

			const char *pLineStart = p;
			while(*p && *p != '\n' && *p != '\r')
				++p;
			const char *pLineEnd = p;

			vTimestamps.clear();
			const char *pCursor = pLineStart;
			while(pCursor < pLineEnd)
			{
				int64_t TimestampMs = 0;
				const char *pAfter = nullptr;
				if(!ParseLrcTimestamp(pCursor, TimestampMs, &pAfter) || pAfter == nullptr || pAfter > pLineEnd)
					break;
				vTimestamps.push_back(TimestampMs);
				pCursor = pAfter;
			}

			while(pCursor < pLineEnd && (*pCursor == ' ' || *pCursor == '\t'))
				++pCursor;

			if(vTimestamps.empty())
				continue;

			std::string Text(pCursor, pLineEnd);
			while(!Text.empty() && (Text.back() == ' ' || Text.back() == '\t'))
				Text.pop_back();
			if(Text.empty())
				continue;

			for(int64_t TimestampMs : vTimestamps)
				vOut.push_back({TimestampMs, Text});
		}

		if(vOut.empty())
			return false;

		std::stable_sort(vOut.begin(), vOut.end(), [](const SLyricLine &A, const SLyricLine &B) {
			return A.m_StartMs < B.m_StartMs;
		});

		// merge duplicate timestamps of the same line
		std::vector<SLyricLine> vMerged;
		vMerged.reserve(vOut.size());
		for(SLyricLine &Line : vOut)
		{
			if(!vMerged.empty() && vMerged.back().m_Text == Line.m_Text && Line.m_StartMs - vMerged.back().m_StartMs <= 150)
				continue;
			vMerged.push_back(std::move(Line));
		}
		vOut = std::move(vMerged);
		SynthesizeWordTimings(vOut);
		return !vOut.empty();
	}

	// No per-character timings available (plain LRC). Synthesize an even
	// per-character timeline across each line so we can still show a karaoke
	// wipe: every char of a line turns green in proportion to how far playback
	// has progressed through that line. Line duration = next line's start - this
	// line's start (capped to a sane maximum for the final / very long lines).
	static void SynthesizeWordTimings(std::vector<SLyricLine> &vLines)
	{
		for(size_t i = 0; i < vLines.size(); ++i)
		{
			SLyricLine &Line = vLines[i];
			if(!Line.m_vWords.empty())
				continue; // real yrc timings already present
			if(Line.m_Text.empty())
				continue;

			int64_t LineStart = Line.m_StartMs;
			int64_t LineEnd;
			if(i + 1 < vLines.size())
				LineEnd = vLines[i + 1].m_StartMs;
			else
				LineEnd = LineStart + 5000; // last line: assume 5s
			int64_t LineDur = LineEnd - LineStart;
			if(LineDur <= 0)
				LineDur = 3000; // degenerate: assume 3s
			if(LineDur > 15000)
				LineDur = 15000; // don't stretch silence gaps over the whole line

			// split into UTF-8 characters
			const std::string &Text = Line.m_Text;
			std::vector<std::pair<int, int>> vChars; // (byteOffset, byteLen)
			for(int Off = 0; Off < (int)Text.size();)
			{
				int Len = Utf8CharLen((unsigned char)Text[Off]);
				if(Off + Len > (int)Text.size())
					Len = 1;
				vChars.push_back({Off, Len});
				Off += Len;
			}
			if(vChars.empty())
				continue;

			const int N = (int)vChars.size();
			for(int k = 0; k < N; ++k)
			{
				SWord W;
				W.m_CharBegin = vChars[k].first;
				W.m_CharLen = vChars[k].second;
				W.m_StartMs = LineStart + LineDur * k / N;
				W.m_EndMs = LineStart + LineDur * (k + 1) / N;
				Line.m_vWords.push_back(W);
			}
		}
	}

	// ---- karaoke (per-character) lyrics ----
	// UTF-8 lead-byte -> sequence length. Invalid bytes are treated as 1.
	static int Utf8CharLen(unsigned char c)
	{
		if(c < 0x80)
			return 1;
		if((c & 0xE0) == 0xC0)
			return 2;
		if((c & 0xF0) == 0xE0)
			return 3;
		if((c & 0xF8) == 0xF0)
			return 4;
		return 1;
	}

	// Build one SWord per UTF-8 character from the raw timings produced by the
	// netease yrc format: each timing segment is (time_ms, duration_ms, text)
	// where `text` may span several characters. Every char inside a segment
	// shares the segment's start time.
	static void BuildLineWords(const std::string &Text, std::vector<SWord> &vOut,
		const std::vector<std::pair<int64_t, int64_t>> &vSegStartDur)
	{
		vOut.clear();
		(void)Text;
		(void)vSegStartDur;
	}

	static bool ParseYrcLyrics(const char *pYrc, std::vector<SLyricLine> &vOut)
	{
		vOut.clear();
		if(pYrc == nullptr || pYrc[0] == '\0')
			return false;

		const char *p = pYrc;
		while(*p)
		{
			while(*p == '\r' || *p == '\n')
				++p;
			if(*p == '\0')
				break;

			const char *pLineStart = p;
			while(*p && *p != '\n' && *p != '\r')
				++p;
			const char *pLineEnd = p;

			// line metadata: [start,duration](segStart,dur,0)text(segStart,dur,0)text...
			const char *pCursor = pLineStart;
			if(*pCursor != '[')
				continue;
			++pCursor;
			int64_t LineStart = 0, LineDur = 0;
			{
				int64_t Val = 0;
				bool Any = false;
				while(pCursor < pLineEnd && *pCursor != ',')
				{
					if(isdigit((unsigned char)*pCursor)) { Val = Val * 10 + (*pCursor - '0'); Any = true; }
					++pCursor;
				}
				if(!Any) continue;
				LineStart = Val;
				if(pCursor < pLineEnd && *pCursor == ',') ++pCursor;
				Val = 0;
				while(pCursor < pLineEnd && *pCursor != ']')
				{
					if(isdigit((unsigned char)*pCursor)) Val = Val * 10 + (*pCursor - '0');
					++pCursor;
				}
				LineDur = Val;
				if(pCursor < pLineEnd && *pCursor == ']') ++pCursor;
			}

			std::string Text;
			std::vector<SWord> vWords;
			// parse (segStart, segDur, 0)char(s) groups
			while(pCursor < pLineEnd)
			{
				if(*pCursor == '(')
				{
					++pCursor;
					int64_t SegStart = 0, SegDur = 0;
					int Field = 0;
					int64_t Val = 0;
					while(pCursor < pLineEnd && *pCursor != ')')
					{
						if(isdigit((unsigned char)*pCursor)) Val = Val * 10 + (*pCursor - '0');
						else if(*pCursor == ',')
						{
							if(Field == 0) SegStart = Val;
							else if(Field == 1) SegDur = Val;
							Val = 0;
							++Field;
						}
						++pCursor;
					}
					if(pCursor < pLineEnd) ++pCursor; // skip ')'
					// consume chars until next '(' or end
					int SegCharBegin = (int)Text.size();
					while(pCursor < pLineEnd && *pCursor != '(')
					{
						Text.push_back(*pCursor);
						++pCursor;
					}
					// split the just-added run into individual chars
					int i = SegCharBegin;
					int NumChars = 0;
					std::vector<int> vCharPos;
					while(i < (int)Text.size())
					{
						int Len = Utf8CharLen((unsigned char)Text[i]);
						if(i + Len > (int)Text.size()) Len = (int)Text.size() - i;
						vCharPos.push_back(i);
						i += Len;
						NumChars++;
					}
					for(int c = 0; c < NumChars; ++c)
					{
						SWord W;
						W.m_StartMs = SegStart;
						W.m_EndMs = SegStart + SegDur;
						W.m_CharBegin = vCharPos[c];
						int NextPos = (c + 1 < NumChars) ? vCharPos[c + 1] : (int)Text.size();
						W.m_CharLen = NextPos - vCharPos[c];
						vWords.push_back(W);
					}
				}
				else
				{
					// stray char outside a segment: keep as plain char, no timing
					Text.push_back(*pCursor);
					++pCursor;
				}
			}

			while(!Text.empty() && (Text.back() == ' ' || Text.back() == '\t'))
				Text.pop_back();
			if(Text.empty())
				continue;
			// trim leading whitespace (and its words)
			{
				int Lead = 0;
				while(Lead < (int)Text.size() && (Text[Lead] == ' ' || Text[Lead] == '\t'))
					Lead++;
				if(Lead > 0)
				{
					Text.erase(0, Lead);
					std::vector<SWord> vFixed;
					for(SWord &W : vWords)
					{
						if(W.m_CharBegin < Lead)
							continue;
						W.m_CharBegin -= Lead;
						vFixed.push_back(W);
					}
					vWords.swap(vFixed);
				}
			}
			if(Text.empty())
				continue;

			SLyricLine Line;
			Line.m_StartMs = LineStart;
			Line.m_Text = Text;
			Line.m_vWords = vWords;
			vOut.push_back(std::move(Line));
		}

		if(vOut.empty())
			return false;
		std::stable_sort(vOut.begin(), vOut.end(), [](const SLyricLine &A, const SLyricLine &B) {
			return A.m_StartMs < B.m_StartMs;
		});
		return true;
	}

	// media clock
	int64_t m_ClockPositionMs = 0;
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
		const bool SameSnapshot = (Snapshot == m_LastSnapshotMs);
		m_LastSnapshotMs = Snapshot;

		const bool FirstSync = m_ClockTick == 0;
		const bool StateChanged = Playing != m_ClockPlaying;

		if(TrustPosition)
		{
			// Real progress available: adopt it and restart the interpolation window
			// on every fresh snapshot so wall clock can't drift more than one poll.
			if(FirstSync || StateChanged || !SameSnapshot)
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
	// When lrclib has definitively missed a track we switch to NetEase and must
	// NOT bounce back to lrclib on the next frame (the two use different key
	// prefixes, which would otherwise abort each other forever). This records
	// the lrclib key that has been handed over to NetEase.
	std::string m_LrclibFailedKey;
	// Gate for the NetEase-first fallback: when NetEase misses, we fire exactly
	// one lrclib request per track (LOADING blocks re-entry until it resolves).
	ELyricsState m_LrclibState = ELyricsState::IDLE;
	std::string m_LrclibOwnedKey; // v1-cache key of the track lrclib is handling
	ELyricsState m_LyricsState = ELyricsState::IDLE;
	std::vector<SLyricLine> m_vLines;
	std::map<std::string, SCacheEntry> m_Cache;
	std::shared_ptr<IHttpRequest> m_pRequest;
	std::string m_RequestKey;

	void AbortRequest()
	{
		if(m_pRequest)
		{
			m_pRequest->Abort();
			m_pRequest.reset();
		}
		m_RequestKey.clear();
	}

	// ---------------- lrclib lyrics (foreign tracks / SMTC) ----------------
	void StartRequest(const STrack &Track)
	{
		if(m_pHttp == nullptr)
		{
			m_LyricsState = ELyricsState::OFFLINE;
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

		// IMPORTANT: use /api/search (fuzzy) rather than /api/get (exact match).
		// SMTC titles often carry suffixes ("(Live)", "- Remastered"), missing album,
		// or slightly different artist strings, which make the exact /api/get 404.
		// Search returns an array; ProcessRequest() picks the best synced entry.
		// We keep duration in the query to help ranking when available.
		char aUrl[2048];
		if(DurationSec >= 1 && DurationSec <= 3600)
			str_format(aUrl, sizeof(aUrl), "https://lrclib.net/api/search?track_name=%s&artist_name=%s&duration=%d",
				aEscapedTitle, aEscapedArtist, DurationSec);
		else
			str_format(aUrl, sizeof(aUrl), "https://lrclib.net/api/search?track_name=%s&artist_name=%s",
				aEscapedTitle, aEscapedArtist);

		m_pRequest = HttpGet(aUrl);
		m_pRequest->Timeout(CTimeout{10000, 0, 500, 10});
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

		std::shared_ptr<IHttpRequest> pFinished = m_pRequest;
		m_pRequest.reset();
		const std::string FinishedKey = m_RequestKey;
		m_RequestKey.clear();
		const bool ApplyToActive = !FinishedKey.empty() && FinishedKey == m_ActiveKey;

		if(pFinished->State() != EHttpState::DONE || pFinished->StatusCode() == 0)
		{
			if(ApplyToActive)
			{
				m_LyricsState = ELyricsState::OFFLINE;
				m_vLines.clear();
			}
			return;
		}

		const int StatusCode = pFinished->StatusCode();
		SCacheEntry Entry;
		Entry.m_State = ELyricsState::NOTFOUND;
		if(StatusCode >= 200 && StatusCode < 300)
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

		if(m_Cache.size() >= LYRICS_CACHE_MAX)
			m_Cache.clear();
		m_Cache[FinishedKey] = Entry;
		m_LrclibState = Entry.m_State == ELyricsState::IDLE ? ELyricsState::NOTFOUND : Entry.m_State;
		if(ApplyToActive)
		{
			m_LyricsState = Entry.m_State;
			m_vLines = Entry.m_vLines;
		}
	}

	void UpdateLyrics(const STrack &Track)
	{
		ProcessRequest();
		if(!Track.m_Valid || (Track.m_Title.empty() && Track.m_Artist.empty()))
		{
			if(!m_ActiveKey.empty())
			{
				m_ActiveKey.clear();
				m_LrclibFailedKey.clear();
				m_LrclibState = ELyricsState::IDLE;
				m_LyricsState = ELyricsState::IDLE;
				m_vLines.clear();
				AbortRequest();
				AbortNetease();
			}
			return;
		}

#if defined(CONF_FAMILY_WINDOWS)
		// The NetEase window-title monitor path always goes straight to NetEase.
		if(Track.m_ServiceId == "netease-title")
		{
			UpdateNetease(Track);
			return;
		}
#endif

		const std::string Key = BuildCacheKey(Track);

#if defined(CONF_FAMILY_WINDOWS)
		// NetEase-first: every track goes to NetEase CloudMusic first (it has the
		// best simplified-Chinese + word-by-word yrc coverage). Only if NetEase
		// definitively has no lyrics do we fall back to lrclib (StartRequest).
		// m_LrclibState tracks that fallback: IDLE means "NetEase is (or should be)
		// the source"; anything else (LOADING/READY/NOTFOUND) means lrclib owns the
		// display for m_LrclibOwnedKey. Any other track goes to NetEase first.
		if(m_LrclibState != ELyricsState::IDLE && m_LrclibOwnedKey == Key)
		{
			// lrclib owns this track — keep driving its request pipeline.
			if(m_ActiveKey != Key)
			{
				AbortRequest();
				AbortNetease();
				m_ActiveKey = Key;
				m_vLines.clear();
				ResetMediaClock();
				SyncMediaClock(Track.m_PositionMs, Track.m_DurationMs, Track.m_Playing);
				StartRequest(Track);
			}
			else
			{
				SyncMediaClock(Track.m_PositionMs, Track.m_DurationMs, Track.m_Playing);
				ProcessRequest();
			}
			return;
		}
		// new track (or first): NetEase owns it now, reset any lrclib ownership.
		if(m_LrclibOwnedKey != Key)
		{
			m_LrclibState = ELyricsState::IDLE;
			m_LrclibOwnedKey.clear();
		}

		UpdateNetease(Track);
		return;
#else
		if(Key == m_LrclibFailedKey)
		{
			return;
		}

		if(Key != m_ActiveKey)
		{
			AbortRequest();
			AbortNetease();
			m_ActiveKey = Key;
			m_vLines.clear();
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
		ELyricsState m_LyricState = ELyricsState::IDLE;
		std::vector<SLyricLine> m_vLines;
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
		bool m_TriedQQ = false;      // QQ 流程已走完（成功或失败）
		std::string m_QQMid;         // 搜索到的 songmid
	};

	std::map<std::string, SNeteaseMeta> m_NeteaseData;
	std::string m_NeteaseInFlight; // cache key currently being resolved
	bool m_NeteaseInLyricPhase = false;
	bool m_NeteaseInCoverPhase = false;
	bool m_NeteaseInDetailPhase = false;
	std::shared_ptr<IHttpRequest> m_pNeteaseRequest;
	std::shared_ptr<IHttpRequest> m_pQQRequest;
	bool m_QQInSearch = false;  // QQ 搜索阶段进行中
	bool m_QQInLyric = false;   // QQ 歌词阶段进行中

	void AbortNetease()
	{
		if(m_pNeteaseRequest)
		{
			m_pNeteaseRequest->Abort();
			m_pNeteaseRequest.reset();
		}
		if(m_pQQRequest)
		{
			m_pQQRequest->Abort();
			m_pQQRequest.reset();
		}
		m_NeteaseInFlight.clear();
		m_NeteaseInLyricPhase = false;
		m_NeteaseInCoverPhase = false;
		m_NeteaseInDetailPhase = false;
		m_QQInSearch = false;
		m_QQInLyric = false;
	}

	static std::string NeteaseCacheKey(const STrack &Track)
	{
		std::string Key;
		Key.reserve(160);
		Key += "n163|";
		Key += Track.m_Artist;
		Key += '|';
		Key += Track.m_Title;
		return Key;
	}

	void UpdateNetease(const STrack &Track)
	{
		const std::string Key = NeteaseCacheKey(Track);
		if(Key != m_ActiveKey)
		{
			AbortRequest();
			AbortNetease();
			m_ActiveKey = Key;
			m_vLines.clear();
			ResetMediaClock();
			// 切歌：清掉上一首的曲长，防止新歌 resolve 完成前旧曲长参与重锚换算
			if(m_pIslandView)
				*(volatile double *)((uint8_t *)m_pIslandView + offsetof(SIslandShm, ClientDurationMs)) = 0.0;
		}

		SNeteaseMeta &Meta = m_NeteaseData[Key];
		Meta.m_Title = Track.m_Title;
		Meta.m_Artist = Track.m_Artist;
		// v6: 单字段原子回传曲长给 monitor（seek 重锚把 Fraction 换算成秒用）。
		// 必须只写这一个字段——整结构回写会与 monitor 50ms 一次的 SHM 刷新竞争、覆盖它的数据。
		if(m_pIslandView && Meta.m_DurationMs > 0)
			*(volatile double *)((uint8_t *)m_pIslandView + offsetof(SIslandShm, ClientDurationMs)) = (double)Meta.m_DurationMs;
		// 2026-10-29 起位置来源升级：monitor 的 WASAPI 进程环回提供真实"已渲染毫秒"（AudioMs），
		// 暂停自动冻结、seek 由 monitor 侧 LS 重锚修正 —— 它是真进度，不再走"纯墙钟自计时"。
		// DurationMs>0（LS 给了曲长）时 SyncMediaClock 采信绝对位置并逐轮重锚；
		// DurationMs==0 时自动退回旧的自计时分支（含暂停冻结），行为不变。
		SyncMediaClock(Track.m_PositionMs, Track.m_DurationMs, Track.m_Playing);

		// apply cached/finished data
		if(Meta.m_LyricState == ELyricsState::READY || Meta.m_LyricState == ELyricsState::NOTFOUND)
		{
			m_LyricsState = Meta.m_LyricState;
			m_vLines = Meta.m_vLines;
		}
		else if(m_NeteaseInFlight == Key)
		{
			m_LyricsState = ELyricsState::LOADING;
		}

		// start resolution when idle and untried
		if(m_pNeteaseRequest || Meta.m_TriedLyric)
		{
			// If lyrics are resolved but the cover still hasn't been fetched, kick
			// the cover download here too. ProcessNetease only chains phases on the
			// frame a request completes; a lyrics-READY track served from the cache
			// would otherwise never get its cover requested.
			if(false && !m_pNeteaseRequest && m_pHttp != nullptr && Meta.m_Resolved && Meta.m_SongId != 0 && Meta.m_TriedLyric && Meta.m_PicUrl.empty() && !Meta.m_DetailQueued && !m_NeteaseInDetailPhase && !m_NeteaseInCoverPhase)
			{
				// lyrics came from cache: resolve album.picUrl via song/detail first.
				char aUrl[512];
				str_format(aUrl, sizeof(aUrl), "https://music.163.com/api/song/detail?ids=[%lld]", (long long)Meta.m_SongId);
				m_pNeteaseRequest = HttpGet(aUrl);
				m_pNeteaseRequest->Timeout(CTimeout{10000, 0, 500, 10});
				m_pNeteaseRequest->LogProgress(HTTPLOG::FAILURE);
				m_pNeteaseRequest->FailOnErrorStatus(false);
				m_NeteaseInFlight = Key;
				m_NeteaseInDetailPhase = true;
				m_pHttp->Run(m_pNeteaseRequest);
			}
			else if(!m_pNeteaseRequest && m_pHttp != nullptr && Meta.m_Resolved && Meta.m_TriedLyric && !Meta.m_CoverQueued && !Meta.m_PicUrl.empty() && !m_NeteaseInCoverPhase)
			{
				if(Meta.m_PicUrl.rfind("http", 0) != 0)
				{
					Meta.m_CoverQueued = true;
				}
				else
				{
					m_pNeteaseRequest = HttpGet(Meta.m_PicUrl.c_str());
					m_pNeteaseRequest->Timeout(CTimeout{10000, 0, 500, 10});
					m_pNeteaseRequest->LogProgress(HTTPLOG::FAILURE);
					m_pNeteaseRequest->FailOnErrorStatus(false);
					m_NeteaseInFlight = Key;
					m_NeteaseInCoverPhase = true;
					m_pHttp->Run(m_pNeteaseRequest);
				}
			}
			ProcessNetease();
			ProcessQQ();
			return;
		}

		if(m_pHttp == nullptr)
		{
			m_LyricsState = ELyricsState::OFFLINE;
			return;
		}

		// phase 1: search song (yields id + duration)
		char aEscapedQuery[768];
		std::string Query = Track.m_Title;
		if(!Track.m_Artist.empty())
		{
			Query += ' ';
			Query += Track.m_Artist;
		}
		EscapeUrl(aEscapedQuery, Query.c_str());

		char aUrl[1400];
		str_format(aUrl, sizeof(aUrl), "https://music.163.com/api/search/get/web?s=%s&type=1&limit=20", aEscapedQuery);

		m_pNeteaseRequest = HttpGet(aUrl);
		m_pNeteaseRequest->Timeout(CTimeout{10000, 0, 500, 10});
		m_pNeteaseRequest->LogProgress(HTTPLOG::FAILURE);
		m_pNeteaseRequest->FailOnErrorStatus(false);
		m_NeteaseInFlight = Key;
		m_NeteaseInLyricPhase = false;
		m_pHttp->Run(m_pNeteaseRequest);
		m_LyricsState = ELyricsState::LOADING;
	}

	void ProcessNetease()
	{
		if(!m_pNeteaseRequest || !m_pNeteaseRequest->Done())
			return;

		std::shared_ptr<IHttpRequest> pFinished = m_pNeteaseRequest;
		m_pNeteaseRequest.reset();
		const std::string Key = m_NeteaseInFlight;
		m_NeteaseInFlight.clear();
		if(Key.empty() || m_NeteaseData.find(Key) == m_NeteaseData.end())
			return;

		SNeteaseMeta &Meta = m_NeteaseData[Key];
		const bool ApplyToActive = Key == m_ActiveKey;

		// detail phase: resolve album.picUrl that the search response omitted.
		if(m_NeteaseInDetailPhase)
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
						Meta.m_PicUrl = pPic->u.string.ptr;
				}
				if(pRoot != nullptr && pRoot != &json_value_none)
					json_value_free(pRoot);
			}
			else
			{
			}
			m_NeteaseInDetailPhase = false;
			Meta.m_DetailQueued = true;
			return;
		}

		// cover phase: raw image bytes (not JSON). Store into the shared cover
		// buffer used by the render thread's UpdateCoverTexture().
		if(m_NeteaseInCoverPhase)
		{
			if(pFinished->State() == EHttpState::DONE && pFinished->StatusCode() >= 200 && pFinished->StatusCode() < 300)
			{
				unsigned char *pResultPtr = nullptr;
				size_t ResultLen = 0;
				pFinished->Result(&pResultPtr, &ResultLen);
				if(pResultPtr != nullptr && ResultLen > 0)
				{
					std::lock_guard<std::mutex> Lock(m_Mutex);
					m_CoverPng.assign(pResultPtr, pResultPtr + ResultLen);
					m_CoverKey = "163|" + Meta.m_Title;
					m_CoverDirty = true;
				}
			}
			else
			{
			}
			m_NeteaseInCoverPhase = false;
			Meta.m_CoverQueued = true;
			if(ApplyToActive && !m_NeteaseInFlight.empty() && Key == m_ActiveKey)
				m_LyricsState = Meta.m_LyricState;
			return;
		}

		if(pFinished->State() == EHttpState::DONE && pFinished->StatusCode() >= 200 && pFinished->StatusCode() < 300)
		{
			json_value *pRoot = pFinished->ResultJson();
			if(pRoot != nullptr && pRoot != &json_value_none && pRoot->type == json_object)
			{
				if(!m_NeteaseInLyricPhase)
				{
					// search response: pick the version (studio / live / remix / cover) that matches the
					// title in the netease window. Blindly taking songs[0] handed a (Live版) track the
					// studio lyrics, and netease search happily returns covers.
					const json_value *pResult = json_object_get(pRoot, "result");
					const json_value *pSongs = pResult ? json_object_get(pResult, "songs") : nullptr;
					const json_value *pBestSong = nullptr;
					int BestScore = -1000000;

					static const char *s_apLiveTags[] = {"live", "现场", "演唱会", "concert", "unplugged"};
					static const char *s_apRemixTags[] = {"remix", "混音", "dj版", "抖音"};
					static const char *s_apInstTags[] = {"伴奏", "instrumental", "karaoke"};
					static const char *s_apAcousticTags[] = {"acoustic", "不插电"};
					static const char *s_apCoverTags[] = {"翻唱", "cover", "深情版", "女声版", "男声版", "童声版", "钢琴版", "纯音乐"};

					auto ContainsAny = [](const std::string &Name, const char *const *ppTags, size_t NumTags) {
						std::string Lower;
						Lower.reserve(Name.size());
						for(char c : Name) Lower.push_back((char)tolower((unsigned char)c));
						for(size_t i = 0; i < NumTags; ++i)
							if(Lower.find(ppTags[i]) != std::string::npos) return true;
						return false;
					};
					// 把 "(Live版)" / "（女声）" / "[remix]" 这类括号段整段去掉，剩下的才是"歌名本体"。
					// 网易云标题花样多："思念 (Live)" vs "思念 - (Live)" vs "思念（live）"，只有比本体才稳。
					auto StripBrackets = [](const std::string &Name) {
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
						// 去掉空白，中文歌名里空格无意义，还顺手统一中英文标题的差异
						std::string Compact;
						Compact.reserve(Out.size());
						for(char c : Out)
						{
							if(c == ' ' || c == '\t' || c == '-')
								continue;
							Compact.push_back((char)tolower((unsigned char)c));
						}
						return Compact;
					};

					const bool WantLive = ContainsAny(Meta.m_Title, s_apLiveTags, sizeof(s_apLiveTags) / sizeof(s_apLiveTags[0]));
					const bool WantRemix = ContainsAny(Meta.m_Title, s_apRemixTags, sizeof(s_apRemixTags) / sizeof(s_apRemixTags[0]));
					const bool WantAcoustic = ContainsAny(Meta.m_Title, s_apAcousticTags, sizeof(s_apAcousticTags) / sizeof(s_apAcousticTags[0]));
					const bool WantCover = ContainsAny(Meta.m_Title, s_apCoverTags, sizeof(s_apCoverTags) / sizeof(s_apCoverTags[0]));
					const std::string WantBase = StripBrackets(Meta.m_Title);

					if(pSongs != nullptr && pSongs->type == json_array)
					{
						for(unsigned i = 0; i < pSongs->u.array.length; ++i)
						{
							const json_value *pSong = pSongs->u.array.values[i];
							if(pSong == nullptr || pSong->type != json_object) continue;
							const json_value *pId = json_object_get(pSong, "id");
							if(pId == nullptr || pId->type != json_integer) continue;
							const json_value *pName = json_object_get(pSong, "name");
							const json_value *pDur = json_object_get(pSong, "duration");
							const char *pNameText = (pName != nullptr && pName->type == json_string) ? pName->u.string.ptr : "";
							const std::string CandidateName(pNameText);
							const std::string CandidateBase = StripBrackets(CandidateName);

							const bool IsLive = ContainsAny(CandidateName, s_apLiveTags, sizeof(s_apLiveTags) / sizeof(s_apLiveTags[0]));
							const bool IsRemix = ContainsAny(CandidateName, s_apRemixTags, sizeof(s_apRemixTags) / sizeof(s_apRemixTags[0]));
							const bool IsAcoustic = ContainsAny(CandidateName, s_apAcousticTags, sizeof(s_apAcousticTags) / sizeof(s_apAcousticTags[0]));
							const bool IsInst = ContainsAny(CandidateName, s_apInstTags, sizeof(s_apInstTags) / sizeof(s_apInstTags[0]));
							const bool IsCover = ContainsAny(CandidateName, s_apCoverTags, sizeof(s_apCoverTags) / sizeof(s_apCoverTags[0]));

							// 歌名本体先过闸门：完全一致 1000，包含关系 400，完全不搭 -1200
							int Score;
							if(!WantBase.empty() && CandidateBase == WantBase)
								Score = 1000;
							else if(!WantBase.empty() && (CandidateBase.find(WantBase) != std::string::npos || WantBase.find(CandidateBase) != std::string::npos))
								Score = 400;
							else
								Score = -1200;

							// 版本类型是硬闸门，权重必须压过歌名——"原版"和"Live 版"歌名本体一模一样，
							// 光看歌名永远分不出来，这正是之前串词的原因。
							if(WantLive != IsLive)
								Score -= 1500;
							else if(WantLive)
								Score += 400;
							if(WantRemix != IsRemix)
								Score -= 600;
							else if(WantRemix)
								Score += 200;
							if(WantAcoustic && !IsAcoustic)
								Score -= 300;
							if(IsInst && !WantRemix)
								Score -= 800;
							if(IsCover && !WantCover)
								Score -= 700;
							else if(IsCover && WantCover)
								Score += 150;

							// 歌手：完全一致 600，包含 300，不一样 -500（翻唱署名不同、串烧合唱也算不一样）
							if(!Meta.m_Artist.empty())
							{
								const json_value *pArtists = json_object_get(pSong, "artists");
								if(pArtists != nullptr && pArtists->type == json_array && pArtists->u.array.length > 0)
								{
									const json_value *pFirst = pArtists->u.array.values[0];
									const json_value *pArtistName = (pFirst != nullptr && pFirst->type == json_object) ? json_object_get(pFirst, "name") : nullptr;
									if(pArtistName != nullptr && pArtistName->type == json_string)
									{
										const std::string CandidateArtist(pArtistName->u.string.ptr);
										if(CandidateArtist == Meta.m_Artist)
											Score += 600;
										else if(!CandidateArtist.empty() && (CandidateArtist.find(Meta.m_Artist) != std::string::npos || Meta.m_Artist.find(CandidateArtist) != std::string::npos))
											Score += 300;
										else
											Score -= 500;
									}
								}
							}

							// 曲长只在 LS 给了真实时长时才有意义（monitor 现在会填 DurationMs 了）
							if(pDur != nullptr && pDur->type == json_integer && Meta.m_DurationMs > 0)
							{
								int64_t DurDiff = std::llabs((long long)(pDur->u.integer - Meta.m_DurationMs)) / 10;
								if(DurDiff > 300)
									DurDiff = 300;
								Score -= (int)DurDiff;
							}
							if(Score > BestScore) { BestScore = Score; pBestSong = pSong; }
						}
					}
					if(pBestSong != nullptr)
					{
						const json_value *pId = json_object_get(pBestSong, "id");
						const json_value *pDur = json_object_get(pBestSong, "duration");
						Meta.m_Resolved = true; Meta.m_SongId = pId->u.integer;
						if(pDur != nullptr && pDur->type == json_integer) Meta.m_DurationMs = std::max<int64_t>(0, pDur->u.integer);
						// scrape album cover url (album.picUrl) straight from the search hit
						const json_value *pAlbum = json_object_get(pBestSong, "album");
						const json_value *pPic = pAlbum ? json_object_get(pAlbum, "picUrl") : nullptr;
						if(pPic != nullptr && pPic->type == json_string && pPic->u.string.length > 0)
							Meta.m_PicUrl = pPic->u.string.ptr;
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
							Ok = true;
						}
					}
					if(!Ok && LrcOk)
					{
						Meta.m_vLines = Meta.m_vFallbackLines;
						Ok = true;
					}

					if(Ok)
						Meta.m_LyricState = ELyricsState::READY;
					else
					{
						Meta.m_LyricState = ELyricsState::NOTFOUND;
						// NetEase 没有歌词 → 发起 QQ 音乐搜索（第三源，两阶段）
						if(!Meta.m_TriedQQ && m_pHttp != nullptr)
						{
							char aEscT[512], aEscA[512];
							EscapeUrl(aEscT, Meta.m_Title.c_str());
							EscapeUrl(aEscA, Meta.m_Artist.c_str());
							char aQQUrl[2048];
							str_format(aQQUrl, sizeof(aQQUrl),
								"https://c.y.qq.com/soso/fcgi-bin/client_search_cp?format=json&n=3&w=%s%%20%s",
								aEscT, aEscA);
							m_pQQRequest = HttpGet(aQQUrl);
							m_pQQRequest->Timeout(CTimeout{10000, 0, 500, 10});
							m_pQQRequest->LogProgress(HTTPLOG::FAILURE);
							m_pQQRequest->HeaderString("Referer", "https://y.qq.com/");
							m_QQInSearch = true;
						}
					}
					Meta.m_TriedLyric = true;
					m_NeteaseInLyricPhase = false;
				}
			}
			if(pRoot != nullptr && pRoot != &json_value_none)
				json_value_free(pRoot);
		}

		// chain: search -> lyric request (only if not already tried)
		if(!m_NeteaseInLyricPhase && Meta.m_Resolved && !Meta.m_TriedLyric && m_pHttp != nullptr)
		{
			char aUrl[512];
			str_format(aUrl, sizeof(aUrl), "https://music.163.com/api/song/lyric?id=%lld&lv=-1&tv=-1&yv=-1", (long long)Meta.m_SongId);
			m_pNeteaseRequest = HttpGet(aUrl);
			m_pNeteaseRequest->Timeout(CTimeout{10000, 0, 500, 10});
			m_pNeteaseRequest->LogProgress(HTTPLOG::FAILURE);
			m_pNeteaseRequest->FailOnErrorStatus(false);
			m_NeteaseInFlight = Key;
			m_NeteaseInLyricPhase = true;
			m_pHttp->Run(m_pNeteaseRequest);
		}
		else if(false)
		{
			// DISABLED-FOR-CRASHTEST: detail phase off
			char aUrl[512];
			str_format(aUrl, sizeof(aUrl), "https://music.163.com/api/song/detail?ids=[%lld]", (long long)Meta.m_SongId);
			m_pNeteaseRequest = HttpGet(aUrl);
			m_pNeteaseRequest->Timeout(CTimeout{10000, 0, 500, 10});
			m_pNeteaseRequest->LogProgress(HTTPLOG::FAILURE);
			m_pNeteaseRequest->FailOnErrorStatus(false);
			m_NeteaseInFlight = Key;
			m_NeteaseInDetailPhase = true;
			m_pHttp->Run(m_pNeteaseRequest);
		}
		else if(!m_NeteaseInCoverPhase && Meta.m_Resolved && Meta.m_TriedLyric && !Meta.m_CoverQueued && !Meta.m_PicUrl.empty() && m_pHttp != nullptr)
		{
			// cover request: download the album art we scraped from the search hit.
			if(Meta.m_PicUrl.rfind("http", 0) != 0)
			{
				Meta.m_CoverQueued = true; // not a plain http url, skip
			}
			else
			{
				m_pNeteaseRequest = HttpGet(Meta.m_PicUrl.c_str());
				m_pNeteaseRequest->Timeout(CTimeout{10000, 0, 500, 10});
				m_pNeteaseRequest->LogProgress(HTTPLOG::FAILURE);
				m_pNeteaseRequest->FailOnErrorStatus(false);
				m_NeteaseInFlight = Key;
				m_NeteaseInCoverPhase = true;
				m_pHttp->Run(m_pNeteaseRequest);
			}
		}
		else if(m_NeteaseInLyricPhase && !Meta.m_TriedLyric && m_pNeteaseRequest)
		{
			// A lyric request is still in flight (m_pNeteaseRequest set) but not
			// done yet — do NOT mark OFFLINE here. To reach this branch the request
			// must have completed, so m_pNeteaseRequest would be null; guard on it.
			Meta.m_TriedLyric = true;
		}
		else if(!m_NeteaseInLyricPhase && !Meta.m_Resolved)
		{
			// search failed (no in-flight request and no song id)
			Meta.m_TriedLyric = true;
		}

		if(m_NeteaseData.size() >= NETEASE_META_MAX)
			m_NeteaseData.clear();
		if(ApplyToActive && !m_NeteaseInFlight.empty() && Key == m_ActiveKey)
			m_LyricsState = Meta.m_LyricState;

		// NetEase-first fallback: if NetEase definitively has no lyrics for this
		// track, hand it to lrclib so we still show something.
		if(ApplyToActive && Meta.m_TriedLyric && Meta.m_TriedQQ && Meta.m_LyricState == ELyricsState::NOTFOUND && !m_NeteaseInLyricPhase && !m_NeteaseInCoverPhase && !m_pNeteaseRequest && !m_pQQRequest && !m_QQInSearch && !m_QQInLyric && m_pRequest == nullptr && m_LrclibState == ELyricsState::IDLE)
		{
			// hand the track over to lrclib. Switch the active key to the lrclib
			// cache key so the next UpdateLyrics frame drives the lrclib pipeline.
			STrack Fallback;
			Fallback.m_Valid = true;
			Fallback.m_Title = Meta.m_Title;
			Fallback.m_Artist = Meta.m_Artist;
			Fallback.m_DurationMs = Meta.m_DurationMs;
			m_LrclibState = ELyricsState::LOADING;
			m_LrclibOwnedKey = BuildCacheKey(Fallback);
			m_ActiveKey = m_LrclibOwnedKey;
			StartRequest(Fallback);
		}
	}
#endif

	// ---------------- QQ 音乐第三源（NetEase NOTFOUND 后、lrclib 前） ----------------
	// 两阶段：搜索(w=标题 歌手)拿 songmid → 歌词接口拿明文 LRC。解析复用 ParseSyncedLyrics。
	void ProcessQQ()
	{
		if(!m_pQQRequest || !m_pQQRequest->Done())
			return;
		std::shared_ptr<IHttpRequest> pFinished = m_pQQRequest;
		m_pQQRequest.reset();
		const bool WasSearch = m_QQInSearch;
		m_QQInSearch = false;
		m_QQInLyric = false;
		auto It = m_NeteaseData.find(m_NeteaseInFlight);
		if(It == m_NeteaseData.end())
			return; // 已切歌，丢弃
		SNeteaseMeta &Meta = It->second;
		if(Meta.m_TriedQQ)
			return;

		json_value *pRoot = pFinished->ResultJson();
		if(pRoot != nullptr && pRoot != &json_value_none)
		{
			if(WasSearch)
			{
				// {"code":0,"data":{"song":{"list":[{...,"songmid":"xxx",...}]}}}
				const json_value *pData = json_object_get(pRoot, "data");
				const json_value *pSong = pData ? json_object_get(pData, "song") : nullptr;
				const json_value *pList = pSong ? json_object_get(pSong, "list") : nullptr;
				const json_value *pFirst = (pList != nullptr && pList->type == json_array && pList->u.array.length > 0)
					? pList->u.array.values[0] : nullptr;
				const json_value *pMid = pFirst ? json_object_get(pFirst, "songmid") : nullptr;
				if(pMid != nullptr && pMid->type == json_string && pMid->u.string.length > 0)
				{
					Meta.m_QQMid = pMid->u.string.ptr;
					// 第二阶段：拿歌词
					char aUrl[512];
					str_format(aUrl, sizeof(aUrl),
						"https://c.y.qq.com/lyric/fcgi-bin/fcg_query_lyric_new.fcg?format=json&nobase64=1&g_tk=5381&loginUin=0&hostUin=0&platform=yqq&songmid=%s",
						Meta.m_QQMid.c_str());
					m_pQQRequest = HttpGet(aUrl);
					m_pQQRequest->Timeout(CTimeout{10000, 0, 500, 10});
					m_pQQRequest->LogProgress(HTTPLOG::FAILURE);
					m_pQQRequest->HeaderString("Referer", "https://y.qq.com/");
					m_QQInLyric = true;
				}
				else
					Meta.m_TriedQQ = true; // 搜索无结果 → 放行 lrclib
			}
			else
			{
				// 歌词响应：{"code":0,"lyric":"[mm:ss.xx]..."}
				const json_value *pLyric = json_object_get(pRoot, "lyric");
				const char *pLyricText = (pLyric != nullptr && pLyric->type == json_string) ? pLyric->u.string.ptr : "";
				std::vector<SLyricLine> vLines;
				if(pLyricText[0] != '\0' && ParseSyncedLyrics(pLyricText, vLines) && !vLines.empty())
				{
					Meta.m_vLines = std::move(vLines);
					Meta.m_vFallbackLines.clear();
					Meta.m_LyricState = ELyricsState::READY;
					Meta.m_Resolved = true;
				}
				Meta.m_TriedQQ = true; // 成功或失败都收口，失败放行 lrclib
			}
		}
		else
			Meta.m_TriedQQ = true; // 网络失败也放行 lrclib
		if(pRoot != nullptr && pRoot != &json_value_none)
			json_value_free(pRoot);
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
		str_time((int64_t)Time * 100, ETimeFormat::DAYS, pBuf, BufSize);
	}

	// Ensure the GPU texture matches the worker's current cover PNG. Re-decodes
	// only when the published cover key changed. Returns true when m_CoverTexture
	// holds a valid texture ready to draw this frame.
	bool UpdateCoverTexture()
	{
		std::string Key;
		bool Dirty = false;
		{
			std::lock_guard<std::mutex> Lock(m_Mutex);
			Key = m_CoverKey;
			if(m_CoverDirty)
			{
				m_CoverDirty = false;
				Dirty = true;
				m_LoadedPng.swap(m_CoverPng); // take ownership of the fresh bytes
			}
			else if(Key != m_LoadedCoverKey)
			{
				// key changed but no new bytes queued (e.g. a track with no cover):
				// drop the old texture so we don't show the wrong art.
				Dirty = true;
				m_LoadedPng.clear();
			}
		}
		if(!Dirty)
			return m_CoverTextureValid;
		if(m_CoverTextureValid)
		{
			m_pGraphics->UnloadTexture(&m_CoverTexture);
			m_CoverTextureValid = false;
		}
		m_LoadedCoverKey = Key;
		if(m_LoadedPng.empty())
		{
			m_LastTexResult = "empty-png len=" + std::to_string((int)m_LoadedPng.size());
			return false;
		}
		int IW = 0, IH = 0, IChannels = 0;
		unsigned char *pPixels = stbi_load_from_memory(m_LoadedPng.data(), (int)m_LoadedPng.size(), &IW, &IH, &IChannels, 4);
		if(!pPixels || IW <= 0 || IH <= 0)
		{
			m_LastTexResult = "stbi-FAIL bytes=" + std::to_string((int)m_LoadedPng.size()) + " err=" + std::string(stbi_failure_reason() ? stbi_failure_reason() : "?");
			stbi_image_free(pPixels);
			return false;
		}
		CImageInfo Image;
		Image.m_Width = (size_t)IW;
		Image.m_Height = (size_t)IH;
		Image.m_Format = CImageInfo::FORMAT_RGBA;
		Image.Allocate();
		memcpy(Image.m_pData, pPixels, (size_t)IW * (size_t)IH * 4);
		stbi_image_free(pPixels);
		// 网易云黑胶样式：封面裁成圆形（边缘 2px 抗锯齿），黑胶边从圆外露出
		{
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
					unsigned char &Alpha = pPx[(y * IW + x) * 4 + 3];
					if(Edge <= 0.0f)
						Alpha = 0;
					else if(Edge < 2.0f)
						Alpha = (unsigned char)(Alpha * (Edge / 2.0f));
				}
			}
		}
		m_CoverTexture = m_pGraphics->LoadTextureRawMove(Image, 0, "island-cover");
		m_CoverTextureValid = m_CoverTexture.IsValid();
		m_LastTexResult = "decoded " + std::to_string(IW) + "x" + std::to_string(IH) + " valid=" + std::to_string((int)m_CoverTextureValid);
		return m_CoverTextureValid;
	}

	void Render()
	{
		if(m_pClient->State() != IClient::STATE_ONLINE && m_pClient->State() != IClient::STATE_DEMOPLAYBACK)
			return;
		if(!g_Config.m_TcIslandEnable)
			return;

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

		// ---- iOS Dynamic Island black capsule style -------------------------
		// A pure-black pill glued to the very top edge of the screen, horizontally
		// centred, with only the two bottom corners rounded (top edge sits flush
		// against the screen top). Content is either the song title or a single
		// karaoke lyric line, whichever the user has enabled.

		const ColorRGBA Black(0.0f, 0.0f, 0.0f, 1.0f);
		const ColorRGBA White(1.0f, 1.0f, 1.0f, 1.0f);
		const ColorRGBA Green(0.12f, 0.84f, 0.38f, 1.0f);

		// idle: no track playing. Collapse to a compact capsule that shows only the
		// game round timer; with no timer either (menus, demo playback, a server
		// without a limit) fall back to a small black dot.
		if(!Track.m_Valid)
		{
			const float IdleTimerSize = 16.0f * Scale;
			const float IdlePad = 20.0f * Scale;
			char aIdleTime[32];
			GetGameTimeString(aIdleTime, sizeof(aIdleTime));
			const bool ShowIdleTime = g_Config.m_TcIslandShowProgress && aIdleTime[0] != '\0';

			float IdleW, IdleH;
			if(ShowIdleTime)
			{
				// 固定宽度：计时器数字宽度会随时间变化（1:09 -> 1:10），跟着它伸缩
				// 每次都要换一张端帽纹理。这里按 8px 网格取一个不会变的上限宽度。
				IdleW = std::ceil((m_pTextRender->TextWidth(IdleTimerSize, "00:00:00") + IdlePad * 2.0f) / 8.0f) * 8.0f;
				IdleH = Height;
			}
			else
			{
				IdleW = 40.0f * Scale;
				IdleH = Height * 0.42f;
			}

			const float IdleX = (ScreenW - IdleW) * 0.5f + OffsetX;
			if(g_Config.m_TcIslandTransparent == 0)
				DrawPanel(IdleX, PillY, IdleW, IdleH, PillRounding, PillCorners, false);
			if(ShowIdleTime)
			{
				const float IdleTextW = m_pTextRender->TextWidth(IdleTimerSize, aIdleTime);
				m_pTextRender->TextColor(1.0f, 1.0f, 1.0f, 0.92f);
				m_pTextRender->Text((int)(IdleX + (IdleW - IdleTextW) * 0.5f),
					(int)(PillY + (IdleH - IdleTimerSize) * 0.5f),
					IdleTimerSize, aIdleTime);
			}
			return;
		}

		// content strings (artist intentionally not shown)
		char aSong[512];
		if(!Track.m_Title.empty())
			str_copy(aSong, Track.m_Title.c_str());
		else
			str_copy(aSong, Track.m_Artist.c_str());

		// sizes: real pixel values at the 72 px reference height, so on a 1080p
		// screen the text renderer gets exactly integer font sizes and the glyphs
		// stay crisp. Anything derived from Height*0.x lands on half pixels.
		const float TitleSize = 12.0f * Scale;
		const float LyricSize = 19.0f * Scale;
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
		if(ShowGameTime)
			GameW = m_pTextRender->TextWidth(TimerSize, aGameTime);

		// ---- right zone: equaliser only -------------------------------------
		// The elapsed time used to live here, but the progress hairline along the
		// bottom edge already says how far into the track we are, so the number
		// was pure duplication. The column now carries just the bars.
		const int NumBars = 4;
		const float EqBarW = 3.0f * Scale;
		const float EqBarGap = 3.0f * Scale;
		const float EqW = NumBars * EqBarW + (NumBars - 1) * EqBarGap;

		// ---- layout: gauge cells split by hairline dividers -----------------
		// Each zone is a cell with its own horizontal padding; a 1 px rule sits on
		// the left edge of every cell but the first. This reads as a machined
		// panel rather than one long smear of text.
		const float CoverSize = 44.0f * Scale;
		const float ClockPad = 19.0f * Scale;          // padding inside the clock cell
		const float DiscPad = 15.0f * Scale;           // padding either side of the disc
		const float EqPadL = 18.0f * Scale;            // divider -> equaliser
		const float EqPadR = 20.0f * Scale;            // equaliser -> right edge

		// measure the text column width. The capsule is dynamic: it grows to
		// fit the whole lyric/title line so we never crop the tail. Only when a
		// line is longer than the on-screen cap do we fall back to marquee scroll.
		char aMeasure[512];
		float FullTextW;
		if(UseLyric)
		{
			str_copy(aMeasure, aLyricBuf);
			FullTextW = m_pTextRender->TextWidth(LyricSize, aMeasure);
		}
		else
		{
			str_copy(aMeasure, aSong);
			FullTextW = m_pTextRender->TextWidth(TitleSize, aMeasure);
		}

		// fixed left/right chrome: the clock cell, the disc cell, the text gutters
		// and the equaliser cell on the right
		const float ClockCellW = ClockPad + GameW + ClockPad;
		const float DiscCellW = DiscPad + CoverSize + DiscPad;
		const float EqCellW = EqPadL + EqW + EqPadR;
		const float TextGapL = 14.0f * Scale;          // disc cell -> lyric text
		const float TextGapR = 14.0f * Scale;          // lyric text -> divider
		const float LeftChrome = (ShowGameTime ? ClockCellW : 0.0f) + DiscCellW + TextGapL;
		const float RightChrome = TextGapR + EqCellW;
		const float ChromeW = LeftChrome + RightChrome;
		// the widest the capsule may get on this screen: keep it on-screen with a
		// comfortable margin, but never below the compact minimum.
		const float MaxPillW = std::min(ScreenW * 0.78f, 560.0f * Scale);
		const float MinPillW = 180.0f * Scale;

		const float TextW = std::max(0.0f, std::min(FullTextW, MaxPillW - ChromeW));
		// 宽度固定：胶囊不再随歌词长短伸缩，换行时连宽度都不变，因此端帽纹理永远是
		// 同一张、整帧不需要任何新的烘焙。长歌词由下面的 FitText/marquee 在框内处理。
		(void)TextW;
		const float Width = std::floor(std::clamp(MaxPillW, MinPillW, MaxPillW) / 8.0f) * 8.0f;

		const float X = (ScreenW - Width) * 0.5f + OffsetX;

		// ---- draw the capsule body ------------------------------------------
		// Gauge style: no halo, no glow. One inset hairline marks the rim, and the
		// body is a flat near-black panel. A 2 px progress rail runs along the
		// bottom edge, so the only colour in the pill is the karaoke green.
		//
		// The silhouette is baked into an RGBA texture with supersampled coverage
		// instead of using DrawRect's triangle fan, which has no antialiasing and
		// left the arcs visibly stair-stepped.
		// 透明模式：只画内容，不画黑色胶囊底（适合叠在游戏画面上或自己加遮罩）。
		// 没有黑底后白字压在亮画面上会糊，所以透明模式给文字加一圈黑描边。
		const bool TransparentMode = g_Config.m_TcIslandTransparent != 0;
		if(TransparentMode)
			m_pTextRender->TextOutlineColor(0.0f, 0.0f, 0.0f, std::clamp((float)g_Config.m_TcIslandTransparentTextShadow / 100.0f, 0.0f, 1.0f));
		if(!TransparentMode)
			DrawPanel(X, PillY, Width, Height, PillRounding, PillCorners, !NotchStyle);

		// ---- bottom progress rail -------------------------------------------
		// A hairline of the track's elapsed fraction, clipped to the rounded body
		// so it follows the capsule silhouette instead of cutting across the
		// corners.
		//
		// The NetEase desktop client routinely reports Duration==0 over SMTC, so
		// m_ClockDurationMs alone leaves the rail empty for most tracks. Fall back
		// to the end of the last lyric line, which is a decent stand-in for the
		// track length whenever lyrics are available.
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
			const float RailH = 2.0f * Scale;
			const float RailY = PillY + Height - RailH;
			// The rail must not run into the bottom corner curves: clipping it to
			// the rounded body ate its first and last few pixels, which read as an
			// empty gap at the left end. Start and end it where the corner arcs
			// finish instead, so the line is flush against the panel edge.
			const float RailInset = NotchStyle ? Height * 0.30f : PillRounding;
			const float RailX = X + RailInset;
			const float RailW = std::max(0.0f, Width - RailInset * 2.0f);
			float Frac = 0.0f;
			if(TotalMs > 0)
				Frac = std::clamp((float)PosMs / (float)TotalMs, 0.0f, 1.0f);
			if(Frac > 0.0f && RailW > 0.0f)
			{
				m_pGraphics->ClipEnable((int)X, (int)RailY, (int)Width, (int)std::ceil(RailH) + 1);
				m_pGraphics->DrawRect(RailX, RailY, RailW, RailH,
					ColorRGBA(1.0f, 1.0f, 1.0f, 0.08f), IGraphics::CORNER_NONE, 0.0f);
				m_pGraphics->DrawRect(RailX, RailY, RailW * Frac, RailH,
					ColorRGBA(0.118f, 0.843f, 0.376f, 0.9f), IGraphics::CORNER_NONE, 0.0f);
				m_pGraphics->ClipDisable();
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
		if(ShowGameTime)
		{
			// game round time, dim white so it recedes next to the artwork
			m_pTextRender->TextColor(1.0f, 1.0f, 1.0f, 0.80f);
			m_pTextRender->Text(CellX + ClockPad, PillY + (Height - TimerSize) * 0.5f, TimerSize, aGameTime);
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
		const float TextRight = EqCellX - TextGapR;
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
				DrawShape(EqCellX + EqPadL + i * (EqBarW + EqBarGap), EqY + (EqH - SnappedH), EqBarW, SnappedH,
					EqBarW * 0.5f, IGraphics::CORNER_ALL,
					ColorRGBA(0.12f, 0.84f, 0.38f, m_ClockPlaying ? 0.85f : 0.35f));
			}
		}

		if(UseLyric)
		{
			// two rows: song title above, the karaoke line below
			const float MaxTextW = TextRight - TextX;
			str_copy(aLyricBuf, m_vLines[LyricIndex].m_Text.c_str());
			const float FullW = m_pTextRender->TextWidth(LyricSize, aLyricBuf);
			const float TitleGap = 2.0f * Scale;
			// centre the two-row block vertically instead of pinning it to a
			// fraction of the height, which left a big gap under the lyric.
			const float BlockH = TitleSize + TitleGap + LyricSize;
			const float TitleY = PillY + (Height - BlockH) * 0.5f;
			const float LyricY = TitleY + TitleSize + TitleGap;

			// small, dim title keeps the lyric as the loud element
			{
				char aTitleFit[512];
				str_copy(aTitleFit, aSong);
				FitText(m_pTextRender, TitleSize, aTitleFit, MaxTextW);
				m_pTextRender->TextColor(1.0f, 1.0f, 1.0f, 0.55f);
				m_pTextRender->Text(TextX, TitleY, TitleSize, aTitleFit);
			}

			const ColorRGBA ColorUnsung(1.0f, 1.0f, 1.0f, 0.95f);       // not yet sung -> white
			const ColorRGBA ColorSung(0.12f, 0.84f, 0.38f, 1.0f);        // sung / singing -> green

			const SLyricLine &Line = m_vLines[LyricIndex];
			const bool HasWords = !Line.m_vWords.empty();

			// Paint the current line at baseline X (already includes any marquee
			// offset). If the line carries per-character timings, do a smooth
			// left-to-right karaoke fill; otherwise fall back to whole-line colour.
			// Compute the exact x position where green fill currently ends so the
			// wipe is continuous (sub-character progress), not a hard per-char jump.
			auto GreenEndX = [&](float DrawX) -> float {
				// Karaoke fill runs a touch ahead of the lyric line so the green
				// wipe leads the current syllable slightly (feels snappier).
				const int64_t KaraokePosMs = LyricPosMs + g_Config.m_TcIslandKaraokeLead;
				if(!HasWords)
					return DrawX + (Line.m_StartMs > KaraokePosMs ? 0.0f : m_pTextRender->TextWidth(LyricSize, aLyricBuf));
				const std::string &Text = Line.m_Text;
				float CurX = DrawX;
				for(const SWord &W : Line.m_vWords)
				{
					if(W.m_CharBegin < 0 || W.m_CharBegin + W.m_CharLen > (int)Text.size())
						continue;
					const std::string Sub = Text.substr(W.m_CharBegin, W.m_CharLen);
					const float CharW = m_pTextRender->TextWidth(LyricSize, Sub.c_str());
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
			char aFitSong[512];
			str_copy(aFitSong, aSong);
			FitText(m_pTextRender, LyricSize, aFitSong, MaxTextW);
			m_pTextRender->TextColor(1.0f, 1.0f, 1.0f, 0.96f);
			m_pTextRender->Text(TextX, PillY + (Height - LyricSize) * 0.5f, LyricSize, aFitSong);
		}
		m_pTextRender->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
		if(TransparentMode)
			m_pTextRender->TextOutlineColor(m_pTextRender->DefaultTextOutlineColor());
	}
};

void CIslandMusic::EnsureImpl()
{
	if(!m_pImpl)
	{
		m_pImpl = new CImpl();
		m_pImpl->m_pClient = Client();
		m_pImpl->m_pGameClient = GameClient();
		m_pImpl->m_pHttp = Http();
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
#endif
}

void CIslandMusic::OnReset()
{
}

void CIslandMusic::OnRender()
{
	EnsureImpl();
#if TCLIENT_HAS_WINRT_MUSIC
	if(g_Config.m_TcIslandEnable)
		m_pImpl->StartWorker();
	m_pImpl->Render();
#endif
}

void CIslandMusic::OnShutdown()
{
	if(m_pImpl)
		m_pImpl->StopWorker();
#if defined(CONF_FAMILY_WINDOWS)
	if(m_pImpl)
		m_pImpl->CloseIslandMap();
#endif
}
