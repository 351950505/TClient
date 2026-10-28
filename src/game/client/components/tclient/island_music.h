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

private:
	class CImpl;
	CImpl *m_pImpl = nullptr;

	void EnsureImpl();
};

#endif
