#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_ISLAND_MUSIC_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_ISLAND_MUSIC_H

#include <game/client/component.h>

// TClient: Dynamic Island (notch) style system music + synced lyrics display.
// Reads the active Windows media session (SMTC) and shows title/artist plus
// synced lyrics fetched from lrclib.net inside a capsule at the top.
class CIslandMusic : public CComponent
{
public:
	int Sizeof() const override { return sizeof(*this); }

	void OnInit() override;
	void OnReset() override;
	void OnRender() override;
	void OnShutdown() override;

	// 媒体传输控制（计分板左侧面板用）。命令只入队，由 SMTC worker 线程执行。
	void TogglePlayPause();
	void NextTrack();
	void PrevTrack();
	// 系统音量（0~100，作用于默认播放设备）。读取失败返回 -1。
	void SetVolume(int Percent);
	int GetVolume() const;
	// 当前是否正在播放（无歌曲时返回 false）。
	bool IsPlaying() const;
	// 当前曲目名与歌手，无歌曲时为空串。
	void GetTrack(char *pTitle, int TitleSize, char *pArtist, int ArtistSize) const;
	// 是否检测到可控制的播放器会话。
	bool HasSession() const;

private:
	class CImpl;
	CImpl *m_pImpl = nullptr;

	void EnsureImpl();
};

#endif
