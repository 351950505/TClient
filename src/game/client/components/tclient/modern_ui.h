#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_MODERN_UI_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_MODERN_UI_H

#include <base/color.h>
#include <base/math.h>
#include <base/time.h>

#include <engine/shared/config.h>

#include <algorithm>
#include <cmath>

namespace TClientModernUi
{
	inline ColorRGBA Accent(float X = 0.0f, float Y = 0.0f)
	{
		const float Seconds = (time_get_nanoseconds().count() % 3600000000000LL) / 1000000000.0f;
		const float Phase = Seconds * g_Config.m_TcUiAccentSpeed / 100.0f;
		if(g_Config.m_TcUiAccentMode == 3)
			return color_cast<ColorRGBA>(ColorHSLA(std::fmod(Phase * 0.1f, 1.0f), 0.8f, 0.65f));
		const ColorRGBA First = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_TcUiAccent));
		if(g_Config.m_TcUiAccentMode == 0)
			return First;
		const ColorRGBA Second = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_TcUiAccent2));
		const float Angle = g_Config.m_TcUiAccentAngle * pi / 180.0f + (g_Config.m_TcUiAccentMode == 2 ? Phase : 0.0f);
		const float Position = X * std::cos(Angle) / 300.0f + Y * std::sin(Angle) / 300.0f;
		const float Blend = std::clamp(0.5f + 0.5f * std::sin(Position * pi), 0.0f, 1.0f);
		return ColorRGBA(mix(First.r, Second.r, Blend), mix(First.g, Second.g, Blend), mix(First.b, Second.b, Blend), 1.0f);
	}

	inline ColorRGBA Background(float Alpha = 1.0f)
	{
		return ColorRGBA(0.035f, 0.045f, 0.065f, Alpha * g_Config.m_TcUiBackgroundOpacity / 100.0f);
	}
}

#endif
