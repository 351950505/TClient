/*
 * Based on QmClient's src/game/client/components/qmclient/hud_map_progress.h.
 * Adapted for TClient's standalone HUD and Dynamic Island surfaces; optional
 * Player Stats integration is not part of this port.
 * Source license: QmClient root license.txt (permissive license).
 *
 * Teeworlds Copyright (C) 2007-2014 Magnus Auvinen
 * DDRace    Copyright (C) 2010-2011 Shereef Marzouk
 * DDNet     Copyright (C)           Dennis Felsing
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */
#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_HUD_MAP_PROGRESS_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_HUD_MAP_PROGRESS_H

#include <base/detect.h>

#include <algorithm>

namespace TClientHudMapProgress
{
#if defined(CONF_FAMILY_WINDOWS) && __has_include(<winrt/Windows.Media.Control.h>)
	inline constexpr bool DYNAMIC_ISLAND_AVAILABLE = true;
#else
	inline constexpr bool DYNAMIC_ISLAND_AVAILABLE = false;
#endif

	inline bool ShouldReserveIslandValue(bool Enabled, int Style, bool IslandEnabled, bool IslandAvailable)
	{
		return Enabled && Style == 1 && IslandEnabled && IslandAvailable;
	}

	inline bool ShouldShowIslandValue(bool Enabled, int Style, bool IslandEnabled, bool HasProgress, bool IslandAvailable)
	{
		return ShouldReserveIslandValue(Enabled, Style, IslandEnabled, IslandAvailable) && HasProgress;
	}

	inline bool ShouldShowStandaloneValue(bool Enabled, int Style, bool IslandEnabled, bool IslandAvailable)
	{
		return Enabled && !(Style == 1 && IslandEnabled && IslandAvailable);
	}

	inline constexpr float MOVEMENT_INFORMATION_BOX_WIDTH = 62.0f;
	inline constexpr float MAP_PROGRESS_HUD_MAX_WIDTH = 25.0f;
	inline constexpr float MAP_PROGRESS_HUD_HEIGHT = 5.5f;
	inline constexpr float MAP_PROGRESS_HUD_GAP = 2.0f;
	inline constexpr float MAP_PROGRESS_HUD_FONT_SIZE = 4.5f;
	inline constexpr float LABEL_SCALE = 0.70f;
	inline constexpr float LABEL_GAP = 0.85f;
	inline constexpr float MINI_DEBUG_TEXT_HEIGHT = 11.0f;

	inline float StandaloneHudStackOffset(bool Visible)
	{
		// The reference layout lifts the board by 36 px at a HUD height of 300 / 1056.
		return Visible ? MAP_PROGRESS_HUD_HEIGHT + MAP_PROGRESS_HUD_GAP + 2.5f : 0.0f;
	}

	inline float ReadoutFit(float TextWidth)
	{
		const float Available = MAP_PROGRESS_HUD_MAX_WIDTH - LABEL_GAP;
		return TextWidth > Available ? Available / TextWidth : 1.0f;
	}

	inline float MapProgressHudTop(float MovementInformationTop)
	{
		return MovementInformationTop - MAP_PROGRESS_HUD_GAP - MAP_PROGRESS_HUD_HEIGHT;
	}

	inline float MiniDebugCoordinateOffsetY(bool HasDDraceOffset, bool HasHealthAmmoOffset)
	{
		return 3.0f + (HasDDraceOffset ? 50.0f : HasHealthAmmoOffset ? 27.0f : 0.0f);
	}

	struct SIslandValueLayout
	{
		float m_ValueX = 0.0f;
		float m_ValueY = 0.0f;
		float m_FontSize = 0.0f;
		bool m_Visible = false;
	};

	inline float IslandValueContentWidth(float ValueWidth, float IconWidth, float IconGap)
	{
		return ValueWidth + (IconWidth > 0.0f ? IconWidth + IconGap : 0.0f);
	}

	inline float IslandValueCellWidth(float ContentWidth, float Padding)
	{
		return ContentWidth + 2.0f * Padding;
	}

	inline SIslandValueLayout IslandValueLayout(float CellX, float CellY, float CellWidth, float CellHeight, float ValueWidth, float FontSize, float Padding)
	{
		SIslandValueLayout Result;
		if(Padding < 0.0f || CellWidth < IslandValueCellWidth(ValueWidth, Padding) || CellHeight < FontSize || FontSize <= 0.0f)
			return Result;
		Result.m_ValueX = CellX + (CellWidth - ValueWidth) * 0.5f;
		Result.m_ValueY = CellY + (CellHeight - FontSize) * 0.5f;
		Result.m_FontSize = FontSize;
		Result.m_Visible = true;
		return Result;
	}

	class CProgressState
	{
		bool m_Initialized = false;
		float m_Displayed = 0.0f;
		int m_ClientId = -1;

	public:
		void Reset() { *this = {}; }
		bool IsTracking(int ClientId) const { return m_Initialized && m_ClientId == ClientId; }
		bool TryGetForClient(int ClientId, float &Progress) const
		{
			if(ClientId < 0 || !IsTracking(ClientId))
				return false;
			Progress = m_Displayed;
			return true;
		}
		float Update(bool HasProgress, float Progress, float DeltaSeconds, int ClientId = -1)
		{
			if(!HasProgress)
			{
				Reset();
				return 0.0f;
			}
			const float Target = std::clamp(Progress, 0.0f, 1.0f);
			if(!IsTracking(ClientId))
				m_Displayed = Target;
			else
				m_Displayed += (Target - m_Displayed) * std::clamp(DeltaSeconds * 8.0f, 0.0f, 1.0f);
			m_Initialized = true;
			m_ClientId = ClientId;
			return m_Displayed;
		}
	};
}

#endif
