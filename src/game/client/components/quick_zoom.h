#ifndef GAME_CLIENT_COMPONENTS_QUICK_ZOOM_H
#define GAME_CLIENT_COMPONENTS_QUICK_ZOOM_H

namespace tclient_quick_zoom
{
	inline float SelectTarget(bool QuickZoomActive, float DefaultZoom, float QuickZoom)
	{
		return QuickZoomActive ? DefaultZoom : QuickZoom;
	}
}

#endif
