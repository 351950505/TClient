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

#include <game/client/ui_rect.h>

#include <algorithm>

namespace TClientHudMapProgress
{
	inline bool ShouldShowIslandValue(bool Enabled, int Style, bool IslandEnabled, bool HasProgress)
	{
		return Enabled && Style == 1 && IslandEnabled && HasProgress;
	}

	struct SFallbackTextLayout
	{
		CUIRect m_Text{};
		float m_FontSize = 0.0f;
	};

	inline SFallbackTextLayout FallbackTextLayout(float HudWidth, float HudHeight, float WidthPercent, float Size, float PositionXPercent, float PositionYPercent)
	{
		SFallbackTextLayout Result;
		const float TextWidth = std::clamp(HudWidth * std::clamp(WidthPercent / 100.0f, 0.10f, 0.80f), 80.0f, std::max(80.0f, HudWidth - 12.0f));
		Result.m_FontSize = std::clamp(std::clamp(Size, 1.0f, 50.0f) * 0.72f, 6.0f, 16.0f);
		const float RawX = HudWidth * std::clamp(PositionXPercent / 100.0f, 0.0f, 1.0f) - TextWidth * 0.5f;
		const float AnchorY = HudHeight * std::clamp(PositionYPercent / 100.0f, 0.0f, 1.0f);
		const float X = std::round(std::clamp(RawX, 6.0f, std::max(6.0f, HudWidth - TextWidth - 6.0f)));
		const float Gap = std::clamp(Size * 0.35f, 2.0f, 8.0f);
		const float Y = std::round(std::clamp(AnchorY - Result.m_FontSize - Gap, 2.0f, std::max(2.0f, HudHeight - Result.m_FontSize - 2.0f)));
		Result.m_Text = {X, Y, TextWidth, Result.m_FontSize};
		return Result;
	}

	inline constexpr float BOARD_VALUE_GAP = 2.0f;
	inline constexpr float BOARD_VALUE_HEIGHT = 6.0f;
	inline constexpr float BOARD_VALUE_BOTTOM_PADDING = 2.0f;

	inline bool HasRoomBelowBoard(const CUIRect &Board, float SafeBottom)
	{
		return Board.w > 0.0f && Board.h > 0.0f &&
			Board.y + Board.h + BOARD_VALUE_GAP + BOARD_VALUE_HEIGHT + BOARD_VALUE_BOTTOM_PADDING <= SafeBottom;
	}

	inline CUIRect BoardValue(const CUIRect &Board)
	{
		const float ValueWidth = std::min(Board.w, 40.0f);
		return {Board.x + (Board.w - ValueWidth) * 0.5f, Board.y + Board.h + BOARD_VALUE_GAP, ValueWidth, BOARD_VALUE_HEIGHT};
	}

	struct SIslandValueLayout
	{
		float m_ValueX = 0.0f;
		float m_ValueY = 0.0f;
		float m_FontSize = 0.0f;
		bool m_Visible = false;
	};

	inline float IslandValueCellWidth(float ValueWidth, float Padding)
	{
		return ValueWidth + 2.0f * Padding;
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
		float Displayed() const { return m_Displayed; }
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
