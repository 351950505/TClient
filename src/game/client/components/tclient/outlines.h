#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_OUTLINES_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_OUTLINES_H

#include <game/client/component.h>
#include <game/map/freeze_outline_geometry.h>

class CTile;
class CTeleTile;

class COutlines : public CComponent
{
private:
	ivec2 m_MapDataSize;
	int *m_pMapData = nullptr;

public:
	int Sizeof() const override { return sizeof(*this); }
	void OnMapLoad() override;
	CFreezeOutlineMapView FreezeOutlineMap() const { return {m_pMapData, m_MapDataSize.x, m_MapDataSize.y}; }
	void OnRender() override;
	~COutlines() override { delete[] m_pMapData; }
};

#endif
